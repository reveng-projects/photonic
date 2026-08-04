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
/// Implementation of the shared PixeLINK test scaffolding declared in
/// framework.h: the export binding table, the assertion harness, the camera
/// fixture and the test registry/runner.

#include "framework.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <vector>

/// Globals populated by the runner before any test executes.
PixelinkApi g_api;
DeviceOptions g_opt;
std::wstring g_dllPath;

HMODULE LoadPlDll() {
    // LOAD_WITH_ALTERED_SEARCH_PATH only redirects the dependency search when
    // the load path is absolute, so resolve g_dllPath first.
    std::wstring fullPath(MAX_PATH, L'\0');
    DWORD len = GetFullPathNameW(g_dllPath.c_str(), static_cast<DWORD>(fullPath.size()), &fullPath[0], nullptr);
    if (len > fullPath.size()) {
        fullPath.resize(len);
        len = GetFullPathNameW(g_dllPath.c_str(), static_cast<DWORD>(fullPath.size()), &fullPath[0], nullptr);
    }
    if (len == 0) {
        return nullptr;
    }
    fullPath.resize(len);
    return LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

/// Export binding
bool LoadApi(HMODULE hDll, PixelinkApi &api) {
    bool ok = true;
#define PL_LOAD_PTR(name)                                                         \
    api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(hDll, #name)); \
    if (api.name == nullptr) {                                                    \
        Log(ERROR, L"MISSING export: %hs", #name);                                \
        ok = false;                                                               \
    }
    PL_API_LIST(PL_LOAD_PTR)
#undef PL_LOAD_PTR
    return ok;
}

/// Assertion harness
static int g_checks = 0;
static int g_failed = 0;

void Check(bool cond, const wchar_t *fmt, ...) {
    g_checks++;
    if (!cond) {
        g_failed++;
    }
    // Assemble the caller's message into one line, then emit it at a level that
    // reflects the verdict (the Log() macro needs a literal level, so the
    // pass/fail choice goes through LogWrite directly).
    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf(msg, _countof(msg), fmt, args);
    va_end(args);
    msg[_countof(msg) - 1] = L'\0';
    LogWrite(cond ? LOGLEVEL_INFO : LOGLEVEL_ERROR, __FILE__, __LINE__, L"[%s] %s", cond ? L"PASS" : L"FAIL", msg);
}

const wchar_t *RcName(PL_RETURN_CODE rc) {
    switch (rc) {
        case PL_SUCCESS:
            return L"PL_SUCCESS";
        case PL_ERROR:
            return L"PL_ERROR";
        case PL_ERROR_INVALID_PARAM:
            return L"PL_ERROR_INVALID_PARAM";
        case PL_ERROR_DEVICE_NOT_FOUND:
            return L"PL_ERROR_DEVICE_NOT_FOUND";
        case PL_ERROR_WDM_INIT:
            return L"PL_ERROR_WDM_INIT";
        case PL_ERROR_INVALID_COUNT:
            return L"PL_ERROR_INVALID_COUNT";
        case PL_ERROR_OUT_OF_MEMORY:
            return L"PL_ERROR_OUT_OF_MEMORY";
        case PL_ERROR_NO_PREVIEW:
            return L"PL_ERROR_NO_PREVIEW";
        case PL_ERROR_NO_STREAM:
            return L"PL_ERROR_NO_STREAM";
        case PL_ERROR_NOT_PREPARED:
            return L"PL_ERROR_NOT_PREPARED";
        case PL_ERROR_UNSUPPORTED_XPORT:
            return L"PL_ERROR_UNSUPPORTED_XPORT";
        case PL_ERROR_TIMEOUT:
            return L"PL_ERROR_TIMEOUT";
        case PL_ERROR_OVERFLOW:
            return L"PL_ERROR_OVERFLOW";
        case PL_ERROR_UNSUPPORTED_FORMAT:
            return L"PL_ERROR_UNSUPPORTED_FORMAT";
        case PL_ERROR_FORMAT_UNAVAILABLE:
            return L"PL_ERROR_FORMAT_UNAVAILABLE";
        case PL_ERROR_SIZE_MISMATCH:
            return L"PL_ERROR_SIZE_MISMATCH";
        default:
            return L"PL_ERROR_<unknown>";
    }
}

void CheckRc(PL_RETURN_CODE actual, PL_RETURN_CODE expected, const wchar_t *what) {
    Check(actual == expected, L"%s -> %s (%d), expected %s (%d)", what, RcName(actual), actual, RcName(expected),
          expected);
}

void Report(PL_RETURN_CODE rc, const wchar_t *what) {
    Log(INFO, L"%s -> %s (%d)", what, RcName(rc), rc);
}

bool ReportNoCamera() {
    Check(false, L"no camera available; device-dependent checks cannot run.");
    return false;
}

int PlChecks() {
    return g_checks;
}
int PlFailed() {
    return g_failed;
}

/// Camera fixture
HANDLE OpenCamera() {
    const char *filter = g_opt.filter.c_str();
    HANDLE cam = nullptr;
    PL_RETURN_CODE rc = g_api.PlInitialize((ULONG) (ULONG_PTR) filter, g_opt.deviceIndex, &cam);
    if (rc != PL_SUCCESS || cam == nullptr) {
        return nullptr;
    }
    return cam;
}

bool PrepareCameraForStreaming(HANDLE cam) {
    // Re-apply the current pixel format and sub-window.  The values themselves
    // are unchanged, but the PlSet* calls are what actually select a video
    // format on the device and establish the frame geometry.  Without them
    // starting the stream is rejected.
    ULONG pf = 0;
    if (g_api.PlGetPixelFormat(cam, &pf) == PL_SUCCESS) {
        g_api.PlSetPixelFormat(cam, pf);
    }

    ULONG decimX = 0, decimY = 0, offX = 0, offY = 0, width = 0, height = 0;
    if (g_api.PlGetSubWindowSettings(cam, &decimX, &decimY, &offX, &offY, &width, &height) == PL_SUCCESS) {
        g_api.PlSetSubWindowSettings(cam, decimX, decimY, offX, offY, width, height);
    }

    // Explicitly select a video mode: enumerate the frame rates the camera can
    // deliver and set the first available one.  (The frame rate is the
    // selectable part of the DCAM video mode; enumeration only works once a
    // format has been programmed above.)
    float rates[64];
    ULONG nRates = (ULONG) (sizeof(rates) / sizeof(rates[0]));
    if (g_api.PlEnumAvailableFrameRates(cam, rates, &nRates) == PL_SUCCESS && nRates > 0) {
        Report(g_api.PlSetCurrentFrameRate(cam, rates[0]), L"PlSetCurrentFrameRate(rates[0])");
        Log(INFO, L"selected video mode: %.3g fps", rates[0]);
    } else {
        Log(INFO, L"no enumerable frame rates; keeping the default video mode.");
    }

    // Confirm the camera now reports a usable ROI.
    width = height = 0;
    g_api.PlGetSubWindowSettings(cam, &decimX, &decimY, &offX, &offY, &width, &height);
    return width != 0 && height != 0;
}

/// Test registry
struct PlTestCase {
    const wchar_t *group;
    const wchar_t *name;
    PlTestFn fn;
    int order; ///< registration order, for a stable sort
};

/// Function-local static avoids any static-initialization-order dependency
/// between this TU and the test TUs that register into it.
static std::vector<PlTestCase> &Registry() {
    static std::vector<PlTestCase> reg;
    return reg;
}

int RegisterPlTest(const wchar_t *group, const wchar_t *name, PlTestFn fn) {
    PlTestCase tc = {group, name, fn, (int) Registry().size()};
    Registry().push_back(tc);
    return 0;
}

/// Stable, grouped run order: alphabetical by group, registration order within.
static std::vector<PlTestCase> SortedTests() {
    std::vector<PlTestCase> tests = Registry();
    std::stable_sort(tests.begin(), tests.end(), [](const PlTestCase &a, const PlTestCase &b) {
        int c = wcscmp(a.group, b.group);
        if (c != 0) {
            return c < 0;
        }
        return a.order < b.order;
    });
    return tests;
}

/// Case-insensitive "does `hay` contain `needle`?".
static bool ContainsNoCase(const std::wstring &hay, const std::wstring &needle) {
    if (needle.empty()) {
        return true;
    }
    auto lower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t) towlower(c); });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::wstring::npos;
}

/// A case matches a filter if the filter is a (case-insensitive) substring of its
/// group, its name, or its fully-qualified "group.name".
static bool TestMatches(const PlTestCase &tc, const std::vector<std::wstring> &filters) {
    if (filters.empty()) {
        return true;
    }
    std::wstring qualified = std::wstring(tc.group) + L"." + tc.name;
    for (size_t i = 0; i < filters.size(); i++) {
        if (ContainsNoCase(tc.group, filters[i]) || ContainsNoCase(tc.name, filters[i]) ||
            ContainsNoCase(qualified, filters[i])) {
            return true;
        }
    }
    return false;
}

void ListPlTests() {
    std::vector<PlTestCase> tests = SortedTests();
    for (size_t i = 0; i < tests.size(); i++) {
        Log(INFO, L"%s.%s", tests[i].group, tests[i].name);
    }
}

int RunPlTests(const std::vector<std::wstring> &filters) {
    std::vector<PlTestCase> tests = SortedTests();

    int ran = 0;
    const wchar_t *currentGroup = nullptr;
    for (size_t i = 0; i < tests.size(); i++) {
        const PlTestCase &tc = tests[i];
        if (!TestMatches(tc, filters)) {
            continue;
        }
        ran++;
        if (currentGroup == nullptr || wcscmp(currentGroup, tc.group) != 0) {
            currentGroup = tc.group;
            Log(INFO, L"======== %s ========", currentGroup);
        }
        Log(INFO, L"--- %s ---", tc.name);

        // Load and bind a fresh copy of the DLL for this test, so DLL-global
        // state cannot leak between tests, then unload it afterwards.  Each
        // test fully tears down its camera (ScopedCamera / explicit
        // PlUninitialize) before returning, so no API pointer outlives the
        // FreeLibrary below.
        HMODULE hDll = LoadPlDll();
        if (hDll == nullptr) {
            Check(false, L"%s: LoadLibrary(\"%s\") failed (%lu)", tc.name, g_dllPath.c_str(), GetLastError());
            continue;
        }
        if (!LoadApi(hDll, g_api)) {
            Check(false, L"%s: DLL is missing required exports", tc.name);
            FreeLibrary(hDll);
            continue;
        }

        tc.fn();

        FreeLibrary(hDll);
    }
    return ran;
}
