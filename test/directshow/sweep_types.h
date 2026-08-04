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
/// Data types shared across the DirectShow sweep: the command-line Options, the
/// enumerated format Capability, the (format, frame-rate) Combination and the
/// per-combination ComboResult.  The printing method bodies live in
/// sweep_types.cpp.
///
/// Every type here lives in the dsweep namespace: this suite links into the
/// same binary as the other test suites, and capture.cpp defines its own
/// Options with a different layout — at global scope the linker would fold the
/// two inline constructor/destructor COMDATs into one, initializing/destroying
/// fields at the wrong offsets (garbage option flags, access violation in
/// ~Options).

#pragma once

#include <dshow.h>
#include <windows.h>

#include <memory>
#include <string>

#include "framestats.h"
#include "mediatype.h"

namespace dsweep {

/// Command-line options
struct Options {
    std::wstring deviceSubstr = L"Photonic";
    int deviceIndex = -1; ///< -1 == first device matching the name; else the N-th match
    DWORD durationMs = 3000;
    int modeIndex = -1;            ///< -1 == all formats
    double fps = 0.0;              ///< 0  == all standard rates in range
    bool f7Only = false;           ///< test only Format 7 (scalable) caps
    bool headless = false;         ///< never render the preview leg (no video window)
    bool listModes = false;        ///< only enumerate/print modes, run no capture
    bool frameHeaderChecks = true; ///< verify the embedded per-frame test header
    std::wstring dumpDir;          ///< save one PNG sample frame per combination here (empty == off)
};

/// A media-type template owned through unique_ptr: the SDK allocates it with
/// CoTaskMemAlloc, so it must be released through DeleteMediaType rather than
/// plain delete.
struct MediaTypeDeleter {
    void operator()(AM_MEDIA_TYPE *mt) const {
        DeleteMediaType(mt);
    }
};
using MediaTypePtr = std::unique_ptr<AM_MEDIA_TYPE, MediaTypeDeleter>;

/// One advertised format capability, enumerated up front.  Owns its media type
/// through a unique_ptr, so the struct is automatically move-only and frees the
/// AM_MEDIA_TYPE on destruction without any hand-written special members.
struct Capability {
    int index = 0;                        ///< capability index reported by the filter
    MediaTypePtr pmt;                     ///< owned media-type template
    VIDEO_STREAM_CONFIG_CAPS caps = {};   ///< stream-config caps for this format
    LONG width = 0, height = 0, bits = 0; ///< pixel geometry / depth
    LONGLONG defaultInterval = 0;         ///< the format's own AvgTimePerFrame
    std::wstring subName;                 ///< pretty subtype name

    /// A capability is a Format 7 (scalable / partial-image) format if it
    /// advertises a range of output sizes rather than a single fixed size.  The
    /// driver fills MinOutputSize with the unit step and MaxOutputSize with the
    /// maximum only for F7; for the fixed Formats 0-2 it writes the same
    /// width/height into both, so MinOutputSize == MaxOutputSize.
    ///
    /// NOTE: do NOT also test OutputGranularityX/Y here — the driver's descriptor
    /// template hardcodes OutputGranularity = 1x1 for every format (it is never
    /// overwritten per format), so that field is non-zero for the fixed formats
    /// too and would misclassify them as scalable.
    bool IsScalableFormat7() const {
        return caps.MinOutputSize.cx != caps.MaxOutputSize.cx || caps.MinOutputSize.cy != caps.MaxOutputSize.cy;
    }
};

/// Capabilities are shared between the sweep's enumerated list and the
/// Combinations that reference them, so they are passed around as shared_ptr.
using CapabilityPtr = std::shared_ptr<Capability>;

/// One (format, frame-rate) test point.  Co-owns its source format, so it stays
/// valid independently of the enumerated capability list.
struct Combination {
    CapabilityPtr cap;      ///< source format
    LONGLONG interval = 0;  ///< frame interval (100-ns units) to program
    bool isDefault = false; ///< labelled "(default)" (format's own rate)
    std::wstring label;

    /// Header line opening this combination's test (paired with ComboResult::Print).
    void PrintHeader() const;
};

/// Result of running one (format, frame-rate) combination.
struct ComboResult {
    std::wstring label;
    int capIndex = -1; ///< capability index (-m selector) of the tested format
    double fps = 0.0;  ///< frame rate (-f selector) of the tested combination
    HRESULT hrSetFormat;
    HRESULT hrRender;
    HRESULT hrRun;
    long errorEvent;               ///< 0 == none
    bool grabberActive;            ///< pixels were inspected
    long frames;                   ///< frames seen by the grabber
    int maxByte;                   ///< brightest sampled byte across all frames
    int minByte;                   ///< darkest sampled byte
    double meanByte;               ///< average sampled byte
    int propsExpected;             ///< image-control properties required to round-trip
    int propsPassed;               ///< of those, how many were present and round-tripped
    std::wstring propFail;         ///< first image-control failure (empty if none)
    bool streamingOk;              ///< renderer/streaming part succeeded (drives fallback)
    bool fellBackToHeadless;       ///< video renderer unavailable; ran without a window
    FrameMetaStats meta{};         ///< embedded frame-header verification (frame_meta.h)
    bool frameHeaderChecks = true; ///< header parsed/verified (off => real-camera mode)
    bool frameCheckOk;             ///< every frame carried a valid, in-order, intact header
    int featuresChecked;           ///< controls set via DirectShow and re-read from a frame
    int featuresMismatched;        ///< controls whose embedded value != the value set
    std::wstring featureFail;      ///< first feature set/verify failure (empty if none)
    bool featureValuesOk;          ///< every value set reached the camera and the frame
    const wchar_t *failReason;

    /// Overall verdict: the renderer/streaming part succeeded, every frame passed
    /// the embedded-header verification, every image-control value set through
    /// DirectShow was echoed back in the frame metadata, and every image-control
    /// property this camera implements round-tripped (on the fake, that is every
    /// control; a real camera's unsupported controls are skipped).
    bool Passed() const {
        return streamingOk && frameCheckOk && featureValuesOk && (propsExpected == 0 || propsPassed == propsExpected);
    }

    /// Middle diagnostics for one combination's test: the frame/pixel/property
    /// measurements and, on failure, the reason and the raw HRESULTs.
    void PrintDiagnostics() const;

    /// Footer banner closing one combination's test: the header repeated with the
    /// PASS/FAIL verdict.  A failure also prints the -m/-f selector so the
    /// combination can be rerun individually.
    void Print() const;

    /// One summary-table line for a failed combination (HRESULTs + diagnostics),
    /// including the -m/-f selector for an individual rerun.
    void PrintFailure() const;
};

} // namespace dsweep
