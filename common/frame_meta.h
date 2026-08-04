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
/// Binary verification header embedded at offset 0 of every streamed video frame
/// by the fake camera (fake-fw-dev/dcam.c) and checked at the far end of the
/// DirectShow path by the test program (test/directshow).  It carries the
/// virtual camera's state plus a checksum of the whole frame, so the test can
/// prove that frames arrive intact, in order, and starting from index 0.
///
/// This is the single shared definition: the producer (Linux, C) and the
/// consumer (Windows, C++) both include this header so the layout cannot drift.
/// Every field is a fixed-width little-endian quadlet; both ends are
/// little-endian, so the struct travels byte-for-byte with no marshalling.
#ifndef PHOTONIC_FRAME_META_H
#define PHOTONIC_FRAME_META_H

#include <stddef.h>
#include <stdint.h>

#include "camera_regs.h"

/// 'P','H','O','T' read back as a little-endian quadlet.
constexpr uint32_t FRAME_META_MAGIC = 0x544F4850u;
constexpr uint32_t FRAME_META_VERSION = 3u;

/// Camera feature/control slots carried in frame_meta.features[], in the exact
/// order the fake camera lays them out (must match dcam_features[] in
/// fake-fw-dev/dcam.c).  Each name is the DirectShow control the driver maps the
/// underlying DCAM feature register to.  FRAME_META_NUM_FEATURES is both the
/// array length and the value the producer stamps into frame_meta.feature_count.
enum frame_meta_feature {
    FRAME_META_FEAT_BRIGHTNESS = 0, ///< VideoProcAmp Brightness
    FRAME_META_FEAT_SHARPNESS,      ///< VideoProcAmp Sharpness
    FRAME_META_FEAT_WHITEBALANCE,   ///< VideoProcAmp WhiteBalance
    FRAME_META_FEAT_HUE,            ///< VideoProcAmp Hue
    FRAME_META_FEAT_SATURATION,     ///< VideoProcAmp Saturation
    FRAME_META_FEAT_GAMMA,          ///< VideoProcAmp Gamma
    FRAME_META_FEAT_EXPOSURE,       ///< CameraControl Exposure
    FRAME_META_FEAT_CONTRAST,       ///< VideoProcAmp Contrast
    FRAME_META_FEAT_IRIS,           ///< CameraControl Iris
    FRAME_META_FEAT_FOCUS,          ///< CameraControl Focus
    FRAME_META_FEAT_ZOOM,           ///< CameraControl Zoom
    FRAME_META_FEAT_PAN,            ///< CameraControl Pan
    FRAME_META_FEAT_TILT,           ///< CameraControl Tilt
    FRAME_META_NUM_FEATURES
};

/// Value range of each feature: X(feature, min, max, default), in
/// frame_meta_feature order.  This is the single shared definition of the fake
/// camera's control ranges: the fake expands it into its register model
/// (fake-fw-dev/features.c) and the DirectShow test into its GetRange
/// expectations, so the two cannot drift.  The ranges deliberately vary --
/// different widths, several non-zero minima, one full-scale feature and
/// contrast (gain) mirroring the real camera's 0..383 -- so a driver that
/// hardcodes 0..4095 fails the test.  A real camera has its own ranges; the
/// test skips range verification there.
#define FRAME_META_FEATURE_RANGES(X)              \
    X(FRAME_META_FEAT_BRIGHTNESS, 0, 255, 128)    \
    X(FRAME_META_FEAT_SHARPNESS, 0, 7, 4)         \
    X(FRAME_META_FEAT_WHITEBALANCE, 0, 1023, 512) \
    X(FRAME_META_FEAT_HUE, 16, 240, 128)          \
    X(FRAME_META_FEAT_SATURATION, 0, 511, 256)    \
    X(FRAME_META_FEAT_GAMMA, 1, 3, 2)             \
    X(FRAME_META_FEAT_EXPOSURE, 0, 4095, 2048)    \
    X(FRAME_META_FEAT_CONTRAST, 0, 383, 192)      \
    X(FRAME_META_FEAT_IRIS, 0, 100, 50)           \
    X(FRAME_META_FEAT_FOCUS, 50, 1000, 525)       \
    X(FRAME_META_FEAT_ZOOM, 0, 63, 32)            \
    X(FRAME_META_FEAT_PAN, 10, 170, 90)           \
    X(FRAME_META_FEAT_TILT, 0, 90, 45)

#pragma pack(push, 1)
typedef struct frame_meta {
    uint32_t magic;         ///< FRAME_META_MAGIC; locates and validates the header
    uint32_t version;       ///< FRAME_META_VERSION
    uint32_t frame_index;   ///< 0-based frame counter; resets to 0 on a new stream
    uint32_t frame_bytes;   ///< total frame size in bytes, including this header
    uint32_t width;         ///< image width in pixels
    uint32_t height;        ///< image height in pixels
    uint32_t pixfmt;        ///< dcam pixel format id (dcam_pixel_format_t)
    uint32_t format;        ///< dcam video format
    uint32_t mode;          ///< dcam video mode
    uint32_t rate;          ///< dcam frame rate id
    uint32_t feature_count; ///< == FRAME_META_NUM_FEATURES (layout-drift guard)
    /// Live DCAM feature value-register words, indexed by frame_meta_feature.
    /// Each is presence(bit31) | auto-bit | 12-bit value (low bits); see the
    /// accessor macros below.
    uint32_t features[FRAME_META_NUM_FEATURES];
    uint32_t i2c_reg_count; ///< == CAMREG_COUNT (layout-drift guard)
    /// Live camera-head controller register file (common/camera_regs.h), indexed
    /// by register address 0x00..CAMREG_COUNT-1.  A byte-for-byte snapshot of the
    /// registers programmed over the head's I2C bus (gain, exposure, binning,
    /// readout window, trigger mode, ...) plus the live STATUS register, so the
    /// test can verify register writes reached the camera head.
    uint8_t i2c_regs[CAMREG_COUNT];
    uint32_t checksum; ///< FNV-1a over the whole frame, this field taken as 0
} frame_meta_t;
#pragma pack(pop)

/// Extract the 12-bit setting and the presence bit from a feature word.
#define FRAME_META_FEATURE_VALUE(word)   ((word) & 0x0FFFu)
#define FRAME_META_FEATURE_PRESENT(word) (((word) >> 31) & 0x1u)

/// Byte offset of the checksum field, excluded from the checksum it holds.
constexpr size_t FRAME_META_CHECKSUM_OFFSET = offsetof(frame_meta_t, checksum);

/// FNV-1a (32-bit) over the entire frame, treating the four checksum bytes as
/// zero so the result does not depend on the value being stored.  The producer
/// writes the return value into frame_meta.checksum; the consumer recomputes the
/// same way and compares.  frame_bytes must be the full frame length.
static inline uint32_t frame_meta_checksum(const void *frame, size_t frame_bytes) {
    const uint8_t *bytes = (const uint8_t *) frame;
    uint32_t hash = 2166136261u; // FNV-1a offset basis
    size_t i;

    for (i = 0; i < frame_bytes; i++) {
        uint8_t b = bytes[i];
        if (i >= FRAME_META_CHECKSUM_OFFSET && i < FRAME_META_CHECKSUM_OFFSET + sizeof(uint32_t)) {
            b = 0; // exclude the checksum field from its own checksum
        }
        hash ^= b;
        hash *= 16777619u; // FNV-1a prime
    }
    return hash;
}

#endif // PHOTONIC_FRAME_META_H
