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
/// Register map of the camera-head controller: the CCD sequencer/ADC
/// electronics that sit behind the FireWire interface on the camera's internal
/// I2C bus at device address CAMREG_I2C_DEV_ADDR.  The host reaches these
/// registers through the vendor command CSR (see fake-fw-dev/dcam.c,
/// dcam_on_mailbox_write/read): command 0x1009 writes one register, command 0x1008
/// reads one back.  Registers are 8-bit address / 8-bit data; multi-byte
/// quantities occupy consecutive registers little-endian (LSB at the lower
/// address).
///
/// This is the single shared definition: the fake camera (Linux, C) models the
/// register file and the test program (Windows, C++) decodes it from the
/// frame_meta block, so the map cannot drift between the two.
///
/// Full specification: docs/camera-head-registers.md
#ifndef PHOTONIC_CAMERA_REGS_H
#define PHOTONIC_CAMERA_REGS_H

/// I2C device address of the camera-head controller.
constexpr int CAMREG_I2C_DEV_ADDR = 0x14;

/// Number of modelled registers (addresses 0x00..CAMREG_COUNT-1).
constexpr int CAMREG_COUNT = 0x30;

/// Register offsets.  "LE16" pairs hold a 16-bit value split across two
/// consecutive registers, low byte first.
constexpr int CAMREG_VIDEO_GAIN = 0x00;       ///< video/ADC gain, 1..100
constexpr int CAMREG_INTENSIFIER_GAIN = 0x02; ///< image-intensifier gain, 1..100
constexpr int CAMREG_EXPOSURE_UNITS = 0x04;   ///< 0 = us, 1 = ms, 2 = s
constexpr int CAMREG_EXPOSURE_TIME_L = 0x06;  ///< exposure time LE16 low byte
constexpr int CAMREG_EXPOSURE_TIME_H = 0x07;  ///< exposure time LE16 high byte (14-bit, 1..0x3FFF, in EXPOSURE_UNITS)
constexpr int CAMREG_X_BINNING = 0x0C;        ///< horizontal binning factor, 1..63
constexpr int CAMREG_Y_BINNING = 0x0D;        ///< vertical binning factor, 1..63
constexpr int CAMREG_SPEED_L = 0x0E;          ///< pixel clock in kHz LE16 low byte
constexpr int CAMREG_SPEED_H = 0x0F;          ///< pixel clock LE16 high byte (10000 = 10 MHz or 20000 = 20 MHz)
constexpr int CAMREG_SETUP_VALUE_L = 0x1A;    ///< sensor/ADC setup word LE16 low byte
constexpr int CAMREG_SETUP_VALUE_H = 0x1B;    ///< setup word LE16 high byte
constexpr int CAMREG_ROI_X_START_L = 0x1C;    ///< readout window X start LE16 low byte
constexpr int CAMREG_ROI_X_START_H = 0x1D;    ///< readout window X start LE16 high byte
constexpr int CAMREG_ROI_Y_START_L = 0x1E;    ///< readout window Y start LE16 low byte
constexpr int CAMREG_ROI_Y_START_H = 0x1F;    ///< readout window Y start LE16 high byte
constexpr int CAMREG_ROI_X_END_L = 0x20;      ///< readout window X end (exclusive) LE16
constexpr int CAMREG_ROI_X_END_H = 0x21;      ///< = x_start + binned_width * x_bin
constexpr int CAMREG_ROI_Y_END_L = 0x22;      ///< readout window Y end (exclusive) LE16
constexpr int CAMREG_ROI_Y_END_H = 0x23;      ///< = y_start + binned_height * y_bin
constexpr int CAMREG_ADC_OFFSET = 0x26;       ///< ADC black-level offset
constexpr int CAMREG_STATUS = 0x2A;           ///< read-only status; see CAMREG_STATUS_*
constexpr int CAMREG_TRIGGER_MODE = 0x2C;     ///< 0x00 = free-run, 0x31 = external trigger, single frame

/// STATUS register bits.
constexpr int CAMREG_STATUS_FRAME_READY = 0x04; ///< bit 2: a frame is ready for readout

/// TRIGGER_MODE values.
constexpr int CAMREG_TRIGGER_FREE_RUN = 0x00;
constexpr int CAMREG_TRIGGER_EXTERNAL = 0x31;

/// Compose a 16-bit value from a register pair (pass the two register bytes).
#define CAMREG_LE16(lo, hi) ((unsigned) (lo) | ((unsigned) (hi) << 8))

#endif // PHOTONIC_CAMERA_REGS_H
