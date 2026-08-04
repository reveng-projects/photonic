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
/// Photonic IOCTL handlers: the shared capture slot both IOCTL families
/// (video and imager) operate on, including the mapped DMA ring the capture
/// engine streams into, the transfer-info bookkeeping and the
/// frame-completion events. The family-specific lifecycle verbs are thin
/// wrappers in video.c and imager.c around the common prepare/start/stop/
/// unprepare machinery here. See ioctl/ioctl.c for the dispatch mechanism and
/// docs/ioctl-interface-design.md for the two-halves session model.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and frames.tmh (WPP-generated) must come after it. ioctl.h needs
// CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "dcam.h"
#include "capture.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "frames.tmh"
// clang-format on

/// One registered frame-completion event (PHOTONIC_IOCTL_REGISTER_EVENT): the
/// caller's event object, referenced so it outlives the handle, the 32-bit
/// user handle it was registered under (the unregister lookup key) and the
/// countdown of delivered frames left before the event fires. Type 0 arms a
/// countdown of 1 (the client's per-shot / per-fetch completion signal); Type
/// 1 arms a countdown of the mapped ring depth. An entry that fires is retired
/// (removed, dereferenced, freed). Linked on PHOTONIC_IOCTL_CAPTURE.EventList
/// under EventLock.
typedef struct _PHOTONIC_IOCTL_EVENT {
    LIST_ENTRY ListEntry;
    PKEVENT Event;
    ULONG Handle;
    ULONG Type;
    ULONG Countdown;
} PHOTONIC_IOCTL_EVENT, *PPHOTONIC_IOCTL_EVENT;

/// Write the PL_SUCCESS status echo into the shared buffer, but only when the
/// caller supplied room for it (pixelinkapi.dll passes a zero output length on
/// every lifecycle verb).
///
/// @param Request  Decoded IOCTL request.
VOID PhotonicIoctlEchoStatus(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    if (Request->OutputLength >= sizeof(PL_RETURN_CODE)) {
        *(PL_RETURN_CODE *) Request->Buffer = PL_SUCCESS;
        Request->Information = sizeof(PL_RETURN_CODE);
    }
}

/// Buffer source the capture engine's pump pulls from
/// (PPHOTONIC_CAPTURE_ACQUIRE): claim the next ring slot in index order, or
/// produce nothing when the ring is not in play (imager start), nothing is
/// mapped, or the next slot in rotation is not the engine's to attach (in
/// flight, or delivered and still with the caller). A produced slot is owned
/// by the engine and settles through the delivery callback: a delivered or
/// failed slot returns to the rotation there, so no rollback of
/// NextSubmitIndex is ever needed -- the rotation is a ring with no
/// distinguished origin, and a slot that failed simply re-attaches when the
/// rotation reaches it again. Safe at DISPATCH_LEVEL.
///
/// @param Context  IOCTL capture slot pointer.
/// @param Cookie   Receives the ring index as the frame cookie.
/// @param Mdl      Receives the slot's locked MDL.
/// @return TRUE when a slot was produced, FALSE when none is ready.
static BOOLEAN PhotonicIoctlFramesAcquire(_In_ PVOID Context, _Out_ PULONG_PTR Cookie, _Out_ PMDL *Mdl) {
    PPHOTONIC_IOCTL_CAPTURE capture = Context;
    BOOLEAN acquired = FALSE;
    ULONG index;
    KIRQL irql;

    //
    // The ring feeds the engine only when the receive half of the session is
    // in play. An imager start drives the transmit half alone: nothing is
    // attached, no listen is issued, and the camera free-runs with nobody
    // listening (see PHOTONIC_IOCTL_CAPTURE).
    //
    if (!capture->PumpRingOnStart) {
        return FALSE;
    }

    KeAcquireSpinLock(&capture->FrameLock, &irql);
    index = capture->NextSubmitIndex;
    if (capture->MappedFrameCount != 0 && capture->Frames[index].State == PhotonicIoctlFrameQueued) {
        capture->Frames[index].State = PhotonicIoctlFrameInFlight;
        capture->NextSubmitIndex = (index + 1) % capture->MappedFrameCount;
        *Cookie = (ULONG_PTR) index;
        *Mdl = capture->Frames[index].Mdl;
        acquired = TRUE;
    }
    KeReleaseSpinLock(&capture->FrameLock, irql);

    return acquired;
}

VOID PhotonicIoctlFramesRequeue(_In_ PPHOTONIC_IOCTL_CAPTURE Capture) {
    ULONG requeued = 0;
    ULONG i;
    KIRQL irql;

    KeAcquireSpinLock(&Capture->FrameLock, &irql);
    for (i = 0; i < Capture->MappedFrameCount; i++) {
        if (Capture->Frames[i].State == PhotonicIoctlFrameDelivered) {
            Capture->Frames[i].State = PhotonicIoctlFrameQueued;
            requeued++;
        }
    }
    KeReleaseSpinLock(&Capture->FrameLock, irql);

    if (requeued != 0) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL, "requeued %u delivered ring frame(s)\n", requeued);
    }
    if (Capture->Stream.Capture != NULL) {
        PhotonicCapturePump(Capture->Stream.Capture);
    }
}

/// Account one delivered frame against every registered event: decrement the
/// countdowns under EventLock, retire the entries that reach zero onto a local
/// list, then signal and release them outside the lock. Safe at
/// DISPATCH_LEVEL: the release defers a possible object deletion to a work
/// item, since the driver's reference may be the last one (the client can
/// close its event handle at any time) and last-reference deletion is not
/// legal above APC_LEVEL.
///
/// @param Capture  IOCTL capture slot whose event list is processed.
static VOID PhotonicIoctlFramesSignalEvents(_In_ PPHOTONIC_IOCTL_CAPTURE Capture) {
    LIST_ENTRY fired;
    PLIST_ENTRY entry;
    KIRQL irql;

    InitializeListHead(&fired);

    KeAcquireSpinLock(&Capture->EventLock, &irql);
    entry = Capture->EventList.Flink;
    while (entry != &Capture->EventList) {
        PPHOTONIC_IOCTL_EVENT event = CONTAINING_RECORD(entry, PHOTONIC_IOCTL_EVENT, ListEntry);

        entry = entry->Flink;
        if (--event->Countdown == 0) {
            RemoveEntryList(&event->ListEntry);
            InsertTailList(&fired, &event->ListEntry);
        }
    }
    KeReleaseSpinLock(&Capture->EventLock, irql);

    while (!IsListEmpty(&fired)) {
        PPHOTONIC_IOCTL_EVENT event = CONTAINING_RECORD(RemoveHeadList(&fired), PHOTONIC_IOCTL_EVENT, ListEntry);

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL, "signalling event handle 0x%x (type %u)\n", event->Handle,
                    event->Type);
        KeSetEvent(event->Event, IO_NO_INCREMENT, FALSE);
        ObDereferenceObjectDeferDelete(event->Event);
        ExFreePoolWithTag(event, PHOTONIC_POOL_TAG);
    }
}

/// Drop every registered event without signalling it: the slot is going away,
/// so no frame can fire them anymore. The client's per-shot waits time out and
/// unregister on their own (the entry is already gone; the unregister fails
/// benignly).
///
/// @param Capture  IOCTL capture slot whose event list is flushed.
static VOID PhotonicIoctlFramesFlushEvents(_In_ PPHOTONIC_IOCTL_CAPTURE Capture) {
    LIST_ENTRY flushed;
    KIRQL irql;

    InitializeListHead(&flushed);

    KeAcquireSpinLock(&Capture->EventLock, &irql);
    while (!IsListEmpty(&Capture->EventList)) {
        InsertTailList(&flushed, RemoveHeadList(&Capture->EventList));
    }
    KeReleaseSpinLock(&Capture->EventLock, irql);

    while (!IsListEmpty(&flushed)) {
        PPHOTONIC_IOCTL_EVENT event = CONTAINING_RECORD(RemoveHeadList(&flushed), PHOTONIC_IOCTL_EVENT, ListEntry);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "dropping registered event handle 0x%x (type %u)\n",
                    event->Handle, event->Type);
        ObDereferenceObject(event->Event);
        ExFreePoolWithTag(event, PHOTONIC_POOL_TAG);
    }
}

/// Delivery callback the capture engine invokes when a ring frame it owns
/// finishes (Cookie is the ring index the acquire callback produced). Records
/// the delivery for GET_TRANSFER_INFO, signals the registered events and
/// settles the slot's ownership. In single-frame mode a delivered slot passes
/// to the caller (Delivered): its buffer stays detached (so nothing writes the
/// pixels the caller is reading, not even the teardown's detach flush) until
/// the caller hands it back through the next SW_TRIGGER or start. In
/// continuous mode the protocol has no per-frame hand-back, so the slot
/// recycles into the rotation and the engine's pump re-arms it right after
/// this delivery. A cancelled or failed frame (stream stop, bus error) returns
/// to Queued and waits its turn in the rotation; the engine's pump is bounded
/// to one lap of its descriptor pool per run, so a persistently failing slot
/// cannot make it spin.
///
/// @param Context    IOCTL capture slot pointer.
/// @param Cookie     Ring index produced by the acquire callback.
/// @param BytesUsed  Bytes written into the frame buffer.
/// @param Status     Completion status from the capture engine.
static VOID PhotonicIoctlFramesDeliver(_In_ PVOID Context, _In_ ULONG_PTR Cookie, _In_ ULONG BytesUsed,
                                       _In_ NTSTATUS Status) {
    PPHOTONIC_IOCTL_CAPTURE capture = Context;
    ULONG index = (ULONG) Cookie;
    BOOLEAN callerOwned;
    KIRQL irql;

    if (index >= PHOTONIC_IOCTL_MAX_VIDEO_FRAMES) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "delivery with bad ring index %u\n", index);
        return;
    }

    callerOwned = NT_SUCCESS(Status) && capture->Stream.OneShot;

    KeAcquireSpinLock(&capture->FrameLock, &irql);
    capture->Frames[index].State = callerOwned ? PhotonicIoctlFrameDelivered : PhotonicIoctlFrameQueued;
    if (NT_SUCCESS(Status)) {
        capture->CurrentFrameIndex = index;
        capture->TotalFrameCount++;
    }
    KeReleaseSpinLock(&capture->FrameLock, irql);

    //
    // WPP encodes the trace level statically, so the level argument must be a
    // literal -- hence two calls rather than a conditional level.
    //
    if (NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL, "frame %u delivered: %u bytes (total %u, %s)\n", index,
                    BytesUsed, capture->TotalFrameCount,
                    callerOwned ? "slot with caller until requeue" : "slot recycled");
        PhotonicIoctlFramesSignalEvents(capture);
    } else {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "frame %u dropped: %!STATUS! (total %u)\n", index,
                    Status, capture->TotalFrameCount);
    }
}

/// Unlock and free every mapped ring frame and reset the ring bookkeeping.
/// The capture engine must be stopped: after PhotonicCaptureStop has drained,
/// no MDL is attached to the bus or referenced by a descriptor, so the
/// caller's pages can be released. A frame a quarantined descriptor still
/// holds (its detach failed and the resource free never confirmed the flush)
/// is normally skipped, because the bus driver can still flush the leftover
/// DMA mapping into its pages. A later stop retries the release and unlocks
/// through this path once the flush is confirmed (the loop covers the whole
/// ring array, not just the current map, so a frame held across an earlier
/// unmap is found again). Force overrides the skip for the final cleanup:
/// the locked pages are charged to the client process, and a charge that
/// survives process rundown bugchecks the system (PROCESS_HAS_LOCKED_PAGES),
/// so a slot on its way out must unlock even the quarantine-held frames.
/// Must run at PASSIVE_LEVEL.
///
/// @param Capture  IOCTL capture slot whose ring is unmapped.
/// @param Force    TRUE to unlock quarantine-held frames too. Final cleanup
///                 only, after the stop's release retry left the quarantine
///                 unresolved.
static VOID PhotonicIoctlFramesUnmap(_In_ PPHOTONIC_IOCTL_CAPTURE Capture, _In_ BOOLEAN Force) {
    PPHOTONIC_CAPTURE engine = Capture->Stream.Capture;
    ULONG held = 0;
    ULONG forced = 0;
    ULONG unmapped = 0;
    ULONG i;

    FuncEntry(TRACE_FLAG_IOCTL);

    for (i = 0; i < PHOTONIC_IOCTL_MAX_VIDEO_FRAMES; i++) {
        if (Capture->Frames[i].Mdl != NULL) {
            if (engine != NULL && PhotonicCaptureCookieQuarantined(engine, (ULONG_PTR) i)) {
                if (!Force) {
                    held++;
                    continue;
                }
                forced++;
            }
            MmUnlockPages(Capture->Frames[i].Mdl);
            IoFreeMdl(Capture->Frames[i].Mdl);
            Capture->Frames[i].Mdl = NULL;
            unmapped++;
        }
        Capture->Frames[i].State = PhotonicIoctlFrameQueued;
    }

    if (held != 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "%u ring frame(s) stay locked: quarantined with an unflushed DMA mapping\n", held);
    }
    if (forced != 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "unlocked %u quarantine-held ring frame(s) at final cleanup: the flush is unconfirmed, but a "
                    "locked-page charge surviving process rundown would bugcheck\n",
                    forced);
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "unmapped %u ring frame(s)\n", unmapped);

    Capture->MappedFrameCount = 0;
    Capture->NextSubmitIndex = 0;
    Capture->CurrentFrameIndex = (ULONG) -1;
    Capture->TotalFrameCount = 0;
}

/// Stop the IOCTL capture engine if a slot is prepared.
///
/// @param Extension  Device extension.
VOID PhotonicIoctlCaptureStop(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    PPHOTONIC_IOCTL_CAPTURE capture = Extension->IoctlCapture;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (capture == NULL) {
        return;
    }

    PhotonicCaptureStop(Extension, &capture->Stream);
}

/// Stop the capture engine, unmap the ring, flush events, destroy the engine
/// and free the IOCTL capture slot. Safe to call when no slot exists.
///
/// @param Extension  Device extension.
VOID PhotonicIoctlCaptureRelease(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    PPHOTONIC_IOCTL_CAPTURE capture = Extension->IoctlCapture;
    BOOLEAN quarantined;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (capture == NULL) {
        return;
    }

    //
    // Stop before destroy: the engine must hold no isochronous resources and
    // have no IRP at the bus driver when it is freed (the stop also cancels an
    // armed one-shot acquisition in the camera). Idempotent when the stream
    // never started; the stop also retries any release a previous stop could
    // not finish. With the engine drained, the caller's ring pages can be
    // unlocked and the registered events dropped -- so a client that dies
    // mid-snap with frames mapped and events registered leaks nothing.
    //
    PhotonicCaptureStop(Extension, &capture->Stream);

    //
    // The slot is going away, so no ring page may stay locked: the pages are
    // charged to the client process, and a charge that survives process
    // rundown bugchecks the system (PROCESS_HAS_LOCKED_PAGES). When the
    // stop's release retry still could not confirm the flush, unlock the
    // quarantine-held frames anyway: the isochronous context is stopped, only
    // the bus driver's staging flush is unconfirmed, and that risk is smaller
    // than the certain bugcheck at client exit.
    //
    quarantined = capture->Stream.Capture != NULL && PhotonicCaptureHasQuarantinedFrames(capture->Stream.Capture);
    PhotonicIoctlFramesUnmap(capture, /*Force*/ quarantined);
    PhotonicIoctlFramesFlushEvents(capture);

    Extension->IoctlCapture = NULL;
    Extension->ActiveCaptureStream = NULL;

    if (!PhotonicCaptureDestroy(&capture->Stream)) {
        //
        // The engine was leaked: a quarantined descriptor still holds a DMA
        // mapping the resource free never confirmed flushed. The engine's
        // stream extension and delivery context live inside this slot, so the
        // slot must leak with it (its ring pages are already unlocked above,
        // so only kernel pool leaks). The device stays usable; a new prepare
        // allocates a fresh slot.
        //
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "leaking the capture slot with its engine: quarantined frame(s) hold an unflushed DMA mapping\n");
        return;
    }

    ExFreePoolWithTag(capture, PHOTONIC_POOL_TAG);
}

/// Return TRUE when FileObject is the handle that prepared the IOCTL capture
/// slot, FALSE otherwise.
///
/// @param Extension   Device extension.
/// @param FileObject  File object to test.
/// @return TRUE if FileObject owns the capture slot, FALSE otherwise.
BOOLEAN PhotonicIoctlCaptureIsOwner(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PFILE_OBJECT FileObject) {
    return Extension->IoctlCapture != NULL && Extension->IoctlCapture->FileObject == FileObject;
}

BOOLEAN PhotonicIoctlCaptureCheckOwner(_In_ PPHOTONIC_IOCTL_REQUEST Request, _In_ PCSTR Verb) {
    if (Request->Extension->IoctlCapture->FileObject == Request->FileObject) {
        return TRUE;
    }
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                "%s from a handle that does not own the capture slot; rejected\n", Verb);
    return FALSE;
}

/// Shared prepare implementation used by both families: removed-device check,
/// DirectShow exclusion, slot-state checks, slot allocation, current-format
/// resolution, PhotonicCaptureCreate, and publish Extension->IoctlCapture.
///
/// @param Request         Decoded IOCTL request.
/// @param FrameCount      Ring depth requested by the video prepare; 0 for
///                        the imager prepare.
/// @param AllowReprepare  TRUE to release and rebuild a stopped slot (video
///                        rule); FALSE to fail whenever a slot exists
///                        (imager rule).
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlCapturePrepare(_Inout_ PPHOTONIC_IOCTL_REQUEST Request, _In_ ULONG FrameCount,
                                     _In_ BOOLEAN AllowReprepare) {
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;
    PPHOTONIC_IOCTL_CAPTURE capture;
    PPHOTONIC_VIDEO_MODE mode;
    ULONG bpp;
    ULONG interval;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "frame count %u, reprepare %s\n", FrameCount,
                AllowReprepare ? "allowed" : "rejected");

    //
    // Slot-state rules. The video prepare may re-prepare a stopped slot it
    // owns: the old slot is released and rebuilt below, picking up the fresh
    // frame count and whatever format the caller selected since the first
    // prepare. A slot prepared by another handle is never released here, so
    // one client cannot tear down another's session. The imager prepare has
    // no re-prepare path and fails whenever a slot exists; a running slot
    // rejects both.
    //
    if (extension->IoctlCapture != NULL) {
        if (!AllowReprepare || extension->IoctlCapture->Stream.StreamState != KSSTATE_STOP ||
            extension->IoctlCapture->FileObject != Request->FileObject) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                        "capture slot already exists (state %u); prepare rejected\n",
                        extension->IoctlCapture->Stream.StreamState);
            Request->Error = PL_ERROR_INVALID_STATE;
            return STATUS_INVALID_DEVICE_STATE;
        }
        PhotonicIoctlCaptureRelease(extension);
    }

    //
    // With no IOCTL slot in the picture, an active session belongs to the
    // DirectShow pin, which owns the camera and the capture engine while it
    // is open. The mirror check in SRB_OPEN_STREAM runs under the same
    // mutex, so exactly one of the two interfaces can claim the camera.
    //
    if (extension->ActiveCaptureStream != NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "DirectShow stream is open; IOCTL capture rejected\n");
        Request->Error = PL_ERROR_INVALID_STATE;
        return STATUS_DEVICE_BUSY;
    }

    capture = (PPHOTONIC_IOCTL_CAPTURE) ExAllocatePoolZero(NonPagedPoolNx, sizeof(*capture), PHOTONIC_POOL_TAG);
    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "failed to allocate the capture slot (%u bytes)\n",
                    (ULONG) sizeof(*capture));
        Request->Error = PL_ERROR_OUT_OF_MEMORY;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    capture->FrameCount = FrameCount;
    capture->FileObject = Request->FileObject;
    capture->CurrentFrameIndex = (ULONG) -1;
    KeInitializeSpinLock(&capture->FrameLock);
    KeInitializeSpinLock(&capture->EventLock);
    InitializeListHead(&capture->EventList);
    PhotonicStreamExtensionInit(&capture->Stream);

    //
    // Resolve the camera's live format/mode selection to the enumerated mode
    // the engine will stream, and derive the frame geometry the ring buffers
    // must match (MAP_VIDEO_FRAME validates its FrameSize against ImageSize).
    // Identical for both families, so the frame size is the same whichever
    // prepare created the slot.
    //
    status = PhotonicDcamGetCurrentMode(extension, &mode);
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_HARDWARE;
        goto fail;
    }

    bpp = PhotonicDcamCodingBpp(mode->PixelFormat);
    if (bpp == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "coding %u has no known depth\n", mode->PixelFormat);
        Request->Error = PL_ERROR_INVALID_STATE;
        status = STATUS_INVALID_DEVICE_STATE;
        goto fail;
    }

    capture->Stream.Mode = mode;
    capture->Stream.Width = mode->DefaultWidth;
    capture->Stream.Height = mode->DefaultHeight;
    capture->Stream.BitCount = bpp;
    capture->Stream.ImageSize = PhotonicImageBytes(mode->DefaultWidth, mode->DefaultHeight, bpp);

    //
    // Frame interval from the camera's live FRAME_RATE selection, so the rate
    // the caller set through SET_FRAME_RATE is the rate the stream configures.
    // When the read fails the mode's fastest rate stands in.
    //
    status = PhotonicDcamGetFrameRate(extension, &interval);
    if (!NT_SUCCESS(status) || interval == 0) {
        interval = mode->MinFrameInterval;
    }
    capture->Stream.FrameInterval = interval;

    status = PhotonicCaptureCreate(extension, &capture->Stream, PhotonicIoctlFramesDeliver, PhotonicIoctlFramesAcquire,
                                   capture);
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_NO_RESOURCES;
        goto fail;
    }

    extension->IoctlCapture = capture;
    extension->ActiveCaptureStream = &capture->Stream;

    PhotonicIoctlEchoStatus(Request);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL,
                "prepared: %u frames, F%u/M%u %ux%u coding=%u %u bytes/frame interval=%u\n", FrameCount, mode->Format,
                mode->Mode, capture->Stream.Width, capture->Stream.Height, mode->PixelFormat, capture->Stream.ImageSize,
                capture->Stream.FrameInterval);
    return STATUS_SUCCESS;

fail:
    ExFreePoolWithTag(capture, PHOTONIC_POOL_TAG);
    return status;
}

/// Shared start implementation used by both families: validates the slot, sets
/// Stream.OneShot from StartValue and calls PhotonicCaptureStart, which
/// publishes KSSTATE_RUN itself on success.
///
/// @param Request     Decoded IOCTL request.
/// @param StartValue  Transmission mode: non-zero for continuous (ISO_EN),
///                    zero for single-frame mode (SW_TRIGGER armed shots).
/// @param PumpRing    TRUE to requeue and attach the mapped ring (video start);
///                    FALSE for transmit-only without listen (imager start).
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlCaptureStart(_Inout_ PPHOTONIC_IOCTL_REQUEST Request, _In_ ULONG StartValue,
                                   _In_ BOOLEAN PumpRing) {
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;
    PPHOTONIC_IOCTL_CAPTURE capture = extension->IoctlCapture;
    PPHOTONIC_CAPTURE engine;
    BOOLEAN oneShot;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "start value 0x%x (%s transmission, %s ring pump)\n",
                StartValue, StartValue != 0 ? "continuous" : "single-frame", PumpRing ? "with" : "no");

    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "no prepared capture slot\n");
        Request->Error = PL_ERROR_NOT_STREAMING;
        return STATUS_UNSUCCESSFUL;
    }

    if (!PhotonicIoctlCaptureCheckOwner(Request, "start")) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    // The start value selects the transmission mode: non-zero enables
    // continuous transmission (ISO_EN); zero selects single-frame mode, where
    // ISO_EN stays off and each acquisition is individually armed through
    // SW_TRIGGER. A property of the start, not of the prepare: re-starting
    // (after a stop) with a different value flips the mode.
    //
    // A start on a running engine must not change the mode, though:
    // PhotonicCaptureStart is a no-op while streaming, so a flipped OneShot
    // or PumpRingOnStart would take effect with no engine transition to honor
    // it, desynchronizing delivery from transmission (a continuous client
    // over a camera that never transmits, or a free-running camera whose
    // delivered slots are never handed back). Reject the mismatch; an
    // identical repeated start stays harmless. InterfaceMutex is held, so
    // Draining is stable to read here.
    //
    oneShot = (BOOLEAN) (StartValue == 0);
    engine = capture->Stream.Capture;
    if (engine != NULL && !engine->Draining &&
        (capture->Stream.OneShot != oneShot || capture->PumpRingOnStart != PumpRing)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "start value 0x%x with %s ring pump would change the delivery mode of a running session\n",
                    StartValue, PumpRing ? "a" : "no");
        Request->Error = PL_ERROR_INVALID_STATE;
        return STATUS_INVALID_DEVICE_STATE;
    }
    capture->Stream.OneShot = oneShot;

    //
    // The video start commits whatever ring MAP_VIDEO_FRAME has already
    // mapped to the new stream (the DLL maps either before or after starting;
    // with nothing mapped yet the pump attaches nothing and MAP_VIDEO_FRAME
    // pumps instead): reclaim the slots still held by the caller from an
    // earlier run now, so the engine's start pump attaches the whole ring --
    // and confirms the listen -- before it enables the camera, and the
    // stream's first frame is captured rather than lost on the wire. The
    // imager start drives the transmit half only: PumpRingOnStart stays
    // clear, the acquire callback produces nothing, and with nothing
    // attached no listen is issued and the camera free-runs with nobody
    // listening.
    //
    capture->PumpRingOnStart = PumpRing;
    if (PumpRing) {
        PhotonicIoctlFramesRequeue(capture);
    }

    //
    // PhotonicCaptureStart succeeds without doing anything when the engine is
    // already streaming, so a repeated identical start is harmless (the
    // requeue above already recommitted the ring in that case; a start that
    // would change the delivery mode was rejected above).
    //
    // A video start is a receiving session: with the ring already mapped its
    // buffers attach right in the start, and with the map still to come the
    // camera enable is deferred until the map attaches the first frame -- so
    // in both orders the leading frame is captured, without any wait. An
    // imager start is transmit-only and must free-run immediately.
    //
    status = PhotonicCaptureStart(extension, &capture->Stream, /*ExpectBuffers*/ PumpRing);
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_HARDWARE;
        return status;
    }

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}

/// Stop the capture session, keeping the slot prepared so it can be restarted
/// without a re-prepare. Fails benignly with STATUS_UNSUCCESSFUL when no slot
/// exists.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or STATUS_UNSUCCESSFUL if no slot exists.
NTSTATUS PhotonicIoctlCaptureStopRequest(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;

    FuncEntry(TRACE_FLAG_IOCTL);

    //
    // Fail without a slot -- no LastError: the client's teardown interleaves
    // both families' stops and unprepares and ignores the trailing failures,
    // so this must fail gracefully (error return, no side effects). Runs even
    // on a surprise-removed device -- the host-side resources must still be
    // released (the stop path skips the camera register writes itself).
    //
    if (extension->IoctlCapture == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "no prepared capture slot; stop fails benignly\n");
        return STATUS_UNSUCCESSFUL;
    }

    if (!PhotonicIoctlCaptureCheckOwner(Request, "stop")) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Idempotent while the slot exists: stopping an engine that never started
    // (or already stopped) does nothing. The slot stays prepared, so a start
    // can run the stream again without a re-prepare.
    //
    PhotonicIoctlCaptureStop(extension);

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}

/// Release the capture slot (stop, unmap, engine destroy) and free it. Fails
/// benignly with STATUS_UNSUCCESSFUL when no slot exists.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or STATUS_UNSUCCESSFUL if no slot exists.
NTSTATUS PhotonicIoctlCaptureUnprepareRequest(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;

    FuncEntry(TRACE_FLAG_IOCTL);

    //
    // Fail without a slot -- no LastError: the interface errors here rather
    // than no-op'ing, and the client's unconditional teardown call (whichever
    // family's unprepare runs second) ignores the status.
    //
    if (extension->IoctlCapture == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "no prepared capture slot; unprepare fails benignly\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Only the handle that prepared the slot may release it. A second handle
    // unpreparing another client's live session would unlock ring pages that
    // client still expects frames in.
    //
    if (!PhotonicIoctlCaptureCheckOwner(Request, "unprepare")) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Runs even on a surprise-removed device -- the host-side resources must
    // still be released (the stop path skips the camera register writes
    // itself).
    //
    PhotonicIoctlCaptureRelease(extension);

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_MAP_VIDEO_FRAME -- lock FrameCount user-space frame buffers
/// (stride FrameSize, starting at the caller's base VA) as the DMA ring the
/// capture engine streams into. Each frame is probed and locked for write in
/// the caller's process context and described by its own MDL, so the
/// isochronous DMA lands directly in the caller's pages -- the same zero-copy
/// handoff the DirectShow read path gets from its framework-built MDLs.
/// FrameSize must match the frame size computed at prepare (LastError=0x19
/// otherwise); the slot may have been prepared by either family. When the
/// stream is already running the ring is attached immediately, which also
/// issues the deferred listen.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlMapVideoFrame(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_MAP_VIDEO_FRAME_IN32 *in = Request->Buffer;
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;
    PPHOTONIC_IOCTL_CAPTURE capture = extension->IoctlCapture;
    ULONG frameCount = in->FrameCount;
    ULONG frameSize = in->FrameSize;
    PUCHAR base = (PUCHAR) (ULONG_PTR) in->BaseVA;
    ULONG mapped;
    KIRQL irql;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "count=%u size=%u base=0x%08x\n", frameCount, frameSize,
                in->BaseVA);

    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "no prepared capture slot\n");
        Request->Error = PL_ERROR_NOT_PREPARED;
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!PhotonicIoctlCaptureCheckOwner(Request, "map")) {
        return STATUS_UNSUCCESSFUL;
    }

    if (capture->MappedFrameCount != 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "%u frame(s) already mapped\n", capture->MappedFrameCount);
        Request->Error = PL_ERROR_INVALID_STATE;
        return STATUS_INVALID_DEVICE_STATE;
    }

    //
    // A quarantined descriptor still holds a ring frame of the previous map
    // behind an unconfirmed resource-handle flush (the unmap left its pages
    // locked). Mapping a new ring over those slot indices would orphan the
    // held MDL, so refuse until a stop's release retry confirms the flush.
    //
    if (capture->Stream.Capture != NULL && PhotonicCaptureHasQuarantinedFrames(capture->Stream.Capture)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "cannot map: quarantined frame(s) of the previous ring hold an unflushed DMA mapping\n");
        Request->Error = PL_ERROR_INVALID_STATE;
        return STATUS_DEVICE_NOT_READY;
    }

    //
    // A frame an earlier unmap skipped as quarantine-held stays locked until
    // some path revisits it: the stop that later confirmed the flush unlocks
    // nothing itself. With no quarantined frame left (checked above), any
    // leftover MDL is safe to release now, and must be, before the new ring
    // overwrites its slot entry and orphans the locked pages for good.
    //
    PhotonicIoctlFramesUnmap(capture, /*Force*/ FALSE);

    if (frameCount == 0 || frameCount > PHOTONIC_IOCTL_MAX_VIDEO_FRAMES) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "frame count %u out of range (1..%u)\n", frameCount,
                    PHOTONIC_IOCTL_MAX_VIDEO_FRAMES);
        Request->Error = PL_ERROR_INVALID_COUNT;
        return STATUS_INVALID_PARAMETER;
    }

    //
    // The ring frames are the DMA targets for frames of the prepared mode, so
    // each must be exactly the prepared frame size (the caller derives it
    // from the same format queries).
    //
    if (frameSize != capture->Stream.ImageSize) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "frame size %u does not match prepared size %u\n", frameSize,
                    capture->Stream.ImageSize);
        Request->Error = PL_ERROR_BAD_FRAME_SIZE;
        return STATUS_INVALID_PARAMETER;
    }

    //
    // The caller is 32-bit, so the whole ring must fit its 32-bit address
    // space. This also keeps the per-frame offset arithmetic below from
    // wrapping SIZE_T on a 32-bit build, where a wrapped offset would probe
    // an unrelated range of the caller's address space.
    //
    if ((ULONGLONG) in->BaseVA + (ULONGLONG) frameCount * frameSize > (1ULL << 32)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "ring of %u frames x %u bytes at 0x%08x exceeds the 32-bit address space\n", frameCount, frameSize,
                    in->BaseVA);
        Request->Error = PL_ERROR_INVALID_COUNT;
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Lock the caller's frames one by one. This runs in the caller's process
    // context (METHOD_BUFFERED dispatch), which is what makes the user-mode
    // VAs probeable; an unreadable or unwritable frame unwinds everything
    // locked so far.
    //
    status = STATUS_SUCCESS;
    for (mapped = 0; mapped < frameCount; mapped++) {
        PMDL mdl = IoAllocateMdl(base + (SIZE_T) mapped * frameSize, frameSize, FALSE, FALSE, NULL);

        if (mdl == NULL) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                        "failed to allocate the MDL for ring frame %u (%u bytes)\n", mapped, frameSize);
            Request->Error = PL_ERROR_OUT_OF_MEMORY;
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        __try {
            MmProbeAndLockPages(mdl, UserMode, IoWriteAccess);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "frame %u at %p failed to lock: %!STATUS!\n", mapped,
                        base + (SIZE_T) mapped * frameSize, status);
            IoFreeMdl(mdl);
            Request->Error = PL_ERROR_LOCK_FAILED;
            break;
        }

        capture->Frames[mapped].Mdl = mdl;
        capture->Frames[mapped].State = PhotonicIoctlFrameQueued;
    }

    if (!NT_SUCCESS(status)) {
        capture->MappedFrameCount = mapped;
        PhotonicIoctlFramesUnmap(capture, /*Force*/ FALSE);
        return status;
    }

    //
    // Publish the ring under FrameLock, which guards these fields for the
    // acquire and delivery callbacks. The pump picks it up from slot 0; when
    // the stream is not running yet, the video start pumps instead.
    //
    KeAcquireSpinLock(&capture->FrameLock, &irql);
    capture->NextSubmitIndex = 0;
    capture->CurrentFrameIndex = (ULONG) -1;
    capture->TotalFrameCount = 0;
    capture->MappedFrameCount = frameCount;
    KeReleaseSpinLock(&capture->FrameLock, irql);

    if (capture->Stream.StreamState == KSSTATE_RUN && capture->Stream.Capture != NULL) {
        PhotonicCapturePump(capture->Stream.Capture);
    }

    PhotonicIoctlEchoStatus(Request);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "mapped %u ring frame(s) of %u bytes\n", frameCount,
                frameSize);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_UNMAP_VIDEO_FRAME -- unlock every frame mapped by
/// MAP_VIDEO_FRAME and reset the ring. The engine cannot keep streaming into
/// pages about to be unlocked, so a still-running stream is stopped first --
/// the caller must start again after a re-map, as the client's
/// stop/unmap/remap cycle does. Unmap with nothing mapped (or nothing
/// prepared) is a no-op: the DLL's cleanup paths call it unconditionally.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlUnmapVideoFrame(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;
    PPHOTONIC_IOCTL_CAPTURE capture = extension->IoctlCapture;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (capture != NULL) {
        if (!PhotonicIoctlCaptureCheckOwner(Request, "unmap")) {
            return STATUS_UNSUCCESSFUL;
        }
        PhotonicIoctlCaptureStop(extension);
        PhotonicIoctlFramesUnmap(capture, /*Force*/ FALSE);
    }

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_GET_TRANSFER_INFO -- report the delivery bookkeeping of the
/// shared capture slot: the ring index of the last delivered frame (the slot
/// the client reads the pixels from after its registered event fires) and the
/// running delivered total.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlGetTransferInfo(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_GET_TRANSFER_INFO_OUT *out = Request->Buffer;
    PPHOTONIC_IOCTL_CAPTURE capture = Request->Extension->IoctlCapture;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "no prepared capture slot\n");
        Request->Error = PL_ERROR_NOT_STREAMING;
        return STATUS_UNSUCCESSFUL;
    }

    KeAcquireSpinLock(&capture->FrameLock, &irql);
    out->CurrentFrameIndex = capture->CurrentFrameIndex;
    out->TotalFrameCount = capture->TotalFrameCount;
    KeReleaseSpinLock(&capture->FrameLock, irql);
    out->Status = PL_SUCCESS;
    Request->Information = sizeof(*out);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL, "current frame %u, total %u\n", out->CurrentFrameIndex,
                out->TotalFrameCount);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_REGISTER_EVENT -- reference the caller's event handle and
/// arm it with a delivered-frame countdown: Type 0 fires on the next delivered
/// frame (the client's per-shot / per-fetch completion signal, re-registered
/// before every wait), Type 1 once a full ring's worth of frames
/// (MappedFrameCount) has been delivered. The entry is retired when it fires;
/// UNREGISTER_EVENT removes it early (the client does so on a wait timeout).
/// Requires the slot and a mapped ring, whichever family prepared the slot.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlRegisterEvent(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_REGISTER_EVENT_IN32 *in = Request->Buffer;
    PPHOTONIC_IOCTL_CAPTURE capture = Request->Extension->IoctlCapture;
    PPHOTONIC_IOCTL_EVENT event;
    KIRQL irql;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "handle 0x%x type %u\n", in->Handle, in->Type);

    if (capture == NULL || capture->MappedFrameCount == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "no capture slot / no mapped frames\n");
        Request->Error = PL_ERROR_NOT_STREAMING;
        return STATUS_UNSUCCESSFUL;
    }

    if (!PhotonicIoctlCaptureCheckOwner(Request, "event registration")) {
        return STATUS_UNSUCCESSFUL;
    }

    if (in->Type != 0 && in->Type != 1) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "invalid event type %u\n", in->Type);
        Request->Error = PL_ERROR_FORMAT_UNAVAILABLE;
        return STATUS_INVALID_PARAMETER;
    }

    event = (PPHOTONIC_IOCTL_EVENT) ExAllocatePoolZero(NonPagedPoolNx, sizeof(*event), PHOTONIC_POOL_TAG);
    if (event == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "failed to allocate the event registration (%u bytes)\n",
                    (ULONG) sizeof(*event));
        Request->Error = PL_ERROR_OUT_OF_MEMORY;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Reference the caller's event object; runs in the caller's process
    // context (METHOD_BUFFERED dispatch), which is what makes the 32-bit user
    // handle resolvable.
    //
    status = ObReferenceObjectByHandle((HANDLE) (ULONG_PTR) in->Handle, EVENT_MODIFY_STATE, *ExEventObjectType,
                                       UserMode, (PVOID *) &event->Event, NULL);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "event handle 0x%x not referencable: %!STATUS!\n", in->Handle,
                    status);
        ExFreePoolWithTag(event, PHOTONIC_POOL_TAG);
        Request->Error = PL_ERROR_INVALID_COUNT;
        return status;
    }

    event->Handle = in->Handle;
    event->Type = in->Type;
    event->Countdown = in->Type == 0 ? 1 : capture->MappedFrameCount;

    KeAcquireSpinLock(&capture->EventLock, &irql);
    InsertTailList(&capture->EventList, &event->ListEntry);
    KeReleaseSpinLock(&capture->EventLock, irql);

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_UNREGISTER_EVENT -- remove a registered entry by its user
/// handle before it fires (the client only calls this on a shot timeout) and
/// drop the object reference. Fails with STATUS_NOT_FOUND -- and no LastError,
/// since racing a concurrent delivery that just retired the entry is benign --
/// when no matching entry exists.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or STATUS_NOT_FOUND if no match exists.
NTSTATUS PhotonicIoctlUnregisterEvent(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_UNREGISTER_EVENT_IN32 *in = Request->Buffer;
    PPHOTONIC_IOCTL_CAPTURE capture = Request->Extension->IoctlCapture;
    PPHOTONIC_IOCTL_EVENT found = NULL;
    PLIST_ENTRY entry;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_IOCTL);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "handle 0x%x type %u\n", in->Handle, in->Type);

    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "no prepared capture slot\n");
        return STATUS_NOT_FOUND;
    }

    if (!PhotonicIoctlCaptureCheckOwner(Request, "event unregistration")) {
        return STATUS_UNSUCCESSFUL;
    }

    KeAcquireSpinLock(&capture->EventLock, &irql);
    for (entry = capture->EventList.Flink; entry != &capture->EventList; entry = entry->Flink) {
        PPHOTONIC_IOCTL_EVENT candidate = CONTAINING_RECORD(entry, PHOTONIC_IOCTL_EVENT, ListEntry);

        if (candidate->Handle == in->Handle && candidate->Type == in->Type) {
            RemoveEntryList(entry);
            found = candidate;
            break;
        }
    }
    KeReleaseSpinLock(&capture->EventLock, irql);

    if (found == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "no registered event matches handle 0x%x type %u\n",
                    in->Handle, in->Type);
        return STATUS_NOT_FOUND;
    }

    ObDereferenceObject(found->Event);
    ExFreePoolWithTag(found, PHOTONIC_POOL_TAG);

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}
