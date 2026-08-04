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
/// Getters/setters for the camera-head controller registers (the I2C device at
/// CAMREG_I2C_DEV_ADDR behind the FireWire interface), implemented in i2c.cpp
/// on top of the PHOTONIC_IOCTL_MAILBOX command gate.  Register map:
/// common/camera_regs.h; full specification: docs/camera-head-registers.md.
///
/// All functions take an already-opened camera handle
/// (CreateFileA("\\\\.\\vitdcamN", ...)) and return the DeviceIoControl BOOL
/// (TRUE on success).  Getters leave the out parameter untouched on failure;
/// a NULL out parameter discards the value read.

#pragma once

#include <windows.h>

/// Generic single-register read (8-bit register address / 8-bit data).
///
/// @param hCamera  Opened camera handle.
/// @param reg      Register address to read.
/// @param value    Receives the register value read.  May be NULL to discard.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegRead(HANDLE hCamera, UINT8 reg, UINT8 *value);
/// Generic single-register write (8-bit register address / 8-bit data).
///
/// @param hCamera  Opened camera handle.
/// @param reg      Register address to write.
/// @param value    Value to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegWrite(HANDLE hCamera, UINT8 reg, UINT8 value);

/// LE16 pair read: regLo addresses the low byte, regLo + 1 the high byte.
/// The pair has no atomic latch (two transactions per call).
///
/// @param hCamera  Opened camera handle.
/// @param regLo    Address of the low byte of the LE16 pair.
/// @param value    Receives the 16-bit value.  May be NULL to discard.
/// @return TRUE on success, FALSE if either transaction fails.
BOOL CamRegRead16(HANDLE hCamera, UINT8 regLo, UINT16 *value);
/// LE16 pair write: regLo addresses the low byte, regLo + 1 the high byte.
/// The pair has no atomic latch (two transactions per call).
///
/// @param hCamera  Opened camera handle.
/// @param regLo    Address of the low byte of the LE16 pair.
/// @param value    16-bit value to write.
/// @return TRUE on success, FALSE if either transaction fails.
BOOL CamRegWrite16(HANDLE hCamera, UINT8 regLo, UINT16 value);

/// Read the video/ADC gain register (1..100).
///
/// @param hCamera  Opened camera handle.
/// @param gain     Receives the gain value.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetVideoGain(HANDLE hCamera, UINT8 *gain);
/// Write the video/ADC gain register (1..100).
///
/// @param hCamera  Opened camera handle.
/// @param gain     Gain value to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetVideoGain(HANDLE hCamera, UINT8 gain);

/// Read the image-intensifier gain register (1..100).
///
/// @param hCamera  Opened camera handle.
/// @param gain     Receives the gain value.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetIntensifierGain(HANDLE hCamera, UINT8 *gain);
/// Write the image-intensifier gain register (1..100).
///
/// @param hCamera  Opened camera handle.
/// @param gain     Gain value to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetIntensifierGain(HANDLE hCamera, UINT8 gain);

/// Read the exposure time base register (0 = us, 1 = ms, 2 = s).
///
/// @param hCamera  Opened camera handle.
/// @param units    Receives the exposure-unit code.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetExposureUnits(HANDLE hCamera, UINT8 *units);
/// Write the exposure time base register (0 = us, 1 = ms, 2 = s).
///
/// @param hCamera  Opened camera handle.
/// @param units    Exposure-unit code to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetExposureUnits(HANDLE hCamera, UINT8 units);

/// Read the exposure time register, 14-bit (1..0x3FFF), in EXPOSURE_UNITS.
///
/// @param hCamera  Opened camera handle.
/// @param time     Receives the exposure time.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetExposureTime(HANDLE hCamera, UINT16 *time);
/// Write the exposure time register, 14-bit (1..0x3FFF), in EXPOSURE_UNITS.
///
/// @param hCamera  Opened camera handle.
/// @param time     Exposure time to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetExposureTime(HANDLE hCamera, UINT16 time);

/// Read the horizontal binning factor register (1..63).
///
/// @param hCamera  Opened camera handle.
/// @param factor   Receives the binning factor.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetXBinning(HANDLE hCamera, UINT8 *factor);
/// Write the horizontal binning factor register (1..63).
///
/// @param hCamera  Opened camera handle.
/// @param factor   Binning factor to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetXBinning(HANDLE hCamera, UINT8 factor);
/// Read the vertical binning factor register (1..63).
///
/// @param hCamera  Opened camera handle.
/// @param factor   Receives the binning factor.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetYBinning(HANDLE hCamera, UINT8 *factor);
/// Write the vertical binning factor register (1..63).
///
/// @param hCamera  Opened camera handle.
/// @param factor   Binning factor to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetYBinning(HANDLE hCamera, UINT8 factor);

/// Read the readout pixel clock register, in kHz (10000 = 10 MHz, 20000 = 20 MHz).
///
/// @param hCamera   Opened camera handle.
/// @param speedKhz  Receives the pixel clock in kHz.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetSpeed(HANDLE hCamera, UINT16 *speedKhz);
/// Write the readout pixel clock register, in kHz (10000 = 10 MHz, 20000 = 20 MHz).
///
/// @param hCamera   Opened camera handle.
/// @param speedKhz  Pixel clock in kHz to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetSpeed(HANDLE hCamera, UINT16 speedKhz);

/// Read the sensor/ADC setup (calibration) word.
///
/// @param hCamera  Opened camera handle.
/// @param setup    Receives the calibration word.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetSetupValue(HANDLE hCamera, UINT16 *setup);
/// Write the sensor/ADC setup (calibration) word.
///
/// @param hCamera  Opened camera handle.
/// @param setup    Calibration word to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetSetupValue(HANDLE hCamera, UINT16 setup);

/// Read the readout window in unbinned sensor pixels; end coordinates are
/// exclusive (end = start + binned_size * binning).  Any out parameter may be
/// NULL.
///
/// @param hCamera  Opened camera handle.
/// @param xStart   Receives the left edge in unbinned pixels.  May be NULL.
/// @param yStart   Receives the top edge in unbinned pixels.  May be NULL.
/// @param xEnd     Receives the right edge (exclusive) in unbinned pixels.  May be NULL.
/// @param yEnd     Receives the bottom edge (exclusive) in unbinned pixels.  May be NULL.
/// @return TRUE on success, FALSE if any register read fails.
BOOL CamRegGetRoi(HANDLE hCamera, UINT16 *xStart, UINT16 *yStart, UINT16 *xEnd, UINT16 *yEnd);
/// Write the readout window in unbinned sensor pixels; end coordinates are
/// exclusive (end = start + binned_size * binning).  Programs all four LE16
/// pairs (eight transactions).
///
/// @param hCamera  Opened camera handle.
/// @param xStart   Left edge in unbinned pixels.
/// @param yStart   Top edge in unbinned pixels.
/// @param xEnd     Right edge (exclusive) in unbinned pixels.
/// @param yEnd     Bottom edge (exclusive) in unbinned pixels.
/// @return TRUE on success, FALSE if any register write fails.
BOOL CamRegSetRoi(HANDLE hCamera, UINT16 xStart, UINT16 yStart, UINT16 xEnd, UINT16 yEnd);

/// Read the ADC black-level offset register.
///
/// @param hCamera  Opened camera handle.
/// @param offset   Receives the ADC offset value.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetAdcOffset(HANDLE hCamera, UINT8 *offset);
/// Write the ADC black-level offset register.
///
/// @param hCamera  Opened camera handle.
/// @param offset   ADC offset value to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetAdcOffset(HANDLE hCamera, UINT8 offset);

/// Read-only status register (CAMREG_STATUS_* bits).
///
/// @param hCamera  Opened camera handle.
/// @param status   Receives the status register bitmask.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetStatus(HANDLE hCamera, UINT8 *status);

/// Read the acquisition mode register (CAMREG_TRIGGER_FREE_RUN or CAMREG_TRIGGER_EXTERNAL).
///
/// @param hCamera  Opened camera handle.
/// @param mode     Receives the trigger mode code.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegGetTriggerMode(HANDLE hCamera, UINT8 *mode);
/// Write the acquisition mode register (CAMREG_TRIGGER_FREE_RUN or CAMREG_TRIGGER_EXTERNAL).
///
/// @param hCamera  Opened camera handle.
/// @param mode     Trigger mode code to write.
/// @return TRUE on success, FALSE on IOCTL failure.
BOOL CamRegSetTriggerMode(HANDLE hCamera, UINT8 mode);

/// Per-head calibration tables from the vendor's ps_setup.dat.  SETUP_VALUE
/// and ADC_OFFSET depend on the readout speed and the binning factor; the
/// vendor file only provides tables for 10 MHz, binning factors 1..8.
/// Index the arrays with the binning factor ([0] is unused).
constexpr int CAMCAL_MAX_BINNING = 8;

typedef struct _CAMERA_HEAD_CALIBRATION {
    UINT16 SpeedKhz;                           ///< readout speed the tables are valid for
    UINT16 SetupValue[CAMCAL_MAX_BINNING + 1]; ///< regs 0x1A/0x1B per binning factor
    UINT8 AdcOffset[CAMCAL_MAX_BINNING + 1];   ///< reg 0x26 per binning factor
} CAMERA_HEAD_CALIBRATION;

/// The DualFDI system's two camera heads ("A" and "B").
extern const CAMERA_HEAD_CALIBRATION CamCalibHeadA;
extern const CAMERA_HEAD_CALIBRATION CamCalibHeadB;

/// Operational defaults programmed by CamRegInitDefaults: 10 MHz readout,
/// full 1388x1032 frame at 1x1 binning, 100 ms exposure, both gains 1.
/// SETUP_VALUE and ADC_OFFSET come from head A's calibration table.
constexpr int CAMDEF_SENSOR_WIDTH = 1388;
constexpr int CAMDEF_SENSOR_HEIGHT = 1032;
constexpr int CAMDEF_BINNING = 1;
constexpr int CAMDEF_EXPOSURE_UNITS = 1;  ///< milliseconds
constexpr int CAMDEF_EXPOSURE_TIME = 100; ///< 100 ms
constexpr int CAMDEF_GAIN = 1;

/// Program every writable register to its operational default (10 MHz,
/// full-frame ROI at 1x1 binning, 100 ms exposure, gains 1, free-run trigger).
/// Logs and returns FALSE on the first failed write.
///
/// @param hCamera  Opened camera handle.
/// @return TRUE on success, FALSE on the first failed write.
BOOL CamRegInitDefaults(HANDLE hCamera);

/// Read the whole register file and write one decoded Log() line per register
/// (LE16 pairs combined).  Returns FALSE as soon as a register read fails.
///
/// @param hCamera  Opened camera handle.
/// @return TRUE on success, FALSE on the first failed read.
BOOL CamDumpRegisters(HANDLE hCamera);
