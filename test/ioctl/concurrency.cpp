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
/// Concurrency tests for the driver's Photonic IOCTL interface.  The hooked
/// IOCTL path serializes the capture-slot lifecycle on a per-device mutex
/// (PHOTONIC_DEVICE_EXTENSION.InterfaceMutex); these tests hammer the verbs
/// from several threads to exercise that serialization where the DLL's
/// single-threaded usage never would:
///
///   1. Prepare/unprepare storm: N threads loop PREPARE_VIDEO / UNPREPARE_VIDEO
///      on one handle.  Racing prepares and unprepares used to double-free the
///      slot or orphan a whole capture engine.
///   2. Lifecycle storm: concurrent start/stop, map/unmap, transfer-info +
///      event registration, SW_TRIGGER and prepare/unprepare against a live
///      stream.  Covers the stop-vs-unmap and start/start windows that used to
///      free ring MDLs still attached to the bus or double-allocate the
///      isochronous resources.
///   3. Second-handle ownership: a slot prepared on handle A must reject
///      unprepare and re-prepare from handle B, and closing B (its cleanup)
///      must not release A's slot.
///   4. Handle-close storm: worker threads hammer verbs on a handle while the
///      main thread closes it, racing IRP_MJ_CLEANUP's slot release against
///      in-flight handlers.
///   5. Exclusion race: DirectShow pin connect/disconnect racing an IOCTL
///      prepare/unprepare loop, exercising the check-and-publish window of
///      the two interfaces' mutual exclusion.
///
/// Individual verb results inside a storm are deliberately ignored: with the
/// interleaving unknown, any of them may fail benignly.  What each storm
/// asserts is that every thread finishes (a stuck thread means a deadlocked or
/// wedged driver), that the slot state is consistent afterwards, and that a
/// full prepare / map / start / frame-delivery / teardown cycle still works,
/// which a leaked isochronous channel, leaked bandwidth or a corrupted slot
/// would fail.  Memory-safety violations (the crashes these races used to
/// cause) surface as bugchecks under Driver Verifier rather than as test
/// failures here.

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "../../common/camera_regs.h"
#include "../directshow/comptr.h"
#include "../directshow/finddevice.h"
#include "../utils/log.h"
#include "concurrency.h"
#include "i2c.h"
#include "ioctl.h"

/// Pass/fail bookkeeping, reported back to the suite through RunConcurrencyTests.
static int g_passed;
static int g_failed;

static void Check(bool ok, const wchar_t *what) {
    if (ok) {
        g_passed++;
        Log(INFO, L"PASS %s", what);
    } else {
        g_failed++;
        Log(ERROR, L"FAIL %s", what);
    }
}

/// How long to wait for storm threads before declaring the driver wedged.  The
/// slowest verbs block for a few seconds inside the driver (SW_TRIGGER's listen
/// wait, register-access timeouts), so this must be generous; a healthy driver
/// finishes orders of magnitude sooner.
static const DWORD STORM_JOIN_TIMEOUT_MS = 30000;

/// The greyscale coding every storm pins so the prepared frame size stays
/// deterministic (PHOTONIC_DCAM_PIX_MONO8 in the driver's kernel-only header).
static const UINT32 STORM_PIXEL_FORMAT_MONO8 = 0;

/// Ring depth used by the storms and the health check.
static const UINT32 STORM_FRAME_COUNT = 2;

/// Shared state of one running storm.
namespace {

struct StormContext {
    HANDLE h = INVALID_HANDLE_VALUE; ///< Device handle the threads hammer.
    volatile LONG stop = 0;          ///< Set by the main thread to end the storm.
    UINT32 frameSize = 0;            ///< Prepared frame size for the map thread.
};

} // namespace

/// Pin MONO8 and return the full-sensor frame size the driver will compute at
/// prepare, or 0 on failure.  Same derivation as the main suite's
/// SnapFrameSize.
static UINT32 StormFrameSize(HANDLE h) {
    UINT32 current = ~0u;
    if (!PhotonicIoctlSetPixelFormat(h, STORM_PIXEL_FORMAT_MONO8) || !PhotonicIoctlGetPixelFormat(h, &current) ||
        current != STORM_PIXEL_FORMAT_MONO8) {
        Log(ERROR, L"cannot pin the pixel format to MONO8 (got %u)", current);
        return 0;
    }
    return CAMDEF_SENSOR_WIDTH * CAMDEF_SENSOR_HEIGHT;
}

/// Run a storm: launch the given thread routines against one StormContext,
/// let them hammer for stormSeconds, then stop and join with a timeout.  A
/// join timeout means at least one thread is stuck inside the driver -- the
/// deadlock/hang signature these tests exist to catch -- and fails the storm.
/// Returns false on that timeout; the stuck threads are left running (they
/// cannot be safely killed while blocked in kernel mode).
static bool RunStorm(StormContext &ctx, const LPTHREAD_START_ROUTINE *routines, DWORD count, int stormSeconds,
                     const wchar_t *what) {
    std::vector<HANDLE> threads;
    wchar_t msg[128];

    ctx.stop = 0;
    for (DWORD i = 0; i < count; i++) {
        HANDLE t = CreateThread(nullptr, 0, routines[i], &ctx, 0, nullptr);
        if (t == nullptr) {
            swprintf(msg, _countof(msg), L"%s: CreateThread %lu", what, i);
            Check(false, msg);
            break;
        }
        threads.push_back(t);
    }

    Sleep((DWORD) stormSeconds * 1000);
    InterlockedExchange(&ctx.stop, 1);

    bool joined = true;
    if (!threads.empty()) {
        DWORD wr = WaitForMultipleObjects((DWORD) threads.size(), threads.data(), TRUE, STORM_JOIN_TIMEOUT_MS);
        joined = wr < WAIT_OBJECT_0 + threads.size();
    }
    swprintf(msg, _countof(msg), L"%s: all %lu threads finished (stuck threads mean a wedged driver)", what,
             (ULONG) threads.size());
    Check(joined && threads.size() == count, msg);

    for (HANDLE t : threads) {
        // A stuck thread's handle leaks with it; the process is failing anyway.
        if (joined) {
            CloseHandle(t);
        }
    }
    return joined;
}

/// Post-storm health check: a complete prepare / map / start / frame-delivery /
/// teardown cycle on a quiesced device.  This is what a storm-induced leak
/// breaks: a leaked isochronous channel or bandwidth makes the start fail, a
/// corrupted slot fails the prepare or map, and a wedged engine delivers no
/// frame.  The head is put in free-run mode so frames flow unprompted.
static void VerifyStreamingHealthy(HANDLE h, const wchar_t *phase) {
    wchar_t what[128];
    bool delivered = false;

    // Quiesce whatever state the storm left behind.  Every call may fail
    // benignly; only the cycle below is asserted.
    PhotonicIoctlStopVideo(h);
    PhotonicIoctlUnmapVideoFrame(h);
    PhotonicIoctlUnprepareVideo(h);

    swprintf(what, _countof(what), L"%s: head back in free-run mode", phase);
    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN) != FALSE, what);

    UINT32 frameSize = StormFrameSize(h);
    BYTE *frames = frameSize == 0 ? nullptr
                                  : (BYTE *) VirtualAlloc(nullptr, (SIZE_T) STORM_FRAME_COUNT * frameSize,
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (frames != nullptr && hEvent != nullptr) {
        bool up = PhotonicIoctlPrepareVideo(h, STORM_FRAME_COUNT) != FALSE &&
                  PhotonicIoctlMapVideoFrame(h, STORM_FRAME_COUNT, frameSize, (UINT32) (ULONG_PTR) frames) != FALSE &&
                  PhotonicIoctlStartVideo(h, 1) != FALSE;
        swprintf(what, _countof(what), L"%s: PREPARE + MAP(%u) + START_VIDEO(1) after the storm", phase,
                 STORM_FRAME_COUNT);
        Check(up, what);

        if (up && PhotonicIoctlRegisterEvent(h, hEvent, 0) != FALSE) {
            if (WaitForSingleObject(hEvent, 5000) == WAIT_OBJECT_0) {
                UINT32 index = ~0u, total = 0;
                delivered = PhotonicIoctlGetTransferInfo(h, &index, &total) != FALSE && total > 0;
            } else {
                PhotonicIoctlUnregisterEvent(h, hEvent, 0);
            }
        }
        swprintf(what, _countof(what), L"%s: frame delivered after the storm", phase);
        Check(delivered, what);

        PhotonicIoctlStopVideo(h);
        PhotonicIoctlUnmapVideoFrame(h);
        swprintf(what, _countof(what), L"%s: clean teardown after the storm", phase);
        Check(PhotonicIoctlUnprepareVideo(h) != FALSE, what);
    } else {
        swprintf(what, _countof(what), L"%s: health-check setup (frame size / ring / event)", phase);
        Check(false, what);
    }

    if (hEvent != nullptr) {
        CloseHandle(hEvent);
    }
    if (frames != nullptr) {
        VirtualFree(frames, 0, MEM_RELEASE);
    }
}

/// Storm thread routines.  Each loops its verbs until ctx->stop; individual
/// results are ignored (any call may fail benignly depending on the
/// interleaving).  Sleep(0) yields so the threads interleave rather than one
/// monopolizing the driver's serialization.

static DWORD WINAPI StormPrepareUnprepare(LPVOID param) {
    StormContext *ctx = (StormContext *) param;
    while (!ctx->stop) {
        PhotonicIoctlPrepareVideo(ctx->h, STORM_FRAME_COUNT);
        Sleep(0);
        PhotonicIoctlUnprepareVideo(ctx->h);
        Sleep(0);
    }
    return 0;
}

static DWORD WINAPI StormStartStop(LPVOID param) {
    StormContext *ctx = (StormContext *) param;
    while (!ctx->stop) {
        PhotonicIoctlStartVideo(ctx->h, 1);
        Sleep(0);
        PhotonicIoctlStopVideo(ctx->h);
        Sleep(0);
    }
    return 0;
}

/// Maps its own ring into the shared slot and unmaps it again.  Racing another
/// thread's unprepare exercises the map-vs-release window that used to leave
/// user pages locked forever, and stop-vs-unmap the window that used to free
/// MDLs the bus driver was still detaching.
static DWORD WINAPI StormMapUnmap(LPVOID param) {
    StormContext *ctx = (StormContext *) param;
    BYTE *frames = (BYTE *) VirtualAlloc(nullptr, (SIZE_T) STORM_FRAME_COUNT * ctx->frameSize, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
    if (frames == nullptr) {
        return 1;
    }
    while (!ctx->stop) {
        PhotonicIoctlMapVideoFrame(ctx->h, STORM_FRAME_COUNT, ctx->frameSize, (UINT32) (ULONG_PTR) frames);
        Sleep(0);
        PhotonicIoctlUnmapVideoFrame(ctx->h);
        Sleep(0);
    }
    // The pages must be unlocked before the buffer is freed; whichever of
    // this unmap and a racing release runs last unlocks them.
    PhotonicIoctlUnmapVideoFrame(ctx->h);
    VirtualFree(frames, 0, MEM_RELEASE);
    return 0;
}

/// Readers and event registration against the slot other threads are tearing
/// down: GET_TRANSFER_INFO dereferences the slot, REGISTER_EVENT references a
/// kernel event into it (racing a release used to orphan that reference), and
/// the unregister reclaims it when no frame retires it first.
static DWORD WINAPI StormReadersAndEvents(LPVOID param) {
    StormContext *ctx = (StormContext *) param;
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (!ctx->stop) {
        UINT32 index = 0, total = 0;
        PhotonicIoctlGetTransferInfo(ctx->h, &index, &total);
        if (hEvent != nullptr && PhotonicIoctlRegisterEvent(ctx->h, hEvent, 0)) {
            Sleep(0);
            PhotonicIoctlUnregisterEvent(ctx->h, hEvent, 0);
        }
        Sleep(0);
    }
    if (hEvent != nullptr) {
        CloseHandle(hEvent);
    }
    return 0;
}

/// SW_TRIGGER blocks inside the driver (up to 1 s on the deferred-listen wait)
/// while other threads release the engine it waits on -- the window that used
/// to be a use-after-free of the capture slot.
static DWORD WINAPI StormSwTrigger(LPVOID param) {
    StormContext *ctx = (StormContext *) param;
    while (!ctx->stop) {
        PhotonicIoctlSwTrigger(ctx->h);
        Sleep(0);
    }
    return 0;
}

/// Test 1: prepare/unprepare storm on one handle.
static void TestPrepareUnprepareStorm(HANDLE h, int stormSeconds) {
    StormContext ctx;
    ctx.h = h;

    static const LPTHREAD_START_ROUTINE routines[] = {
        StormPrepareUnprepare,
        StormPrepareUnprepare,
        StormPrepareUnprepare,
        StormPrepareUnprepare,
    };
    if (RunStorm(ctx, routines, _countof(routines), stormSeconds, L"prepare storm")) {
        VerifyStreamingHealthy(h, L"prepare storm");
    }
}

/// Test 2: full lifecycle storm against a live stream.
static void TestLifecycleStorm(HANDLE h, int stormSeconds) {
    StormContext ctx;
    ctx.h = h;
    ctx.frameSize = StormFrameSize(h);
    if (ctx.frameSize == 0) {
        Check(false, L"lifecycle storm: frame size resolved");
        return;
    }

    // Free-run so started intervals actually deliver frames into the ring,
    // keeping the delivery DPC path busy under the storm.
    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN) != FALSE, L"lifecycle storm: head in free-run mode");

    static const LPTHREAD_START_ROUTINE routines[] = {
        StormPrepareUnprepare, StormStartStop, StormMapUnmap, StormReadersAndEvents, StormSwTrigger,
    };
    if (RunStorm(ctx, routines, _countof(routines), stormSeconds, L"lifecycle storm")) {
        VerifyStreamingHealthy(h, L"lifecycle storm");
    }
}

/// Test 3: ownership rules across two handles (deterministic, no threads).
static void TestSecondHandleOwnership(HANDLE hA, const std::wstring &devicePath) {
    HANDLE hB = DsOpenDevicePath(devicePath);
    if (hB == INVALID_HANDLE_VALUE) {
        Check(false, L"ownership: second device handle opened");
        return;
    }
    Check(true, L"ownership: second device handle opened");

    Check(PhotonicIoctlPrepareVideo(hA, STORM_FRAME_COUNT) != FALSE, L"ownership: PREPARE_VIDEO on handle A");

    // The slot belongs to A's file object: B may neither release it nor
    // rebuild it out from under A.
    Check(PhotonicIoctlUnprepareVideo(hB) == FALSE, L"ownership: UNPREPARE_VIDEO from handle B fails");
    Check(PhotonicIoctlPrepareVideo(hB, STORM_FRAME_COUNT) == FALSE,
          L"ownership: re-PREPARE_VIDEO from handle B fails");
    Check(PhotonicIoctlPrepareImager(hB) == FALSE, L"ownership: PREPARE_IMAGER from handle B fails");

    // Closing B sends its cleanup; B never owned the slot, so the cleanup
    // must leave A's slot in place.  PREPARE_IMAGER failing on A proves the
    // slot still exists; it would succeed on a freed slot.
    CloseHandle(hB);
    Check(PhotonicIoctlPrepareImager(hA) == FALSE, L"ownership: handle B close left A's slot in place");

    Check(PhotonicIoctlUnprepareVideo(hA) != FALSE, L"ownership: UNPREPARE_VIDEO on handle A");
}

/// Test 4: close a handle while worker threads still hammer it, racing the
/// cleanup hook's slot release against in-flight handlers.  Several rounds,
/// each on a fresh handle; the workers' calls fail with an invalid handle once
/// the close lands, which ends their loops.
namespace {

struct CloseStormContext {
    HANDLE h = INVALID_HANDLE_VALUE;
    volatile LONG stop = 0;
};

} // namespace

static DWORD WINAPI CloseStormWorker(LPVOID param) {
    CloseStormContext *ctx = (CloseStormContext *) param;
    while (!ctx->stop) {
        if (!PhotonicIoctlPrepareVideo(ctx->h, STORM_FRAME_COUNT) && GetLastError() == ERROR_INVALID_HANDLE) {
            break;
        }
        UINT32 index = 0, total = 0;
        PhotonicIoctlGetTransferInfo(ctx->h, &index, &total);
        PhotonicIoctlStartVideo(ctx->h, 1);
        PhotonicIoctlUnprepareVideo(ctx->h);
        Sleep(0);
    }
    return 0;
}

static void TestHandleCloseStorm(HANDLE h, const std::wstring &devicePath) {
    const int rounds = 5;
    wchar_t what[128];
    int completed = 0;

    for (int round = 0; round < rounds; round++) {
        CloseStormContext ctx;
        ctx.h = DsOpenDevicePath(devicePath);
        if (ctx.h == INVALID_HANDLE_VALUE) {
            break;
        }

        HANDLE threads[2] = {};
        threads[0] = CreateThread(nullptr, 0, CloseStormWorker, &ctx, 0, nullptr);
        threads[1] = CreateThread(nullptr, 0, CloseStormWorker, &ctx, 0, nullptr);
        if (threads[0] == nullptr || threads[1] == nullptr) {
            InterlockedExchange(&ctx.stop, 1);
        }

        // Let the workers get in flight, then close the handle under them.
        Sleep(100);
        CloseHandle(ctx.h);
        InterlockedExchange(&ctx.stop, 1);

        bool joined = true;
        for (HANDLE t : threads) {
            if (t != nullptr) {
                joined = WaitForSingleObject(t, STORM_JOIN_TIMEOUT_MS) == WAIT_OBJECT_0 && joined;
                CloseHandle(t);
            }
        }
        if (!joined || threads[0] == nullptr || threads[1] == nullptr) {
            break;
        }
        completed++;
    }

    swprintf(what, _countof(what), L"close storm: %d/%d rounds completed (stuck threads mean a wedged driver)",
             completed, rounds);
    Check(completed == rounds, what);

    VerifyStreamingHealthy(h, L"close storm");
}

/// Test 5: DirectShow pin connect/disconnect racing an IOCTL prepare/unprepare
/// loop.  Each round builds a fresh graph and tries to connect the capture pin
/// while a worker hammers the slot; whichever side wins, the loser must fail
/// cleanly, the interfaces must never both own the camera, and the device must
/// stream afterwards.
static void TestExclusionRace(HANDLE h, IMoniker *moniker, int stormSeconds) {
    StormContext ctx;
    ctx.h = h;

    HANDLE worker = CreateThread(nullptr, 0, StormPrepareUnprepare, &ctx, 0, nullptr);
    if (worker == nullptr) {
        Check(false, L"exclusion race: worker thread");
        return;
    }

    DWORD deadline = GetTickCount() + (DWORD) stormSeconds * 1000;
    int rounds = 0;
    int connected = 0;
    int exclusionHeld = 0;
    do {
        ComPtr<IGraphBuilder> graph;
        ComPtr<ICaptureGraphBuilder2> builder;
        ComPtr<IBaseFilter> filter;

        if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))) ||
            FAILED(
                CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder))) ||
            FAILED(builder->SetFiltergraph(graph.Get())) ||
            FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter))) ||
            FAILED(graph->AddFilter(filter.Get(), L"Photonic"))) {
            break;
        }
        rounds++;

        HRESULT hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, filter.Get(), nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            connected++;
            // The pin owns the camera right now, so the worker's prepares are
            // all failing; a prepare from here must fail too.
            if (PhotonicIoctlPrepareVideo(h, STORM_FRAME_COUNT) == FALSE) {
                exclusionHeld++;
            } else {
                PhotonicIoctlUnprepareVideo(h);
            }
        }
        // The scoped graph releases here, disconnecting the pin.
    } while (GetTickCount() < deadline);

    InterlockedExchange(&ctx.stop, 1);
    bool joined = WaitForSingleObject(worker, STORM_JOIN_TIMEOUT_MS) == WAIT_OBJECT_0;
    if (joined) {
        CloseHandle(worker);
    }

    wchar_t what[128];
    swprintf(what, _countof(what), L"exclusion race: %d rounds, pin connected %d times, worker finished", rounds,
             connected);
    Check(rounds > 0 && joined, what);
    swprintf(what, _countof(what), L"exclusion race: IOCTL prepare failed while the pin was open (%d/%d)",
             exclusionHeld, connected);
    Check(exclusionHeld == connected, what);

    VerifyStreamingHealthy(h, L"exclusion race");
}

void RunConcurrencyTests(HANDLE hCamera, const std::wstring &devicePath, IMoniker *moniker, int stormSeconds,
                         int &passed, int &failed) {
    g_passed = 0;
    g_failed = 0;

    Log(INFO, L"concurrency tests: %d second(s) per storm", stormSeconds);

    TestPrepareUnprepareStorm(hCamera, stormSeconds);
    TestLifecycleStorm(hCamera, stormSeconds);
    TestSecondHandleOwnership(hCamera, devicePath);
    TestHandleCloseStorm(hCamera, devicePath);
    TestExclusionRace(hCamera, moniker, stormSeconds);

    passed += g_passed;
    failed += g_failed;
}
