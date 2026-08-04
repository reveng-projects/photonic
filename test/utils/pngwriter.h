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
/// PNG sample-frame writer backed by the Windows Imaging Component (WIC), used
/// by the test suites' --dump-dir option to record one sample image per tested
/// mode.  Implemented in pngwriter.cpp.
///
/// The caller must have COM initialized on the calling thread (both suites
/// CoInitializeEx before running).  Every function logs its own failure detail
/// through Log() and returns false, so callers only report success.

#pragma once

#include <windows.h>

#include <guiddef.h>

/// Save one delivered video frame as a PNG file, converting from the camera's
/// wire format:
///   RGB24        -> 24-bit BGR PNG
///   RGB8 (MONO8) -> 8-bit greyscale PNG (the palette is a greyscale ramp)
///   Y16 (MONO16) -> 16-bit greyscale PNG (lossless, little-endian source)
///   RGGB (RAW8)  -> 8-bit greyscale PNG of the raw Bayer mosaic (no demosaic)
///   UYVY / Y411  -> 8-bit greyscale PNG of the extracted luma plane
/// biHeight is the signed VIDEOINFOHEADER height: negative means top-down; a
/// positive value on a BI_RGB coding (RGB8/RGB24) means bottom-up and the rows
/// are flipped into the PNG.  The row stride is derived as len / height.
///
/// @param path      Output PNG file path.
/// @param data      Pointer to the raw frame bytes.
/// @param len       Size of the frame buffer in bytes.
/// @param width     Frame width in pixels.
/// @param biHeight  Signed frame height: negative = top-down, positive = bottom-up.
/// @param subtype   DirectShow media subtype GUID identifying the pixel format.
/// @return true on success, false on failure after logging the error.
bool SaveFramePng(const wchar_t *path, const BYTE *data, size_t len, LONG width, LONG biHeight, const GUID &subtype);

/// Save a raw greyscale frame that has no media type attached (the ioctl
/// suite's mapped ring): top-down rows, bitsPerPixel 8 (MONO8) or 16 (MONO16,
/// little-endian).
///
/// @param path         Output PNG file path.
/// @param data         Pointer to the raw frame bytes.
/// @param len          Size of the frame buffer in bytes.
/// @param width        Frame width in pixels.
/// @param height       Frame height in pixels (top-down).
/// @param bitsPerPixel Bits per pixel: 8 for MONO8, 16 for MONO16.
/// @return true on success, false on failure after logging the error.
bool SaveMonoFramePng(const wchar_t *path, const BYTE *data, size_t len, LONG width, LONG height, UINT32 bitsPerPixel);

/// Create the PNG dump directory (a single level; an existing directory is
/// fine).  Returns false after logging when it cannot be created.
///
/// @param dir  Path to the directory to create.
/// @return true on success or if the directory already exists, false on creation failure.
bool EnsureDumpDir(const wchar_t *dir);
