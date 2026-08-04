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
/// PlGetSubWindowSettings / PlSetSubWindowSettings: one case for the pair.

#include "framework.h"

PL_TEST(subwindow, SubWindowSettings_GetSet) {
    // Cameraless contract.
    ULONG u = 0;
    CheckRc(g_api.PlGetSubWindowSettings(nullptr, &u, &u, &u, &u, &u, &u), PL_ERROR, L"PlGetSubWindowSettings(NULL)");
    CheckRc(g_api.PlSetSubWindowSettings(nullptr, 0, 0, 0, 0, 0, 0), PL_ERROR, L"PlSetSubWindowSettings(NULL)");

    // Camera round-trip.  Argument order is
    // (decimationX, decimationY, offsetX, offsetY, width, height) — see
    // pixelink.h.
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }

    ULONG decimX = 0, decimY = 0, offX = 0, offY = 0, width = 0, height = 0;
    if (g_api.PlGetSubWindowSettings(cam, &decimX, &decimY, &offX, &offY, &width, &height) != PL_SUCCESS) {
        Report(PL_ERROR, L"PlGetSubWindowSettings");
        return;
    }
    Log(INFO, L"ROI: %lux%lu @ (%lu,%lu) decim %lux%lu", width, height, offX, offY, decimX, decimY);
    Report(g_api.PlSetSubWindowSettings(cam, decimX, decimY, offX, offY, width, height),
           L"PlSetSubWindowSettings(current)");

    // Report (don't assert) the get-after-set result.  PlGetSubWindowSettings
    // returns only decimation/offset/scaled-size, NOT the DCAM video format,
    // and those six values do not uniquely identify a format on a multi-format
    // camera, so PlSetSubWindowSettings may land on a different resolution
    // than the one read back.  Exact round-trip is therefore not an invariant
    // the API guarantees, so a mismatch is only flagged as INFO.
    ULONG decimX2 = 0, decimY2 = 0, offX2 = 0, offY2 = 0, width2 = 0, height2 = 0;
    if (g_api.PlGetSubWindowSettings(cam, &decimX2, &decimY2, &offX2, &offY2, &width2, &height2) == PL_SUCCESS) {
        Log(INFO, L"ROI after set: %lux%lu @ (%lu,%lu) decim %lux%lu%s", width2, height2, offX2, offY2, decimX2,
            decimY2,
            (width2 == width && height2 == height && offX2 == offX && offY2 == offY && decimX2 == decimX &&
             decimY2 == decimY)
                ? L" (round-trip preserved)"
                : L" (changed; see note)");
    }
}
