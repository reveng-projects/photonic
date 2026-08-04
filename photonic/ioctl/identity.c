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
/// Photonic IOCTL handlers: identity / version / device naming.
/// See ioctl/ioctl.c for the dispatch mechanism.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and identity.tmh (WPP-generated) must come after it. ioctl.h
// needs CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "identity.tmh"
// clang-format on

/// PHOTONIC_IOCTL_SDK_VERSION_1 / _2 -- SDK version queries. Both codes share
/// this routine and report not-supported.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlSdkVersion(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_SERIAL_NUMBER -- returns the byte-swapped low or high 32-bit
/// word of the camera serial number, selected by PHOTONIC_SERIAL_NUMBER_IN.Index.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlSerialNumber(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_SET_CANCEL_TIMEOUT -- stores the request cancel timeout
/// (milliseconds, converted to 100-ns units) in the device extension.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlSetCancelTimeout(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_GET_VENDOR_NAME -- copies the vendor-name string from the
/// camera's TEXTUAL_LEAF (header stripped, no NUL terminator).
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlGetVendorName(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_GET_MODEL_NAME -- copies the model-name string from the
/// camera's TEXTUAL_LEAF (header stripped, no NUL terminator).
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlGetModelName(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_GET_DCAM_VERSION -- returns the unit directory's
/// unit_sw_version, the implemented DCAM specification revision (not a CSR
/// address).
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlGetDcamVersion(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_GET_NAMES_LENGTH -- returns the byte lengths of the vendor
/// and model name strings so the caller can size GET_VENDOR_NAME /
/// GET_MODEL_NAME.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlGetNamesLength(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_GET_LAST_ERROR -- returns the PL_RETURN_CODE of the last
/// failed operation (forced to PL_ERROR_DEVICE_NOT_FOUND on surprise removal).
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlGetLastError(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_GET_LAST_ERROR_OUT *out = Request->Buffer;
    PPHOTONIC_DEVICE_EXTENSION extension = Request->Extension;

    FuncEntry(TRACE_FLAG_IOCTL);

    //
    // A surprise-removed device reports device-removed regardless of what
    // failed before.
    //
    if (extension->Removed) {
        extension->LastError = PL_ERROR_DEVICE_NOT_FOUND;
    }

    out->LastError = extension->LastError;
    Request->Information = sizeof(*out);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "last error %u\n", (ULONG) out->LastError);
    return STATUS_SUCCESS;
}
