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
/// Device discovery and lifecycle:
///   * PlGetNumberDevices  - cameraless parameter validation + a live count.
///   * PlInitialize / PlUninitialize - open the configured camera and verify the
///     handle is cleared on teardown (one case for the open/close pair).

#include "framework.h"

/// PlGetNumberDevices rejects a NULL pFilter with PL_ERROR_INVALID_PARAM and
/// zeroes *pDeviceCount even then.  The live enumeration count is reported.
PL_TEST(enumerate, GetNumberDevices) {
    ULONG count = 123;
    CheckRc(g_api.PlGetNumberDevices(nullptr, &count, nullptr, nullptr), PL_ERROR_INVALID_PARAM,
            L"PlGetNumberDevices(NULL filter)");
    // *pDeviceCount is zeroed even though the call fails.
    Check(count == 0, L"PlGetNumberDevices zeroed *pDeviceCount (got %lu)", count);

    // Live enumeration (informational; the fake camera reports its count here).
    count = 0;
    const char *filter = g_opt.filter.c_str();
    PL_RETURN_CODE rc = g_api.PlGetNumberDevices(filter, &count, filter, &count);
    Report(rc, L"PlGetNumberDevices(filter)");
    Log(INFO, L"device count: %lu", count);
}

/// Open the configured camera and tear it down, asserting the documented
/// post-conditions of the open/close pair.
PL_TEST(enumerate, Initialize_Uninitialize) {
    HANDLE cam = OpenCamera();
    if (cam == nullptr) {
        ReportNoCamera();
        return;
    }
    Log(INFO, L"opened camera handle %p", cam);
    Check(cam != nullptr, L"PlInitialize returned a non-NULL handle");

    PL_RETURN_CODE rc = g_api.PlUninitialize(&cam);
    CheckRc(rc, PL_SUCCESS, L"PlUninitialize");
    Check(cam == nullptr, L"PlUninitialize cleared the handle (got %p)", cam);
}
