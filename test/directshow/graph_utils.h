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
/// Filter-graph plumbing helpers for the DirectShow sweep (implemented in
/// graph_utils.cpp): tear the render chain down to the capture filter, render
/// the capture pin into a sink across the pin categories, verify the tap's
/// connection is the native format, and log/configure the graph's filters and
/// video window.

#pragma once

#include <dshow.h>
#include <windows.h>

/// Remove every filter except the capture filter so the capture pin is left
/// disconnected and SetFormat() is legal again.
///
/// @param graph          The filter graph to modify.
/// @param captureFilter  The capture filter to retain in the graph.
void TeardownRenderChain(IGraphBuilder *graph, IBaseFilter *captureFilter);

/// RenderStream the capture filter into `intermediate`->`sink`, trying the
/// CAPTURE category first, then PREVIEW, then "any pin".
///
/// @param capBuilder     The capture graph builder to use for rendering.
/// @param captureFilter  The capture filter whose output pin is rendered.
/// @param intermediate   An optional intermediate filter inserted between capture and sink (may be NULL).
/// @param sink           The sink filter that receives the rendered stream.
/// @return S_OK if a pin category succeeded, or the last HRESULT error if all attempts failed.
HRESULT RenderStreamAny(ICaptureGraphBuilder2 *capBuilder, IBaseFilter *captureFilter, IBaseFilter *intermediate,
                        IBaseFilter *sink);

/// Check that the tap sink's input pin is connected with exactly the format
/// that was set on the capture pin: same subtype and same image geometry,
/// including the SIGN of biHeight (a converter that flips a top-down frame to
/// bottom-up reorders the bytes just as destructively as a colour-space
/// conversion).  A converter inserted upstream of the tap would invalidate the
/// pixel statistics and destroy the embedded frame header.
///
/// @param tap  The tap sink filter whose input connection is inspected.
/// @param pmt  The media type that was set on the capture pin.
/// @return True if the tap's input connection matches the requested format, false if a converter was inserted.
bool TapConnectionIsNative(IBaseFilter *tap, const AM_MEDIA_TYPE *pmt);

/// Log every filter currently in the graph (diagnostic): reveals which
/// converter(s) DirectShow inserted to satisfy the connection — e.g. a
/// "Color Space Converter" for the RGB8/palettized -> RGB24 path.
///
/// @param graph  The filter graph whose filters are logged.
void LogGraphFilters(IGraphBuilder *graph);

/// Position/caption the renderer's video window (if one was created).
///
/// @param graph    The filter graph whose video window is configured.
/// @param caption  The window title to set.
/// @param width    The desired window width in pixels.
/// @param height   The desired window height in pixels.
void ConfigureVideoWindow(IGraphBuilder *graph, const wchar_t *caption, LONG width, LONG height);

/// Report whether the graph exposes a renderer video window and, if so, its
/// visibility and geometry.  When the windowed renderer fails to materialise (no
/// IVideoWindow on the graph) this is what shows the headless path was taken
/// and no window will ever appear.
///
/// @param graph  The filter graph to inspect for a video window.
void LogVideoWindowState(IGraphBuilder *graph);
