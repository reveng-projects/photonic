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
/// Standard IIDC/DCAM video formats: the fixed geometry/packetisation table
/// behind the VIDEO_FORMAT_INQ / VIDEO_MODE_INQ / FRAME_RATE_INQ register
/// families hosts read to enumerate Formats 0/1/2.

#include "video.h"

#include "dcam_internal.h"
#include "format7.h"

/// Frame geometry and per-rate packetisation, indexed [format][mode].  The
/// (resolution, colour coding) per slot follows the mode table in video.h.
static const dcam_mode_info_t dcam_modes[DCAM_NUM_FORMATS][DCAM_NUM_MODES] = {
    {
        // Format 0
        [DCAM_MODE_0] = {DCAM_PIX_YUV444, 160, 120, {{0, 0}, {0, 0}, {60, 960}, {120, 480}, {240, 240}, {0, 0}}},
        [DCAM_MODE_1] = {DCAM_PIX_YUV422, 320, 240, {{0, 0}, {80, 1920}, {160, 960}, {320, 480}, {640, 240}, {0, 0}}},
        [DCAM_MODE_2] = {DCAM_PIX_YUV411, 640, 480, {{0, 0}, {240, 1920}, {480, 960}, {960, 480}, {1920, 240}, {0, 0}}},
        [DCAM_MODE_3] =
            {DCAM_PIX_YUV422, 640, 480, {{0, 0}, {320, 1920}, {640, 960}, {1280, 480}, {2560, 240}, {0, 0}}},
        [DCAM_MODE_4] = {DCAM_PIX_RGB24, 640, 480, {{0, 0}, {480, 1920}, {960, 960}, {1920, 480}, {3840, 240}, {0, 0}}},
        [DCAM_MODE_5] =
            {DCAM_PIX_MONO8, 640, 480, {{0, 0}, {160, 1920}, {320, 960}, {640, 480}, {1280, 240}, {2560, 120}}},
        [DCAM_MODE_6] =
            {DCAM_PIX_MONO16, 640, 480, {{0, 0}, {320, 1920}, {640, 960}, {1280, 480}, {2560, 240}, {0, 0}}},
        [DCAM_MODE_7] = {DCAM_PIX_INVALID, 0, 0, {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
    },
    {
        // Format 1
        [DCAM_MODE_0] =
            {DCAM_PIX_YUV422, 800, 600, {{0, 0}, {500, 1920}, {1000, 960}, {2000, 480}, {4000, 240}, {0, 0}}},
        [DCAM_MODE_1] = {DCAM_PIX_RGB24, 800, 600, {{0, 0}, {0, 0}, {1500, 960}, {3000, 480}, {0, 0}, {0, 0}}},
        [DCAM_MODE_2] = {DCAM_PIX_MONO8, 800, 600, {{0, 0}, {0, 0}, {500, 960}, {1000, 480}, {2000, 240}, {4000, 120}}},
        [DCAM_MODE_3] =
            {DCAM_PIX_YUV422, 1024, 768, {{384, 4096}, {768, 2048}, {1536, 1024}, {3072, 512}, {0, 0}, {0, 0}}},
        [DCAM_MODE_4] = {DCAM_PIX_RGB24, 1024, 768, {{576, 4096}, {1152, 2048}, {2304, 1024}, {0, 0}, {0, 0}, {0, 0}}},
        [DCAM_MODE_5] =
            {DCAM_PIX_MONO8, 1024, 768, {{192, 4096}, {384, 2048}, {768, 1024}, {1536, 512}, {3072, 256}, {0, 0}}},
        [DCAM_MODE_6] =
            {DCAM_PIX_MONO16, 800, 600, {{0, 0}, {500, 1920}, {1000, 960}, {2000, 480}, {4000, 240}, {0, 0}}},
        [DCAM_MODE_7] =
            {DCAM_PIX_MONO16, 1024, 768, {{384, 4096}, {768, 2048}, {1536, 1024}, {3072, 512}, {0, 0}, {0, 0}}},
    },
    {
        // Format 2
        [DCAM_MODE_0] = {DCAM_PIX_YUV422, 1280, 960, {{640, 3840}, {1280, 1920}, {2560, 960}, {0, 0}, {0, 0}, {0, 0}}},
        [DCAM_MODE_1] = {DCAM_PIX_RGB24, 1280, 960, {{960, 3840}, {1920, 1920}, {3840, 960}, {0, 0}, {0, 0}, {0, 0}}},
        [DCAM_MODE_2] =
            {DCAM_PIX_MONO8, 1280, 960, {{320, 3840}, {640, 1920}, {1280, 960}, {2560, 480}, {0, 0}, {0, 0}}},
        [DCAM_MODE_3] =
            {DCAM_PIX_YUV422, 1600, 1200, {{1000, 3840}, {2000, 1920}, {4000, 960}, {0, 0}, {0, 0}, {0, 0}}},
        [DCAM_MODE_4] = {DCAM_PIX_RGB24, 1600, 1200, {{1500, 3840}, {3000, 1920}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
        [DCAM_MODE_5] =
            {DCAM_PIX_MONO8, 1600, 1200, {{500, 3840}, {1000, 1920}, {2000, 960}, {4000, 480}, {0, 0}, {0, 0}}},
        [DCAM_MODE_6] = {DCAM_PIX_MONO16, 1280, 960, {{640, 3840}, {1280, 1920}, {2560, 960}, {0, 0}, {0, 0}, {0, 0}}},
        [DCAM_MODE_7] =
            {DCAM_PIX_MONO16, 1600, 1200, {{1000, 3840}, {2000, 1920}, {4000, 960}, {0, 0}, {0, 0}, {0, 0}}},
    },
};

const dcam_mode_info_t *dcam_mode_info_get(int format, int mode) {
    return &dcam_modes[format][mode];
}

/// Returns the packet geometry for one (format, mode, rate).
///
/// @param format  DCAM format index.
/// @param mode    DCAM mode index.
/// @param rate    DCAM frame-rate index.
/// @return        Pointer to the packet geometry entry.
static const dcam_packet_geometry_t *dcam_packet_geom(int format, int mode, int rate) {
    return &dcam_modes[format][mode].rate[rate];
}

uint32_t dcam_frame_rate_inq(int format, int mode) {
    uint32_t mask = 0;
    int rate;
    for (rate = 0; rate < DCAM_NUM_RATES; rate++) {
        if (dcam_packet_geom(format, mode, rate)->bytes_per_packet != 0) {
            mask |= 0x80000000U >> rate;
        }
    }
    return mask;
}

uint32_t dcam_video_mode_inq(int format) {
    uint32_t mask = 0;
    int mode;
    for (mode = 0; mode < DCAM_NUM_MODES; mode++) {
        if (dcam_frame_rate_inq(format, mode) != 0) {
            mask |= 0x80000000U >> mode;
        }
    }
    return mask;
}

/// VIDEO_FORMAT_INQ bitmask: bit (31-format) per advertised format.  Format 7
/// (scalable image) is always advertised; the standard Formats 0/1/2 are only
/// advertised when advertise_std_formats is set.  The real camera advertises
/// Format 7 alone, which is the default.
uint32_t dcam_video_format_inq(const dcam_camera_t *cam) {
    uint32_t mask = 0x80000000U >> DCAM_F7_FORMAT;
    int format;
    if (cam->advertise_std_formats) {
        for (format = 0; format < DCAM_NUM_FORMATS; format++) {
            if (dcam_video_mode_inq(format) != 0) {
                mask |= 0x80000000U >> format;
            }
        }
    }
    return mask;
}

/// Reset the camera's video state to power-up defaults: standard
/// format/mode/rate and every Format 7 mode's geometry, colour coding and
/// packet size.  A real DCAM camera does this at power-up and on the
/// DCAM_REG_INITIALIZE software reset, so each fresh host session sees a
/// pristine camera rather than state left over from a previous session (e.g. a
/// MONO16 colour coding latched by an earlier stream, which would otherwise
/// skew the advertised packet bounds and frame-rate range).  Feature registers
/// are left untouched; the host programs those explicitly before streaming.
void dcam_reset_video_state(dcam_camera_t *cam) {
    cam->cur_format = 0;
    cam->cur_mode = DCAM_MODE_5; // Format 0 mode 5 = 640x480 Mono8
    cam->cur_rate = DCAM_RATE_30;
    dcam_f7_reset(cam);
}
