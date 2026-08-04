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
/// Pretty-printing helpers for DirectShow media types.  See format_names.h.

#include "format_names.h"

#include <cctype>
#include <iomanip>
#include <sstream>

static const wchar_t *SubtypeName(const GUID &sub) {
    struct {
        const GUID *guid;
        const wchar_t *name;
    } known[] = {
        {&MEDIASUBTYPE_RGB8, L"RGB8/Y8"}, {&MEDIASUBTYPE_RGB24, L"RGB24"}, {&MEDIASUBTYPE_RGB32, L"RGB32"},
        {&MEDIASUBTYPE_UYVY, L"UYVY"},    {&MEDIASUBTYPE_YUY2, L"YUY2"},
    };
    for (auto &k : known) {
        if (IsEqualGUID(sub, *k.guid)) {
            return k.name;
        }
    }
    return nullptr;
}

std::wstring FormatSubtype(const GUID &sub) {
    const wchar_t *name = SubtypeName(sub);
    if (name != nullptr) {
        return name;
    }
    // Many camera subtypes are FOURCC-derived (first DWORD is the FOURCC).
    DWORD fourcc = sub.Data1;
    char c0 = (char) (fourcc & 0xFF), c1 = (char) ((fourcc >> 8) & 0xFF);
    char c2 = (char) ((fourcc >> 16) & 0xFF), c3 = (char) ((fourcc >> 24) & 0xFF);
    std::wostringstream out;
    if (isprint((unsigned char) c0) && isprint((unsigned char) c1) && isprint((unsigned char) c2) &&
        isprint((unsigned char) c3)) {
        out << L'\'' << c0 << c1 << c2 << c3 << L'\'';
    } else {
        out << L"{" << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0') << fourcc << L"}";
    }
    return out.str();
}

const wchar_t *FormatMajorName(const GUID &major) {
    if (IsEqualGUID(major, MEDIATYPE_Video)) {
        return L"Video";
    }
    if (IsEqualGUID(major, MEDIATYPE_Stream)) {
        return L"Stream";
    }
    return L"(other)";
}

double FpsFromInterval(LONGLONG interval100ns) {
    if (interval100ns <= 0) {
        return 0.0;
    }
    return 10000000.0 / (double) interval100ns;
}
