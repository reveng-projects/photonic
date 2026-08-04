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
/// PlGetDeviceInfo: NULL-handle contract plus a live query against a camera.

#include <vector>

#include "framework.h"

PL_TEST(deviceinfo, GetDeviceInfo) {
    // Cameraless contract: NULL handle must be rejected without dereferencing.
    ULONG u = 0;
    int dummy = 0;
    CheckRc(g_api.PlGetDeviceInfo(nullptr, &dummy, &u, 0), PL_ERROR, L"PlGetDeviceInfo(NULL)");

    // Live query (informational).
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }
    std::vector<BYTE> info(1024);
    ULONG infoSize = (ULONG) info.size();
    Report(g_api.PlGetDeviceInfo(cam, info.data(), &infoSize, 0), L"PlGetDeviceInfo");
}
