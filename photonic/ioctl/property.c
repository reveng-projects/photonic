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
/// Photonic IOCTL handlers: initialize / property (feature) get-set.
/// See ioctl/ioctl.c for the dispatch mechanism.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and property.tmh (WPP-generated) must come after it. ioctl.h
// needs CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "property.tmh"
// clang-format on

/// PHOTONIC_IOCTL_INITIALIZE -- resets the camera to a known state via the
/// DCAM power / initialize register.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlInitialize(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_PROPERTY_GET -- reads a DCAM feature (PHOTONIC_FEATURE_*)
/// and returns its control flags and value.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlPropertyGet(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}

/// PHOTONIC_IOCTL_PROPERTY_SET -- writes a DCAM feature value / control flags;
/// TEMPERATURE is read-only and rejected.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_NOT_IMPLEMENTED (stub).
NTSTATUS PhotonicIoctlPropertySet(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    UNREFERENCED_PARAMETER(Request);
    FuncEntry(TRACE_FLAG_IOCTL);
    return STATUS_NOT_IMPLEMENTED;
}
