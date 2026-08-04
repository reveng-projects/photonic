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
/// AV/C FCP command handler.
///
/// AV/C frame byte 0 is the ctype (command) or response code:
///   0x00 = CONTROL  -> 0x09 ACCEPTED
///   0x01 = STATUS   -> 0x0C STABLE
///   others          -> 0x08 NOT_IMPLEMENTED
///
/// For PLUG INFO STATUS (opcode 0x31) the camera reports 1 isochronous output
/// plug (video) and 0 input plugs, satisfying the host's plug enumeration.

#include "avc.h"

#include "log.h"

#include <string.h>

#include <linux/firewire-constants.h>

void dcam_on_fcp_command(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx) {
    const uint8_t *cmd = (const uint8_t *) (uintptr_t) req->data;
    uint32_t len = req->length;
    (void) ctx;

    LOG(TRACE, "[FCP cmd ] len=%u  ctype=0x%02x  subunit=0x%02x  opcode=0x%02x", len, len > 0 ? cmd[0] : 0,
        len > 1 ? cmd[1] : 0, len > 2 ? cmd[2] : 0);

    // Release the kernel-side pending request first (mandatory).
    fw_device_send_response(dev, req, RCODE_COMPLETE, NULL, 0);

    if (len < 3) {
        return;
    }

    uint8_t ctype = cmd[0];
    uint8_t opcode = cmd[2];

    // Build response: mirror the command, replace byte 0 with response code.
    uint8_t resp[512];
    uint32_t resp_len = len < sizeof(resp) ? len : sizeof(resp);
    memcpy(resp, cmd, resp_len);

    if (ctype == 0x00) {
        resp[0] = AVC_RESP_ACCEPTED; // CONTROL
    } else if (ctype == 0x01) {
        resp[0] = AVC_RESP_STABLE; // STATUS
    } else {
        resp[0] = AVC_RESP_NOT_IMPLEMENTED;
    }

    // PLUG INFO STATUS (0x31): report 1 isoch-output plug, 0 input plugs.
    if (ctype == 0x01 && opcode == 0x31 && resp_len >= 8) {
        resp[3] = 0x00; // serial bus subfunction
        resp[4] = 0x01; // 1 isochronous output plug
        resp[5] = 0x00; // 0 isochronous input plugs
        resp[6] = 0x00; // 0 external output plugs
        resp[7] = 0x00; // 0 external input plugs
    }

    // SUBUNIT INFO (0x30): this device uses DCAM, not AV/C subunits.  Return
    // NOT IMPLEMENTED so the host does not create phantom AV/C subunit nodes.
    if (opcode == 0x30) {
        resp[0] = AVC_RESP_NOT_IMPLEMENTED;
    }

    fw_device_send_fcp_response(dev, (uint16_t) req->source_node_id, req->generation, resp, resp_len);
}
