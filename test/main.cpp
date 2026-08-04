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
/// Unified entry point for the Photonic test executable.  It hosts three
/// independent test suites and dispatches to one of them based on the suite
/// selector on the command line:
///
///   test.exe --test-pixelink <path-to-pixelink.dll> [options]
///       Exercises every exported Pl* entry point in the given DLL
///       (implemented in pixelink.cpp).
///
///   test.exe --test-directshow [options]
///       Runs the DirectShow capture sweep (implemented in directshow.cpp).
///
///   test.exe --test-ioctl [options]
///       Checks the camera-head controller registers and drives the raw
///       Photonic capture IOCTLs (implemented in ioctl/capture.cpp).
///
/// The dispatcher also owns logging: it parses the shared -o/--output option,
/// opens the results file before dispatching, and closes it afterwards, so each
/// suite just calls Log() without managing the file itself.  The suite
/// selector and the -o/--output option are stripped from the argv forwarded to
/// the suite, which then parses only its own options.

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "../version.h"
#include "utils/log.h"

/// Suite entry points, implemented in their respective translation units.
int RunPixelinkTests(int argc, wchar_t *argv[]);   ///< pixelink.cpp
int RunDirectShowTests(int argc, wchar_t *argv[]); ///< directshow.cpp
int RunIoctlTests(int argc, wchar_t *argv[]);      ///< ioctl/capture.cpp

static void PrintTopUsage(const wchar_t *exe) {
    fwprintf(stderr,
             L"Usage: %s <suite> [options]\n"
             L"\n"
             L"Suites:\n"
             L"  --test-pixelink <path-to-pixelink.dll> [options]\n"
             L"        Test every exported Pl* entry point in the given DLL.\n"
             L"  --test-directshow [options]\n"
             L"        Run the DirectShow capture sweep.\n"
             L"  --test-ioctl [options]\n"
             L"        Check every camera-head controller register, then drive the raw\n"
             L"        Photonic capture IOCTLs (triggered snap, transmit-only imager,\n"
             L"        teardown and exclusion rules).\n"
             L"\n"
             L"Common options:\n"
             L"  -o, --output <file>  Tee all output to this results file (default: test-results.txt)\n"
             L"\n"
             L"Run a suite with -h/--help for its own options.\n",
             exe);
}

int wmain(int argc, wchar_t *argv[]) {
    if (argc < 2) {
        PrintTopUsage(argv[0]);
        return 1;
    }

    // Scan for the suite selector and the shared -o/--output option anywhere on
    // the command line; build a forwarding argv that preserves argv[0] but drops
    // the tokens consumed here, so each suite parses only its own options.
    int mode = 0; // 1 = pixelink, 2 = directshow, 3 = ioctl
    std::wstring outPath = L"test-results.txt";
    std::vector<wchar_t *> forward;
    forward.push_back(argv[0]);
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--test-pixelink") == 0) {
            mode = 1;
        } else if (wcscmp(argv[i], L"--test-directshow") == 0) {
            mode = 2;
        } else if (wcscmp(argv[i], L"--test-ioctl") == 0) {
            mode = 3;
        } else if (wcscmp(argv[i], L"-o") == 0 || wcscmp(argv[i], L"--output") == 0) {
            if (i + 1 >= argc) {
                fwprintf(stderr, L"Error: %s requires a value.\n", argv[i]);
                return 1;
            }
            outPath = argv[++i];
        } else {
            forward.push_back(argv[i]);
        }
    }

    if (mode == 0) {
        fwprintf(stderr, L"Error: choose a suite with --test-pixelink, --test-directshow or --test-ioctl.\n\n");
        PrintTopUsage(argv[0]);
        return 1;
    }

    // Tee all output to the results file as well as the console.
    if (!LogOpen(outPath.c_str())) {
        fwprintf(stderr,
                 L"Warning: could not open output file \"%s\"; "
                 L"logging to console only.\n",
                 outPath.c_str());
    }

    Log(INFO, L"Version %hs", PHOTONIC_VERSION_STRING);
    Log(INFO, L"Command line: %s", GetCommandLineW());

    int rc;
    if (mode == 1) {
        rc = RunPixelinkTests((int) forward.size(), forward.data());
    } else if (mode == 2) {
        rc = RunDirectShowTests((int) forward.size(), forward.data());
    } else {
        rc = RunIoctlTests((int) forward.size(), forward.data());
    }

    Log(INFO, L"Test suite finished with code %d.", rc);

    LogClose();
    return rc;
}
