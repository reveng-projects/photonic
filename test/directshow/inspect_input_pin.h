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
/// InspectInputPin — the single input pin of InspectRenderer (see
/// inspect_renderer.h).
///
/// A hand-rolled IPin / IMemInputPin that accepts any MEDIATYPE_Video connection
/// and forwards every received sample's bytes to the owning filter's FrameStats.
/// The DirectShow base classes (CBasePin) are absent from the Windows 7.1A SDK
/// that the v140_xp toolset builds against, so the pin is implemented by hand.
///
/// The pin is embedded in the filter (InspectRenderer::m_pin), so it has no
/// refcount of its own: AddRef/Release are delegated to the owning filter.
#pragma once

#include <dshow.h>

class InspectRenderer;
class FrameStats;

class InspectInputPin : public IPin, public IMemInputPin {
public:
    InspectInputPin(InspectRenderer *owner, FrameStats *stats);
    ~InspectInputPin();

    /// IUnknown — lifetime delegated to the owning filter.
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    /// IPin
    STDMETHODIMP Connect(IPin *, const AM_MEDIA_TYPE *);
    STDMETHODIMP ReceiveConnection(IPin *connector, const AM_MEDIA_TYPE *pmt);
    STDMETHODIMP Disconnect();
    STDMETHODIMP ConnectedTo(IPin **pPin);
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *pmt);
    STDMETHODIMP QueryPinInfo(PIN_INFO *pInfo);
    STDMETHODIMP QueryDirection(PIN_DIRECTION *pPinDir);
    STDMETHODIMP QueryId(LPWSTR *Id);
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *pmt);
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **);
    STDMETHODIMP QueryInternalConnections(IPin **, ULONG *);
    STDMETHODIMP EndOfStream();
    STDMETHODIMP BeginFlush();
    STDMETHODIMP EndFlush();
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double);

    /// IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator **);
    STDMETHODIMP NotifyAllocator(IMemAllocator *, BOOL);
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES *);
    STDMETHODIMP Receive(IMediaSample *pSample);
    STDMETHODIMP ReceiveMultiple(IMediaSample **pSamples, long n, long *nProcessed);
    STDMETHODIMP ReceiveCanBlock();

private:
    InspectRenderer *m_owner;
    FrameStats *m_stats;
    IPin *m_connected;
    AM_MEDIA_TYPE m_mt;
};
