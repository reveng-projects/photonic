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
/// Per-stream control SRBs for the Photonic stream minidriver: stream state
/// changes, the format and rate stubs, master clock and flush handling, all
/// dispatched from PhotonicStreamReceiveControlPacket.

// clang-format off
// control.tmh (WPP-generated) must come last.
#include "photonic.h"
#include "capture.h"
#include "properties.h"
#include "control.tmh"
// clang-format on

/// SRB_SET_STREAM_STATE -- Stop / Acquire / Pause / Run. Capture is brought up on
/// the transition to KSSTATE_RUN (the camera starts streaming and frames begin
/// DMAing directly into the reads' buffers) and torn down on KSSTATE_STOP. Acquire
/// and Pause only record the new state.
///
/// @param Srb  Stream request block for SRB_SET_STREAM_STATE.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicStreamControlSetState(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PPHOTONIC_STREAM_EXTENSION streamExt = (PPHOTONIC_STREAM_EXTENSION) Srb->StreamObject->HwStreamExtension;
    KSSTATE newState = Srb->CommandData.StreamState;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;

    FuncEntry(TRACE_FLAG_STREAM);

    //
    // State changes are control transitions: the mutex serializes them against
    // the IOCTL handlers, the cleanup hook, the control work item and the SRB
    // teardown paths, so the engine is started and stopped by exactly one
    // context at a time. Stream control SRBs run at PASSIVE_LEVEL, so the
    // wait is legal.
    //
    KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);

    if (newState == KSSTATE_RUN) {
        //
        // A run over a settled engine starts a new capture session, and
        // PictureNumber counts frames within one session, so the counter
        // restarts here. A Pause to Run resume finds the engine already
        // streaming and keeps its numbering. Draining is stable under the
        // mutex.
        //
        if (streamExt->Capture == NULL || streamExt->Capture->Draining) {
            streamExt->FrameNumber = 0;
        }

        //
        // The class driver always issues reads on a running capture pin, but
        // they arrive asynchronously around this very transition, so the
        // start may find none armed yet and then defers the camera enable to
        // the first attach rather than transmitting with nobody listening.
        //
        status = PhotonicCaptureStart(extension, streamExt, /*ExpectBuffers*/ TRUE);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM, "capture start failed: %!STATUS!\n", status);
            KeReleaseMutex(&extension->InterfaceMutex, FALSE);
            return status;
        }
    } else if (newState == KSSTATE_STOP) {
        //
        // PhotonicCaptureStop publishes KSSTATE_STOP under PendingLock before
        // draining, so a read racing this transition either parks in time to
        // be drained or is rejected in PhotonicStreamReceiveReadData.
        //
        PhotonicCaptureStop(extension, streamExt);
    }

    //
    // The stop and start above already published their states under
    // PendingLock (KSSTATE_STOP on every stop path, KSSTATE_RUN on every
    // start success path); the remaining transitions publish here, under the
    // same lock the read and cancel paths use to observe StreamState.
    //
    if (newState != KSSTATE_STOP && newState != KSSTATE_RUN) {
        KeAcquireSpinLock(&streamExt->PendingLock, &irql);
        streamExt->StreamState = newState;
        KeReleaseSpinLock(&streamExt->PendingLock, irql);
    }
    KeReleaseMutex(&extension->InterfaceMutex, FALSE);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM, "stream state -> %d\n", newState);
    return status;
}

/// SRB_GET_STREAM_STATE -- report the stream's current KS state.
///
/// @param Srb  Stream request block for SRB_GET_STREAM_STATE.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicStreamControlGetState(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_STREAM_EXTENSION streamExt = (PPHOTONIC_STREAM_EXTENSION) Srb->StreamObject->HwStreamExtension;

    FuncEntry(TRACE_FLAG_STREAM);

    Srb->CommandData.StreamState = streamExt->StreamState;
    Srb->ActualBytesTransferred = sizeof(KSSTATE);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM, "stream state == %d\n", streamExt->StreamState);
    return STATUS_SUCCESS;
}

/// SRB_OPEN_MASTER_CLOCK -- the graph nominates a master clock for the stream.
/// The driver stamps no clock-based presentation times and runs no hardware
/// clock, so there is nothing to attach to; acknowledging is the correct
/// behaviour, not a stub.
///
/// @param Srb  Stream request block for SRB_OPEN_MASTER_CLOCK.
/// @return STATUS_SUCCESS always.
static NTSTATUS PhotonicStreamControlOpenMasterClock(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    FuncEntry(TRACE_FLAG_STREAM);

    UNREFERENCED_PARAMETER(Srb);
    return STATUS_SUCCESS;
}

/// SRB_INDICATE_MASTER_CLOCK -- the master clock handle is being supplied. The
/// driver does not consume it (see PhotonicStreamControlOpenMasterClock); acknowledge.
///
/// @param Srb  Stream request block for SRB_INDICATE_MASTER_CLOCK.
/// @return STATUS_SUCCESS always.
static NTSTATUS PhotonicStreamControlIndicateMasterClock(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    FuncEntry(TRACE_FLAG_STREAM);

    UNREFERENCED_PARAMETER(Srb);
    return STATUS_SUCCESS;
}

/// SRB_CLOSE_MASTER_CLOCK -- the master clock is being released. Nothing to tear
/// down (see PhotonicStreamControlOpenMasterClock); acknowledge.
///
/// @param Srb  Stream request block for SRB_CLOSE_MASTER_CLOCK.
/// @return STATUS_SUCCESS always.
static NTSTATUS PhotonicStreamControlCloseMasterClock(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    FuncEntry(TRACE_FLAG_STREAM);

    UNREFERENCED_PARAMETER(Srb);
    return STATUS_SUCCESS;
}

/// SRB_BEGIN_FLUSH -- discard any queued data. Parked reads awaiting a frame are
/// completed with zero bytes; reads already attached to the bus stay with the
/// capture engine and complete when their frame or teardown arrives.
///
/// @param Srb  Stream request block for SRB_BEGIN_FLUSH.
/// @return STATUS_SUCCESS always.
static NTSTATUS PhotonicStreamControlBeginFlush(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_STREAM_EXTENSION streamExt = (PPHOTONIC_STREAM_EXTENSION) Srb->StreamObject->HwStreamExtension;

    FuncEntry(TRACE_FLAG_STREAM);

    PhotonicStreamCompletePendingReads(streamExt);
    return STATUS_SUCCESS;
}

/// SRB_END_FLUSH -- resume normal operation after a flush. Nothing was held back
/// (see PhotonicStreamControlBeginFlush); acknowledge.
///
/// @param Srb  Stream request block for SRB_END_FLUSH.
/// @return STATUS_SUCCESS always.
static NTSTATUS PhotonicStreamControlEndFlush(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    FuncEntry(TRACE_FLAG_STREAM);

    UNREFERENCED_PARAMETER(Srb);
    return STATUS_SUCCESS;
}

/// The shared not-implemented reply: SRB_UNKNOWN_STREAM_COMMAND, any
/// unrecognised command, and the format and rate commands the dispatch switch
/// routes here (see its case labels for each command's rationale).
///
/// @param Srb  Stream request block for the unhandled command.
/// @return STATUS_NOT_IMPLEMENTED always.
static NTSTATUS PhotonicStreamControlUnknownCommand(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    FuncEntry(TRACE_FLAG_STREAM);

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "%s (0x%x) not implemented\n",
                PhotonicSrbCommandName(Srb->Command), Srb->Command);
    return STATUS_NOT_IMPLEMENTED;
}

/// PhotonicStreamReceiveControlPacket -- per-stream control SRBs (state changes,
/// format and property get/set). Installed in the stream object by
/// PhotonicStreamOpen. Each implemented command is handled by its own function
/// above; the format and rate commands share the not-implemented reply.
///
/// @param Srb  Control stream request block dispatched by the class driver.
VOID STREAMAPI PhotonicStreamReceiveControlPacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM, "%s (0x%x)\n", PhotonicSrbCommandName(Srb->Command),
                Srb->Command);

    switch (Srb->Command) {
        case SRB_SET_STREAM_STATE:
            Srb->Status = PhotonicStreamControlSetState(Srb);
            break;

        case SRB_GET_STREAM_STATE:
            Srb->Status = PhotonicStreamControlGetState(Srb);
            break;

        case SRB_GET_STREAM_PROPERTY:
            Srb->Status = PhotonicGetStreamProperty(Srb);
            break;

        case SRB_SET_STREAM_PROPERTY:
            Srb->Status = PhotonicSetStreamProperty(Srb);
            break;

        case SRB_OPEN_MASTER_CLOCK:
            Srb->Status = PhotonicStreamControlOpenMasterClock(Srb);
            break;

        case SRB_INDICATE_MASTER_CLOCK:
            Srb->Status = PhotonicStreamControlIndicateMasterClock(Srb);
            break;

        case SRB_CLOSE_MASTER_CLOCK:
            Srb->Status = PhotonicStreamControlCloseMasterClock(Srb);
            break;

        case SRB_BEGIN_FLUSH:
            Srb->Status = PhotonicStreamControlBeginFlush(Srb);
            break;

        case SRB_END_FLUSH:
            Srb->Status = PhotonicStreamControlEndFlush(Srb);
            break;

        //
        // The format and rate commands share the not-implemented reply:
        // PROPOSE_DATA_FORMAT because validating a proposal against the DCAM
        // mode table is not implemented yet (report that rather than blindly
        // accepting every format), SET_DATA_FORMAT because the driver cannot
        // switch the format of an open stream (do not pretend it succeeded),
        // GET_DATA_FORMAT because no current format object is kept to hand
        // back, and the two rate commands because the driver streams at the
        // rate negotiated at connection and cannot change it on an open
        // stream (fail rather than silently ignore the new rate).
        //
        case SRB_PROPOSE_DATA_FORMAT:
        case SRB_SET_DATA_FORMAT:
        case SRB_GET_DATA_FORMAT:
        case SRB_SET_STREAM_RATE:
        case SRB_PROPOSE_STREAM_RATE:
        case SRB_UNKNOWN_STREAM_COMMAND:
        default:
            Srb->Status = PhotonicStreamControlUnknownCommand(Srb);
            break;
    }

    StreamClassStreamNotification(ReadyForNextStreamControlRequest, Srb->StreamObject);
    StreamClassStreamNotification(StreamRequestComplete, Srb->StreamObject, Srb);
}
