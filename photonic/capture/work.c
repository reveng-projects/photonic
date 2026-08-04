// Copyright (c) 2026 Vitaly Chipounov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

/// @file
/// The control work item and the transitions it performs at
/// PASSIVE_LEVEL: cancel resolution, the isochronous listen and the
/// deferred camera enable. See capture.h.

// clang-format off
#include "capture.h"
#include "p1394.h"
#include "dcam.h"
#include "capture_private.h"
#include "work.tmh"
// clang-format on

/// Retry budget for the control work item's listen and deferred camera
/// enable. Attach completions stop arriving once every descriptor is
/// attached (nothing completes before the listen), so a failed attempt has
/// no external retrigger: the work item requeues itself instead, and this
/// cap keeps persistently failing hardware from requeueing forever. Each
/// attempt is already bounded by the bus transaction timeout.
#define PHOTONIC_CAPTURE_CONTROL_RETRY_MAX 8

VOID PhotonicCaptureEnsureListen(_In_ PPHOTONIC_CAPTURE Capture) {
    LARGE_INTEGER timeout;
    IRB irb;
    NTSTATUS status;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_ISO);

    KeAcquireSpinLock(&Capture->Lock, &irql);
    if (Capture->Draining || Capture->Listening || IsListEmpty(&Capture->PendingList)) {
        KeReleaseSpinLock(&Capture->Lock, irql);
        return;
    }
    KeReleaseSpinLock(&Capture->Lock, irql);

    //
    // The bus driver accepts a listen only once at least one buffer is
    // attached, and attaches complete asynchronously. Wait briefly for the
    // first one; on timeout leave the listen to the control work item, which
    // retries on the next attach completion.
    //
    timeout.QuadPart = -10 * 1000 * 1000; // 1 second, relative
    if (KeWaitForSingleObject(&Capture->FirstAttachDone, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO,
                    "no attach completed within 1s; listen deferred to the next attach completion\n");
        return;
    }

    //
    // Re-check under the lock: frames keep flowing during the wait, so the
    // attached buffer may already have delivered and recycled. (Draining
    // cannot change concurrently, the mutex serializes teardowns; the check
    // is kept so the guard reads the same as the one above.)
    //
    KeAcquireSpinLock(&Capture->Lock, &irql);
    if (Capture->Draining || Capture->Listening || IsListEmpty(&Capture->PendingList)) {
        KeReleaseSpinLock(&Capture->Lock, irql);
        return;
    }
    KeReleaseSpinLock(&Capture->Lock, irql);

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_ISOCH_LISTEN;
    irb.u.IsochListen.hResource = Capture->ResourceHandle;
    irb.u.IsochListen.fulFlags = 0;
    status = PhotonicSubmitIrb(Capture->Extension, &irb);

    if (NT_SUCCESS(status)) {
        KeAcquireSpinLock(&Capture->Lock, &irql);
        Capture->Listening = TRUE;
        KeReleaseSpinLock(&Capture->Lock, irql);
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO, "listening on channel %u\n", Capture->Channel);
    } else {
        //
        // STATUS_INSUFFICIENT_RESOURCES just means the attached buffer is gone
        // again (delivered before the listen arrived), a benign outcome. Any
        // other failure (invalid handle, bus reset) deserves a warning. Both
        // are retried by the control work item: the next attach completion
        // queues it, and PhotonicCaptureWorkService requeues itself a bounded
        // number of times when no further attach completion is coming. WPP
        // levels must be literals, so two calls rather than a conditional.
        //
        if (status == STATUS_INSUFFICIENT_RESOURCES) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO,
                        "listen not accepted (%!STATUS!); retried by the control work item\n", status);
        } else {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO,
                        "listen failed (%!STATUS!); retried by the control work item\n", status);
        }
    }
}

/// PhotonicCaptureWorkService -- perform whatever control transition the engine
/// needs: resolve a pending cancel (quiesce the stream, which detaches and
/// completes every in-flight read, then resume it), and issue a listen that
/// became possible. Runs on the control work item at PASSIVE_LEVEL with
/// InterfaceMutex held, so it is serialized against every other control
/// transition and recomputes the state it acts on rather than trusting the
/// trigger.
///
/// @param Extension  Device extension whose active engine is serviced.
static VOID PhotonicCaptureWorkService(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    PPHOTONIC_STREAM_EXTENSION stream = Extension->ActiveCaptureStream;
    PPHOTONIC_CAPTURE capture;
    BOOLEAN running;
    BOOLEAN retryListen;
    BOOLEAN retryEnable;
    KIRQL irql;

    if (stream == NULL || stream->Capture == NULL) {
        return;
    }
    capture = stream->Capture;

    if (InterlockedExchange(&capture->CancelPending, 0) != 0) {
        KeAcquireSpinLock(&capture->Lock, &irql);
        running = !capture->Draining;
        KeReleaseSpinLock(&capture->Lock, irql);

        //
        // A read the class driver cancelled is (or was) attached to the bus.
        // Quiesce the stream: every in-flight read, the cancelled one
        // included, is detached and completed. When the engine is already
        // settled a teardown has completed them, so there is nothing to do.
        // The camera keeps transmitting throughout (frames simply fall on the
        // floor while nothing is attached); the resume re-attaches the
        // front-end's buffers and listens again.
        //
        if (running) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO, "resolving a read cancel: quiescing the stream\n");
            PhotonicCaptureTeardownQuiesce(capture);

            if (Extension->Removed || PhotonicCaptureHasQuarantinedFrames(capture)) {
                //
                // Do not resume. Either the camera is gone and there is
                // nothing to resume into, or a detach failed during the run
                // and its quarantined read completes only once the
                // isochronous resources are freed. The class driver cancels
                // a read exactly once, so resuming would strand that read
                // until the client stops the stream. Finish the teardown
                // through the shared shutdown instead: the stopped state is
                // published so new reads fail fast rather than park forever
                // on a dead session, the resources are released, the
                // quarantined read completes (or stays held until a retried
                // release confirms the flush), and the later stop finds the
                // engine settled.
                //
                PhotonicCaptureTeardownShutdown(Extension, stream, capture);
            } else {
                KeAcquireSpinLock(&capture->Lock, &irql);
                capture->Draining = FALSE;
                KeReleaseSpinLock(&capture->Lock, irql);
                PhotonicCapturePump(capture);
            }
        }
    }

    //
    // Issue the listen the attach completion asked for, or that the resume
    // above now needs. A no-op when already listening, idle, or nothing is
    // attached.
    //
    PhotonicCaptureEnsureListen(capture);

    //
    // Complete a deferred camera enable: the start found no buffer armed and
    // left ISO_EN clear (see PhotonicCaptureStart), and the listen above is
    // active now, so the camera can transmit with the receive path ready and
    // the stream's first frame is captured. A failed register write keeps
    // EnableWanted set and the retry pass below queues another run.
    //
    if (capture->EnableWanted && capture->Listening && !Extension->Removed) {
        NTSTATUS status = PhotonicDcamSetIsochEnable(Extension, TRUE);

        if (NT_SUCCESS(status)) {
            capture->EnableWanted = FALSE;
            capture->IsoEnabled = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO,
                        "camera enabled after the first buffer armed the receive path\n");
        } else {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "deferred camera enable failed: %!STATUS!\n", status);
        }
    }

    //
    // Retry what did not stick. A wanted listen or deferred enable that
    // failed above has no reliable external retrigger: attach completions
    // stop once every descriptor is attached (nothing completes before the
    // listen), so the work item requeues itself, bounded by the retry cap.
    // The budget resets whenever a run finds nothing left to retry, so it
    // applies per stall, not per session.
    //
    KeAcquireSpinLock(&capture->Lock, &irql);
    retryListen = !capture->Draining && !capture->Listening && !IsListEmpty(&capture->PendingList);
    KeReleaseSpinLock(&capture->Lock, irql);
    retryEnable = capture->EnableWanted && capture->Listening;

    if ((retryListen || retryEnable) && !Extension->Removed) {
        if (capture->ControlRetries < PHOTONIC_CAPTURE_CONTROL_RETRY_MAX) {
            capture->ControlRetries++;
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO, "%s did not stick; queuing retry %u of %u\n",
                        retryEnable ? "deferred camera enable" : "listen", capture->ControlRetries,
                        PHOTONIC_CAPTURE_CONTROL_RETRY_MAX);
            PhotonicCaptureRequestWork(Extension);
        } else {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                        "%s still failing after %u retries; giving up until the stream is restarted\n",
                        retryEnable ? "deferred camera enable" : "listen", PHOTONIC_CAPTURE_CONTROL_RETRY_MAX);
        }
    } else {
        capture->ControlRetries = 0;
    }
}

/// PhotonicCaptureWorker -- the control work item. Clears the coalescing flag
/// first, so a trigger that fires while this run is working queues another run
/// rather than being lost, then services the engine under InterfaceMutex.
///
/// @param DeviceObject  Device object the work item was allocated on (unused).
/// @param Context       The device extension.
static IO_WORKITEM_ROUTINE PhotonicCaptureWorker;
static VOID PhotonicCaptureWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    FuncEntry(TRACE_FLAG_ISO);

    if (extension == NULL) {
        return;
    }

    InterlockedExchange(&extension->ControlWorkQueued, 0);

    KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);
    PhotonicCaptureWorkService(extension);
    KeReleaseMutex(&extension->InterfaceMutex, FALSE);

    IoReleaseRemoveLock(&extension->IrbRemoveLock, extension->ControlWorkItem);
}

VOID PhotonicCaptureRequestWork(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    if (InterlockedCompareExchange(&Extension->ControlWorkQueued, 1, 0) != 0) {
        //
        // A run is already queued and has not begun acting yet; it will see
        // the state this caller just recorded.
        //
        return;
    }

    //
    // The remove lock keeps SRB_UNINITIALIZE_DEVICE from returning while a run
    // is queued or executing. Once its wait has begun, new work is refused;
    // the teardown paths perform any remaining transitions themselves.
    //
    if (!NT_SUCCESS(IoAcquireRemoveLock(&Extension->IrbRemoveLock, Extension->ControlWorkItem))) {
        InterlockedExchange(&Extension->ControlWorkQueued, 0);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO,
                    "control work refused: device teardown has begun and performs the remaining transitions itself\n");
        return;
    }

    IoQueueWorkItem(Extension->ControlWorkItem, PhotonicCaptureWorker, DelayedWorkQueue, Extension);
}

VOID PhotonicCaptureNotifyCancel(_In_ PPHOTONIC_CAPTURE Capture) {
    FuncEntry(TRACE_FLAG_ISO);

    InterlockedExchange(&Capture->CancelPending, 1);
    PhotonicCaptureRequestWork(Capture->Extension);
}
