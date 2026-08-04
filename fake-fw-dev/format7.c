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
/// DCAM Format 7 (scalable / partial-image) support: the mode table, the
/// derived packet-size envelope and the per-mode CSR register read/write
/// handlers.  See format7.h for the register map and envelope rules.

#include "format7.h"

#include "dcam_internal.h"
#include "log.h"

#include <linux/firewire-constants.h>

/// F7 modes.  Mode 0 is the real "Photonic" camera from the capture trace; by
/// default (PHOTONIC_F7_MODES=1, PHOTONIC_STD_FORMATS=0) it is the only mode
/// exposed and the fake advertises exactly what the real camera does.  Modes 1..3
/// are the fake's own larger mode set, exposed only when PHOTONIC_F7_MODES is
/// raised, so existing multi-mode testing still works.
static const dcam_f7_mode_info_t dcam_f7_modes[DCAM_NUM_F7_MODES] = {
    // mode 0: real camera — full frame 1388x1032, 4x4 unit, greyscale only
    // (MONO8/MONO16).  MAX_IMAGE_SIZE reports the raw sensor 1575x1053 (hosts
    // align it down onto the 4x4 unit grid) and the default IMAGE_SIZE is
    // 1388x1032, matching the trace: 1388x1032 Y8 and Y16 at 7.75..15.5 fps.
    {1575, 1053, 4, 4, 1388, 1032, DCAM_F7_COLOR_INQ_MONO},
    // mode 1: full frame 1280x960, 8x8 unit.  No YUV422: at this size part of
    // the YUV422 rate range would exceed the S400 payload limit (format7.h).
    {1280, 960, 8, 8, 1280, 960, DCAM_F7_COLOR_INQ_NO_YUV422},
    // mode 2: half frame 640x480 (e.g. 2x2 binned), 8x8 unit
    {640, 480, 8, 8, 640, 480, DCAM_F7_COLOR_INQ_ALL},
    // mode 3: small ROI 320x240, 4x4 unit
    {320, 240, 4, 4, 320, 240, DCAM_F7_COLOR_INQ_ALL},
};

uint32_t dcam_f7_bits_per_pixel(uint32_t coding) {
    switch (coding) {
        case 0:
            return 8; // MONO8
        case 1:
            return 12; // YUV411
        case 2:
        case 5:
            return 16; // YUV422 / MONO16
        case 3:
        case 4:
            return 24; // YUV444 / RGB8
        case 6:
            return 48; // RGB16
        case 7:
        case 8:
            return 8; // RAW8 (Bayer RGGB / GRBG)
        default:
            return 0;
    }
}

/// Returns the frame size in bytes for an F7 mode's currently programmed geometry.
///
/// @param cam   Camera whose f7[mode] state to use.
/// @param mode  F7 mode index.
/// @return      Frame size in bytes.
static uint32_t dcam_f7_frame_bytes(const dcam_camera_t *cam, int mode) {
    uint32_t bits = dcam_f7_bits_per_pixel(cam->f7[mode].color_coding);
    return (uint32_t) (((uint64_t) cam->f7[mode].width * cam->f7[mode].height * bits) >> 3);
}

/// Derives the F7 PACKET_PARA_INQ bytes-per-packet bounds for the mode's currently
/// programmed geometry/coding.  *unit_out is UNIT_BYTE_PER_PACKET (the packet-size
/// step / smallest packet, i.e. the slowest advertised rate); *max_out is
/// MAX_BYTE_PER_PACKET (the largest packet, i.e. the fastest rate).
///
/// Like a real DCAM camera, the unit is the bytes in one image line
/// (width * bits_per_pixel / 8), quadlet-aligned up as the 1394 iso path requires,
/// and the max is the largest whole-line multiple of that unit that still fits the
/// S400 isochronous payload (DCAM_F7_BUS_MAX_BPP).  BYTE_PER_PACKET must be an
/// integer multiple of the unit within [unit .. max]; dcam_f7_bpp_is_legal()
/// enforces this and the write handler NAKs anything else.  For 1388-wide MONO8
/// this yields unit=1388, max=2776 (PACKET_PARA_INQ = 0x056c0ad8); for MONO16
/// unit=2776, max=2776 (0x0ad80ad8) — exactly the trace values.
///
/// @param cam       Camera whose f7[mode] state to use.
/// @param mode      F7 mode index.
/// @param unit_out  Receives UNIT_BYTE_PER_PACKET (smallest legal packet size).
/// @param max_out   Receives MAX_BYTE_PER_PACKET (largest legal packet size).
static void dcam_f7_packet_params(const dcam_camera_t *cam, int mode, uint32_t *unit_out, uint32_t *max_out) {
    const struct dcam_f7_live *live = &cam->f7[mode];
    uint32_t bits = dcam_f7_bits_per_pixel(live->color_coding);
    uint32_t unit = (uint32_t) (((uint64_t) live->width * bits + 7) >> 3); // bytes/line
    uint32_t max;

    unit = (unit + 3u) & ~3u; // real cameras report a quadlet-aligned unit
    if (unit == 0) {
        unit = 4;
    }
    max = (DCAM_F7_BUS_MAX_BPP / unit) * unit; // largest whole-line multiple that fits
    if (max < unit) {
        max = unit;
    }
    *unit_out = unit;
    *max_out = max;
}

/// Returns non-zero if `bpp` is a legal BYTE_PER_PACKET for the mode: a multiple
/// of the unit within [unit .. max].
///
/// @param cam   Camera whose f7[mode] state to use.
/// @param mode  F7 mode index.
/// @param bpp   Candidate bytes-per-packet value.
/// @return      Non-zero if legal, zero otherwise.
static int dcam_f7_bpp_is_legal(const dcam_camera_t *cam, int mode, uint32_t bpp) {
    uint32_t unit, max;
    dcam_f7_packet_params(cam, mode, &unit, &max);
    return bpp >= unit && bpp <= max && (bpp % unit) == 0;
}

/// Returns the power-on / recommended BYTE_PER_PACKET: the unit (slowest rate),
/// matching the trace's initial read of reg 0x44 (0x056c0000 for 1388-wide MONO8).
///
/// @param cam   Camera whose f7[mode] state to use.
/// @param mode  F7 mode index.
/// @return      Recommended bytes-per-packet value.
static uint32_t dcam_f7_recommended_bpp(const dcam_camera_t *cam, int mode) {
    uint32_t unit, max;
    dcam_f7_packet_params(cam, mode, &unit, &max);
    return unit;
}

/// Returns the PACKET_PER_FRAME_INQ (reg 0x48): number of isochronous packets the
/// camera sends per frame.  Uses ceil() so a frame whose byte count is not an
/// exact multiple of the packet size still occupies a whole number of packets, as
/// real DCAM hardware reports.  Hosts size receive buffers as
/// PACKET_PER_FRAME_INQ * BYTE_PER_PACKET, so this must divide the frame by the
/// same bytes-per-packet that reg 0x44 returns (the recommended/unit bpp) or the
/// product overshoots the true frame size.
///
/// @param cam   Camera whose f7[mode] state to use.
/// @param mode  F7 mode index.
/// @return      Packets per frame.
static uint32_t dcam_f7_packets_per_frame(const dcam_camera_t *cam, int mode) {
    uint32_t frame = dcam_f7_frame_bytes(cam, mode);
    uint32_t bpp = dcam_f7_recommended_bpp(cam, mode);
    if (bpp == 0) {
        return 0;
    }
    return (frame + bpp - 1) / bpp;
}

uint32_t dcam_f7_csr_pointer(const dcam_camera_t *cam, int mode) {
    uint32_t block = DCAM_F7_CSR_BLOCK + (uint32_t) mode * DCAM_F7_CSR_BLOCK_LEN;
    return (cam->csr_base - 0xf0000000u + block) / 4;
}

uint32_t dcam_f7_csr_read(const dcam_camera_t *cam, int mode, uint32_t reg) {
    const dcam_f7_mode_info_t *mi = &dcam_f7_modes[mode];
    switch (reg) {
        case DCAM_F7_MAX_IMAGE_SIZE:
            return (mi->max_width << 16) | mi->max_height;
        case DCAM_F7_UNIT_SIZE:
            return (mi->unit_width << 16) | mi->unit_height;
        case DCAM_F7_IMAGE_POSITION:
            return (cam->f7[mode].offx << 16) | (cam->f7[mode].offy & 0xffff);
        case DCAM_F7_IMAGE_SIZE:
            return (cam->f7[mode].width << 16) | (cam->f7[mode].height & 0xffff);
        case DCAM_F7_COLOR_CODING_ID:
            return cam->f7[mode].color_coding << 24;
        case DCAM_F7_COLOR_CODING_INQ:
            return mi->color_coding_inq;
        case DCAM_F7_PACKET_PARA_INQ: {
            // high = unit step (slowest rate), low = max packet (fastest rate);
            // both derived from the current geometry/coding so the legal packet
            // sizes are the whole-line multiples in [unit .. max].
            uint32_t unit, max;
            dcam_f7_packet_params(cam, mode, &unit, &max);
            return (unit << 16) | max;
        }
        case DCAM_F7_BYTE_PER_PACKET:
            // power-on / recommended bytes-per-packet (the unit) in the high word
            return dcam_f7_recommended_bpp(cam, mode) << 16;
        case DCAM_F7_PIXEL_NUMBER_INQ:
            return (uint32_t) cam->f7[mode].width * cam->f7[mode].height;
        case DCAM_F7_TOTAL_BYTES_HI:
            return 0; // frame byte count always fits in 32 bits
        case DCAM_F7_TOTAL_BYTES_LO:
            return dcam_f7_frame_bytes(cam, mode);
        case DCAM_F7_PACKET_PER_FRAME_INQ:
            return dcam_f7_packets_per_frame(cam, mode);
        default:
            return 0;
    }
}

int dcam_f7_csr_write(dcam_camera_t *cam, int mode, uint32_t reg, uint32_t logical) {
    switch (reg) {
        case DCAM_F7_IMAGE_POSITION:
            cam->f7[mode].offx = logical >> 16;
            cam->f7[mode].offy = logical & 0xffff;
            LOG(TRACE, "F7[%d] IMAGE_POSITION: offX=%u offY=%u", mode, cam->f7[mode].offx, cam->f7[mode].offy);
            break;
        case DCAM_F7_IMAGE_SIZE:
            cam->f7[mode].width = logical >> 16;
            cam->f7[mode].height = logical & 0xffff;
            // The packet unit (bytes per image line) depends on width, so re-default
            // bytes-per-packet to the new unit.  Otherwise a stale value from the
            // previous geometry would make the iso transmit split the frame into the
            // wrong number of packets and the receiving host would never assemble a
            // complete frame (a later legal BYTE_PER_PACKET write overrides this).
            cam->f7[mode].bytes_per_packet = dcam_f7_recommended_bpp(cam, mode);
            LOG(TRACE, "F7[%d] IMAGE_SIZE: %ux%u  (bytes/packet -> %u)", mode, cam->f7[mode].width,
                cam->f7[mode].height, cam->f7[mode].bytes_per_packet);
            break;
        case DCAM_F7_COLOR_CODING_ID:
            cam->f7[mode].color_coding = logical >> 24;
            // The packet unit also depends on bits-per-pixel, so re-default
            // bytes-per-packet when the coding changes (e.g. MONO8 unit 1388 ->
            // MONO16 unit 2776 for a 1388-wide image).
            cam->f7[mode].bytes_per_packet = dcam_f7_recommended_bpp(cam, mode);
            LOG(TRACE, "F7[%d] COLOR_CODING_ID: %u  (bytes/packet -> %u)", mode, cam->f7[mode].color_coding,
                cam->f7[mode].bytes_per_packet);
            break;
        case DCAM_F7_BYTE_PER_PACKET: {
            uint32_t bpp = logical >> 16;
            // A real DCAM camera only accepts a BYTE_PER_PACKET that is an integer
            // multiple of UNIT_BYTE_PER_PACKET within [unit .. max] (see
            // dcam_f7_packet_params); it NAKs anything else with a transaction
            // data-error.  Reject the write (rather than latching it) to match.
            if (!dcam_f7_bpp_is_legal(cam, mode, bpp)) {
                uint32_t unit, max;
                dcam_f7_packet_params(cam, mode, &unit, &max);
                LOG(WARN,
                    "F7[%d] BYTE_PER_PACKET %u is not a multiple of unit %u in "
                    "[%u..%u]; NAK (data error)",
                    mode, bpp, unit, unit, max);
                return RCODE_DATA_ERROR;
            }
            cam->f7[mode].bytes_per_packet = bpp;
            LOG(TRACE, "F7[%d] BYTE_PER_PACKET: %u", mode, cam->f7[mode].bytes_per_packet);
            break;
        }
        default:
            // read-only inquiry register or unmodeled F7 register: ignore
            break;
    }
    return RCODE_COMPLETE;
}

/// Resets Format 7 per-mode state to power-up defaults (host reprograms these
/// via each mode's CSR block).  Default size/coding come from the mode table;
/// the streaming bytes-per-packet defaults to the packet unit (slowest rate)
/// for that size.
///
/// @param cam  Camera whose F7 live state to reset.
void dcam_f7_reset(dcam_camera_t *cam) {
    int m;

    for (m = 0; m < DCAM_NUM_F7_MODES; m++) {
        const dcam_f7_mode_info_t *mi = &dcam_f7_modes[m];
        cam->f7[m].width = mi->def_width;
        cam->f7[m].height = mi->def_height;
        cam->f7[m].offx = 0;
        cam->f7[m].offy = 0;
        cam->f7[m].color_coding = DCAM_F7_CODING_MONO8;
        cam->f7[m].bytes_per_packet = dcam_f7_recommended_bpp(cam, m);
    }
}
