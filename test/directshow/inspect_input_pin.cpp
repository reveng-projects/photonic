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
/// Implementation of InspectInputPin (see inspect_input_pin.h).  Needs the full
/// definitions of InspectRenderer (refcount/owner delegation) and FrameStats
/// (sample forwarding), plus the shared AM_MEDIA_TYPE helpers.

#include "inspect_input_pin.h"

#include "framestats.h"
#include "inspect_renderer.h"
#include "mediatype.h"

InspectInputPin::InspectInputPin(InspectRenderer *owner, FrameStats *stats)
    : m_owner(owner), m_stats(stats), m_connected(nullptr) {
    ZeroMemory(&m_mt, sizeof(m_mt));
}

InspectInputPin::~InspectInputPin() {
    if (m_connected) {
        m_connected->Release();
    }
    FreeMediaTypeContents(&m_mt);
}

/// IUnknown — lifetime delegated to the owning filter.
STDMETHODIMP InspectInputPin::QueryInterface(REFIID riid, void **ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPin)) {
        *ppv = static_cast<IPin *>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IMemInputPin)) {
        *ppv = static_cast<IMemInputPin *>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) InspectInputPin::AddRef() {
    return m_owner->AddRef();
}
STDMETHODIMP_(ULONG) InspectInputPin::Release() {
    return m_owner->Release();
}

/// IPin
STDMETHODIMP InspectInputPin::Connect(IPin *, const AM_MEDIA_TYPE *) {
    return E_UNEXPECTED;
}
STDMETHODIMP InspectInputPin::ReceiveConnection(IPin *connector, const AM_MEDIA_TYPE *pmt) {
    if (connector == nullptr || pmt == nullptr) {
        return E_POINTER;
    }
    if (!IsEqualGUID(pmt->majortype, MEDIATYPE_Video)) {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    FreeMediaTypeContents(&m_mt);
    CopyMediaType(&m_mt, pmt);
    if (m_connected) {
        m_connected->Release();
    }
    m_connected = connector;
    m_connected->AddRef();
    return S_OK;
}
STDMETHODIMP InspectInputPin::Disconnect() {
    if (m_connected) {
        m_connected->Release();
        m_connected = nullptr;
    }
    return S_OK;
}
STDMETHODIMP InspectInputPin::ConnectedTo(IPin **pPin) {
    if (pPin == nullptr) {
        return E_POINTER;
    }
    if (m_connected == nullptr) {
        *pPin = nullptr;
        return VFW_E_NOT_CONNECTED;
    }
    *pPin = m_connected;
    m_connected->AddRef();
    return S_OK;
}
STDMETHODIMP InspectInputPin::ConnectionMediaType(AM_MEDIA_TYPE *pmt) {
    if (pmt == nullptr) {
        return E_POINTER;
    }
    if (m_connected == nullptr) {
        ZeroMemory(pmt, sizeof(*pmt));
        return VFW_E_NOT_CONNECTED;
    }
    return CopyMediaType(pmt, &m_mt);
}
STDMETHODIMP InspectInputPin::QueryPinInfo(PIN_INFO *pInfo) {
    if (pInfo == nullptr) {
        return E_POINTER;
    }
    pInfo->pFilter = static_cast<IBaseFilter *>(m_owner);
    m_owner->AddRef();
    pInfo->dir = PINDIR_INPUT;
    lstrcpynW(pInfo->achName, L"In", MAX_PIN_NAME);
    return S_OK;
}
STDMETHODIMP InspectInputPin::QueryDirection(PIN_DIRECTION *pPinDir) {
    if (pPinDir == nullptr) {
        return E_POINTER;
    }
    *pPinDir = PINDIR_INPUT;
    return S_OK;
}
STDMETHODIMP InspectInputPin::QueryId(LPWSTR *Id) {
    if (Id == nullptr) {
        return E_POINTER;
    }
    *Id = (LPWSTR) CoTaskMemAlloc(sizeof(L"In"));
    if (*Id == nullptr) {
        return E_OUTOFMEMORY;
    }
    memcpy(*Id, L"In", sizeof(L"In"));
    return S_OK;
}
STDMETHODIMP InspectInputPin::QueryAccept(const AM_MEDIA_TYPE *pmt) {
    return (pmt != nullptr && IsEqualGUID(pmt->majortype, MEDIATYPE_Video)) ? S_OK : S_FALSE;
}
STDMETHODIMP InspectInputPin::EnumMediaTypes(IEnumMediaTypes **) {
    return E_NOTIMPL;
}
STDMETHODIMP InspectInputPin::QueryInternalConnections(IPin **, ULONG *) {
    return E_NOTIMPL;
}
STDMETHODIMP InspectInputPin::EndOfStream() {
    return S_OK;
}
STDMETHODIMP InspectInputPin::BeginFlush() {
    return S_OK;
}
STDMETHODIMP InspectInputPin::EndFlush() {
    return S_OK;
}
STDMETHODIMP InspectInputPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) {
    return S_OK;
}

/// IMemInputPin
STDMETHODIMP InspectInputPin::GetAllocator(IMemAllocator **) {
    return VFW_E_NO_ALLOCATOR;
}
STDMETHODIMP InspectInputPin::NotifyAllocator(IMemAllocator *, BOOL) {
    return S_OK;
}
STDMETHODIMP InspectInputPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES *) {
    return E_NOTIMPL;
}
STDMETHODIMP InspectInputPin::Receive(IMediaSample *pSample) {
    if (pSample != nullptr && m_stats != nullptr) {
        BYTE *buf = nullptr;
        if (SUCCEEDED(pSample->GetPointer(&buf)) && buf != nullptr) {
            m_stats->BufferCB(0.0, buf, pSample->GetActualDataLength());
        }
    }
    return S_OK;
}
STDMETHODIMP InspectInputPin::ReceiveMultiple(IMediaSample **pSamples, long n, long *nProcessed) {
    long i = 0;
    for (; i < n; i++) {
        Receive(pSamples[i]);
    }
    if (nProcessed) {
        *nProcessed = i;
    }
    return S_OK;
}
STDMETHODIMP InspectInputPin::ReceiveCanBlock() {
    return S_FALSE;
}
