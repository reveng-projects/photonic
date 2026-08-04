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
/// DirectShow capture-device selection helpers shared by the test suites
/// (implemented in finddevice.cpp): enumerate the video-input device category,
/// pick a device by FriendlyName substring and match ordinal, and resolve /
/// open its kernel DevicePath.  Implemented once here so the DirectShow sweep
/// and the camera-register suite locate the device the same way.

#pragma once

#include <dshow.h>
#include <windows.h>

#include <string>

#include "comptr.h"

/// Enumerate the video capture devices, log each one, and select a device by
/// FriendlyName: without deviceIndex (-1) the first device whose name contains
/// nameSubstr (case-insensitive) wins; with deviceIndex N the N-th (0-based)
/// matching device wins, so identically-named cameras can be told apart.  On
/// success the selected moniker and its FriendlyName are returned.
///
/// @param nameSubstr   Case-insensitive substring to match against FriendlyName.
/// @param deviceIndex  0-based ordinal among matching devices, or -1 for the first match.
/// @param moniker      Receives the selected device moniker on success.
/// @param chosenName   Receives the FriendlyName of the selected device on success.
/// @return S_OK on success, E_FAIL if no matching device is found.
HRESULT DsFindCaptureMoniker(const std::wstring &nameSubstr, int deviceIndex, ComPtr<IMoniker> &moniker,
                             std::wstring &chosenName);

/// Read the moniker's DevicePath property: the kernel device path that ksproxy
/// opens, suitable for a raw CreateFile.
///
/// @param moniker      The device moniker to query.
/// @param devicePath   Receives the kernel device path string on success.
/// @return S_OK on success, or an HRESULT error code on failure.
HRESULT DsGetDevicePath(IMoniker *moniker, std::wstring &devicePath);

/// Open the kernel device path with the same access/share mode ksproxy uses.
/// Returns INVALID_HANDLE_VALUE on failure (GetLastError() holds the reason).
///
/// @param devicePath  The kernel device path to open.
/// @return An open device handle, or INVALID_HANDLE_VALUE on failure.
HANDLE DsOpenDevicePath(const std::wstring &devicePath);

/// Select the capture device (DsFindCaptureMoniker), run the raw-open / KS pin
/// diagnostics on its kernel device path, and bind the moniker to the capture
/// filter.  On success *ppFilter holds the filter and chosenName its
/// FriendlyName.
///
/// @param nameSubstr   Case-insensitive substring to match against FriendlyName.
/// @param deviceIndex  0-based ordinal among matching devices, or -1 for the first match.
/// @param ppFilter     Receives the bound capture filter on success.
/// @param chosenName   Receives the FriendlyName of the selected device on success.
/// @return S_OK on success, or an HRESULT error code on failure.
HRESULT FindCaptureDevice(const std::wstring &nameSubstr, int deviceIndex, IBaseFilter **ppFilter,
                          std::wstring &chosenName);
