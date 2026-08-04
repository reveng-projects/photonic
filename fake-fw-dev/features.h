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
/// DCAM feature-control definitions (brightness, shutter, gain, ...).  Each
/// feature has a read-only inquiry register in the 0x500/0x580 block and a
/// read/write status-value register 0x300 above it (0x800/0x880 block).  The
/// host reads the inquiry register to learn that the feature exists and its
/// auto/manual capability, then reads/writes the value register for the live
/// setting.  See dcam_features[] in features.c for the per-feature map.

#ifndef FEATURES_H
#define FEATURES_H

#include <stdint.h>

constexpr int DCAM_NUM_FEATURES = 13;
constexpr uint32_t DCAM_FEATURE_VALUE_OFFSET = 0x300;

/// Feature presence-inquiry registers.  Hosts read these first to discover
/// which features are implemented before touching the per-feature registers.
/// FEATURE_HI_INQ covers the 0x500 block (brightness..focus), with one bit per
/// feature at bit (31 - (reg-0x500)/4); FEATURE_LO_INQ covers the 0x580 block
/// (zoom/pan/tilt) at bit (31 - (reg-0x580)/4).
constexpr uint32_t DCAM_REG_FEATURE_HI_INQ = 0x404;
constexpr uint32_t DCAM_REG_FEATURE_LO_INQ = 0x408;
constexpr uint32_t DCAM_FEATURE_HI_BLOCK = 0x500;
constexpr uint32_t DCAM_FEATURE_LO_BLOCK = 0x580;

/// Capability bits of the inquiry word advertised for every supported feature:
///   bit31 (0x80000000) Presence_Inq — feature is implemented
///   bit27 (0x08000000) Readout_Inq  — value register is readable
///   bit25 (0x02000000) Auto_Inq     — AUTO mode supported
///   bit24 (0x01000000) Manual_Inq   — MANUAL mode supported
/// The full word adds bits[23:12] MIN and bits[11:0] MAX, which differ per
/// feature (dcam_feature_inq_word) so hosts' range discovery is exercised with
/// something other than a uniform 0..4095.
constexpr uint32_t DCAM_FEATURE_INQ_BITS = 0x8b000000U;

/// Mode bits of a value word: presence + manual mode (ON, auto clear).
constexpr uint32_t DCAM_FEATURE_VALUE_BITS = 0x82000000U;

/// One feature-control mapping: the DCAM inquiry register and the feature's
/// name.  The value register sits DCAM_FEATURE_VALUE_OFFSET (0x300) above the
/// inquiry register.  The value range each feature supports lives in the shared
/// FRAME_META_FEATURE_RANGES list (common/frame_meta.h), expanded in features.c.
typedef struct dcam_feature_info {
    uint32_t inq_reg; ///< feature inquiry register offset
    const char *name; ///< feature name (tracing, PHOTONIC_FEATURES parsing)
} dcam_feature_info_t;

/// The per-feature map (features.c); parallel to cam->feat_value[].
extern const dcam_feature_info_t dcam_features[DCAM_NUM_FEATURES];

/// Returns the index of the feature whose inquiry register is `off`, or -1.
///
/// @param off  Register offset to look up.
/// @return     Feature index, or -1 if no feature matches.
int dcam_feature_by_inq(uint32_t off);

/// Returns the index of the feature whose value register is `off`, or -1.
///
/// @param off  Register offset to look up.
/// @return     Feature index, or -1 if no feature matches.
int dcam_feature_by_value(uint32_t off);

/// Returns the inquiry word of feature `idx`: capability bits + its min/max range.
///
/// @param idx  Feature index.
/// @return     32-bit inquiry register word.
uint32_t dcam_feature_inq_word(int idx);

/// Returns the power-up value word of feature `idx`: presence + manual + its default.
///
/// @param idx  Feature index.
/// @return     32-bit value register word.
uint32_t dcam_feature_default_word(int idx);

/// Returns the value word as the camera latches it: the 12-bit value clamped
/// into the feature's range (like the real camera), flag bits preserved.
///
/// @param idx   Feature index.
/// @param word  Value word as written by the host.
/// @return      Clamped value word.
uint32_t dcam_feature_clamp_word(int idx, uint32_t word);

/// Returns the presence-inquiry word for one feature block: a bit per
/// implemented feature in [block, block+0x40), at bit (31 - (reg-block)/4).
/// Used for FEATURE_HI_INQ (block 0x500) and FEATURE_LO_INQ (block 0x580).
///
/// @param block         Base register offset of the inquiry block (0x500 or 0x580).
/// @param present_mask  Bit mask of implemented features (bit i = dcam_features[i]).
/// @return              32-bit inquiry bitmask.
uint32_t dcam_feature_block_inq(uint32_t block, uint32_t present_mask);

/// Parses a PHOTONIC_FEATURES specification into a presence mask (bit i =
/// dcam_features[i] implemented).  NULL, "" and "all" select every feature (the
/// default), "none" selects none; anything else is a comma-separated list of
/// feature names (dcam_features[].name) to implement.  Unknown names are logged
/// and ignored.
///
/// @param spec  Specification string, or NULL for all features.
/// @return      Presence bitmask.
uint32_t dcam_feature_parse_present(const char *spec);

#endif // FEATURES_H
