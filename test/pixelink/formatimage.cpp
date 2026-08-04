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
/// PlFormatImage is a pure (cameraless) conversion routine, so its whole
/// contract is exercised here without opening a device.

#include <cstring>
#include <vector>

#include "framework.h"

/// Exercise the validation edges and one full MONO->format1 conversion whose
/// required size is rowBytes*height + 0x436 (a BMP header + 256-entry palette).
PL_TEST(formatimage, FormatImage) {
    const ULONG rowBytes = 16;
    const ULONG height = 8;
    std::vector<BYTE> src(rowBytes * height);
    for (size_t i = 0; i < src.size(); i++) {
        src[i] = (BYTE) i;
    }
    const ULONG kHeaderAndPalette = 0x436;
    const ULONG required = rowBytes * height + kHeaderAndPalette;

    std::vector<BYTE> dst(required + 64);
    ULONG dstSize = (ULONG) dst.size();

    // NULL pDestBufferSize -> PL_ERROR_INVALID_PARAM (checked before the
    // format arguments).
    CheckRc(g_api.PlFormatImage(src.data(), PL_IMAGE_FORMAT_SRC_MONO, rowBytes, height, PL_IMAGE_FORMAT_DST_1,
                                dst.data(), nullptr),
            PL_ERROR_INVALID_PARAM, L"PlFormatImage(NULL size)");

    // Unrecognised (input,output) pair -> PL_ERROR_INVALID_COUNT.
    dstSize = (ULONG) dst.size();
    CheckRc(g_api.PlFormatImage(src.data(), PL_IMAGE_FORMAT_SRC_MONO, rowBytes, height, PL_IMAGE_FORMAT_DST_3,
                                dst.data(), &dstSize),
            PL_ERROR_INVALID_COUNT, L"PlFormatImage(mono,out3 unsupported)");

    // Unknown source format (1 is not one of 0/2/5) -> PL_ERROR_INVALID_COUNT.
    dstSize = (ULONG) dst.size();
    CheckRc(g_api.PlFormatImage(src.data(), 1, rowBytes, height, PL_IMAGE_FORMAT_DST_1, dst.data(), &dstSize),
            PL_ERROR_INVALID_COUNT, L"PlFormatImage(badsrc)");

    // Destination too small -> PL_ERROR_UNSUPPORTED_FORMAT, and the routine
    // reports the size it needs.
    dstSize = 1;
    CheckRc(g_api.PlFormatImage(src.data(), PL_IMAGE_FORMAT_SRC_MONO, rowBytes, height, PL_IMAGE_FORMAT_DST_1,
                                dst.data(), &dstSize),
            PL_ERROR_UNSUPPORTED_FORMAT, L"PlFormatImage(dest too small)");
    Check(dstSize == required, L"PlFormatImage reported required size %lu (expected %lu)", dstSize, required);

    // Full valid conversion: MONO -> format1.  Produces a BMP ('BM' magic).
    dstSize = (ULONG) dst.size();
    memset(dst.data(), 0, dst.size());
    PL_RETURN_CODE rc = g_api.PlFormatImage(src.data(), PL_IMAGE_FORMAT_SRC_MONO, rowBytes, height,
                                            PL_IMAGE_FORMAT_DST_1, dst.data(), &dstSize);
    CheckRc(rc, PL_SUCCESS, L"PlFormatImage(mono->1)");
    Check(dst[0] == 'B' && dst[1] == 'M', L"PlFormatImage produced a BMP header (%c%c)", dst[0], dst[1]);
}
