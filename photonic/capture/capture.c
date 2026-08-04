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
/// Capture engine entry points: create, destroy, start and stop. The
/// engine itself is split across the sibling files in this folder (see
/// capture_private.h); the public contract is capture.h.

// clang-format off
#include "capture.h"
#include "p1394.h"
#include "dcam.h"
#include "capture_private.h"
#include "capture.tmh"
// clang-format on

/// PhotonicCaptureNeedsReleaseRetry -- TRUE when a previous stop left something
/// to release: a bus resource whose free request never reached the bus driver,
/// or a quarantined read held behind an unconfirmed flush.
///
/// @param Capture  Capture engine whose release state is checked.
/// @return TRUE when a release retry is needed.
static BOOLEAN PhotonicCaptureNeedsReleaseRetry(_In_ PPHOTONIC_CAPTURE Capture) {
    return Capture->ResourceAllocated || Capture->BandwidthAllocated || Capture->ChannelAllocated ||
           PhotonicCaptureHasQuarantinedFrames(Capture);
}

NTSTATUS PhotonicCaptureCreate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                               _In_ PPHOTONIC_CAPTURE_DELIVER Deliver, _In_ PPHOTONIC_CAPTURE_ACQUIRE Acquire,
                               _In_ PVOID CallbackContext) {
    PPHOTONIC_CAPTURE capture;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_ISO);

    capture = (PPHOTONIC_CAPTURE) ExAllocatePoolZero(NonPagedPoolNx, sizeof(*capture), PHOTONIC_POOL_TAG);
    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "failed to allocate the capture engine (%u bytes)\n",
                    (ULONG) sizeof(*capture));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    capture->Extension = Extension;
    capture->Deliver = Deliver;
    capture->Acquire = Acquire;
    capture->CallbackContext = CallbackContext;
    KeInitializeSpinLock(&capture->Lock);
    KeInitializeEvent(&capture->DrainDone, NotificationEvent, FALSE);
    KeInitializeEvent(&capture->FirstAttachDone, NotificationEvent, FALSE);
    InitializeListHead(&capture->FreeList);
    InitializeListHead(&capture->PendingList);

    //
    // The engine idles with Draining set: the pump attaches nothing until
    // PhotonicCaptureStart has acquired the isochronous resources.
    //
    capture->Draining = TRUE;

    status = PhotonicCapturePoolAllocate(capture);
    if (!NT_SUCCESS(status)) {
        PhotonicCapturePoolFree(capture);
        ExFreePoolWithTag(capture, PHOTONIC_POOL_TAG);
        return status;
    }

    Stream->Capture = capture;
    return STATUS_SUCCESS;
}

BOOLEAN PhotonicCaptureDestroy(_In_ PPHOTONIC_STREAM_EXTENSION Stream) {
    PPHOTONIC_CAPTURE capture = Stream->Capture;

    FuncEntry(TRACE_FLAG_ISO);

    if (capture == NULL) {
        return TRUE;
    }

    //
    // A quarantined descriptor that survived the stop means the resource free
    // never confirmed the flush of its DMA mapping: the bus driver may still
    // record the descriptor's ISOCH_DESCRIPTOR array, which lives inside this
    // engine block, and a mapping of the consumer's pages. Leak the engine
    // deliberately rather than hand that memory back to the pool; the caller
    // must leak whatever the engine's callbacks reference too. Freeing memory
    // the bus driver may still write is worse.
    //
    if (PhotonicCaptureHasQuarantinedFrames(capture)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                    "leaking the capture engine: quarantined buffer(s) with an unflushed DMA mapping\n");
        Stream->Capture = NULL;
        return FALSE;
    }

    PhotonicCapturePoolFree(capture);
    PhotonicCapturePoolFreeChunkMdls(capture);

    Stream->Capture = NULL;
    ExFreePoolWithTag(capture, PHOTONIC_POOL_TAG);
    return TRUE;
}

NTSTATUS PhotonicCaptureStart(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                              _In_ BOOLEAN ExpectBuffers) {
    PPHOTONIC_CAPTURE capture = Stream->Capture;
    ULONG bytesPerPacket;
    ULONG packetsPerFrame;
    UCHAR scode;
    ULONG speedFlags;
    KIRQL irql;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_ISO);

    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "no capture engine; the stream is not open\n");
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Extension->Removed) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO, "device removed; not starting capture\n");
        return STATUS_DEVICE_REMOVED;
    }
    if (!capture->Draining) {
        //
        // Already streaming. Still publish the running state so a Pause to
        // Run resume lands on KSSTATE_RUN like a fresh start.
        //
        KeAcquireSpinLock(&Stream->PendingLock, &irql);
        Stream->StreamState = KSSTATE_RUN;
        KeReleaseSpinLock(&Stream->PendingLock, irql);
        return STATUS_SUCCESS;
    }
    if (Stream->Mode == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO, "no negotiated mode; cannot start capture\n");
        return STATUS_INVALID_DEVICE_STATE;
    }

    //
    // A previous stop may have failed to release the session (a free request
    // that never reached the bus driver, or a quarantined read held behind an
    // unconfirmed flush). Retry the release first: before fresh allocations
    // would overwrite the recorded handles and strand the old resources,
    // before PhotonicCapturePoolSetupChunks allocates this session's chunk
    // MDLs (the release path frees the chunk MDLs it finds, so running it any
    // later would free the new session's), and so a held read completes as
    // soon as the flush is confirmed.
    //
    if (PhotonicCaptureNeedsReleaseRetry(capture)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO, "retrying the release a previous stop left unfinished\n");
        PhotonicCaptureTeardownReleaseSessionResources(capture);
        if (PhotonicCaptureHasQuarantinedFrames(capture)) {
            //
            // Still unflushed. Starting a new session over descriptors the
            // bus driver may still write is not safe.
            //
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                        "cannot start: quarantined buffer(s) still hold an unflushed DMA mapping\n");
            return STATUS_DEVICE_NOT_READY;
        }
    }

    //
    // Program the camera for the negotiated mode and learn its isochronous
    // packetisation before any host resources are taken.
    //
    status = PhotonicDcamConfigureStream(Extension, Stream, &bytesPerPacket, &packetsPerFrame);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    if (bytesPerPacket == 0 || packetsPerFrame == 0) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto fail;
    }
    capture->BytesPerPacket = bytesPerPacket;
    capture->PacketsPerFrame = packetsPerFrame;
    capture->FrameBytes = bytesPerPacket * packetsPerFrame;

    status = PhotonicCapturePoolSetupChunks(capture);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    scode = Photonic1394StreamScode(Extension);
    speedFlags = 1u << scode;

    status = PhotonicCaptureResourceAllocateChannel(capture);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    //
    // Tell the camera which channel and speed to transmit on before enabling it.
    //
    status = PhotonicDcamSetIsochChannel(Extension, capture->Channel, scode);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    status = PhotonicCaptureResourceAllocateBandwidth(capture, speedFlags);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    status = PhotonicCaptureResourceAllocateHandle(capture, speedFlags);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    //
    // Open the engine for submits before the camera transmits anything.
    // FirstAttachDone is reset while the engine is still settled, so the
    // listen step below waits only for attaches issued by this start. The
    // control work item's retry budget starts fresh for this session. A
    // cancel notification still pending from the previous session is
    // discarded: every read it referred to was completed by the teardown,
    // and consuming it now would quiesce the new session and cancel healthy
    // reads.
    //
    KeClearEvent(&capture->FirstAttachDone);
    capture->ControlRetries = 0;
    InterlockedExchange(&capture->CancelPending, 0);
    KeAcquireSpinLock(&capture->Lock, &irql);
    capture->Draining = FALSE;
    KeReleaseSpinLock(&capture->Lock, irql);

    //
    // Pump the buffers the front-end holds ready -- the stream's parked reads,
    // or the IOCTL slot's mapped ring -- then listen before enabling the
    // camera, otherwise the leading frames transmit before the host receives
    // and are lost. EnsureListen waits for the first attach to complete and
    // issues the listen synchronously; it returns at once when nothing is
    // submitted.
    //
    PhotonicCapturePump(capture);
    PhotonicCaptureEnsureListen(capture);

    //
    // Start the camera streaming, but only once the receive path is armed. A
    // receiving session whose listen is not active yet (no buffer attached:
    // the front-end's buffers arrive asynchronously around the run
    // transition, or the ring is mapped only after the start) must not set
    // ISO_EN here -- the leading frames would transmit with nobody listening
    // and be lost on the wire (delivered frame indices starting at 1 instead
    // of 0). Nothing can be received before a buffer is armed anyway, so the
    // enable is deferred, without waiting: EnableWanted is recorded and the
    // control work item sets ISO_EN right after the first attach completion
    // confirms the listen. A transmit-only session (imager start) free-runs
    // by design and is enabled immediately.
    //
    // In single-frame mode (Stream->OneShot) continuous transmission is never
    // enabled: each acquisition is individually armed through the ONE_SHOT
    // register (PhotonicDcamOneShot) and the camera transmits one frame per
    // external trigger, so ISO_EN stays clear and IsoEnabled FALSE.
    //
    if (!Stream->OneShot) {
        if (ExpectBuffers && !capture->Listening) {
            capture->EnableWanted = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO,
                        "no buffer armed yet; camera enable deferred to the first attach\n");
        } else {
            status = PhotonicDcamSetIsochEnable(Extension, TRUE);
            if (!NT_SUCCESS(status)) {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "capture start failed: %!STATUS!\n", status);
                PhotonicCaptureStop(Extension, Stream);
                return status;
            }
            capture->IsoEnabled = TRUE;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO,
                "capture started: %u bytes/frame, %u packets x %u bytes, %u descriptors, channel %u, %s (listen %s)\n",
                capture->FrameBytes, capture->PacketsPerFrame, capture->BytesPerPacket, PHOTONIC_CAPTURE_FRAME_COUNT,
                capture->Channel,
                Stream->OneShot         ? "single-frame (ISO_EN off)"
                : capture->EnableWanted ? "continuous (camera enable deferred)"
                                        : "continuous",
                capture->Listening ? "active" : "deferred to the first attach");

    //
    // Publish the running state under PendingLock on every success path,
    // mirroring the stop's KSSTATE_STOP publish, so no front-end has to
    // post-publish it and every observer of StreamState reads it under one
    // lock.
    //
    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    Stream->StreamState = KSSTATE_RUN;
    KeReleaseSpinLock(&Stream->PendingLock, irql);
    return STATUS_SUCCESS;

fail:
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "capture start failed: %!STATUS!\n", status);
    PhotonicCaptureTeardownReleaseSessionResources(capture);
    return status;
}

VOID PhotonicCaptureStop(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream) {
    PPHOTONIC_CAPTURE capture = Stream->Capture;
    BOOLEAN settled;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_ISO);

    //
    // Publish the stopped state first, on every path including the settled
    // one, so no caller has to pre-publish it. A data request racing this
    // stop (TurnOffSynchronization) either parks in time for the drain below
    // to complete it or observes KSSTATE_STOP and fails fast instead of
    // parking forever on a stopped stream.
    //
    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    Stream->StreamState = KSSTATE_STOP;
    KeReleaseSpinLock(&Stream->PendingLock, irql);

    if (capture == NULL) {
        return;
    }

    //
    // Every teardown runs under InterfaceMutex and completes before the mutex
    // is released, so a draining engine here is settled idle: nothing is
    // attached at the bus driver. Only reads parked in the front-end since
    // then remain, plus possibly a release a previous stop could not finish
    // (a free request that never reached the bus driver, or a quarantined
    // read held behind an unconfirmed flush) -- retry that release here, so a
    // stop with no subsequent start still frees the bus resources and
    // completes the held reads once the flush is confirmed.
    //
    KeAcquireSpinLock(&capture->Lock, &irql);
    settled = capture->Draining;
    KeReleaseSpinLock(&capture->Lock, irql);
    if (settled) {
        //
        // A pump run triggered by a late read may still be executing its
        // no-op lap. Wait it out so the caller can safely destroy the engine
        // after this stop returns.
        //
        PhotonicCaptureTeardownWaitPumpIdle(capture);
        if (PhotonicCaptureNeedsReleaseRetry(capture)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO, "retrying the release a previous stop left unfinished\n");
            PhotonicCaptureTeardownReleaseSessionResources(capture);
        }
        PhotonicStreamCompletePendingReads(Stream);
        return;
    }

    PhotonicCaptureTeardownShutdown(Extension, Stream, capture);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO, "capture stopped\n");
}
