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
/// Photonic IOCTL handlers: the video-family lifecycle verbs of the shared
/// capture slot (prepare / start / stop / unprepare). The video family drives
/// both halves of the capture session -- the camera transmit side and the host
/// receive side -- so a client using only these verbs gets a complete session;
/// the imager family (imager.c) drives the transmit half alone. All shared
/// machinery (the mapped DMA ring, transfer info, events, the common
/// prepare/start/stop/unprepare helpers) lives in frames.c. See ioctl/ioctl.c
/// for the dispatch mechanism.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and video.tmh (WPP-generated) must come after it. ioctl.h needs
// CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "video.tmh"
// clang-format on

/// PHOTONIC_IOCTL_PREPARE_VIDEO -- allocates the shared IOCTL capture slot
/// (the stream extension plus the capture engine's descriptor pool and
/// reusable IRPs). The IOCTL interface has no format negotiation: the slot
/// captures the camera's current selection (SET_PIXEL_FORMAT / SET_FRAME_RATE)
/// at its default full-frame geometry. The isochronous channel, bandwidth and
/// resource handle are acquired at start (PhotonicCaptureStart), exactly as
/// the DirectShow pin acquires them at KSSTATE_RUN. On an existing stopped
/// slot the prepare succeeds by releasing and rebuilding it (the interface's
/// re-prepare rule).
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlPrepareVideo(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_PREPARE_VIDEO_IN *in = Request->Buffer;
    ULONG frameCount = in->FrameCount;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (frameCount == 0 || frameCount > PHOTONIC_IOCTL_MAX_VIDEO_FRAMES) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "frame count %u out of range (1..%u)\n", frameCount,
                    PHOTONIC_IOCTL_MAX_VIDEO_FRAMES);
        Request->Error = PL_ERROR_INVALID_COUNT;
        return STATUS_INVALID_PARAMETER;
    }

    return PhotonicIoctlCapturePrepare(Request, frameCount, /*AllowReprepare*/ TRUE);
}

/// PHOTONIC_IOCTL_UNPREPARE_VIDEO -- releases everything the prepare (and any
/// start since) acquired and frees the slot; fails benignly when the slot is
/// already gone (the client's teardown interleaves both families).
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlUnprepareVideo(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    FuncEntry(TRACE_FLAG_IOCTL);

    return PhotonicIoctlCaptureUnprepareRequest(Request);
}

/// PHOTONIC_IOCTL_START_VIDEO -- starts the capture session through the common
/// engine: programs the camera for the prepared mode, acquires the isochronous
/// channel, bandwidth and resource handle, and attaches the mapped ring. The
/// input dword selects the transmission mode: non-zero (the DLL sends 1 when
/// streaming) enables continuous transmission (ISO_EN); zero selects
/// single-frame mode, where ISO_EN stays off and each externally triggered
/// acquisition is armed through PHOTONIC_IOCTL_SW_TRIGGER.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlStartVideo(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_START_VIDEO_IN *in = Request->Buffer;

    FuncEntry(TRACE_FLAG_IOCTL);

    return PhotonicIoctlCaptureStart(Request, in->Flags, /*PumpRing*/ TRUE);
}

/// PHOTONIC_IOCTL_STOP_VIDEO -- stops the capture session through the common
/// engine: disables camera transmission, cancels the attached buffers and
/// releases the isochronous resources (PhotonicCaptureStop). The slot stays
/// prepared, so a start can run the stream again without a re-prepare.
///
/// Protocol note: historically STOP_VIDEO left ISO_EN set and only the imager
/// stop cleared it; the shared stop here clears it on both. Benign for the
/// known client, which always issues STOP_VIDEO and STOP_IMAGER back-to-back.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlStopVideo(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    FuncEntry(TRACE_FLAG_IOCTL);

    return PhotonicIoctlCaptureStopRequest(Request);
}
