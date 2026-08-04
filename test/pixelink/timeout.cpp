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
/// PlGetTimeout / PlSetTimeout: one case for the getter/setter pair.

#include "framework.h"

PL_TEST(timeout, Timeout_GetSet) {
    // Cameraless contract.
    ULONG u = 0;
    CheckRc(g_api.PlGetTimeout(nullptr, &u), PL_ERROR, L"PlGetTimeout(NULL)");
    CheckRc(g_api.PlSetTimeout(nullptr, 0), PL_ERROR, L"PlSetTimeout(NULL)");

    // Camera round-trip: bump the timeout, confirm the new value, then restore.
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }

    ULONG to = 0;
    if (g_api.PlGetTimeout(cam, &to) != PL_SUCCESS) {
        Report(PL_ERROR, L"PlGetTimeout");
        return;
    }
    Log(INFO, L"timeout: %lu ms", to);
    Report(g_api.PlSetTimeout(cam, to + 100), L"PlSetTimeout(+100)");

    ULONG to2 = 0;
    if (g_api.PlGetTimeout(cam, &to2) == PL_SUCCESS) {
        Check(to2 == to + 100, L"timeout round-trip (set %lu, got %lu)", to + 100, to2);
    }
    g_api.PlSetTimeout(cam, to); // restore
}
