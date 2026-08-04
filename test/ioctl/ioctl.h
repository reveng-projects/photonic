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
/// User-mode wrappers for the photonic driver's PHOTONIC_IOCTL_* interface,
/// implemented in ioctl.cpp (see that file for the per-wrapper contracts).

#pragma once

#include <windows.h>
#include <winioctl.h> // CTL_CODE for photonic/ioctl.h, even under WIN32_LEAN_AND_MEAN

#include "../../photonic/ioctl.h"

/// Section 1: Identity / version / device naming
BOOL PhotonicIoctlSdkVersion1(HANDLE hCamera);
BOOL PhotonicIoctlSdkVersion2(HANDLE hCamera);
BOOL PhotonicIoctlGetSerialNumber(HANDLE hCamera, UINT32 index, UINT32 *value);
BOOL PhotonicIoctlSetCancelTimeout(HANDLE hCamera, UINT32 timeoutMs);
BOOL PhotonicIoctlGetVendorName(HANDLE hCamera, char *name, DWORD nameLen);
BOOL PhotonicIoctlGetModelName(HANDLE hCamera, char *name, DWORD nameLen);
BOOL PhotonicIoctlGetDcamVersion(HANDLE hCamera, UINT32 *unitSwVersion);
BOOL PhotonicIoctlGetNamesLength(HANDLE hCamera, UINT32 *vendorNameLen, UINT32 *modelNameLen);
BOOL PhotonicIoctlGetNames(HANDLE hCamera, char *names);
BOOL PhotonicIoctlGetLastError(HANDLE hCamera, UINT32 *lastError);

/// Section 2: Sub-window (ROI) / pixel format / channel
BOOL PhotonicIoctlSubwindowGet(HANDLE hCamera, PHOTONIC_SUBWINDOW *out);
BOOL PhotonicIoctlSubwindowSet(HANDLE hCamera, const PHOTONIC_SUBWINDOW *in);
BOOL PhotonicIoctlGetPixelFormat(HANDLE hCamera, UINT32 *pixelFormat);
BOOL PhotonicIoctlSetPixelFormat(HANDLE hCamera, UINT32 pixelFormat);
BOOL PhotonicIoctlImageFlip(HANDLE hCamera, UINT32 horizontal, UINT32 vertical);
BOOL PhotonicIoctlInvalidateFormat(HANDLE hCamera);
BOOL PhotonicIoctlGetChannel(HANDLE hCamera, UINT32 *channel);
BOOL PhotonicIoctlGetPacketSize(HANDLE hCamera, UINT32 *bytesPerPacket);

/// Section 3: Video streaming (continuous video)
BOOL PhotonicIoctlStartVideo(HANDLE hCamera, UINT32 flags);
BOOL PhotonicIoctlStopVideo(HANDLE hCamera);
BOOL PhotonicIoctlPrepareVideo(HANDLE hCamera, UINT32 frameCount);
BOOL PhotonicIoctlUnprepareVideo(HANDLE hCamera);
BOOL PhotonicIoctlMapVideoFrame(HANDLE hCamera, UINT32 frameCount, UINT32 frameSize, UINT32 baseVa);
BOOL PhotonicIoctlUnmapVideoFrame(HANDLE hCamera);
BOOL PhotonicIoctlGetTransferInfo(HANDLE hCamera, UINT32 *currentFrameIndex, UINT32 *totalFrameCount);
BOOL PhotonicIoctlRegisterEvent(HANDLE hCamera, HANDLE hEvent, UINT32 type);
BOOL PhotonicIoctlUnregisterEvent(HANDLE hCamera, HANDLE hEvent, UINT32 type);

/// Section 4: Imager (single-frame still capture)
BOOL PhotonicIoctlPrepareImager(HANDLE hCamera);
BOOL PhotonicIoctlUnprepareImager(HANDLE hCamera);
BOOL PhotonicIoctlStartImager(HANDLE hCamera, UINT32 mode);
BOOL PhotonicIoctlStopImager(HANDLE hCamera);

/// Section 5: Frame rate
BOOL PhotonicIoctlEnumFrameRate(HANDLE hCamera, UINT32 index, float *fps);
BOOL PhotonicIoctlSetFrameRate(HANDLE hCamera, UINT32 index);
BOOL PhotonicIoctlGetFrameRate(HANDLE hCamera, float *fps);

/// Section 6: Trigger / strobe
BOOL PhotonicIoctlTriggerSet(HANDLE hCamera, const PHOTONIC_TRIGGER_SET_IN *in);
BOOL PhotonicIoctlStrobeSet(HANDLE hCamera, const PHOTONIC_STROBE_SET_IN *in);
BOOL PhotonicIoctlSwTrigger(HANDLE hCamera);

/// Section 7: Initialize / property (feature) get-set
BOOL PhotonicIoctlInitialize(HANDLE hCamera);
BOOL PhotonicIoctlPropertyGet(HANDLE hCamera, UINT32 featureId, UINT32 *flags, UINT32 *value);
BOOL PhotonicIoctlPropertySet(HANDLE hCamera, UINT32 featureId, UINT32 flags, UINT32 value);

/// Section 8: mailbox / external I2C register access
BOOL PhotonicIoctlMailbox(HANDLE hCamera, void *in, DWORD inLen, void *out, DWORD outLen);
