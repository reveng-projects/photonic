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
/// End-to-end triggered acquisition through the DLL API:
///
///   1. Initialize the library: enumerate, open the camera, select pixel
///      format 5 (Mono16) and verify the read-back.
///   2. Program the camera-head controller over the external I2C gate
///      (device CAMREG_I2C_DEV_ADDR): calibration, readout speed, readout
///      window, binning, exposure, intensifier gain, trigger mode.
///   3. Start the video stream and flush any exposure already latched
///      (drain frames while the STATUS register reports FRAME_READY).
///   4. Fire the exposure trigger: a DTR pulse on the serial port wired to
///      the camera trigger input (--com-port).
///   5. Poll the STATUS register until FRAME_READY, fetch the frame with
///      PlReturnVideoData, and apply the 12->16-bit stretch.
///   6. Verdict: the delivered frame must contain non-black pixel data.
///
/// Without --com-port nothing can fire an external-trigger exposure, so the
/// head is programmed to free-run (TRIGGER_MODE 0x00 instead of 0x31) and
/// every other step of the sequence is unchanged.
///
/// The register defaults and the head-A calibration table are shared with the
/// IOCTL suite (ioctl/i2c.cpp).

#include <cstring>
#include <vector>

#include "framework.h"

#include "../../common/camera_regs.h" // camera-head register map
#include "../ioctl/i2c.h"             // CamCalibHeadA + CAMDEF_* defaults

/// DCAM pixel-format code for 16-bit mono frames.
static const ULONG PIXEL_FORMAT_MONO16 = 5;

/// Operation timeout programmed alongside the exposure: long enough that a
/// slow externally-triggered exposure cannot time out the frame fetch.
static const ULONG SNAP_TIMEOUT_MS = 14400000; // 4 hours

/// STATUS polling cadence while waiting for the triggered exposure.
static const int STATUS_POLL_MS = 100;
static const int STATUS_POLL_TOTAL_MS = 5000;

/// Bound for the flush drain loop.
static const int FLUSH_DRAIN_LIMIT = 8;

/// One camera-head register write through the external I2C gate.
static bool CamHeadWrite(HANDLE cam, UINT8 reg, UINT8 value) {
    PL_RETURN_CODE rc = g_api.PlWriteExtI2cRegister(cam, CAMREG_I2C_DEV_ADDR, reg, value);
    if (rc != PL_SUCCESS) {
        Log(ERROR, L"head register 0x%02X <- 0x%02X failed: %s (%d)", reg, value, RcName(rc), rc);
        return false;
    }
    return true;
}

/// Read the STATUS register; the FRAME_READY bit reports a latched exposure.
static bool CamHeadReadStatus(HANDLE cam, ULONG *status) {
    PL_RETURN_CODE rc = g_api.PlReadExtI2cRegister(cam, CAMREG_I2C_DEV_ADDR, CAMREG_STATUS, status);
    if (rc != PL_SUCCESS) {
        Log(ERROR, L"STATUS register read failed: %s (%d)", RcName(rc), rc);
        return false;
    }
    return true;
}

/// Program the speed/binning-dependent calibration pair: the sensor/ADC setup
/// word (regs 0x1A/0x1B) and the ADC black-level offset (reg 0x26).
static bool WriteCalibration(HANDLE cam, UINT16 setup, UINT8 offset) {
    if (!CamHeadWrite(cam, CAMREG_SETUP_VALUE_L, (UINT8) (setup & 0xFF))) {
        Log(ERROR, L"calibration: SETUP_VALUE_L write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_SETUP_VALUE_H, (UINT8) (setup >> 8))) {
        Log(ERROR, L"calibration: SETUP_VALUE_H write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ADC_OFFSET, offset)) {
        Log(ERROR, L"calibration: ADC_OFFSET write failed");
        return false;
    }
    return true;
}

/// Program the readout speed: the calibration pair first (it is valid for
/// this speed), then the pixel clock low/high bytes.
static bool WriteSpeed(HANDLE cam, UINT16 speedKhz, UINT16 setup, UINT8 offset) {
    if (!WriteCalibration(cam, setup, offset)) {
        Log(ERROR, L"speed: calibration write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_SPEED_L, (UINT8) (speedKhz & 0xFF))) {
        Log(ERROR, L"speed: SPEED_L write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_SPEED_H, (UINT8) (speedKhz >> 8))) {
        Log(ERROR, L"speed: SPEED_H write failed");
        return false;
    }
    return true;
}

/// Program the full-frame readout window (X start, X end, Y start, Y end, in
/// that write order), the binning factors, then the calibration pair again
/// (SETUP_VALUE/ADC_OFFSET depend on the binning factor).
static bool SetSubareaAndBinning(HANDLE cam, UINT16 setup, UINT8 offset) {
    UINT16 xEnd = CAMDEF_SENSOR_WIDTH; // 0 + binned_width * binning
    UINT16 yEnd = CAMDEF_SENSOR_HEIGHT;
    if (!CamHeadWrite(cam, CAMREG_ROI_X_START_L, 0)) {
        Log(ERROR, L"subarea: ROI_X_START_L write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ROI_X_START_H, 0)) {
        Log(ERROR, L"subarea: ROI_X_START_H write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ROI_X_END_L, (UINT8) (xEnd & 0xFF))) {
        Log(ERROR, L"subarea: ROI_X_END_L write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ROI_X_END_H, (UINT8) (xEnd >> 8))) {
        Log(ERROR, L"subarea: ROI_X_END_H write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ROI_Y_START_L, 0)) {
        Log(ERROR, L"subarea: ROI_Y_START_L write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ROI_Y_START_H, 0)) {
        Log(ERROR, L"subarea: ROI_Y_START_H write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ROI_Y_END_L, (UINT8) (yEnd & 0xFF))) {
        Log(ERROR, L"subarea: ROI_Y_END_L write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_ROI_Y_END_H, (UINT8) (yEnd >> 8))) {
        Log(ERROR, L"subarea: ROI_Y_END_H write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_X_BINNING, CAMDEF_BINNING)) {
        Log(ERROR, L"subarea: X_BINNING write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_Y_BINNING, CAMDEF_BINNING)) {
        Log(ERROR, L"subarea: Y_BINNING write failed");
        return false;
    }
    if (!WriteCalibration(cam, setup, offset)) {
        Log(ERROR, L"subarea: calibration re-latch failed");
        return false;
    }
    return true;
}

/// Program the exposure: units, the 14-bit time low/high, then the DLL
/// operation timeout.
static bool WriteExposure(HANDLE cam, UINT8 units, UINT16 time) {
    if (!CamHeadWrite(cam, CAMREG_EXPOSURE_UNITS, units)) {
        Log(ERROR, L"exposure: EXPOSURE_UNITS write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_EXPOSURE_TIME_L, (UINT8) (time & 0xFF))) {
        Log(ERROR, L"exposure: EXPOSURE_TIME_L write failed");
        return false;
    }
    if (!CamHeadWrite(cam, CAMREG_EXPOSURE_TIME_H, (UINT8) (time >> 8))) {
        Log(ERROR, L"exposure: EXPOSURE_TIME_H write failed");
        return false;
    }
    PL_RETURN_CODE rc = g_api.PlSetTimeout(cam, SNAP_TIMEOUT_MS);
    if (rc != PL_SUCCESS) {
        Log(ERROR, L"PlSetTimeout(%lu) failed: %s (%d)", SNAP_TIMEOUT_MS, RcName(rc), rc);
        return false;
    }
    return true;
}

/// Start the stream, settle, and drain frames while STATUS reads FRAME_READY,
/// so a stale exposure cannot be handed out as the triggered snap. The
/// stream is left running for the snap.
static bool FlushPipeline(HANDLE cam, ULONG frameBytes, BYTE *frame) {
    PL_RETURN_CODE rc = g_api.PlStartVideoStream(cam);
    if (rc != PL_SUCCESS) {
        Log(ERROR, L"flush: PlStartVideoStream -> %s (%d)", RcName(rc), rc);
        return false;
    }
    for (int pass = 0; pass < 2; pass++) {
        Sleep(250);
        ULONG status = 0;
        int drained = 0;
        while (CamHeadReadStatus(cam, &status) && (status & CAMREG_STATUS_FRAME_READY) != 0) {
            if (drained >= FLUSH_DRAIN_LIMIT) {
                Log(WARN, L"flush: STATUS still FRAME_READY after draining %d frames", drained);
                break;
            }
            rc = g_api.PlReturnVideoData(cam, frameBytes, frame);
            if (rc != PL_SUCCESS) {
                Log(ERROR, L"flush: PlReturnVideoData -> %s (%d)", RcName(rc), rc);
                return false;
            }
            drained++;
        }
        if (drained > 0) {
            Log(INFO, L"flush pass %d: drained %d stale frame(s)", pass + 1, drained);
        }
    }
    return true;
}

/// Open the trigger serial port and park the DTR line low. The \\.\ prefix
/// is required for COM10 and above and harmless for COM1..COM9.
static HANDLE OpenTriggerPort(const std::string &port) {
    std::string path = port.compare(0, 4, "\\\\.\\") == 0 ? port : "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(ERROR, L"cannot open trigger port %hs (err=%lu)", port.c_str(), GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    EscapeCommFunction(h, CLRDTR);
    return h;
}

/// One DTR pulse: the out-of-band exposure trigger.
static void PulseTrigger(HANDLE comPort) {
    EscapeCommFunction(comPort, SETDTR);
    EscapeCommFunction(comPort, CLRDTR);
}

PL_TEST(acquisition, EndToEndSnap) {
    // Out-of-band trigger wiring; without it the head runs free (see the
    // file header).
    HANDLE comPort = INVALID_HANDLE_VALUE;
    if (!g_opt.comPort.empty()) {
        comPort = OpenTriggerPort(g_opt.comPort);
    }
    bool externalTrigger = comPort != INVALID_HANDLE_VALUE;

    // Library initialization: enumerate, then open the configured camera.
    ULONG count = 0;
    const char *filter = g_opt.filter.c_str();
    PL_RETURN_CODE rc = g_api.PlGetNumberDevices(filter, &count, filter, &count);
    CheckRc(rc, PL_SUCCESS, L"PlGetNumberDevices");
    Log(INFO, L"device count: %lu", count);

    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        if (comPort != INVALID_HANDLE_VALUE) {
            CloseHandle(comPort);
        }
        return;
    }

    // Pixel format 5 (Mono16) with read-back verification; everything below
    // sizes the frame as 16 bits per pixel.
    rc = g_api.PlSetPixelFormat(cam, PIXEL_FORMAT_MONO16);
    CheckRc(rc, PL_SUCCESS, L"PlSetPixelFormat(5 = Mono16)");
    ULONG pf = 0;
    rc = g_api.PlGetPixelFormat(cam, &pf);
    Check(rc == PL_SUCCESS && pf == PIXEL_FORMAT_MONO16, L"PlGetPixelFormat read back 5 (got %lu)", pf);

    // Head-A calibration for 10 MHz at the default binning (ioctl/i2c.cpp).
    UINT16 setup = CamCalibHeadA.SetupValue[CAMDEF_BINNING];
    UINT8 offset = CamCalibHeadA.AdcOffset[CAMDEF_BINNING];

    // Program the camera head over the external I2C gate: speed with its
    // calibration, then the readout window and binning (twice; the second
    // pass re-latches the geometry-dependent calibration), exposure and
    // intensifier gain.
    Check(WriteSpeed(cam, CamCalibHeadA.SpeedKhz, setup, offset), L"WriteSpeed(10000 kHz)");
    Check(SetSubareaAndBinning(cam, setup, offset), L"SetSubareaAndBinning (1st)");
    Check(SetSubareaAndBinning(cam, setup, offset), L"SetSubareaAndBinning (2nd)");
    Check(WriteExposure(cam, CAMDEF_EXPOSURE_UNITS, CAMDEF_EXPOSURE_TIME), L"WriteExposure(units=ms, time=100)");
    Check(CamHeadWrite(cam, CAMREG_INTENSIFIER_GAIN, CAMDEF_GAIN), L"WriteIntensifierGain(1)");

    // Trigger mode: external single-frame when the trigger port is wired,
    // free-run otherwise.
    UINT8 triggerMode = externalTrigger ? CAMREG_TRIGGER_EXTERNAL : CAMREG_TRIGGER_FREE_RUN;
    Check(CamHeadWrite(cam, CAMREG_TRIGGER_MODE, triggerMode), L"SetTriggerMode(0x%02X)", triggerMode);
    if (!externalTrigger) {
        Log(WARN, L"no --com-port: head left in free-run instead of external trigger (0x31).");
    }

    // Frame geometry as the API reports it, so the byte count passed to
    // PlReturnVideoData matches the frame exactly (the copy has no clamp,
    // see stream.cpp).
    ULONG decimX = 0, decimY = 0, offX = 0, offY = 0, width = 0, height = 0;
    rc = g_api.PlGetSubWindowSettings(cam, &decimX, &decimY, &offX, &offY, &width, &height);
    Check(rc == PL_SUCCESS && width == CAMDEF_SENSOR_WIDTH && height == CAMDEF_SENSOR_HEIGHT,
          L"geometry %lux%lu (expected %ux%u)", width, height, CAMDEF_SENSOR_WIDTH, CAMDEF_SENSOR_HEIGHT);
    if (rc != PL_SUCCESS || width == 0 || height == 0) {
        if (comPort != INVALID_HANDLE_VALUE) {
            CloseHandle(comPort);
        }
        return; // no usable frame size; the snap below would fault
    }
    ULONG frameBytes = width * height * 2; // Mono16
    std::vector<BYTE> frame(frameBytes);

    // Start the stream and drain any exposure already latched, leaving the
    // stream running for the snap.
    Check(FlushPipeline(cam, frameBytes, frame.data()), L"pipeline flush (PlStartVideoStream + drain)");

    // Fire one shot.
    if (externalTrigger) {
        PulseTrigger(comPort);
        Log(INFO, L"DTR trigger pulse sent on %hs", g_opt.comPort.c_str());
    }

    // Poll STATUS until the exposure is latched.  The poll is informational
    // (the fetch below blocks until the frame arrives); it just avoids
    // sitting inside PlReturnVideoData while the exposure runs.
    ULONG status = 0;
    int waited = 0;
    while (waited < STATUS_POLL_TOTAL_MS) {
        if (!CamHeadReadStatus(cam, &status)) {
            break;
        }
        if ((status & CAMREG_STATUS_FRAME_READY) != 0) {
            break;
        }
        Sleep(STATUS_POLL_MS);
        waited += STATUS_POLL_MS;
    }
    Log(INFO, L"final snap status 0x%02lX after %d ms%s", status, waited,
        (status & CAMREG_STATUS_FRAME_READY) != 0 ? L" (FRAME_READY)" : L"");

    // Fetch the frame from the running stream.  Zero-fill first so an
    // undelivered frame reads all-black and fails the content check below.
    memset(frame.data(), 0, frame.size());
    rc = g_api.PlReturnVideoData(cam, frameBytes, frame.data());
    CheckRc(rc, PL_SUCCESS, L"PlReturnVideoData");

    // The frame must carry image data: fail only if every raw byte is 0,
    // matching the all-black judgement the other suites use.
    int maxByte = 0;
    for (ULONG i = 0; i < frameBytes; i++) {
        if (frame[i] > maxByte) {
            maxByte = frame[i];
        }
    }
    Check(maxByte > 0, L"frame not all-black (px max byte %d)", maxByte);

    // 12->16-bit stretch: v = (v >> 12) + v * 16.
    USHORT *pixels = (USHORT *) frame.data();
    ULONG pixelCount = frameBytes / 2;
    for (ULONG i = 0; i < pixelCount; i++) {
        pixels[i] = (USHORT) ((pixels[i] >> 12) + pixels[i] * 16);
    }

    // Pixel statistics over the stretched buffer (informational).
    ULONG mn = 0xFFFF, mx = 0, nonzero = 0;
    double mean = 0.0;
    for (ULONG i = 0; i < pixelCount; i++) {
        USHORT v = pixels[i];
        if (v < mn) {
            mn = v;
        }
        if (v > mx) {
            mx = v;
        }
        if (v != 0) {
            nonzero++;
        }
        mean += v;
    }
    mean = pixelCount != 0 ? mean / pixelCount : 0.0;
    Log(INFO, L"u16 stats after stretch: min %lu max %lu mean %.3f nonzero %lu/%lu", mn, mx, mean, nonzero, pixelCount);

    // Teardown: stop the stream and put the head back into free-run so later
    // cases are not left waiting on a trigger that never comes.
    Report(g_api.PlStopVideoStream(cam), L"PlStopVideoStream");
    CamHeadWrite(cam, CAMREG_TRIGGER_MODE, CAMREG_TRIGGER_FREE_RUN);
    if (comPort != INVALID_HANDLE_VALUE) {
        CloseHandle(comPort);
    }
}
