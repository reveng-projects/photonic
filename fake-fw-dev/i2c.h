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
/// Camera-head controller register file (I2C device CAMREG_I2C_DEV_ADDR).
/// The camera-head electronics (CCD sequencer / ADC controller) live behind the
/// FireWire interface on the head's I2C bus; the host programs them one 8-bit
/// register at a time through the command mailbox: command 0x1009 writes a
/// register, command 0x1008 reads one back (see mailbox.c).  The register map is
/// shared with the test program in common/camera_regs.h and specified in
/// docs/camera-head-registers.md; the live register file travels to the test in
/// every frame's metadata block (frame.c).

#ifndef I2C_H
#define I2C_H

#include "dcam.h"

#include <stdint.h>

/// Returns the human-readable name of a head register, for tracing; NULL if unnamed.
///
/// @param reg  Register address.
/// @return     Name string, or NULL if the register is not named.
const char *dcam_i2c_reg_name(uint32_t reg);

/// Resets the head register file to power-up defaults.  Multi-register values
/// default to zero; the factors that a real head reports as at least 1 (binning,
/// gains) power up at 1 so an initial read never sees an impossible 0 factor.
///
/// @param cam  Camera whose head register file to reset.
void dcam_i2c_reset(dcam_camera_t *cam);

/// Returns the live STATUS register value.  FRAME_READY (bit 2) reports that an
/// exposed frame is waiting for readout.  In free-run the fake generates frames
/// continuously while the isochronous stream runs, so the bit mirrors the
/// streaming state; with TRIGGER_MODE = CAMREG_TRIGGER_EXTERNAL it reports
/// that a hardware trigger pulse (trigger.c) is pending whose frame has not
/// been read out yet.  Reading STATUS never clears the bit; only reading the
/// frame out does.
///
/// @param cam  Camera to query.
/// @return     STATUS register byte.
uint8_t dcam_i2c_status(const dcam_camera_t *cam);

/// Reads one head register.  STATUS is synthesised live; others come from the
/// register file; out-of-range addresses read as 0.
///
/// @param cam  Camera to query.
/// @param reg  Register address.
/// @return     Register value.
uint8_t dcam_i2c_read_reg(const dcam_camera_t *cam, uint32_t reg);

/// Writes one head register.  STATUS is read-only and out-of-range addresses are
/// ignored; both are logged and otherwise dropped, like a real head that simply
/// does not decode them.
///
/// @param cam    Camera whose register file to update.
/// @param reg    Register address.
/// @param value  Value to write.
void dcam_i2c_write_reg(dcam_camera_t *cam, uint32_t reg, uint8_t value);

#endif // I2C_H
