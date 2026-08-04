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
/// The camera's command mailbox (see csr.h for the CSR address): the host
/// block-writes a command packet to the fixed mailbox CSR and, when a response
/// is expected, block-reads it back from the same address.

#ifndef MAILBOX_H
#define MAILBOX_H

#include "fwdev.h"

/// Mailbox CSR write handler: latches the command packet (mailbox.c).
///
/// @param dev  Device the request arrived on.
/// @param req  The incoming write request.
/// @param ctx  The dcam_camera_t owning the mailbox region.
void dcam_on_mailbox_write(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx);

/// Mailbox CSR read handler: answers the latched command (mailbox.c).
///
/// @param dev  Device the request arrived on.
/// @param req  The incoming read request.
/// @param ctx  The dcam_camera_t owning the mailbox region.
void dcam_on_mailbox_read(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx);

#endif // MAILBOX_H
