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
/// Read data path for the Photonic stream minidriver: SRB_READ_DATA handling
/// and the parked-read queue, the acquire and deliver callbacks the capture
/// engine pulls frames through, read cancellation, and the data-path gate
/// the close's rundown waits on.

// clang-format off
// read.tmh (WPP-generated) must come last.
#include "photonic.h"
#include "capture.h"
#include "stream_private.h"
#include "read.tmh"
// clang-format on

/// PhotonicStreamCompleteRead -- finish one SRB_READ_DATA. Fills the KSSTREAM_HEADER for
/// the frame just delivered (BytesWritten == 0 marks a non-delivered sample), restores
/// the request timeout cleared when it was parked, and completes the request.
/// ReadyForNextStreamDataRequest is issued when the read is first received, not here.
///
/// @param Stream        Stream extension for this capture pin.
/// @param Srb           Read request to complete.
/// @param BytesWritten  Number of frame bytes written into the buffer (0 if not delivered).
/// @param Status        Completion status for the read.
static VOID PhotonicStreamCompleteRead(_In_ PPHOTONIC_STREAM_EXTENSION Stream, _In_ PHW_STREAM_REQUEST_BLOCK Srb,
                                       _In_ ULONG BytesWritten, _In_ NTSTATUS Status) {
    PKSSTREAM_HEADER header = Srb->CommandData.DataBufferArray;
    ULONG frameNumber;

    FuncEntry(TRACE_FLAG_STREAM);

    //
    // PictureNumber counts frames captured in the running session, and the
    // graph's quality management derives drop statistics from gaps in it. A
    // delivered frame claims its 1-based number atomically: two detach
    // completions can finish on different CPUs at once, and bare increments
    // would duplicate and skip values. A non-delivered completion (stop
    // drain, cancel, failure) claims no number and reports the count as it
    // stands, so drained reads cannot fabricate phantom drops.
    //
    if (BytesWritten != 0) {
        frameNumber = (ULONG) InterlockedIncrement((volatile LONG *) &Stream->FrameNumber);
    } else {
        frameNumber = Stream->FrameNumber;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_STREAM, "completing read frame %u: %u bytes, %!STATUS!\n", frameNumber,
                BytesWritten, Status);

    header->DataUsed = BytesWritten;
    header->Duration = Stream->FrameInterval;
    header->PresentationTime.Numerator = 1;
    header->PresentationTime.Denominator = 1;
    header->OptionsFlags = BytesWritten != 0 ? KSSTREAM_HEADER_OPTIONSF_SPLICEPOINT : 0;

    //
    // Fill the KS_FRAME_INFO extended header that follows the stream header on
    // video pins (declared via StreamHeaderMediaSpecific in PhotonicStreamOpen).
    // A client that connected without extended headers sends plain-sized headers,
    // so only fill it when the header is large enough to carry it.
    //
    if (header->Size >= sizeof(KSSTREAM_HEADER) + sizeof(KS_FRAME_INFO)) {
        PKS_FRAME_INFO frameInfo = (PKS_FRAME_INFO) (header + 1);

        frameInfo->ExtendedHeaderSize = sizeof(KS_FRAME_INFO);
        frameInfo->dwFrameFlags = KS_VIDEO_FLAG_FRAME;
        frameInfo->PictureNumber = (LONGLONG) frameNumber;
        frameInfo->DropCount = 0;
    }

    Srb->TimeoutCounter = Srb->TimeoutOriginal;
    Srb->Status = Status;
    StreamClassStreamNotification(StreamRequestComplete, Srb->StreamObject, Srb);
}

/// PhotonicStreamDeliverFrame -- the capture engine's per-frame delivery callback
/// (PPHOTONIC_CAPTURE_DELIVER). Invoked at IRQL <= DISPATCH_LEVEL when a frame's
/// buffer has been detached and its descriptor recycled (delivered, cancelled or
/// torn down alike), or when its attach failed. The pixels are already in the
/// consumer's pages (zero copy); this only fills the stream header in place and
/// completes the read. Cookie is the read SRB pointer.
///
/// @param Context    Stream extension passed as the callback context.
/// @param Cookie     Opaque cookie identifying the read (the SRB pointer).
/// @param BytesUsed  Captured frame size reported by the engine.
/// @param Status     Completion status for the frame.
VOID PhotonicStreamDeliverFrame(_In_ PVOID Context, _In_ ULONG_PTR Cookie, _In_ ULONG BytesUsed, _In_ NTSTATUS Status) {
    PPHOTONIC_STREAM_EXTENSION stream = (PPHOTONIC_STREAM_EXTENSION) Context;
    PHW_STREAM_REQUEST_BLOCK srb = (PHW_STREAM_REQUEST_BLOCK) Cookie;
    PKSSTREAM_HEADER header = srb->CommandData.DataBufferArray;
    ULONG used = BytesUsed;

    FuncEntry(TRACE_FLAG_STREAM);

    //
    // The frame is at most the read buffer's extent; a delivered frame reports its
    // captured size, a cancelled/failed one reports zero.
    //
    if (used > header->FrameExtent) {
        used = header->FrameExtent;
    }
    if (!NT_SUCCESS(Status)) {
        used = 0;
    }

    PhotonicStreamCompleteRead(stream, srb, used, Status);
}

/// PhotonicStreamAcquireBuffer -- the stream's buffer source
/// (PPHOTONIC_CAPTURE_ACQUIRE). The engine's pump calls it at
/// IRQL <= DISPATCH_LEVEL each time a free descriptor can carry a read: pop the
/// oldest parked read and hand out its framework-built MDL
/// (Srb->Irp->MdlAddress) as the zero-copy DMA target. A produced read is owned
/// by the engine and completes through PhotonicStreamDeliverFrame whatever
/// happens to it. A read without an MDL is completed failed right here and the
/// next one is offered instead.
///
/// @param Context  Stream extension passed as the callback context.
/// @param Cookie   Receives the read SRB pointer as the frame cookie.
/// @param Mdl      Receives the read's framework-built MDL.
/// @return TRUE when a read was produced, FALSE when none is parked.
BOOLEAN PhotonicStreamAcquireBuffer(_In_ PVOID Context, _Out_ PULONG_PTR Cookie, _Out_ PMDL *Mdl) {
    PPHOTONIC_STREAM_EXTENSION stream = (PPHOTONIC_STREAM_EXTENSION) Context;
    KIRQL irql;

    for (;;) {
        PLIST_ENTRY entry;
        PPHOTONIC_SRB_EXTENSION ext;
        PHW_STREAM_REQUEST_BLOCK srb;
        PMDL mdl;

        KeAcquireSpinLock(&stream->PendingLock, &irql);
        if (IsListEmpty(&stream->PendingReads)) {
            KeReleaseSpinLock(&stream->PendingLock, irql);
            return FALSE;
        }
        entry = RemoveHeadList(&stream->PendingReads);
        ext = CONTAINING_RECORD(entry, PHOTONIC_SRB_EXTENSION, ListEntry);
        ext->Queued = FALSE;
        KeReleaseSpinLock(&stream->PendingLock, irql);

        srb = ext->Srb;

        //
        // The class driver builds a locked MDL over every read's frame buffer and
        // hangs it off the IRP, independent of the stream's Dma/Pio flags. That MDL
        // is the zero-copy DMA target. The driver never maps the buffer itself.
        //
        mdl = srb->Irp != NULL ? srb->Irp->MdlAddress : NULL;
        if (mdl == NULL) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM, "read has no MDL; completing with 0 bytes\n");
            PhotonicStreamCompleteRead(stream, srb, 0, STATUS_INVALID_PARAMETER);
            continue;
        }

        *Cookie = (ULONG_PTR) srb;
        *Mdl = mdl;
        return TRUE;
    }
}

/// PhotonicStreamEnterDataPath -- enter the data-path gate (see the gate
/// fields in PHOTONIC_STREAM_EXTENSION). Returns FALSE once the close has
/// begun its rundown; the caller must then complete its request without
/// touching the stream extension or the capture engine. Safe at
/// DISPATCH_LEVEL.
///
/// @param Stream  Stream extension whose gate is entered.
/// @return TRUE when the caller may run, FALSE when the stream is closing.
BOOLEAN PhotonicStreamEnterDataPath(_In_ PPHOTONIC_STREAM_EXTENSION Stream) {
    BOOLEAN entered = FALSE;
    KIRQL irql;

    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    if (!Stream->Rundown) {
        Stream->DataPathRefs++;
        entered = TRUE;
    }
    KeReleaseSpinLock(&Stream->PendingLock, irql);
    return entered;
}

/// PhotonicStreamLeaveDataPath -- leave the data-path gate. When this was the
/// last thread inside and the close has begun its rundown, wake the close's
/// wait. Rundown is published under the same lock before the close samples
/// the count, so the wakeup cannot be lost. Safe at DISPATCH_LEVEL.
///
/// @param Stream  Stream extension whose gate is left.
VOID PhotonicStreamLeaveDataPath(_In_ PPHOTONIC_STREAM_EXTENSION Stream) {
    BOOLEAN idle;
    KIRQL irql;

    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    Stream->DataPathRefs--;
    idle = Stream->DataPathRefs == 0 && Stream->Rundown;
    KeReleaseSpinLock(&Stream->PendingLock, irql);

    if (idle) {
        KeSetEvent(&Stream->DataPathIdle, IO_NO_INCREMENT, FALSE);
    }
}

/// PhotonicStreamCompletePendingReads -- complete every parked read with zero bytes.
/// Called when capture stops, on flush, and at close, where no frame will ever be
/// delivered to the queued requests.
///
/// @param Stream  Stream extension whose parked-read queue is drained.
VOID PhotonicStreamCompletePendingReads(_In_ PPHOTONIC_STREAM_EXTENSION Stream) {
    ULONG drained = 0;

    FuncEntry(TRACE_FLAG_STREAM);

    for (;;) {
        PLIST_ENTRY entry;
        PPHOTONIC_SRB_EXTENSION ext;
        KIRQL irql;

        KeAcquireSpinLock(&Stream->PendingLock, &irql);
        if (IsListEmpty(&Stream->PendingReads)) {
            KeReleaseSpinLock(&Stream->PendingLock, irql);
            break;
        }
        entry = RemoveHeadList(&Stream->PendingReads);
        ext = CONTAINING_RECORD(entry, PHOTONIC_SRB_EXTENSION, ListEntry);
        ext->Queued = FALSE;
        KeReleaseSpinLock(&Stream->PendingLock, irql);

        PhotonicStreamCompleteRead(Stream, ext->Srb, 0, STATUS_SUCCESS);
        drained++;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM, "drained %u parked read(s)\n", drained);
}

/// PhotonicStreamCancelRead -- cancel a single read wherever it lives. A parked
/// read (the Queued flag and the extension's Srb back-pointer, both under
/// PendingLock, arbitrate against the acquire and drain paths) is completed
/// cancelled directly. A read that is not parked is
/// owned by the engine, or in the pump's hands on its way into it, and is
/// handed to the control work item: the work item quiesces the engine, which
/// detaches and completes every read it holds -- a buffer the pump publishes
/// after the quiesce began is handed straight back cancelled -- so the
/// cancelled read completes in every interleaving. An attached buffer must
/// never complete before it is detached, or the controller could DMA into
/// pages the framework has reclaimed; routing the cancel through the quiesce
/// is what enforces that. On a stopped (or closing) stream the teardown drain
/// already owns and completes every read the engine held, so nothing is
/// queued -- the engine may already be destroyed by the close. A read found
/// nowhere is already completing and the spurious quiesce it triggers is a
/// harmless stream hiccup.
///
/// @param Stream  Stream extension to search for the read.
/// @param Srb     Read request to cancel.
VOID PhotonicStreamCancelRead(_In_ PPHOTONIC_STREAM_EXTENSION Stream, _In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_SRB_EXTENSION ext = (PPHOTONIC_SRB_EXTENSION) Srb->SRBExtension;
    BOOLEAN owned = FALSE;
    BOOLEAN stopped;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_STREAM);

    //
    // Claim the read only when the extension really describes a parked
    // instance of this SRB. The back-pointer match guards the first-use case:
    // a cancel can run before the data handler has initialized the extension,
    // and trusting a garbage Queued alone would walk a garbage ListEntry.
    //
    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    if (ext->Queued && ext->Srb == Srb) {
        RemoveEntryList(&ext->ListEntry);
        ext->Queued = FALSE;
        owned = TRUE;
    }
    stopped = Stream->StreamState == KSSTATE_STOP;
    KeReleaseSpinLock(&Stream->PendingLock, irql);

    if (owned) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM, "cancel read: removed parked and cancelled\n");
        PhotonicStreamCompleteRead(Stream, Srb, 0, STATUS_CANCELLED);
        return;
    }

    //
    // Stopped is published under PendingLock before the teardown drains, so
    // observing it here means the drain owns the read and completes it. Do
    // not touch the capture engine: a close may already have destroyed it.
    //
    if (stopped) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM,
                    "cancel read: stream stopped, the teardown drain completes the read\n");
        return;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM,
                "cancel read: not parked; handing to the control work item\n");
    if (Stream->Capture != NULL) {
        PhotonicCaptureNotifyCancel(Stream->Capture);
    }
}

/// PhotonicStreamReceiveReadData -- handle one SRB_READ_DATA. The framework has already
/// built a locked MDL over this read's frame buffer; the driver will
/// hand that MDL to the bus as the DMA target so the frame lands directly in the
/// consumer's pages -- no copy. The request is parked and the engine's pump is
/// run, which pulls it through the acquire callback and attaches it (the whole
/// path is DISPATCH-safe); the read completes
/// later via the delivery callback. While the engine is idle (before KSSTATE_RUN)
/// the read stays parked, so its buffer is attached before the camera is enabled and
/// the first transmitted frame is captured. A read arriving while the pin is at
/// KSSTATE_STOP is completed cancelled instead of parked: nothing drains the queue
/// again on a stopped pin, so parking would pend the read forever. The class driver
/// is told it may send the next read immediately, so several buffers can be attached
/// at once.
///
/// @param Stream  Stream extension for this capture pin.
/// @param Srb     Read request carrying the frame buffer.
static VOID PhotonicStreamReceiveReadData(_In_ PPHOTONIC_STREAM_EXTENSION Stream, _In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PPHOTONIC_SRB_EXTENSION ext = (PPHOTONIC_SRB_EXTENSION) Srb->SRBExtension;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_STREAM);

    //
    // The per-request extension is uninitialized workspace; initialize it as
    // early as possible. The SRB is findable by a racing HwCancelPacket even
    // before these stores. That is safe: the cancel claims the read only when
    // Queued is set and the extension's Srb back-pointer matches this SRB,
    // every completion path leaves a recycled extension with Queued == FALSE,
    // and a first-use extension with arbitrary content fails the back-pointer
    // match, so a cancel in that sub-window takes the not-parked path. The
    // guard is sound only if Queued == FALSE is visible before the
    // back-pointer can match, so both stores run under PendingLock, which
    // orders them against the cancel's claim. Bare stores could be reordered
    // and let a garbage Queued byte pair with a matching back-pointer on a
    // first-use extension.
    //
    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    ext->Queued = FALSE;
    ext->Srb = Srb;
    KeReleaseSpinLock(&Stream->PendingLock, irql);

    //
    // The engine attaches exactly one MDL and fills exactly one stream header
    // per read. kswdmcap submits one header per read, but a raw client can
    // send IOCTL_KS_READ_STREAM with several, which stream.sys forwards as
    // NumberOfBuffers > 1 with one MDL per buffer chained off the IRP, or
    // with none at all, which arrives as NumberOfBuffers == 0 and a NULL
    // DataBufferArray. Completing a multi-header read as if it had one header
    // would return the other headers untouched under a success status, so
    // reject both shapes. This runs before anything, the verbose trace below
    // included, dereferences DataBufferArray.
    //
    if (Srb->NumberOfBuffers != 1 || Srb->CommandData.DataBufferArray == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM, "read carries %u stream headers; only 1 is supported\n",
                    Srb->NumberOfBuffers);
        Srb->Status = STATUS_INVALID_PARAMETER;
        StreamClassStreamNotification(ReadyForNextStreamDataRequest, Srb->StreamObject);
        StreamClassStreamNotification(StreamRequestComplete, Srb->StreamObject, Srb);
        return;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_STREAM, "read: %u-byte buffer, stream frame %u\n",
                Srb->CommandData.DataBufferArray->FrameExtent, Stream->FrameNumber);

    //
    // Tell the class driver it may send the next read regardless of how this one is
    // handled, so several reads can be attached at once (up to the descriptor pool).
    //
    StreamClassStreamNotification(ReadyForNextStreamDataRequest, Srb->StreamObject);

    Srb->TimeoutCounter = 0; // do not time out a parked/attached read; restored on completion

    //
    // The park under PendingLock is the single place this read's fate is
    // decided; every condition that forbids parking is observed in the same
    // lock hold. The Removed and StreamState checks pair with the teardowns:
    // surprise removal sets Removed, and the stop transition sets
    // KSSTATE_STOP, before their final drain takes this lock, so a read
    // either parks in time to be drained there or fails immediately -- no
    // read can slip through and wait forever on a camera that is gone or a
    // pin that is stopped. The stop check matters when a run transition fails
    // (for example the mode exceeds the bus-speed payload limit): the
    // client's capture thread resubmits the buffer the stop drain just
    // returned, and parking that read on a stopped pin would pend it forever.
    //
    // The Irp->Cancel check pairs with HwCancelPacket the same way: the class
    // driver cancels a read exactly once, and IoCancelIrp sets the bit before
    // the cancel routine can run, so a cancel that raced ahead of this park
    // (it found the read neither parked nor in the engine, and on a paused
    // pin the work item then resolves nothing) is observed here and the read
    // completes instead of parking forever. A cancel arriving after the park
    // finds Queued set and claims the read normally; the two claims cannot
    // both fire, they run under the same lock.
    //
    KeAcquireSpinLock(&Stream->PendingLock, &irql);
    if (extension->Removed) {
        KeReleaseSpinLock(&Stream->PendingLock, irql);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "device removed; failing read\n");
        PhotonicStreamCompleteRead(Stream, Srb, 0, STATUS_DEVICE_REMOVED);
        return;
    }
    if (Stream->StreamState == KSSTATE_STOP) {
        KeReleaseSpinLock(&Stream->PendingLock, irql);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "stream stopped; cancelling read\n");
        PhotonicStreamCompleteRead(Stream, Srb, 0, STATUS_CANCELLED);
        return;
    }
    if (Srb->Irp != NULL && Srb->Irp->Cancel) {
        KeReleaseSpinLock(&Stream->PendingLock, irql);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "read already cancelled; not parking it\n");
        PhotonicStreamCompleteRead(Stream, Srb, 0, STATUS_CANCELLED);
        return;
    }
    InsertTailList(&Stream->PendingReads, &ext->ListEntry);
    ext->Queued = TRUE;
    KeReleaseSpinLock(&Stream->PendingLock, irql);

    if (Stream->Capture != NULL) {
        PhotonicCapturePump(Stream->Capture);
    }
}

/// PhotonicStreamReceiveDataPacket -- per-stream data SRBs. For a capture (output) stream
/// these are SRB_READ_DATA requests asking the driver to fill a frame buffer, which
/// are pended and completed asynchronously (see PhotonicStreamReceiveReadData). Writes and
/// any other command are output-only-stream errors completed synchronously.
///
/// @param Srb  Data stream request block dispatched by the class driver.
VOID STREAMAPI PhotonicStreamReceiveDataPacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_STREAM_EXTENSION streamExt = (PPHOTONIC_STREAM_EXTENSION) Srb->StreamObject->HwStreamExtension;

    //
    // The gate keeps this whole handler inside the close's rundown: the close
    // waits for every entered thread to leave before it destroys the capture
    // engine, so everything below may use Stream->Capture freely. A request
    // refused here arrived after the close began; complete it cancelled
    // without touching the stream any further.
    //
    if (!PhotonicStreamEnterDataPath(streamExt)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "%s (0x%x) refused: the stream is closing\n",
                    PhotonicSrbCommandName(Srb->Command), Srb->Command);
        Srb->Status = STATUS_CANCELLED;
        StreamClassStreamNotification(ReadyForNextStreamDataRequest, Srb->StreamObject);
        StreamClassStreamNotification(StreamRequestComplete, Srb->StreamObject, Srb);
        return;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_STREAM, "%s (0x%x) frame %u\n", PhotonicSrbCommandName(Srb->Command),
                Srb->Command, streamExt->FrameNumber);

    if (Srb->Command == SRB_READ_DATA) {
        PhotonicStreamReceiveReadData(streamExt, Srb);
        PhotonicStreamLeaveDataPath(streamExt);
        return;
    }

    //
    // A capture stream is output-only; writes are not supported, and anything else
    // is unimplemented. Both complete synchronously.
    //
    Srb->Status = Srb->Command == SRB_WRITE_DATA ? STATUS_NOT_SUPPORTED : STATUS_NOT_IMPLEMENTED;

    StreamClassStreamNotification(ReadyForNextStreamDataRequest, Srb->StreamObject);
    StreamClassStreamNotification(StreamRequestComplete, Srb->StreamObject, Srb);
    PhotonicStreamLeaveDataPath(streamExt);
}
