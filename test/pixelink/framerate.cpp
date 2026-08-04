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
/// Frame-rate entry points:
///   * PlEnumAvailableFrameRates            - one case.
///   * PlGetCurrentFrameRate / PlSetCurrentFrameRate - one case for the pair,
///     with a get/set/get round-trip and restore.

#include <cstdio>
#include <string>

#include "framework.h"

PL_TEST(framerate, EnumAvailableFrameRates) {
    // Cameraless contract.
    float f = 0.0f;
    ULONG u = 0;
    CheckRc(g_api.PlEnumAvailableFrameRates(nullptr, &f, &u), PL_ERROR, L"PlEnumAvailableFrameRates(NULL)");

    // Live enumeration.
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }
    float rates[64];
    ULONG nRates = (ULONG) (sizeof(rates) / sizeof(rates[0]));
    PL_RETURN_CODE rc = g_api.PlEnumAvailableFrameRates(cam, rates, &nRates);
    Report(rc, L"PlEnumAvailableFrameRates");
    if (rc == PL_SUCCESS) {
        std::wstring list;
        wchar_t buf[32];
        for (ULONG i = 0; i < nRates && i < 16; i++) {
            swprintf(buf, _countof(buf), L" %.3g", rates[i]);
            list += buf;
        }
        Log(INFO, L"%lu rate(s):%s", nRates, list.c_str());
    }
}

PL_TEST(framerate, CurrentFrameRate_GetSet) {
    // Cameraless contract for both halves of the pair.
    float f = 0.0f;
    CheckRc(g_api.PlGetCurrentFrameRate(nullptr, &f), PL_ERROR, L"PlGetCurrentFrameRate(NULL)");
    CheckRc(g_api.PlSetCurrentFrameRate(nullptr, 30.0f), PL_ERROR, L"PlSetCurrentFrameRate(NULL)");

    // Camera round-trip: read the current rate, set the first enumerated rate,
    // confirm it reads back, then restore the original.
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }

    float fr = 0.0f;
    if (g_api.PlGetCurrentFrameRate(cam, &fr) != PL_SUCCESS) {
        Report(PL_ERROR, L"PlGetCurrentFrameRate");
        return;
    }
    Log(INFO, L"current frame rate: %.3g fps", fr);

    float rates[64];
    ULONG nRates = (ULONG) (sizeof(rates) / sizeof(rates[0]));
    if (g_api.PlEnumAvailableFrameRates(cam, rates, &nRates) == PL_SUCCESS && nRates > 0) {
        Report(g_api.PlSetCurrentFrameRate(cam, rates[0]), L"PlSetCurrentFrameRate(rates[0])");
        float back = 0.0f;
        if (g_api.PlGetCurrentFrameRate(cam, &back) == PL_SUCCESS) {
            Check(back == rates[0], L"frame rate round-trip (set %.3g, got %.3g)", rates[0], back);
        }
        g_api.PlSetCurrentFrameRate(cam, fr); // restore
    }
}
