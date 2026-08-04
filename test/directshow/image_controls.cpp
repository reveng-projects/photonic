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
/// Per-combination image-control checks for the DirectShow sweep.  See
/// image_controls.h.

#include "image_controls.h"

#include <iomanip>
#include <sstream>

namespace dsweep {

enum PropOutcome { PROP_UNSUPPORTED, PROP_PASS, PROP_FAIL };

/// The control range the fake advertises per feature, expanded from the shared
/// FRAME_META_FEATURE_RANGES list (common/frame_meta.h) its register model is
/// built from, so the expectation cannot drift.  Indexed by FRAME_META_FEAT_*.
/// Only checked in frame-header (fake) mode: a real camera's ranges are unknown.
struct ExpectedRange {
    long mn;
    long mx;
};
#define EXPECTED_RANGE_ENTRY(feat, mn, mx, df) {mn, mx},
static const ExpectedRange kExpectedRanges[FRAME_META_NUM_FEATURES] = {FRAME_META_FEATURE_RANGES(EXPECTED_RANGE_ENTRY)};

/// Test one property on either IAMVideoProcAmp or IAMCameraControl (the two
/// interfaces share GetRange/Get/Set signatures, hence the template).  The Auto
/// and Manual flag bits have the same numeric values in both flag enums.
///
/// `expected` is non-null when testing the fake device: it implements every
/// control with a known range, so a missing control there means it regressed
/// (driver no longer advertises it, or the fake stopped backing its DCAM
/// feature register) and a GetRange that does not match the shared range list
/// means the driver mangled the inquiry register — both are failures.  A real
/// camera legitimately implements only a subset with unknown ranges, so with
/// `expected` null an unsupported control is just skipped and the advertised
/// range is taken at face value.
template <class IFace>
static PropOutcome TestOneProperty(IFace *iface, long id, const wchar_t *name, long flagAuto, long flagManual,
                                   const ExpectedRange *expected, std::wstring &firstFail) {
    long mn = 0, mx = 0, step = 0, def = 0, caps = 0;
    HRESULT hrRange = iface->GetRange(id, &mn, &mx, &step, &def, &caps);
    if (FAILED(hrRange)) {
        if (expected != nullptr && firstFail.empty()) {
            std::wostringstream b;
            b << name << L" unsupported (GetRange=0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill(L'0') << (unsigned long) hrRange << L")";
            firstFail = b.str();
        }
        return PROP_UNSUPPORTED;
    }
    if (expected != nullptr && (mn != expected->mn || mx != expected->mx)) {
        if (firstFail.empty()) {
            std::wostringstream b;
            b << name << L" range [" << mn << L".." << mx << L"] != advertised [" << expected->mn << L".."
              << expected->mx << L"]";
            firstFail = b.str();
        }
        return PROP_FAIL;
    }
    if (step <= 0) {
        step = 1;
    }

    // Remember the live setting so it can be restored after poking the control.
    long origVal = def, origFlags = flagManual;
    iface->Get(id, &origVal, &origFlags);

    bool ok = true;
    auto note = [&](const wchar_t *what, long want, long got, long gf) {
        if (firstFail.empty()) {
            std::wostringstream b;
            b << name << L" " << what << L" want=" << want << L" got=" << got << L" flags=0x" << std::hex << gf;
            firstFail = b.str();
        }
    };

    if ((caps & flagManual) != 0) {
        long mid = mn + ((mx - mn) / 2 / step) * step;
        long wants[] = {mn, mx, mid};
        for (long want : wants) {
            long got = 0, gf = 0;
            HRESULT hs = iface->Set(id, want, flagManual);
            HRESULT hg = iface->Get(id, &got, &gf);
            long diff = got > want ? got - want : want - got;
            if (FAILED(hs) || FAILED(hg) || diff > step || (gf & flagManual) == 0) {
                ok = false;
                note(L"manual", want, got, gf);
            }
        }
    }

    if ((caps & flagAuto) != 0) {
        long got = 0, gf = 0;
        HRESULT hs = iface->Set(id, def, flagAuto);
        HRESULT hg = iface->Get(id, &got, &gf);
        if (FAILED(hs) || FAILED(hg) || (gf & flagAuto) == 0) {
            ok = false;
            note(L"auto", def, got, gf);
        }
    }

    iface->Set(id, origVal, origFlags); // restore original setting
    return ok ? PROP_PASS : PROP_FAIL;
}

PropSummary CheckProperties(IAMVideoProcAmp *procAmp, IAMCameraControl *camControl, bool expectAll) {
    PropSummary s;
    // An unsupported control on a real camera is only counted here; the
    // support set itself is printed once at setup (LogPropertySupport).
    auto tally = [&](PropOutcome o) {
        if (o == PROP_UNSUPPORTED && !expectAll) {
            s.skipped++;
            return;
        }
        s.expected++;
        if (o == PROP_PASS) {
            s.passed++;
        }
    };
    if (procAmp != nullptr) {
        for (auto &p : kProcAmpProps) {
            tally(TestOneProperty(procAmp, p.id, p.name, VideoProcAmp_Flags_Auto, VideoProcAmp_Flags_Manual,
                                  expectAll ? &kExpectedRanges[p.metaIndex] : nullptr, s.firstFail));
        }
    }
    if (camControl != nullptr) {
        for (auto &p : kCameraControlProps) {
            tally(TestOneProperty(camControl, p.id, p.name, CameraControl_Flags_Auto, CameraControl_Flags_Manual,
                                  expectAll ? &kExpectedRanges[p.metaIndex] : nullptr, s.firstFail));
        }
    }
    return s;
}

} // namespace dsweep
