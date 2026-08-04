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
/// Photonic IOCTL handlers: frame rate enumeration / selection.
/// See ioctl/ioctl.c for the dispatch mechanism.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and framerate.tmh (WPP-generated) must come after it. ioctl.h
// needs CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "dcam.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "framerate.tmh"
// clang-format on

/// The compiler marks every translation unit that uses floating point with a
/// reference to the CRT symbol _fltused, which the driver must define itself
/// because it does not link a CRT. Only a marker: the sole floating-point use
/// (PhotonicIntervalToFps) is bracketed by KeSaveFloatingPointState.
int _fltused = 0;

/// Convert a frame interval in 100ns units to frames per second as an IEEE
/// float. Kernel floating point must be bracketed by a state save on x86
/// (a no-op that always succeeds on x64).
///
/// @param Interval  Frame interval in 100-nanosecond units.
/// @param Fps       Receives the frame rate in frames per second.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicIntervalToFps(_In_ ULONG Interval, _Out_ float *Fps) {
    KFLOATING_SAVE fpState;
    NTSTATUS status;

    //
    // A camera reporting a zero interval would turn into +INF frames per
    // second below; fail the query instead of returning nonsense.
    //
    if (Interval == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "zero frame interval reported by the camera\n");
        return STATUS_DEVICE_DATA_ERROR;
    }

    status = KeSaveFloatingPointState(&fpState);
    if (!NT_SUCCESS(status)) {
        //
        // Traced here so both callers' failure paths are visible at default
        // levels (the dispatcher's result trace is verbose only).
        //
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "KeSaveFloatingPointState failed: %!STATUS!\n", status);
        return status;
    }
    *Fps = 10000000.0f / (float) Interval;
    KeRestoreFloatingPointState(&fpState);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_ENUM_FRAME_RATE -- returns the Nth frame rate the camera
/// supports in its current mode as an IEEE float (frames per second). An index
/// past the last supported rate succeeds the IRP but reports
/// PL_ERROR_OUT_OF_RANGE in the buffer, which is how the DLL detects the end
/// of the enumeration.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlEnumFrameRate(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_ENUM_FRAME_RATE_IN *in = Request->Buffer;
    PHOTONIC_ENUM_FRAME_RATE_OUT *out = Request->Buffer;
    ULONG interval;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (Request->Extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "index %u\n", in->Index);

    status = PhotonicDcamEnumFrameRate(Request->Extension, in->Index, &interval);
    if (status == STATUS_NO_MORE_ENTRIES) {
        out->Status = PL_ERROR_OUT_OF_RANGE;
        Request->Information = sizeof(*out);
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_HARDWARE;
        return status;
    }

    status = PhotonicIntervalToFps(interval, &out->FrameRateFps);
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR;
        return status;
    }

    out->Status = PL_SUCCESS;
    Request->Information = sizeof(*out);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "index %u -> interval %u (100ns)\n", in->Index, interval);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_SET_FRAME_RATE -- selects the active frame rate by its
/// ENUM_FRAME_RATE enumeration index, writing the matching DCAM rate id to the
/// FRAME_RATE register; rejected while the capture stream is open.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlSetFrameRate(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_SET_FRAME_RATE_IN *in = Request->Buffer;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (Request->Extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "index %u\n", in->Index);

    //
    // Changing the camera's rate under an open capture session would
    // invalidate the isochronous configuration derived from it. Both
    // front-ends count: the DirectShow stream, and a prepared direct-buffer
    // slot whose session was validated against the prepared geometry.
    //
    if (Request->Extension->VideoStream != NULL || Request->Extension->IoctlCapture != NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "capture session active; rate change rejected\n");
        Request->Error = PL_ERROR_INVALID_STATE;
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = PhotonicDcamSetFrameRate(Request->Extension, in->Index);
    if (status == STATUS_NO_MORE_ENTRIES) {
        //
        // No rate at the requested index: a caller error, reported with the
        // code the driver uses for out-of-range values.
        //
        Request->Error = PL_ERROR_INVALID_COUNT;
        return STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_HARDWARE;
        return status;
    }

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_GET_FRAME_RATE -- returns the current frame rate as an IEEE
/// float (frames per second), read live from the camera.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlGetFrameRate(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_GET_FRAME_RATE_OUT *out = Request->Buffer;
    ULONG interval;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (Request->Extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    status = PhotonicDcamGetFrameRate(Request->Extension, &interval);
    if (!NT_SUCCESS(status)) {
        //
        // PL_ERROR_NO_PREVIEW doubles as the failed-frame-rate-query code
        // (see return_code.h).
        //
        Request->Error = PL_ERROR_NO_PREVIEW;
        return status;
    }

    status = PhotonicIntervalToFps(interval, &out->FrameRateFps);
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR;
        return status;
    }

    out->Status = PL_SUCCESS;

    //
    // The dispatch table enforces no input minimum for this code, so the
    // caller's input copy need not cover Reserved. Every reported output byte
    // must be written here, or the copy-back would return uninitialized pool
    // to user mode.
    //
    out->Reserved = 0;
    Request->Information = sizeof(*out);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "interval %u (100ns)\n", interval);
    return STATUS_SUCCESS;
}
