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
/// PlGetFeature / PlSetFeature: one case for the getter/setter pair.
///
/// NOTE: unlike the other entry points, PlGetFeature and PlSetFeature do not
/// reject a NULL handle and the call crashes, so there is no NULL-handle
/// contract test.  They are exercised only against a real camera handle.

#include "framework.h"

PL_TEST(feature, Feature_GetSet) {
    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }

    float feat[8] = {0};
    // Both calls pass featureId 0 and flags 0.
    Report(g_api.PlGetFeature(cam, 0, 0, feat), L"PlGetFeature(0)");
    Report(g_api.PlSetFeature(cam, 0, 0, feat), L"PlSetFeature(0)");
}
