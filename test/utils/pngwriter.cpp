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
/// PNG sample-frame writer.  See pngwriter.h for the contract.
///
/// WIC is used as the encoder: it ships in the OS (XP SP3 and later, so it is
/// available everywhere the v140_xp binary runs), supports 16-bit greyscale
/// PNG natively (GUID_WICPixelFormat16bppGray), and needs no third-party code.
/// The camera's wire formats are converted to a WIC-encodable layout one row
/// at a time (luma extraction for the YUV codings, row flip for bottom-up
/// BI_RGB), then handed to the PNG frame encoder in a single WritePixels call.

#include "pngwriter.h"

#include <dshow.h>
#include <wincodec.h>
#include <windows.h>

#include <vector>

#include "../directshow/comptr.h"
#include "log.h"

#pragma comment(lib, "windowscodecs.lib")

namespace {

/// How the source rows are turned into PNG rows.  One entry per wire format
/// the driver can deliver (photonic/stream/formats.c, PhotonicStreamFormatPixelToMedia).
enum class SrcLayout {
    Gray8,    ///< RGB8 (greyscale palette) and RGGB: copy the row
    Gray16,   ///< Y16: copy the row (little-endian, matching 16bppGray)
    Bgr24,    ///< RGB24: copy the row
    UyvyLuma, ///< UYVY (U0 Y0 V0 Y1): keep the odd bytes
    Y411Luma, ///< Y411 (U0 Y0 Y1 V0 Y2 Y3): keep bytes 1,2,4,5 of each group
};

} // namespace

/// FOURCC-derived subtypes follow {FOURCC-0000-0010-8000-00AA00389B71}; the
/// non-FOURCC part is shared, so matching Data1 against the FOURCC plus the
/// common tail identifies them without per-format GUID constants.
static bool IsFourccSubtype(const GUID &sub, DWORD fourcc) {
    static const GUID base = {0, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    GUID expected = base;
    expected.Data1 = fourcc;
    return IsEqualGUID(sub, expected) != FALSE;
}

/// Map a delivered subtype to its row conversion, the minimum bytes one source
/// row needs, and whether it is a BI_RGB coding (whose positive biHeight means
/// bottom-up rows).  Returns false for a subtype the writer does not handle.
static bool MapSubtype(const GUID &subtype, LONG width, SrcLayout &layout, size_t &minRowBytes, bool &isBiRgb) {
    isBiRgb = false;
    if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB8)) {
        layout = SrcLayout::Gray8;
        minRowBytes = (size_t) width;
        isBiRgb = true;
    } else if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) {
        layout = SrcLayout::Bgr24;
        minRowBytes = (size_t) width * 3;
        isBiRgb = true;
    } else if (IsFourccSubtype(subtype, MAKEFOURCC('Y', '1', '6', ' '))) {
        layout = SrcLayout::Gray16;
        minRowBytes = (size_t) width * 2;
    } else if (IsFourccSubtype(subtype, MAKEFOURCC('R', 'G', 'G', 'B'))) {
        layout = SrcLayout::Gray8;
        minRowBytes = (size_t) width;
    } else if (IsEqualGUID(subtype, MEDIASUBTYPE_UYVY)) {
        layout = SrcLayout::UyvyLuma;
        minRowBytes = (size_t) width * 2;
    } else if (IsFourccSubtype(subtype, MAKEFOURCC('Y', '4', '1', '1'))) {
        layout = SrcLayout::Y411Luma;
        minRowBytes = (size_t) width * 3 / 2;
    } else {
        return false;
    }
    return true;
}

/// PNG pixel format and output bytes-per-pixel for one row conversion.
static const GUID &DstFormat(SrcLayout layout, size_t &dstBytesPerPixel) {
    switch (layout) {
        case SrcLayout::Gray16:
            dstBytesPerPixel = 2;
            return GUID_WICPixelFormat16bppGray;
        case SrcLayout::Bgr24:
            dstBytesPerPixel = 3;
            return GUID_WICPixelFormat24bppBGR;
        default:
            dstBytesPerPixel = 1;
            return GUID_WICPixelFormat8bppGray;
    }
}

/// Convert one source row into `width` destination pixels.
static void ConvertRow(SrcLayout layout, const BYTE *src, BYTE *dst, LONG width) {
    switch (layout) {
        case SrcLayout::Gray8:
            memcpy(dst, src, (size_t) width);
            break;
        case SrcLayout::Gray16:
            memcpy(dst, src, (size_t) width * 2);
            break;
        case SrcLayout::Bgr24:
            memcpy(dst, src, (size_t) width * 3);
            break;
        case SrcLayout::UyvyLuma:
            for (LONG x = 0; x < width; x++) {
                dst[x] = src[2 * x + 1];
            }
            break;
        case SrcLayout::Y411Luma:
            // 6 bytes carry 4 pixels; the DCAM widths using Y411 are multiples
            // of 4 (the caller validated the row size).
            for (LONG g = 0; g < width / 4; g++) {
                const BYTE *p = src + (size_t) g * 6;
                BYTE *q = dst + (size_t) g * 4;
                q[0] = p[1];
                q[1] = p[2];
                q[2] = p[4];
                q[3] = p[5];
            }
            break;
    }
}

/// Encode a fully converted (top-down, unpadded) pixel buffer as a PNG file.
static bool WicEncodePng(const wchar_t *path, LONG width, LONG height, const GUID &format, UINT dstStride,
                         const BYTE *pixels) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Log(ERROR, L"PNG save: creating the WIC factory failed: 0x%08lX", hr);
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    }
    if (FAILED(hr)) {
        Log(ERROR, L"PNG save: opening \"%s\" for writing failed: 0x%08lX", path, hr);
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&frame, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize((UINT) width, (UINT) height);
    }
    if (SUCCEEDED(hr)) {
        // The PNG encoder supports 8/16-bit grey and 24-bit BGR natively; a
        // substituted format would silently change the file's depth, so treat
        // it as a failure instead.
        WICPixelFormatGUID actual = format;
        hr = frame->SetPixelFormat(&actual);
        if (SUCCEEDED(hr) && !IsEqualGUID(actual, format)) {
            Log(ERROR, L"PNG save: the encoder substituted the pixel format");
            return false;
        }
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels((UINT) height, dstStride, dstStride * (UINT) height, const_cast<BYTE *>(pixels));
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    if (FAILED(hr)) {
        Log(ERROR, L"PNG save: encoding \"%s\" failed: 0x%08lX", path, hr);
        return false;
    }
    return true;
}

/// Convert the whole frame (row conversion + row order) and encode it.
static bool SavePng(const wchar_t *path, const BYTE *data, size_t len, LONG width, LONG height, bool bottomUp,
                    SrcLayout layout, size_t minRowBytes) {
    if (width <= 0 || height <= 0 || len == 0 || len % (size_t) height != 0) {
        Log(ERROR, L"PNG save: frame length %Iu does not divide into %ld rows", len, height);
        return false;
    }
    size_t srcStride = len / (size_t) height;
    if (srcStride < minRowBytes) {
        Log(ERROR, L"PNG save: row stride %Iu below the %Iu bytes a %ld-pixel row needs", srcStride, minRowBytes,
            width);
        return false;
    }

    size_t dstBytesPerPixel = 0;
    const GUID &format = DstFormat(layout, dstBytesPerPixel);
    size_t dstStride = (size_t) width * dstBytesPerPixel;
    std::vector<BYTE> pixels(dstStride * (size_t) height);
    for (LONG y = 0; y < height; y++) {
        LONG srcRow = bottomUp ? height - 1 - y : y;
        ConvertRow(layout, data + (size_t) srcRow * srcStride, pixels.data() + (size_t) y * dstStride, width);
    }
    return WicEncodePng(path, width, height, format, (UINT) dstStride, pixels.data());
}

bool SaveFramePng(const wchar_t *path, const BYTE *data, size_t len, LONG width, LONG biHeight, const GUID &subtype) {
    LONG height = biHeight < 0 ? -biHeight : biHeight;
    SrcLayout layout;
    size_t minRowBytes = 0;
    bool isBiRgb = false;
    if (!MapSubtype(subtype, width, layout, minRowBytes, isBiRgb)) {
        Log(WARN, L"PNG save: unhandled subtype, \"%s\" not written", path);
        return false;
    }
    // Only the BI_RGB codings encode row order in the biHeight sign; the
    // FOURCC codings are top-down regardless.
    bool bottomUp = isBiRgb && biHeight > 0;
    return SavePng(path, data, len, width, height, bottomUp, layout, minRowBytes);
}

bool SaveMonoFramePng(const wchar_t *path, const BYTE *data, size_t len, LONG width, LONG height, UINT32 bitsPerPixel) {
    SrcLayout layout = bitsPerPixel == 16 ? SrcLayout::Gray16 : SrcLayout::Gray8;
    size_t minRowBytes = (size_t) width * (bitsPerPixel == 16 ? 2 : 1);
    return SavePng(path, data, len, width, height, false, layout, minRowBytes); // bottomUp = false
}

bool EnsureDumpDir(const wchar_t *dir) {
    if (CreateDirectoryW(dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    Log(ERROR, L"cannot create the dump directory \"%s\" (err=%lu)", dir, GetLastError());
    return false;
}
