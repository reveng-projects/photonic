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
/// PixeLINK test suite runner (dispatched from main.cpp via --test-pixelink).
///
/// This translation unit owns only the plumbing: it parses the command line,
/// loads the DLL dynamically (LoadLibrary/GetProcAddress) from the path given on
/// the command line, binds every exported Pl* entry point, then runs every test
/// case registered with PL_TEST().  The test cases themselves live one feature
/// per file under pixelink/ (framerate.cpp, pixelformat.cpp, stream.cpp,
/// preview.cpp, ...); see pixelink/framework.h for the scaffolding.
///
/// Organising principle for the cases:
///   * Cameraless entry points (PlFormatImage, PlGetNumberDevices parameter
///     validation, every NULL-handle contract) get one deterministic case each.
///   * Camera-dependent entry points open the configured camera, exercise the
///     call, check the result, then tear down.  Getters and setters are grouped
///     into a single case per pair so the round-trip can be asserted.
///   * Streaming and preview each live in their own file and case.

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "../utils/log.h"
#include "../utils/util.h"
#include "framework.h"

/// PixeLINK suite entry point
static void PrintPixelinkUsage(const wchar_t *exe) {
    fwprintf(stderr,
             L"Usage: %s --test-pixelink <path-to-pixelink.dll> [options]\n"
             L"  --filter <name>     Device FriendlyName to match in PlGetNumberDevices\n"
             L"                      (default: \"Photonic Camera\")\n"
             L"  --device-index <n>  0-based device index to open (default: 0)\n"
             L"  --serial <n>        Non-zero serial number for PlInitialize (default: 1)\n"
             L"  --no-preview        Skip the preview-window walk-through\n"
             L"  --no-stream         Skip the video-stream walk-through\n"
             L"  --com-port <port>   Serial port (e.g. COM1) whose DTR line is wired to the\n"
             L"                      camera trigger input (default: none; the acquisition\n"
             L"                      test then free-runs instead of external-triggering)\n"
             L"  --test <name>       Run only matching test case(s); matches a group,\n"
             L"                      a case name, or \"group.name\" (case-insensitive\n"
             L"                      substring).  May be repeated to select several.\n"
             L"  --list-tests        List every registered test case and exit.\n"
             L"  -h, --help          Show this help and exit\n"
             L"\n"
             L"Output is teed to a results file via the shared -o/--output option,\n"
             L"which is handled before the suite runs.\n",
             exe);
}

/// Entry point for the PixeLINK suite; called by the dispatcher in main.cpp.
int RunPixelinkTests(int argc, wchar_t *argv[]) {
    // argv layout (the dispatcher leaves argv[0] = exe and strips the mode):
    //   argv[1] = <path-to-pixelink.dll>, followed by options.
    const wchar_t *dllPath = nullptr;
    std::vector<std::wstring> testFilters;
    bool listTests = false;

    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        auto needValue = [&](const wchar_t *name) -> const wchar_t * {
            if (i + 1 >= argc) {
                fwprintf(stderr, L"Error: %s requires a value.\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == L"-h" || arg == L"--help") {
            PrintPixelinkUsage(argv[0]);
            return 0;
        } else if (arg == L"--filter") {
            const wchar_t *v = needValue(L"--filter");
            if (v == nullptr) {
                return 1;
            }
            g_opt.filter = WideToString(v);
        } else if (arg == L"--device-index") {
            const wchar_t *v = needValue(L"--device-index");
            if (v == nullptr) {
                return 1;
            }
            g_opt.deviceIndex = (ULONG) _wtoi(v);
        } else if (arg == L"--serial") {
            const wchar_t *v = needValue(L"--serial");
            if (v == nullptr) {
                return 1;
            }
            g_opt.serial = (ULONG) _wtoi(v);
        } else if (arg == L"--preview") {
            g_opt.doPreview = true;
        } else if (arg == L"--no-preview") {
            g_opt.doPreview = false;
        } else if (arg == L"--stream") {
            g_opt.doStream = true;
        } else if (arg == L"--no-stream") {
            g_opt.doStream = false;
        } else if (arg == L"--com-port") {
            const wchar_t *v = needValue(L"--com-port");
            if (v == nullptr) {
                return 1;
            }
            g_opt.comPort = WideToString(v);
        } else if (arg == L"--test") {
            const wchar_t *v = needValue(L"--test");
            if (v == nullptr) {
                return 1;
            }
            testFilters.push_back(v);
        } else if (arg == L"--list-tests") {
            listTests = true;
        } else if (!arg.empty() && arg[0] == L'-') {
            fwprintf(stderr, L"Error: unknown argument \"%s\".\n", arg.c_str());
            PrintPixelinkUsage(argv[0]);
            return 1;
        } else if (dllPath == nullptr) {
            dllPath = argv[i];
        } else {
            fwprintf(stderr, L"Error: unexpected argument \"%s\".\n", arg.c_str());
            return 1;
        }
    }

    // Listing the registered cases needs no DLL or camera, so handle it before
    // requiring the DLL path.
    if (listTests) {
        ListPlTests();
        return 0;
    }

    if (dllPath == nullptr) {
        fwprintf(stderr, L"Error: path to pixelink.dll is required.\n");
        PrintPixelinkUsage(argv[0]);
        return 1;
    }

    Log(INFO, L"Loading PixeLINK API from: %s", dllPath);
    g_dllPath = dllPath;

    // One-time sanity load: verify every documented export resolves, then
    // unload.  Each test reloads the DLL itself (see RunPlTests) so its
    // global state starts clean.
    HMODULE hDll = LoadPlDll();
    if (hDll == nullptr) {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    bool allBound = LoadApi(hDll, g_api);
    Check(allBound, L"all %d documented exports resolved", PlApiCount());
    FreeLibrary(hDll);
    if (!allBound) {
        Log(ERROR, L"Aborting: the DLL is missing required exports.");
        return 2;
    }

    // Run the selected test cases (grouped by feature), each against a freshly
    // loaded copy of the DLL.  With no --test filter this is every case.
    int ran = RunPlTests(testFilters);
    if (ran == 0 && !testFilters.empty()) {
        Log(ERROR, L"No test case matched the given --test filter(s).");
        return 1;
    }

    Log(INFO, L"==================== SUMMARY ====================");
    Log(INFO, L"%d/%d assertions passed.", PlChecks() - PlFailed(), PlChecks());
    Log(INFO, L"================================================");
    return PlFailed() == 0 ? 0 : 2;
}
