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
/// DCAM reset and video-mode enumeration. See dcam.h.

// clang-format off
#include "dcam.h"
#include "p1394.h"
#include "dcam.tmh"
// clang-format on

/// DCAM CSR register offsets, relative to the camera's command base. These
/// mirror the register map the device exposes (see fake-fw-dev/dcam.c).
#define DCAM_REG_INITIALIZE       0x000 ///< write bit 31 to reset the camera
#define DCAM_REG_VIDEO_FORMAT_INQ 0x100 ///< bitmask of supported formats
#define DCAM_REG_VIDEO_MODE_INQ   0x180 ///< VIDEO_MODE_INQ_f at +f*4
#define DCAM_REG_FRAME_RATE_INQ   0x200 ///< FRAME_RATE_INQ_(f,m) at +(f*8+m)*4
#define DCAM_REG_FRAME_RATE       0x600 ///< current frame rate (rate << 29)
#define DCAM_REG_VIDEO_MODE       0x604 ///< current mode (mode << 29)
#define DCAM_REG_VIDEO_FORMAT     0x608 ///< current format (format << 29)
#define DCAM_REG_ISOCH_CHANNEL    0x60C ///< channel << 28 | speed << 24
#define DCAM_REG_ISO_EN           0x614 ///< write bit 31 to start streaming
#define DCAM_REG_ONE_SHOT         0x61C ///< bit 31 = arm a single-frame acquisition

/// DCAM ISO_EN start bit and ONE_SHOT arm bit, and field shifts for FRAME_RATE /
/// VIDEO_MODE / VIDEO_FORMAT (all rate/mode/format ids live in the top three
/// bits) and for the ISOCH_CHANNEL register (channel in bits 31..28, speed in
/// bits 27..24).
#define DCAM_ISO_EN_BIT          0x80000000UL
#define DCAM_ONE_SHOT_BIT        0x80000000UL
#define DCAM_SELECT_SHIFT        29
#define DCAM_ISOCH_CHANNEL_SHIFT 28
#define DCAM_ISOCH_SPEED_SHIFT   24

/// Format 7 per-mode CSR register holding the current isochronous packet size in
/// its high word. The host writes it to fix the packet size before streaming; the
/// enumeration reads it to gate out modes the camera cannot currently stream (a
/// zero high word).
#define DCAM_F7_BYTE_PER_PACKET 0x44 ///< bytes per packet << 16 (read/write)

/// Upper bound on the frame byte size a Format 7 mode may advertise. The
/// standard formats have fixed table geometry, but Format 7 width and height
/// come from device-controlled 16-bit register fields, and an absurd geometry
/// (up to 65535 x 65535) would wrap the 32-bit width * height * bytes-per-pixel
/// products used for buffer sizing throughout the driver. Modes above the cap
/// are rejected at enumeration, so every stored geometry multiplies safely in
/// ULONG arithmetic. 256 MB is far above any real DCAM frame (the largest
/// mode a camera of this class advertises is a few megabytes).
#define DCAM_MAX_IMAGE_BYTES (256ul * 1024 * 1024)

/// DCAM Format 7 (scalable image) inquiry registers. Format 7 carries no fixed
/// resolution: each mode advertises a per-mode CSR block (pointed to by
/// V_CSR_INQ_7_m) holding the maximum image size, the unit step, the supported
/// colour codings and the packet parameters. See the F7 enumeration below and
/// the device contract in fake-fw-dev/dcam.c.
#define DCAM_F7_FORMAT            7                                              ///< DCAM format id for scalable image
#define DCAM_REG_VIDEO_MODE_INQ_7 (DCAM_REG_VIDEO_MODE_INQ + DCAM_F7_FORMAT * 4) ///< 0x19c
#define DCAM_REG_V_CSR_INQ_7      0x2e0                                          ///< V_CSR_INQ_7_m at +m*4

/// Format 7 per-mode CSR registers, relative to the mode's CSR block base (which
/// is resolved from the V_CSR_INQ_7_m pointer).
#define DCAM_F7_MAX_IMAGE_SIZE   0x00 ///< max width << 16 | height (read-only)
#define DCAM_F7_UNIT_SIZE        0x04 ///< unit width << 16 | height (read-only)
#define DCAM_F7_IMAGE_POSITION   0x08 ///< current left << 16 | top (read/write)
#define DCAM_F7_IMAGE_SIZE       0x0c ///< current width << 16 | height (read/write)
#define DCAM_F7_COLOR_CODING_ID  0x10 ///< coding << 24 (read/write)
#define DCAM_F7_COLOR_CODING_INQ 0x14 ///< supported codings bitmask (read-only)
#define DCAM_F7_PACKET_PARA_INQ  0x40 ///< unit bytes/packet << 16 | max bytes/packet (read-only)

/// A 1394 isochronous cycle is 125us = 1250 units of 100ns. At one packet per
/// cycle the frame interval (100ns) is packets-per-frame * this, so for a frame
/// of TotalBytes streamed at BytesPerPacket the interval is
/// TotalBytes / BytesPerPacket * DCAM_ISO_CYCLE_100NS, i.e. TotalBytes *
/// DCAM_ISO_CYCLE_100NS / BytesPerPacket.
#define DCAM_ISO_CYCLE_100NS 1250

/// DCAM INITIALIZE register: writing this bit asks the camera to reset its
/// registers to power-up defaults and self-clears when reset completes.
#define DCAM_INITIALIZE_BIT 0x80000000UL

/// DCAM geometry-table dimensions for the standard formats (0, 1, 2). Format 7
/// (scalable image) is advertised through a separate per-mode CSR block and is
/// enumerated by PhotonicDcamEnumerateFormat7, not from this table.
#define DCAM_NUM_FORMATS 3
#define DCAM_NUM_MODES   8
#define DCAM_NUM_RATES   6

/// Decoded geometry for one (format, mode) slot. PixelFormat ==
/// PHOTONIC_DCAM_PIX_INVALID marks a slot the standard DCAM table leaves
/// undefined. This is the authoritative (resolution, colour coding) mapping from
/// the IIDC/DCAM "1394-based Digital Camera Specification" v1.31 and matches the
/// table the emulated camera streams from.
typedef struct _DCAM_MODE_GEOMETRY {
    ULONG PixelFormat;
    USHORT Width;
    USHORT Height;
} DCAM_MODE_GEOMETRY;

static const DCAM_MODE_GEOMETRY g_DcamGeometry[DCAM_NUM_FORMATS][DCAM_NUM_MODES] = {
    {
        // Format 0
        {PHOTONIC_DCAM_PIX_YUV444, 160, 120},
        {PHOTONIC_DCAM_PIX_YUV422, 320, 240},
        {PHOTONIC_DCAM_PIX_YUV411, 640, 480},
        {PHOTONIC_DCAM_PIX_YUV422, 640, 480},
        {PHOTONIC_DCAM_PIX_RGB24, 640, 480},
        {PHOTONIC_DCAM_PIX_MONO8, 640, 480},
        {PHOTONIC_DCAM_PIX_MONO16, 640, 480},
        {PHOTONIC_DCAM_PIX_INVALID, 0, 0},
    },
    {
        // Format 1
        {PHOTONIC_DCAM_PIX_YUV422, 800, 600},
        {PHOTONIC_DCAM_PIX_RGB24, 800, 600},
        {PHOTONIC_DCAM_PIX_MONO8, 800, 600},
        {PHOTONIC_DCAM_PIX_YUV422, 1024, 768},
        {PHOTONIC_DCAM_PIX_RGB24, 1024, 768},
        {PHOTONIC_DCAM_PIX_MONO8, 1024, 768},
        {PHOTONIC_DCAM_PIX_MONO16, 800, 600},
        {PHOTONIC_DCAM_PIX_MONO16, 1024, 768},
    },
    {
        // Format 2
        {PHOTONIC_DCAM_PIX_YUV422, 1280, 960},
        {PHOTONIC_DCAM_PIX_RGB24, 1280, 960},
        {PHOTONIC_DCAM_PIX_MONO8, 1280, 960},
        {PHOTONIC_DCAM_PIX_YUV422, 1600, 1200},
        {PHOTONIC_DCAM_PIX_RGB24, 1600, 1200},
        {PHOTONIC_DCAM_PIX_MONO8, 1600, 1200},
        {PHOTONIC_DCAM_PIX_MONO16, 1280, 960},
        {PHOTONIC_DCAM_PIX_MONO16, 1600, 1200},
    },
};

/// Frame interval in 100ns units for each standard DCAM frame-rate id, ordered
/// from the slowest (rate 0 = 1.875 fps) to the fastest (rate 5 = 60 fps). A
/// higher rate id means a faster rate and thus a smaller interval.
static const ULONG g_DcamRateInterval[DCAM_NUM_RATES] = {
    5333333, // rate 0: 1.875 fps
    2666667, // rate 1: 3.75 fps
    1333333, // rate 2: 7.5 fps
    666667,  // rate 3: 15 fps
    333333,  // rate 4: 30 fps
    166667,  // rate 5: 60 fps
};

NTSTATUS PhotonicDcamReset(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    NTSTATUS status;
    ULONG value;
    ULONG attempt;

    FuncEntry(TRACE_FLAG_DCAM);

    status = Photonic1394WriteRegister(Extension, DCAM_REG_INITIALIZE, DCAM_INITIALIZE_BIT);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "INITIALIZE write failed: %!STATUS!\n", status);
        return status;
    }

    //
    // Poll the INITIALIZE register until the reset bit self-clears, bounded so a
    // camera that never clears it cannot hang bring-up. A short stall between
    // reads gives the camera time to finish; this runs at PASSIVE_LEVEL during
    // SRB_INITIALIZE_DEVICE, so blocking the thread is safe.
    //
    for (attempt = 0; attempt < 50; attempt++) {
        LARGE_INTEGER delay;

        status = Photonic1394ReadRegister(Extension, DCAM_REG_INITIALIZE, &value);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "INITIALIZE read failed: %!STATUS!\n", status);
            return status;
        }
        if ((value & DCAM_INITIALIZE_BIT) == 0) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "reset complete after %u poll(s)\n", attempt + 1);
            return STATUS_SUCCESS;
        }

        delay.QuadPart = -10 * 1000 * 10; // 10 ms, relative
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "reset bit did not clear; continuing\n");
    return STATUS_SUCCESS;
}

/// PhotonicDcamRateBounds -- from a FRAME_RATE_INQ bitmask (bit (31 - rate) set
/// per supported rate), compute the minimum and maximum frame interval in 100ns
/// units across the supported rates.
///
/// @param RateMask    FRAME_RATE_INQ bitmask (bit 31-rate set per supported rate).
/// @param MinInterval Receives the shortest frame interval (fastest rate) in 100ns units.
/// @param MaxInterval Receives the longest frame interval (slowest rate) in 100ns units.
static VOID PhotonicDcamRateBounds(_In_ ULONG RateMask, _Out_ PULONG MinInterval, _Out_ PULONG MaxInterval) {
    ULONG min = 0;
    ULONG max = 0;
    ULONG rate;

    for (rate = 0; rate < DCAM_NUM_RATES; rate++) {
        if (RateMask & (0x80000000UL >> rate)) {
            ULONG interval = g_DcamRateInterval[rate];
            if (max == 0 || interval > max) {
                max = interval;
            }
            if (min == 0 || interval < min) {
                min = interval;
            }
        }
    }

    *MinInterval = min;
    *MaxInterval = max;
}

/// PhotonicDcamFormat7Interval -- frame interval in 100ns units for a Format 7
/// frame of TotalBytes streamed at BytesPerPacket (one packet per isochronous
/// cycle). A larger packet means fewer packets per frame and so a shorter
/// interval (faster rate). Returns 0 when BytesPerPacket is 0 (unstreamable),
/// and saturates to MAXULONG when the interval does not fit 32 bits.
///
/// @param TotalBytes     Total frame size in bytes.
/// @param BytesPerPacket Isochronous payload per packet in bytes.
/// @return Frame interval in 100ns units, or 0 when BytesPerPacket is 0.
static ULONG PhotonicDcamFormat7Interval(_In_ ULONG TotalBytes, _In_ ULONG BytesPerPacket) {
    ULONGLONG interval;

    if (BytesPerPacket == 0) {
        return 0;
    }

    //
    // The multiply fits 64 bits (TotalBytes is capped at 256 MB), but the
    // quotient can still exceed a ULONG when a corrupt camera pairs a large
    // frame with a tiny packet. Saturate rather than truncate so the mode
    // table never carries a wrapped interval or inverted bounds.
    //
    interval = ((ULONGLONG) TotalBytes * DCAM_ISO_CYCLE_100NS) / BytesPerPacket;
    return interval > MAXULONG ? MAXULONG : (ULONG) interval;
}

/// PhotonicDcamCodingBpp -- bits per pixel for a DCAM colour coding id
/// (PHOTONIC_DCAM_PIX_*). The enumeration derives a frame's byte size from its
/// geometry and this depth (width * height * bpp / 8) rather than reading the
/// camera's TOTAL_BYTES register. Returns 0 for a coding the driver does not
/// recognise.
///
/// @param Coding  PHOTONIC_DCAM_PIX_* colour coding id.
/// @return Bits per pixel, or 0 for an unrecognised coding.
ULONG PhotonicDcamCodingBpp(_In_ ULONG Coding) {
    switch (Coding) {
        case PHOTONIC_DCAM_PIX_MONO8:
        case PHOTONIC_DCAM_PIX_RAW8:
            return 8;
        case PHOTONIC_DCAM_PIX_YUV411:
            return 12;
        case PHOTONIC_DCAM_PIX_YUV422:
        case PHOTONIC_DCAM_PIX_MONO16:
            return 16;
        case PHOTONIC_DCAM_PIX_YUV444:
        case PHOTONIC_DCAM_PIX_RGB24:
            return 24;
        default:
            return 0;
    }
}

/// PhotonicDcamEnumerateFormat7 -- enumerate the camera's Format 7 (scalable
/// image) modes. Each F7 mode advertised in VIDEO_MODE_INQ_7 has its own CSR
/// block (pointed to by V_CSR_INQ_7_m) describing the maximum image size, the
/// unit step and the colour codings it supports. One PHOTONIC_VIDEO_MODE entry is
/// added per (mode, supported coding): its scalable output-size window runs from
/// the unit step up to the maximum image size (aligned to the unit grid) and
/// defaults to the camera's current image size. The frame-rate bounds come from
/// the camera's packet parameters read at the current image size; the CSR block
/// is only read here, never programmed. The enumeration selects format 7
/// (VIDEO_FORMAT) once and each mode (VIDEO_MODE) before reading its CSR block, so a camera that only refreshes the
/// per-mode registers for the selected mode returns live values. Best effort: a failed register access skips the
/// affected mode or coding rather than discarding the standard modes already
/// enumerated.
///
/// @param Extension  Device extension.
static VOID PhotonicDcamEnumerateFormat7(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    NTSTATUS status;
    ULONG modeMask;
    ULONG mode;

    FuncEntry(TRACE_FLAG_DCAM);

    //
    // Select format 7 before reading its mode-inquiry register: a camera that
    // refreshes VIDEO_MODE_INQ_7 only for the selected format then reports the
    // live mask.
    //
    status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_FORMAT, (ULONG) DCAM_F7_FORMAT << DCAM_SELECT_SHIFT);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "VIDEO_FORMAT (format 7) write failed: %!STATUS!\n", status);
        return;
    }

    status = Photonic1394ReadRegister(Extension, DCAM_REG_VIDEO_MODE_INQ_7, &modeMask);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "VIDEO_MODE_INQ_7 read failed: %!STATUS!\n", status);
        return;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "Format 7 VIDEO_MODE_INQ_7=0x%08x\n", modeMask);

    for (mode = 0; mode < DCAM_NUM_MODES; mode++) {
        ULONG pointer;
        ULONG base;
        ULONG maxSize;
        ULONG unitSize = 0;
        ULONG codingMask = 0;
        ULONG curSize = 0;
        ULONG curCoding = 0;
        ULONG packetPara = 0;
        ULONG bytePerPacket = 0;
        ULONG maxWidth;
        ULONG maxHeight;
        ULONG unitWidth;
        ULONG unitHeight;
        ULONG alignedMaxWidth;
        ULONG alignedMaxHeight;
        ULONG curWidth;
        ULONG curHeight;
        ULONG unitBpp;
        ULONG maxBpp;
        ULONG bpp;
        ULONG frameBytes;
        ULONG coding;

        if ((modeMask & (0x80000000UL >> mode)) == 0) {
            continue;
        }

        //
        // Resolve the mode's CSR block from its V_CSR_INQ_7_m pointer, then
        // select the mode so the per-mode registers below describe it, and read
        // its fixed geometry (maximum and unit image size) and the bitmask of
        // colour codings it supports.
        //
        status = Photonic1394ReadRegister(Extension, DCAM_REG_V_CSR_INQ_7 + mode * 4, &pointer);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u V_CSR_INQ read failed: %!STATUS!\n", mode,
                        status);
            continue;
        }
        status = Photonic1394CsrPointerToOffset(Extension, pointer, DCAM_F7_BYTE_PER_PACKET + sizeof(ULONG), &base);
        if (!NT_SUCCESS(status)) {
            //
            // The camera advertised a CSR pointer outside the plausible
            // register window. Writes through it would alias arbitrary camera
            // registers later (IMAGE_SIZE could land on ISO_EN), so the mode
            // is skipped.
            //
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u CSR pointer rejected; mode skipped\n", mode);
            continue;
        }

        status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_MODE, mode << DCAM_SELECT_SHIFT);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u VIDEO_MODE write failed: %!STATUS!\n", mode,
                        status);
            continue;
        }

        status = Photonic1394ReadRegister(Extension, base + DCAM_F7_MAX_IMAGE_SIZE, &maxSize);
        if (NT_SUCCESS(status)) {
            status = Photonic1394ReadRegister(Extension, base + DCAM_F7_UNIT_SIZE, &unitSize);
        }
        if (NT_SUCCESS(status)) {
            status = Photonic1394ReadRegister(Extension, base + DCAM_F7_COLOR_CODING_INQ, &codingMask);
        }
        if (NT_SUCCESS(status)) {
            //
            // Read the camera's current (power-up default) image size. Unlike the
            // maximum size, this is guaranteed to be unit-aligned, so it is the
            // geometry the pin connects at, and the packet parameters read below
            // describe this frame. IMAGE_SIZE is never programmed during
            // enumeration (see below).
            //
            status = Photonic1394ReadRegister(Extension, base + DCAM_F7_IMAGE_SIZE, &curSize);
        }
        if (NT_SUCCESS(status)) {
            //
            // Read the current colour coding: its bit depth gives the frame's
            // byte size (width * height * bpp / 8), derived in software rather
            // than read from a TOTAL_BYTES register.
            //
            status = Photonic1394ReadRegister(Extension, base + DCAM_F7_COLOR_CODING_ID, &curCoding);
        }
        if (NT_SUCCESS(status)) {
            //
            // Read the packet-size bounds and the current bytes-per-packet once
            // per mode (they are per-mode values, not per-coding). The
            // bounds set the advertised frame-rate range; the current
            // bytes-per-packet high word gates out an unstreamable mode below.
            //
            status = Photonic1394ReadRegister(Extension, base + DCAM_F7_PACKET_PARA_INQ, &packetPara);
        }
        if (NT_SUCCESS(status)) {
            status = Photonic1394ReadRegister(Extension, base + DCAM_F7_BYTE_PER_PACKET, &bytePerPacket);
        }
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u CSR read failed: %!STATUS!\n", mode, status);
            continue;
        }

        maxWidth = maxSize >> 16;
        maxHeight = maxSize & 0xFFFF;
        unitWidth = unitSize >> 16;
        unitHeight = unitSize & 0xFFFF;
        if (maxWidth == 0 || maxHeight == 0) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u has zero max image size; skipped\n", mode);
            continue;
        }
        if (unitWidth == 0 || unitHeight == 0) {
            //
            // A camera must report a non-zero unit step; fall back to the max
            // size (a single non-scalable size) rather than dividing by zero.
            //
            unitWidth = maxWidth;
            unitHeight = maxHeight;
        }

        //
        // The scalable output-size window runs up to the maximum image size
        // aligned down to the unit grid -- the maximum itself need not be a
        // multiple of the unit step (e.g. 1575x1053 with a 4x4 unit) and so is not
        // a legal size. This aligned maximum is the ceiling the mode advertises:
        // the range is capped at MAX_IMAGE_SIZE rather than at the current size.
        //
        alignedMaxWidth = (maxWidth / unitWidth) * unitWidth;
        alignedMaxHeight = (maxHeight / unitHeight) * unitHeight;
        if (alignedMaxWidth == 0 || alignedMaxHeight == 0) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u has no usable max image size; skipped\n",
                        mode);
            continue;
        }

        //
        // The pin connects at the camera's current image size by default. The
        // camera reports a unit-aligned current IMAGE_SIZE by design, so
        // aligning it to the unit grid is normally a no-op. The clamp that
        // follows is the real guard: the geometry is device-controlled, and a
        // camera reporting a current size above its own maximum would
        // otherwise advertise a default larger than the range ceiling (and
        // than MAX_IMAGE_BYTES allows for a deeper coding), so the default is
        // capped at the aligned maximum, which is itself a unit multiple.
        // Fall back to the aligned maximum if the camera reports an empty
        // size.
        //
        curWidth = ((curSize >> 16) / unitWidth) * unitWidth;
        curHeight = ((curSize & 0xFFFF) / unitHeight) * unitHeight;
        if (curWidth > alignedMaxWidth) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                        "F7 mode %u current width %u exceeds the aligned max %u; clamped\n", mode, curWidth,
                        alignedMaxWidth);
            curWidth = alignedMaxWidth;
        }
        if (curHeight > alignedMaxHeight) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                        "F7 mode %u current height %u exceeds the aligned max %u; clamped\n", mode, curHeight,
                        alignedMaxHeight);
            curHeight = alignedMaxHeight;
        }
        if (curWidth == 0 || curHeight == 0) {
            curWidth = alignedMaxWidth;
            curHeight = alignedMaxHeight;
        }

        //
        // The packet-size bounds and the current bytes-per-packet describe the
        // current frame. Skip the mode unless all three are non-zero: a zero unit
        // or max packet size means the camera advertises no usable packetisation,
        // and a zero current bytes-per-packet means it cannot stream this frame as
        // configured.
        //
        unitBpp = packetPara >> 16;
        maxBpp = packetPara & 0xFFFF;
        if (unitBpp == 0 || maxBpp == 0 || (bytePerPacket >> 16) == 0) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                        "F7 mode %u unstreamable (packet=0x%08x byte/packet=0x%08x); skipped\n", mode, packetPara,
                        bytePerPacket);
            continue;
        }

        //
        // Derive the frame's byte size from its geometry and the current colour
        // coding's bit depth (width * height * bpp / 8) rather than reading a
        // TOTAL_BYTES register. Skip the mode if the
        // current coding is one the driver does not recognise.
        //
        bpp = PhotonicDcamCodingBpp(curCoding >> 24);
        if (bpp == 0) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u unknown current coding %u; skipped\n", mode,
                        curCoding >> 24);
            continue;
        }

        //
        // The geometry fields are device-controlled. Cap the frame byte size
        // with the multiply done in 64 bits, so a broken camera cannot make
        // the 32-bit sizing products wrap downstream.
        //
        if ((((ULONGLONG) curWidth * curHeight * bpp) >> 3) > DCAM_MAX_IMAGE_BYTES) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                        "F7 mode %u current frame %ux%u at %u bpp exceeds the %u-byte cap; skipped\n", mode, curWidth,
                        curHeight, bpp, DCAM_MAX_IMAGE_BYTES);
            continue;
        }
        frameBytes = PhotonicImageBytes(curWidth, curHeight, bpp);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM,
                    "F7 mode %u: cur %ux%u max %ux%u (aligned %ux%u) unit %ux%u codings=0x%08x coding=%u bytes=%u "
                    "bpp=[%u..%u] csr+0x%x\n",
                    mode, curWidth, curHeight, maxWidth, maxHeight, alignedMaxWidth, alignedMaxHeight, unitWidth,
                    unitHeight, codingMask, curCoding >> 24, frameBytes, unitBpp, maxBpp, base);

        //
        // Do NOT program IMAGE_SIZE during enumeration. Writing back the maximum
        // size fails on real hardware because the maximum is not a multiple of
        // the unit step, so the camera rejects the asynchronous write
        // (STATUS_DEVICE_DATA_ERROR). The CSR block is only read here; IMAGE_SIZE
        // is programmed later, on the stream-configure path, with the negotiated
        // (unit-aligned) geometry.
        //
        for (coding = 0; coding < 32; coding++) {
            PPHOTONIC_VIDEO_MODE entry;
            ULONG codingBpp;
            ULONG codingFrameBytes;

            if ((codingMask & (0x80000000UL >> coding)) == 0) {
                continue;
            }

            //
            // Validate each advertised coding against the maximum geometry
            // before it enters the mode table, so every consumer of the table
            // can multiply width, height and depth in ULONG arithmetic
            // without re-checking. A coding the driver does not recognise
            // could never be streamed or mapped to a DirectShow subtype, so
            // it is dropped here as well.
            //
            codingBpp = PhotonicDcamCodingBpp(coding);
            if (codingBpp == 0) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7 mode %u unknown coding %u; not advertised\n",
                            mode, coding);
                continue;
            }
            if ((((ULONGLONG) alignedMaxWidth * alignedMaxHeight * codingBpp) >> 3) > DCAM_MAX_IMAGE_BYTES) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                            "F7 mode %u coding %u max frame %ux%u at %u bpp exceeds the %u-byte cap; not advertised\n",
                            mode, coding, alignedMaxWidth, alignedMaxHeight, codingBpp, DCAM_MAX_IMAGE_BYTES);
                continue;
            }

            if (Extension->ModeCount >= PHOTONIC_MAX_VIDEO_MODES) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "mode table full (%u); remaining F7 modes dropped\n",
                            PHOTONIC_MAX_VIDEO_MODES);
                return;
            }

            entry = &Extension->Modes[Extension->ModeCount];
            entry->Format = (UCHAR) DCAM_F7_FORMAT;
            entry->Mode = (UCHAR) mode;
            entry->PixelFormat = coding;
            entry->Width = alignedMaxWidth;
            entry->Height = alignedMaxHeight;
            entry->DefaultWidth = curWidth;
            entry->DefaultHeight = curHeight;
            entry->IsFormat7 = TRUE;
            entry->UnitWidth = unitWidth;
            entry->UnitHeight = unitHeight;
            entry->Format7CsrOffset = base;
            entry->RateMask = 0;
            //
            // The fastest rate uses the largest packet (maxBpp), the slowest the
            // unit packet (unitBpp); a larger packet gives a shorter interval.
            // The interval bounds are per coding: the default frame's byte size
            // scales with this coding's depth, not with the depth of whichever
            // coding the camera currently has selected. The frame-byte product
            // fits a ULONG because the cap check above bounded the
            // aligned-maximum frame for this coding and the current size never
            // exceeds it; the interval helper saturates the quotient itself,
            // which can overflow for a tiny advertised packet.
            //
            codingFrameBytes = PhotonicImageBytes(curWidth, curHeight, codingBpp);
            entry->MinFrameInterval = PhotonicDcamFormat7Interval(codingFrameBytes, maxBpp);
            entry->MaxFrameInterval = PhotonicDcamFormat7Interval(codingFrameBytes, unitBpp);
            Extension->ModeCount++;

            TraceEvents(
                TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM,
                "mode[%u]: F7/M%u def %ux%u max %ux%u (unit %ux%u) pix=%u bytes=%u bpp=[%u..%u] interval=[%u..%u]\n",
                Extension->ModeCount - 1, mode, curWidth, curHeight, alignedMaxWidth, alignedMaxHeight, unitWidth,
                unitHeight, coding, codingFrameBytes, unitBpp, maxBpp, entry->MinFrameInterval,
                entry->MaxFrameInterval);
        }
    }
}

static NTSTATUS PhotonicDcamReadSelection(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG Format,
                                          _Out_ PULONG Mode);

/// PhotonicDcamNormalizeSelection -- leave the camera's VIDEO_FORMAT /
/// VIDEO_MODE selection on a mode the driver enumerated. The Format 7 probe
/// walks the selection registers as it inspects each mode, and a probe whose
/// last-walked mode was skipped (a failed CSR read, an unstreamable
/// packetisation) would otherwise leave the camera selected on a pair absent
/// from the mode table, making every current-selection lookup (the IOCTL
/// prepare, GET_VIDEO_FORMAT) fail until the client programs a format
/// explicitly. Restores the pre-enumeration selection when it maps to an
/// enumerated mode, otherwise programs the first enumerated mode. The caller
/// guarantees the mode table is not empty.
///
/// @param Extension    Device extension holding the enumerated mode table.
/// @param SavedValid   TRUE when the pre-enumeration selection was read.
/// @param SavedFormat  Pre-enumeration VIDEO_FORMAT selection.
/// @param SavedMode    Pre-enumeration VIDEO_MODE selection.
static VOID PhotonicDcamNormalizeSelection(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ BOOLEAN SavedValid,
                                           _In_ ULONG SavedFormat, _In_ ULONG SavedMode) {
    ULONG format = Extension->Modes[0].Format;
    ULONG modeId = Extension->Modes[0].Mode;
    ULONG i;
    NTSTATUS status;

    if (SavedValid) {
        for (i = 0; i < Extension->ModeCount; i++) {
            if (Extension->Modes[i].Format == SavedFormat && Extension->Modes[i].Mode == SavedMode) {
                format = SavedFormat;
                modeId = SavedMode;
                break;
            }
        }
    }

    status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_FORMAT, format << DCAM_SELECT_SHIFT);
    if (NT_SUCCESS(status)) {
        status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_MODE, modeId << DCAM_SELECT_SHIFT);
    }
    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "selection left on F%u/M%u after enumeration\n", format,
                    modeId);
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                    "could not program the post-enumeration selection F%u/M%u: %!STATUS!\n", format, modeId, status);
    }
}

NTSTATUS PhotonicDcamEnumerateModes(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    NTSTATUS status;
    ULONG formatMask;
    ULONG format;
    ULONG savedFormat = 0;
    ULONG savedMode = 0;
    BOOLEAN savedValid;

    FuncEntry(TRACE_FLAG_DCAM);

    Extension->ModeCount = 0;

    //
    // Save the camera's power-on selection before any probing writes it: the
    // Format 7 pass below walks VIDEO_FORMAT / VIDEO_MODE while it inspects
    // each mode, and the selection is put back (or normalized onto an
    // enumerated mode) once enumeration is done.
    //
    savedValid = NT_SUCCESS(PhotonicDcamReadSelection(Extension, &savedFormat, &savedMode));

    status = Photonic1394ReadRegister(Extension, DCAM_REG_VIDEO_FORMAT_INQ, &formatMask);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "VIDEO_FORMAT_INQ=0x%08x\n", formatMask);

    for (format = 0; format < DCAM_NUM_FORMATS; format++) {
        ULONG modeMask;
        ULONG mode;

        if ((formatMask & (0x80000000UL >> format)) == 0) {
            continue;
        }

        status = Photonic1394ReadRegister(Extension, DCAM_REG_VIDEO_MODE_INQ + format * 4, &modeMask);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "format %u VIDEO_MODE_INQ=0x%08x\n", format, modeMask);

        for (mode = 0; mode < DCAM_NUM_MODES; mode++) {
            const DCAM_MODE_GEOMETRY *geom = &g_DcamGeometry[format][mode];
            ULONG rateMask;
            ULONG minInterval;
            ULONG maxInterval;
            PPHOTONIC_VIDEO_MODE entry;

            if ((modeMask & (0x80000000UL >> mode)) == 0) {
                continue;
            }
            if (geom->PixelFormat == PHOTONIC_DCAM_PIX_INVALID) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                            "format %u mode %u advertised but has no standard geometry; skipped\n", format, mode);
                continue;
            }

            status = Photonic1394ReadRegister(Extension, DCAM_REG_FRAME_RATE_INQ + (format * DCAM_NUM_MODES + mode) * 4,
                                              &rateMask);
            if (!NT_SUCCESS(status)) {
                return status;
            }

            //
            // The mask must carry at least one rate id this driver streams
            // (rates 0 to 5). A camera can legally, or maliciously, set only
            // bits outside that range, and zero interval bounds would later
            // be papered over with a 30 fps default: the mode would negotiate
            // normally and then fail every start, since the rate selection
            // honors the same mask. Skip it like any other unstreamable mode.
            //
            PhotonicDcamRateBounds(rateMask, &minInterval, &maxInterval);
            if (minInterval == 0) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM,
                            "format %u mode %u FRAME_RATE_INQ 0x%08x carries no supported rate; skipped\n", format,
                            mode, rateMask);
                continue;
            }

            if (Extension->ModeCount >= PHOTONIC_MAX_VIDEO_MODES) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "mode table full (%u); remaining modes dropped\n",
                            PHOTONIC_MAX_VIDEO_MODES);
                goto done;
            }

            entry = &Extension->Modes[Extension->ModeCount];
            entry->Format = (UCHAR) format;
            entry->Mode = (UCHAR) mode;
            entry->PixelFormat = geom->PixelFormat;
            entry->Width = geom->Width;
            entry->Height = geom->Height;
            entry->DefaultWidth = geom->Width;
            entry->DefaultHeight = geom->Height;
            entry->IsFormat7 = FALSE;
            entry->UnitWidth = 0;
            entry->UnitHeight = 0;
            entry->Format7CsrOffset = 0;
            entry->RateMask = rateMask;
            entry->MinFrameInterval = minInterval;
            entry->MaxFrameInterval = maxInterval;
            Extension->ModeCount++;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM,
                        "mode[%u]: F%u/M%u %ux%u pix=%u rates=0x%08x interval=[%u..%u]\n", Extension->ModeCount - 1,
                        format, mode, geom->Width, geom->Height, geom->PixelFormat, rateMask, entry->MinFrameInterval,
                        entry->MaxFrameInterval);
        }
    }

    //
    // Format 7 (scalable image) is advertised through a separate per-mode CSR
    // block rather than the standard mode/rate inquiry registers. Enumerate it
    // when VIDEO_FORMAT_INQ advertises format 7.
    //
    if (formatMask & (0x80000000UL >> DCAM_F7_FORMAT)) {
        PhotonicDcamEnumerateFormat7(Extension);
    }

done:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "enumerated %u video mode(s)\n", Extension->ModeCount);

    if (Extension->ModeCount == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    PhotonicDcamNormalizeSelection(Extension, savedValid, savedFormat, savedMode);
    return STATUS_SUCCESS;
}

/// Isochronous packetisation of each standard (format, mode, rate): the per-packet
/// payload in bytes and the number of packets per frame. This is the authoritative
/// DCAM packet table (the camera derives its own packetisation from the same
/// IIDC/DCAM specification, so it is carried here rather than read from a register;
/// the emulated camera streams from the identical table). A zero BytesPerPacket
/// marks an unsupported (mode, rate) slot. Rate order is the DCAM rate id
/// (0 = 1.875 fps .. 5 = 60 fps); BytesPerPacket * PacketsPerFrame is the frame
/// size in bytes and matches the geometry advertised for the mode.
typedef struct _DCAM_PACKET_GEOMETRY {
    USHORT BytesPerPacket;
    USHORT PacketsPerFrame;
} DCAM_PACKET_GEOMETRY;

static const DCAM_PACKET_GEOMETRY g_DcamPacketGeometry[DCAM_NUM_FORMATS][DCAM_NUM_MODES][DCAM_NUM_RATES] = {
    {
        // Format 0
        {{0, 0}, {0, 0}, {60, 960}, {120, 480}, {240, 240}, {0, 0}},             // M0 160x120 YUV444
        {{0, 0}, {80, 1920}, {160, 960}, {320, 480}, {640, 240}, {0, 0}},        // M1 320x240 YUV422
        {{0, 0}, {240, 1920}, {480, 960}, {960, 480}, {1920, 240}, {0, 0}},      // M2 640x480 YUV411
        {{0, 0}, {320, 1920}, {640, 960}, {1280, 480}, {2560, 240}, {0, 0}},     // M3 640x480 YUV422
        {{0, 0}, {480, 1920}, {960, 960}, {1920, 480}, {3840, 240}, {0, 0}},     // M4 640x480 RGB24
        {{0, 0}, {160, 1920}, {320, 960}, {640, 480}, {1280, 240}, {2560, 120}}, // M5 640x480 MONO8
        {{0, 0}, {320, 1920}, {640, 960}, {1280, 480}, {2560, 240}, {0, 0}},     // M6 640x480 MONO16
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},                        // M7 reserved
    },
    {
        // Format 1
        {{0, 0}, {500, 1920}, {1000, 960}, {2000, 480}, {4000, 240}, {0, 0}},      // M0 800x600 YUV422
        {{0, 0}, {0, 0}, {1500, 960}, {3000, 480}, {0, 0}, {0, 0}},                // M1 800x600 RGB24
        {{0, 0}, {0, 0}, {500, 960}, {1000, 480}, {2000, 240}, {4000, 120}},       // M2 800x600 MONO8
        {{384, 4096}, {768, 2048}, {1536, 1024}, {3072, 512}, {0, 0}, {0, 0}},     // M3 1024x768 YUV422
        {{576, 4096}, {1152, 2048}, {2304, 1024}, {0, 0}, {0, 0}, {0, 0}},         // M4 1024x768 RGB24
        {{192, 4096}, {384, 2048}, {768, 1024}, {1536, 512}, {3072, 256}, {0, 0}}, // M5 1024x768 MONO8
        {{0, 0}, {500, 1920}, {1000, 960}, {2000, 480}, {4000, 240}, {0, 0}},      // M6 800x600 MONO16
        {{384, 4096}, {768, 2048}, {1536, 1024}, {3072, 512}, {0, 0}, {0, 0}},     // M7 1024x768 MONO16
    },
    {
        // Format 2
        {{640, 3840}, {1280, 1920}, {2560, 960}, {0, 0}, {0, 0}, {0, 0}},      // M0 1280x960 YUV422
        {{960, 3840}, {1920, 1920}, {3840, 960}, {0, 0}, {0, 0}, {0, 0}},      // M1 1280x960 RGB24
        {{320, 3840}, {640, 1920}, {1280, 960}, {2560, 480}, {0, 0}, {0, 0}},  // M2 1280x960 MONO8
        {{1000, 3840}, {2000, 1920}, {4000, 960}, {0, 0}, {0, 0}, {0, 0}},     // M3 1600x1200 YUV422
        {{1500, 3840}, {3000, 1920}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},          // M4 1600x1200 RGB24
        {{500, 3840}, {1000, 1920}, {2000, 960}, {4000, 480}, {0, 0}, {0, 0}}, // M5 1600x1200 MONO8
        {{640, 3840}, {1280, 1920}, {2560, 960}, {0, 0}, {0, 0}, {0, 0}},      // M6 1280x960 MONO16
        {{1000, 3840}, {2000, 1920}, {4000, 960}, {0, 0}, {0, 0}, {0, 0}},     // M7 1600x1200 MONO16
    },
};

/// PhotonicDcamSelectRate -- pick the DCAM rate id for a standard (format, mode)
/// whose nominal interval is closest to the connection's negotiated frame interval,
/// considering only rates the camera advertises in FRAME_RATE_INQ (RateMask), that
/// the packet table marks supported, and whose packet fits the bus speed's
/// isochronous payload limit (MaxPayload). Skipping rates outside RateMask matters:
/// a conforming camera NAKs a FRAME_RATE write it never advertised, and a lenient
/// one would stream a packetisation that disagrees with the host's table. Returns
/// the rate id, or DCAM_NUM_RATES if the mode has no streamable rate.
///
/// @param Format      DCAM format id (0..2).
/// @param Mode        DCAM mode id within the format (0..7).
/// @param RateMask    FRAME_RATE_INQ bitmask (bit 31-rate set per supported rate).
/// @param Interval    Negotiated frame interval in 100ns units.
/// @param MaxPayload  Maximum isochronous payload the bus speed allows, in bytes.
/// @return Best-matching DCAM rate id, or DCAM_NUM_RATES if no streamable rate exists.
static ULONG PhotonicDcamSelectRate(_In_ ULONG Format, _In_ ULONG Mode, _In_ ULONG RateMask, _In_ ULONG Interval,
                                    _In_ ULONG MaxPayload) {
    ULONG best = DCAM_NUM_RATES;
    ULONG bestDelta = 0;
    ULONG rate;

    for (rate = 0; rate < DCAM_NUM_RATES; rate++) {
        ULONG delta;

        if ((RateMask & (0x80000000UL >> rate)) == 0) {
            continue;
        }
        if (g_DcamPacketGeometry[Format][Mode][rate].BytesPerPacket == 0 ||
            g_DcamPacketGeometry[Format][Mode][rate].BytesPerPacket > MaxPayload) {
            continue;
        }
        delta = g_DcamRateInterval[rate] > Interval ? g_DcamRateInterval[rate] - Interval
                                                    : Interval - g_DcamRateInterval[rate];
        if (best == DCAM_NUM_RATES || delta < bestDelta) {
            best = rate;
            bestDelta = delta;
        }
    }

    return best;
}

/// PhotonicDcamConfigureFormat7 -- program a Format 7 mode's CSR block for the
/// negotiated geometry and pick the isochronous packet size. The packet size is
/// derived from the negotiated frame rate, floored to an integer multiple of
/// UNIT_BYTE_PER_PACKET (which the camera requires, and which keeps the size
/// quadlet-aligned for the iso path), clamped to one unit on underflow and to
/// the advertised PACKET_PARA_INQ maximum -- capped at the bus speed's payload
/// limit -- on overflow, and finally snapped to a legal size that divides the
/// frame exactly: the largest such divisor at or below the rate-derived
/// target, or the smallest above it when none is smaller. The exact division
/// is what makes the camera and
/// the host agree on the same whole number of packets per frame: a size that
/// does not divide the frame would truncate the tail of every delivered image.
/// A geometry no legal packet size divides is rejected.
///
/// @param Extension       Device extension.
/// @param Stream          Stream extension holding the negotiated mode, geometry, and
///                        frame interval.
/// @param BytesPerPacket  Receives the per-packet isochronous payload in bytes.
/// @param PacketsPerFrame Receives the number of isochronous packets per frame.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicDcamConfigureFormat7(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                                             _In_ PPHOTONIC_STREAM_EXTENSION Stream, _Out_ PULONG BytesPerPacket,
                                             _Out_ PULONG PacketsPerFrame) {
    PPHOTONIC_VIDEO_MODE mode = Stream->Mode;
    ULONG base = mode->Format7CsrOffset;
    ULONG frameBytes = Stream->ImageSize;
    ULONG packetPara;
    ULONG unitBpp;
    ULONG maxBpp;
    ULONG speedMaxBpp;
    ULONG rawBpp;
    ULONG bpp;
    ULONG ppf;
    ULONG divisorBelow;
    ULONG divisorAbove;
    ULONG candidate;
    NTSTATUS status;

    *BytesPerPacket = 0;
    *PacketsPerFrame = 0;

    if (frameBytes == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "F7 mode %u: negotiated image size is zero; cannot configure\n",
                    mode->Mode);
        return STATUS_INVALID_PARAMETER;
    }

    status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_FORMAT, (ULONG) DCAM_F7_FORMAT << DCAM_SELECT_SHIFT);
    if (NT_SUCCESS(status)) {
        status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_MODE, (ULONG) mode->Mode << DCAM_SELECT_SHIFT);
    }
    if (NT_SUCCESS(status)) {
        status = Photonic1394WriteRegister(Extension, base + DCAM_F7_IMAGE_SIZE,
                                           ((Stream->Width & 0xFFFF) << 16) | (Stream->Height & 0xFFFF));
    }
    if (NT_SUCCESS(status)) {
        status = Photonic1394WriteRegister(Extension, base + DCAM_F7_COLOR_CODING_ID, mode->PixelFormat << 24);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Read the packet-size bounds the camera now advertises for this geometry.
    //
    status = Photonic1394ReadRegister(Extension, base + DCAM_F7_PACKET_PARA_INQ, &packetPara);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    unitBpp = packetPara >> 16;   // UNIT_BYTE_PER_PACKET (high word)
    maxBpp = packetPara & 0xFFFF; // MAX_BYTE_PER_PACKET  (low word)
    //
    // A camera that advertises a zero unit or maximum is rejected outright rather
    // than patched up with a default.
    //
    if (unitBpp == 0 || maxBpp == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM,
                    "F7 mode %u advertises no usable packetisation (PACKET_PARA_INQ=0x%08x); cannot configure\n",
                    mode->Mode, packetPara);
        return STATUS_UNSUCCESSFUL;
    }

    //
    // The camera advertises the largest packet it can produce, which may assume a
    // faster bus than the one negotiated (the maximum here presumes S400, but a
    // link running at S200 carries at most 2048 bytes per packet). The bus driver
    // rejects a bandwidth allocation beyond the speed's payload limit with
    // STATUS_INVALID_PARAMETER, so cap the advertised maximum at that limit,
    // floored to a unit multiple. A unit larger than the limit leaves no legal
    // packet size at all: the mode cannot stream at this speed.
    //
    speedMaxBpp = Photonic1394MaxIsochPayload(Extension);
    if (maxBpp > speedMaxBpp) {
        maxBpp = (speedMaxBpp / unitBpp) * unitBpp;
        if (maxBpp == 0) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM,
                        "F7 mode %u unit %u exceeds the %s payload limit %u; mode unstreamable\n", mode->Mode, unitBpp,
                        Photonic1394SpeedName(Photonic1394StreamScode(Extension)), speedMaxBpp);
            return STATUS_INVALID_PARAMETER;
        }
    }

    //
    // Choose the packet size that streams this frame at the negotiated rate (one
    // packet per 125us isochronous cycle), then snap it to a legal value.
    //
    if (Stream->FrameInterval != 0) {
        ULONGLONG target = ((ULONGLONG) frameBytes * DCAM_ISO_CYCLE_100NS) / Stream->FrameInterval;

        //
        // Clamp in 64 bits before narrowing: a tiny interval against a large
        // frame pushes the quotient past ULONG range, and the size is capped
        // to the advertised maximum below anyway.
        //
        rawBpp = target > maxBpp ? maxBpp : (ULONG) target;
    } else {
        rawBpp = maxBpp;
    }
    //
    // BYTE_PER_PACKET must be an integer multiple of UNIT_BYTE_PER_PACKET: the
    // DCAM spec requires it and the camera NAKs any other value with a transaction
    // data-error (STATUS_DEVICE_DATA_ERROR). Floor the rate-derived size DOWN to a
    // unit multiple; flooring undershoots the requested rate rather than exceeding
    // it. When the result underflows to zero the requested rate is slower than the
    // geometry can go, so snap UP to one unit (the slowest legal rate) -- snapping
    // to the maximum here would flip a too-slow request into the fastest rate the
    // mode supports. A result above the advertised maximum clamps to that maximum.
    // Because the camera reports a quadlet-aligned unit, the result stays
    // quadlet-aligned as the OHCI iso-transmit path requires.
    //
    bpp = (rawBpp / unitBpp) * unitBpp;
    if (bpp == 0) {
        bpp = unitBpp;
    }
    if (bpp > maxBpp) {
        bpp = maxBpp;
    }

    //
    // The packet size must divide the frame exactly. The camera transmits
    // whole packets of bpp bytes, so a size that does not divide frameBytes
    // would make the host attach floor(frameBytes / bpp) packets and drop the
    // tail of every frame: the delivered byte count would silently fall short
    // of the negotiated image size. Search the legal unit multiples for the
    // divisor closest to the rate-derived target, preferring the largest one
    // at or below it (undershooting the requested rate rather than exceeding
    // it) and falling back to the smallest one above it. A geometry no legal
    // packet size divides cannot stream intact frames and is rejected rather
    // than truncated. The scan is bounded by maxBpp / unitBpp candidates.
    //
    divisorBelow = 0;
    divisorAbove = 0;
    for (candidate = unitBpp; candidate <= maxBpp; candidate += unitBpp) {
        if (frameBytes % candidate != 0) {
            continue;
        }
        if (candidate <= bpp) {
            divisorBelow = candidate; // rises to the largest divisor at or below the target
        } else {
            divisorAbove = candidate;
            break;
        }
    }
    if (divisorBelow != 0) {
        bpp = divisorBelow;
    } else if (divisorAbove != 0) {
        bpp = divisorAbove;
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM,
                    "F7 mode %u: no packet size in [%u..%u] step %u divides the %u-byte frame; mode rejected\n",
                    mode->Mode, unitBpp, maxBpp, unitBpp, frameBytes);
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Trace the full packet-size derivation: PACKET_PARA_INQ envelope, the bus
    // speed payload limit, the raw rate-derived size and the clamped/aligned
    // result.
    //
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM,
                "F7 mode %u %ux%u coding=%u interval=%u frameBytes=%u packetPara=0x%08x unit=%u max=%u speedMax=%u "
                "rawBpp=%u -> bpp=%u\n",
                mode->Mode, Stream->Width, Stream->Height, mode->PixelFormat, Stream->FrameInterval, frameBytes,
                packetPara, unitBpp, maxBpp, speedMaxBpp, rawBpp, bpp);

    ppf = frameBytes / bpp;
    if (ppf == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "packet size %u exceeds the %u-byte frame\n", bpp, frameBytes);
        return STATUS_INVALID_PARAMETER;
    }

    status = Photonic1394WriteRegister(Extension, base + DCAM_F7_BYTE_PER_PACKET, bpp << 16);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *BytesPerPacket = bpp;
    *PacketsPerFrame = ppf;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM,
                "F7 mode %u configured %ux%u coding=%u bpp=%u ppf=%u (%u bytes/frame)\n", mode->Mode, Stream->Width,
                Stream->Height, mode->PixelFormat, bpp, ppf, frameBytes);
    return STATUS_SUCCESS;
}

NTSTATUS PhotonicDcamConfigureStream(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                                     _Out_ PULONG BytesPerPacket, _Out_ PULONG PacketsPerFrame) {
    PPHOTONIC_VIDEO_MODE mode = Stream->Mode;
    const DCAM_PACKET_GEOMETRY *geom;
    ULONG rate;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_DCAM);

    *BytesPerPacket = 0;
    *PacketsPerFrame = 0;

    if (mode == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (mode->IsFormat7) {
        return PhotonicDcamConfigureFormat7(Extension, Stream, BytesPerPacket, PacketsPerFrame);
    }

    rate = PhotonicDcamSelectRate(mode->Format, mode->Mode, mode->RateMask, Stream->FrameInterval,
                                  Photonic1394MaxIsochPayload(Extension));
    if (rate >= DCAM_NUM_RATES) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "F%u/M%u has no streamable rate at %s\n", mode->Format,
                    mode->Mode, Photonic1394SpeedName(Photonic1394StreamScode(Extension)));
        return STATUS_INVALID_DEVICE_STATE;
    }
    Stream->RateId = rate;
    geom = &g_DcamPacketGeometry[mode->Format][mode->Mode][rate];

    //
    // Latch format, mode and rate. The camera derives its packetisation from
    // these; the host mirrors it from the table above so both agree on the frame
    // size and packets per frame.
    //
    status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_FORMAT, (ULONG) mode->Format << DCAM_SELECT_SHIFT);
    if (NT_SUCCESS(status)) {
        status = Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_MODE, (ULONG) mode->Mode << DCAM_SELECT_SHIFT);
    }
    if (NT_SUCCESS(status)) {
        status = Photonic1394WriteRegister(Extension, DCAM_REG_FRAME_RATE, rate << DCAM_SELECT_SHIFT);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *BytesPerPacket = geom->BytesPerPacket;
    *PacketsPerFrame = geom->PacketsPerFrame;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "F%u/M%u/R%u configured bpp=%u ppf=%u (%u bytes/frame)\n",
                mode->Format, mode->Mode, rate, geom->BytesPerPacket, geom->PacketsPerFrame,
                (ULONG) geom->BytesPerPacket * geom->PacketsPerFrame);
    return STATUS_SUCCESS;
}

/// PhotonicDcamReadSelection -- read the camera's live VIDEO_FORMAT /
/// VIDEO_MODE selection. Fails when the CSR base has not been discovered.
///
/// @param Extension  Device extension.
/// @param Format     Receives the current DCAM format id.
/// @param Mode       Receives the current DCAM mode id.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicDcamReadSelection(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG Format,
                                          _Out_ PULONG Mode) {
    ULONG value;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_DCAM);

    if (Extension->CsrBaseAddress == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "CSR base not discovered; camera bring-up failed\n");
        return STATUS_DEVICE_NOT_READY;
    }

    status = Photonic1394ReadRegister(Extension, DCAM_REG_VIDEO_FORMAT, &value);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *Format = value >> DCAM_SELECT_SHIFT;

    status = Photonic1394ReadRegister(Extension, DCAM_REG_VIDEO_MODE, &value);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *Mode = value >> DCAM_SELECT_SHIFT;

    return STATUS_SUCCESS;
}

NTSTATUS PhotonicDcamGetCurrentPixelFormat(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG PixelFormat) {
    ULONG format;
    ULONG mode;
    ULONG value;
    NTSTATUS status;
    ULONG i;

    FuncEntry(TRACE_FLAG_DCAM);

    *PixelFormat = PHOTONIC_DCAM_PIX_INVALID;

    status = PhotonicDcamReadSelection(Extension, &format, &mode);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    for (i = 0; i < Extension->ModeCount; i++) {
        PPHOTONIC_VIDEO_MODE entry = &Extension->Modes[i];

        if (entry->Format != format || entry->Mode != mode) {
            continue;
        }

        if (!entry->IsFormat7) {
            *PixelFormat = entry->PixelFormat;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "F%u/M%u -> coding %u\n", format, mode, *PixelFormat);
            return STATUS_SUCCESS;
        }

        //
        // Format 7: several table entries (one per colour coding) share this
        // (format, mode) pair, so the table cannot pin the coding. Read the
        // live COLOR_CODING_ID from the mode's CSR block instead.
        //
        status = Photonic1394ReadRegister(Extension, entry->Format7CsrOffset + DCAM_F7_COLOR_CODING_ID, &value);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        *PixelFormat = value >> 24;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "F7/M%u -> coding %u (COLOR_CODING_ID)\n", mode,
                    *PixelFormat);
        return STATUS_SUCCESS;
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "current selection F%u/M%u matches no enumerated mode\n", format,
                mode);
    return STATUS_NOT_FOUND;
}

/// PhotonicDcamGetCurrentMode -- read the camera's live VIDEO_FORMAT /
/// VIDEO_MODE selection and return the matching enumerated mode entry. For a
/// Format 7 selection the match is refined with the live COLOR_CODING_ID,
/// since several entries (one per colour coding) share one (format, mode) pair
/// and their frame sizes -- so their achievable rates -- differ; the first
/// matching entry is kept when the read fails or the camera's coding was not
/// enumerated. Returns STATUS_NOT_FOUND when the selection matches no
/// enumerated mode.
///
/// @param Extension  Device extension.
/// @param Mode       Receives a pointer to the matching enumerated mode entry.
/// @return STATUS_SUCCESS on success, STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamGetCurrentMode(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Outptr_ PPHOTONIC_VIDEO_MODE *Mode) {
    ULONG format;
    ULONG mode;
    NTSTATUS status;
    ULONG i;

    FuncEntry(TRACE_FLAG_DCAM);

    *Mode = NULL;

    status = PhotonicDcamReadSelection(Extension, &format, &mode);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    for (i = 0; i < Extension->ModeCount; i++) {
        PPHOTONIC_VIDEO_MODE entry = &Extension->Modes[i];

        if (entry->Format != format || entry->Mode != mode) {
            continue;
        }

        if (entry->IsFormat7) {
            ULONG value;

            status = Photonic1394ReadRegister(Extension, entry->Format7CsrOffset + DCAM_F7_COLOR_CODING_ID, &value);
            if (NT_SUCCESS(status)) {
                ULONG coding = value >> 24;
                ULONG j;

                for (j = i; j < Extension->ModeCount; j++) {
                    PPHOTONIC_VIDEO_MODE candidate = &Extension->Modes[j];

                    if (candidate->Format == format && candidate->Mode == mode && candidate->PixelFormat == coding) {
                        entry = candidate;
                        break;
                    }
                }
            }
        }

        *Mode = entry;
        return STATUS_SUCCESS;
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "current selection F%u/M%u matches no enumerated mode\n", format,
                mode);
    return STATUS_NOT_FOUND;
}

/// PhotonicDcamGetCurrentSubwindow -- derive the camera's current ROI in
/// full-resolution sensor coordinates from the live VIDEO_FORMAT / VIDEO_MODE
/// selection. Scale is the decimation factor relative to the full-resolution
/// mode (the largest enumerated default width); every geometry field is
/// multiplied by it so offsets and size are expressed in full-sensor units. A
/// standard fixed mode reports its default full frame at offset 0,0; a Format
/// 7 mode reports the live IMAGE_POSITION / IMAGE_SIZE from its CSR block,
/// since the capture path may have reprogrammed them after enumeration.
///
/// @param Extension  Device extension.
/// @param Scale      Receives the decimation factor relative to the full-resolution mode.
/// @param OffsetX    Receives the horizontal ROI offset in full-sensor units.
/// @param OffsetY    Receives the vertical ROI offset in full-sensor units.
/// @param Width      Receives the ROI width in full-sensor units.
/// @param Height     Receives the ROI height in full-sensor units.
/// @return STATUS_SUCCESS on success, STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamGetCurrentSubwindow(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG Scale,
                                         _Out_ PULONG OffsetX, _Out_ PULONG OffsetY, _Out_ PULONG Width,
                                         _Out_ PULONG Height) {
    PPHOTONIC_VIDEO_MODE mode;
    ULONG fullWidth = 0;
    ULONG offsetX = 0;
    ULONG offsetY = 0;
    ULONG width;
    ULONG height;
    ULONG scale;
    NTSTATUS status;
    ULONG i;

    FuncEntry(TRACE_FLAG_DCAM);

    status = PhotonicDcamGetCurrentMode(Extension, &mode);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // The decimation factor is relative to the widest enumerated mode: the
    // full-resolution mode reports scale 1, a 2x2-binned mode scale 2, etc.
    //
    for (i = 0; i < Extension->ModeCount; i++) {
        if (Extension->Modes[i].DefaultWidth > fullWidth) {
            fullWidth = Extension->Modes[i].DefaultWidth;
        }
    }
    scale = fullWidth / mode->DefaultWidth;

    if (mode->IsFormat7) {
        //
        // Read the live window: the capture path may have reprogrammed the
        // mode's CSR block after enumeration.
        //
        ULONG position;
        ULONG size;

        status = Photonic1394ReadRegister(Extension, mode->Format7CsrOffset + DCAM_F7_IMAGE_POSITION, &position);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        status = Photonic1394ReadRegister(Extension, mode->Format7CsrOffset + DCAM_F7_IMAGE_SIZE, &size);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        offsetX = position >> 16;
        offsetY = position & 0xFFFF;
        width = size >> 16;
        height = size & 0xFFFF;
    } else {
        width = mode->DefaultWidth;
        height = mode->DefaultHeight;
    }

    *Scale = scale;
    *OffsetX = offsetX * scale;
    *OffsetY = offsetY * scale;
    *Width = width * scale;
    *Height = height * scale;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "F%u/M%u scale=%u size=%ux%u offset=%u,%u\n", mode->Format,
                mode->Mode, *Scale, *Width, *Height, *OffsetX, *OffsetY);
    return STATUS_SUCCESS;
}

/// PhotonicDcamRateFromIndex -- map a zero-based enumeration index to the DCAM
/// rate id of the Index-th rate set in a FRAME_RATE_INQ mask (ascending rate
/// id, slowest first). Returns DCAM_NUM_RATES when the index is past the last
/// supported rate.
///
/// @param RateMask  FRAME_RATE_INQ bitmask (bit 31-rate set per supported rate).
/// @param Index     Zero-based position into the supported-rate list (slowest first).
/// @return DCAM rate id, or DCAM_NUM_RATES when the index is past the last rate.
static ULONG PhotonicDcamRateFromIndex(_In_ ULONG RateMask, _In_ ULONG Index) {
    ULONG count = 0;
    ULONG rate;

    for (rate = 0; rate < DCAM_NUM_RATES; rate++) {
        if ((RateMask & (0x80000000UL >> rate)) == 0) {
            continue;
        }
        if (count == Index) {
            return rate;
        }
        count++;
    }

    return DCAM_NUM_RATES;
}

NTSTATUS PhotonicDcamSelectPixelFormat(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG PixelFormat) {
    NTSTATUS status;
    ULONG i;

    FuncEntry(TRACE_FLAG_DCAM);

    if (Extension->CsrBaseAddress == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "CSR base not discovered; camera bring-up failed\n");
        return STATUS_DEVICE_NOT_READY;
    }

    for (i = 0; i < Extension->ModeCount; i++) {
        PPHOTONIC_VIDEO_MODE entry = &Extension->Modes[i];

        if (entry->PixelFormat != PixelFormat) {
            continue;
        }

        status =
            Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_FORMAT, (ULONG) entry->Format << DCAM_SELECT_SHIFT);
        if (NT_SUCCESS(status)) {
            status =
                Photonic1394WriteRegister(Extension, DCAM_REG_VIDEO_MODE, (ULONG) entry->Mode << DCAM_SELECT_SHIFT);
        }
        if (NT_SUCCESS(status) && entry->IsFormat7) {
            //
            // Several Format 7 codings share this (format, mode) pair; latch
            // the requested one. The ROI (IMAGE_SIZE) is deliberately left as
            // the camera has it.
            //
            status = Photonic1394WriteRegister(Extension, entry->Format7CsrOffset + DCAM_F7_COLOR_CODING_ID,
                                               entry->PixelFormat << 24);
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "coding %u -> F%u/M%u f7=%u\n", PixelFormat,
                    entry->Format, entry->Mode, entry->IsFormat7);
        return STATUS_SUCCESS;
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "no enumerated mode has coding %u\n", PixelFormat);
    return STATUS_NOT_FOUND;
}

NTSTATUS PhotonicDcamEnumFrameRate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Index, _Out_ PULONG Interval) {
    PPHOTONIC_VIDEO_MODE entry;
    ULONG rate;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_DCAM);

    *Interval = 0;

    status = PhotonicDcamGetCurrentMode(Extension, &entry);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (entry->IsFormat7) {
        //
        // Format 7 has no discrete rate table: the rate follows from the
        // packet size. Enumerate a single entry, the fastest rate the mode's
        // packet parameters allow.
        //
        if (Index == 0 && entry->MinFrameInterval != 0) {
            *Interval = entry->MinFrameInterval;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "F7/M%u index 0 -> interval %u\n", entry->Mode,
                        *Interval);
            return STATUS_SUCCESS;
        }
        return STATUS_NO_MORE_ENTRIES;
    }

    rate = PhotonicDcamRateFromIndex(entry->RateMask, Index);
    if (rate >= DCAM_NUM_RATES) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "F%u/M%u index %u exhausted\n", entry->Format,
                    entry->Mode, Index);
        return STATUS_NO_MORE_ENTRIES;
    }

    *Interval = g_DcamRateInterval[rate];
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "F%u/M%u index %u -> rate %u interval %u\n", entry->Format,
                entry->Mode, Index, rate, *Interval);
    return STATUS_SUCCESS;
}

NTSTATUS PhotonicDcamSetFrameRate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Index) {
    PPHOTONIC_VIDEO_MODE entry;
    ULONG rate;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_DCAM);

    status = PhotonicDcamGetCurrentMode(Extension, &entry);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (entry->IsFormat7) {
        //
        // Format 7 has no discrete rate table and no rate register to
        // program: only index 0 -- the single rate ENUM_FRAME_RATE reports --
        // is accepted, as a no-op.
        //
        if (Index != 0 || entry->MinFrameInterval == 0) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F7/M%u has no rate at index %u\n", entry->Mode, Index);
            return STATUS_NO_MORE_ENTRIES;
        }
        return STATUS_SUCCESS;
    }

    rate = PhotonicDcamRateFromIndex(entry->RateMask, Index);
    if (rate >= DCAM_NUM_RATES) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DCAM, "F%u/M%u has no rate at index %u\n", entry->Format,
                    entry->Mode, Index);
        return STATUS_NO_MORE_ENTRIES;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "index %u -> rate %u\n", Index, rate);
    return Photonic1394WriteRegister(Extension, DCAM_REG_FRAME_RATE, rate << DCAM_SELECT_SHIFT);
}

NTSTATUS PhotonicDcamGetFrameRate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG Interval) {
    PPHOTONIC_VIDEO_MODE entry;
    ULONG value;
    ULONG rate;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_DCAM);

    *Interval = 0;

    status = PhotonicDcamGetCurrentMode(Extension, &entry);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (entry->IsFormat7) {
        //
        // Format 7 has no rate register; report the fastest rate the mode's
        // packet parameters allow, matching what ENUM_FRAME_RATE enumerates.
        //
        if (entry->MinFrameInterval == 0) {
            return STATUS_UNSUCCESSFUL;
        }
        *Interval = entry->MinFrameInterval;
        return STATUS_SUCCESS;
    }

    status = Photonic1394ReadRegister(Extension, DCAM_REG_FRAME_RATE, &value);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    rate = value >> DCAM_SELECT_SHIFT;
    if (rate >= DCAM_NUM_RATES) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM, "camera reports invalid rate id %u\n", rate);
        return STATUS_DEVICE_DATA_ERROR;
    }

    *Interval = g_DcamRateInterval[rate];
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "rate %u interval %u\n", rate, *Interval);
    return STATUS_SUCCESS;
}

NTSTATUS PhotonicDcamSetIsochChannel(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Channel,
                                     _In_ ULONG SpeedCode) {
    ULONG value = (Channel << DCAM_ISOCH_CHANNEL_SHIFT) | (SpeedCode << DCAM_ISOCH_SPEED_SHIFT);

    FuncEntry(TRACE_FLAG_DCAM);

    //
    // The register's channel field is 4 bits (31:28), but the bus driver can
    // allocate any of the 64 IEEE 1394 channels. A channel above 15 would
    // silently wrap to channel mod 16: the host listens on the real channel
    // while the camera transmits elsewhere, a zero-frame stall with no error
    // anywhere. Fail the start cleanly instead, so the caller releases its
    // resources and reports. The speed field takes codes 0 to 2
    // (S100/S200/S400), guaranteed by Photonic1394StreamScode today and
    // checked here so no future caller can corrupt the neighboring bits.
    //
    if (Channel > 15 || SpeedCode > 2) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DCAM,
                    "channel %u or speed code %u does not fit the ISOCH_CHANNEL register fields\n", Channel, SpeedCode);
        return STATUS_INVALID_PARAMETER;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "ISOCH_CHANNEL channel=%u speed=%s\n", Channel,
                Photonic1394SpeedName((UCHAR) SpeedCode));
    return Photonic1394WriteRegister(Extension, DCAM_REG_ISOCH_CHANNEL, value);
}

NTSTATUS PhotonicDcamSetIsochEnable(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ BOOLEAN Enable) {
    FuncEntry(TRACE_FLAG_DCAM);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "ISO_EN %s\n", Enable ? "set (start)" : "clear (stop)");
    return Photonic1394WriteRegister(Extension, DCAM_REG_ISO_EN, Enable ? DCAM_ISO_EN_BIT : 0);
}

NTSTATUS PhotonicDcamOneShot(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ BOOLEAN Arm) {
    FuncEntry(TRACE_FLAG_DCAM);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DCAM, "ONE_SHOT %s\n", Arm ? "arm" : "cancel");
    return Photonic1394WriteRegister(Extension, DCAM_REG_ONE_SHOT, Arm ? DCAM_ONE_SHOT_BIT : 0);
}
