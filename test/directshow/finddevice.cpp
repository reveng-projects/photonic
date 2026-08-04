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
/// DirectShow capture-device selection helpers.  See finddevice.h.

#include "finddevice.h"

#include <cwchar>
#include <cwctype>

#include "../ioctl/i2c.h"
#include "../utils/log.h"

static bool ContainsNoCase(const std::wstring &hay, const std::wstring &needle) {
    if (needle.empty()) {
        return true;
    }
    std::wstring h = hay, n = needle;
    for (auto &c : h) {
        c = (wchar_t) towlower(c);
    }
    for (auto &c : n) {
        c = (wchar_t) towlower(c);
    }
    return h.find(n) != std::wstring::npos;
}

HRESULT DsFindCaptureMoniker(const std::wstring &nameSubstr, int deviceIndex, ComPtr<IMoniker> &match,
                             std::wstring &chosenName) {
    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IEnumMoniker> enumMoniker;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    if (hr != S_OK) {
        Log(ERROR, L"No video capture devices found (hr=0x%08lX).", hr);
        return E_FAIL;
    }

    Log(INFO, L"Video capture devices:");
    ComPtr<IMoniker> moniker;
    int index = 0;
    int matchOrdinal = 0; // 0-based position among devices matching the name
    while (enumMoniker->Next(1, moniker.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        ComPtr<IPropertyBag> bag;
        if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
            continue;
        }
        VARIANT v;
        VariantInit(&v);
        std::wstring name = L"(unknown)";
        if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) {
            name = v.bstrVal;
        }
        VariantClear(&v);

        // Without --device-index the first matching device is used; with
        // --device-index N the N-th (0-based) device matching the name is used,
        // so cameras that share a friendly name can still be told apart.
        bool isMatch = ContainsNoCase(name, nameSubstr);
        bool selected = false;
        std::wstring tag;
        if (isMatch) {
            bool wanted = (deviceIndex < 0) ? (match == nullptr) : (matchOrdinal == deviceIndex);
            if (wanted && match == nullptr) {
                match = moniker;
                chosenName = name;
                selected = true;
            }
            wchar_t buf[64];
            swprintf(buf, _countof(buf), L"  (match #%d)%s", matchOrdinal, selected ? L"  <= selected" : L"");
            tag = buf;
            matchOrdinal++;
        }
        Log(INFO, L"[%d] %s%s", index, name.c_str(), tag.c_str());
        index++;
    }

    if (match == nullptr) {
        if (deviceIndex >= 0 && matchOrdinal > 0) {
            Log(ERROR, L"--device-index %d is out of range: only %d device(s) match \"%s\".", deviceIndex, matchOrdinal,
                nameSubstr.c_str());
        } else {
            Log(ERROR, L"No device name contains \"%s\".", nameSubstr.c_str());
        }
        return E_FAIL;
    }
    return S_OK;
}

HRESULT DsGetDevicePath(IMoniker *moniker, std::wstring &devicePath) {
    ComPtr<IPropertyBag> bag;
    HRESULT hr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag));
    if (FAILED(hr)) {
        Log(ERROR, L"BindToStorage for DevicePath failed (hr=0x%08lX).", hr);
        return hr;
    }
    VARIANT v;
    VariantInit(&v);
    hr = bag->Read(L"DevicePath", &v, nullptr);
    if (FAILED(hr)) {
        Log(ERROR, L"Read(DevicePath) failed (hr=0x%08lX).", hr);
        return hr;
    }
    if (v.vt != VT_BSTR) {
        Log(ERROR, L"DevicePath has unexpected type (vt=%d).", v.vt);
        VariantClear(&v);
        return E_FAIL;
    }
    devicePath = v.bstrVal;
    VariantClear(&v);
    return S_OK;
}

HANDLE DsOpenDevicePath(const std::wstring &devicePath) {
    return CreateFileW(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

/// Diagnose whether ksproxy can construct a KS filter on this device: query a
/// set of pin-0 properties (KSPROPSETID_Pin via IOCTL_KS_PROPERTY) over the
/// raw device handle.  A failure here means stream.sys exposed no usable KS
/// filter/pin, so ksproxy has nothing to construct -> E_NOTIMPL.
static void ProbeKsPinProperties(HANDLE h) {
    const GUID KSPROPSETID_Pin_local = {0x8C134960, 0x51AD, 0x11CF, {0x87, 0x8A, 0x94, 0xF8, 0x01, 0xC1, 0x00, 0x00}};
    const ULONG KSPROPERTY_TYPE_GET_local = 0x00000001;
    const DWORD IOCTL_KS_PROPERTY_local = 0x002F0003;
    struct KSPROPERTY_L {
        GUID Set;
        ULONG Id;
        ULONG Flags;
    };
    struct KSP_PIN_L {
        KSPROPERTY_L P;
        ULONG PinId;
        ULONG Reserved;
    };

    KSP_PIN_L kspPin = {};
    kspPin.P.Set = KSPROPSETID_Pin_local;
    kspPin.P.Id = 0; // overwritten per probe below
    kspPin.P.Flags = KSPROPERTY_TYPE_GET_local;

    // The properties ksproxy queries for pin 0 while building the
    // pin (KSPROPERTY_PIN enum, 0-based). Each uses KSP_PIN with
    // PinId = 0. For the multiple-item properties
    // (DATARANGES/INTERFACES/MEDIUMS) the output starts with
    // KSMULTIPLE_ITEM { ULONG Size; ULONG Count; }, logged as
    // u0=Size, u1=Count. For DATAFLOW/COMMUNICATION the output is
    // a single ULONG (logged as u0): DATAFLOW OUT=2;
    // COMMUNICATION NONE=0/SINK=1/SOURCE=2/BOTH=3/BRIDGE=4.
    BYTE outBuf[512];
    struct PinProbe {
        ULONG id;
        const wchar_t *name;
    };
    const PinProbe probes[] = {
        {2, L"PIN_DATAFLOW"}, {3, L"PIN_DATARANGES"},    {5, L"PIN_INTERFACES"},
        {6, L"PIN_MEDIUMS"},  {7, L"PIN_COMMUNICATION"},
    };
    for (const auto &pr : probes) {
        kspPin.P.Id = pr.id;
        kspPin.PinId = 0;
        ZeroMemory(outBuf, sizeof(outBuf));
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_KS_PROPERTY_local, &kspPin, sizeof(KSP_PIN_L), outBuf, sizeof(outBuf),
                                  &bytes, nullptr);
        if (ok) {
            Log(INFO, L"%s(id=%lu): u0=%lu u1=%lu (bytes=%lu).", pr.name, pr.id, ((ULONG *) outBuf)[0],
                ((ULONG *) outBuf)[1], bytes);
            // For INTERFACES (5) / MEDIUMS (6) the data after the
            // KSMULTIPLE_ITEM header (8 bytes) is KSIDENTIFIER
            // { GUID Set; ULONG Id; ULONG Flags; }. Dump the GUID +
            // Id to show whether it is the Standard set (which
            // ksproxy has a built-in handler for).
            if ((pr.id == 5 || pr.id == 6) && bytes >= 8 + 24) {
                const GUID *g = (const GUID *) (outBuf + 8);
                ULONG idv = *(const ULONG *) (outBuf + 8 + 16);
                Log(INFO, L"   %s set={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} id=%lu", pr.name, g->Data1,
                    g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
                    g->Data4[6], g->Data4[7], idv);
            }
        } else {
            Log(ERROR, L"%s(id=%lu) failed (err=%lu).", pr.name, pr.id, GetLastError());
        }
    }
}

HRESULT FindCaptureDevice(const std::wstring &nameSubstr, int deviceIndex, IBaseFilter **ppFilter,
                          std::wstring &chosenName) {
    *ppFilter = nullptr;

    ComPtr<IMoniker> match;
    std::wstring matchName;
    HRESULT hr = DsFindCaptureMoniker(nameSubstr, deviceIndex, match, matchName);
    if (FAILED(hr)) {
        return hr;
    }

    // --- Diagnostic: is the device actually openable from user mode? ---------
    // When BindToObject fails with E_NOTIMPL and no IRP reaches the driver, the
    // question is whether ksproxy could even open the device. Read the moniker's
    // DevicePath and try a raw CreateFile on it: success isolates the failure to
    // ksproxy / KS filter creation; failure points at the kernel device itself.
    {
        std::wstring devicePath;
        if (SUCCEEDED(DsGetDevicePath(match.Get(), devicePath))) {
            Log(INFO, L"DevicePath: %s", devicePath.c_str());
            HANDLE h = DsOpenDevicePath(devicePath);
            if (h == INVALID_HANDLE_VALUE) {
                Log(ERROR, L"raw CreateFile on device failed (err=%lu).", GetLastError());
            } else {
                Log(INFO, L"raw CreateFile on device succeeded.");

                // Snapshot the camera-head controller registers over this
                // handle before any DirectShow test touches the device.
                CamDumpRegisters(h);

                ProbeKsPinProperties(h);
                CloseHandle(h);
            }
        }
    }

    hr = match->BindToObject(nullptr, nullptr, IID_PPV_ARGS(ppFilter));
    if (FAILED(hr)) {
        Log(ERROR, L"BindToObject failed (hr=0x%08lX).", hr);
        return hr;
    }
    chosenName = matchName;
    return S_OK;
}
