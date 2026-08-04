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
/// Photonic IOCTL handlers: trigger / strobe.
/// See ioctl/ioctl.c for the dispatch mechanism.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and trigger.tmh (WPP-generated) must come after it. ioctl.h
// needs CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "dcam.h"
#include "capture.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "trigger.tmh"
// clang-format on

/// PHOTONIC_IOCTL_TRIGGER_SET -- programs the DCAM TRIGGER_MODE register and
/// forwards polarity, delay and the extra parameter to extended commands 9-11.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlTriggerSet(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_STROBE_SET -- assembles the strobe-control packet and
/// forwards it to extended command 12.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlStrobeSet(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_SW_TRIGGER -- arm one externally triggered acquisition by
/// writing the DCAM ONE_SHOT register. The write captures nothing by itself:
/// it instructs the camera to wait for its external trigger input (wired to a
/// serial port's DTR line and pulsed by user space, out of band), capture one
/// frame when the pulse arrives, transmit it isochronously and stop. The
/// interval between arm and frame is unbounded -- the driver imposes no
/// timeout; completion timeouts belong to the client.
///
/// The only state checks are that the capture slot exists and that the caller
/// owns it: the real client fires this against a video-prepared, video-started
/// slot (single-frame snaps do not use the imager family at all); the register
/// write is forwarded in any slot state. An arm with no frames mapped simply
/// loses the frame on the wire; sequencing -- one arm per completed frame --
/// is the client's responsibility, and re-arming before the previous shot
/// triggered replaces it in the camera.
///
/// The arm is also the hand-back point of the ring's ownership model
/// (PHOTONIC_IOCTL_FRAME_STATE): in single-frame mode a delivered slot stays
/// with the caller, detached from the bus, and the client arms the next shot
/// only after consuming the previous frame -- so every Delivered slot is
/// requeued and attached here, before the camera can transmit into it.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlSwTrigger(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;
    PPHOTONIC_IOCTL_CAPTURE capture = extension->IoctlCapture;
    PPHOTONIC_CAPTURE engine;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    if (capture == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "no prepared capture slot\n");
        Request->Error = PL_ERROR_NOT_PREPARED;
        return STATUS_UNSUCCESSFUL;
    }

    if (!PhotonicIoctlCaptureCheckOwner(Request, "trigger arm")) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Hand the caller's consumed slots back to the engine and attach them
    // before the arm, so the shot has a buffer to land in. Skipped while the
    // stream is not running: the pump would reject the attach, and the start
    // requeues the ring itself.
    //
    if (capture->Stream.StreamState == KSSTATE_RUN) {
        PhotonicIoctlFramesRequeue(capture);
    }

    //
    // Once the camera is armed, the trigger pulse -- and the frame -- can
    // arrive at any moment, so the host end must be listening before the arm.
    // The dispatcher holds the interface mutex and this IOCTL runs at
    // PASSIVE_LEVEL, so the listen can be issued right here once the first
    // ring attach has completed. On every shot after the first the engine is
    // already listening and this returns immediately.
    //
    engine = capture->Stream.Capture;
    if (engine != NULL) {
        PhotonicCaptureEnsureListen(engine);
    }

    status = PhotonicDcamOneShot(extension, TRUE);
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_HARDWARE;
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "armed one externally triggered acquisition\n");

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}
