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
/// DCAM Format 7 (scalable / partial-image) definitions: the F7 inquiry and
/// per-mode CSR registers, the colour-coding ids and inquiry masks, and the
/// per-mode sensor-preset type.

#ifndef FORMAT7_H
#define FORMAT7_H

#include "dcam.h"
#include "video.h"

#include <stdint.h>

/// DCAM Format 7 (scalable / partial-image) support.  Unlike Formats 0-2, a
/// Format 7 mode carries no fixed resolution: the host programs IMAGE_SIZE,
/// IMAGE_POSITION, COLOR_CODING and BYTE_PER_PACKET into a per-mode CSR block
/// and then streams the resulting geometry.  A host discovers Format 7 by:
///   - reading VIDEO_FORMAT_INQ bit 24 (DCAM format id 7);
///   - reading VIDEO_MODE_INQ_7 (reg 0x19c = 0x180 + 7*4) for the F7 mode bitmask;
///   - reading the per-mode CSR pointer V_CSR_INQ_7_m (reg 0x2e0 + m*4): a quadlet
///     offset whose CSR address low-32 = value*4 + 0xf0000000;
///   - reading/writing the F7 mode CSR registers at that base (offsets below).
///
/// The fake exposes DCAM_NUM_F7_MODES Format 7 modes (each a sensor preset: a
/// maximum resolution, unit step, supported colour codings, and a packet-size
/// step).  Each mode m gets its own CSR block at
/// DCAM_F7_CSR_BLOCK + m*DCAM_F7_CSR_BLOCK_LEN inside the DCAM register region,
/// so the CSR base the host computes resolves back into the region and the same
/// off = req->offset - dcam_base dispatch serves it.  Within a mode the image is
/// scalable (any width/height that is a multiple of the unit size, up to the
/// maximum); the bytes-per-packet bounds are derived per resolution at runtime
/// to stay in the standard DCAM packet envelope (see dcam_f7_packet_params).
constexpr int DCAM_F7_FORMAT = 7;    ///< DCAM format id for scalable image
constexpr int DCAM_NUM_F7_MODES = 4; ///< number of F7 modes in the table
constexpr uint32_t DCAM_REG_VIDEO_MODE_INQ_7 = DCAM_REG_VIDEO_MODE_INQ + DCAM_F7_FORMAT * 4; ///< 0x19c
constexpr uint32_t DCAM_REG_V_CSR_INQ_7 = 0x2e0; ///< V_CSR_INQ_7_0; mode m at +m*4

/// F7 mode CSR blocks, relative to the DCAM region base: mode m occupies
/// [DCAM_F7_CSR_BLOCK + m*LEN, +LEN).  The block array sits above the feature
/// value registers (which reach 0x888) and below the 0x1000 region length:
/// 4 modes -> 0xa00..0xe00.
constexpr uint32_t DCAM_F7_CSR_BLOCK = 0xa00;
constexpr uint32_t DCAM_F7_CSR_BLOCK_LEN = 0x100;

/// F7 packet-size envelope.  A real DCAM/IIDC camera advertises its BYTE_PER_PACKET
/// granularity in PACKET_PARA_INQ (reg 0x40): the high word UNIT_BYTE_PER_PACKET is
/// the packet-size step and the low word MAX_BYTE_PER_PACKET the largest packet it
/// will emit.  BYTE_PER_PACKET (reg 0x44) must be an integer multiple of the unit
/// within [unit .. max]; the camera NAKs (transaction data-error) any other value.
///
/// This camera reports the unit as the bytes in one image line
/// (width * bits_per_pixel / 8) and caps the max at the largest whole-line multiple
/// that still fits the S400 isochronous payload (DCAM_F7_BUS_MAX_BPP).  So for
/// 1388-wide MONO8 the unit is 1388 and the only legal packet sizes are 1388 or
/// 2776 (PACKET_PARA_INQ = 0x056c0ad8); for MONO16 the unit is 2776 and 2776 is the
/// only legal value (0x0ad80ad8).  Frame rate = 8000 / packets_per_frame, so the
/// unit gives the slowest rate (one packet per line, ppf = height) and the max the
/// fastest.
///
/// Modelling this granularity means a BYTE_PER_PACKET value that is merely
/// quadlet-aligned instead of unit-aligned is rejected exactly as the real
/// camera rejects it.
constexpr uint32_t DCAM_F7_BUS_MAX_BPP = 4096; ///< S400 isochronous payload ceiling

/// F7 mode CSR registers, relative to DCAM_F7_CSR_BLOCK.
constexpr uint32_t DCAM_F7_MAX_IMAGE_SIZE = 0x00;       ///< MAX width<<16 | height (read-only)
constexpr uint32_t DCAM_F7_UNIT_SIZE = 0x04;            ///< unit width<<16 | height (read-only)
constexpr uint32_t DCAM_F7_IMAGE_POSITION = 0x08;       ///< offX<<16 | offY (read/write)
constexpr uint32_t DCAM_F7_IMAGE_SIZE = 0x0c;           ///< width<<16 | height (read/write)
constexpr uint32_t DCAM_F7_COLOR_CODING_ID = 0x10;      ///< coding<<24 (read/write)
constexpr uint32_t DCAM_F7_COLOR_CODING_INQ = 0x14;     ///< supported codings bitmask (read-only)
constexpr uint32_t DCAM_F7_PIXEL_NUMBER_INQ = 0x34;     ///< pixels per frame (read-only)
constexpr uint32_t DCAM_F7_TOTAL_BYTES_HI = 0x38;       ///< total bytes/frame, high 32 bits (RO)
constexpr uint32_t DCAM_F7_TOTAL_BYTES_LO = 0x3c;       ///< total bytes/frame, low 32 bits (RO)
constexpr uint32_t DCAM_F7_PACKET_PARA_INQ = 0x40;      ///< unitBPP<<16 | maxBPP (read-only)
constexpr uint32_t DCAM_F7_BYTE_PER_PACKET = 0x44;      ///< bytesPerPacket<<16 (read/write)
constexpr uint32_t DCAM_F7_PACKET_PER_FRAME_INQ = 0x48; ///< packets per frame (read-only)

/// F7 COLOR_CODING_ID values (DCAM colour-coding ids) and the COLOR_CODING_INQ
/// bitmask the fake advertises.  COLOR_CODING_INQ sets bit (31 - codingId) per
/// supported coding.  The fake advertises four codings: MONO8, YUV422 (UYVY),
/// MONO16, and RAW8 (Bayer RGGB).  A coding must be advertised before the host
/// may select it.
constexpr int DCAM_F7_CODING_MONO8 = 0;
constexpr int DCAM_F7_CODING_YUV422 = 2;
constexpr int DCAM_F7_CODING_MONO16 = 5;
constexpr int DCAM_F7_CODING_RAW8 = 7;
constexpr uint32_t DCAM_F7_COLOR_INQ_ALL = (0x80000000u >> DCAM_F7_CODING_MONO8) |  ///< 0x80000000 MONO8
                                           (0x80000000u >> DCAM_F7_CODING_YUV422) | ///< 0x20000000 YUV422
                                           (0x80000000u >> DCAM_F7_CODING_MONO16) | ///< 0x04000000 MONO16
                                           (0x80000000u >> DCAM_F7_CODING_RAW8);    ///< 0x01000000 RAW8

/// Same set without YUV422, for the full-frame mode: a 1280x960 YUV422 stream
/// at 15 fps needs packets above the S400 4096-byte isochronous payload limit,
/// so advertising YUV422 there would include an unstreamable rate.  The smaller
/// F7 modes keep YUV422 (every advertised rate fits the payload limit).
constexpr uint32_t DCAM_F7_COLOR_INQ_NO_YUV422 = (0x80000000u >> DCAM_F7_CODING_MONO8) |  ///< 0x80000000 MONO8
                                                 (0x80000000u >> DCAM_F7_CODING_MONO16) | ///< 0x04000000 MONO16
                                                 (0x80000000u >> DCAM_F7_CODING_RAW8);    ///< 0x01000000 RAW8

/// Greyscale-only coding set (MONO8 + MONO16).  The real "Photonic" camera in the
/// capture trace advertises exactly this for its full-frame F7 mode
/// (COLOR_CODING_INQ = 0x84000000), producing the two stream formats the test
/// exercises (1388x1032 Y8 and Y16).
constexpr uint32_t DCAM_F7_COLOR_INQ_MONO = (0x80000000u >> DCAM_F7_CODING_MONO8) | ///< 0x80000000 MONO8
                                            (0x80000000u >> DCAM_F7_CODING_MONO16); ///< 0x04000000 MONO16

/// One Format 7 mode (sensor preset).  The image is scalable within [unit .. max],
/// so width/height are not fixed here; def_* is the size programmed at reset and
/// advertised as the default.  The PACKET_PARA_INQ bytes-per-packet bounds are NOT
/// stored per mode: a fixed packet step cannot honour a frame rate across a
/// scalable resolution (it either runs a small ROI too fast or pins a large frame
/// to one rate), so the bounds are derived from the live frame size to stay in the
/// standard DCAM packet envelope at runtime (see dcam_f7_packet_params).
typedef struct dcam_f7_mode_info {
    uint16_t max_width, max_height;   ///< MAX_IMAGE_SIZE (reg 0x00)
    uint16_t unit_width, unit_height; ///< UNIT_SIZE (reg 0x04)
    uint16_t def_width, def_height;   ///< default/current IMAGE_SIZE
    uint32_t color_coding_inq;        ///< COLOR_CODING_INQ (reg 0x14)
} dcam_f7_mode_info_t;

/// Returns the bits-per-pixel for an F7 colour coding id.
///
/// @param coding  F7 colour coding id (e.g. DCAM_F7_CODING_MONO8).
/// @return        Bits per pixel, or 0 for an unknown coding.
uint32_t dcam_f7_bits_per_pixel(uint32_t coding);

/// Returns the V_CSR_INQ_7_m register value: a quadlet offset such that
/// value*4 + 0xf0000000 lands on (csr_base + mode's CSR block), so that mode's
/// register accesses arrive at off = block_base + reg.
///
/// @param cam   Camera whose csr_base to use.
/// @param mode  F7 mode index.
/// @return      Quadlet pointer value for the V_CSR_INQ register.
uint32_t dcam_f7_csr_pointer(const dcam_camera_t *cam, int mode);

/// Reads one CSR register (reg relative to the block base) of F7 mode `mode`.
///
/// @param cam   Camera whose F7 live state to read.
/// @param mode  F7 mode index.
/// @param reg   Register offset relative to the mode's CSR block base.
/// @return      Logical (host-byte-order) register value.
uint32_t dcam_f7_csr_read(const dcam_camera_t *cam, int mode, uint32_t reg);

/// Writes one CSR register (reg relative to the block base) of F7 mode `mode`.
///
/// @param cam      Camera whose F7 live state to update.
/// @param mode     F7 mode index.
/// @param reg      Register offset relative to the mode's CSR block base.
/// @param logical  Logical (host-byte-order) value to write.
/// @return         1394 response code: RCODE_COMPLETE on success, or
///                 RCODE_DATA_ERROR for an illegal BYTE_PER_PACKET.
int dcam_f7_csr_write(dcam_camera_t *cam, int mode, uint32_t reg, uint32_t logical);

/// Resets every F7 mode's geometry, colour coding and packet size to the mode
/// table's power-up defaults.
///
/// @param cam  Camera whose F7 state to reset.
void dcam_f7_reset(dcam_camera_t *cam);

#endif // FORMAT7_H
