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
/// PlReadExtI2cRegister / PlWriteExtI2cRegister: one case for the pair.

#include "framework.h"

PL_TEST(i2c, ExtI2cRegister_ReadWrite) {
    // Cameraless contract.
    ULONG u = 0;
    CheckRc(g_api.PlReadExtI2cRegister(nullptr, 0, 0, &u), PL_ERROR, L"PlReadExtI2cRegister(NULL)");
    CheckRc(g_api.PlWriteExtI2cRegister(nullptr, 0, 0, 0), PL_ERROR, L"PlWriteExtI2cRegister(NULL)");

    // Live access against a camera (informational).
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }
    ULONG i2cVal = 0;
    Report(g_api.PlReadExtI2cRegister(cam, 0, 0, &i2cVal), L"PlReadExtI2cRegister(0,0)");
    Report(g_api.PlWriteExtI2cRegister(cam, 0, 0, 0), L"PlWriteExtI2cRegister(0,0,0)");
}
