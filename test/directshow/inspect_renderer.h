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
/// InspectRenderer — a minimal in-process video renderer that accepts ANY
/// MEDIATYPE_Video subtype and feeds every delivered frame to FrameStats.
///
/// qedit's Sample Grabber refuses the camera's exotic subtypes (Bayer / Mono16),
/// so the headless path terminates the graph in this filter instead of a bare
/// Null Renderer: capture -> [Smart Tee] -> InspectRenderer taps the native
/// bytes for the non-black pixel check, regardless of subtype.  The DirectShow
/// base classes (CBaseRenderer) are not in the Windows 7.1A SDK that the v140_xp
/// toolset builds against, so the filter, its input pin (InspectInputPin) and a
/// pin enumerator (InspectEnumPins) are hand-rolled COM.  The input pin shares
/// the filter's refcount (the pin is part of the filter).
#pragma once

#include <dshow.h>

#include "inspect_input_pin.h"

class FrameStats;

class InspectRenderer : public IBaseFilter, public IAMFilterMiscFlags {
public:
    InspectRenderer(FrameStats *stats);
    ~InspectRenderer();

    /// IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    /// IPersist
    STDMETHODIMP GetClassID(CLSID *pClassID);

    /// IMediaFilter
    STDMETHODIMP Stop();
    STDMETHODIMP Pause();
    STDMETHODIMP Run(REFERENCE_TIME);
    STDMETHODIMP GetState(DWORD, FILTER_STATE *pState);
    STDMETHODIMP SetSyncSource(IReferenceClock *pClock);
    STDMETHODIMP GetSyncSource(IReferenceClock **pClock);

    /// IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins **ppEnum);
    STDMETHODIMP FindPin(LPCWSTR Id, IPin **ppPin);
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *pInfo);
    STDMETHODIMP JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR pName);
    STDMETHODIMP QueryVendorInfo(LPWSTR *);

    /// IAMFilterMiscFlags: report the renderer flag so the filter-graph
    /// manager treats the graph as complete (no extra renderer is added) and
    /// runs it.
    STDMETHODIMP_(ULONG) GetMiscFlags();

private:
    LONG m_ref;
    IFilterGraph *m_graph; ///< weak
    IReferenceClock *m_clock;
    FILTER_STATE m_state;
    WCHAR m_name[MAX_FILTER_NAME];
    InspectInputPin m_pin;
};
