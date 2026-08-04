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
/// The engine's buffer pump and per-frame attach/detach state machine:
/// submission laps, the asynchronous attach and detach IRBs, and their
/// completion routines. Everything here runs at DISPATCH_LEVEL on the
/// descriptors' pre-allocated IRPs. See capture.h.

// clang-format off
#include "capture.h"
#include "capture_private.h"
#include "pump.tmh"
// clang-format on

/// PhotonicCapturePumpIrpDone -- account one attach/detach IRP as no longer at the
/// bus driver (see the InFlightIrps invariant in capture.h). When a teardown is
/// draining and this was the last in-flight IRP, releases the quiesce to
/// release the isochronous resources. This must be the caller's LAST access to the
/// engine: once DrainDone is set, the quiesce proceeds and the resource handle
/// goes away.
///
/// @param Capture  Capture engine whose in-flight IRP count is decremented.
static VOID PhotonicCapturePumpIrpDone(_In_ PPHOTONIC_CAPTURE Capture) {
    if (InterlockedDecrement(&Capture->InFlightIrps) == 0 && Capture->Draining) {
        KeSetEvent(&Capture->DrainDone, IO_NO_INCREMENT, FALSE);
    }
}

/// PhotonicCapturePumpCallBus -- send one IRB to the 1394 bus driver asynchronously on
/// a reusable pre-allocated IRP, continuing in CompletionRoutine (which must return
/// STATUS_MORE_PROCESSING_REQUIRED so the IRP survives for the next reuse). Safe at
/// DISPATCH_LEVEL; never blocks.
///
/// @param Capture            Capture engine whose bus device object receives the IRP.
/// @param Irp                Pre-allocated reusable IRP to send.
/// @param Irb                IRB describing the 1394 request.
/// @param CompletionRoutine  I/O completion routine called when the IRP finishes.
/// @param Context            Context value passed to CompletionRoutine.
static VOID PhotonicCapturePumpCallBus(_In_ PPHOTONIC_CAPTURE Capture, _In_ PIRP Irp, _In_ PIRB Irb,
                                       _In_ PIO_COMPLETION_ROUTINE CompletionRoutine, _In_opt_ PVOID Context) {
    PIO_STACK_LOCATION stack;

    IoReuseIrp(Irp, STATUS_SUCCESS);
    stack = IoGetNextIrpStackLocation(Irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_1394_CLASS;
    stack->Parameters.Others.Argument1 = Irb;
    IoSetCompletionRoutine(Irp, CompletionRoutine, Context, TRUE, TRUE, TRUE);

    (VOID) IoCallDriver(Capture->Extension->PhysicalDeviceObject, Irp);
}

/// PhotonicCapturePumpDetachComplete -- I/O completion routine for the asynchronous
/// detach. Runs at IRQL <= DISPATCH_LEVEL in an arbitrary context. Recycles the
/// descriptor to the free list, hands the frame to the front-end (which completes
/// the read), and re-runs the front-end read pump so the recycled descriptor can
/// carry the next parked read. A failed detach leaves the buffer attached with a
/// live DMA mapping of the consumer's pages, so the descriptor is quarantined
/// instead: completing the read would unlock pages the bus driver can still flush
/// into (observed as a memcpy bugcheck inside REQUEST_ISOCH_FREE_RESOURCES).
/// PhotonicCaptureStop delivers quarantined reads once the resource is freed.
/// Returns STATUS_MORE_PROCESSING_REQUIRED so the I/O manager leaves the reusable
/// IRP for the next cycle.
///
/// @param DeviceObject  Device object (unused).
/// @param Irp           The completed detach IRP.
/// @param Context       The descriptor (PPHOTONIC_CAPTURE_FRAME) being detached.
/// @return STATUS_MORE_PROCESSING_REQUIRED always, to preserve the reusable IRP.
static IO_COMPLETION_ROUTINE PhotonicCapturePumpDetachComplete;
static NTSTATUS PhotonicCapturePumpDetachComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context) {
    PPHOTONIC_CAPTURE_FRAME desc = (PPHOTONIC_CAPTURE_FRAME) Context;
    PPHOTONIC_CAPTURE capture = desc->Capture;
    ULONG_PTR cookie = desc->Cookie;
    NTSTATUS completion = desc->CompletionStatus;
    ULONG bytes = NT_SUCCESS(completion) ? capture->FrameBytes : 0;
    KIRQL irql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!NT_SUCCESS(Irp->IoStatus.Status)) {
        KeAcquireSpinLock(&capture->Lock, &irql);
        desc->State = PhotonicFrameDetachFailed;
        KeReleaseSpinLock(&capture->Lock, irql);

        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                    "ISOCH_DETACH_BUFFERS failed: cookie 0x%p %!STATUS!; read quarantined until resource free\n",
                    (PVOID) cookie, Irp->IoStatus.Status);
        PhotonicCapturePumpIrpDone(capture);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    KeAcquireSpinLock(&capture->Lock, &irql);
    desc->State = PhotonicFrameFree;
    desc->Cookie = 0;
    InsertTailList(&capture->FreeList, &desc->ListEntry);
    KeReleaseSpinLock(&capture->Lock, irql);

    //
    // Hand the finished frame to the front-end. The pixels are already in the
    // consumer's pages (zero copy); this only completes the read.
    //
    capture->Deliver(capture->CallbackContext, cookie, bytes, completion);

    //
    // The recycled descriptor may let the front-end's next buffer attach
    // right now.
    //
    PhotonicCapturePump(capture);

    //
    // Nothing may touch capture/desc/Irp after the count drops (see
    // PhotonicCapturePumpIrpDone). Do not add work below this call.
    //
    PhotonicCapturePumpIrpDone(capture);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/// PhotonicCapturePumpBeginDetach -- start recycling a descriptor the caller has
/// already claimed under Lock: removed from the pending list, State set to
/// Detaching and CompletionStatus decided (the claim under Lock is the
/// single-completer handoff -- the frame callback tests State directly, so the
/// transition must not happen after the lock is dropped) and counted in
/// InFlightIrps. The isochronous context must be stopped first if the hardware
/// may still be filling the buffer (see PhotonicCaptureTeardownQuiesce). Issues the detach
/// as an asynchronous IRB on the descriptor's pre-allocated IRP so it is safe at
/// DISPATCH_LEVEL and never blocks; completion continues in
/// PhotonicCapturePumpDetachComplete.
///
/// @param Capture  Capture engine that owns the descriptor.
/// @param Desc     Descriptor to detach (already claimed and counted by the caller).
VOID PhotonicCapturePumpBeginDetach(_In_ PPHOTONIC_CAPTURE Capture, _In_ PPHOTONIC_CAPTURE_FRAME Desc) {
    RtlZeroMemory(&Desc->Irb, sizeof(Desc->Irb));
    Desc->Irb.FunctionNumber = REQUEST_ISOCH_DETACH_BUFFERS;
    Desc->Irb.u.IsochDetachBuffers.hResource = Capture->ResourceHandle;
    Desc->Irb.u.IsochDetachBuffers.nNumberOfDescriptors = Capture->ChunkCount;
    Desc->Irb.u.IsochDetachBuffers.pIsochDescriptor = Desc->Isoch;

    PhotonicCapturePumpCallBus(Capture, Desc->Irp, &Desc->Irb, PhotonicCapturePumpDetachComplete, Desc);
}

/// PhotonicCapturePumpIsochCallback -- ISOCH_DESCRIPTOR completion routine, invoked by
/// the bus driver at DISPATCH_LEVEL when a frame's DMA into the descriptor's MDL
/// finishes. Only the last chunk of a frame carries this callback, so one
/// invocation means the whole frame is in. Context1 is the engine, Context2 the
/// descriptor. A Pending descriptor
/// is removed from the pending list (winning the single-completer race) and its
/// recycle started. One still Attaching cannot start the detach -- its shared IRP is
/// still at the bus driver carrying the attach -- so it is only marked complete and
/// PhotonicCapturePumpAttachComplete begins the detach. Any other state means another
/// path (cancel or teardown) already owns the descriptor.
///
/// @param Context1  The capture engine (PPHOTONIC_CAPTURE).
/// @param Context2  The completed frame descriptor (PPHOTONIC_CAPTURE_FRAME).
static VOID PhotonicCapturePumpIsochCallback(_In_ PVOID Context1, _In_ PVOID Context2) {
    PPHOTONIC_CAPTURE capture = (PPHOTONIC_CAPTURE) Context1;
    PPHOTONIC_CAPTURE_FRAME desc = (PPHOTONIC_CAPTURE_FRAME) Context2;
    ULONG_PTR cookie;
    NTSTATUS frameStatus;
    BOOLEAN detach = FALSE;
    const char *outcome;

    KeAcquireSpinLockAtDpcLevel(&capture->Lock);
    //
    // Snapshot under the lock: once another path can own this descriptor its
    // fields may be recycled out from under the trace below.
    //
    cookie = desc->Cookie;
    frameStatus = desc->Isoch[capture->ChunkCount - 1].status;

    switch (desc->State) {
        case PhotonicFramePending:
            RemoveEntryList(&desc->ListEntry);
            desc->State = PhotonicFrameDetaching;
            //
            // The per-descriptor status is the one channel the bus driver has
            // for a receive-path error. A failed frame must not be delivered
            // as a full good one: the detach completion maps a failed
            // CompletionStatus to zero bytes used.
            //
            desc->CompletionStatus = NT_SUCCESS(frameStatus) ? STATUS_SUCCESS : frameStatus;
            InterlockedIncrement(&capture->InFlightIrps);
            detach = TRUE;
            outcome = "recycling";
            break;
        case PhotonicFrameAttaching:
            desc->State = PhotonicFrameCompletedWhileAttaching;
            outcome = "deferred to attach completion";
            break;
        default:
            outcome = "already taken";
            break;
    }
    KeReleaseSpinLockFromDpcLevel(&capture->Lock);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_ISO, "frame complete: cookie 0x%p status %!STATUS! (%s)\n",
                (PVOID) cookie, frameStatus, outcome);
    if (!NT_SUCCESS(frameStatus)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "frame DMA failed: cookie 0x%p status %!STATUS! (%s)\n",
                    (PVOID) cookie, frameStatus, outcome);
    }

    if (detach) {
        PhotonicCapturePumpBeginDetach(capture, desc);
    }
}

/// PhotonicCapturePumpAttachComplete -- I/O completion routine for the asynchronous
/// attach issued by the pump. Runs at IRQL <= DISPATCH_LEVEL. On
/// failure the buffer never attached -- the frame callback cannot have fired, so
/// the removal is unopposed -- and the read is failed through the delivery
/// callback. On success the descriptor either goes straight to detach (the frame
/// callback already fired; the in-flight count carries over from the attach to
/// the detach) or is published Pending: while live for the frame callback and
/// the teardown to take, while draining for the quiesce's claim loop, since
/// only the quiesce may detach an incomplete buffer. A live publish also
/// signals FirstAttachDone and, when nothing is listening yet, queues the
/// control work item: a completion routine issues no listen itself, the
/// executor does at PASSIVE_LEVEL.
///
/// @param DeviceObject  Device object (unused).
/// @param Irp           The completed attach IRP.
/// @param Context       The descriptor (PPHOTONIC_CAPTURE_FRAME) being attached.
/// @return STATUS_MORE_PROCESSING_REQUIRED always, to preserve the reusable IRP.
static IO_COMPLETION_ROUTINE PhotonicCapturePumpAttachComplete;
static NTSTATUS PhotonicCapturePumpAttachComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context) {
    PPHOTONIC_CAPTURE_FRAME desc = (PPHOTONIC_CAPTURE_FRAME) Context;
    PPHOTONIC_CAPTURE capture = desc->Capture;
    NTSTATUS status = Irp->IoStatus.Status;
    ULONG_PTR cookie = desc->Cookie;
    NTSTATUS frameStatus;
    BOOLEAN frameDone;
    BOOLEAN needListen;
    KIRQL irql;

    UNREFERENCED_PARAMETER(DeviceObject);

    KeAcquireSpinLock(&capture->Lock, &irql);

    if (!NT_SUCCESS(status)) {
        RemoveEntryList(&desc->ListEntry);
        desc->State = PhotonicFrameFree;
        desc->Cookie = 0;
        InsertTailList(&capture->FreeList, &desc->ListEntry);
        KeReleaseSpinLock(&capture->Lock, irql);

        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "ISOCH_ATTACH_BUFFERS failed: cookie 0x%p %!STATUS!\n",
                    (PVOID) cookie, status);
        capture->Deliver(capture->CallbackContext, cookie, 0, status);
        PhotonicCapturePumpIrpDone(capture);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    frameDone = desc->State == PhotonicFrameCompletedWhileAttaching;
    if (frameDone) {
        //
        // The frame-complete callback beat this routine; deliver the finished
        // frame by beginning the detach here, on the now-idle shared IRP.
        // Detaching a completed buffer is the steady-state operation, legal
        // whether or not the context is still running. The frame's own
        // outcome is the descriptor status the bus driver wrote back, same
        // as in the frame callback's claim.
        //
        frameStatus = desc->Isoch[capture->ChunkCount - 1].status;
        RemoveEntryList(&desc->ListEntry);
        desc->State = PhotonicFrameDetaching;
        desc->CompletionStatus = NT_SUCCESS(frameStatus) ? STATUS_SUCCESS : frameStatus;
        KeReleaseSpinLock(&capture->Lock, irql);

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_ISO,
                    "attach completed, frame already done: cookie 0x%p status %!STATUS!\n", (PVOID) cookie,
                    frameStatus);
        if (!NT_SUCCESS(frameStatus)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "frame DMA failed: cookie 0x%p status %!STATUS!\n",
                        (PVOID) cookie, frameStatus);
        }
        PhotonicCapturePumpBeginDetach(capture, desc);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (capture->Draining) {
        //
        // A teardown started while the attach was in flight. The buffer is
        // incomplete, and only the quiesce may detach an incomplete buffer
        // (its claim loop runs after its ISOCH_STOP, keeping the
        // stop-before-detach rule in one place), so publish the descriptor
        // Pending and leave it for the loop: the quiesce drains the in-flight
        // IRPs, this one included, and claims whatever they published.
        //
        desc->State = PhotonicFramePending;
        KeReleaseSpinLock(&capture->Lock, irql);

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_ISO,
                    "attach completed while draining: cookie 0x%p left for the quiesce\n", (PVOID) cookie);
        PhotonicCapturePumpIrpDone(capture);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    //
    // Attached and live: publish Pending so the frame callback and the teardown
    // can take the descriptor.
    //
    desc->State = PhotonicFramePending;
    needListen = !capture->Listening;
    KeReleaseSpinLock(&capture->Lock, irql);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_ISO, "attached buffer: cookie 0x%p (%u bytes)\n", (PVOID) cookie,
                capture->FrameBytes);

    //
    // A buffer is attached now, so the bus driver can accept a listen. Release
    // a starter waiting in PhotonicCaptureEnsureListen and, when nothing is
    // listening yet, hand the listen to the control work item.
    //
    KeSetEvent(&capture->FirstAttachDone, IO_NO_INCREMENT, FALSE);
    if (needListen) {
        PhotonicCaptureRequestWork(capture->Extension);
    }

    PhotonicCapturePumpIrpDone(capture);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/// PhotonicCapturePumpBeginAttach -- start attaching a primed descriptor's MDL to the
/// bus as the DMA target. The caller has already published the descriptor on the
/// pending list (State == Attaching) and counted the IRP under Lock. Issues the
/// attach as an asynchronous IRB on the descriptor's pre-allocated IRP so it is
/// safe at DISPATCH_LEVEL and never blocks; completion continues in
/// PhotonicCapturePumpAttachComplete.
///
/// @param Capture  Capture engine that owns the descriptor.
/// @param Desc     Primed descriptor to attach (already published by the caller).
static VOID PhotonicCapturePumpBeginAttach(_In_ PPHOTONIC_CAPTURE Capture, _In_ PPHOTONIC_CAPTURE_FRAME Desc) {
    RtlZeroMemory(&Desc->Irb, sizeof(Desc->Irb));
    Desc->Irb.FunctionNumber = REQUEST_ISOCH_ATTACH_BUFFERS;
    Desc->Irb.u.IsochAttachBuffers.hResource = Capture->ResourceHandle;
    Desc->Irb.u.IsochAttachBuffers.nNumberOfDescriptors = Capture->ChunkCount;
    Desc->Irb.u.IsochAttachBuffers.pIsochDescriptor = Desc->Isoch;

    PhotonicCapturePumpCallBus(Capture, Desc->Irp, &Desc->Irb, PhotonicCapturePumpAttachComplete, Desc);
}

/// PhotonicCapturePumpPrime -- (re)initialise a recycled descriptor's
/// ISOCH_DESCRIPTOR chain for a fresh frame: the consumer's MDL becomes the DMA
/// target, one frame long, beginning on a start-of-frame packet (sy == 1), firing
/// the completion callback when full. A frame larger than the bus driver's
/// per-buffer limit is described by ChunkCount descriptors, each viewing a
/// packet-aligned slice of the consumer's pages through the descriptor's reusable
/// partial MDL -- the DMA target is still the consumer's memory, no staging copy.
/// The first chunk gates DMA on the start-of-frame packet and later chunks fill in
/// sequence behind it; only the last chunk carries the completion callback, so one
/// callback means the whole frame is in. A recycled descriptor is never assumed
/// clean -- the bus/port driver stomps status/reserved fields -- so every field is
/// set.
///
/// @param Capture  Capture engine providing the chunk geometry and packet size.
/// @param Desc     Descriptor to prime for the new frame.
/// @param Cookie   Opaque cookie identifying the read (stored in Desc->Cookie).
/// @param Mdl      Consumer's locked MDL that is the DMA target.
static VOID PhotonicCapturePumpPrime(_In_ PPHOTONIC_CAPTURE Capture, _In_ PPHOTONIC_CAPTURE_FRAME Desc,
                                     _In_ ULONG_PTR Cookie, _In_ PMDL Mdl) {
    ULONG offset = 0;
    ULONG chunk;

    for (chunk = 0; chunk < Capture->ChunkCount; chunk++) {
        PISOCH_DESCRIPTOR iso = &Desc->Isoch[chunk];
        ULONG length = Capture->FrameBytes - offset;
        PMDL chunkMdl = Mdl;

        if (length > Capture->ChunkBytes) {
            length = Capture->ChunkBytes;
        }

        //
        // A single-chunk frame attaches the consumer's MDL directly. A multi-chunk
        // frame rebuilds this chunk's partial MDL over the consumer's pages;
        // MmPrepareMdlForReuse first, in case the bus/port driver mapped the
        // partial MDL on a previous cycle.
        //
        if (Capture->ChunkCount > 1) {
            chunkMdl = Desc->ChunkMdls[chunk];
            MmPrepareMdlForReuse(chunkMdl);
            IoBuildPartialMdl(Mdl, chunkMdl, (PUCHAR) MmGetMdlVirtualAddress(Mdl) + offset, length);
        }

        RtlZeroMemory(iso, sizeof(*iso));
        iso->Mdl = chunkMdl;
        iso->ulLength = length;
        iso->nMaxBytesPerFrame = Capture->BytesPerPacket;
        iso->ulTag = 0;
        if (chunk == 0) {
            iso->fulFlags = DESCRIPTOR_SYNCH_ON_SY;
            iso->ulSynch = 1; // DCAM tags the first packet of each frame sy = 1
        }
        if (chunk == Capture->ChunkCount - 1) {
            iso->Callback = PhotonicCapturePumpIsochCallback;
            iso->Context1 = Capture;
            iso->Context2 = Desc;
        }
        offset += length;
    }

    Desc->Cookie = Cookie;
    Desc->CompletionStatus = STATUS_SUCCESS;
}

/// PhotonicCapturePumpLap -- one lap of the pump: claim free descriptors while
/// the engine is running and fill each with the front-end's next ready buffer,
/// publishing it (Attaching) on the pending list before issuing the attach so
/// the frame callback and the teardown can see the cookie is taken. Bounded to
/// one pass over the descriptor pool. Only the elected pump runner calls this
/// (see PhotonicCapturePump), so buffers attach in exactly the order the
/// front-end hands them out.
///
/// @param Capture  Capture engine being pumped.
static VOID PhotonicCapturePumpLap(_In_ PPHOTONIC_CAPTURE Capture) {
    ULONG i;

    for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
        PPHOTONIC_CAPTURE_FRAME desc;
        ULONG_PTR cookie;
        PMDL mdl;
        KIRQL irql;

        //
        // Claim a free descriptor first, so a buffer is pulled from the
        // front-end only when it can actually be attached. While the engine
        // is idle nothing is pulled at all, which is what keeps reads parked
        // (buffer at the ready) until the start attaches them.
        //
        KeAcquireSpinLock(&Capture->Lock, &irql);
        if (Capture->Draining || IsListEmpty(&Capture->FreeList)) {
            KeReleaseSpinLock(&Capture->Lock, irql);
            return;
        }
        desc = CONTAINING_RECORD(RemoveHeadList(&Capture->FreeList), PHOTONIC_CAPTURE_FRAME, ListEntry);
        KeReleaseSpinLock(&Capture->Lock, irql);

        //
        // Pull the next buffer. The acquire callback takes the front-end's own
        // lock, so the engine lock is not held across it (no nested locks).
        //
        if (!Capture->Acquire(Capture->CallbackContext, &cookie, &mdl)) {
            KeAcquireSpinLock(&Capture->Lock, &irql);
            desc->State = PhotonicFrameFree;
            InsertHeadList(&Capture->FreeList, &desc->ListEntry);
            KeReleaseSpinLock(&Capture->Lock, irql);
            return;
        }

        //
        // Guard the zero-copy DMA target: the bus writes up to FrameBytes into
        // these pages, so attaching a buffer shorter than one frame would let
        // the host controller DMA past the consumer's buffer and corrupt
        // memory. Given how modes are negotiated the buffer always matches the
        // mode's sample size (>= FrameBytes), but a DMA length must never be
        // trusted against a client-supplied buffer, so fail the read rather
        // than risk the overrun.
        //
        if (mdl == NULL || MmGetMdlByteCount(mdl) < Capture->FrameBytes) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "buffer rejected: MDL %u bytes < frame %u bytes\n",
                        mdl != NULL ? MmGetMdlByteCount(mdl) : 0, Capture->FrameBytes);
            KeAcquireSpinLock(&Capture->Lock, &irql);
            desc->State = PhotonicFrameFree;
            InsertHeadList(&Capture->FreeList, &desc->ListEntry);
            KeReleaseSpinLock(&Capture->Lock, irql);
            Capture->Deliver(Capture->CallbackContext, cookie, 0, STATUS_INVALID_PARAMETER);
            continue;
        }

        //
        // Prime outside the lock: the descriptor is privately owned by this
        // lap (claimed from the free list, not yet published), so nothing
        // races the chunk rebuild and the MDL work stays off the lock hold.
        //
        PhotonicCapturePumpPrime(Capture, desc, cookie, mdl);

        //
        // Publish and attach. Draining is re-checked in the same lock hold as
        // the publish: a quiesce that began after the claim above must not
        // gain a new attach behind its drain, so the buffer is handed back
        // cancelled instead.
        //
        KeAcquireSpinLock(&Capture->Lock, &irql);
        if (Capture->Draining) {
            desc->State = PhotonicFrameFree;
            InsertHeadList(&Capture->FreeList, &desc->ListEntry);
            KeReleaseSpinLock(&Capture->Lock, irql);
            Capture->Deliver(Capture->CallbackContext, cookie, 0, STATUS_CANCELLED);
            return;
        }
        desc->State = PhotonicFrameAttaching;
        InsertTailList(&Capture->PendingList, &desc->ListEntry);
        InterlockedIncrement(&Capture->InFlightIrps);
        KeReleaseSpinLock(&Capture->Lock, irql);

        PhotonicCapturePumpBeginAttach(Capture, desc);

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_ISO, "submitted buffer: cookie 0x%p (%u bytes)\n", (PVOID) cookie,
                    Capture->FrameBytes);
    }
}

VOID PhotonicCapturePump(_In_ PPHOTONIC_CAPTURE Capture) {
    BOOLEAN again;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_ISO);

    //
    // Single-pumper election. A request that loses it flags a re-run: the
    // runner takes another lap, so no readiness is lost and buffers attach in
    // exactly the order the front-end hands them out.
    //
    KeAcquireSpinLock(&Capture->Lock, &irql);
    if (Capture->PumpActive) {
        Capture->PumpAgain = TRUE;
        KeReleaseSpinLock(&Capture->Lock, irql);
        return;
    }
    Capture->PumpActive = TRUE;
    KeReleaseSpinLock(&Capture->Lock, irql);

    do {
        KeAcquireSpinLock(&Capture->Lock, &irql);
        Capture->PumpAgain = FALSE;
        KeReleaseSpinLock(&Capture->Lock, irql);

        PhotonicCapturePumpLap(Capture);

        KeAcquireSpinLock(&Capture->Lock, &irql);
        again = Capture->PumpAgain;
        if (!again) {
            Capture->PumpActive = FALSE;
        }
        KeReleaseSpinLock(&Capture->Lock, irql);
    } while (again);
}
