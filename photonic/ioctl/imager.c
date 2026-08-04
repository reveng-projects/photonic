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
/// Photonic IOCTL handlers: the imager-family lifecycle verbs of the shared
/// capture slot (prepare / start / stop / unprepare). Despite the name, the
/// imager family is not a still-capture mode: it is the control surface over
/// the camera/transmit side of the one capture session -- isochronous
/// bandwidth, the channel, and whether the camera transmits continuously
/// (ISO_EN) -- while the video family (video.c) drives both that and the host
/// receive side. Both families operate on the same slot
/// (Extension->IoctlCapture) through the shared helpers in frames.c; there is
/// no per-family ownership, and either family's stop / unprepare acts on the
/// whole session. See ioctl/ioctl.c for the dispatch mechanism and
/// docs/ioctl-interface-design.md for the session model.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and imager.tmh (WPP-generated) must come after it. ioctl.h needs
// CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "imager.tmh"
// clang-format on

/// PHOTONIC_IOCTL_PREPARE_IMAGER -- allocates the shared IOCTL capture slot,
/// resolving the camera's current format selection exactly as PREPARE_VIDEO
/// does (the frame size a later MAP_VIDEO_FRAME must match is the same
/// whichever family prepared the slot). The 8-byte input is not read. Unlike
/// the video prepare there is no re-prepare path: any existing slot fails the
/// call with PL_ERROR_INVALID_STATE.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlPrepareImager(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    FuncEntry(TRACE_FLAG_IOCTL);

    return PhotonicIoctlCapturePrepare(Request, /*FrameCount*/ 0, /*AllowReprepare*/ FALSE);
}

/// PHOTONIC_IOCTL_UNPREPARE_IMAGER -- releases the whole session (stop, unmap,
/// engine destroy) and frees the slot, whichever family prepared it; fails
/// benignly when the slot is already gone. The client's teardown interleaves
/// both families -- after UNPREPARE_VIDEO has freed the slot, this trailing
/// call fails with STATUS_UNSUCCESSFUL and the DLL ignores the status.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlUnprepareImager(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    FuncEntry(TRACE_FLAG_IOCTL);

    return PhotonicIoctlCaptureUnprepareRequest(Request);
}

/// PHOTONIC_IOCTL_START_IMAGER -- starts the transmit half only: programs the
/// camera and acquires the isochronous resources through the common engine,
/// but pumps no ring. If frames are mapped they stay unattached until a video
/// start pumps them; with nothing mapped the deferred listen is never issued,
/// so a continuous start (input dword non-zero -- the DLL always sends 1)
/// produces a free-running camera nobody listens to. A zero input dword
/// selects single-frame mode, exactly as in START_VIDEO.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlStartImager(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_START_IMAGER_IN *in = Request->Buffer;

    FuncEntry(TRACE_FLAG_IOCTL);

    return PhotonicIoctlCaptureStart(Request, in->Mode, /*PumpRing*/ FALSE);
}

/// PHOTONIC_IOCTL_STOP_IMAGER -- full stop of both halves of the session:
/// camera transmission disabled (ISO_EN cleared, an armed one-shot cancelled),
/// attached buffers cancelled and drained, isochronous resources released.
/// The slot stays prepared, so a start can run again without a re-prepare;
/// fails benignly when no slot exists.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlStopImager(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    FuncEntry(TRACE_FLAG_IOCTL);

    return PhotonicIoctlCaptureStopRequest(Request);
}
