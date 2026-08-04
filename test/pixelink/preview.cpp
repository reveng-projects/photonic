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
/// Preview-window entry points, kept separate from the streaming tests:
///   PlStartPreview / PlGetPreviewWindow / PlSetPreviewWindow /
///   PlResetPreviewWindow / PlSetPreviewState.
///
/// The camera walk-through creates real windows and is gated by g_opt.doPreview.

#include <vector>

#include "framework.h"

/// Human-readable name for a PixeLINK pixel-format ("color mode") code.
static const wchar_t *ColorModeName(ULONG pf) {
    switch (pf) {
        case 0:
            return L"Mono8";
        case 1:
            return L"Mono16";
        case 2:
            return L"YUV422";
        case 3:
            return L"RGB24";
        case 4:
            return L"BGR24";
        default:
            return L"?";
    }
}

static void PumpMessages(DWORD ms) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(20);
    }
}

/// Create a top-level host window to parent the preview child into.  The
/// PixeLINK preview MUST be created as a WS_CHILD window: with an overlapped /
/// pop-up style, preview window creation fails and PlStartPreview returns
/// PL_ERROR_INVALID_COUNT (7).  A child window also requires a valid parent,
/// so the test supplies one.
static HWND CreateHostWindow(int clientW, int clientH) {
    static const wchar_t kClass[] = L"PixelinkTestHost";
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); // ignore "already registered" on re-entry

    // Size the window so its CLIENT area is exactly clientW x clientH, so the
    // preview child (placed at 0,0 with the same size) fills it with no border.
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    RECT rc = {0, 0, clientW, clientH};
    AdjustWindowRect(&rc, style, FALSE);
    return CreateWindowExW(0, kClass, L"Pixelink preview host", style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                           rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

/// NULL-handle contract for every preview entry point (cameraless).
PL_TEST(preview, Preview_NullHandle) {
    RECT rc = {0};
    POINT pt = {0};
    CheckRc(g_api.PlGetPreviewWindow(nullptr, &rc), PL_ERROR, L"PlGetPreviewWindow(NULL)");
    CheckRc(g_api.PlSetPreviewWindow(nullptr, &pt), PL_ERROR, L"PlSetPreviewWindow(NULL)");
    CheckRc(g_api.PlResetPreviewWindow(nullptr), PL_ERROR, L"PlResetPreviewWindow(NULL)");
    CheckRc(g_api.PlSetPreviewState(nullptr, PL_PREVIEW_STOP), PL_ERROR, L"PlSetPreviewState(NULL)");
    CheckRc(g_api.PlStartPreview(nullptr, "t", 0, 0, 0, 0, 0, nullptr, 0), PL_ERROR, L"PlStartPreview(NULL)");
}

/// Camera walk-through: start a preview, query/move/reset its window, then step
/// through the valid preview states.
PL_TEST(preview, Preview_Walkthrough) {
    if (!g_opt.doPreview) {
        Log(INFO, L"preview walk-through disabled (--no-preview).");
        return;
    }

    ScopedCamera cam;
    if (!cam) {
        ReportNoCamera();
        return;
    }

    // A freshly-opened camera has no video format selected; program one (see
    // PrepareCameraForStreaming) or PlStartPreview fails (code 25) and the
    // preview window stays blank.
    if (!PrepareCameraForStreaming(cam)) {
        Log(WARN, L"could not program a video format; skipping preview.");
        return;
    }

    // Size the preview to the CURRENT streamed image, not the native sensor.
    // Passing 0xFFFFFFFF sizes the preview rect to the native sensor (e.g.
    // 1280x960), which is larger than the active ROI (e.g. 1024x768), so the
    // window dwarfs the image.  Query the current ROI and pass it
    // explicitly.  (Explicit dimensions are used verbatim.)
    ULONG decimX = 0, decimY = 0, offX = 0, offY = 0, imgW = 0, imgH = 0;
    g_api.PlGetSubWindowSettings(cam, &decimX, &decimY, &offX, &offY, &imgW, &imgH);

    // Place the preview child at (kMargin, kMargin) and size the host client
    // area to the image plus an equal margin on every side, so the video is
    // centered in the window.  Fall back to the native sensor size if the ROI
    // is unavailable.
    const int kMargin = 32;
    int imgPxW = 1280, imgPxH = 960;
    if (imgW != 0 && imgH != 0) {
        imgPxW = (int) imgW;
        imgPxH = (int) imgH;
    } else {
        imgW = 0xFFFFFFFF; // use the native sensor size
        imgH = 0xFFFFFFFF;
    }

    HWND host = CreateHostWindow(imgPxW + 2 * kMargin, imgPxH + 2 * kMargin);
    if (host == nullptr) {
        Log(ERROR, L"failed to create host window (%lu); skipping preview.", GetLastError());
        return;
    }
    PumpMessages(100);

    // The preview is a WS_CHILD window of the host, placed at (kMargin,
    // kMargin) so the video is centered.  windowTitle must be non-NULL and
    // controlId is an unused child id.  NOTE the geometry argument order is
    // (width, height).  Passing them swapped renders the image transposed
    // (narrow/tall).
    PL_RETURN_CODE rc =
        g_api.PlStartPreview(cam, "Pixelink preview", WS_CHILD | WS_VISIBLE, kMargin, kMargin, imgW, imgH, host, 0);
    Report(rc, L"PlStartPreview");
    PumpMessages(5000);

    RECT r = {0};
    Report(g_api.PlGetPreviewWindow(cam, &r), L"PlGetPreviewWindow");
    Log(INFO, L"preview rect: (%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom);

    // Exercise the move/reset APIs at the same (kMargin, kMargin) position so
    // the video stays centered and never visibly shifts.  (PlResetPreviewWindow
    // restores the origin last set through PlSetPreviewWindow, NOT the
    // PlStartPreview position, so a different point here would leave it
    // offset.)
    POINT pos = {kMargin, kMargin};
    Report(g_api.PlSetPreviewWindow(cam, &pos), L"PlSetPreviewWindow(centered)");
    Report(g_api.PlResetPreviewWindow(cam), L"PlResetPreviewWindow");

    // Preview state walk-through.  Only values 0/2/3 are valid; value 1 hangs
    // the caller (see PL_PREVIEW_STATE in pixelink.h), so start->pause->resume->
    // stop is expressed with START(2)/PAUSE(3)/START(2)/STOP(0).
    Report(g_api.PlSetPreviewState(cam, PL_PREVIEW_START), L"PlSetPreviewState(START)");
    PumpMessages(2000);
    Report(g_api.PlSetPreviewState(cam, PL_PREVIEW_PAUSE), L"PlSetPreviewState(PAUSE)");
    PumpMessages(2000);
    Report(g_api.PlSetPreviewState(cam, PL_PREVIEW_START), L"PlSetPreviewState(RESUME)");
    PumpMessages(2000);
    Report(g_api.PlSetPreviewState(cam, PL_PREVIEW_STOP), L"PlSetPreviewState(STOP)");

    DestroyWindow(host);
    PumpMessages(100);
}
