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
/// AM_MEDIA_TYPE helpers (the SDK only ships these in the DirectShow base
/// classes, so minimal versions are provided here).
///
/// Shared by the hand-rolled InspectInputPin (which copies/frees the connection
/// media type) and by the main sweep in directshow.cpp.  Defined inline so both
/// translation units can include them without an ODR clash.
#pragma once

#include <dshow.h>
#include <windows.h>

inline void FreeMediaTypeContents(AM_MEDIA_TYPE *mt) {
    if (mt == nullptr) {
        return;
    }
    if (mt->cbFormat != 0 && mt->pbFormat != nullptr) {
        CoTaskMemFree(mt->pbFormat);
    }
    mt->cbFormat = 0;
    mt->pbFormat = nullptr;
    if (mt->pUnk != nullptr) {
        mt->pUnk->Release();
        mt->pUnk = nullptr;
    }
}

inline void DeleteMediaType(AM_MEDIA_TYPE *mt) {
    if (mt != nullptr) {
        FreeMediaTypeContents(mt);
        CoTaskMemFree(mt);
    }
}

inline HRESULT CopyMediaType(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src) {
    *dst = *src;
    if (src->cbFormat != 0 && src->pbFormat != nullptr) {
        dst->pbFormat = (BYTE *) CoTaskMemAlloc(src->cbFormat);
        if (dst->pbFormat == nullptr) {
            dst->cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    } else {
        dst->pbFormat = nullptr;
        dst->cbFormat = 0;
    }
    if (dst->pUnk != nullptr) {
        dst->pUnk->AddRef();
    }
    return S_OK;
}
