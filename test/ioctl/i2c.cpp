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
/// Getters/setters for the camera-head controller registers (see i2c.h).
///
/// Both directions go through PHOTONIC_IOCTL_MAILBOX raw packets: mailbox command 0x1009
/// writes one register, command 0x1008 reads one back (packet layouts in
/// photonic/ioctl.h, protocol specification in docs/camera-head-registers.md).
/// The driver handles the DWORD byte-swap to 1394 bus order, so the structs
/// are filled in logical byte order here.

#include "i2c.h"

#include "ioctl.h"

#include "../utils/log.h"

#include "../../common/camera_regs.h"

BOOL CamRegRead(HANDLE hCamera, UINT8 reg, UINT8 *value) {
    PHOTONIC_MAILBOX_PKT1008_IN in = {0};
    PHOTONIC_MAILBOX_PKT1008_OUT out = {0};
    in.Command = MAILBOX_CMD_IMAGER_READ;
    in.DeviceAddr = CAMREG_I2C_DEV_ADDR;
    in.SubCommand = reg;
    in.Mode = 1;
    if (!PhotonicIoctlMailbox(hCamera, &in, sizeof(in), &out, sizeof(out))) {
        return FALSE;
    }
    if (value) {
        *value = out.Value;
    }
    return TRUE;
}

BOOL CamRegWrite(HANDLE hCamera, UINT8 reg, UINT8 value) {
    PHOTONIC_MAILBOX_PKT1009_IN in = {0};
    PHOTONIC_MAILBOX_PKT_OUT8 out = {0};
    in.Command = MAILBOX_CMD_IMAGER_CONFIG;
    in.DeviceAddr = CAMREG_I2C_DEV_ADDR;
    in.SubCommand = reg;
    in.Mode = 1;
    in.Value = value;
    return PhotonicIoctlMailbox(hCamera, &in, sizeof(in), &out, sizeof(out));
}

BOOL CamRegRead16(HANDLE hCamera, UINT8 regLo, UINT16 *value) {
    UINT8 lo, hi;
    if (!CamRegRead(hCamera, regLo, &lo) || !CamRegRead(hCamera, (UINT8) (regLo + 1), &hi)) {
        return FALSE;
    }
    if (value) {
        *value = (UINT16) CAMREG_LE16(lo, hi);
    }
    return TRUE;
}

BOOL CamRegWrite16(HANDLE hCamera, UINT8 regLo, UINT16 value) {
    return CamRegWrite(hCamera, regLo, (UINT8) (value & 0xFF)) &&
           CamRegWrite(hCamera, (UINT8) (regLo + 1), (UINT8) (value >> 8));
}

BOOL CamRegGetVideoGain(HANDLE hCamera, UINT8 *gain) {
    return CamRegRead(hCamera, CAMREG_VIDEO_GAIN, gain);
}

BOOL CamRegSetVideoGain(HANDLE hCamera, UINT8 gain) {
    return CamRegWrite(hCamera, CAMREG_VIDEO_GAIN, gain);
}

BOOL CamRegGetIntensifierGain(HANDLE hCamera, UINT8 *gain) {
    return CamRegRead(hCamera, CAMREG_INTENSIFIER_GAIN, gain);
}

BOOL CamRegSetIntensifierGain(HANDLE hCamera, UINT8 gain) {
    return CamRegWrite(hCamera, CAMREG_INTENSIFIER_GAIN, gain);
}

BOOL CamRegGetExposureUnits(HANDLE hCamera, UINT8 *units) {
    return CamRegRead(hCamera, CAMREG_EXPOSURE_UNITS, units);
}

BOOL CamRegSetExposureUnits(HANDLE hCamera, UINT8 units) {
    return CamRegWrite(hCamera, CAMREG_EXPOSURE_UNITS, units);
}

BOOL CamRegGetExposureTime(HANDLE hCamera, UINT16 *time) {
    return CamRegRead16(hCamera, CAMREG_EXPOSURE_TIME_L, time);
}

BOOL CamRegSetExposureTime(HANDLE hCamera, UINT16 time) {
    return CamRegWrite16(hCamera, CAMREG_EXPOSURE_TIME_L, time);
}

BOOL CamRegGetXBinning(HANDLE hCamera, UINT8 *factor) {
    return CamRegRead(hCamera, CAMREG_X_BINNING, factor);
}

BOOL CamRegSetXBinning(HANDLE hCamera, UINT8 factor) {
    return CamRegWrite(hCamera, CAMREG_X_BINNING, factor);
}

BOOL CamRegGetYBinning(HANDLE hCamera, UINT8 *factor) {
    return CamRegRead(hCamera, CAMREG_Y_BINNING, factor);
}

BOOL CamRegSetYBinning(HANDLE hCamera, UINT8 factor) {
    return CamRegWrite(hCamera, CAMREG_Y_BINNING, factor);
}

BOOL CamRegGetSpeed(HANDLE hCamera, UINT16 *speedKhz) {
    return CamRegRead16(hCamera, CAMREG_SPEED_L, speedKhz);
}

BOOL CamRegSetSpeed(HANDLE hCamera, UINT16 speedKhz) {
    return CamRegWrite16(hCamera, CAMREG_SPEED_L, speedKhz);
}

BOOL CamRegGetSetupValue(HANDLE hCamera, UINT16 *setup) {
    return CamRegRead16(hCamera, CAMREG_SETUP_VALUE_L, setup);
}

BOOL CamRegSetSetupValue(HANDLE hCamera, UINT16 setup) {
    return CamRegWrite16(hCamera, CAMREG_SETUP_VALUE_L, setup);
}

BOOL CamRegGetRoi(HANDLE hCamera, UINT16 *xStart, UINT16 *yStart, UINT16 *xEnd, UINT16 *yEnd) {
    return CamRegRead16(hCamera, CAMREG_ROI_X_START_L, xStart) && CamRegRead16(hCamera, CAMREG_ROI_Y_START_L, yStart) &&
           CamRegRead16(hCamera, CAMREG_ROI_X_END_L, xEnd) && CamRegRead16(hCamera, CAMREG_ROI_Y_END_L, yEnd);
}

BOOL CamRegSetRoi(HANDLE hCamera, UINT16 xStart, UINT16 yStart, UINT16 xEnd, UINT16 yEnd) {
    return CamRegWrite16(hCamera, CAMREG_ROI_X_START_L, xStart) &&
           CamRegWrite16(hCamera, CAMREG_ROI_Y_START_L, yStart) && CamRegWrite16(hCamera, CAMREG_ROI_X_END_L, xEnd) &&
           CamRegWrite16(hCamera, CAMREG_ROI_Y_END_L, yEnd);
}

BOOL CamRegGetAdcOffset(HANDLE hCamera, UINT8 *offset) {
    return CamRegRead(hCamera, CAMREG_ADC_OFFSET, offset);
}

BOOL CamRegSetAdcOffset(HANDLE hCamera, UINT8 offset) {
    return CamRegWrite(hCamera, CAMREG_ADC_OFFSET, offset);
}

BOOL CamRegGetStatus(HANDLE hCamera, UINT8 *status) {
    return CamRegRead(hCamera, CAMREG_STATUS, status);
}

BOOL CamRegGetTriggerMode(HANDLE hCamera, UINT8 *mode) {
    return CamRegRead(hCamera, CAMREG_TRIGGER_MODE, mode);
}

BOOL CamRegSetTriggerMode(HANDLE hCamera, UINT8 mode) {
    return CamRegWrite(hCamera, CAMREG_TRIGGER_MODE, mode);
}

/// Calibration tables from ps_setup.dat, [Dual FDI Firewire] "Laue 310192".
/// SetupValue[n] holds camera10binsetup{A,B}<n>; AdcOffset[n] holds
/// adc10binoffset{A,B}<n>, except [1] where the file has no per-bin key and
/// the whole-frame Offset10{A,B} value applies.
const CAMERA_HEAD_CALIBRATION CamCalibHeadA = {
    10000,                           // SpeedKhz
    {0, 19, 3, 3, 27, 11, 3, 3, 11}, // SetupValue, binning 1..8
    {0, 6, 6, 6, 6, 6, 6, 6, 6},     // AdcOffset,  binning 1..8
};

const CAMERA_HEAD_CALIBRATION CamCalibHeadB = {
    10000,                             // SpeedKhz
    {0, 33, 29, 25, 8, 5, 20, 20, 29}, // SetupValue, binning 1..8
    {0, 6, 6, 6, 6, 6, 6, 7, 6},       // AdcOffset,  binning 1..8
};

BOOL CamRegInitDefaults(HANDLE hCamera) {
    // Calibration first (it is speed/binning dependent), then speed, then
    // geometry (window before binning), then exposure, gains and trigger mode.
    if (!CamRegSetSetupValue(hCamera, CamCalibHeadA.SetupValue[CAMDEF_BINNING])) {
        Log(ERROR, L"camera-head init: setup value programming failed");
        return FALSE;
    }
    if (!CamRegSetAdcOffset(hCamera, CamCalibHeadA.AdcOffset[CAMDEF_BINNING])) {
        Log(ERROR, L"camera-head init: ADC offset programming failed");
        return FALSE;
    }
    if (!CamRegSetSpeed(hCamera, CamCalibHeadA.SpeedKhz)) {
        Log(ERROR, L"camera-head init: speed programming failed");
        return FALSE;
    }
    if (!CamRegSetRoi(hCamera, 0, 0, CAMDEF_SENSOR_WIDTH, CAMDEF_SENSOR_HEIGHT)) {
        Log(ERROR, L"camera-head init: ROI programming failed");
        return FALSE;
    }
    if (!CamRegSetXBinning(hCamera, CAMDEF_BINNING)) {
        Log(ERROR, L"camera-head init: X binning programming failed");
        return FALSE;
    }
    if (!CamRegSetYBinning(hCamera, CAMDEF_BINNING)) {
        Log(ERROR, L"camera-head init: Y binning programming failed");
        return FALSE;
    }
    if (!CamRegSetExposureUnits(hCamera, CAMDEF_EXPOSURE_UNITS)) {
        Log(ERROR, L"camera-head init: exposure units programming failed");
        return FALSE;
    }
    if (!CamRegSetExposureTime(hCamera, CAMDEF_EXPOSURE_TIME)) {
        Log(ERROR, L"camera-head init: exposure time programming failed");
        return FALSE;
    }
    if (!CamRegSetVideoGain(hCamera, CAMDEF_GAIN)) {
        Log(ERROR, L"camera-head init: video gain programming failed");
        return FALSE;
    }
    if (!CamRegSetIntensifierGain(hCamera, CAMDEF_GAIN)) {
        Log(ERROR, L"camera-head init: intensifier gain programming failed");
        return FALSE;
    }
    // Free-run so streaming tests get frames without an external trigger.
    if (!CamRegSetTriggerMode(hCamera, CAMREG_TRIGGER_FREE_RUN)) {
        Log(ERROR, L"camera-head init: trigger mode programming failed");
        return FALSE;
    }
    return TRUE;
}

BOOL CamDumpRegisters(HANDLE hCamera) {
    UINT8 regs[CAMREG_COUNT];
    for (UINT32 reg = 0; reg < CAMREG_COUNT; reg++) {
        if (!CamRegRead(hCamera, (UINT8) reg, &regs[reg])) {
            Log(ERROR, L"camera-head register 0x%02X read failed", reg);
            return FALSE;
        }
    }

    UINT8 status = regs[CAMREG_STATUS];
    UINT8 trigger = regs[CAMREG_TRIGGER_MODE];
    Log(INFO, L"camera-head registers (I2C device 0x%02X):", CAMREG_I2C_DEV_ADDR);
    Log(INFO, L"VIDEO_GAIN       = %u", regs[CAMREG_VIDEO_GAIN]);
    Log(INFO, L"INTENSIFIER_GAIN = %u", regs[CAMREG_INTENSIFIER_GAIN]);
    Log(INFO, L"EXPOSURE_UNITS   = %u (0=us, 1=ms, 2=s)", regs[CAMREG_EXPOSURE_UNITS]);
    Log(INFO, L"EXPOSURE_TIME    = %u", CAMREG_LE16(regs[CAMREG_EXPOSURE_TIME_L], regs[CAMREG_EXPOSURE_TIME_H]));
    Log(INFO, L"X_BINNING        = %u", regs[CAMREG_X_BINNING]);
    Log(INFO, L"Y_BINNING        = %u", regs[CAMREG_Y_BINNING]);
    Log(INFO, L"SPEED            = %u kHz", CAMREG_LE16(regs[CAMREG_SPEED_L], regs[CAMREG_SPEED_H]));
    Log(INFO, L"SETUP_VALUE      = 0x%04X", CAMREG_LE16(regs[CAMREG_SETUP_VALUE_L], regs[CAMREG_SETUP_VALUE_H]));
    Log(INFO, L"ROI              = x [%u..%u) y [%u..%u)",
        CAMREG_LE16(regs[CAMREG_ROI_X_START_L], regs[CAMREG_ROI_X_START_H]),
        CAMREG_LE16(regs[CAMREG_ROI_X_END_L], regs[CAMREG_ROI_X_END_H]),
        CAMREG_LE16(regs[CAMREG_ROI_Y_START_L], regs[CAMREG_ROI_Y_START_H]),
        CAMREG_LE16(regs[CAMREG_ROI_Y_END_L], regs[CAMREG_ROI_Y_END_H]));
    Log(INFO, L"ADC_OFFSET       = %u", regs[CAMREG_ADC_OFFSET]);
    Log(INFO, L"STATUS           = 0x%02X%s", status, (status & CAMREG_STATUS_FRAME_READY) ? L" (FRAME_READY)" : L"");
    Log(INFO, L"TRIGGER_MODE     = 0x%02X (%s)", trigger,
        trigger == CAMREG_TRIGGER_FREE_RUN   ? L"free-run"
        : trigger == CAMREG_TRIGGER_EXTERNAL ? L"external"
                                             : L"unknown");
    return TRUE;
}
