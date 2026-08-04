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
/// Standard IIDC/DCAM video definitions: the video control and inquiry register
/// offsets, the frame-rate / video-mode / pixel-format ids, and the geometry
/// types describing one (format, mode, rate).

#ifndef VIDEO_H
#define VIDEO_H

#include "dcam.h"

#include <stdint.h>

/// DCAM CSR register offsets (relative to CsrBase).
constexpr uint32_t DCAM_REG_INITIALIZE = 0x000; ///< bit31 = software reset to power-up state
constexpr uint32_t DCAM_REG_FRAME_RATE = 0x600;
constexpr uint32_t DCAM_REG_VIDEO_MODE = 0x604;
constexpr uint32_t DCAM_REG_VIDEO_FORMAT = 0x608;
constexpr uint32_t DCAM_REG_ISOCH_CHANNEL = 0x60C;
constexpr uint32_t DCAM_REG_ISO_EN = 0x614;   ///< bit31 = continuous isochronous transmit
constexpr uint32_t DCAM_REG_ONE_SHOT = 0x61C; ///< bit31 = one-shot (capture one frame)

/// DCAM read-only inquiry-register blocks (capability advertisement).
///   VIDEO_FORMAT_INQ: one word, bitmask of supported formats.
///   VIDEO_MODE_INQ_<f>: one word per format (stride 4), bitmask of modes.
///   FRAME_RATE_INQ_<f>_<m>: one word per (format, mode) (stride 4), rate bitmask.
constexpr uint32_t DCAM_REG_VIDEO_FORMAT_INQ = 0x100;
constexpr uint32_t DCAM_REG_VIDEO_MODE_INQ = 0x180;
constexpr uint32_t DCAM_REG_FRAME_RATE_INQ = 0x200;

/// The standard IIDC/DCAM frame-rate ids (the value latched into cam->cur_rate
/// from the FRAME_RATE register).
typedef enum dcam_frame_rate {
    DCAM_RATE_1_875 = 0, ///< 1.875 fps
    DCAM_RATE_3_75 = 1,  ///< 3.75 fps
    DCAM_RATE_7_5 = 2,   ///< 7.5 fps
    DCAM_RATE_15 = 3,    ///< 15 fps
    DCAM_RATE_30 = 4,    ///< 30 fps
    DCAM_RATE_60 = 5,    ///< 60 fps
} dcam_frame_rate_t;

/// DCAM video-mode ids within a format (the value latched into cam->cur_mode
/// from the VIDEO_MODE register).
///
/// A mode index has NO fixed resolution/coding of its own: its meaning is
/// format-relative, so the modes cannot carry descriptive names.  The
/// (resolution, colour coding) for each (format, mode) per the IIDC/DCAM
/// "1394-based Digital Camera Specification" v1.31 is:
///
///   mode | Format 0          | Format 1           | Format 2
///   -----+-------------------+--------------------+--------------------
///    0   | 160x120  YUV444   | 800x600   YUV422   | 1280x960  YUV422
///    1   | 320x240  YUV422   | 800x600   RGB      | 1280x960  RGB
///    2   | 640x480  YUV411   | 800x600   Mono8    | 1280x960  Mono8
///    3   | 640x480  YUV422   | 1024x768  YUV422   | 1600x1200 YUV422
///    4   | 640x480  RGB      | 1024x768  RGB      | 1600x1200 RGB
///    5   | 640x480  Mono8    | 1024x768  Mono8    | 1600x1200 Mono8
///    6   | 640x480  Mono16   | 800x600   Mono16   | 1280x960  Mono16
///    7   | (reserved)        | 1024x768  Mono16   | 1600x1200 Mono16
///
/// Only Format 0 modes 0..5 date back to IIDC v1.04 (1996), which defined
/// Format 0 alone; Format 1/2 (SVGA, v1.20) and the Mono16 modes (v1.30) are
/// later additions.  This is the authoritative source for the dcam_modes[]
/// table in video.c.
typedef enum dcam_video_mode {
    DCAM_MODE_0 = 0,
    DCAM_MODE_1 = 1,
    DCAM_MODE_2 = 2,
    DCAM_MODE_3 = 3,
    DCAM_MODE_4 = 4,
    DCAM_MODE_5 = 5,
    DCAM_MODE_6 = 6,
    DCAM_MODE_7 = 7,
} dcam_video_mode_t;

/// DCAM pixel-format ids (latched into cam->pixfmt): the standard IIDC/DCAM
/// colour codings selected by a (format, mode) pair; PIX_INVALID marks an
/// unsupported mode slot.
typedef enum dcam_pixel_format {
    DCAM_PIX_MONO8 = 0,            ///< 8-bit greyscale
    DCAM_PIX_YUV411 = 1,           ///< YUV 4:1:1
    DCAM_PIX_YUV422 = 2,           ///< YUV 4:2:2 (UYVY)
    DCAM_PIX_YUV444 = 3,           ///< YUV 4:4:4
    DCAM_PIX_RGB24 = 4,            ///< 24-bit RGB
    DCAM_PIX_MONO16 = 5,           ///< 16-bit greyscale
    DCAM_PIX_RAW8 = 7,             ///< 8-bit Bayer RGGB (F7 only)
    DCAM_PIX_INVALID = 0xffffffff, ///< unsupported mode
} dcam_pixel_format_t;

/// DCAM geometry-table dimensions.
constexpr int DCAM_NUM_FORMATS = 3;
constexpr int DCAM_NUM_MODES = 8;
constexpr int DCAM_NUM_RATES = 6;

/// Isochronous packetisation of a single (format, mode, rate); zero
/// bytes-per-packet marks an unsupported slot.
typedef struct dcam_packet_geometry {
    uint16_t bytes_per_packet;
    uint16_t packets_per_frame;
} dcam_packet_geometry_t;

/// Frame geometry of one (format, mode) plus its per-frame-rate iso
/// packetisation.  rate[r].bytes_per_packet == 0 marks an unsupported rate;
/// pixfmt == DCAM_PIX_INVALID marks an unsupported mode.  Rate order matches
/// dcam_frame_rate_t (DCAM_RATE_1_875 .. DCAM_RATE_60).
typedef struct dcam_mode_info {
    dcam_pixel_format_t pixfmt; ///< colour coding (DCAM_PIX_*)
    uint16_t width;
    uint16_t height;
    dcam_packet_geometry_t rate[DCAM_NUM_RATES];
} dcam_mode_info_t;

/// Returns the geometry/packetisation of one (format, mode) from the merged mode table.
///
/// @param format  DCAM format index.
/// @param mode    DCAM mode index.
/// @return        Pointer to the mode info entry.
const dcam_mode_info_t *dcam_mode_info_get(int format, int mode);

/// Returns the FRAME_RATE_INQ bitmask for (format, mode): bit (31-rate) per supported rate.
///
/// @param format  DCAM format index.
/// @param mode    DCAM mode index.
/// @return        Rate bitmask.
uint32_t dcam_frame_rate_inq(int format, int mode);

/// Returns the VIDEO_MODE_INQ bitmask for a format: bit (31-mode) per mode with any rate.
///
/// @param format  DCAM format index.
/// @return        Mode bitmask.
uint32_t dcam_video_mode_inq(int format);

/// Returns the VIDEO_FORMAT_INQ bitmask: bit (31-format) per advertised format.
///
/// @param cam  Camera whose advertise_std_formats flag to consult.
/// @return     Format bitmask.
uint32_t dcam_video_format_inq(const dcam_camera_t *cam);

/// Resets format/mode/rate and the Format 7 state to power-up defaults.
///
/// @param cam  Camera to reset.
void dcam_reset_video_state(dcam_camera_t *cam);

#endif // VIDEO_H
