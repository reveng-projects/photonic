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
/// Implementation of InspectRenderer (see inspect_renderer.h).  EnumPins hands
/// out an InspectEnumPins, so the full enumerator definition is needed here.

#include "inspect_renderer.h"

#include "framestats.h"
#include "inspect_enum_pins.h"

InspectRenderer::InspectRenderer(FrameStats *stats)
    : m_ref(1), m_graph(nullptr), m_clock(nullptr), m_state(State_Stopped), m_pin(this, stats) {
    m_name[0] = 0;
}

InspectRenderer::~InspectRenderer() {
    if (m_clock) {
        m_clock->Release();
    }
}

/// IUnknown
STDMETHODIMP InspectRenderer::QueryInterface(REFIID riid, void **ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPersist) || IsEqualIID(riid, IID_IMediaFilter) ||
        IsEqualIID(riid, IID_IBaseFilter)) {
        *ppv = static_cast<IBaseFilter *>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IAMFilterMiscFlags)) {
        *ppv = static_cast<IAMFilterMiscFlags *>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) InspectRenderer::AddRef() {
    return (ULONG) InterlockedIncrement(&m_ref);
}
STDMETHODIMP_(ULONG) InspectRenderer::Release() {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        delete this;
    }
    return (ULONG) r;
}

/// IPersist
STDMETHODIMP InspectRenderer::GetClassID(CLSID *pClassID) {
    if (pClassID == nullptr) {
        return E_POINTER;
    }
    *pClassID = CLSID_NULL;
    return S_OK;
}

/// IMediaFilter
STDMETHODIMP InspectRenderer::Stop() {
    m_state = State_Stopped;
    return S_OK;
}
STDMETHODIMP InspectRenderer::Pause() {
    m_state = State_Paused;
    return S_OK;
}
STDMETHODIMP InspectRenderer::Run(REFERENCE_TIME) {
    m_state = State_Running;
    return S_OK;
}
STDMETHODIMP InspectRenderer::GetState(DWORD, FILTER_STATE *pState) {
    if (pState == nullptr) {
        return E_POINTER;
    }
    *pState = m_state;
    return S_OK;
}
STDMETHODIMP InspectRenderer::SetSyncSource(IReferenceClock *pClock) {
    if (pClock) {
        pClock->AddRef();
    }
    if (m_clock) {
        m_clock->Release();
    }
    m_clock = pClock;
    return S_OK;
}
STDMETHODIMP InspectRenderer::GetSyncSource(IReferenceClock **pClock) {
    if (pClock == nullptr) {
        return E_POINTER;
    }
    *pClock = m_clock;
    if (m_clock) {
        m_clock->AddRef();
    }
    return S_OK;
}

/// IBaseFilter
STDMETHODIMP InspectRenderer::EnumPins(IEnumPins **ppEnum) {
    if (ppEnum == nullptr) {
        return E_POINTER;
    }
    *ppEnum = new InspectEnumPins(&m_pin);
    return (*ppEnum != nullptr) ? S_OK : E_OUTOFMEMORY;
}
STDMETHODIMP InspectRenderer::FindPin(LPCWSTR Id, IPin **ppPin) {
    if (ppPin == nullptr) {
        return E_POINTER;
    }
    if (Id != nullptr && wcscmp(Id, L"In") == 0) {
        *ppPin = &m_pin;
        m_pin.AddRef();
        return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}
STDMETHODIMP InspectRenderer::QueryFilterInfo(FILTER_INFO *pInfo) {
    if (pInfo == nullptr) {
        return E_POINTER;
    }
    lstrcpynW(pInfo->achName, m_name[0] ? m_name : L"Inspector", MAX_FILTER_NAME);
    pInfo->pGraph = m_graph;
    if (m_graph) {
        m_graph->AddRef();
    }
    return S_OK;
}
STDMETHODIMP InspectRenderer::JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR pName) {
    m_graph = pGraph; // weak reference: the graph owns the filter
    if (pName) {
        lstrcpynW(m_name, pName, MAX_FILTER_NAME);
    }
    return S_OK;
}
STDMETHODIMP InspectRenderer::QueryVendorInfo(LPWSTR *) {
    return E_NOTIMPL;
}

/// IAMFilterMiscFlags
STDMETHODIMP_(ULONG) InspectRenderer::GetMiscFlags() {
    return AM_FILTER_MISC_FLAGS_IS_RENDERER;
}
