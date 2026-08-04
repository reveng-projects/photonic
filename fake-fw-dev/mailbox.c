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
/// Command mailbox CSR (write-command / read-response register pair).
///
/// The host block-writes a command packet to the fixed mailbox CSR and, when a
/// response is expected, block-reads it back from the same address.  On the
/// wire each 32-bit word is big-endian; de-swap on write and re-swap on read to
/// work in logical (little-endian) byte order.  Transfer lengths are always
/// DWORD-aligned.
///
/// A real camera interprets each command; the fake only needs the transactions
/// to COMPLETE (and to satisfy a couple of response-field checks) so host-side
/// initialization sequences proceed.

#include "mailbox.h"

#include "dcam_internal.h"
#include "i2c.h"
#include "log.h"

#include "../common/camera_regs.h"

#include <string.h>

#include <linux/firewire-constants.h>

/// Byte-swaps each 32-bit word in `src` into `dst`.  Any tail bytes not forming
/// a complete word are copied verbatim.
///
/// @param dst  Destination buffer.
/// @param src  Source buffer.
/// @param len  Number of bytes to process.
static void dcam_swap_dwords(uint8_t *dst, const uint8_t *src, uint32_t len) {
    uint32_t i;
    for (i = 0; i + 4 <= len; i += 4) {
        dst[i + 0] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i + 0];
    }
    for (; i < len; i++) { // DWORD-aligned in practice; copy any tail as-is
        dst[i] = src[i];
    }
}

void dcam_on_mailbox_write(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx) {
    dcam_camera_t *cam = ctx;
    uint32_t len = req->length;

    if (len > sizeof(cam->mailbox_cmd)) {
        len = sizeof(cam->mailbox_cmd);
    }
    dcam_swap_dwords(cam->mailbox_cmd, (const uint8_t *) (uintptr_t) req->data, len);
    cam->mailbox_cmd_len = len;

    LOG(TRACE, "[MBOX W] cmd=0x%04x  p1=0x%02x p2=0x%02x  len=%u",
        len >= 2 ? (cam->mailbox_cmd[0] | cam->mailbox_cmd[1] << 8) : 0, len > 4 ? cam->mailbox_cmd[4] : 0,
        len > 5 ? cam->mailbox_cmd[5] : 0, len);

    // Command 0x1009: write one register of an I2C device on the camera head.
    // Packet layout (logical order): [0..3]=0x1009, [4]=I2C device address,
    // [5]=register address, [6]=1, [7]=0, [8]=value.  Only the head controller
    // at CAMREG_I2C_DEV_ADDR is modelled; writes to other addresses complete
    // without effect (the transaction still succeeds, as on a bus with no
    // responder check at this layer).
    if (len >= 9) {
        uint32_t cmd = (uint32_t) (cam->mailbox_cmd[0] | cam->mailbox_cmd[1] << 8 | cam->mailbox_cmd[2] << 16 |
                                   cam->mailbox_cmd[3] << 24);
        if (cmd == 0x1009 && cam->mailbox_cmd[4] == CAMREG_I2C_DEV_ADDR) {
            dcam_i2c_write_reg(cam, cam->mailbox_cmd[5], cam->mailbox_cmd[8]);
        }
    }

    fw_device_send_response(dev, req, RCODE_COMPLETE, NULL, 0);
}

void dcam_on_mailbox_read(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx) {
    dcam_camera_t *cam = ctx;
    uint8_t resp[512];
    uint8_t wire[512];
    uint32_t outlen = req->length;
    uint32_t cmd;

    if (outlen > sizeof(resp)) {
        outlen = sizeof(resp);
    }
    memset(resp, 0, outlen);

    cmd = cam->mailbox_cmd_len >= 4 ? (uint32_t) (cam->mailbox_cmd[0] | cam->mailbox_cmd[1] << 8 |
                                                  cam->mailbox_cmd[2] << 16 | cam->mailbox_cmd[3] << 24)
                                    : 0;

    switch (cmd) {
        case 0x1008:
            // Read one register of an I2C device on the camera head: command
            // bytes [4]=I2C device address, [5]=register address; the register
            // value is returned in response byte [8].  The head controller at
            // CAMREG_I2C_DEV_ADDR answers from its register file (STATUS is
            // synthesised live); other device addresses report 1, so unmodelled
            // scale factors read as the identity value.
            if (outlen > 8 && cam->mailbox_cmd_len > 5) {
                if (cam->mailbox_cmd[4] == CAMREG_I2C_DEV_ADDR) {
                    uint32_t reg = cam->mailbox_cmd[5];
                    resp[8] = dcam_i2c_read_reg(cam, reg);
                    LOG(TRACE, "[i2c 0x%02x] reg 0x%02x (%s) -> 0x%02x", CAMREG_I2C_DEV_ADDR, reg,
                        dcam_i2c_reg_name(reg) != NULL ? dcam_i2c_reg_name(reg) : "?", resp[8]);
                } else {
                    resp[8] = 1;
                }
            }
            break;
        case 0x100d:
            // Ext-I2C read: response byte [5] must echo the requested length
            // (command byte [4]); the data rides at byte [8].  The fake has no
            // external I2C peripheral, so echo the length and return zero data.
            if (outlen > 5 && cam->mailbox_cmd_len > 4) {
                resp[5] = cam->mailbox_cmd[4];
            }
            break;
        default:
            // 0x1009 and others: success with a zeroed response is sufficient.
            break;
    }

    dcam_swap_dwords(wire, resp, outlen);
    LOG(TRACE, "[MBOX R] cmd=0x%04x  outlen=%u", cmd, outlen);
    fw_device_send_response(dev, req, RCODE_COMPLETE, wire, outlen);
}
