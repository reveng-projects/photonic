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
/// Filter-graph plumbing helpers for the DirectShow sweep.  See graph_utils.h.

#include "graph_utils.h"

#include <string>
#include <vector>

#include "../utils/log.h"

#include "comptr.h"
#include "format_names.h"
#include "mediatype.h"

void TeardownRenderChain(IGraphBuilder *graph, IBaseFilter *captureFilter) {
    ComPtr<IEnumFilters> enumFilters;
    if (FAILED(graph->EnumFilters(&enumFilters))) {
        return;
    }
    std::vector<ComPtr<IBaseFilter>> toRemove;
    ComPtr<IBaseFilter> filter;
    while (enumFilters->Next(1, filter.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        if (filter.Get() != captureFilter) {
            toRemove.push_back(filter);
        }
    }
    for (auto &f : toRemove) {
        graph->RemoveFilter(f.Get());
    }
}

HRESULT RenderStreamAny(ICaptureGraphBuilder2 *capBuilder, IBaseFilter *captureFilter, IBaseFilter *intermediate,
                        IBaseFilter *sink) {
    HRESULT hr = capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, captureFilter, intermediate, sink);
    Log(INFO, L"RenderStream[PIN_CATEGORY_CAPTURE] -> 0x%08lX", hr);
    if (FAILED(hr)) {
        hr = capBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, captureFilter, intermediate, sink);
        Log(INFO, L"RenderStream[PIN_CATEGORY_PREVIEW] -> 0x%08lX", hr);
    }
    if (FAILED(hr)) {
        hr = capBuilder->RenderStream(nullptr, &MEDIATYPE_Video, captureFilter, intermediate, sink);
        Log(INFO, L"RenderStream[any pin] -> 0x%08lX", hr);
    }
    return hr;
}

bool TapConnectionIsNative(IBaseFilter *tap, const AM_MEDIA_TYPE *pmt) {
    ComPtr<IEnumPins> pins;
    if (tap == nullptr || FAILED(tap->EnumPins(&pins))) {
        return false;
    }
    ComPtr<IPin> input;
    ComPtr<IPin> pin;
    while (pins->Next(1, pin.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        PIN_DIRECTION dir;
        if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == PINDIR_INPUT) {
            input = pin;
            break;
        }
    }
    if (input == nullptr) {
        Log(WARN, L"tap filter has no input pin");
        return false;
    }
    AM_MEDIA_TYPE connected;
    ZeroMemory(&connected, sizeof(connected));
    HRESULT hr = input->ConnectionMediaType(&connected);
    if (FAILED(hr)) {
        Log(WARN, L"tap input pin has no connection media type (0x%08lX)", hr);
        return false;
    }
    bool native = IsEqualGUID(connected.subtype, pmt->subtype) != FALSE;
    if (native && IsEqualGUID(pmt->formattype, FORMAT_VideoInfo) && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        native = IsEqualGUID(connected.formattype, FORMAT_VideoInfo) && connected.cbFormat >= sizeof(VIDEOINFOHEADER) &&
                 connected.pbFormat != nullptr;
        if (native) {
            const VIDEOINFOHEADER *conVih = (const VIDEOINFOHEADER *) connected.pbFormat;
            const VIDEOINFOHEADER *setVih = (const VIDEOINFOHEADER *) pmt->pbFormat;
            native = conVih->bmiHeader.biWidth == setVih->bmiHeader.biWidth &&
                     conVih->bmiHeader.biHeight == setVih->bmiHeader.biHeight &&
                     conVih->bmiHeader.biBitCount == setVih->bmiHeader.biBitCount;
        }
    }
    if (!native) {
        Log(WARN, L"tap connected with %s, not the native %s: converter inserted upstream",
            FormatSubtype(connected.subtype).c_str(), FormatSubtype(pmt->subtype).c_str());
    }
    FreeMediaTypeContents(&connected);
    return native;
}

void LogGraphFilters(IGraphBuilder *graph) {
    ComPtr<IEnumFilters> enumFilters;
    if (FAILED(graph->EnumFilters(&enumFilters))) {
        return;
    }
    std::wstring line = L"graph:";
    ComPtr<IBaseFilter> filter;
    bool any = false;
    while (enumFilters->Next(1, filter.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        FILTER_INFO fi;
        ZeroMemory(&fi, sizeof(fi));
        if (SUCCEEDED(filter->QueryFilterInfo(&fi))) {
            line += L" [";
            line += fi.achName;
            line += L"]";
            if (fi.pGraph != nullptr) {
                fi.pGraph->Release();
            }
            any = true;
        }
    }
    if (!any) {
        line += L" (none)";
    }
    Log(INFO, L"%s", line.c_str());
}

void ConfigureVideoWindow(IGraphBuilder *graph, const wchar_t *caption, LONG width, LONG height) {
    ComPtr<IVideoWindow> videoWindow;
    if (FAILED(graph->QueryInterface(IID_PPV_ARGS(&videoWindow)))) {
        return;
    }
    LONG w = width > 0 ? width : 320;
    LONG h = height > 0 ? height : 240;
    if (w > 1024) {
        w = 1024;
    }
    if (h > 768) {
        h = 768;
    }

    videoWindow->put_Caption((BSTR) caption);
    videoWindow->put_WindowStyle(WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    videoWindow->SetWindowPosition(60, 60, w, h);
    videoWindow->put_Visible(OATRUE);
}

void LogVideoWindowState(IGraphBuilder *graph) {
    ComPtr<IVideoWindow> videoWindow;
    if (FAILED(graph->QueryInterface(IID_PPV_ARGS(&videoWindow)))) {
        Log(INFO, L"video window: none (no IVideoWindow on the graph)");
        return;
    }
    long visible = 0, left = 0, top = 0, width = 0, height = 0;
    videoWindow->get_Visible(&visible);
    videoWindow->GetWindowPosition(&left, &top, &width, &height);
    Log(INFO, L"video window: present visible=%ld pos=%ld,%ld size=%ldx%ld", visible, left, top, width, height);
}
