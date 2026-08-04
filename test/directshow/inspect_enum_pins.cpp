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
/// Implementation of InspectEnumPins (see inspect_enum_pins.h).

#include "inspect_enum_pins.h"

InspectEnumPins::InspectEnumPins(IPin *pin) : m_ref(1), m_pin(pin), m_pos(0) {
    m_pin->AddRef();
}

InspectEnumPins::~InspectEnumPins() {
    m_pin->Release();
}

STDMETHODIMP InspectEnumPins::QueryInterface(REFIID riid, void **ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumPins)) {
        *ppv = static_cast<IEnumPins *>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) InspectEnumPins::AddRef() {
    return (ULONG) InterlockedIncrement(&m_ref);
}
STDMETHODIMP_(ULONG) InspectEnumPins::Release() {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        delete this;
    }
    return (ULONG) r;
}

STDMETHODIMP InspectEnumPins::Next(ULONG cPins, IPin **ppPins, ULONG *pcFetched) {
    ULONG n = 0;
    while (n < cPins && m_pos < 1) {
        ppPins[n] = m_pin;
        m_pin->AddRef();
        n++;
        m_pos++;
    }
    if (pcFetched) {
        *pcFetched = n;
    }
    return (n == cPins) ? S_OK : S_FALSE;
}
STDMETHODIMP InspectEnumPins::Skip(ULONG cPins) {
    m_pos += cPins;
    return (m_pos <= 1) ? S_OK : S_FALSE;
}
STDMETHODIMP InspectEnumPins::Reset() {
    m_pos = 0;
    return S_OK;
}
STDMETHODIMP InspectEnumPins::Clone(IEnumPins **ppEnum) {
    if (ppEnum == nullptr) {
        return E_POINTER;
    }
    InspectEnumPins *e = new InspectEnumPins(m_pin);
    e->m_pos = m_pos;
    *ppEnum = e;
    return S_OK;
}
