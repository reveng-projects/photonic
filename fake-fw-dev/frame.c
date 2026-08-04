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
/// Raw frame generation: renders the animated test pattern for the current
/// pixel format and stamps the frame-verification metadata header.

#include "frame.h"

#include "dcam_internal.h"
#include "features.h"
#include "i2c.h"
#include "log.h"
#include "video.h"

#include "../common/camera_regs.h"
#include "../common/frame_meta.h"

#include <string.h>

/// Clamp an int to a byte.
///
/// @param v  Value to clamp.
/// @return   The value clamped to [0, 255].
static uint8_t clamp_u8(int v) {
    return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
}

/// Stamp the frame-verification header (common/frame_meta.h) over the first
/// bytes of a rendered frame: the virtual camera's state plus an FNV-1a checksum
/// of the whole frame.  The test program reads it back at the receiving end to
/// prove the frame arrived intact, in order, and from index 0.  Overwriting the
/// top-left pixels is harmless: the header bytes are non-zero, so the test's
/// non-black heuristic still passes.
/// The metadata feature array and dcam_features[] must describe the same set in
/// the same order, so the test can name each value (common/frame_meta.h).
_Static_assert(DCAM_NUM_FEATURES == FRAME_META_NUM_FEATURES,
               "frame_meta feature list out of sync with dcam_features[]");

/// @param cam       Camera whose state to embed.
/// @param dst       Frame buffer whose first bytes receive the header.
/// @param frame_no  Zero-based index of this frame.
static void embed_frame_meta(dcam_camera_t *cam, uint8_t *dst, unsigned frame_no) {
    frame_meta_t meta;
    int i;

    // Only the leading iso_bpp*iso_ppf bytes of the ring slot are transmitted;
    // for Format 7 that is less than the full cam->frame_bytes slot when the
    // image size is not a multiple of the packet size. Describe and checksum
    // the transmitted frame, not the slot, so the metadata matches exactly
    // what arrives at the far end.
    unsigned transmit_bytes = cam->iso_bpp * cam->iso_ppf;

    if (transmit_bytes < sizeof(meta)) {
        LOG(WARN, "[meta] frame is %u bytes, too small for the %zu-byte header; skipping", transmit_bytes,
            sizeof(meta));
        return;
    }

    memset(&meta, 0, sizeof(meta));
    meta.magic = FRAME_META_MAGIC;
    meta.version = FRAME_META_VERSION;
    meta.frame_index = frame_no;
    meta.frame_bytes = transmit_bytes;
    meta.width = cam->img_width;
    meta.height = cam->img_height;
    meta.pixfmt = (uint32_t) cam->pixfmt;
    meta.format = (uint32_t) cam->cur_format;
    meta.mode = (uint32_t) cam->cur_mode;
    meta.rate = (uint32_t) cam->cur_rate;

    // Live camera-control state: the value-register word for every feature the
    // fake backs (brightness, contrast, exposure, ...), so the test sees the
    // current settings travel with each frame.
    meta.feature_count = FRAME_META_NUM_FEATURES;
    for (i = 0; i < DCAM_NUM_FEATURES; i++) {
        meta.features[i] = cam->feat_value[i];
    }

    // Live camera-head register file (gain, exposure, binning, readout window,
    // trigger mode, ...), with the STATUS register synthesised at stamp time,
    // so the test can verify head register writes took effect.
    meta.i2c_reg_count = CAMREG_COUNT;
    memcpy(meta.i2c_regs, cam->i2c_regs, sizeof(meta.i2c_regs));
    meta.i2c_regs[CAMREG_STATUS] = dcam_i2c_status(cam);

    meta.checksum = 0;

    // Lay the header down (checksum field zero), then checksum the transmitted
    // frame and patch the checksum field in place.
    memcpy(dst, &meta, sizeof(meta));
    meta.checksum = frame_meta_checksum(dst, transmit_bytes);
    memcpy(dst + FRAME_META_CHECKSUM_OFFSET, &meta.checksum, sizeof(meta.checksum));

    if (frame_no == 0) {
        LOG(DEBUG,
            "[meta] stamped frame 0: %ux%u pixfmt=%d F%d/M%d/R%d %u bytes "
            "brightness=%u contrast=%u checksum=0x%08x",
            cam->img_width, cam->img_height, cam->pixfmt, cam->cur_format, cam->cur_mode, cam->cur_rate, transmit_bytes,
            FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_BRIGHTNESS]),
            FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_CONTRAST]), meta.checksum);
    }
}

void dcam_fill_frame_image(dcam_camera_t *cam, uint8_t *dst, unsigned frame_no) {
    unsigned w = cam->img_width;
    unsigned h = cam->img_height;
    unsigned bar = w ? (frame_no * 8) % w : 0;
    unsigned x, y;

    if (w == 0 || h == 0) {
        unsigned i;
        for (i = 0; i < cam->frame_bytes; i++) {
            dst[i] = (uint8_t) (i + frame_no * 4);
        }
        embed_frame_meta(cam, dst, frame_no);
        return;
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t lum = (uint8_t) (x + y + frame_no * 4);
            int onbar = ((x - bar) < 16u);
            uint8_t r, g, b;

            if (onbar) {
                lum = 0xFF;
                r = g = b = 0xFF; // white sweeping bar
            } else {
                b = (uint8_t) x; // horizontal blue ramp
                g = (uint8_t) y; // vertical green ramp
                r = lum;         // diagonal red gradient
            }

            switch (cam->pixfmt) {
                case DCAM_PIX_YUV422: { // UYVY: [U Y V Y]; BT.601 from (R,G,B)
                    int Y = (77 * r + 150 * g + 29 * b) >> 8;
                    int U = 128 + ((-43 * r - 85 * g + 128 * b) >> 8);
                    int V = 128 + ((128 * r - 107 * g - 21 * b) >> 8);
                    uint8_t *p = dst + (size_t) (y * w + x) * 2;
                    p[0] = (x & 1) ? clamp_u8(V) : clamp_u8(U); // V on odd, U on even
                    p[1] = clamp_u8(Y);
                    break;
                }
                case DCAM_PIX_YUV411: { // U Y0 Y1 V Y2 Y3: 6 bytes per 4 pixels; BT.601
                    int Y = (77 * r + 150 * g + 29 * b) >> 8;
                    int U = 128 + ((-43 * r - 85 * g + 128 * b) >> 8);
                    int V = 128 + ((128 * r - 107 * g - 21 * b) >> 8);
                    unsigned k = x & 3;
                    uint8_t *p = dst + (size_t) y * (w * 3 / 2) + (size_t) (x / 4) * 6;
                    p[k + 1 + (k >> 1)] = clamp_u8(Y); // Y slots 1,2,4,5
                    if (k == 0) {
                        p[0] = clamp_u8(U); // chroma from the group's first pixel
                    } else if (k == 2) {
                        p[3] = clamp_u8(V); // ... and its third, as UYVY samples V
                    }
                    break;
                }
                case DCAM_PIX_YUV444:
                case DCAM_PIX_RGB24: { // RGB24, byte order B,G,R
                    uint8_t *p = dst + (size_t) (y * w + x) * 3;
                    p[0] = b;
                    p[1] = g;
                    p[2] = r;
                    break;
                }
                case DCAM_PIX_MONO16: { // 16-bit grey, little-endian (lum replicated)
                    uint8_t *p = dst + (size_t) (y * w + x) * 2;
                    p[0] = lum;
                    p[1] = lum;
                    break;
                }
                case DCAM_PIX_RAW8: { // Bayer RGGB mosaic: one colour sample per site
                    uint8_t sample;
                    if ((y & 1) == 0) {
                        sample = (x & 1) == 0 ? r : g; // even row: R G R G
                    } else {
                        sample = (x & 1) == 0 ? g : b; // odd row:  G B G B
                    }
                    dst[(size_t) y * w + x] = sample;
                    break;
                }
                default: // DCAM_PIX_MONO8 (and fallback): 8-bit grey
                    dst[(size_t) y * w + x] = lum;
                    break;
            }
        }
    }

    // Stamp the verification header last, over the finished pixel data, so the
    // checksum it carries covers the frame exactly as it is transmitted.
    embed_frame_meta(cam, dst, frame_no);
}
