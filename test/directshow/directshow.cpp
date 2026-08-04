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
/// Automated DirectShow sweep for the Photonic 1394 camera.
///
/// For every video format the capture filter advertises (via IAMStreamConfig)
/// and, within each, every standard DCAM frame rate that the format's interval
/// range allows, this program:
///   1. tears the render chain down (SetFormat is only legal on a free pin),
///   2. SetFormat()s the combination,
///   3. RenderStream()s the capture pin into the InspectRenderer pixel tap
///      (always fed the native, unconverted bytes) and, unless --headless,
///      renders the preview leg into a default video window,
///   4. runs the graph for a few seconds while pumping window/graph messages,
///   5. while it streams, exercises the IAMVideoProcAmp / IAMCameraControl image
///      controls (contrast, brightness, exposure, ...): for each supported
///      property it drives min/mid/max in manual mode, verifies the value
///      round-trips with the manual flag set, exercises auto mode if advertised,
///      then restores the original setting,
///   6. reports PASS/FAIL (SetFormat / RenderStream / Run HRESULTs, any EC_*
///      error event seen while running, and the image-control round-trip).
///
/// A summary table is printed at the end.
///
/// The sweep is driven by the DirectShowSweep class (sweep.h / sweep.cpp): it
/// owns the filter graph and the camera interfaces (so they are never threaded
/// through parameter lists) and runs a small pipeline -- EnumerateCapabilities()
/// gathers every advertised format, FilterCapabilities() narrows it to the
/// command-line selection, GetIntervals()/BuildCombinations() turns each
/// surviving format into the (format, frame-rate) Combinations to test, and
/// EvaluateCombo() runs one Combination.  The supporting pieces live alongside:
/// sweep_types.h (Options/Capability/Combination/ComboResult), image_controls.h
/// (the per-combination property checks), graph_utils.h (filter-graph plumbing),
/// format_names.h (media-type pretty-printing) and finddevice.h (device
/// selection and the raw-open diagnostics).
///
/// Usage:
///   directshow.exe [options]
///     -d, --device <substr>  Pick the capture device whose friendly name
///                            contains this (case-insensitive). Default: "Photonic".
///     -i, --device-index <n> Pick the n-th (0-based) device matching the name,
///                            so identically-named cameras can be told apart.
///                            Default: the first match.
///     -t, --seconds <n>      Dwell time per combination, in seconds. Default: 3.
///     -o, --output <file>    Results file (teed with the console).
///                            Default: "test-results.txt".
///     -m, --mode <index>     Test only this capture mode (the format-capability
///                            index reported by the capture filter). Default: all.
///     -f, --fps <value>      Test only this frame rate, in fps. Default: every
///                            standard DCAM rate that the format's range allows.
///         --headless         Never render the preview leg (no video window).
///                            The windowed default already runs without a
///                            window per combination on renderer failure.
///     -h, --help             Show this help and exit.
///
///   With no -m/-f the program sweeps every advertised format and, within each,
///   every standard DCAM frame rate. Supplying -m and/or -f pins a single
///   capture mode (format and/or frame rate) instead.
///
/// Build: links against strmiids/ole32/oleaut32 (see #pragma comment below).

#include <dshow.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "../utils/log.h"
#include "../utils/pngwriter.h"

#include "sweep.h"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// Output logging (Log / LogFlush) is shared with the other test suites; see
// log.h.  The results file is opened/closed by the dispatcher in main.cpp (the
// shared -o/--output option), so this suite only writes through Log().

using dsweep::DirectShowSweep;
using dsweep::Options;

/// Command-line parsing
///
/// @param exe  argv[0], used in the usage text.
static void PrintUsage(const wchar_t *exe) {
    fwprintf(stderr,
             L"Usage: %s [options]\n"
             L"  -d, --device <substr>  Capture device friendly-name substring (default: Photonic)\n"
             L"  -i, --device-index <n> Pick the n-th (0-based) device matching the name,\n"
             L"                         to disambiguate identically-named cameras (default: first)\n"
             L"  -t, --seconds <n>      Dwell time per combination, in seconds (default: 3)\n"
             L"  -m, --mode <index>     Test only this capture-mode (format) index (default: all)\n"
             L"  -f, --fps <value>      Test only this frame rate, in fps (default: all in range)\n"
             L"  -7, --test-format7     Test only the Format 7 (scalable) capabilities\n"
             L"  -l, --list-modes       Enumerate advertised modes and exit (no capture).\n"
             L"                         Honours -m/-7 and prints the -m/-f selector for\n"
             L"                         each mode/rate so one can be picked for a later run.\n"
             L"      --headless         Never show a video window. The windowed default\n"
             L"                         already falls back to this per combination if the\n"
             L"                         video renderer cannot start.\n"
             L"      --frame-header-check <true|false>\n"
             L"                         Verify the embedded per-frame test header (checksum/\n"
             L"                         index/feature values). A real camera does not generate\n"
             L"                         it, so pass false for one. Defaults to true when omitted.\n"
             L"      --dump-dir <dir>   Save the last delivered frame of each combination as a\n"
             L"                         PNG in this directory (created if missing). 16-bit\n"
             L"                         greyscale modes are saved losslessly; YUV modes as the\n"
             L"                         extracted luma plane. Default: no samples saved.\n"
             L"  -h, --help             Show this help and exit\n"
             L"\n"
             L"Output is teed to a results file via the shared -o/--output option,\n"
             L"which is handled before the suite runs.\n",
             exe);
}

/// Returns true on success, false if parsing failed or help was requested
/// (in which case the caller should exit).  *exitCode is set accordingly.
///
/// @param argc      Argument count from main.
/// @param argv      Argument vector from main.
/// @param opt       Receives the parsed options on success.
/// @param exitCode  Receives the exit code to use when returning false.
/// @return true on success, false if parsing failed or help was requested.
static bool ParseArgs(int argc, wchar_t *argv[], Options &opt, int &exitCode) {
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];

        // A switch that needs a value; fetch and validate the next argument.
        auto needValue = [&](const wchar_t *name) -> const wchar_t * {
            if (i + 1 >= argc) {
                fwprintf(stderr, L"Error: %s requires a value.\n", name);
                exitCode = 1;
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == L"-h" || arg == L"--help") {
            PrintUsage(argv[0]);
            exitCode = 0;
            return false;
        } else if (arg == L"-d" || arg == L"--device") {
            const wchar_t *v = needValue(L"--device");
            if (v == nullptr) {
                return false;
            }
            opt.deviceSubstr = v;
        } else if (arg == L"-i" || arg == L"--device-index") {
            const wchar_t *v = needValue(L"--device-index");
            if (v == nullptr) {
                return false;
            }
            opt.deviceIndex = _wtoi(v);
            if (opt.deviceIndex < 0) {
                fwprintf(stderr, L"Error: --device-index must be >= 0.\n");
                exitCode = 1;
                return false;
            }
        } else if (arg == L"-t" || arg == L"--seconds") {
            const wchar_t *v = needValue(L"--seconds");
            if (v == nullptr) {
                return false;
            }
            int secs = _wtoi(v);
            opt.durationMs = (secs > 0) ? (DWORD) (secs * 1000) : 3000;
        } else if (arg == L"-m" || arg == L"--mode") {
            const wchar_t *v = needValue(L"--mode");
            if (v == nullptr) {
                return false;
            }
            opt.modeIndex = _wtoi(v);
            if (opt.modeIndex < 0) {
                fwprintf(stderr, L"Error: --mode index must be >= 0.\n");
                exitCode = 1;
                return false;
            }
        } else if (arg == L"-f" || arg == L"--fps") {
            const wchar_t *v = needValue(L"--fps");
            if (v == nullptr) {
                return false;
            }
            opt.fps = _wtof(v);
            if (opt.fps <= 0.0) {
                fwprintf(stderr, L"Error: --fps must be a positive number.\n");
                exitCode = 1;
                return false;
            }
        } else if (arg == L"-7" || arg == L"--test-format7") {
            opt.f7Only = true;
        } else if (arg == L"-l" || arg == L"--list-modes") {
            opt.listModes = true;
        } else if (arg == L"--headless") {
            opt.headless = true;
        } else if (arg == L"--dump-dir") {
            const wchar_t *v = needValue(L"--dump-dir");
            if (v == nullptr) {
                return false;
            }
            opt.dumpDir = v;
        } else if (arg == L"--frame-header-check") {
            const wchar_t *v = needValue(L"--frame-header-check");
            if (v == nullptr) {
                return false;
            }
            std::wstring val = v;
            if (val == L"true" || val == L"1") {
                opt.frameHeaderChecks = true;
            } else if (val == L"false" || val == L"0") {
                opt.frameHeaderChecks = false;
            } else {
                fwprintf(stderr, L"Error: --frame-header-check expects true or false.\n");
                exitCode = 1;
                return false;
            }
        } else {
            fwprintf(stderr, L"Error: unknown argument \"%s\".\n", arg.c_str());
            PrintUsage(argv[0]);
            exitCode = 1;
            return false;
        }
    }
    return true;
}

/// Entry point for the DirectShow sweep.
///
/// Invoked by the unified dispatcher in main.cpp when the program is run with
/// --test-directshow.  argv[0] is preserved (the dispatcher strips only the
/// mode selector), so the usage text and option parsing below are unchanged.
///
/// @param argc  Argument count from main.
/// @param argv  Argument vector from main.
/// @return Process exit code: 0 on success, 1 on bad arguments, 2 on test failures.
int RunDirectShowTests(int argc, wchar_t *argv[]) {
    Options opt;
    int parseExit = 0;
    if (!ParseArgs(argc, argv, opt, parseExit)) {
        return parseExit;
    }

    if (!opt.dumpDir.empty() && !EnsureDumpDir(opt.dumpDir.c_str())) {
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        Log(ERROR, L"CoInitializeEx failed: 0x%08lX", hr);
        return 1;
    }

    int exitCode = 1;
    {
        // Scoped so the sweep (and all its COM interfaces) release before
        // CoUninitialize.
        DirectShowSweep sweep;
        if (sweep.Initialize(opt)) {
            exitCode = sweep.RunSweep();
        }
    }

    CoUninitialize();
    return exitCode;
}
