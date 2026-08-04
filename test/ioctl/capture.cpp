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
/// --test-ioctl suite (RunIoctlTests): exercises the camera through the device
/// handle opened from the DirectShow moniker's DevicePath
/// (directshow/finddevice.cpp); no capture graph is built.
///
/// The suite first covers the camera-head controller registers
/// (common/camera_regs.h) through the PHOTONIC_IOCTL_MAILBOX command gate (i2c.cpp):
/// every writable register is programmed to its operational default via
/// CamRegInitDefaults and read back to verify the value stuck, leaving the
/// camera in its default operational state (10 MHz, full-frame ROI at 1x1
/// binning, 100 ms exposure, gains 1, free-run trigger).
///
/// It then drives the driver's shared IOCTL capture slot through the raw
/// PHOTONIC_IOCTL_* wrappers (ioctl.cpp), covering the sequences of
/// docs/ioctl-interface-design.md section 9: the continuous-video stream
/// (START_VIDEO(1) free-run, at least 15 frames drained through the Type-0
/// event pump and verified consecutive, as the DirectShow sweep does), the
/// triggered-snap bring-up and shot loop (video family, start value 0,
/// per-shot SW_TRIGGER arm) -- each run once at MONO8 and once at MONO16 to
/// cover both frame depths and the
/// doubled 16-bit frame geometry -- the transmit-only imager path, the
/// family-composition and error rules, the client's redundant teardown order,
/// the fake device's trigger gating (over the mailbox head-register gate) and the
/// IOCTL/DirectShow exclusion.  It ends with the concurrency tests
/// (concurrency.cpp): multithreaded storms over the lifecycle verbs, the
/// cross-handle ownership rules and the exclusion race, sized by
/// --storm-seconds.
///
/// The out-of-band hardware trigger is a DTR pulse on the COM port wired to
/// the fake device's PHOTONIC_TRIGGER_PORT (-c/--com-port; without it the
/// pulse-dependent checks are skipped).  On a real camera an
/// externally-triggered shot is only delivered for a pulse, so without a
/// usable trigger port the triggered-snap tests are skipped as well.
///
/// Fake-device caveat (design section 1): the fake transmits the one-shot
/// frame immediately on the ONE_SHOT arm instead of holding it for the
/// trigger pulse, so this suite asserts per-arm delivery -- never the
/// arm -> pulse -> frame ordering.
///
/// Each delivered frame's content is verified through the DirectShow suite's
/// FrameStats accumulator (directshow/framestats.h): the fake stamps every
/// transmitted frame with the frame_meta_t header (common/frame_meta.h), so
/// the suite checks the header, the whole-frame checksum, the frame index and
/// the embedded head-register snapshot, plus non-black pixel data.  A real
/// camera stamps no header; --frame-header-check false keeps only the
/// non-black pixel check.

#include <dshow.h>
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <sstream>
#include <string>
#include <vector>

#include "../../common/camera_regs.h"
#include "../directshow/comptr.h"
#include "../directshow/finddevice.h"
#include "../directshow/framestats.h"
#include "../utils/log.h"
#include "../utils/pngwriter.h"
#include "concurrency.h"
#include "i2c.h"
#include "ioctl.h"

/// Pass/fail bookkeeping
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

/// Value-comparison variant: logs the actual value on pass, expected vs. got
/// on fail.
static void Check(bool ok, const wchar_t *what, UINT32 expected, UINT32 actual) {
    if (ok) {
        g_passed++;
        Log(INFO, L"PASS %s (0x%04X)", what, actual);
    } else {
        g_failed++;
        Log(ERROR, L"FAIL %s: expected 0x%04X, got 0x%04X", what, expected, actual);
    }
}

/// Assert the driver reported the expected LastError after a failed IOCTL.
static void CheckLastError(HANDLE h, UINT32 expected, const wchar_t *what) {
    UINT32 lastError = 0;
    bool ok = PhotonicIoctlGetLastError(h, &lastError) != FALSE && lastError == expected;
    wchar_t msg[128];
    swprintf(msg, _countof(msg), L"%s (LastError 0x%02X, expected 0x%02X)", what, lastError, expected);
    Check(ok, msg);
}

/// Options
/// Anonymous namespace for internal linkage: other suites define their own,
/// differently laid out Options at global scope (see directshow.cpp).
namespace {

struct Options {
    std::wstring deviceSubstr = L"Photonic";
    int deviceIndex = -1;          ///< -1 == first device matching the name; else the N-th match
    std::string comPort;           ///< empty == skip the pulse-dependent checks
    int snapCount = 8;             ///< shots in the repeated-snap loop
    bool frameHeaderChecks = true; ///< verify the embedded per-frame test header
    std::wstring dumpDir;          ///< save PNG sample frames here (empty == off)
    int stormSeconds = 2;          ///< duration of each concurrency storm; 0 == skip them
};

} // namespace

/// COM-port hardware trigger

/// The serial port whose DTR line is wired to the camera trigger input, opened
/// once for the whole suite with DTR parked low.  Each pulse is SETDTR then
/// CLRDTR.
static HANDLE g_comPort = INVALID_HANDLE_VALUE;

static bool ComTriggerOpen(const std::string &port) {
    // CreateFile needs the \\.\ device-namespace prefix for COM10 and above;
    // it is harmless for COM1..COM9.
    std::string path = port.compare(0, 4, "\\\\.\\") == 0 ? port : "\\\\.\\" + port;
    g_comPort = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_comPort == INVALID_HANDLE_VALUE) {
        Log(ERROR, L"cannot open trigger port %hs (err=%lu)", port.c_str(), GetLastError());
        return false;
    }
    EscapeCommFunction(g_comPort, CLRDTR);
    return true;
}

static bool ComTriggerPulse() {
    if (g_comPort == INVALID_HANDLE_VALUE) {
        return false;
    }
    return EscapeCommFunction(g_comPort, SETDTR) && EscapeCommFunction(g_comPort, CLRDTR);
}

static void ComTriggerClose() {
    if (g_comPort != INVALID_HANDLE_VALUE) {
        CloseHandle(g_comPort);
        g_comPort = INVALID_HANDLE_VALUE;
    }
}

/// Per-register verification

/// Read an 8-bit register and check it holds the expected default.
static void VerifyReg8(HANDLE h, const wchar_t *name, UINT8 reg, UINT8 expected) {
    wchar_t what[64];
    swprintf(what, _countof(what), L"%s == 0x%02X", name, expected);
    UINT8 value = 0;
    bool ok = CamRegRead(h, reg, &value) && value == expected;
    Check(ok, what, expected, value);
}

/// Read an LE16 register pair and check it holds the expected default.
static void VerifyReg16(HANDLE h, const wchar_t *name, UINT8 regLo, UINT16 expected) {
    wchar_t what[64];
    swprintf(what, _countof(what), L"%s == 0x%04X", name, expected);
    UINT16 value = 0;
    bool ok = CamRegRead16(h, regLo, &value) && value == expected;
    Check(ok, what, expected, value);
}

/// Program the operational defaults through CamRegInitDefaults, then read
/// every register it touches back and verify the value stuck.
static void RunRegisterTests(HANDLE h) {
    if (!CamRegInitDefaults(h)) {
        g_failed++;
        Log(ERROR, L"FAIL CamRegInitDefaults");
        return;
    }
    g_passed++;
    Log(INFO, L"PASS CamRegInitDefaults");

    VerifyReg16(h, L"SETUP_VALUE", CAMREG_SETUP_VALUE_L, CamCalibHeadA.SetupValue[CAMDEF_BINNING]);
    VerifyReg8(h, L"ADC_OFFSET", CAMREG_ADC_OFFSET, CamCalibHeadA.AdcOffset[CAMDEF_BINNING]);
    VerifyReg16(h, L"SPEED", CAMREG_SPEED_L, CamCalibHeadA.SpeedKhz);

    VerifyReg16(h, L"ROI_X_START", CAMREG_ROI_X_START_L, 0);
    VerifyReg16(h, L"ROI_Y_START", CAMREG_ROI_Y_START_L, 0);
    VerifyReg16(h, L"ROI_X_END", CAMREG_ROI_X_END_L, CAMDEF_SENSOR_WIDTH);
    VerifyReg16(h, L"ROI_Y_END", CAMREG_ROI_Y_END_L, CAMDEF_SENSOR_HEIGHT);

    VerifyReg8(h, L"X_BINNING", CAMREG_X_BINNING, CAMDEF_BINNING);
    VerifyReg8(h, L"Y_BINNING", CAMREG_Y_BINNING, CAMDEF_BINNING);

    VerifyReg8(h, L"EXPOSURE_UNITS", CAMREG_EXPOSURE_UNITS, CAMDEF_EXPOSURE_UNITS);
    VerifyReg16(h, L"EXPOSURE_TIME", CAMREG_EXPOSURE_TIME_L, CAMDEF_EXPOSURE_TIME);

    VerifyReg8(h, L"VIDEO_GAIN", CAMREG_VIDEO_GAIN, CAMDEF_GAIN);
    VerifyReg8(h, L"INTENSIFIER_GAIN", CAMREG_INTENSIFIER_GAIN, CAMDEF_GAIN);

    VerifyReg8(h, L"TRIGGER_MODE", CAMREG_TRIGGER_MODE, CAMREG_TRIGGER_FREE_RUN);
}

/// PNG sample frames

/// Directory the --dump-dir option points at; empty disables the samples.
/// Suite-wide state like the trigger port above.
static std::wstring g_dumpDir;

/// Save one delivered frame as <dump-dir>\ioctl_<test>_<MONO8|MONO16>.png.
/// The frames are always full-sensor greyscale at the given pixel format
/// (see SnapFrameSize); a save failure is logged by the writer, never failed on.
static void SaveSamplePng(const wchar_t *test, UINT32 pixelFormat, const BYTE *data, size_t len);

/// DCAM colour-coding ids as carried by GET/SET_PIXEL_FORMAT
/// (PHOTONIC_DCAM_PIX_* in photonic/photonic.h, a kernel-only header).
static const UINT32 PIXEL_FORMAT_MONO8 = 0;
static const UINT32 PIXEL_FORMAT_MONO16 = 5;

/// The two greyscale codings the fake's Format 7 mode 0 advertises.
static UINT32 PixelFormatBpp(UINT32 pixelFormat) {
    return pixelFormat == PIXEL_FORMAT_MONO16 ? 16 : 8;
}

static const wchar_t *PixelFormatName(UINT32 pixelFormat) {
    return pixelFormat == PIXEL_FORMAT_MONO16 ? L"MONO16" : L"MONO8";
}

/// Frame size the driver computed at prepare: the camera's default full-frame
/// geometry at the current pixel format.  The fake device's default Format 7
/// IMAGE_SIZE is the full sensor (CAMDEF_SENSOR_WIDTH x CAMDEF_SENSOR_HEIGHT,
/// see fake-fw-dev/format7.c); the suite pins the pixel format so the size is
/// deterministic.  A successful MAP_VIDEO_FRAME with this size also proves the
/// driver derived the same bytes-per-frame from the selected coding's depth
/// (a mismatch fails the map with PL_ERROR_BAD_FRAME_SIZE).
static UINT32 SnapFrameSize(HANDLE h, UINT32 pixelFormat) {
    UINT32 current = ~0u;
    if (!PhotonicIoctlSetPixelFormat(h, pixelFormat) || !PhotonicIoctlGetPixelFormat(h, &current) ||
        current != pixelFormat) {
        Log(ERROR, L"cannot pin the pixel format to %s (got %u)", PixelFormatName(pixelFormat), current);
        return 0;
    }
    return CAMDEF_SENSOR_WIDTH * CAMDEF_SENSOR_HEIGHT * PixelFormatBpp(pixelFormat) / 8;
}

static void SaveSamplePng(const wchar_t *test, UINT32 pixelFormat, const BYTE *data, size_t len) {
    if (g_dumpDir.empty()) {
        return;
    }
    std::wostringstream name;
    name << L"ioctl_" << test << L"_" << PixelFormatName(pixelFormat) << L".png";
    std::wstring path = g_dumpDir + L"\\" + name.str();
    if (SaveMonoFramePng(path.c_str(), data, len, CAMDEF_SENSOR_WIDTH, CAMDEF_SENSOR_HEIGHT,
                         PixelFormatBpp(pixelFormat))) {
        Log(INFO, L"saved sample frame %s", path.c_str());
    }
}

/// Snap ring fixture: PREPARE_VIDEO + MAP_VIDEO_FRAME(4) + START_VIDEO(value)
/// on construction (each step checked), full teardown on destruction unless
/// the test already tore down.
namespace {

struct SnapRing {
    static const UINT32 FrameCount = 4;

    HANDLE h;
    UINT32 count = FrameCount;
    UINT32 frameSize = 0;
    BYTE *frames = nullptr;
    bool up = false;

    SnapRing(HANDLE hCamera, UINT32 startValue, UINT32 pixelFormat, UINT32 frameCount = FrameCount)
        : h(hCamera), count(frameCount) {
        frameSize = SnapFrameSize(h, pixelFormat);
        if (frameSize == 0) {
            return;
        }
        frames = (BYTE *) VirtualAlloc(nullptr, (SIZE_T) count * frameSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (frames == nullptr) {
            Log(ERROR, L"cannot allocate the %u-frame user ring", count);
            return;
        }
        if (!PhotonicIoctlPrepareVideo(h, count)) {
            Log(ERROR, L"PREPARE_VIDEO failed");
            return;
        }
        if (!PhotonicIoctlMapVideoFrame(h, count, frameSize, (UINT32) (ULONG_PTR) frames)) {
            Log(ERROR, L"MAP_VIDEO_FRAME failed");
            PhotonicIoctlUnprepareVideo(h);
            return;
        }
        if (!PhotonicIoctlStartVideo(h, startValue)) {
            Log(ERROR, L"START_VIDEO(%u) failed", startValue);
            PhotonicIoctlUnmapVideoFrame(h);
            PhotonicIoctlUnprepareVideo(h);
            return;
        }
        up = true;
    }

    /// The client's interleaved teardown; the trailing imager calls fail
    /// benignly once the video unprepare has freed the slot.
    void Teardown() {
        if (up) {
            PhotonicIoctlStopVideo(h);
            PhotonicIoctlStopImager(h);
            PhotonicIoctlUnmapVideoFrame(h);
            PhotonicIoctlUnprepareVideo(h);
            PhotonicIoctlUnprepareImager(h);
            up = false;
        }
    }

    BYTE *Frame(UINT32 index) const {
        return frames + (SIZE_T) index * frameSize;
    }

    ~SnapRing() {
        Teardown();
        if (frames != nullptr) {
            VirtualFree(frames, 0, MEM_RELEASE);
        }
    }

    SnapRing(const SnapRing &) = delete;
    SnapRing &operator=(const SnapRing &) = delete;
};

} // namespace

/// Arm one shot and wait for its delivery: REGISTER_EVENT(Type 0) ->
/// SW_TRIGGER -> DTR pulse (when wired) -> wait.  Returns true when the event
/// fired within the timeout.  On timeout the event is unregistered.
static bool SnapOneShot(HANDLE h, HANDLE hEvent) {
    ResetEvent(hEvent);
    if (!PhotonicIoctlRegisterEvent(h, hEvent, 0)) {
        Log(ERROR, L"REGISTER_EVENT(Type 0) failed");
        return false;
    }
    if (!PhotonicIoctlSwTrigger(h)) {
        Log(ERROR, L"SW_TRIGGER failed");
        PhotonicIoctlUnregisterEvent(h, hEvent, 0);
        return false;
    }
    // Out-of-band exposure trigger.  The fake transmits on the arm itself, so
    // the pulse is issued for fidelity, not as a precondition of delivery.
    ComTriggerPulse();
    if (WaitForSingleObject(hEvent, 5000) != WAIT_OBJECT_0) {
        Log(ERROR, L"shot event did not fire within 5s");
        PhotonicIoctlUnregisterEvent(h, hEvent, 0);
        return false;
    }
    return true;
}

/// Quiet window that must elapse with no delivery before the first arm; long
/// enough for a free-run exposure (default 100 ms) plus its readout.
static const DWORD SETTLE_WAIT_MS = 750;

/// Settle after the switch to external trigger mode.  The previous test's
/// teardown leaves the head free-running, so an exposure started before the
/// switch can complete after it and be transmitted into the just-started
/// ring, where the shot loop would count it as the triggered shot.  Wait
/// until no frame arrives for SETTLE_WAIT_MS and return the number of frames
/// delivered so far, the baseline for the shot loop's transfer-info checks.
static UINT32 DrainInFlightFrames(HANDLE h, HANDLE hEvent, const wchar_t *fmt) {
    UINT32 baseline = 0;
    for (;;) {
        ResetEvent(hEvent);
        if (!PhotonicIoctlRegisterEvent(h, hEvent, 0)) {
            Log(ERROR, L"%s settle: REGISTER_EVENT(Type 0) failed", fmt);
            return baseline;
        }
        bool fired = WaitForSingleObject(hEvent, SETTLE_WAIT_MS) == WAIT_OBJECT_0;
        if (!fired) {
            PhotonicIoctlUnregisterEvent(h, hEvent, 0);
        }
        // The event only reports deliveries made while it was registered, so
        // probe the totals as well: a frame can land between START_VIDEO and
        // the first registration.  The probe fails benignly while nothing has
        // been delivered yet.
        UINT32 index = ~0u, total = 0;
        if (PhotonicIoctlGetTransferInfo(h, &index, &total) != FALSE && total > baseline) {
            Log(WARN, L"%s settle: %u in-flight free-run frame(s) drained after the external-trigger switch", fmt,
                total - baseline);
            baseline = total;
            continue;
        }
        if (!fired) {
            return baseline;
        }
    }
}

/// Frame-content verification

/// Verify the content of the frame delivered into ring slot `slot`, reusing the
/// DirectShow suite's FrameStats accumulator (directshow/framestats.h).  The
/// slot was zero-filled before the arm, so an undelivered or stale frame reads
/// all-black and fails the non-black pixel check, which works for a real
/// camera as well.  Byte-wise sampling covers MONO16 too: the fake replicates
/// the luminance into both bytes of each little-endian 16-bit pixel
/// (fake-fw-dev/frame.c), so a bright pixel yields a high byte either way.
/// With header checks on (the fake stamps every transmitted frame with the
/// frame_meta_t verification header, common/frame_meta.h, and each ONE_SHOT
/// arm restarts its stream) the frame must additionally carry index 0, a
/// checksum covering exactly the mapped frame size, and the intact
/// head-register snapshot (still in external trigger mode).
static void CheckSnapFrameContent(const SnapRing &ring, UINT32 slot, int shot, bool frameHeaderChecks,
                                  const wchar_t *fmt) {
    wchar_t what[96];

    FrameStats stats;
    stats.SetVerifyMeta(frameHeaderChecks);
    stats.BufferCB(0.0, ring.Frame(slot), (long) ring.frameSize);

    long frames = 0;
    int maxByte = 0, minByte = 0;
    double meanByte = 0.0;
    stats.Get(frames, maxByte, minByte, meanByte);

    swprintf(what, _countof(what), L"%s shot %d frame not all-black (px max %d)", fmt, shot, maxByte);
    Check(maxByte > 0, what);

    if (!frameHeaderChecks) {
        return;
    }

    FrameMetaStats meta;
    stats.GetMeta(meta);

    swprintf(what, _countof(what), L"%s shot %d frame header intact (checksum over %u bytes)", fmt, shot,
             ring.frameSize);
    Check(meta.withHeader == 1 && meta.badHeader == 0 && meta.badChecksum == 0 && meta.badSize == 0 &&
              meta.badFeatures == 0,
          what);

    swprintf(what, _countof(what), L"%s shot %d frame index 0 (one-shot restarts the stream)", fmt, shot);
    Check(meta.firstIndex == 0 && meta.outOfOrder == 0, what);

    swprintf(what, _countof(what), L"%s shot %d frame head-register snapshot: TRIGGER_MODE external", fmt, shot);
    Check(meta.i2cRegs[CAMREG_TRIGGER_MODE] == CAMREG_TRIGGER_EXTERNAL, what);
}

/// Section-9 sequences

/// Minimum frames a continuous-video pass must capture and verify, matching
/// the DirectShow sweep's MIN_FRAMES_PER_TEST.
static const UINT32 VIDEO_MIN_FRAMES = 15;

/// Continuous video streaming (the design's section-2 streaming sequence):
/// PREPARE_VIDEO -> MAP_VIDEO_FRAME(4) -> START_VIDEO(1) free-runs the camera
/// into the 4-slot ring, then the client's fetch loop pulls frames: one
/// REGISTER_EVENT(Type 0) per wakeup, wait, GET_TRANSFER_INFO, and drain every
/// frame delivered since the previous wakeup (frame n lands in ring slot
/// n % 4) so a burst between wakeups cannot slip past unseen.  Each drained
/// frame is fed to the DirectShow suite's FrameStats accumulator, and the pass
/// asserts the same verdict the DirectShow sweep does over at least
/// VIDEO_MIN_FRAMES frames: non-black pixel data and, with header checks on,
/// intact per-frame headers whose indices run 0,1,2,... with no gaps --
/// proving the delivered frames are consecutive with nothing dropped or
/// reordered.
static void TestContinuousVideo(HANDLE h, bool frameHeaderChecks, UINT32 pixelFormat) {
    const wchar_t *fmt = PixelFormatName(pixelFormat);
    wchar_t what[96];

    // Free-run trigger mode: continuous video needs the head transmitting on
    // its own, not holding exposures for a trigger edge.
    swprintf(what, _countof(what), L"%s video mailbox write TRIGGER_MODE=FREE_RUN", fmt);
    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN) != FALSE, what);

    SnapRing ring(h, 1, pixelFormat);
    swprintf(what, _countof(what), L"%s video bring-up: PREPARE_VIDEO + MAP(4) + START_VIDEO(1)", fmt);
    Check(ring.up, what);
    if (!ring.up) {
        return;
    }

    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (hEvent == nullptr) {
        Check(false, L"CreateEvent for the video frame completion");
        return;
    }

    FrameStats stats;
    stats.SetVerifyMeta(frameHeaderChecks);

    std::vector<BYTE> lastFrame; // newest drained frame, kept for the PNG sample
    UINT32 drained = 0;
    bool pumpOk = true;
    while (pumpOk && drained < VIDEO_MIN_FRAMES) {
        ResetEvent(hEvent);
        if (!PhotonicIoctlRegisterEvent(h, hEvent, 0)) {
            Log(ERROR, L"REGISTER_EVENT(Type 0) failed after %u frames", drained);
            pumpOk = false;
            break;
        }
        if (WaitForSingleObject(hEvent, 5000) != WAIT_OBJECT_0) {
            Log(ERROR, L"no video frame within 5s after %u frames", drained);
            PhotonicIoctlUnregisterEvent(h, hEvent, 0);
            pumpOk = false;
            break;
        }

        UINT32 index = ~0u, total = 0;
        if (PhotonicIoctlGetTransferInfo(h, &index, &total) == FALSE || total <= drained ||
            index != (total - 1) % SnapRing::FrameCount) {
            Log(ERROR, L"bad transfer info after %u frames: total %u, ring slot %u", drained, total, index);
            pumpOk = false;
            break;
        }
        // More pending frames than ring slots means the oldest were already
        // overwritten; their slots would feed newer frames to the accumulator
        // and fail the contiguity verdict with a misleading message.
        if (total - drained > SnapRing::FrameCount) {
            Log(ERROR, L"ring overrun: %u frames pending, %u slots", total - drained, SnapRing::FrameCount);
            pumpOk = false;
            break;
        }
        for (; drained < total; drained++) {
            BYTE *frame = ring.Frame(drained % SnapRing::FrameCount);
            stats.BufferCB(0.0, frame, (long) ring.frameSize);
            // Snapshot for the PNG sample now: the stream keeps free-running,
            // so this slot is overwritten again soon after the pump exits.
            if (!g_dumpDir.empty()) {
                lastFrame.assign(frame, frame + ring.frameSize);
            }
        }
    }

    CloseHandle(hEvent);

    swprintf(what, _countof(what), L"%s video captured %u frames (minimum %u)", fmt, drained, VIDEO_MIN_FRAMES);
    Check(pumpOk && drained >= VIDEO_MIN_FRAMES, what);

    long frames = 0;
    int maxByte = 0, minByte = 0;
    double meanByte = 0.0;
    stats.Get(frames, maxByte, minByte, meanByte);
    swprintf(what, _countof(what), L"%s video frames not all-black (px max %d)", fmt, maxByte);
    Check(maxByte > 0, what);

    if (frameHeaderChecks) {
        FrameMetaStats meta;
        stats.GetMeta(meta);

        swprintf(what, _countof(what), L"%s video frame headers intact (%ld frames, checksum over %u bytes)", fmt,
                 meta.withHeader, ring.frameSize);
        Check(meta.withHeader == frames && meta.badHeader == 0 && meta.badChecksum == 0 && meta.badSize == 0 &&
                  meta.badFeatures == 0,
              what);

        swprintf(what, _countof(what), L"%s video frames consecutive (indices %ld..%ld, %ld out of order)", fmt,
                 meta.firstIndex, meta.lastIndex, meta.outOfOrder);
        Check(meta.indexContiguous && meta.firstIndex == 0 && meta.outOfOrder == 0, what);
    }

    if (!lastFrame.empty()) {
        SaveSamplePng(L"video", pixelFormat, lastFrame.data(), lastFrame.size());
    }

    // Stopping the still-running stream must succeed; the rest is the ring's
    // client-order teardown, whose redundant second stop fails benignly.
    swprintf(what, _countof(what), L"%s video teardown: STOP_VIDEO on the running stream", fmt);
    Check(PhotonicIoctlStopVideo(h) != FALSE, what);
    ring.Teardown();
}

/// 9.1 Snap bring-up: PREPARE_VIDEO -> MAP_VIDEO_FRAME(4) -> START_VIDEO(0).
/// The absent ISO_EN write and the idle iso context are asserted against the
/// fake's log, not from here.
/// 9.2 Full triggered snap: TRIGGER_MODE=EXTERNAL (mailbox), one armed shot,
/// exactly one delivered frame in ring slot 0 with verified content
/// (CheckSnapFrameContent).
/// 9.3 Repeated shots: N distinct deliveries rotating round-robin through the
/// 4-slot ring, each shot's frame content verified.
/// Run per pixel format: the MONO8 pass and a MONO16 pass whose ring frames
/// are twice the size, exercising the driver's bpp-derived prepare geometry
/// and the 16-bit isochronous payload end to end.
static void TestTriggeredSnap(HANDLE h, int snapCount, bool frameHeaderChecks, UINT32 pixelFormat) {
    const wchar_t *fmt = PixelFormatName(pixelFormat);
    wchar_t what[96];

    SnapRing ring(h, 0, pixelFormat);
    swprintf(what, _countof(what), L"%s snap bring-up: PREPARE_VIDEO + MAP(4) + START_VIDEO(0)", fmt);
    Check(ring.up, what);
    if (!ring.up) {
        return;
    }

    // The head must be in external-trigger mode for the trigger port to be
    // honoured; written over the vendor I2C gate, never through the capture
    // IOCTLs.
    swprintf(what, _countof(what), L"%s mailbox write TRIGGER_MODE=EXTERNAL", fmt);
    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_EXTERNAL) != FALSE, what);

    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (hEvent == nullptr) {
        Check(false, L"CreateEvent for the shot completion");
        return;
    }

    // An in-flight free-run frame must not pass as the first triggered shot;
    // drained frames occupy ring slots, so the shot loop counts on top of
    // the baseline.
    UINT32 baseline = DrainInFlightFrames(h, hEvent, fmt);

    for (int shot = 0; shot < snapCount; shot++) {
        UINT32 expectIndex = (baseline + (UINT32) shot) % SnapRing::FrameCount;

        // Zero-fill the whole target slot so an undelivered frame, or a stale
        // one from an earlier shot, reads all-black and fails the content
        // checks.
        memset(ring.Frame(expectIndex), 0, ring.frameSize);
        bool fired = SnapOneShot(h, hEvent);
        swprintf(what, _countof(what), L"%s shot %d delivered", fmt, shot);
        Check(fired, what);
        if (!fired) {
            break;
        }

        UINT32 index = ~0u, total = 0;
        UINT32 expectTotal = baseline + (UINT32) shot + 1;
        swprintf(what, _countof(what), L"%s shot %d transfer info: total %u, ring slot %u", fmt, shot, expectTotal,
                 expectIndex);
        Check(PhotonicIoctlGetTransferInfo(h, &index, &total) != FALSE && total == expectTotal && index == expectIndex,
              what);

        CheckSnapFrameContent(ring, expectIndex, shot, frameHeaderChecks, fmt);

        // One PNG sample per format; the delivered slot stays stable until
        // the next arm, so the bytes can be saved in place.
        if (shot == 0) {
            SaveSamplePng(L"snap", pixelFormat, ring.Frame(expectIndex), ring.frameSize);
        }
    }

    CloseHandle(hEvent);
    CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN);

    // 9.7 Teardown in the client's order, asserting the trailing imager calls
    // fail benignly after the video unprepare freed the slot.
    swprintf(what, _countof(what), L"%s teardown: STOP_VIDEO", fmt);
    Check(PhotonicIoctlStopVideo(h) != FALSE, what);
    swprintf(what, _countof(what), L"%s teardown: STOP_IMAGER (slot still prepared)", fmt);
    Check(PhotonicIoctlStopImager(h) != FALSE, what);
    swprintf(what, _countof(what), L"%s teardown: UNMAP_VIDEO_FRAME", fmt);
    Check(PhotonicIoctlUnmapVideoFrame(h) != FALSE, what);
    swprintf(what, _countof(what), L"%s teardown: UNPREPARE_VIDEO", fmt);
    Check(PhotonicIoctlUnprepareVideo(h) != FALSE, what);
    swprintf(what, _countof(what), L"%s teardown: trailing UNPREPARE_IMAGER fails benignly", fmt);
    Check(PhotonicIoctlUnprepareImager(h) == FALSE, what);
    ring.up = false; // torn down here; the destructor must not repeat it
}

/// Single-slot ring variant (not in the design's section-9 list):
/// PREPARE_VIDEO(1) + MAP_VIDEO_FRAME(1) + START_VIDEO(0).  With one slot the
/// engine re-arms the
/// just-delivered frame's buffer the moment it is delivered, so the consumer
/// reads its pixels from memory that is attached to the bus again and stays
/// attached through the teardown.  The frame is verified at delivery (as the
/// 4-slot tests do) and then byte-compared, after the full client-order
/// teardown, against a copy snapshotted at delivery -- catching a stop/unmap
/// path that clobbers the consumer's pages after the frame was handed over,
/// which the 4-slot ring cannot see (its re-arm lands in the next slot, which
/// nobody re-checks).
static void TestSnapSingleSlotRing(HANDLE h, bool frameHeaderChecks, UINT32 pixelFormat) {
    const wchar_t *fmt = PixelFormatName(pixelFormat);
    wchar_t what[96];

    SnapRing ring(h, 0, pixelFormat, 1);
    swprintf(what, _countof(what), L"%s single-slot bring-up: PREPARE_VIDEO(1) + MAP(1) + START_VIDEO(0)", fmt);
    Check(ring.up, what);
    if (!ring.up) {
        return;
    }

    swprintf(what, _countof(what), L"%s single-slot mailbox write TRIGGER_MODE=EXTERNAL", fmt);
    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_EXTERNAL) != FALSE, what);

    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (hEvent == nullptr) {
        Check(false, L"CreateEvent for the single-slot shot completion");
        return;
    }

    // Same in-flight free-run drain as the 4-slot shot loop; with one slot a
    // drained frame landed in slot 0, which the zero-fill below clears.
    DrainInFlightFrames(h, hEvent, fmt);

    memset(ring.Frame(0), 0, ring.frameSize);
    bool fired = SnapOneShot(h, hEvent);
    swprintf(what, _countof(what), L"%s single-slot shot delivered", fmt);
    Check(fired, what);
    if (fired) {
        CheckSnapFrameContent(ring, 0, 0, frameHeaderChecks, fmt);

        BYTE *snapshot = (BYTE *) malloc(ring.frameSize);
        if (snapshot != nullptr) {
            memcpy(snapshot, ring.Frame(0), ring.frameSize);
        }

        CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN);
        ring.Teardown();

        if (snapshot != nullptr) {
            UINT32 diff = 0;
            UINT32 firstDiff = 0;
            for (UINT32 i = 0; i < ring.frameSize; i++) {
                if (ring.Frame(0)[i] != snapshot[i]) {
                    if (diff == 0) {
                        firstDiff = i;
                    }
                    diff++;
                }
            }
            swprintf(what, _countof(what), L"%s single-slot frame unchanged by teardown (%u bytes differ, first at %u)",
                     fmt, diff, firstDiff);
            Check(diff == 0, what);
            free(snapshot);
        }
    } else {
        CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN);
    }

    CloseHandle(hEvent);
}

/// 9.4 Trigger gating (fake behavior, via the mailbox head registers): a DTR pulse
/// while the head is in free-run mode is ignored (FRAME_READY stays clear);
/// in external mode it is counted (FRAME_READY set); leaving external mode
/// drops the pending exposure (abort path).  Needs the trigger port.
static void TestTriggerGating(HANDLE h) {
    if (g_comPort == INVALID_HANDLE_VALUE) {
        Log(INFO, L"SKIP trigger gating (no usable trigger port)");
        return;
    }

    UINT8 status = 0xFF;
    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN) != FALSE, L"gating: TRIGGER_MODE=FREE_RUN");
    ComTriggerPulse();
    Sleep(300); // let the fake's trigger watcher poll the port
    Check(CamRegGetStatus(h, &status) != FALSE && (status & CAMREG_STATUS_FRAME_READY) == 0,
          L"gating: pulse in free-run ignored (FRAME_READY clear)");

    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_EXTERNAL) != FALSE, L"gating: TRIGGER_MODE=EXTERNAL");
    ComTriggerPulse();
    Sleep(300);
    Check(CamRegGetStatus(h, &status) != FALSE && (status & CAMREG_STATUS_FRAME_READY) != 0,
          L"gating: pulse in external mode counted (FRAME_READY set)");

    Check(CamRegSetTriggerMode(h, CAMREG_TRIGGER_FREE_RUN) != FALSE, L"gating: back to FREE_RUN with a pulse pending");
    Sleep(300);
    Check(CamRegGetStatus(h, &status) != FALSE && (status & CAMREG_STATUS_FRAME_READY) == 0,
          L"gating: pending exposure dropped on mode exit (FRAME_READY clear)");
}

/// 9.5 Transmit-only path: PREPARE_IMAGER -> START_IMAGER(1) free-runs the
/// camera with nobody listening (ISO_EN written, no listen/attach -- asserted
/// against the fake's log); STOP_IMAGER clears ISO_EN.
static void TestTransmitOnly(HANDLE h) {
    Check(PhotonicIoctlPrepareImager(h) != FALSE, L"transmit-only: PREPARE_IMAGER");
    Check(PhotonicIoctlStartImager(h, 1) != FALSE, L"transmit-only: START_IMAGER(1)");
    Sleep(200); // let the fake log a few unmatched transmissions
    Check(PhotonicIoctlStopImager(h) != FALSE, L"transmit-only: STOP_IMAGER");
    Check(PhotonicIoctlUnprepareImager(h) != FALSE, L"transmit-only: UNPREPARE_IMAGER");
}

/// 9.6 Composition and error rules on the one shared slot.
static void TestComposition(HANDLE h) {
    UINT32 frameSize = SnapFrameSize(h, PIXEL_FORMAT_MONO8);

    // SW_TRIGGER's only precondition is that the slot exists.
    Check(PhotonicIoctlSwTrigger(h) == FALSE, L"composition: SW_TRIGGER with no slot fails");
    CheckLastError(h, 0x0D, L"composition: SW_TRIGGER with no slot");

    Check(PhotonicIoctlPrepareImager(h) != FALSE, L"composition: PREPARE_IMAGER on a free slot");
    Check(PhotonicIoctlPrepareImager(h) == FALSE, L"composition: second PREPARE_IMAGER fails");
    CheckLastError(h, 0x17, L"composition: second PREPARE_IMAGER");

    // The video prepare tops up an existing (imager-prepared, stopped) slot.
    Check(PhotonicIoctlPrepareVideo(h, SnapRing::FrameCount) != FALSE,
          L"composition: PREPARE_VIDEO tops up the imager slot");
    Check(PhotonicIoctlPrepareImager(h) == FALSE, L"composition: PREPARE_IMAGER over the video slot fails");
    CheckLastError(h, 0x17, L"composition: PREPARE_IMAGER over the video slot");

    // A video-prepared slot accepts the arm (the snap sequence never prepares
    // the imager family at all).
    Check(PhotonicIoctlSwTrigger(h) != FALSE, L"composition: SW_TRIGGER on the video slot");

    // The events need a mapped ring, whichever family prepared the slot.
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (hEvent != nullptr) {
        Check(PhotonicIoctlRegisterEvent(h, hEvent, 0) == FALSE, L"composition: REGISTER_EVENT with no ring fails");
        CheckLastError(h, 0x1F, L"composition: REGISTER_EVENT with no ring");
        CloseHandle(hEvent);
    }
    Check(frameSize != 0, L"composition: frame size resolved");

    Check(PhotonicIoctlUnprepareVideo(h) != FALSE, L"composition: UNPREPARE_VIDEO frees the slot");
    Check(PhotonicIoctlUnprepareImager(h) == FALSE, L"composition: trailing UNPREPARE_IMAGER fails benignly");
}

/// 9.8 Exclusion: while the IOCTL slot is prepared (either family) the
/// DirectShow capture pin must not connect, and while a DirectShow pin is
/// connected both prepares must fail.
static void TestExclusion(HANDLE h, IMoniker *moniker) {
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    ComPtr<IBaseFilter> filter;

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))) ||
        FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder))) ||
        FAILED(builder->SetFiltergraph(graph.Get())) ||
        FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter))) ||
        FAILED(graph->AddFilter(filter.Get(), L"Photonic"))) {
        Check(false, L"exclusion: DirectShow graph setup");
        return;
    }

    // IOCTL slot prepared -> the pin open (SRB_OPEN_STREAM) is rejected, so
    // the capture pin cannot connect.
    Check(PhotonicIoctlPrepareImager(h) != FALSE, L"exclusion: PREPARE_IMAGER");
    HRESULT hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, filter.Get(), nullptr, nullptr);
    Check(FAILED(hr), L"exclusion: pin connect fails while the IOCTL slot is prepared");
    Check(PhotonicIoctlUnprepareImager(h) != FALSE, L"exclusion: UNPREPARE_IMAGER");

    // Slot freed -> the pin connects; while it is connected both prepares
    // fail.
    hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, filter.Get(), nullptr, nullptr);
    Check(SUCCEEDED(hr), L"exclusion: pin connects once the slot is freed");
    if (SUCCEEDED(hr)) {
        Check(PhotonicIoctlPrepareVideo(h, SnapRing::FrameCount) == FALSE,
              L"exclusion: PREPARE_VIDEO fails while the pin is open");
        CheckLastError(h, 0x17, L"exclusion: PREPARE_VIDEO while the pin is open");
        Check(PhotonicIoctlPrepareImager(h) == FALSE, L"exclusion: PREPARE_IMAGER fails while the pin is open");
    }
}

/// Options / entry point

static void PrintUsage(const wchar_t *exe) {
    fwprintf(stderr,
             L"Usage: %s [options]\n"
             L"  -d, --device <substr>  Capture device friendly-name substring (default: Photonic)\n"
             L"  -i, --device-index <n> Pick the n-th (0-based) device matching the name,\n"
             L"                         to disambiguate identically-named cameras (default: first)\n"
             L"  -c, --com-port <port>  Serial port (e.g. COM1) whose DTR line is wired to the\n"
             L"                         camera trigger input (default: none; the pulse-dependent\n"
             L"                         checks are skipped, and with --frame-header-check false\n"
             L"                         the triggered-snap tests as well: a real camera only\n"
             L"                         delivers a shot for a pulse)\n"
             L"  -n, --snap-count <n>   Shots in the repeated-snap loop (default: 8)\n"
             L"      --frame-header-check <true|false>\n"
             L"                         Verify the embedded per-frame test header on each\n"
             L"                         delivered snap (checksum/index/registers). A real\n"
             L"                         camera does not generate it, so pass false for one.\n"
             L"                         Defaults to true when omitted.\n"
             L"      --dump-dir <dir>   Save one PNG sample frame per pixel format of the\n"
             L"                         continuous-video and triggered-snap tests in this\n"
             L"                         directory (created if missing; MONO16 saved as 16-bit\n"
             L"                         greyscale). Default: no samples saved.\n"
             L"      --storm-seconds <n>\n"
             L"                         Duration of each concurrency storm (multithreaded\n"
             L"                         races over the capture-slot lifecycle verbs).\n"
             L"                         0 skips the concurrency tests. Default: 2.\n"
             L"  -h, --help             Show this help and exit\n"
             L"\n"
             L"Output is teed to a results file via the shared -o/--output option,\n"
             L"which is handled before the suite runs.\n",
             exe);
}

/// Returns true on success, false if parsing failed or help was requested
/// (in which case the caller should exit).  *exitCode is set accordingly.
static bool ParseArgs(int argc, wchar_t *argv[], Options &opt, int &exitCode) {
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help") {
            PrintUsage(argv[0]);
            exitCode = 0;
            return false;
        } else if ((arg == L"-d" || arg == L"--device") && i + 1 < argc) {
            opt.deviceSubstr = argv[++i];
        } else if ((arg == L"-c" || arg == L"--com-port") && i + 1 < argc) {
            // Port names are plain ASCII, so narrowing each character is safe
            std::wstring port = argv[++i];
            opt.comPort.clear();
            opt.comPort.reserve(port.size());
            for (wchar_t ch : port) {
                opt.comPort.push_back(static_cast<char>(ch));
            }
        } else if ((arg == L"-i" || arg == L"--device-index") && i + 1 < argc) {
            opt.deviceIndex = _wtoi(argv[++i]);
            if (opt.deviceIndex < 0) {
                fwprintf(stderr, L"Error: --device-index must be >= 0.\n");
                exitCode = 1;
                return false;
            }
        } else if ((arg == L"-n" || arg == L"--snap-count") && i + 1 < argc) {
            opt.snapCount = _wtoi(argv[++i]);
            if (opt.snapCount < 1) {
                fwprintf(stderr, L"Error: --snap-count must be >= 1.\n");
                exitCode = 1;
                return false;
            }
        } else if (arg == L"--dump-dir" && i + 1 < argc) {
            opt.dumpDir = argv[++i];
        } else if (arg == L"--storm-seconds" && i + 1 < argc) {
            opt.stormSeconds = _wtoi(argv[++i]);
            if (opt.stormSeconds < 0) {
                fwprintf(stderr, L"Error: --storm-seconds must be >= 0.\n");
                exitCode = 1;
                return false;
            }
        } else if (arg == L"--frame-header-check" && i + 1 < argc) {
            std::wstring val = argv[++i];
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
            fwprintf(stderr, L"Error: unknown or incomplete option \"%s\".\n\n", arg.c_str());
            PrintUsage(argv[0]);
            exitCode = 1;
            return false;
        }
    }
    return true;
}

int RunIoctlTests(int argc, wchar_t *argv[]) {
    Options opt;
    int parseExit = 0;
    if (!ParseArgs(argc, argv, opt, parseExit)) {
        return parseExit;
    }

    if (!opt.dumpDir.empty()) {
        if (!EnsureDumpDir(opt.dumpDir.c_str())) {
            return 1;
        }
        g_dumpDir = opt.dumpDir;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        Log(ERROR, L"CoInitializeEx failed: 0x%08lX", hr);
        return 1;
    }

    int exitCode = 1;
    {
        // Scoped so the moniker releases before CoUninitialize.
        ComPtr<IMoniker> moniker;
        std::wstring deviceName;
        std::wstring devicePath;
        if (SUCCEEDED(DsFindCaptureMoniker(opt.deviceSubstr, opt.deviceIndex, moniker, deviceName)) &&
            SUCCEEDED(DsGetDevicePath(moniker.Get(), devicePath))) {
            Log(INFO, L"Device: %s", deviceName.c_str());
            Log(INFO, L"DevicePath: %s", devicePath.c_str());
            HANDLE h = DsOpenDevicePath(devicePath);
            if (h == INVALID_HANDLE_VALUE) {
                Log(ERROR, L"CreateFile on device failed (err=%lu).", GetLastError());
            } else {
                if (!opt.comPort.empty()) {
                    ComTriggerOpen(opt.comPort);
                }

                CamDumpRegisters(h);
                RunRegisterTests(h);

                // Continuous streaming at both frame depths: at least
                // VIDEO_MIN_FRAMES consecutive frames per pass, mirroring the
                // DirectShow sweep's contiguity verdict over the IOCTL pump.
                TestContinuousVideo(h, opt.frameHeaderChecks, PIXEL_FORMAT_MONO8);
                TestContinuousVideo(h, opt.frameHeaderChecks, PIXEL_FORMAT_MONO16);

                // On a real camera an externally-triggered shot is only
                // delivered for a trigger pulse, so without a usable trigger
                // port the snap tests can never receive a frame.  The fake
                // transmits on the ONE_SHOT arm itself (see the caveat in the
                // file header), and header checks are only usable against the
                // fake, so they identify a port-less run that can still snap.
                if (g_comPort != INVALID_HANDLE_VALUE || opt.frameHeaderChecks) {
                    // The full snap sequence at both frame depths: the MONO16
                    // pass doubles the frame size, covering the driver's
                    // bpp-derived prepare geometry and the 16-bit isochronous
                    // payload.
                    TestTriggeredSnap(h, opt.snapCount, opt.frameHeaderChecks, PIXEL_FORMAT_MONO8);
                    TestTriggeredSnap(h, opt.snapCount, opt.frameHeaderChecks, PIXEL_FORMAT_MONO16);

                    // The single-slot ring at both depths: catches
                    // teardown-time clobbering of the delivered frame,
                    // which the 4-slot ring cannot see.
                    TestSnapSingleSlotRing(h, opt.frameHeaderChecks, PIXEL_FORMAT_MONO8);
                    TestSnapSingleSlotRing(h, opt.frameHeaderChecks, PIXEL_FORMAT_MONO16);
                } else {
                    Log(INFO, L"SKIP triggered-snap tests (real camera without a usable trigger port)");
                }
                TestTriggerGating(h);
                TestTransmitOnly(h);
                TestComposition(h);
                TestExclusion(h, moniker.Get());

                // Multithreaded races over the capture-slot lifecycle
                // (concurrency.cpp). Last: the storms leave no state behind,
                // but a driver bug they trip could wedge the device.
                if (opt.stormSeconds > 0) {
                    RunConcurrencyTests(h, devicePath, moniker.Get(), opt.stormSeconds, g_passed, g_failed);
                } else {
                    Log(INFO, L"SKIP concurrency tests (--storm-seconds 0)");
                }

                ComTriggerClose();
                CloseHandle(h);
                Log(INFO, L"ioctl tests: %d passed, %d failed.", g_passed, g_failed);
                exitCode = (g_failed == 0) ? 0 : 1;
            }
        }
    }

    CoUninitialize();
    return exitCode;
}
