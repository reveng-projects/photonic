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
/// Small shared helpers used across the test suites.
#pragma once

#include <windows.h>

#include <string>

/// Convert a wide (UTF-16) string to a narrow (ANSI / CP_ACP) std::string.
/// Useful when passing command-line arguments to ANSI-only APIs (LPCSTR).
inline std::string WideToString(const std::wstring &wide) {
    if (wide.empty()) {
        return std::string();
    }
    int len = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int) wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t) (len > 0 ? len : 0), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int) wide.size(), &out[0], len, nullptr, nullptr);
    }
    return out;
}
