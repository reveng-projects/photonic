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
/// The dcam_camera object: all mutable state of the emulated camera.  Private
/// to the dcam implementation; other modules see only the opaque dcam_camera_t
/// from dcam.h.

#ifndef DCAM_INTERNAL_H
#define DCAM_INTERNAL_H

#include "dcam.h"
#include "features.h"
#include "format7.h"
#include "fwdev.h"
#include "video.h"

#include "../common/camera_regs.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct dcam_camera {
    fw_device_t *dev;

    uint64_t dcam_base; ///< 48-bit base of the DCAM CSR region
    uint32_t csr_base;  ///< low 32 bits of dcam_base

    /// Currently-selected DCAM video format, latched from register writes.
    /// Defaults: Format 0, Mode 5 (640x480 Mono8), rate 4 (30 fps).
    int cur_format;
    dcam_video_mode_t cur_mode;
    dcam_frame_rate_t cur_rate;

    /// Format 7 (scalable image) live state, one entry per F7 mode, programmed by
    /// the host into that mode's CSR block.  Used to answer F7 register reads and
    /// to derive the iso geometry when streaming the selected F7 mode.
    struct dcam_f7_live {
        uint32_t width;            ///< IMAGE_SIZE (reg 0x0c) width
        uint32_t height;           ///< IMAGE_SIZE (reg 0x0c) height
        uint32_t offx;             ///< IMAGE_POSITION (reg 0x08) offX
        uint32_t offy;             ///< IMAGE_POSITION (reg 0x08) offY
        uint32_t color_coding;     ///< COLOR_CODING_ID (reg 0x10)
        uint32_t bytes_per_packet; ///< BYTE_PER_PACKET (reg 0x44) hi16
    } f7[DCAM_NUM_F7_MODES];

    /// Live value-register words for the DCAM feature controls.  Parallel to
    /// dcam_features[]; each holds the 32-bit logical value-register contents
    /// (presence + mode bit + 12-bit value) last written by the host, returned
    /// verbatim on read.
    uint32_t feat_value[DCAM_NUM_FEATURES];

    /// Bit i set: dcam_features[i] is implemented.  All bits by default,
    /// overridden by the PHOTONIC_FEATURES environment variable (parsed at
    /// creation by dcam_feature_parse_present).  A cleared feature behaves like
    /// an unimplemented register on the real camera: no presence bit anywhere
    /// and value-register writes rejected.
    uint32_t feat_present;

    /// Iso packet-header tuning (diagnostic overrides).
    int iso_tag;
    int iso_sy_all;

    /// Advertised mode set (see dcam_iso_config_t).  Defaults reproduce the real
    /// camera: standard formats off, a single Format 7 mode.
    int advertise_std_formats;
    unsigned num_f7_modes;

    /// Isochronous transmit state.
    int iso_fd;                 ///< dedicated fw-cdev fd owning the context, -1
    int iso_handle;             ///< fw-cdev iso context handle
    int iso_created;            ///< context created (channel fixed)
    uint8_t *iso_buffer;        ///< mmap'd DMA payload ring
    size_t iso_buffer_bytes;    ///< mapped size (fixed once)
    int iso_running;            ///< START_ISO issued, not stopped
    int iso_channel;            ///< from ISOCH_CHANNEL write
    int iso_speed;              ///< SCODE 0/1/2 = 100/200/400 Mbps
    unsigned refill_slot;       ///< next ring slot to regenerate
    unsigned iso_acked_packets; ///< packets acked since last refill
    unsigned frame_counter;     ///< drives the animated test pattern
    unsigned frames_transmitted;
    int iso_oneshot; ///< 1 = transmit a single frame then stop (DCAM ONE_SHOT,
    ///<     reg 0x61C); 0 = continuous (ISO_EN, reg 0x614)

    /// Geometry of the frame currently being streamed.
    unsigned iso_bpp;           ///< bytes per isochronous packet
    unsigned iso_ppf;           ///< packets per frame
    unsigned frame_bytes;       ///< bpp * ppf
    unsigned ring_frames;       ///< slots that fit in the buffer
    dcam_pixel_format_t pixfmt; ///< dcam_modes pixel id
    unsigned img_width;
    unsigned img_height;

    /// Command mailbox CSR (0xFFFFF0204000): a write-command / read-response
    /// register pair.  The camera stashes the last command packet
    /// (de-byte-swapped to little-endian order) so the following block-read
    /// can answer it.
    uint64_t mailbox_base;
    uint8_t mailbox_cmd[512];
    uint32_t mailbox_cmd_len;

    /// External hardware-trigger input (trigger.c): a serial port whose modem
    /// input lines carry the host's DTR pulse.  trigger_pending counts pulses
    /// whose frame has not been read out yet: the watcher thread increments it,
    /// the iso completion path consumes it one frame at a time, and STATUS
    /// FRAME_READY reports pending > 0 while TRIGGER_MODE is external.
    int trigger_fd;               ///< watched serial port fd, -1 = no input
    volatile int trigger_stop;    ///< tells the watcher thread to exit
    pthread_mutex_t trigger_lock; ///< guards trigger_pending
    unsigned trigger_pending;     ///< pulses not yet consumed by a frame

    /// Camera-head controller register file (common/camera_regs.h): the 8-bit
    /// registers of the sensor/ADC electronics on the head's I2C bus at device
    /// address CAMREG_I2C_DEV_ADDR, programmed through mailbox commands 0x1008
    /// (read) / 0x1009 (write).  CAMREG_STATUS is not stored here; it is
    /// synthesised live from the streaming state (see dcam_i2c_status).
    uint8_t i2c_regs[CAMREG_COUNT];
};

#endif // DCAM_INTERNAL_H
