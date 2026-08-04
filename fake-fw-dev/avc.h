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
/// AV/C (IEC 61883-1 FCP) protocol constants used by the FCP command handler.

#ifndef AVC_H
#define AVC_H

#include "fwdev.h"

/// AV/C response codes (first nibble of response[0]).
constexpr int AVC_RESP_NOT_IMPLEMENTED = 0x08;
constexpr int AVC_RESP_ACCEPTED = 0x09;
constexpr int AVC_RESP_STABLE = 0x0C;

/// FCP command-register write handler: answers AV/C commands (avc.c).
///
/// @param dev  Device the request arrived on.
/// @param req  The incoming write request carrying the AV/C frame.
/// @param ctx  Caller context (unused).
void dcam_on_fcp_command(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx);

#endif // AVC_H
