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
/// DirectShowSweep — owns the filter graph and every camera interface so the
/// per-combination helpers do not have to be threaded through parameter lists.
/// Implemented in sweep.cpp.
///
/// Lifetime: construct, Initialize() (build the graph + bind the interfaces),
/// RunSweep() (enumerate -> filter -> run -> summarize).  COM init/uninit is the
/// caller's responsibility (see RunDirectShowTests).

#pragma once

#include <dshow.h>
#include <windows.h>

#include <string>
#include <vector>

#include "comptr.h"
#include "framestats.h"
#include "image_controls.h"
#include "sweep_types.h"

namespace dsweep {

class DirectShowSweep {
public:
    DirectShowSweep() = default;

    /// Find the device, build the graph and bind the capture interfaces.
    /// Returns false (after logging) if any required step fails.
    ///
    /// @param opt  Command-line options for the sweep.
    /// @return true on success, false if any required step fails.
    bool Initialize(const Options &opt);

    /// Print, once at setup, which image controls this camera supports and which
    /// it does not: one line per control in the VideoProcAmp / CameraControl
    /// tables with the advertised range, step, default and auto/manual
    /// capability, or "not supported".  This makes the camera's support set
    /// obvious up front — the driver advertises only the controls whose DCAM
    /// feature register is present, so a real camera lists a subset while the
    /// fake lists everything — before the per-combination checks report
    /// against it.
    void LogPropertySupport();

    /// Run the whole sweep and print the summary.
    ///
    /// @return Process exit code: 0 if all passed, 2 if some failed, 1 for bad selection.
    int RunSweep();

private:
    /// List every advertised capture mode (after the -m/-7 selection) without
    /// building any render chain or capturing a frame.  For each format it prints
    /// the capability line plus every (format, frame-rate) point a sweep would
    /// test, each annotated with the exact "-m <index> -f <fps>" selector, so a
    /// single mode/rate can be picked out for a later targeted run.
    ///
    /// @return 0.
    int ListModes();

    // -----------------------------------------------------------------------
    // Pipeline: enumerate -> filter -> intervals -> combinations.
    // -----------------------------------------------------------------------

    /// Read every advertised format capability into a Capability (geometry,
    /// subtype and the owned media-type template).
    ///
    /// @return Vector of enumerated capabilities.
    std::vector<CapabilityPtr> EnumerateCapabilities();

    /// Keep only the capabilities the command-line selection asks for:
    ///   -m/--mode        : exactly the one capability index.
    ///   -7/--test-format7: only the scalable Format 7 capabilities.
    /// Dropped capabilities free their media type as their shared_ptr expires.
    ///
    /// @param all  Full capability list from EnumerateCapabilities.
    /// @param opt  Command-line options supplying the selection criteria.
    /// @return Filtered capability list.
    static std::vector<CapabilityPtr> FilterCapabilities(std::vector<CapabilityPtr> all, const Options &opt);

    /// Decide which frame intervals to test for one format:
    ///   -f/--fps  : exactly one interval, derived from the requested fps (logs a
    ///               skip message and returns empty if it falls outside range).
    ///   otherwise : every standard DCAM rate the format's range allows.
    ///
    /// @param caps  Stream-configuration caps for the format.
    /// @return Vector of 100-ns frame intervals to test.
    std::vector<LONGLONG> GetIntervals(const VIDEO_STREAM_CONFIG_CAPS &caps) const;

    /// Turn one format into the (format, frame-rate) Combinations to test.  If no
    /// standard interval fits the range (and no explicit --fps was given), fall
    /// back to a single combination at the format's own default rate.
    ///
    /// @param cap  The format capability to build combinations for.
    /// @return Vector of Combinations to test for this format.
    std::vector<Combination> BuildCombinations(const CapabilityPtr &cap) const;

    // -----------------------------------------------------------------------
    // Running one combination.
    // -----------------------------------------------------------------------

    /// Run one combination with the chosen output and return its result (the
    /// caller prints it).  In the default (windowed) mode a preview-leg render
    /// failure only costs the on-screen window (the InspectRenderer tap still
    /// measures the device); the headless retry covers genuine streaming
    /// failures.  --headless never renders the preview leg.
    ///
    /// @param combo  The (format, frame-rate) combination to evaluate.
    /// @return The result of running the combination.
    ComboResult EvaluateCombo(const Combination &combo);

    /// Run one (format, frame-rate) combination end-to-end: tear down, SetFormat,
    /// build the pixel tap + sink, RenderStream, run while exercising the image
    /// controls and watching for streaming error events, then decide the verdict.
    ///
    /// @param pmt             Media type with the frame interval already set.
    /// @param label           Human-readable combination label for logging.
    /// @param useNullRenderer true for headless (InspectRenderer only), false for windowed.
    /// @return The result of running the combination.
    ComboResult RunCombination(const AM_MEDIA_TYPE *pmt, const std::wstring &label, bool useNullRenderer);

    /// Run the graph, exercise the image controls, dwell while watching for
    /// streaming error events, then decide the streaming verdict and the overall
    /// pass/fail.  Fills the streaming/diagnostic fields of `r`.
    ///
    /// @param r     Result record to fill with streaming and diagnostic data.
    /// @param pmt   Active media type (for window sizing and header checks).
    /// @param label Combination label for logging.
    void RunAndMeasure(ComboResult &r, const AM_MEDIA_TYPE *pmt, const std::wstring &label);

    /// Probe value for control `lane` (its index in kFeatureProbes) in the current
    /// combination (m_probeRound), within the control's advertised range [mn, mx]:
    /// mn + 1 + (lane + N*round) mod (mx - mn) with N controls.  Values differ
    /// across controls as far as their ranges allow and change every combination,
    /// so a stale frame from an earlier combination cannot match this
    /// combination's freshly set value even in a narrow range.
    ///
    /// @param lane  Control index in kFeatureProbes.
    /// @param mn    Minimum of the control's advertised range.
    /// @param mx    Maximum of the control's advertised range.
    /// @return Probe value within [mn+1, mx].
    long ProbeValue(int lane, long mn, long mx) const;

    /// Set every available image control to its distinct probe value (manual mode),
    /// recording the first Set() failure.  The value actually set is remembered in
    /// m_probeValue (-1: interface/control absent or Set failed, nothing to
    /// verify) and verified after the dwell, once frames carrying it have been
    /// delivered (see VerifyFeatureProbes).
    ///
    /// @param r  Result record; receives the first feature Set() failure.
    void ApplyFeatureProbes(ComboResult &r);

    /// Compare each control's probe value against the value embedded in the most
    /// recent frame's metadata (1:1 with the DCAM register), recording the first
    /// mismatch.  ApplyFeatureProbes must have run for this combination; controls
    /// it could not set (m_probeValue -1) are skipped.
    ///
    /// @param r  Result record; receives the mismatch count and first failure description.
    void VerifyFeatureProbes(ComboResult &r);

    /// Save the most recent frame FrameStats captured for this combination as a
    /// PNG in m_opt.dumpDir (see the --dump-dir option), named after the
    /// capability index, geometry, subtype and frame rate.  No-op when no frame
    /// was delivered; a save failure is logged, never failed on.
    ///
    /// @param r    Result record supplying the capability index and frame rate.
    /// @param pmt  Active media type supplying the geometry and subtype.
    void SavePngSample(const ComboResult &r, const AM_MEDIA_TYPE *pmt);

    /// Print the pass count and the per-failure detail.
    ///
    /// @param results  All combination results from the sweep.
    /// @return Number of combinations that passed.
    int PrintSummary(const std::vector<ComboResult> &results) const;

    // Owned interfaces / state.
    ComPtr<IBaseFilter> m_captureFilter;
    ComPtr<IGraphBuilder> m_graph;
    ComPtr<ICaptureGraphBuilder2> m_capBuilder;
    ComPtr<IMediaControl> m_mediaControl;
    ComPtr<IMediaEventEx> m_mediaEvent;
    ComPtr<IAMStreamConfig> m_streamConfig;
    ComPtr<IAMVideoProcAmp> m_procAmp;
    ComPtr<IAMCameraControl> m_camControl;
    FrameStats m_stats; ///< grabber callback that inspects native-format pixels
    Options m_opt;
    std::wstring m_deviceName;
    int m_capsCount = 0;
    unsigned m_probeRound = 0;                        ///< bumped per streamed combination for unique probe values
    long m_probeValue[_countof(kFeatureProbes)] = {}; ///< value set per control this combination (-1: none)
};

} // namespace dsweep
