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
/// Camera-head controller register file (I2C device CAMREG_I2C_DEV_ADDR).  See
/// i2c.h; the register map is shared with the test program in
/// common/camera_regs.h and specified in docs/camera-head-registers.md.

#include "i2c.h"

#include "dcam_internal.h"
#include "log.h"
#include "trigger.h"

#include "../common/camera_regs.h"

#include <string.h>

const char *dcam_i2c_reg_name(uint32_t reg) {
    switch (reg) {
        case CAMREG_VIDEO_GAIN:
            return "VIDEO_GAIN";
        case CAMREG_INTENSIFIER_GAIN:
            return "INTENSIFIER_GAIN";
        case CAMREG_EXPOSURE_UNITS:
            return "EXPOSURE_UNITS";
        case CAMREG_EXPOSURE_TIME_L:
            return "EXPOSURE_TIME_L";
        case CAMREG_EXPOSURE_TIME_H:
            return "EXPOSURE_TIME_H";
        case CAMREG_X_BINNING:
            return "X_BINNING";
        case CAMREG_Y_BINNING:
            return "Y_BINNING";
        case CAMREG_SPEED_L:
            return "SPEED_L";
        case CAMREG_SPEED_H:
            return "SPEED_H";
        case CAMREG_SETUP_VALUE_L:
            return "SETUP_VALUE_L";
        case CAMREG_SETUP_VALUE_H:
            return "SETUP_VALUE_H";
        case CAMREG_ROI_X_START_L:
            return "ROI_X_START_L";
        case CAMREG_ROI_X_START_H:
            return "ROI_X_START_H";
        case CAMREG_ROI_Y_START_L:
            return "ROI_Y_START_L";
        case CAMREG_ROI_Y_START_H:
            return "ROI_Y_START_H";
        case CAMREG_ROI_X_END_L:
            return "ROI_X_END_L";
        case CAMREG_ROI_X_END_H:
            return "ROI_X_END_H";
        case CAMREG_ROI_Y_END_L:
            return "ROI_Y_END_L";
        case CAMREG_ROI_Y_END_H:
            return "ROI_Y_END_H";
        case CAMREG_ADC_OFFSET:
            return "ADC_OFFSET";
        case CAMREG_STATUS:
            return "STATUS";
        case CAMREG_TRIGGER_MODE:
            return "TRIGGER_MODE";
        default:
            return NULL;
    }
}

void dcam_i2c_reset(dcam_camera_t *cam) {
    memset(cam->i2c_regs, 0, sizeof(cam->i2c_regs));
    cam->i2c_regs[CAMREG_VIDEO_GAIN] = 1;
    cam->i2c_regs[CAMREG_INTENSIFIER_GAIN] = 1;
    cam->i2c_regs[CAMREG_X_BINNING] = 1;
    cam->i2c_regs[CAMREG_Y_BINNING] = 1;
}

uint8_t dcam_i2c_status(const dcam_camera_t *cam) {
    if (cam->i2c_regs[CAMREG_TRIGGER_MODE] == CAMREG_TRIGGER_EXTERNAL) {
        // Externally triggered: a frame is ready only after a trigger pulse
        // fired and before its frame has been read out (trigger.c counts the
        // pulses, the iso transmit path consumes them).
        return dcam_trigger_pending(cam) > 0 ? CAMREG_STATUS_FRAME_READY : 0;
    }
    // Free-run: the fake renders frames continuously while the isochronous
    // stream runs, so a frame is ready exactly when streaming.
    return cam->iso_running ? CAMREG_STATUS_FRAME_READY : 0;
}

uint8_t dcam_i2c_read_reg(const dcam_camera_t *cam, uint32_t reg) {
    if (reg == CAMREG_STATUS) {
        return dcam_i2c_status(cam);
    }
    if (reg < CAMREG_COUNT) {
        return cam->i2c_regs[reg];
    }
    return 0;
}

void dcam_i2c_write_reg(dcam_camera_t *cam, uint32_t reg, uint8_t value) {
    const char *name = dcam_i2c_reg_name(reg);

    if (reg == CAMREG_STATUS || reg >= CAMREG_COUNT) {
        LOG(WARN, "[i2c 0x%02x] write reg 0x%02x (%s) <- 0x%02x ignored (%s)", CAMREG_I2C_DEV_ADDR, reg,
            name != NULL ? name : "?", value, reg == CAMREG_STATUS ? "read-only" : "out of range");
        return;
    }
    cam->i2c_regs[reg] = value;
    LOG(TRACE, "[i2c 0x%02x] reg 0x%02x (%s) <- 0x%02x", CAMREG_I2C_DEV_ADDR, reg, name != NULL ? name : "?", value);

    // Leaving external trigger mode cancels any armed-but-unread exposure.
    if (reg == CAMREG_TRIGGER_MODE && value != CAMREG_TRIGGER_EXTERNAL) {
        dcam_trigger_disarm(cam);
    }
}
