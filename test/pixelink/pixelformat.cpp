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
/// PlGetPixelFormat / PlSetPixelFormat: one case for the getter/setter pair.

#include "framework.h"

PL_TEST(pixelformat, PixelFormat_GetSet) {
    // Cameraless contract.
    ULONG u = 0;
    CheckRc(g_api.PlGetPixelFormat(nullptr, &u), PL_ERROR, L"PlGetPixelFormat(NULL)");
    CheckRc(g_api.PlSetPixelFormat(nullptr, 0), PL_ERROR, L"PlSetPixelFormat(NULL)");

    // Camera round-trip: read the format, set it back, confirm it is unchanged.
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }

    ULONG pf = 0;
    if (g_api.PlGetPixelFormat(cam, &pf) != PL_SUCCESS) {
        Report(PL_ERROR, L"PlGetPixelFormat");
        return;
    }
    Log(INFO, L"pixel format: %lu", pf);
    Report(g_api.PlSetPixelFormat(cam, pf), L"PlSetPixelFormat(current)");

    ULONG pf2 = ~pf;
    if (g_api.PlGetPixelFormat(cam, &pf2) == PL_SUCCESS) {
        Check(pf2 == pf, L"pixel-format round-trip (set %lu, got %lu)", pf, pf2);
    }
}
