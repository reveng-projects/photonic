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
/// Pretty-printing helpers for DirectShow media types (implemented in
/// format_names.cpp): render a subtype GUID as a readable name or FOURCC, name
/// the major type, and convert a 100-ns frame interval to fps.  Shared by the
/// sweep, its graph helpers and the per-combination result printing.

#pragma once

#include <dshow.h>
#include <windows.h>

#include <string>

/// Render a subtype GUID readably: a known name (RGB24, UYVY, ...), the FOURCC
/// in quotes when printable, or the raw first DWORD in braces.
///
/// @param sub     The subtype GUID to format.
/// @return A human-readable string representation of the subtype.
std::wstring FormatSubtype(const GUID &sub);

/// Name the major media type (Video / Stream / "(other)").
///
/// @param major   The major-type GUID to name.
/// @return A static string naming the major type.
const wchar_t *FormatMajorName(const GUID &major);

/// Convert a frame interval in 100-ns units to frames per second (0 for a
/// non-positive interval).
///
/// @param interval100ns  Frame interval in 100-nanosecond units.
/// @return Frames per second, or 0 if the interval is non-positive.
double FpsFromInterval(LONGLONG interval100ns);
