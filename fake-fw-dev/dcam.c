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
/// Emulated 1394 DCAM/IIDC "Photonic" camera.  See dcam.h.  This file owns the
/// camera lifecycle and the DCAM CSR register dispatch; the functional pieces
/// live in per-topic modules: video.c (standard formats), format7.c, features.c,
/// i2c.c (camera-head registers), frame.c (raw frame generation), iso.c
/// (isochronous transmit), avc.c (FCP commands) and mailbox.c (command mailbox).
/// All mutable state lives in dcam_camera_t (dcam_internal.h).

#include "dcam.h"

#include "avc.h"
#include "csr.h"
#include "dcam_internal.h"
#include "features.h"
#include "format7.h"
#include "i2c.h"
#include "iso.h"
#include "log.h"
#include "mailbox.h"
#include "trigger.h"
#include "video.h"

#include <endian.h>
#include <stdlib.h>
#include <string.h>

#include <linux/firewire-constants.h>

/// Returns the value a Photonic camera would expose for each DCAM register.
/// Register values are big-endian on the wire, so send htobe32(logical_value).
///
/// @param dev  The device object the request arrived on.
/// @param req  The incoming read request.
/// @param ctx  The dcam_camera_t owning this CSR region.
static void dcam_on_register_read(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx) {
    dcam_camera_t *cam = ctx;
    uint32_t off = (uint32_t) (req->offset - cam->dcam_base);
    uint32_t logical = 0;
    int fidx;

    if (off == DCAM_REG_VIDEO_FORMAT_INQ) {
        logical = dcam_video_format_inq(cam); // VIDEO_FORMAT_INQ
    } else if (off >= DCAM_REG_VIDEO_MODE_INQ && off <= DCAM_REG_VIDEO_MODE_INQ + (DCAM_NUM_FORMATS - 1) * 4 &&
               (off & 3) == 0) {
        logical = dcam_video_mode_inq((off - DCAM_REG_VIDEO_MODE_INQ) / 4); // VIDEO_MODE_INQ_f
    } else if (off >= DCAM_REG_FRAME_RATE_INQ &&
               off <= DCAM_REG_FRAME_RATE_INQ + (DCAM_NUM_FORMATS * DCAM_NUM_MODES - 1) * 4 && (off & 3) == 0) {
        int combined = (int) ((off - DCAM_REG_FRAME_RATE_INQ) / 4); // mode + format*8
        int format = combined / DCAM_NUM_MODES;
        int mode = combined % DCAM_NUM_MODES;
        if (format >= 0 && format < DCAM_NUM_FORMATS) {
            logical = dcam_frame_rate_inq(format, mode); // FRAME_RATE_INQ
        }
    } else if (off == DCAM_REG_VIDEO_MODE_INQ_7) {
        // VIDEO_MODE_INQ_7: bitmask of exposed F7 modes (bit 31-mode).
        unsigned m;
        for (m = 0; m < cam->num_f7_modes; m++) {
            logical |= 0x80000000u >> m;
        }
    } else if (off >= DCAM_REG_V_CSR_INQ_7 && off < DCAM_REG_V_CSR_INQ_7 + cam->num_f7_modes * 4 && (off & 3) == 0) {
        // V_CSR_INQ_7_m: quadlet pointer to F7 mode m's CSR block.
        logical = dcam_f7_csr_pointer(cam, (int) ((off - DCAM_REG_V_CSR_INQ_7) / 4));
    } else if (off >= DCAM_F7_CSR_BLOCK && off < DCAM_F7_CSR_BLOCK + cam->num_f7_modes * DCAM_F7_CSR_BLOCK_LEN) {
        uint32_t rel = off - DCAM_F7_CSR_BLOCK;
        logical = dcam_f7_csr_read(cam, (int) (rel / DCAM_F7_CSR_BLOCK_LEN),
                                   rel % DCAM_F7_CSR_BLOCK_LEN); // F7 mode CSR
    } else if (off == DCAM_REG_FRAME_RATE) {
        logical = (uint32_t) cam->cur_rate << 29;
    } else if (off == DCAM_REG_VIDEO_MODE) {
        logical = (uint32_t) cam->cur_mode << 29;
    } else if (off == DCAM_REG_VIDEO_FORMAT) {
        logical = (uint32_t) cam->cur_format << 29;
    } else if (off == DCAM_REG_FEATURE_HI_INQ) {
        logical = dcam_feature_block_inq(DCAM_FEATURE_HI_BLOCK, cam->feat_present); // 0x500 block
    } else if (off == DCAM_REG_FEATURE_LO_INQ) {
        logical = dcam_feature_block_inq(DCAM_FEATURE_LO_BLOCK, cam->feat_present); // 0x580 block
    } else if ((fidx = dcam_feature_by_inq(off)) >= 0) {
        // Feature inquiry: present + auto/manual capable + the feature's range;
        // 0 (no presence bit) when disabled via PHOTONIC_FEATURES, like an
        // unimplemented register on the real camera.
        logical = (cam->feat_present & (1u << fidx)) != 0 ? dcam_feature_inq_word(fidx) : 0;
    } else if ((fidx = dcam_feature_by_value(off)) >= 0) {
        // Feature status/value: last value written by the host (0, no
        // presence bit, for a feature disabled via PHOTONIC_FEATURES).
        logical = cam->feat_value[fidx];
    } else {
        logical = 0; // BASIC_FUNC_INQ and other unmodeled registers
    }

    LOG(TRACE, "[DCAM  ] off=0x%03x -> 0x%08x", off, logical);

    uint32_t wire = htobe32(logical);
    fw_device_send_response(dev, req, RCODE_COMPLETE, &wire, sizeof(wire));
}

/// Register values are big-endian on the wire; swap the wire quadlet back to
/// recover the logical value.  The handler intercepts the registers that drive
/// streaming; all others are accepted and ignored.
///
/// @param dev  The device object the request arrived on.
/// @param req  The incoming write request.
/// @param ctx  The dcam_camera_t owning this CSR region.
static void dcam_on_register_write(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx) {
    dcam_camera_t *cam = ctx;
    uint32_t off = (uint32_t) (req->offset - cam->dcam_base);
    uint32_t logical = 0;
    int rcode = RCODE_COMPLETE;
    int fidx;

    if (req->length >= 4) {
        uint32_t wire;
        memcpy(&wire, (const void *) (uintptr_t) req->data, 4);
        logical = be32toh(wire);
    }

    LOG(TRACE, "[DCAM W] off=0x%03x <- 0x%08x", off, logical);

    switch (off) {
        case DCAM_REG_INITIALIZE:
            // bit31 = software reset: restore power-up video state.
            if (logical & 0x80000000U) {
                dcam_reset_video_state(cam);
                LOG(TRACE, "INITIALIZE: video state reset to power-up defaults");
            }
            break;
        case DCAM_REG_VIDEO_FORMAT:
            cam->cur_format = (int) (logical >> 29);
            LOG(TRACE, "VIDEO_FORMAT: format=%d", cam->cur_format);
            break;
        case DCAM_REG_VIDEO_MODE:
            cam->cur_mode = (dcam_video_mode_t) (logical >> 29);
            LOG(TRACE, "VIDEO_MODE: mode=%d", cam->cur_mode);
            break;
        case DCAM_REG_FRAME_RATE:
            cam->cur_rate = (dcam_frame_rate_t) (logical >> 29);
            LOG(TRACE, "FRAME_RATE: rate=%d", cam->cur_rate);
            break;
        case DCAM_REG_ISOCH_CHANNEL: {
            // logical = (channel<<4 | speedBits) << 24
            int channel = (logical >> 28) & 0x0F;
            int speed = (logical >> 24) & 0x0F; // 0=100,1=200,2=400 Mbps (== SCODE)
            if (cam->iso_created && (channel != cam->iso_channel || speed != cam->iso_speed)) {
                // The transmit context fixes channel/speed at creation and a
                // fw-cdev context cannot be destroyed without closing the fd.
                LOG(WARN,
                    "ISOCH_CHANNEL: channel/speed changed to %d/%d but iso "
                    "context is fixed at %d/%d",
                    channel, speed, cam->iso_channel, cam->iso_speed);
            } else {
                cam->iso_channel = channel;
                cam->iso_speed = speed;
            }
            LOG(TRACE, "ISOCH_CHANNEL: channel=%d  speedBits=%d", cam->iso_channel, cam->iso_speed);
            break;
        }
        case DCAM_REG_ISO_EN:
            if (logical & 0x80000000U) {
                cam->iso_oneshot = 0;
                dcam_iso_start(cam);
            } else {
                dcam_iso_stop(cam);
            }
            break;
        case DCAM_REG_ONE_SHOT:
            // One-shot (bit31): transmit exactly one frame and stop.  Tear down any
            // prior (idle) shot first so each ONE_SHOT produces a fresh frame, then
            // start a single-frame transmit that dcam_on_iso_interrupt stops once
            // the frame has gone out.
            if (logical & 0x80000000U) {
                dcam_iso_stop(cam);
                cam->iso_oneshot = 1;
                dcam_iso_start(cam);
            } else if (logical != 0) {
                LOG(WARN,
                    "[dcam] ONE_SHOT (0x61C) written 0x%08x without bit31; "
                    "ignored",
                    logical);
            }
            break;
        default:
            // Format 7 mode CSR block: latch the programmed geometry / coding.  An
            // illegal BYTE_PER_PACKET is NAKed (RCODE_DATA_ERROR) rather than latched.
            if (off >= DCAM_F7_CSR_BLOCK && off < DCAM_F7_CSR_BLOCK + cam->num_f7_modes * DCAM_F7_CSR_BLOCK_LEN) {
                uint32_t rel = off - DCAM_F7_CSR_BLOCK;
                rcode =
                    dcam_f7_csr_write(cam, (int) (rel / DCAM_F7_CSR_BLOCK_LEN), rel % DCAM_F7_CSR_BLOCK_LEN, logical);
                break;
            }
            // Feature value register: clamp the value into the feature's range
            // (like the real camera) and latch it so the following read returns
            // it.  A feature disabled via PHOTONIC_FEATURES rejects the write,
            // like the real camera rejects writes to unimplemented feature
            // registers.
            fidx = dcam_feature_by_value(off);
            if (fidx >= 0) {
                if ((cam->feat_present & (1u << fidx)) == 0) {
                    rcode = RCODE_DATA_ERROR;
                    LOG(TRACE, "FEATURE %s: disabled; write rejected", dcam_features[fidx].name);
                } else {
                    cam->feat_value[fidx] = dcam_feature_clamp_word(fidx, logical);
                    LOG(TRACE, "FEATURE %s: value=%u (requested %u)  mode=%s", dcam_features[fidx].name,
                        cam->feat_value[fidx] & 0xfff, logical & 0xfff, (logical & 0x1000000U) ? "auto" : "manual");
                }
            }
            break;
    }

    fw_device_send_response(dev, req, rcode, NULL, 0);
}

dcam_camera_t *dcam_create(fw_device_t *dev, const dcam_iso_config_t *iso_cfg) {
    dcam_camera_t *cam = calloc(1, sizeof(*cam));
    if (cam == NULL) {
        return NULL;
    }
    cam->dev = dev;
    // Standard format/mode/rate and Format 7 per-mode geometry/coding defaults.
    dcam_reset_video_state(cam);
    // Camera-head register file power-up defaults.  The head controller is a
    // separate device on the camera's I2C bus, so it is deliberately NOT reset
    // by the DCAM INITIALIZE software reset — like real head electronics it
    // keeps its settings until power-cycled (i.e. until this process restarts).
    dcam_i2c_reset(cam);
    // External hardware-trigger input (serial port, PHOTONIC_TRIGGER_PORT).
    // An unusable port only disables triggered mode; creation continues.
    dcam_trigger_start(cam);
    // Feature controls: all implemented unless PHOTONIC_FEATURES narrows the
    // set.  An implemented feature powers up at its default value word; a
    // disabled one reads 0 (no presence bit), like the real camera's
    // unimplemented registers.
    cam->feat_present = dcam_feature_parse_present(getenv("PHOTONIC_FEATURES"));
    for (int i = 0; i < DCAM_NUM_FEATURES; i++) {
        cam->feat_value[i] = (cam->feat_present & (1u << i)) != 0 ? dcam_feature_default_word(i) : 0;
        if ((cam->feat_present & (1u << i)) == 0) {
            LOG(INFO, "feature %s: disabled (PHOTONIC_FEATURES)", dcam_features[i].name);
        }
    }
    cam->iso_fd = -1;
    cam->iso_handle = -1;
    cam->iso_speed = 2; // default 400 Mbps
    // Default mode set = the real camera: no standard formats, one F7 mode.
    cam->advertise_std_formats = 0;
    cam->num_f7_modes = 1;
    if (iso_cfg != NULL) {
        cam->iso_tag = iso_cfg->tag;
        cam->iso_sy_all = iso_cfg->sy_all;
        cam->advertise_std_formats = iso_cfg->advertise_std_formats;
        if (iso_cfg->num_f7_modes >= 1 && iso_cfg->num_f7_modes <= DCAM_NUM_F7_MODES) {
            cam->num_f7_modes = (unsigned) iso_cfg->num_f7_modes;
        } else if (iso_cfg->num_f7_modes > DCAM_NUM_F7_MODES) {
            cam->num_f7_modes = DCAM_NUM_F7_MODES;
        }
    }
    LOG(INFO, "advertised modes: standard Formats 0/1/2 %s, %u Format 7 mode(s)",
        cam->advertise_std_formats ? "on" : "off", cam->num_f7_modes);

    // Allocate the DCAM CSR register region.  Must happen before
    // fw_device_publish() so the unit-dependent directory encodes the real
    // CsrBase.  The kernel picks a free slot in the search range.
    cam->dcam_base = fw_device_allocate(dev, DCAM_SEARCH_START, DCAM_SEARCH_END, DCAM_ADDR_LENGTH,
                                        dcam_on_register_read, dcam_on_register_write, cam);
    if (cam->dcam_base == 0) {
        LOG(ERROR, "DCAM CSR allocation failed; DCAM reads will error.");
        dcam_trigger_stop(cam);
        free(cam);
        return NULL;
    }
    cam->csr_base = (uint32_t) cam->dcam_base;
    LOG(INFO, "DCAM CSR region allocated at 0x%012llx  CsrBase=0x%08x", (unsigned long long) cam->dcam_base,
        cam->csr_base);
    fw_device_set_csr_base(dev, cam->csr_base);

    // Allocate the fixed FCP command region (AV/C commands land here).
    if (fw_device_allocate(dev, FCP_COMMAND_ADDR, FCP_COMMAND_ADDR + FCP_REGION_LENGTH, FCP_REGION_LENGTH, NULL,
                           dcam_on_fcp_command, cam) == 0) {
        LOG(WARN, "FCP command allocation failed (continuing).");
    } else {
        LOG(INFO, "FCP command region allocated at 0x%012llx", (unsigned long long) FCP_COMMAND_ADDR);
    }

    // Allocate the fixed command mailbox CSR (write-command / read-response
    // interface used for head-register and ext-I2C access; see mailbox.c).
    cam->mailbox_base = fw_device_allocate(dev, DCAM_MAILBOX_ADDR, DCAM_MAILBOX_ADDR + DCAM_MAILBOX_LENGTH,
                                           DCAM_MAILBOX_LENGTH, dcam_on_mailbox_read, dcam_on_mailbox_write, cam);
    if (cam->mailbox_base == 0) {
        LOG(WARN, "mailbox CSR allocation failed (continuing).");
    } else {
        LOG(INFO, "mailbox CSR region allocated at 0x%012llx", (unsigned long long) cam->mailbox_base);
    }

    fw_device_set_iso_interrupt_handler(dev, dcam_on_iso_interrupt, cam);
    fw_device_set_iso_error_handler(dev, dcam_on_iso_error, cam);
    return cam;
}

void dcam_destroy(dcam_camera_t *cam) {
    if (cam == NULL) {
        return;
    }
    // Tears down the iso context (STOP_ISO, unmap buffer, close iso fd).
    dcam_iso_stop(cam);
    dcam_trigger_stop(cam);
    free(cam);
}

void dcam_force_stream(dcam_camera_t *cam, uint32_t bytes_per_packet) {
    // Select Format 7 mode 0, exactly as the VIDEO_FORMAT / VIDEO_MODE register
    // writes would.  The F7 power-up geometry (full frame, MONO8, packet unit)
    // is already in place from dcam_reset_video_state().
    cam->cur_format = DCAM_F7_FORMAT;
    cam->cur_mode = (dcam_video_mode_t) 0;

    // Program the packet size through the same CSR write path a host uses so
    // an illegal value is rejected instead of latched.
    if (bytes_per_packet != 0) {
        uint32_t logical = bytes_per_packet << 16;
        if (dcam_f7_csr_write(cam, 0, DCAM_F7_BYTE_PER_PACKET, logical) != RCODE_COMPLETE) {
            LOG(ERROR, "PHOTONIC_STREAM: BYTE_PER_PACKET %u rejected; not streaming", bytes_per_packet);
            return;
        }
    }

    cam->iso_oneshot = 0;
    dcam_iso_start(cam);
    if (cam->iso_running) {
        LOG(INFO, "PHOTONIC_STREAM: self-started F7/M0 streaming (no controlling host)");
    } else {
        LOG(ERROR, "PHOTONIC_STREAM: streaming failed to start");
    }
}
