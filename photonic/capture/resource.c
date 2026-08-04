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
/// 1394 isochronous resource lifecycle: allocation of the channel,
/// bandwidth and resource handle at start, and their best-effort release
/// at stop. See capture.h.

// clang-format off
#include "capture.h"
#include "p1394.h"
#include "capture_private.h"
#include "resource.tmh"
// clang-format on

/// PhotonicCaptureResourceAllocateChannel -- allocate the isochronous channel,
/// a single synchronous IRB to the 1394 bus driver.
///
/// @param Capture  Capture engine to receive the allocated channel number.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicCaptureResourceAllocateChannel(_In_ PPHOTONIC_CAPTURE Capture) {
    IRB irb;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_ISO);

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_ISOCH_ALLOCATE_CHANNEL;
    irb.u.IsochAllocateChannel.nRequestedChannel = ISOCH_ANY_CHANNEL;
    status = PhotonicSubmitIrb(Capture->Extension, &irb);
    if (NT_SUCCESS(status)) {
        Capture->Channel = irb.u.IsochAllocateChannel.Channel;
        Capture->ChannelAllocated = TRUE;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO, "allocated isoch channel %u\n", Capture->Channel);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "ISOCH_ALLOCATE_CHANNEL failed: %!STATUS!\n", status);
    }
    return status;
}

/// PhotonicCaptureResourceAllocateBandwidth -- allocate the isochronous
/// bandwidth, a single synchronous IRB to the 1394 bus driver.
///
/// @param Capture     Capture engine to receive the allocated bandwidth handle.
/// @param SpeedFlags  Speed flags for the bandwidth request (1 << scode).
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicCaptureResourceAllocateBandwidth(_In_ PPHOTONIC_CAPTURE Capture, _In_ ULONG SpeedFlags) {
    IRB irb;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_ISO);

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_ISOCH_ALLOCATE_BANDWIDTH;
    irb.u.IsochAllocateBandwidth.nMaxBytesPerFrameRequested = Capture->BytesPerPacket;
    irb.u.IsochAllocateBandwidth.fulSpeed = SpeedFlags;
    status = PhotonicSubmitIrb(Capture->Extension, &irb);
    if (NT_SUCCESS(status)) {
        Capture->BandwidthHandle = irb.u.IsochAllocateBandwidth.hBandwidth;
        Capture->BandwidthAllocated = TRUE;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO, "allocated isoch bandwidth (%u bytes/cycle)\n",
                    Capture->BytesPerPacket);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "ISOCH_ALLOCATE_BANDWIDTH failed: %!STATUS!\n", status);
    }
    return status;
}

/// PhotonicCaptureResourceAllocateHandle -- allocate the isochronous resource handle for
/// the framework-owned (zero-copy) model. Unlike a self-sustaining circular ring,
/// the resource is NON-circular: buffers are attached lazily, one per read, filled
/// once and detached. Per the design's host-capability table the resource flags are
/// derived from Extension->HostCapabilities:
///   - RESOURCE_USED_IN_LISTENING always (this endpoint receives);
///   - RESOURCE_STRIP_ADDITIONAL_QUADLETS (strip the single isochronous header
///     quadlet) when the host controller can strip / return the iso header, so each
///     buffer holds only frame payload; otherwise no strip;
///   - RESOURCE_USE_PACKET_BASED when the host is not stream-based.
/// The buffer unit is one chunk (ChunkBytes; the whole frame unless it exceeds the
/// bus driver's per-buffer limit -- see PhotonicCapturePoolSetupChunks), so the
/// buffer count is N + 1 frames (the extra slot the non-circular pool needs) times
/// the chunks per frame; the actual per-frame MDLs are attached later by
/// the pump.
///
/// @param Capture     Capture engine to receive the allocated resource handle.
/// @param SpeedFlags  Speed flags for the resource request (1 << scode).
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicCaptureResourceAllocateHandle(_In_ PPHOTONIC_CAPTURE Capture, _In_ ULONG SpeedFlags) {
    IRB irb;
    PPHOTONIC_DEVICE_EXTENSION extension = Capture->Extension;
    ULONG buffers = (PHOTONIC_CAPTURE_FRAME_COUNT + 1) * Capture->ChunkCount;
    ULONG flags = RESOURCE_USED_IN_LISTENING;
    ULONG quadletsToStrip = 0;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_ISO);

    //
    // Strip the header quadlet only if the host controller supports it. The design
    // phrases this as "the host can return the iso packet header"; accept either the
    // stripping or the header-return capability so a controller that reports only one
    // still strips (the previous fixed-ring code stripped unconditionally).
    //
    if (extension->HostCapabilities & (HOST_INFO_SUPPORTS_ISOCH_STRIPPING | HOST_INFO_SUPPORTS_RETURNING_ISO_HDR)) {
        flags |= RESOURCE_STRIP_ADDITIONAL_QUADLETS;
        quadletsToStrip = 1;
    }
    if (!(extension->HostCapabilities & HOST_INFO_STREAM_BASED)) {
        flags |= RESOURCE_USE_PACKET_BASED;
    }

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_ISOCH_ALLOCATE_RESOURCES;
    irb.u.IsochAllocateResources.fulSpeed = SpeedFlags;
    irb.u.IsochAllocateResources.fulFlags = flags;
    irb.u.IsochAllocateResources.nChannel = Capture->Channel;
    irb.u.IsochAllocateResources.nMaxBytesPerFrame = Capture->BytesPerPacket;
    irb.u.IsochAllocateResources.nNumberOfBuffers = buffers;
    irb.u.IsochAllocateResources.nMaxBufferSize = Capture->ChunkBytes;
    irb.u.IsochAllocateResources.nQuadletsToStrip = quadletsToStrip;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO,
                "ISOCH_ALLOCATE_RESOURCES request: channel=%u speed=%s flags=0x%x hostCaps=0x%x maxBytesPerFrame=%u "
                "buffers=%u maxBufferSize=%u quadletsToStrip=%u (chunks/frame=%u packets/buffer=%u)\n",
                Capture->Channel, Photonic1394SpeedName(Photonic1394StreamScode(extension)), flags,
                extension->HostCapabilities, Capture->BytesPerPacket, buffers, Capture->ChunkBytes, quadletsToStrip,
                Capture->ChunkCount, Capture->BytesPerPacket != 0 ? Capture->ChunkBytes / Capture->BytesPerPacket : 0);
    status = PhotonicSubmitIrb(Capture->Extension, &irb);
    if (NT_SUCCESS(status)) {
        Capture->ResourceHandle = irb.u.IsochAllocateResources.hResource;
        Capture->ResourceAllocated = TRUE;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO, "allocated isoch resources (%u buffers x %u bytes)\n",
                    buffers, Capture->ChunkBytes);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "ISOCH_ALLOCATE_RESOURCES failed: %!STATUS!\n", status);
    }
    return status;
}

/// PhotonicCaptureResourceIsochStop -- synchronous ISOCH_STOP. The bus driver fires no
/// further frame-complete callbacks after it returns. Must run at PASSIVE_LEVEL.
///
/// @param Capture  Capture engine whose isochronous context is stopped.
VOID PhotonicCaptureResourceIsochStop(_In_ PPHOTONIC_CAPTURE Capture) {
    IRB irb;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_ISO);

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_ISOCH_STOP;
    irb.u.IsochStop.hResource = Capture->ResourceHandle;
    irb.u.IsochStop.fulFlags = 0;
    (VOID) PhotonicSubmitIrb(Capture->Extension, &irb);

    //
    // Listening is read at DISPATCH_LEVEL under Lock (the attach completion
    // decides whether to queue a listen), so the clear takes it too.
    //
    KeAcquireSpinLock(&Capture->Lock, &irql);
    Capture->Listening = FALSE;
    KeReleaseSpinLock(&Capture->Lock, irql);
}

/// PhotonicCaptureResourceFreeOne -- submit one resource-release IRB and decide
/// whether its tracking flag may be cleared. The flag stays set only when the
/// request never reached the bus driver (pool exhaustion, or device teardown
/// already begun), so the next stop or start retries the release instead of
/// stranding the bus-global resource until device removal. Once the bus
/// driver has seen the request the flag is cleared whatever the returned
/// status: re-issuing a free for a handle the bus driver may already have
/// released is not safe.
///
/// @param Capture     Capture engine releasing the resource.
/// @param Irb         Prepared REQUEST_ISOCH_FREE_* IRB.
/// @param What        Resource name for the trace.
/// @param FreeStatus  Receives the status of the free request itself.
/// @return TRUE when the tracking flag may be cleared, FALSE to keep it set.
static BOOLEAN PhotonicCaptureResourceFreeOne(_In_ PPHOTONIC_CAPTURE Capture, _Inout_ PIRB Irb, _In_ PCSTR What,
                                              _Out_ PNTSTATUS FreeStatus) {
    BOOLEAN submitted;
    NTSTATUS status;

    status = PhotonicSubmitIrbTracked(Capture->Extension, Irb, &submitted);
    *FreeStatus = status;
    if (!submitted) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO,
                    "%s free not submitted (%!STATUS!); retried at the next stop or start\n", What, status);
        return FALSE;
    }
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ISO, "%s free failed: %!STATUS!\n", What, status);
    }
    return TRUE;
}

/// PhotonicCaptureResourceReleaseIsoch -- best-effort release of the isochronous channel,
/// bandwidth and resource handle acquired at start, in reverse order. Buffers are
/// detached separately (per descriptor) before this runs. Each step is gated on its
/// own flag so a partially-started engine tears down cleanly, and a flag stays set
/// when its free request never reached the bus driver, so a later stop or start
/// retries it.
///
/// The return value says whether this call's resource-handle free completed
/// with success -- the only event that provably flushes the handle's leftover
/// DMA mappings, and so the only license to deliver quarantined reads. A free
/// that failed, was abandoned, or never reached the bus driver returns FALSE;
/// so does a call that found nothing to free, because the free that cleared
/// the flag earlier either succeeded (no quarantined read survived it) or
/// must never be re-issued (the reads stay held for good).
///
/// @param Capture  Capture engine whose isochronous resources are released.
/// @return TRUE when the resource-handle free completed with success.
BOOLEAN PhotonicCaptureResourceReleaseIsoch(_In_ PPHOTONIC_CAPTURE Capture) {
    IRB irb;
    NTSTATUS freeStatus;
    BOOLEAN flushed = FALSE;

    FuncEntry(TRACE_FLAG_ISO);

    if (Capture->Listening) {
        PhotonicCaptureResourceIsochStop(Capture);
    }

    if (Capture->ResourceAllocated) {
        RtlZeroMemory(&irb, sizeof(irb));
        irb.FunctionNumber = REQUEST_ISOCH_FREE_RESOURCES;
        irb.u.IsochFreeResources.hResource = Capture->ResourceHandle;
        if (PhotonicCaptureResourceFreeOne(Capture, &irb, "isoch resource handle", &freeStatus)) {
            Capture->ResourceAllocated = FALSE;
            flushed = NT_SUCCESS(freeStatus);
        }
    }

    if (Capture->BandwidthAllocated) {
        RtlZeroMemory(&irb, sizeof(irb));
        irb.FunctionNumber = REQUEST_ISOCH_FREE_BANDWIDTH;
        irb.u.IsochFreeBandwidth.hBandwidth = Capture->BandwidthHandle;
        if (PhotonicCaptureResourceFreeOne(Capture, &irb, "isoch bandwidth", &freeStatus)) {
            Capture->BandwidthAllocated = FALSE;
        }
    }

    if (Capture->ChannelAllocated) {
        RtlZeroMemory(&irb, sizeof(irb));
        irb.FunctionNumber = REQUEST_ISOCH_FREE_CHANNEL;
        irb.u.IsochFreeChannel.nChannel = Capture->Channel;
        if (PhotonicCaptureResourceFreeOne(Capture, &irb, "isoch channel", &freeStatus)) {
            Capture->ChannelAllocated = FALSE;
        }
    }

    return flushed;
}
