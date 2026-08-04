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
/// Photonic IOCTL handlers: sub-window (ROI) / pixel format / channel.
/// See ioctl/ioctl.c for the dispatch mechanism.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and format.tmh (WPP-generated) must come after it. ioctl.h needs
// CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "dcam.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "format.tmh"
// clang-format on

/// PHOTONIC_IOCTL_SUBWINDOW_GET -- returns the current ROI in full-resolution
/// sensor coordinates, derived on every call from the camera's live
/// VIDEO_FORMAT / VIDEO_MODE selection (PhotonicDcamGetCurrentSubwindow).
/// Nothing is cached: the DLL's set path programs the head through the
/// external-I2C gate (not SUBWINDOW_SET), issues INVALIDATE_FORMAT and
/// expects the next GET to reflect the camera's live state.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlSubwindowGet(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_SUBWINDOW_GET_OUT *out = Request->Buffer;
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;
    ULONG scale;
    ULONG offsetX;
    ULONG offsetY;
    ULONG width;
    ULONG height;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    status = PhotonicDcamGetCurrentSubwindow(extension, &scale, &offsetX, &offsetY, &width, &height);
    if (!NT_SUCCESS(status)) {
        //
        // Register access failed or the camera's selection matched no
        // enumerated mode -- either way the device command failed.
        //
        Request->Error = PL_ERROR_HARDWARE;
        return status;
    }

    out->Status = PL_SUCCESS;
    out->Scale = scale;
    out->Width = width;
    out->OffsetX = offsetX;
    out->OffsetY = offsetY;
    out->Height = height;
    Request->Information = sizeof(*out);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "scale=%u size=%ux%u offset=%u,%u\n", out->Scale, out->Width,
                out->Height, out->OffsetX, out->OffsetY);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_SUBWINDOW_SET -- validates the requested ROI. The driver does
/// not program the camera's ROI: only the default sub-window -- scale 1, zero
/// offsets and the default full-frame size of one of the enumerated modes --
/// is accepted; everything else is rejected with PL_ERROR_OUT_OF_RANGE.
/// Nothing is stored: SUBWINDOW_GET derives its answer from the camera's live
/// mode, so SET-then-GET consistency comes from the camera state, not from a
/// cache.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlSubwindowSet(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_SUBWINDOW_SET_IN *in = Request->Buffer;
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;
    BOOLEAN isDefault = FALSE;
    ULONG i;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "scale=%u size=%ux%u offset=%u,%u\n", in->Scale, in->Width,
                in->Height, in->OffsetX, in->OffsetY);

    if (in->Scale == 1 && in->OffsetX == 0 && in->OffsetY == 0) {
        for (i = 0; i < extension->ModeCount; i++) {
            if (in->Width == extension->Modes[i].DefaultWidth && in->Height == extension->Modes[i].DefaultHeight) {
                isDefault = TRUE;
                break;
            }
        }
    }

    if (!isDefault) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "not the default sub-window of any enumerated mode\n");
        Request->Error = PL_ERROR_OUT_OF_RANGE;
        return STATUS_INVALID_PARAMETER;
    }

    PhotonicIoctlEchoStatus(Request);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_GET_PIXEL_FORMAT -- returns the current DCAM pixel format code
/// (colour coding, PHOTONIC_DCAM_PIX_*), decoded from the camera's live
/// VIDEO_FORMAT / VIDEO_MODE selection.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlGetPixelFormat(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_GET_PIXEL_FORMAT_OUT *out = Request->Buffer;
    ULONG pixelFormat;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (Request->Extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    status = PhotonicDcamGetCurrentPixelFormat(Request->Extension, &pixelFormat);
    if (!NT_SUCCESS(status)) {
        //
        // Register access failed or the camera's selection matched no
        // enumerated mode -- either way the device command failed.
        //
        Request->Error = PL_ERROR_HARDWARE;
        return status;
    }

    out->PixelFormat = pixelFormat;
    Request->Information = sizeof(*out);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "pixel format %u\n", pixelFormat);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_SET_PIXEL_FORMAT -- selects the first enumerated mode matching
/// the requested DCAM pixel format code, preserving the current ROI.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlSetPixelFormat(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_SET_PIXEL_FORMAT_IN *in = Request->Buffer;
    ULONG pixelFormat = in->PixelFormat;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (Request->Extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "pixel format %u\n", pixelFormat);

    //
    // Changing the camera's format under an open capture session would
    // invalidate the negotiated mode and the isochronous configuration
    // derived from it. Both front-ends count: the DirectShow stream, and a
    // prepared direct-buffer slot whose mapped ring was validated against
    // the geometry the prepare resolved.
    //
    if (Request->Extension->VideoStream != NULL || Request->Extension->IoctlCapture != NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "capture session active; format change rejected\n");
        Request->Error = PL_ERROR_INVALID_STATE;
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = PhotonicDcamSelectPixelFormat(Request->Extension, pixelFormat);
    if (status == STATUS_NOT_FOUND) {
        //
        // No enumerated mode produces the requested coding: a caller error,
        // reported with the code the driver uses for out-of-range values.
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

/// PHOTONIC_IOCTL_IMAGE_FLIP -- forwards the two flip control words to extended
/// proprietary command 0xD.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlImageFlip(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_INVALIDATE_FORMAT -- drops any cached current format so the
/// next format-dependent operation re-reads it from the camera. This driver
/// caches no format: every format query
/// (PhotonicDcamGetCurrentMode / GetCurrentPixelFormat / GetFrameRate) reads
/// the live camera registers, so there is nothing to invalidate and the IOCTL
/// succeeds as a no-op. The DLL calls it after every operation that may change
/// the camera's format (the PlSetSubWindowSettings path), so it must succeed.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS always.
NTSTATUS PhotonicIoctlInvalidateCurrentFormat(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_GET_CHANNEL -- returns the isochronous channel number.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlGetChannel(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_GET_PACKET_SIZE -- returns the isochronous packet size in bytes.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlGetPacketSize(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}
