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
/// Concurrency tests for the driver's Photonic IOCTL interface, implemented in
/// concurrency.cpp and run as part of the --test-ioctl suite.

#pragma once

#include <dshow.h>
#include <windows.h>

#include <string>

/// Run the concurrency tests: multithreaded storms over the capture-slot
/// lifecycle verbs, second-handle ownership rules, handle close racing
/// in-flight IOCTLs, and the DirectShow/IOCTL exclusion race. See the file
/// header of concurrency.cpp for what each test asserts.
///
/// @param hCamera       Open device handle the storms hammer.
/// @param devicePath    Device path for opening additional handles.
/// @param moniker       DirectShow moniker of the same device (exclusion race).
/// @param stormSeconds  Duration of each timed storm.
/// @param passed        Incremented per passing check.
/// @param failed        Incremented per failing check.
void RunConcurrencyTests(HANDLE hCamera, const std::wstring &devicePath, IMoniker *moniker, int stormSeconds,
                         int &passed, int &failed);
