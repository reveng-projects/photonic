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
/// DirectShowSweep implementation.  See sweep.h for the class contract and
/// directshow.cpp for the suite overview and usage.

#include "sweep.h"

#include <cwctype>
#include <iomanip>
#include <sstream>
#include <utility>

#include "../utils/log.h"
#include "../utils/pngwriter.h"

#include "finddevice.h"
#include "format_names.h"
#include "graph_utils.h"
#include "inspect_renderer.h"
#include "mediatype.h"

/// Each combination is sampled until at least this many frames have been
/// captured, even when that takes longer than the configured dwell, so low-fps
/// modes (e.g. 1.875 fps) still produce a meaningful number of frames.
static const long MIN_FRAMES_PER_TEST = 15;

/// Hard cap on how long past the configured dwell a run will keep waiting for
/// MIN_FRAMES_PER_TEST frames, so a stalled / black stream cannot loop forever.
/// 15 frames at the slowest standard DCAM rate (1.875 fps) need ~8 s.
static const DWORD MIN_FRAMES_EXTRA_DWELL_MS = 12000;

/// If no new frame arrives for this long after the configured dwell, the stream
/// is treated as stalled and the wait gives up (rather than burning the whole
/// MIN_FRAMES_EXTRA_DWELL_MS cap).  It must comfortably exceed one frame interval
/// at the slowest standard DCAM rate (1.875 fps -> ~533 ms) so a genuinely slow
/// but healthy stream keeps making progress and is not mistaken for a stall.
static const DWORD FRAME_STALL_TIMEOUT_MS = 3000;

namespace dsweep {

bool DirectShowSweep::Initialize(const Options &opt) {
    m_opt = opt;

    HRESULT hr = FindCaptureDevice(m_opt.deviceSubstr, m_opt.deviceIndex, &m_captureFilter, m_deviceName);
    if (FAILED(hr)) {
        return false;
    }
    Log(INFO, L"Using device: %s", m_deviceName.c_str());
    Log(INFO, L"Output mode: %s",
        m_opt.headless ? L"headless (no video window)" : L"windowed video renderer (windowless fallback on failure)");
    Log(INFO, L"Frame-header check: %s", m_opt.frameHeaderChecks ? L"on" : L"off (real-camera mode)");
    m_stats.SetVerifyMeta(m_opt.frameHeaderChecks);
    m_stats.SetCaptureLastFrame(!m_opt.dumpDir.empty());
    if (!m_opt.dumpDir.empty()) {
        Log(INFO, L"Sample frames: one PNG per combination in %s", m_opt.dumpDir.c_str());
    }

    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_graph));
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_capBuilder));
    }
    if (FAILED(hr)) {
        Log(ERROR, L"Failed to create graph builder: 0x%08lX", hr);
        return false;
    }

    m_capBuilder->SetFiltergraph(m_graph.Get());
    m_graph.As(&m_mediaControl);
    m_graph.As(&m_mediaEvent);

    hr = m_graph->AddFilter(m_captureFilter.Get(), L"Capture");
    if (FAILED(hr)) {
        Log(ERROR, L"AddFilter failed: 0x%08lX", hr);
        return false;
    }

    // IAMStreamConfig on the capture pin.
    hr = m_capBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, m_captureFilter.Get(),
                                     IID_PPV_ARGS(&m_streamConfig));
    if (FAILED(hr)) {
        // Some minidrivers only expose the config on the preview category.
        hr = m_capBuilder->FindInterface(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, m_captureFilter.Get(),
                                         IID_PPV_ARGS(&m_streamConfig));
    }
    if (FAILED(hr)) {
        Log(ERROR, L"Could not get IAMStreamConfig: 0x%08lX", hr);
        return false;
    }

    // Image-control interfaces (contrast/brightness/exposure/...).  These
    // live on the capture filter itself; absence just means the per-combo
    // property check is skipped (the format sweep still runs).
    m_captureFilter->QueryInterface(IID_PPV_ARGS(&m_procAmp));
    m_captureFilter->QueryInterface(IID_PPV_ARGS(&m_camControl));
    Log(INFO, L"Image controls: VideoProcAmp %s, CameraControl %s", m_procAmp ? L"available" : L"absent",
        m_camControl ? L"available" : L"absent");
    LogPropertySupport();

    int capsSize = 0;
    hr = m_streamConfig->GetNumberOfCapabilities(&m_capsCount, &capsSize);
    if (FAILED(hr) || capsSize != sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
        Log(ERROR, L"GetNumberOfCapabilities failed or unexpected caps size (hr=0x%08lX size=%d)", hr, capsSize);
        return false;
    }
    Log(INFO, L"Capture filter advertises %d format capabilities.", m_capsCount);
    if (m_opt.f7Only) {
        Log(INFO, L"--test-format7: testing only Format 7 (scalable) capabilities.");
    }
    return true;
}

void DirectShowSweep::LogPropertySupport() {
    int supported = 0, notSupported = 0;
    auto logOne = [&](auto *iface, const wchar_t *setName, const PropItem &p, long flagAuto, long flagManual) {
        long mn = 0, mx = 0, step = 0, def = 0, caps = 0;
        HRESULT hr = iface != nullptr ? iface->GetRange(p.id, &mn, &mx, &step, &def, &caps) : E_NOINTERFACE;
        if (FAILED(hr)) {
            notSupported++;
            Log(INFO, L"%s %s: not supported", setName, p.name);
            return;
        }
        supported++;
        const wchar_t *capsStr = ((caps & flagAuto) != 0 && (caps & flagManual) != 0) ? L"auto+manual"
                                 : (caps & flagAuto) != 0                             ? L"auto"
                                 : (caps & flagManual) != 0                           ? L"manual"
                                                                                      : L"none";
        Log(INFO, L"%s %s: supported, range [%ld..%ld] step %ld default %ld, %s", setName, p.name, mn, mx, step, def,
            capsStr);
    };
    Log(INFO, L"Image-control support:");
    for (auto &p : kProcAmpProps) {
        logOne(m_procAmp.Get(), L"VideoProcAmp", p, (long) VideoProcAmp_Flags_Auto, (long) VideoProcAmp_Flags_Manual);
    }
    for (auto &p : kCameraControlProps) {
        logOne(m_camControl.Get(), L"CameraControl", p, (long) CameraControl_Flags_Auto,
               (long) CameraControl_Flags_Manual);
    }
    Log(INFO, L"Image-control support: %d supported, %d not supported.", supported, notSupported);
}

int DirectShowSweep::RunSweep() {
    if (m_opt.listModes) {
        return ListModes();
    }

    if (m_opt.modeIndex >= 0 && m_opt.modeIndex >= m_capsCount) {
        Log(ERROR, L"Requested capture mode %d is out of range (only %d advertised).", m_opt.modeIndex, m_capsCount);
        return 1;
    }

    // Enumerate every advertised format, then narrow to the selection.
    std::vector<CapabilityPtr> caps = FilterCapabilities(EnumerateCapabilities(), m_opt);

    std::vector<ComboResult> results;
    for (const CapabilityPtr &cap : caps) {
        Log(INFO, L"[cap %2d] %ldx%ld %s %ld-bit  fps %.3g..%.3g%s", cap->index, cap->width, cap->height,
            cap->subName.c_str(), cap->bits,
            FpsFromInterval(cap->caps.MaxFrameInterval), // slowest
            FpsFromInterval(cap->caps.MinFrameInterval), // fastest
            cap->IsScalableFormat7() ? L"  [Format 7 scalable]" : L"");

        for (const Combination &combo : BuildCombinations(cap)) {
            combo.PrintHeader();
            LogFlush();
            ComboResult res = EvaluateCombo(combo);
            res.PrintDiagnostics();
            res.Print();
            results.push_back(std::move(res));
        }
    }

    m_mediaControl->Stop();
    TeardownRenderChain(m_graph.Get(), m_captureFilter.Get());

    if (m_opt.f7Only && results.empty()) {
        Log(WARN, L"No Format 7 (scalable) capabilities were advertised; nothing tested.");
    }

    int passCount = PrintSummary(results);
    return (passCount == (int) results.size()) ? 0 : 2;
}

int DirectShowSweep::ListModes() {
    std::vector<CapabilityPtr> caps = FilterCapabilities(EnumerateCapabilities(), m_opt);
    if (caps.empty()) {
        Log(WARN, L"No capture modes match the current selection.");
        return 0;
    }

    Log(INFO, L"Advertised capture modes (select with -m <index>, optionally -f <fps>):");
    for (const CapabilityPtr &cap : caps) {
        Log(INFO, L"[cap %2d] %ldx%ld %s %ld-bit  fps %.3g..%.3g%s", cap->index, cap->width, cap->height,
            cap->subName.c_str(), cap->bits,
            FpsFromInterval(cap->caps.MaxFrameInterval), // slowest
            FpsFromInterval(cap->caps.MinFrameInterval), // fastest
            cap->IsScalableFormat7() ? L"  [Format 7 scalable]" : L"");

        for (const Combination &combo : BuildCombinations(cap)) {
            Log(INFO, L"         -m %d -f %.3g  (%s)", cap->index, FpsFromInterval(combo.interval),
                combo.label.c_str());
        }
    }
    return 0;
}

std::vector<CapabilityPtr> DirectShowSweep::EnumerateCapabilities() {
    std::vector<CapabilityPtr> caps;
    for (int i = 0; i < m_capsCount; i++) {
        VIDEO_STREAM_CONFIG_CAPS vc;
        AM_MEDIA_TYPE *pmt = nullptr;
        HRESULT hr = m_streamConfig->GetStreamCaps(i, &pmt, (BYTE *) &vc);
        if (hr != S_OK || pmt == nullptr) {
            Log(ERROR, L"GetStreamCaps(%d) failed: 0x%08lX", i, hr);
            continue;
        }

        auto cap = std::make_shared<Capability>();
        cap->index = i;
        cap->pmt.reset(pmt);
        cap->caps = vc;
        cap->subName = FormatSubtype(pmt->subtype);

        if (IsEqualGUID(pmt->formattype, FORMAT_VideoInfo) && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *) pmt->pbFormat;
            cap->width = vih->bmiHeader.biWidth;
            cap->height = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
            cap->bits = vih->bmiHeader.biBitCount;
            cap->defaultInterval = vih->AvgTimePerFrame;
        }

        caps.push_back(std::move(cap));
    }
    return caps;
}

std::vector<CapabilityPtr> DirectShowSweep::FilterCapabilities(std::vector<CapabilityPtr> all, const Options &opt) {
    std::vector<CapabilityPtr> kept;
    for (CapabilityPtr &c : all) {
        if (opt.modeIndex >= 0 && c->index != opt.modeIndex) {
            continue;
        }
        if (opt.f7Only && !c->IsScalableFormat7()) {
            continue;
        }
        kept.push_back(std::move(c));
    }
    return kept;
}

std::vector<LONGLONG> DirectShowSweep::GetIntervals(const VIDEO_STREAM_CONFIG_CAPS &caps) const {
    // Standard DCAM frame intervals (100-ns units): 1.875 .. 60 fps.
    static const LONGLONG standardIntervals[] = {
        5333333, // 1.875 fps
        2666667, // 3.75
        1333333, // 7.5
        666667,  // 15
        333333,  // 30
        166667,  // 60
    };

    std::vector<LONGLONG> intervals;
    if (m_opt.fps > 0.0) {
        LONGLONG interval = (LONGLONG) (10000000.0 / m_opt.fps + 0.5);
        if (interval < caps.MinFrameInterval || interval > caps.MaxFrameInterval) {
            Log(WARN, L"%.3g fps (%lld) out of range [%.3g..%.3g fps]; skipped", m_opt.fps, interval,
                FpsFromInterval(caps.MaxFrameInterval), FpsFromInterval(caps.MinFrameInterval));
        } else {
            intervals.push_back(interval);
        }
    } else {
        for (LONGLONG interval : standardIntervals) {
            if (interval >= caps.MinFrameInterval && interval <= caps.MaxFrameInterval) {
                intervals.push_back(interval);
            }
        }
    }
    return intervals;
}

std::vector<Combination> DirectShowSweep::BuildCombinations(const CapabilityPtr &cap) const {
    // Build one Combination at the given interval; isDefault marks the
    // fall-back to the format's own rate (and appends "(default)").
    auto makeCombo = [&](LONGLONG interval, bool isDefault) {
        std::wostringstream label;
        label.precision(3); // 3 significant digits, matching the old "%.3g"
        label << cap->width << L"x" << cap->height << L" " << cap->subName << L" @ " << FpsFromInterval(interval)
              << L" fps";
        if (isDefault) {
            label << L" (default)";
        }

        Combination c;
        c.cap = cap;
        c.interval = interval;
        c.isDefault = isDefault;
        c.label = label.str();
        return c;
    };

    std::vector<Combination> combos;
    std::vector<LONGLONG> intervals = GetIntervals(cap->caps);
    if (!intervals.empty()) {
        for (LONGLONG interval : intervals) {
            combos.push_back(makeCombo(interval, false)); // isDefault
        }
    } else if (m_opt.fps <= 0.0) {
        // No standard interval fits the range: fall back to the default rate.
        combos.push_back(makeCombo(cap->defaultInterval, true)); // isDefault
    }
    return combos;
}

ComboResult DirectShowSweep::EvaluateCombo(const Combination &combo) {
    // Program the requested frame rate into a private copy of the format
    // template; this copy outlives both the windowed and headless attempts.
    AM_MEDIA_TYPE mt;
    if (FAILED(CopyMediaType(&mt, combo.cap->pmt.get()))) {
        ComboResult r{};
        r.label = combo.label;
        r.capIndex = combo.cap->index;
        r.fps = FpsFromInterval(combo.interval);
        r.failReason = L"CopyMediaType failed";
        return r;
    }
    if (IsEqualGUID(mt.formattype, FORMAT_VideoInfo) && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        ((VIDEOINFOHEADER *) mt.pbFormat)->AvgTimePerFrame = combo.interval;
    }

    ComboResult res = RunCombination(&mt, combo.label, m_opt.headless); // useNullRenderer

    // Only retry headless when the video renderer/streaming actually failed;
    // an image-control round-trip failure is independent of the renderer and
    // would just fail identically headless.
    if (!m_opt.headless && !res.streamingOk) {
        Log(WARN, L"video renderer failed (%s); retrying headless ...", res.failReason);
        LogFlush();
        res = RunCombination(&mt, combo.label, true); // useNullRenderer
        res.fellBackToHeadless = true;
    }
    res.capIndex = combo.cap->index;
    res.fps = FpsFromInterval(combo.interval);

    if (!m_opt.dumpDir.empty()) {
        SavePngSample(res, &mt);
    }

    FreeMediaTypeContents(&mt);
    return res;
}

ComboResult DirectShowSweep::RunCombination(const AM_MEDIA_TYPE *pmt, const std::wstring &label, bool useNullRenderer) {
    ComboResult r{};
    r.label = label;
    r.failReason = L"";

    Log(INFO, L"RunCombination: %s renderer", useNullRenderer ? L"headless/null" : L"windowed");

    m_mediaControl->Stop();
    TeardownRenderChain(m_graph.Get(), m_captureFilter.Get());
    Log(INFO, L"graph stopped and torn down to the capture filter");

    // SetFormat must run on the now-disconnected capture pin.  Dump the
    // media type first: if SetFormat hangs or faults in ksproxy, the last
    // thing logged is the exact format it choked on.
    Log(INFO, L"SetFormat: type=%s fmt=%s cbFormat=%lu sample=%lu fixed=%d", FormatMajorName(pmt->majortype),
        FormatSubtype(pmt->subtype).c_str(), pmt->cbFormat, pmt->lSampleSize, pmt->bFixedSizeSamples);
    if (IsEqualGUID(pmt->formattype, FORMAT_VideoInfo) && pmt->cbFormat >= sizeof(VIDEOINFOHEADER) &&
        pmt->pbFormat != nullptr) {
        VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *) pmt->pbFormat;
        Log(INFO, L"SetFormat: %ldx%ld bits=%u compression=0x%lX sizeImage=%lu avgTime=%lld", vih->bmiHeader.biWidth,
            vih->bmiHeader.biHeight, vih->bmiHeader.biBitCount, vih->bmiHeader.biCompression,
            vih->bmiHeader.biSizeImage, vih->AvgTimePerFrame);
    }
    r.hrSetFormat = m_streamConfig->SetFormat(const_cast<AM_MEDIA_TYPE *>(pmt));
    Log(INFO, L"SetFormat -> 0x%08lX", r.hrSetFormat);
    if (FAILED(r.hrSetFormat)) {
        r.failReason = L"SetFormat failed";
        return r;
    }

    // Build the pixel tap and, in windowed mode, the video window.
    //
    // The tap is always the InspectRenderer terminating the capture leg:
    // it accepts ANY video subtype (qedit's Sample Grabber rejects the
    // camera's Bayer / Mono16 and the top-down BI_RGB types) and feeds
    // every frame's native bytes into FrameStats, so the pixel statistics
    // and the embedded frame-header check always run on unconverted data.
    //
    // Windowed: additionally render the Smart Tee's preview leg into the
    // default video renderer so the video is shown on screen.  Any
    // converter the renderer needs (e.g. RGB8 top-down -> RGB32) is then
    // inserted on the preview leg only, where it cannot touch the tapped
    // bytes.  An inline Sample Grabber ahead of the renderer cannot achieve
    // this: for the top-down RGB8 modes the graph builder slots a Color
    // Space Converter upstream of the grabber, which hands it converted
    // bytes and destroys the embedded frame header.
    m_stats.Reset();

    ComPtr<IBaseFilter> sinkFilter;
    *sinkFilter.GetAddressOf() = new InspectRenderer(&m_stats);
    HRESULT hrAdd = m_graph->AddFilter(sinkFilter.Get(), L"Inspector");
    Log(INFO, L"added InspectRenderer sink -> 0x%08lX", hrAdd);
    if (FAILED(hrAdd)) {
        r.hrRender = hrAdd;
        r.failReason = L"adding InspectRenderer failed";
        return r;
    }
    r.grabberActive = true; // the InspectRenderer taps frames

    Log(INFO, L"RenderStream: capture -> inspector ...");
    r.hrRender = RenderStreamAny(m_capBuilder.Get(), m_captureFilter.Get(), nullptr, sinkFilter.Get());
    Log(INFO, L"RenderStream -> 0x%08lX", r.hrRender);
    if (FAILED(r.hrRender)) {
        r.failReason = L"RenderStream failed";
        return r;
    }

    // A converter upstream of the tap would invalidate everything measured
    // downstream, so treat it as a streaming failure.
    if (!TapConnectionIsNative(sinkFilter.Get(), pmt)) {
        r.failReason = L"tap not native (converter upstream)";
        return r;
    }

    if (!useNullRenderer) {
        HRESULT hrPreview = m_capBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, m_captureFilter.Get(),
                                                       nullptr, nullptr);
        Log(INFO, L"RenderStream[PIN_CATEGORY_PREVIEW -> video renderer] -> 0x%08lX", hrPreview);
        if (FAILED(hrPreview)) {
            // The tap still measures the device; only the on-screen window
            // is unavailable for this format.
            Log(WARN, L"video renderer could not be connected (0x%08lX); running without a window", hrPreview);
            r.fellBackToHeadless = true;
        }
    }

    RunAndMeasure(r, pmt, label);
    return r;
}

void DirectShowSweep::RunAndMeasure(ComboResult &r, const AM_MEDIA_TYPE *pmt, const std::wstring &label) {
    LogGraphFilters(m_graph.Get());

    // New round of probe values for this streamed combination, so they are
    // never reused and a stale frame from an earlier combination cannot pass.
    m_probeRound++;

    // Caption the window with the combination under test.
    LONG width = 0, height = 0;
    if (IsEqualGUID(pmt->formattype, FORMAT_VideoInfo) && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *) pmt->pbFormat;
        width = vih->bmiHeader.biWidth;
        height = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
    }
    ConfigureVideoWindow(m_graph.Get(), label.c_str(), width, height);
    LogVideoWindowState(m_graph.Get());

    // Set each control to its distinct probe value BEFORE starting the graph,
    // so the camera's pre-filled ring — and thus the very first delivered frame
    // — already carries it.  Filter-level properties reach the camera registers
    // independent of stream state, and the fake fills its ring from the current
    // values at stream start, so no waiting for the ring to drain is needed.
    // The probes are verified against the embedded frame header, which only the
    // fake emits, so they are skipped entirely in real-camera mode — a real
    // camera implements only a subset of the controls with narrower ranges, and
    // the probe values would be pointless writes there.
    if (m_opt.frameHeaderChecks && (m_procAmp != nullptr || m_camControl != nullptr)) {
        ApplyFeatureProbes(r);
    }

    r.errorEvent = 0;
    r.hrRun = m_mediaControl->Run();
    Log(INFO, L"Run -> 0x%08lX", r.hrRun);

    // While this combination is streaming, exercise the VideoProcAmp /
    // CameraControl image controls (contrast, brightness, exposure, ...) so
    // the sweep also proves the property path works in every format/rate.
    // CheckProperties restores each control to its current (probe) value, so
    // the streamed frames keep carrying the probe values.  Frame-header mode
    // means the fake device, which implements every control; a real camera
    // implements only a subset, so unsupported controls are then skipped
    // rather than failed.
    if (SUCCEEDED(r.hrRun) && (m_procAmp != nullptr || m_camControl != nullptr)) {
        PropSummary ps = CheckProperties(m_procAmp.Get(), m_camControl.Get(), m_opt.frameHeaderChecks);
        r.propsExpected = ps.expected;
        r.propsPassed = ps.passed;
        r.propFail = ps.firstFail;
    }

    // Pump messages and watch for streaming error events while the graph
    // runs.  Dwell for at least the configured duration, then keep running
    // until MIN_FRAMES_PER_TEST frames have been captured so low-fps modes
    // still yield a meaningful sample (15 frames at 1.875 fps need ~8 s).
    //
    // Two backstops keep a non-delivering stream from hanging: a per-frame
    // stall timeout (give up once no new frame has arrived for
    // FRAME_STALL_TIMEOUT_MS) and an absolute cap (MIN_FRAMES_EXTRA_DWELL_MS
    // past the duration).  The stall timeout is what stops a windowed
    // renderer that back-pressures the capture pin to a halt after a few
    // frames (seen with RGB8/Y8 Format 7 modes) from waiting out the whole
    // cap.  Only the streaming path is waited on: if Run() failed there is
    // nothing to wait for.
    const bool waitForFrames = SUCCEEDED(r.hrRun) && r.grabberActive;
    DWORD start = GetTickCount();
    long frames = 0;
    long lastFrames = 0;
    DWORD lastProgress = start; // last time the frame count increased
    for (;;) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        long evCode;
        LONG_PTR p1, p2;
        while (m_mediaEvent->GetEvent(&evCode, &p1, &p2, 0) == S_OK) {
            if (evCode == EC_ERRORABORT || evCode == EC_STREAM_ERROR_STOPPED ||
                evCode == EC_STREAM_ERROR_STILLPLAYING || evCode == EC_DEVICE_LOST) {
                r.errorEvent = evCode;
                Log(WARN, L"streaming error event 0x%lX (p1=0x%08lX)", evCode, (long) p1);
            }
            m_mediaEvent->FreeEventParams(evCode, p1, p2);
        }
        Sleep(20);

        int mx = 0, mn = 0;
        double mean = 0.0;
        m_stats.Get(frames, mx, mn, mean);
        DWORD now = GetTickCount();
        if (frames > lastFrames) {
            lastFrames = frames;
            lastProgress = now;
        }
        DWORD elapsed = now - start;

        // Always honour the minimum dwell first.
        if (elapsed < m_opt.durationMs) {
            continue;
        }
        // Got enough frames (or there is nothing to wait for): done.
        if (!waitForFrames || frames >= MIN_FRAMES_PER_TEST) {
            break;
        }
        // No new frame for a while: the stream has stalled; stop waiting.
        if (now - lastProgress >= FRAME_STALL_TIMEOUT_MS) {
            Log(WARN, L"capture stalled: %ld/%ld frames, no new frame for %lu ms", frames, MIN_FRAMES_PER_TEST,
                now - lastProgress);
            break;
        }
        // Absolute backstop in case frames keep trickling below the target.
        if (elapsed >= m_opt.durationMs + MIN_FRAMES_EXTRA_DWELL_MS) {
            Log(WARN, L"dwell cap reached after %lu ms with only %ld/%ld frames captured", elapsed, frames,
                MIN_FRAMES_PER_TEST);
            break;
        }
    }
    DWORD dwellMs = GetTickCount() - start;

    m_stats.Get(r.frames, r.maxByte, r.minByte, r.meanByte);
    m_stats.GetMeta(r.meta);
    r.frameHeaderChecks = m_opt.frameHeaderChecks;
    Log(INFO, L"captured %ld frames in %lu ms (minimum %ld)", r.frames, dwellMs, MIN_FRAMES_PER_TEST);

    // Streaming verdict — the renderer/capture part only.
    r.streamingOk = false;
    if (FAILED(r.hrRun)) {
        r.failReason = L"Run failed";
    } else if (r.errorEvent != 0) {
        r.failReason = L"streaming error event";
    } else if (r.grabberActive && r.frames == 0) {
        r.failReason = L"no frames (black window)";
    } else if (r.grabberActive && r.maxByte == 0) {
        r.failReason = L"all-black frames";
    } else {
        r.streamingOk = true;
    }

    // End-to-end frame verdict — the embedded header (common/frame_meta.h)
    // proves each delivered frame arrived intact and in order.  Strict: every
    // delivered frame must carry a valid header, its stored checksum must
    // match the whole frame, its size must match, and the 0-based indices
    // must run 0,1,2,... with no gaps.  Only meaningful once frames flow, so
    // a streaming failure is reported on its own (above) and not masked here.
    //
    // A real camera emits no embedded test header, so the check is gated on
    // --frame-header-check: when set false it is treated as passing and the
    // embedded control verification below (which also relies on the header)
    // is skipped too.
    if (!m_opt.frameHeaderChecks) {
        r.frameCheckOk = true;
    } else {
        r.frameCheckOk =
            !r.grabberActive || (r.streamingOk && r.frames > 0 && r.meta.withHeader == r.frames &&
                                 r.meta.badHeader == 0 && r.meta.badChecksum == 0 && r.meta.badSize == 0 &&
                                 r.meta.badFeatures == 0 && r.meta.outOfOrder == 0 && r.meta.indexContiguous);
        if (r.streamingOk && !r.frameCheckOk) {
            if (r.meta.badChecksum > 0) {
                r.failReason = L"frame checksum mismatch";
            } else if (r.meta.badSize > 0) {
                r.failReason = L"frame size mismatch";
            } else if (r.meta.badHeader > 0 || r.meta.withHeader != r.frames) {
                r.failReason = L"missing/invalid frame header";
            } else if (r.meta.badFeatures > 0) {
                r.failReason = L"frame feature-set mismatch";
            } else if (r.meta.firstIndex != 0) {
                r.failReason = L"frames did not start at index 0";
            } else {
                r.failReason = L"frames out of order / dropped";
            }
        }
    }

    // End-to-end control verdict: confirm the probe values set above survived
    // DirectShow -> driver -> camera -> frame.  Only checkable once frames are
    // flowing and intact; otherwise it cannot be evaluated and does not fail.
    // It reads the values back from the embedded header, so it is skipped when
    // frame-header checks are off (no header to read from).
    r.featureValuesOk = true;
    if (m_opt.frameHeaderChecks && r.streamingOk && r.frameCheckOk && r.frames > 0 &&
        (m_procAmp != nullptr || m_camControl != nullptr)) {
        VerifyFeatureProbes(r);
        r.featureValuesOk = (r.featuresMismatched == 0 && r.featureFail.empty());
        if (!r.featureValuesOk) {
            r.failReason = r.featuresMismatched > 0 ? L"feature value not echoed in frame" : L"feature set failed";
        }
    }

    // The image-control check is independent of the renderer; a failure here
    // is reported but must NOT drive the headless fallback (retrying the
    // renderer cannot change the property result).
    if (r.streamingOk && r.frameCheckOk && r.featureValuesOk && r.propsExpected > 0 &&
        r.propsPassed < r.propsExpected) {
        r.failReason = L"image-control check failed";
    }
}

long DirectShowSweep::ProbeValue(int lane, long mn, long mx) const {
    long n = (long) _countof(kFeatureProbes);
    return mn + 1 + (((long) lane + n * (long) m_probeRound) % (mx - mn));
}

void DirectShowSweep::ApplyFeatureProbes(ComboResult &r) {
    for (int i = 0; i < (int) _countof(kFeatureProbes); i++) {
        const FeatureProbe &fp = kFeatureProbes[i];
        long mn = 0, mx = 0, step = 0, def = 0, caps = 0;
        HRESULT hr;
        m_probeValue[i] = -1;
        if (fp.camControl && m_camControl != nullptr) {
            hr = m_camControl->GetRange(fp.id, &mn, &mx, &step, &def, &caps);
        } else if (!fp.camControl && m_procAmp != nullptr) {
            hr = m_procAmp->GetRange(fp.id, &mn, &mx, &step, &def, &caps);
        } else {
            continue; // interface absent; nothing to set or verify
        }
        if (FAILED(hr) || mx <= mn) {
            // Control not advertised (CheckProperties reports it as a failure
            // in fake mode) or a degenerate range; nothing to probe.
            continue;
        }
        long value = ProbeValue(i, mn, mx);
        hr = fp.camControl ? m_camControl->Set(fp.id, value, CameraControl_Flags_Manual)
                           : m_procAmp->Set(fp.id, value, VideoProcAmp_Flags_Manual);
        if (FAILED(hr)) {
            if (r.featureFail.empty()) {
                std::wostringstream b;
                b << fp.name << L" Set(" << value << L")=0x" << std::hex << std::uppercase << (unsigned long) hr;
                r.featureFail = b.str();
            }
            continue;
        }
        m_probeValue[i] = value;
    }
}

void DirectShowSweep::VerifyFeatureProbes(ComboResult &r) {
    r.featuresChecked = 0;
    r.featuresMismatched = 0;
    for (int i = 0; i < (int) _countof(kFeatureProbes); i++) {
        const FeatureProbe &fp = kFeatureProbes[i];
        if (m_probeValue[i] < 0) {
            continue;
        }
        r.featuresChecked++;
        long want = m_probeValue[i];
        long got = (long) FRAME_META_FEATURE_VALUE(r.meta.features[fp.metaIndex]);
        if (got != want) {
            r.featuresMismatched++;
            if (r.featureFail.empty()) {
                std::wostringstream b;
                b << fp.name << L" embedded=" << got << L" set=" << want;
                r.featureFail = b.str();
            }
        }
    }
}

/// Subtype names carry filename-hostile characters ("RGB8/Y8", "'Y16 '"):
/// keep the alphanumerics, map the rest to '-' and trim the ends.
static std::wstring SanitizeForFilename(const std::wstring &s) {
    std::wstring out;
    for (wchar_t c : s) {
        out += iswalnum(c) ? c : L'-';
    }
    size_t first = out.find_first_not_of(L'-');
    size_t last = out.find_last_not_of(L'-');
    return first == std::wstring::npos ? L"unknown" : out.substr(first, last - first + 1);
}

void DirectShowSweep::SavePngSample(const ComboResult &r, const AM_MEDIA_TYPE *pmt) {
    // Nothing delivered for this combination (it already failed on that).  The
    // frame-count guard also covers a RunCombination that bailed out before
    // resetting the stats: the captured frame is then a stale one from the
    // previous combination and must not be saved under this media type.
    if (r.frames <= 0) {
        return;
    }
    std::vector<BYTE> frame;
    m_stats.GetLastFrame(frame);
    if (frame.empty()) {
        return;
    }
    if (!IsEqualGUID(pmt->formattype, FORMAT_VideoInfo) || pmt->cbFormat < sizeof(VIDEOINFOHEADER)) {
        return;
    }
    VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *) pmt->pbFormat;
    LONG width = vih->bmiHeader.biWidth;
    LONG biHeight = vih->bmiHeader.biHeight;
    LONG height = biHeight < 0 ? -biHeight : biHeight;

    std::wostringstream name;
    name.precision(4); // 4 significant digits for the frame rate (e.g. 1.875)
    name << L"cap" << std::setw(2) << std::setfill(L'0') << r.capIndex << L"_" << width << L"x" << height << L"_"
         << SanitizeForFilename(FormatSubtype(pmt->subtype)) << L"_" << r.fps << L"fps.png";
    std::wstring path = m_opt.dumpDir + L"\\" + name.str();
    if (SaveFramePng(path.c_str(), frame.data(), frame.size(), width, biHeight, pmt->subtype)) {
        Log(INFO, L"saved sample frame %s", path.c_str());
    }
}

int DirectShowSweep::PrintSummary(const std::vector<ComboResult> &results) const {
    int passCount = 0;
    for (auto &r : results) {
        if (r.Passed()) {
            passCount++;
        }
    }
    Log(INFO, L"==================== SUMMARY ====================");
    Log(INFO, L"%d/%u combinations passed.", passCount, (unsigned) results.size());
    for (auto &r : results) {
        if (!r.Passed()) {
            r.PrintFailure();
        }
    }
    Log(INFO, L"================================================");
    return passCount;
}

} // namespace dsweep
