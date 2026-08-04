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
/// Per-combination image-control checks (IAMVideoProcAmp / IAMCameraControl)
/// for the DirectShow sweep, implemented in image_controls.cpp.
///
/// Alongside streaming each format/rate combination the sweep exercises the
/// DirectShow VideoProcAmp and CameraControl properties the driver maps to DCAM
/// feature registers (Brightness->0x500 ... Contrast(gain)->0x520,
/// Exposure(shutter)->0x51c, Focus->0x528, etc.).  For each supported property
/// it reads the range, drives min/mid/max in manual mode and confirms the value
/// round-trips (within one stepping delta) with the manual flag set, exercises
/// auto mode when the caps advertise it, then restores the original setting.
///
/// Every type here lives in the dsweep namespace: this suite links into the
/// same binary as the other test suites, which define their own file-scope
/// types (e.g. capture.cpp's Options), so the suite types are namespaced to
/// keep their linker names distinct.

#pragma once

#include <dshow.h>
#include <windows.h>

#include <string>

#include "../../common/frame_meta.h"

namespace dsweep {

struct PropItem {
    long id;
    const wchar_t *name;
    int metaIndex; ///< FRAME_META_FEAT_* slot: names the control's shared range entry
};

/// The VideoProcAmp properties the driver routes to DCAM feature registers.
static const PropItem kProcAmpProps[] = {
    {VideoProcAmp_Brightness, L"Brightness", FRAME_META_FEAT_BRIGHTNESS},
    {VideoProcAmp_Contrast, L"Contrast", FRAME_META_FEAT_CONTRAST},
    {VideoProcAmp_Hue, L"Hue", FRAME_META_FEAT_HUE},
    {VideoProcAmp_Saturation, L"Saturation", FRAME_META_FEAT_SATURATION},
    {VideoProcAmp_Sharpness, L"Sharpness", FRAME_META_FEAT_SHARPNESS},
    {VideoProcAmp_Gamma, L"Gamma", FRAME_META_FEAT_GAMMA},
    {VideoProcAmp_WhiteBalance, L"WhiteBalance", FRAME_META_FEAT_WHITEBALANCE},
};

/// The CameraControl properties the driver routes to DCAM feature registers.
static const PropItem kCameraControlProps[] = {
    {CameraControl_Pan, L"Pan", FRAME_META_FEAT_PAN},
    {CameraControl_Tilt, L"Tilt", FRAME_META_FEAT_TILT},
    {CameraControl_Zoom, L"Zoom", FRAME_META_FEAT_ZOOM},
    {CameraControl_Exposure, L"Exposure", FRAME_META_FEAT_EXPOSURE},
    {CameraControl_Iris, L"Iris", FRAME_META_FEAT_IRIS},
    {CameraControl_Focus, L"Focus", FRAME_META_FEAT_FOCUS},
};

/// End-to-end control verification: set each control to a distinct probe value via
/// DirectShow, then confirm the fake echoes that exact value back in the embedded
/// frame metadata.  The driver maps DirectShow values 1:1 onto the DCAM register,
/// so the embedded value must equal the value set.  Each control advertises its
/// own range (the fake's ranges vary per feature), so the probe value is derived
/// from the control's GetRange by DirectShowSweep::ProbeValue(): distinct across
/// controls as far as their ranges allow (catches a cross-wired register) and
/// changing every combination (so a stale frame left over from an earlier
/// combination cannot masquerade as this combination's freshly set value).
struct FeatureProbe {
    bool camControl; ///< false: IAMVideoProcAmp, true: IAMCameraControl
    long id;         ///< KS property id
    int metaIndex;   ///< FRAME_META_FEAT_* slot in the frame metadata
    const wchar_t *name;
};
static const FeatureProbe kFeatureProbes[] = {
    {false, VideoProcAmp_Brightness, FRAME_META_FEAT_BRIGHTNESS, L"Brightness"},
    {false, VideoProcAmp_Contrast, FRAME_META_FEAT_CONTRAST, L"Contrast"},
    {false, VideoProcAmp_Hue, FRAME_META_FEAT_HUE, L"Hue"},
    {false, VideoProcAmp_Saturation, FRAME_META_FEAT_SATURATION, L"Saturation"},
    {false, VideoProcAmp_Sharpness, FRAME_META_FEAT_SHARPNESS, L"Sharpness"},
    {false, VideoProcAmp_Gamma, FRAME_META_FEAT_GAMMA, L"Gamma"},
    {false, VideoProcAmp_WhiteBalance, FRAME_META_FEAT_WHITEBALANCE, L"WhiteBalance"},
    {true, CameraControl_Pan, FRAME_META_FEAT_PAN, L"Pan"},
    {true, CameraControl_Tilt, FRAME_META_FEAT_TILT, L"Tilt"},
    {true, CameraControl_Zoom, FRAME_META_FEAT_ZOOM, L"Zoom"},
    {true, CameraControl_Exposure, FRAME_META_FEAT_EXPOSURE, L"Exposure"},
    {true, CameraControl_Iris, FRAME_META_FEAT_IRIS, L"Iris"},
    {true, CameraControl_Focus, FRAME_META_FEAT_FOCUS, L"Focus"},
};

struct PropSummary {
    int expected = 0; ///< controls that must round-trip (supported, or all on the fake)
    int passed = 0;   ///< of those, how many were present and round-tripped
    int skipped = 0;  ///< controls this camera does not implement (real camera only)
    std::wstring firstFail;
};

/// Run the full VideoProcAmp + CameraControl property check once.  With
/// `expectAll` (fake device) every entry in the tables must be present and its
/// GetRange must match the shared range list: each entry counts toward
/// `expected` and only a full round-trip counts toward `passed`, so an
/// unsupported control fails the check.  Without it (real camera) an
/// unsupported control is counted as skipped and does not fail the check, and
/// no range expectation is applied.
///
/// @param procAmp     The VideoProcAmp interface for the capture filter.
/// @param camControl  The CameraControl interface for the capture filter.
/// @param expectAll   True when testing the fake device (all controls must be present and match ranges).
/// @return A PropSummary with counts of expected, passed, and skipped controls and the first failure name.
PropSummary CheckProperties(IAMVideoProcAmp *procAmp, IAMCameraControl *camControl, bool expectAll);

} // namespace dsweep
