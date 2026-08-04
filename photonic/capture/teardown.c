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
/// Session teardown: quiesce the engine, release what it still holds at
/// the bus driver, silence the camera, and the shared shutdown every
/// stopper runs. See capture.h.

// clang-format off
#include "capture.h"
#include "dcam.h"
#include "capture_private.h"
#include "teardown.tmh"
// clang-format on

/// PhotonicCaptureTeardownWaitPumpIdle -- wait for the pump election to settle. A pump
/// lap can hold a claimed descriptor and a front-end buffer with no lock held,
/// which the InFlightIrps drain does not cover, so a teardown must not release
/// the session or let the engine be freed while a lap is mid-flight. The
/// engine is draining (or settled) when this runs, so the elected runner
/// finishes quickly: its laps attach nothing and it cannot re-elect. Must run
/// at PASSIVE_LEVEL.
///
/// @param Capture  Capture engine whose pump election is waited out.
VOID PhotonicCaptureTeardownWaitPumpIdle(_In_ PPHOTONIC_CAPTURE Capture) {
    ULONG waitedMs = 0;
    KIRQL irql;

    for (;;) {
        BOOLEAN active;
        LARGE_INTEGER delay;

        KeAcquireSpinLock(&Capture->Lock, &irql);
        active = Capture->PumpActive;
        KeReleaseSpinLock(&Capture->Lock, irql);
        if (!active) {
            return;
        }

        delay.QuadPart = -10 * 1000; // 1 ms, relative
        KeDelayExecutionThread(KernelMode, FALSE, &delay);

        //
        // The runner should settle within a lap or two; log periodically so a
        // wedged election is diagnosable from a trace, like the IRP drain.
        //
        waitedMs++;
        if (waitedMs % 5000 == 0) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                        "still waiting for the pump election to settle after %u ms\n", waitedMs);
        }
    }
}

/// PhotonicCaptureTeardownQuiesce -- bring the engine from running to settled idle: stop
/// the isochronous context, detach every attached buffer (each read completes
/// through the delivery callback, cancelled unless its frame already finished)
/// and wait for the in-flight attach and detach IRPs to drain. The isochronous
/// resources stay allocated, so the caller can resume the stream (a cancel
/// resolution) or release them (a stop). A no-op on a settled engine.
///
/// The one ordering rule everything here serves: a buffer is detached only from
/// a stopped context, and the resources are freed only after the drain --
/// otherwise REQUEST_ISOCH_FREE_RESOURCES flushes leftover DMA mappings into
/// pages that have already been unlocked. Must run at PASSIVE_LEVEL with
/// InterfaceMutex held.
///
/// @param Capture  Capture engine to quiesce.
VOID PhotonicCaptureTeardownQuiesce(_In_ PPHOTONIC_CAPTURE Capture) {
    KIRQL irql;

    FuncEntry(TRACE_FLAG_ISO);

    //
    // Block new attaches: the pump claims nothing while Draining is set, a
    // buffer it publishes in the same instant is handed straight back
    // cancelled (the publish re-checks Draining), and an in-flight attach
    // completion publishes its descriptor for the claim loop below.
    //
    KeAcquireSpinLock(&Capture->Lock, &irql);
    if (Capture->Draining) {
        KeReleaseSpinLock(&Capture->Lock, irql);
        return;
    }
    Capture->Draining = TRUE;
    KeReleaseSpinLock(&Capture->Lock, irql);

    //
    // Stop the isochronous context before any buffer is detached: the bus driver
    // reliably tears down a buffer's DMA mapping only when the context is not
    // running. Issued whenever the resource exists (stopping a context that
    // never listened is a no-op). The bus fires no further frame-complete
    // callbacks after this; the claim loop below still raises InFlightIrps
    // for its own detaches, but no path outside this teardown raises the
    // count anymore.
    //
    if (Capture->ResourceAllocated) {
        PhotonicCaptureResourceIsochStop(Capture);
    }

    //
    // Detach every attached buffer and drain every in-flight IRP. Only this
    // loop may detach an incomplete buffer, and it runs after the ISOCH_STOP
    // above, so the stop-before-detach rule has exactly one enforcer: an
    // attach completing mid-teardown publishes its descriptor Pending rather
    // than detach from a possibly-running context, the drain wait ends only
    // once that completion has run, and the next pass claims what it
    // published. Attaches can only finish, never start, while Draining is
    // set, so the outer loop settles within a pass or two of the pool.
    //
    for (;;) {
        BOOLEAN empty;

        //
        // Claim and detach every fully-attached descriptor, completing its
        // read cancelled through the normal asynchronous detach path.
        //
        for (;;) {
            PPHOTONIC_CAPTURE_FRAME desc = NULL;
            PLIST_ENTRY entry;

            KeAcquireSpinLock(&Capture->Lock, &irql);
            for (entry = Capture->PendingList.Flink; entry != &Capture->PendingList; entry = entry->Flink) {
                PPHOTONIC_CAPTURE_FRAME candidate = CONTAINING_RECORD(entry, PHOTONIC_CAPTURE_FRAME, ListEntry);

                if (candidate->State == PhotonicFramePending) {
                    RemoveEntryList(entry);
                    candidate->State = PhotonicFrameDetaching;
                    candidate->CompletionStatus = STATUS_CANCELLED;
                    InterlockedIncrement(&Capture->InFlightIrps);
                    desc = candidate;
                    break;
                }
            }
            KeReleaseSpinLock(&Capture->Lock, irql);

            if (desc == NULL) {
                break;
            }
            PhotonicCapturePumpBeginDetach(Capture, desc);
        }

        //
        // Wait for every attach and detach IRP still at the bus driver to
        // finish before the resources they reference can be released or the
        // stream can resume. The wait is bounded per period and logged so a
        // bus driver that never completes an isochronous IRP is observable (a
        // hung teardown is diagnosable, proceeding would use freed pages).
        //
        KeClearEvent(&Capture->DrainDone);
        while (InterlockedCompareExchange(&Capture->InFlightIrps, 0, 0) != 0) {
            LARGE_INTEGER timeout;

            timeout.QuadPart = -10 * 1000 * 1000 * 10; // 10 seconds, relative
            if (KeWaitForSingleObject(&Capture->DrainDone, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                            "still waiting for %d in-flight bus IRP(s) to drain; the bus driver may be wedged\n",
                            Capture->InFlightIrps);
            }
        }

        //
        // With no IRP in flight, every descriptor is Free, quarantined, or a
        // Pending one an attach completion published during the drain. The
        // pending list holds exactly the last kind; loop until it is empty.
        //
        KeAcquireSpinLock(&Capture->Lock, &irql);
        empty = IsListEmpty(&Capture->PendingList);
        KeReleaseSpinLock(&Capture->Lock, irql);
        if (empty) {
            break;
        }
    }

    //
    // A pump election in progress can still hold a claimed descriptor and a
    // front-end buffer outside the lock, invisible to the IRP drain above.
    // Wait for the runner to finish before the caller may release the session
    // or free the pool. Draining is set, so the current lap hands its buffer
    // back cancelled and the runner exits without re-electing.
    //
    PhotonicCaptureTeardownWaitPumpIdle(Capture);

    //
    // Settled: nothing is attached anymore, so the next start (or resume)
    // waits for its own first attach.
    //
    KeClearEvent(&Capture->FirstAttachDone);
}

/// PhotonicCaptureTeardownReleaseSessionResources -- release what a quiesced engine
/// still holds at the bus driver: the isochronous resources, the reads
/// quarantined by a failed detach (deliverable only once
/// REQUEST_ISOCH_FREE_RESOURCES has confirmed their leftover DMA mappings
/// flushed), and the per-chunk partial MDLs. When the resource free did not
/// complete, the quarantined reads stay held and the chunk MDLs stay
/// allocated: a later stop or start retries through this same path and
/// finishes the delivery then. Must run at PASSIVE_LEVEL with InterfaceMutex
/// held, after PhotonicCaptureTeardownQuiesce.
///
/// @param Capture  Quiesced capture engine whose resources are released.
VOID PhotonicCaptureTeardownReleaseSessionResources(_In_ PPHOTONIC_CAPTURE Capture) {
    BOOLEAN flushed;
    ULONG i;
    KIRQL irql;

    flushed = PhotonicCaptureResourceReleaseIsoch(Capture);

    //
    // A descriptor whose detach failed kept its buffer attached and its read
    // uncompleted, so the pages stayed locked. Deliver those reads, and free
    // the chunk MDLs the descriptors reference, only once the resource free
    // has provably flushed the leftover DMA mappings: a free that never
    // reached the bus driver is retried by the next stop or start, and
    // completing the reads before it would unlock pages the retried (or
    // wedged) free then flushes into.
    //
    if (!flushed && PhotonicCaptureHasQuarantinedFrames(Capture)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                    "quarantined read(s) stay held: the isoch resource free did not complete, the DMA mappings may "
                    "be live\n");
        return;
    }

    for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
        PPHOTONIC_CAPTURE_FRAME desc = &Capture->Frames[i];

        if (desc->State == PhotonicFrameDetachFailed) {
            ULONG_PTR cookie = desc->Cookie;

            KeAcquireSpinLock(&Capture->Lock, &irql);
            desc->State = PhotonicFrameFree;
            desc->Cookie = 0;
            InsertTailList(&Capture->FreeList, &desc->ListEntry);
            KeReleaseSpinLock(&Capture->Lock, irql);

            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO,
                        "delivering quarantined read after resource free: cookie 0x%p\n", (PVOID) cookie);
            Capture->Deliver(Capture->CallbackContext, cookie, 0, STATUS_CANCELLED);
        }
    }

    //
    // The drain is complete, so no descriptor references its chunk MDLs anymore.
    //
    PhotonicCapturePoolFreeChunkMdls(Capture);
}

/// PhotonicCaptureTeardownReleaseSession -- PhotonicCaptureTeardownReleaseSessionResources
/// plus the front-end drain: no frames will be delivered now that the engine
/// is idle, so any read still parked in the front-end completes with zero
/// bytes. Must run at PASSIVE_LEVEL with InterfaceMutex held, after
/// PhotonicCaptureTeardownQuiesce.
///
/// @param Capture  Quiesced capture engine whose resources are released.
/// @param Stream   Stream extension whose parked reads are drained.
static VOID PhotonicCaptureTeardownReleaseSession(_In_ PPHOTONIC_CAPTURE Capture,
                                                  _In_ PPHOTONIC_STREAM_EXTENSION Stream) {
    PhotonicCaptureTeardownReleaseSessionResources(Capture);
    PhotonicStreamCompletePendingReads(Stream);
}

/// PhotonicCaptureTeardownDisableCamera -- stop the camera transmitting: withdraw a
/// still-deferred enable (ISO_EN was never set then), clear ISO_EN when it is
/// set, and cancel a single-frame acquisition that may still be armed with its
/// trigger not yet fired, otherwise a later stray trigger pulse would make the
/// camera transmit into a channel whose resources are released. A
/// surprise-removed camera cannot be written to (and sends nothing anyway), so
/// the register writes are skipped rather than riding out the transaction
/// timeout. Must run at PASSIVE_LEVEL with Extension->InterfaceMutex held.
///
/// @param Extension  Device extension for the camera to silence.
/// @param Stream     Stream extension carrying the single-frame flag.
/// @param Capture    Capture engine whose enable state is cleared.
static VOID PhotonicCaptureTeardownDisableCamera(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                                                 _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                                                 _In_ PPHOTONIC_CAPTURE Capture) {
    Capture->EnableWanted = FALSE;
    if (Capture->IsoEnabled) {
        if (!Extension->Removed) {
            (VOID) PhotonicDcamSetIsochEnable(Extension, FALSE);
        }
        Capture->IsoEnabled = FALSE;
    }

    if (Stream->OneShot && !Extension->Removed) {
        (VOID) PhotonicDcamOneShot(Extension, FALSE);
    }
}

/// PhotonicCaptureTeardownShutdown -- the one session teardown every stopper shares:
/// publish the stopped state, silence the camera, quiesce the engine and
/// release the session. Publishing KSSTATE_STOP under PendingLock comes
/// first, so a data request or a pump trigger racing this teardown
/// (TurnOffSynchronization) either parks in time for the drain below or
/// observes the state and fails fast. The stop publishes the state at its
/// own top and the close publishes it together with Rundown before calling
/// in; the publish here covers the control work item's cancel resolution,
/// which reaches this teardown through neither, and costs the other callers
/// only a redundant store. Must run at PASSIVE_LEVEL with InterfaceMutex
/// held.
///
/// @param Extension  Device extension for the camera to stop.
/// @param Stream     Stream extension whose state is published and reads drained.
/// @param Capture    Capture engine to quiesce and release.
VOID PhotonicCaptureTeardownShutdown(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                                     _In_ PPHOTONIC_CAPTURE Capture) {
    KIRQL irql;

    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    Stream->StreamState = KSSTATE_STOP;
    KeReleaseSpinLock(&Stream->PendingLock, irql);

    PhotonicCaptureTeardownDisableCamera(Extension, Stream, Capture);
    PhotonicCaptureTeardownQuiesce(Capture);
    PhotonicCaptureTeardownReleaseSession(Capture, Stream);
}
