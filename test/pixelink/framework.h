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
/// Shared scaffolding for the split PixeLINK test suite.  Each Pl* entry point
/// (or getter/setter pair) lives in its own translation unit under this folder
/// and registers one or more test cases with the PL_TEST() macro; the runner in
/// pixelink.cpp loads the DLL, binds the exports, then executes every registered
/// case.  This header exposes:
///
///   * PixelinkApi          - function-pointer table of every export, plus the
///                            X-macro (PL_API_LIST) and LoadApi() that fills it.
///   * the tiny assertion harness (Check / CheckRc / Report / ReportNoCamera).
///   * the test registry and the PL_TEST() registration macro.
///   * the camera fixture (ScopedCamera / OpenCamera) and shared DeviceOptions.
///
/// Tests reach the bound API through the global g_api and the run options
/// through g_opt; both are populated by the runner before any test executes.

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "../pixelink.h"
#include "../utils/log.h"

/// X-macro list of every exported entry point the suite binds and tests.
/// Used to build the function-pointer table and to load (and verify the
/// presence of) each export by name.
#define PL_API_LIST(X)           \
    X(PlGetNumberDevices)        \
    X(PlInitialize)              \
    X(PlUninitialize)            \
    X(PlEnumAvailableFrameRates) \
    X(PlGetCurrentFrameRate)     \
    X(PlSetCurrentFrameRate)     \
    X(PlGetDeviceInfo)           \
    X(PlGetFeature)              \
    X(PlSetFeature)              \
    X(PlGetPixelFormat)          \
    X(PlSetPixelFormat)          \
    X(PlGetPreviewWindow)        \
    X(PlSetPreviewWindow)        \
    X(PlResetPreviewWindow)      \
    X(PlSetPreviewState)         \
    X(PlStartPreview)            \
    X(PlStartVideoStream)        \
    X(PlStopVideoStream)         \
    X(PlReturnVideoData)         \
    X(PlGetSubWindowSettings)    \
    X(PlSetSubWindowSettings)    \
    X(PlGetTimeout)              \
    X(PlSetTimeout)              \
    X(PlSetTriggerMode)          \
    X(PlFormatImage)             \
    X(PlReadExtI2cRegister)      \
    X(PlWriteExtI2cRegister)

struct PixelinkApi {
#define PL_DECL_PTR(name) decltype(::name) *name;
    PL_API_LIST(PL_DECL_PTR)
#undef PL_DECL_PTR
};

/// Bind every export by name.  Returns false (and lists the offenders) if any
/// documented export is missing — which is itself a useful test of the DLL.
///
/// @param hDll  Loaded DLL handle.
/// @param api   Function-pointer table to populate.
/// @return true if all documented exports resolved, false if any are missing.
bool LoadApi(HMODULE hDll, PixelinkApi &api);

/// Number of entries in PL_API_LIST (every member of PixelinkApi is a pointer).
inline int PlApiCount() {
    return (int) (sizeof(PixelinkApi) / sizeof(void *));
}

/// Run options, parsed from the command line by the runner and shared with all
/// device-dependent tests through g_opt.
struct DeviceOptions {
    std::string filter = "Photonic Camera"; ///< FriendlyName to match / open
    ULONG serial = 1;
    ULONG deviceIndex = 0;
    bool doStream = true;  ///< run the video-stream cases
    bool doPreview = true; ///< run the preview cases (creates real windows)
    std::string comPort;   ///< serial port whose DTR line fires the exposure
                           ///< trigger (empty = none; acquisition free-runs)
};

/// Populated by the runner before any test executes.  g_api is re-bound against
/// a freshly loaded DLL before each test case runs (see RunPlTests), so that
/// DLL-global side effects (transport selection, enumerated device list, ...)
/// cannot leak from one test into the next.  g_dllPath is the path the DLL is
/// (re)loaded from.
extern PixelinkApi g_api;
extern DeviceOptions g_opt;
extern std::wstring g_dllPath;

/// Load the DLL named by g_dllPath.  Resolves the path to an absolute one and
/// loads with LOAD_WITH_ALTERED_SEARCH_PATH so the DLL's own directory is
/// searched for its dependencies; a plain LoadLibrary on a relative path
/// resolves dependencies against the exe directory / PATH only, which fails
/// with error 126 when the dependencies sit next to the DLL.
///
/// @return DLL handle on success, nullptr on failure (GetLastError() holds the reason).
HMODULE LoadPlDll();

/// Assertion harness

/// Hard assertion: logs PASS/FAIL and counts toward the suite total.
///
/// @param cond  Condition to test; false records a failure.
/// @param fmt   printf-style wide format string for the assertion message.
void Check(bool cond, const wchar_t *fmt, ...);

/// Assert an exact return code (hard PASS/FAIL).
///
/// @param actual    Actual return code received.
/// @param expected  Expected return code.
/// @param what      Label for the assertion message.
void CheckRc(PL_RETURN_CODE actual, PL_RETURN_CODE expected, const wchar_t *what);

/// Report a device-dependent return code for information; does not count toward
/// the pass/fail total (a fake/partial camera may legitimately not implement
/// it).
///
/// @param rc    Return code to report.
/// @param what  Label for the log message.
void Report(PL_RETURN_CODE rc, const wchar_t *what);

/// Note that a camera-dependent test could not obtain a camera.  Records a
/// failed assertion (a missing camera must fail the suite, not skip it);
/// returns false for `return ReportNoCamera();`.
///
/// @return false always (for use in `return ReportNoCamera();`).
bool ReportNoCamera();

/// Human-readable name for a return code.
///
/// @param rc  Return code to name.
/// @return Wide string name of the return code.
const wchar_t *RcName(PL_RETURN_CODE rc);

/// Total number of assertions checked so far (read by the runner to print the
/// summary).
///
/// @return Total check count.
int PlChecks();

/// Number of failed assertions so far (read by the runner to print the summary).
///
/// @return Failed check count.
int PlFailed();

/// Camera fixture

/// Open the camera described by g_opt.  Returns a handle, or nullptr on failure.
/// On the WDM/DirectShow transport PlInitialize treats its first argument as an
/// LPCSTR FriendlyName (see the PlInitialize notes in pixelink.h), so the
/// filter string is passed rather than a numeric serial.
///
/// @return Camera handle on success, nullptr on failure.
HANDLE OpenCamera();

/// Program a freshly-opened camera into a streamable state and return true if
/// it now reports a non-empty ROI.  A camera straight out of PlInitialize has
/// no video format selected, so PlStartVideoStream / PlStartPreview fail (the
/// device returns code 25) until a PlSet* call programs the geometry/format
/// and establishes the frame dimensions the capture buffers are sized from.
/// The streaming and preview tests must call this as part of their setup.
///
/// @param cam  Opened camera handle.
/// @return true if the camera now reports a non-empty ROI, false otherwise.
bool PrepareCameraForStreaming(HANDLE cam);

/// RAII wrapper: opens a camera on construction, PlUninitialize on destruction.
/// Use `if (!cam) { return ReportNoCamera(); }` to fail when none is available.
struct ScopedCamera {
    HANDLE h;
    ScopedCamera() : h(OpenCamera()) {
    }
    ~ScopedCamera() {
        if (h != nullptr) {
            g_api.PlUninitialize(&h);
        }
    }
    explicit operator bool() const {
        return h != nullptr;
    }
    operator HANDLE() const {
        return h;
    }

    ScopedCamera(const ScopedCamera &) = delete;
    ScopedCamera &operator=(const ScopedCamera &) = delete;
};

/// Test registry
typedef void (*PlTestFn)();

/// Register a test case (called automatically by PL_TEST at static-init time).
/// `group` orders/labels the output; `name` identifies the individual case.
///
/// @param group  Test group name (also used for stable sort).
/// @param name   Individual test case name.
/// @param fn     Test function pointer.
/// @return 0 (return value used only to trigger static-init registration).
int RegisterPlTest(const wchar_t *group, const wchar_t *name, PlTestFn fn);

/// Run registered test cases, grouped by `group` in stable order.  When
/// `filters` is empty every case runs; otherwise a case runs only if one of the
/// filters matches its group, its name, or its "group.name" (case-insensitive
/// substring match).
///
/// @param filters  Substring filters; empty means run all.
/// @return Number of test cases that were selected and run.
int RunPlTests(const std::vector<std::wstring> &filters);

/// Print every registered case as "group.name", one per line, in run order.
void ListPlTests();

#define PL_WIDEN_(x) L##x
#define PL_WIDEN(x)  PL_WIDEN_(x)

/// Define and self-register a test case.  Usage:
///   PL_TEST(framerate, CurrentFrameRate_GetSet) { ... }
#define PL_TEST(group, testname)                                                                        \
    static void testname();                                                                             \
    static int testname##_registered = RegisterPlTest(PL_WIDEN(#group), PL_WIDEN(#testname), testname); \
    static void testname()
