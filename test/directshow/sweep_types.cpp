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
/// Printing bodies for the DirectShow sweep's shared data types.  See
/// sweep_types.h.

#include "sweep_types.h"

#include <cwchar>

#include "../utils/log.h"

namespace dsweep {

void Combination::PrintHeader() const {
    Log(INFO, L"--- TEST: %s ---", label.c_str());
}

void ComboResult::PrintDiagnostics() const {
    std::wstring line;
    wchar_t buf[128];
    if (grabberActive) {
        swprintf(buf, _countof(buf), L"frames=%ld px[min/mean/max]=%d/%.0f/%d", frames, minByte, meanByte, maxByte);
        line = buf;
    } else {
        line = L"(no grabber: pixels not checked)";
    }
    if (propsExpected > 0) {
        swprintf(buf, _countof(buf), L" props=%d/%d", propsPassed, propsExpected);
        line += buf;
    }
    Log(INFO, L"%s", line.c_str());

    // The embedded-header diagnostics are meaningless without a header to
    // parse, so they are omitted entirely in real-camera mode.
    if (grabberActive && frameHeaderChecks) {
        swprintf(buf, _countof(buf),
                 L"frame-hdr: ok=%ld/%ld idx=[%ld..%ld] contiguous=%d badsum=%ld badsize=%ld badhdr=%ld "
                 L"badfeat=%ld",
                 meta.withHeader, frames, meta.firstIndex, meta.lastIndex, meta.indexContiguous ? 1 : 0,
                 meta.badChecksum, meta.badSize, meta.badHeader, meta.badFeatures);
        LogWrite(frameCheckOk ? LOGLEVEL_INFO : LOGLEVEL_ERROR, __FILE__, __LINE__, L"%s", buf);

        // The live camera-control state carried in the last frame, proving
        // the controls (brightness, contrast, exposure, ...) reach the
        // streamed frames intact (the checksum above guards their bytes).
        wchar_t ctrl[256];
        swprintf(ctrl, _countof(ctrl),
                 L"frame-ctrl: bright=%u contrast=%u expo=%u sharp=%u sat=%u hue=%u gamma=%u wb=%u "
                 L"iris=%u focus=%u zoom=%u pan=%u tilt=%u",
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_BRIGHTNESS]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_CONTRAST]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_EXPOSURE]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_SHARPNESS]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_SATURATION]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_HUE]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_GAMMA]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_WHITEBALANCE]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_IRIS]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_FOCUS]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_ZOOM]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_PAN]),
                 FRAME_META_FEATURE_VALUE(meta.features[FRAME_META_FEAT_TILT]));
        Log(INFO, L"%s", ctrl);

        // The camera-head controller register file carried in the last
        // frame (common/camera_regs.h), decoded: gains, exposure, binning,
        // pixel clock, readout window, trigger mode and live status.
        const uint8_t *rg = meta.i2cRegs;
        wchar_t head[320];
        swprintf(head, _countof(head),
                 L"frame-i2c: vgain=%u igain=%u expo=%u(unit=%u) bin=%ux%u speed=%ukHz "
                 L"setup=0x%04x roi=[%u..%u)x[%u..%u) adcoff=%u trig=0x%02x status=0x%02x",
                 rg[CAMREG_VIDEO_GAIN], rg[CAMREG_INTENSIFIER_GAIN],
                 CAMREG_LE16(rg[CAMREG_EXPOSURE_TIME_L], rg[CAMREG_EXPOSURE_TIME_H]), rg[CAMREG_EXPOSURE_UNITS],
                 rg[CAMREG_X_BINNING], rg[CAMREG_Y_BINNING], CAMREG_LE16(rg[CAMREG_SPEED_L], rg[CAMREG_SPEED_H]),
                 CAMREG_LE16(rg[CAMREG_SETUP_VALUE_L], rg[CAMREG_SETUP_VALUE_H]),
                 CAMREG_LE16(rg[CAMREG_ROI_X_START_L], rg[CAMREG_ROI_X_START_H]),
                 CAMREG_LE16(rg[CAMREG_ROI_X_END_L], rg[CAMREG_ROI_X_END_H]),
                 CAMREG_LE16(rg[CAMREG_ROI_Y_START_L], rg[CAMREG_ROI_Y_START_H]),
                 CAMREG_LE16(rg[CAMREG_ROI_Y_END_L], rg[CAMREG_ROI_Y_END_H]), rg[CAMREG_ADC_OFFSET],
                 rg[CAMREG_TRIGGER_MODE], rg[CAMREG_STATUS]);
        Log(INFO, L"%s", head);

        // End-to-end control verification: the probe values set via DirectShow
        // versus what came back embedded in the frames.
        if (featuresChecked > 0) {
            swprintf(buf, _countof(buf), L"frame-verify: %d/%d controls echoed set values",
                     featuresChecked - featuresMismatched, featuresChecked);
            LogWrite(featureValuesOk ? LOGLEVEL_INFO : LOGLEVEL_ERROR, __FILE__, __LINE__, L"%s", buf);
        }
    }

    if (fellBackToHeadless) {
        Log(WARN, L"no video window: the video renderer could not start this format");
    }
    if (!Passed()) {
        std::wstring reason = failReason;
        if (!propFail.empty()) {
            reason += L" [";
            reason += propFail;
            reason += L"]";
        }
        if (!featureFail.empty()) {
            reason += L" [";
            reason += featureFail;
            reason += L"]";
        }
        Log(ERROR, L"%s", reason.c_str());
        Log(ERROR, L"set=0x%08lX render=0x%08lX run=0x%08lX evt=%ld", hrSetFormat, hrRender, hrRun, errorEvent);
    }
}

void ComboResult::Print() const {
    if (Passed()) {
        Log(INFO, L"--- PASS: %s ---", label.c_str());
    } else {
        Log(INFO, L"--- FAIL: %s (rerun: -m %d -f %.3g) ---", label.c_str(), capIndex, fps);
    }
}

void ComboResult::PrintFailure() const {
    Log(ERROR,
        L"FAIL: %-40s %s (frames=%ld maxpx=%d set=0x%08lX render=0x%08lX run=0x%08lX evt=%ld) "
        L"[rerun: -m %d -f %.3g]",
        label.c_str(), failReason, frames, maxByte, hrSetFormat, hrRender, hrRun, errorEvent, capIndex, fps);
}

} // namespace dsweep
