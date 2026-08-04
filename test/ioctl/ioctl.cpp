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
/// User-mode wrappers for the photonic driver's PHOTONIC_IOCTL_* interface.
///
/// Each wrapper issues a single DeviceIoControl against an already-opened
/// camera handle (CreateFileA("\\\\.\\vitdcamN", ...)).  The wrappers hide the
/// low-level METHOD_BUFFERED IOCTL structs: callers pass plain scalars (or, for
/// genuinely multi-field payloads such as the sub-window/trigger/strobe blocks,
/// the corresponding struct) and never have to deal with the shared in/out
/// buffer duality.
///
/// Return value:
///   - Value getters return FALSE if the IOCTL failed OR the driver reported a
///     non-zero application status (the out value is then meaningless); on
///     FALSE, call PhotonicIoctlGetLastError() for the driver's error code.
///   - Setters / actions return the raw DeviceIoControl BOOL (TRUE on success).

#include "ioctl.h"

/// Generic METHOD_BUFFERED transport. Returns the DeviceIoControl BOOL.
/// `in` and `out` may be distinct objects: the kernel copies in -> system
/// buffer, the driver overwrites it, then it is copied out -> `out`.
static BOOL PhotonicIoctl(HANDLE hCamera, DWORD code, const void *in, DWORD inLen, void *out, DWORD outLen) {
    DWORD returned = 0;
    return DeviceIoControl(hCamera, code, (LPVOID) in, inLen, out, outLen, &returned, NULL);
}

/// Section 1: Identity / version / device naming

/// PHOTONIC_IOCTL_SDK_VERSION_1 (0x222018) — stub, returns STATUS_NOT_SUPPORTED.
BOOL PhotonicIoctlSdkVersion1(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SDK_VERSION_1, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_SDK_VERSION_2 (0x22201C) — stub, returns STATUS_NOT_SUPPORTED.
BOOL PhotonicIoctlSdkVersion2(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SDK_VERSION_2, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_SERIAL_NUMBER (0x222024) — byte-swapped serial-number word.
/// index: 0 = low word, 1 = high word.
BOOL PhotonicIoctlGetSerialNumber(HANDLE hCamera, UINT32 index, UINT32 *value) {
    PHOTONIC_SERIAL_NUMBER_IN in = {};
    PHOTONIC_SERIAL_NUMBER_OUT out;
    in.Index = index;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SERIAL_NUMBER, &in, sizeof(in), &out, sizeof(out))) {
        return FALSE;
    }
    if (out.Status != 0) {
        return FALSE;
    }
    if (value) {
        *value = out.Value;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_SET_CANCEL_TIMEOUT (0x22202C) — set cancel/write timeout (ms).
BOOL PhotonicIoctlSetCancelTimeout(HANDLE hCamera, UINT32 timeoutMs) {
    PHOTONIC_SET_CANCEL_TIMEOUT_IN in = {};
    in.TimeoutMs = timeoutMs;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SET_CANCEL_TIMEOUT, &in, sizeof(in), NULL, 0);
}

/// PHOTONIC_IOCTL_GET_VENDOR_NAME (0x222030) — variable-length ANSI string.
/// The driver does not NUL-terminate; size `name` from
/// PhotonicIoctlGetNamesLength.
BOOL PhotonicIoctlGetVendorName(HANDLE hCamera, char *name, DWORD nameLen) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_VENDOR_NAME, NULL, 0, name, nameLen);
}

/// PHOTONIC_IOCTL_GET_MODEL_NAME (0x222034) — variable-length ANSI string.
BOOL PhotonicIoctlGetModelName(HANDLE hCamera, char *name, DWORD nameLen) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_MODEL_NAME, NULL, 0, name, nameLen);
}

/// PHOTONIC_IOCTL_GET_DCAM_VERSION (0x222170) — DCAM unit_sw_version.
BOOL PhotonicIoctlGetDcamVersion(HANDLE hCamera, UINT32 *unitSwVersion) {
    PHOTONIC_GET_DCAM_VERSION_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_DCAM_VERSION, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (unitSwVersion) {
        *unitSwVersion = out.UnitSwVersion;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_GET_NAMES_LENGTH (0x222174) — vendor/model string lengths.
BOOL PhotonicIoctlGetNamesLength(HANDLE hCamera, UINT32 *vendorNameLen, UINT32 *modelNameLen) {
    PHOTONIC_GET_NAMES_LENGTH_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_NAMES_LENGTH, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (vendorNameLen) {
        *vendorNameLen = out.VendorNameLen;
    }
    if (modelNameLen) {
        *modelNameLen = out.ModelNameLen;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_GET_NAMES (0x2223CC) — newline-delimited device path list.
/// Issued to the "\\.\vitdcam" control device (not the per-camera dispatch).
/// `names` is a single fixed-size (PHOTONIC_GET_NAMES_BUF_SIZE) buffer, used as
/// both input (sent as zeros) and output; the wrapper zeroes it first.
BOOL PhotonicIoctlGetNames(HANDLE hCamera, char *names) {
    ZeroMemory(names, PHOTONIC_GET_NAMES_BUF_SIZE);
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_NAMES, names, PHOTONIC_GET_NAMES_BUF_SIZE, names,
                         PHOTONIC_GET_NAMES_BUF_SIZE);
}

/// PHOTONIC_IOCTL_GET_LAST_ERROR (0x2223F8) — driver's last internal error code.
BOOL PhotonicIoctlGetLastError(HANDLE hCamera, UINT32 *lastError) {
    PHOTONIC_GET_LAST_ERROR_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_LAST_ERROR, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (lastError) {
        *lastError = out.LastError;
    }
    return TRUE;
}

/// Section 2: Sub-window (ROI) / pixel format / channel

/// PHOTONIC_IOCTL_SUBWINDOW_GET (0x2220D4) — read current ROI (input ignored).
BOOL PhotonicIoctlSubwindowGet(HANDLE hCamera, PHOTONIC_SUBWINDOW *out) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SUBWINDOW_GET, NULL, 0, out, sizeof(*out));
}

/// PHOTONIC_IOCTL_SUBWINDOW_SET (0x2220D8) — program ROI.
/// Caller fills Scale/Width/OffsetX/OffsetY/Height; the Status field is ignored
/// on input.
BOOL PhotonicIoctlSubwindowSet(HANDLE hCamera, const PHOTONIC_SUBWINDOW *in) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SUBWINDOW_SET, in, sizeof(*in), NULL, 0);
}

/// PHOTONIC_IOCTL_GET_PIXEL_FORMAT (0x222158) — current DCAM pixel-format code.
BOOL PhotonicIoctlGetPixelFormat(HANDLE hCamera, UINT32 *pixelFormat) {
    PHOTONIC_GET_PIXEL_FORMAT_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_PIXEL_FORMAT, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (pixelFormat) {
        *pixelFormat = out.PixelFormat;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_SET_PIXEL_FORMAT (0x22215C) — select pixel format (keeps ROI).
BOOL PhotonicIoctlSetPixelFormat(HANDLE hCamera, UINT32 pixelFormat) {
    PHOTONIC_SET_PIXEL_FORMAT_IN in;
    in.PixelFormat = pixelFormat;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SET_PIXEL_FORMAT, &in, sizeof(in), NULL, 0);
}

/// PHOTONIC_IOCTL_IMAGE_FLIP (0x222134) — horizontal/vertical flip (ext cmd 0xD).
BOOL PhotonicIoctlImageFlip(HANDLE hCamera, UINT32 horizontal, UINT32 vertical) {
    PHOTONIC_IMAGE_FLIP_IN in;
    in.FlipHorizontal = horizontal;
    in.FlipVertical = vertical;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_IMAGE_FLIP, &in, sizeof(in), NULL, 0);
}

/// PHOTONIC_IOCTL_INVALIDATE_FORMAT (0x222178) — clear cached current format.
BOOL PhotonicIoctlInvalidateFormat(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_INVALIDATE_FORMAT, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_GET_CHANNEL (0x222140) — isochronous channel number.
BOOL PhotonicIoctlGetChannel(HANDLE hCamera, UINT32 *channel) {
    PHOTONIC_GET_CHANNEL_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_CHANNEL, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (channel) {
        *channel = out.IsochChannel;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_GET_PACKET_SIZE (0x222150) — isochronous bytes per packet.
BOOL PhotonicIoctlGetPacketSize(HANDLE hCamera, UINT32 *bytesPerPacket) {
    PHOTONIC_GET_PACKET_SIZE_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_PACKET_SIZE, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (bytesPerPacket) {
        *bytesPerPacket = out.IsochBytesPerPacket;
    }
    return TRUE;
}

/// Section 3: Video streaming (continuous video)

/// PHOTONIC_IOCTL_START_VIDEO (0x2220DC) — start isochronous video stream.
/// flags == 0 -> normal start; non-zero -> stop-mode flags.
BOOL PhotonicIoctlStartVideo(HANDLE hCamera, UINT32 flags) {
    PHOTONIC_START_VIDEO_IN buf;
    buf.Flags = flags;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_START_VIDEO, &buf, sizeof(buf), &buf, sizeof(buf));
}

/// PHOTONIC_IOCTL_STOP_VIDEO (0x2220E0) — stop video stream.
BOOL PhotonicIoctlStopVideo(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_STOP_VIDEO, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_PREPARE_VIDEO (0x2220F0) — allocate `frameCount` DMA ring
/// frames.
BOOL PhotonicIoctlPrepareVideo(HANDLE hCamera, UINT32 frameCount) {
    PHOTONIC_PREPARE_VIDEO_IN buf = {};
    buf.FrameCount = frameCount;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_PREPARE_VIDEO, &buf, sizeof(buf), &buf, sizeof(buf));
}

/// PHOTONIC_IOCTL_UNPREPARE_VIDEO (0x2220F4) — release DMA ring resources.
BOOL PhotonicIoctlUnprepareVideo(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_UNPREPARE_VIDEO, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_MAP_VIDEO_FRAME (0x2220F8) — map user frame buffers into the
/// ring. baseVa is the 32-bit user-mode base VA of the frame array (WOW64 ABI);
/// frameSize must match the size computed by PrepareVideo.
BOOL PhotonicIoctlMapVideoFrame(HANDLE hCamera, UINT32 frameCount, UINT32 frameSize, UINT32 baseVa) {
    PHOTONIC_MAP_VIDEO_FRAME_IN32 buf;
    buf.FrameCount = frameCount;
    buf.FrameSize = frameSize;
    buf.BaseVA = baseVa;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_MAP_VIDEO_FRAME, &buf, sizeof(buf), &buf, sizeof(buf));
}

/// PHOTONIC_IOCTL_UNMAP_VIDEO_FRAME (0x2220FC) — unmap all mapped frames.
BOOL PhotonicIoctlUnmapVideoFrame(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_UNMAP_VIDEO_FRAME, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_GET_TRANSFER_INFO (0x2220EC) — last completed frame + totals.
BOOL PhotonicIoctlGetTransferInfo(HANDLE hCamera, UINT32 *currentFrameIndex, UINT32 *totalFrameCount) {
    PHOTONIC_GET_TRANSFER_INFO_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_TRANSFER_INFO, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (out.Status != 0) {
        return FALSE;
    }
    if (currentFrameIndex) {
        *currentFrameIndex = out.CurrentFrameIndex;
    }
    if (totalFrameCount) {
        *totalFrameCount = out.TotalFrameCount;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_REGISTER_EVENT (0x2220E4) — register a frame-completion event.
/// type: 0 = one-shot (all frames delivered), 1 = per-frame.
BOOL PhotonicIoctlRegisterEvent(HANDLE hCamera, HANDLE hEvent, UINT32 type) {
    PHOTONIC_REGISTER_EVENT_IN32 in = {};
    in.Handle = (UINT32) (ULONG_PTR) hEvent;
    in.Type = type;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_REGISTER_EVENT, &in, sizeof(in), NULL, 0);
}

/// PHOTONIC_IOCTL_UNREGISTER_EVENT (0x2220E8) — unregister a previously-registered
/// event.
BOOL PhotonicIoctlUnregisterEvent(HANDLE hCamera, HANDLE hEvent, UINT32 type) {
    PHOTONIC_UNREGISTER_EVENT_IN32 in = {};
    in.Handle = (UINT32) (ULONG_PTR) hEvent;
    in.Type = type;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_UNREGISTER_EVENT, &in, sizeof(in), NULL, 0);
}

/// Section 4: Imager (single-frame still capture)

/// PHOTONIC_IOCTL_PREPARE_IMAGER (0x222160) — allocate imager resources.
/// The driver ignores the input contents but the shared buffer must be large
/// enough for the status reply, so the wrapper supplies a zeroed dummy.
BOOL PhotonicIoctlPrepareImager(HANDLE hCamera) {
    PHOTONIC_PREPARE_IMAGER_IN in = {};
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_PREPARE_IMAGER, &in, sizeof(in), NULL, 0);
}

/// PHOTONIC_IOCTL_UNPREPARE_IMAGER (0x222164) — release imager resources.
BOOL PhotonicIoctlUnprepareImager(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_UNPREPARE_IMAGER, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_START_IMAGER (0x222168) — start single-frame acquisition.
/// mode: 0 = normal run (flags=1), non-zero = alternate (flags=0xFFFFFFFF).
BOOL PhotonicIoctlStartImager(HANDLE hCamera, UINT32 mode) {
    PHOTONIC_START_IMAGER_IN in;
    in.Mode = mode;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_START_IMAGER, &in, sizeof(in), NULL, 0);
}

/// PHOTONIC_IOCTL_STOP_IMAGER (0x22216C) — stop the imager.
BOOL PhotonicIoctlStopImager(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_STOP_IMAGER, NULL, 0, NULL, 0);
}

/// Section 5: Frame rate

/// PHOTONIC_IOCTL_ENUM_FRAME_RATE (0x22212C) — query the Nth available frame rate.
/// Returns FALSE once `index` is past the last entry (enumeration exhausted),
/// so callers can loop: for (i = 0; PhotonicIoctlEnumFrameRate(h, i, &fps); i++).
BOOL PhotonicIoctlEnumFrameRate(HANDLE hCamera, UINT32 index, float *fps) {
    PHOTONIC_ENUM_FRAME_RATE_IN in = {};
    PHOTONIC_ENUM_FRAME_RATE_OUT out;
    in.Index = index;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_ENUM_FRAME_RATE, &in, sizeof(in), &out, sizeof(out))) {
        return FALSE;
    }
    if (out.Status != 0) { // 0x14 == index out of range / enumeration done
        return FALSE;
    }
    if (fps) {
        *fps = out.FrameRateFps;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_SET_FRAME_RATE (0x222130) — select active frame rate by index.
BOOL PhotonicIoctlSetFrameRate(HANDLE hCamera, UINT32 index) {
    PHOTONIC_SET_FRAME_RATE_IN in = {};
    in.Index = index;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SET_FRAME_RATE, &in, sizeof(in), NULL, 0);
}

/// PHOTONIC_IOCTL_GET_FRAME_RATE (0x22213C) — current frame rate (fps).
BOOL PhotonicIoctlGetFrameRate(HANDLE hCamera, float *fps) {
    PHOTONIC_GET_FRAME_RATE_OUT out;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_GET_FRAME_RATE, NULL, 0, &out, sizeof(out))) {
        return FALSE;
    }
    if (out.Status != 0) {
        return FALSE;
    }
    if (fps) {
        *fps = out.FrameRateFps;
    }
    return TRUE;
}

/// Section 6: Trigger / strobe

/// PHOTONIC_IOCTL_TRIGGER_SET (0x222120) — configure DCAM trigger mode.
/// The five-field payload (enable/type/polarity/delay/parameter) is kept as a
/// struct because every field is independently meaningful.
BOOL PhotonicIoctlTriggerSet(HANDLE hCamera, const PHOTONIC_TRIGGER_SET_IN *in) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_TRIGGER_SET, in, sizeof(*in), NULL, 0);
}

/// PHOTONIC_IOCTL_STROBE_SET (0x222124) — configure strobe output (ext cmd 12).
BOOL PhotonicIoctlStrobeSet(HANDLE hCamera, const PHOTONIC_STROBE_SET_IN *in) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_STROBE_SET, in, sizeof(*in), NULL, 0);
}

/// PHOTONIC_IOCTL_SW_TRIGGER (0x222128) — fire a software trigger pulse.
BOOL PhotonicIoctlSwTrigger(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_SW_TRIGGER, NULL, 0, NULL, 0);
}

/// Section 7: Initialize / property (feature) get-set

/// PHOTONIC_IOCTL_INITIALIZE (0x222114) — reset camera to a known state.
BOOL PhotonicIoctlInitialize(HANDLE hCamera) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_INITIALIZE, NULL, 0, NULL, 0);
}

/// PHOTONIC_IOCTL_PROPERTY_GET (0x222118) — read a DCAM feature.
/// featureId is a PHOTONIC_FEATURE_* code; flags/value receive the DCAM
/// feature-control flags and the feature value (either may be NULL).
/// Offset 0 of the output still holds the caller's feature id: the driver
/// writes only Flags and Value, so failures surface through the IOCTL status
/// and GET_LAST_ERROR rather than through the buffer.
BOOL PhotonicIoctlPropertyGet(HANDLE hCamera, UINT32 featureId, UINT32 *flags, UINT32 *value) {
    PHOTONIC_PROPERTY_IN in = {};
    PHOTONIC_PROPERTY_OUT out;
    in.FeatureId = featureId;
    if (!PhotonicIoctl(hCamera, PHOTONIC_IOCTL_PROPERTY_GET, &in, sizeof(in), &out, sizeof(out))) {
        return FALSE;
    }
    if (flags) {
        *flags = out.Flags;
    }
    if (value) {
        *value = out.Value;
    }
    return TRUE;
}

/// PHOTONIC_IOCTL_PROPERTY_SET (0x22211C) — set a DCAM feature.
/// flags use the same PHOTONIC_FEATURE_FLAG_* bits as PROPERTY_GET.
BOOL PhotonicIoctlPropertySet(HANDLE hCamera, UINT32 featureId, UINT32 flags, UINT32 value) {
    PHOTONIC_PROPERTY_SET_IN in;
    in.FeatureId = featureId;
    in.Flags = flags;
    in.Value = value;
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_PROPERTY_SET, &in, sizeof(in), NULL, 0);
}

/// Section 8: mailbox / external I2C register access

/// PHOTONIC_IOCTL_MAILBOX (0x223718) — multiplexed CSR / I2C command gate.
/// This is the raw escape hatch: the first UINT32 of `in` selects the command
/// and buffer sizes vary per command (see the PHOTONIC_MAILBOX_* structs), so the
/// wrapper stays generic.  The buffer is shared in/out (METHOD_BUFFERED).
BOOL PhotonicIoctlMailbox(HANDLE hCamera, void *in, DWORD inLen, void *out, DWORD outLen) {
    return PhotonicIoctl(hCamera, PHOTONIC_IOCTL_MAILBOX, in, inLen, out, outLen);
}
