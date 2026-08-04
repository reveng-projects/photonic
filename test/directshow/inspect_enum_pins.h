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
/// InspectEnumPins — the pin enumerator returned by InspectRenderer::EnumPins
/// (see inspect_renderer.h).
///
/// A hand-rolled IEnumPins (the DirectShow base classes that would normally
/// provide one are absent from the Windows 7.1A SDK) that walks the renderer's
/// single input pin.  Unlike the filter and its pin, this object is heap
/// allocated and self-owned, so it carries its own refcount and deletes itself
/// when released.
#pragma once

#include <dshow.h>

class InspectEnumPins : public IEnumPins {
public:
    InspectEnumPins(IPin *pin);
    ~InspectEnumPins();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    STDMETHODIMP Next(ULONG cPins, IPin **ppPins, ULONG *pcFetched);
    STDMETHODIMP Skip(ULONG cPins);
    STDMETHODIMP Reset();
    STDMETHODIMP Clone(IEnumPins **ppEnum);

private:
    LONG m_ref;
    IPin *m_pin;
    ULONG m_pos;
};
