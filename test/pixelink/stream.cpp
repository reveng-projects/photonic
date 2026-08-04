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
/// Video-stream entry points, kept separate from the preview tests:
///   PlStartVideoStream / PlReturnVideoData / PlStopVideoStream.
///
/// Gated by g_opt.doStream.

#include <vector>

#include "framework.h"

/// Bytes per pixel for a DCAM pixel-format code.  Conservative: unknown formats
/// fall back to 1 so a computed frame size can never exceed the real frame (too
/// small only yields a partial copy; too large over-reads the source and
/// faults).
static ULONG BytesPerPixel(ULONG pixelFormat) {
    switch (pixelFormat) {
        case 0:
            return 1; // Mono8
        case 1:
            return 2; // Mono16
        case 2:
            return 2; // YUV422 (UYVY)
        case 3:
            return 3; // RGB24
        case 4:
            return 3; // RGB24 (BGR)
        default:
            return 1;
    }
}

PL_TEST(stream, VideoStream) {
    // Cameraless contract for the three stream entry points.
    int dummy = 0;
    CheckRc(g_api.PlStartVideoStream(nullptr), PL_ERROR, L"PlStartVideoStream(NULL)");
    CheckRc(g_api.PlStopVideoStream(nullptr), PL_ERROR, L"PlStopVideoStream(NULL)");
    CheckRc(g_api.PlReturnVideoData(nullptr, 4, &dummy), PL_ERROR, L"PlReturnVideoData(NULL)");

    if (!g_opt.doStream) {
        Log(INFO, L"video-stream walk-through disabled (--no-stream).");
        return;
    }

    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }

    // A freshly-opened camera has no video format selected; program one (see
    // PrepareCameraForStreaming) or PlStartVideoStream is rejected (code 25).
    if (!PrepareCameraForStreaming(cam)) {
        Log(WARN, L"could not program a video format; skipping stream.");
        return;
    }

    // PlReturnVideoData copies exactly the requested byte count out of ONE
    // frame, with no clamp to the frame, so the count must equal the real
    // frame size.  Passing an arbitrary buffer capacity (e.g. 4 MB) over-reads
    // the source frame and faults.  Derive the size from the current geometry
    // and pixel format.
    ULONG decimX = 0, decimY = 0, offX = 0, offY = 0, width = 0, height = 0;
    g_api.PlGetSubWindowSettings(cam, &decimX, &decimY, &offX, &offY, &width, &height);
    ULONG pf = 0;
    g_api.PlGetPixelFormat(cam, &pf);
    ULONG frameBytes = width * height * BytesPerPixel(pf);

    PL_RETURN_CODE rc = g_api.PlStartVideoStream(cam);
    Report(rc, L"PlStartVideoStream");
    if (rc != PL_SUCCESS) {
        return;
    }

    if (frameBytes == 0) {
        Log(WARN, L"frame size unknown (geometry %lux%lu); skipping PlReturnVideoData", width, height);
    } else {
        Log(INFO, L"frame size %lu bytes (%lux%lu, %lu bpp)", frameBytes, width, height, BytesPerPixel(pf));
        std::vector<BYTE> frame(frameBytes);
        Report(g_api.PlReturnVideoData(cam, (ULONG) frame.size(), frame.data()), L"PlReturnVideoData");
    }
    Report(g_api.PlStopVideoStream(cam), L"PlStopVideoStream");
}
