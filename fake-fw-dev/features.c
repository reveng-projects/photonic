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
/// DCAM feature controls: the per-feature register map and the inquiry-register
/// lookups.

#include "features.h"

#include "log.h"

#include "../common/frame_meta.h"

#include <string.h>

/// Each entry maps a DCAM feature inquiry register to its name; the value
/// register sits DCAM_FEATURE_VALUE_OFFSET (0x300) above the inquiry register.
/// Two names differ from the DCAM register names: "exposure" is the SHUTTER
/// register 0x51c and "contrast" is the GAIN register 0x520, matching the
/// controls the host exposes them as.
const dcam_feature_info_t dcam_features[DCAM_NUM_FEATURES] = {
    {0x500, "brightness"},   // BRIGHTNESS
    {0x508, "sharpness"},    // SHARPNESS
    {0x50c, "whitebalance"}, // WHITE_BALANCE
    {0x510, "hue"},          // HUE
    {0x514, "saturation"},   // SATURATION
    {0x518, "gamma"},        // GAMMA
    {0x51c, "exposure"},     // SHUTTER
    {0x520, "contrast"},     // GAIN
    {0x524, "iris"},         // IRIS
    {0x528, "focus"},        // FOCUS
    {0x580, "zoom"},         // ZOOM
    {0x584, "pan"},          // PAN
    {0x588, "tilt"},         // TILT
};

/// Per-feature value ranges, expanded from the shared FRAME_META_FEATURE_RANGES
/// list (common/frame_meta.h) the test program also builds its expectations
/// from, so the register model and the test cannot drift.  Indexed like
/// dcam_features[] (frame_meta_feature order).
typedef struct dcam_feature_range {
    uint16_t min;
    uint16_t max;
    uint16_t def;
} dcam_feature_range_t;

#define DCAM_FEATURE_RANGE_ENTRY(feat, mn, mx, df) {mn, mx, df},
static const dcam_feature_range_t dcam_feature_ranges[DCAM_NUM_FEATURES] = {
    FRAME_META_FEATURE_RANGES(DCAM_FEATURE_RANGE_ENTRY)};

#define DCAM_FEATURE_RANGE_COUNT(feat, mn, mx, df) +1
_Static_assert(0 FRAME_META_FEATURE_RANGES(DCAM_FEATURE_RANGE_COUNT) == DCAM_NUM_FEATURES,
               "FRAME_META_FEATURE_RANGES out of sync with dcam_features[]");

int dcam_feature_by_inq(uint32_t off) {
    int i;
    for (i = 0; i < DCAM_NUM_FEATURES; i++) {
        if (dcam_features[i].inq_reg == off) {
            return i;
        }
    }
    return -1;
}

int dcam_feature_by_value(uint32_t off) {
    int i;
    for (i = 0; i < DCAM_NUM_FEATURES; i++) {
        if (dcam_features[i].inq_reg + DCAM_FEATURE_VALUE_OFFSET == off) {
            return i;
        }
    }
    return -1;
}

uint32_t dcam_feature_block_inq(uint32_t block, uint32_t present_mask) {
    uint32_t mask = 0;
    int i;
    for (i = 0; i < DCAM_NUM_FEATURES; i++) {
        uint32_t reg = dcam_features[i].inq_reg;
        if ((present_mask & (1u << i)) != 0 && reg >= block && reg < block + 0x40) {
            mask |= 0x80000000u >> ((reg - block) / 4);
        }
    }
    return mask;
}

uint32_t dcam_feature_parse_present(const char *spec) {
    const uint32_t all = (1u << DCAM_NUM_FEATURES) - 1;
    uint32_t mask = 0;
    const char *p;

    if (spec == NULL || *spec == '\0' || strcmp(spec, "all") == 0) {
        return all;
    }
    if (strcmp(spec, "none") == 0) {
        return 0;
    }

    p = spec;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        int i, found = 0;

        for (i = 0; i < DCAM_NUM_FEATURES; i++) {
            if (strlen(dcam_features[i].name) == len && strncmp(p, dcam_features[i].name, len) == 0) {
                mask |= 1u << i;
                found = 1;
                break;
            }
        }
        if (!found && len > 0) {
            LOG(WARN, "PHOTONIC_FEATURES: unknown feature \"%.*s\" ignored", (int) len, p);
        }
        p = end != NULL ? end + 1 : p + len;
    }
    return mask;
}

uint32_t dcam_feature_inq_word(int idx) {
    const dcam_feature_range_t *r = &dcam_feature_ranges[idx];
    return DCAM_FEATURE_INQ_BITS | ((uint32_t) (r->min & 0xfffu) << 12) | (r->max & 0xfffu);
}

uint32_t dcam_feature_default_word(int idx) {
    return DCAM_FEATURE_VALUE_BITS | (dcam_feature_ranges[idx].def & 0xfffu);
}

uint32_t dcam_feature_clamp_word(int idx, uint32_t word) {
    const dcam_feature_range_t *r = &dcam_feature_ranges[idx];
    uint32_t value = word & 0xfffu;
    if (value < r->min) {
        value = r->min;
    }
    if (value > r->max) {
        value = r->max;
    }
    return (word & ~0xfffu) | value;
}
