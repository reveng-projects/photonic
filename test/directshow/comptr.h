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
/// Minimal COM smart pointer.
///
/// Microsoft::WRL::ComPtr (<wrl/client.h>) is deliberately avoided: the v140_xp
/// platform toolset builds against the Windows 7.1A SDK so that the binary runs
/// on Windows XP, and WRL only ships with the Windows 8+ SDK.  This self-
/// contained pointer supports just the subset the test suites use (Get / As /
/// ReleaseAndGetAddressOf and IID_PPV_ARGS via operator&).
#pragma once

template <class T> class ComPtr {
public:
    ComPtr() : m_p(nullptr) {
    }
    ComPtr(const ComPtr &o) : m_p(o.m_p) {
        if (m_p)
            m_p->AddRef();
    }
    ComPtr(ComPtr &&o) noexcept : m_p(o.m_p) {
        o.m_p = nullptr;
    }
    ~ComPtr() {
        if (m_p)
            m_p->Release();
    }

    ComPtr &operator=(const ComPtr &o) {
        if (this != &o) {
            if (o.m_p)
                o.m_p->AddRef();
            if (m_p)
                m_p->Release();
            m_p = o.m_p;
        }
        return *this;
    }
    ComPtr &operator=(ComPtr &&o) noexcept {
        if (this != &o) {
            if (m_p)
                m_p->Release();
            m_p = o.m_p;
            o.m_p = nullptr;
        }
        return *this;
    }

    T *Get() const {
        return m_p;
    }
    T *operator->() const {
        return m_p;
    }
    T **GetAddressOf() {
        return &m_p;
    }
    T **ReleaseAndGetAddressOf() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
        return &m_p;
    }
    /// Lets IID_PPV_ARGS(&ptr) work: releases first, then yields T**.
    T **operator&() {
        return ReleaseAndGetAddressOf();
    }

    template <class U> HRESULT As(U **pp) const {
        return m_p->QueryInterface(__uuidof(U), reinterpret_cast<void **>(pp));
    }

    explicit operator bool() const {
        return m_p != nullptr;
    }
    bool operator==(decltype(nullptr)) const {
        return m_p == nullptr;
    }
    bool operator!=(decltype(nullptr)) const {
        return m_p != nullptr;
    }

private:
    T *m_p;
};
