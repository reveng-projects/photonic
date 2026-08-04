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
/// The frame descriptor pool: per-descriptor reusable IRPs, the frame
/// chunk geometry with its partial MDLs, and the quarantine scans over
/// the pool. See capture.h.

// clang-format off
#include "capture.h"
#include "capture_private.h"
#include "pool.tmh"
// clang-format on

/// PhotonicCapturePoolFree -- free the per-descriptor reusable IRPs. Called only
/// once no descriptor IRP can be at the bus driver (create failure, or after the
/// stop drain).
///
/// @param Capture  Capture engine whose descriptor IRPs are freed.
VOID PhotonicCapturePoolFree(_In_ PPHOTONIC_CAPTURE Capture) {
    ULONG i;

    FuncEntry(TRACE_FLAG_ISO);

    for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
        if (Capture->Frames[i].Irp != NULL) {
            IoFreeIrp(Capture->Frames[i].Irp);
            Capture->Frames[i].Irp = NULL;
        }
    }
}

/// PhotonicCapturePoolAllocate -- build the N frame descriptors (each with a
/// pre-allocated reusable IRP) and place them all on the free list.
///
/// @param Capture  Capture engine whose descriptor pool is built.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicCapturePoolAllocate(_In_ PPHOTONIC_CAPTURE Capture) {
    CCHAR stackSize = Capture->Extension->PhysicalDeviceObject->StackSize;
    ULONG i;

    FuncEntry(TRACE_FLAG_ISO);

    for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
        PPHOTONIC_CAPTURE_FRAME desc = &Capture->Frames[i];

        desc->Capture = Capture;
        desc->Cookie = 0;
        desc->State = PhotonicFrameFree;
        desc->CompletionStatus = STATUS_SUCCESS;
        desc->Irp = IoAllocateIrp(stackSize, FALSE);
        if (desc->Irp == NULL) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "failed to allocate descriptor IRP %u\n", i);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        InsertTailList(&Capture->FreeList, &desc->ListEntry);
    }
    return STATUS_SUCCESS;
}

/// PhotonicCapturePoolFreeChunkMdls -- free the per-chunk partial MDLs of a multi-chunk
/// session. Called only once no descriptor is at the bus driver (start failure, or
/// after the stop drain); a partial MDL the bus/port driver mapped is unmapped
/// first. Idempotent; a no-op for single-chunk sessions.
///
/// @param Capture  Capture engine whose per-chunk partial MDLs are freed.
VOID PhotonicCapturePoolFreeChunkMdls(_In_ PPHOTONIC_CAPTURE Capture) {
    ULONG i;
    ULONG chunk;

    FuncEntry(TRACE_FLAG_ISO);

    for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
        for (chunk = 0; chunk < PHOTONIC_CAPTURE_MAX_CHUNKS; chunk++) {
            PMDL mdl = Capture->Frames[i].ChunkMdls[chunk];

            if (mdl != NULL) {
                MmPrepareMdlForReuse(mdl);
                IoFreeMdl(mdl);
                Capture->Frames[i].ChunkMdls[chunk] = NULL;
            }
        }
    }
}

/// PhotonicCapturePoolSetupChunks -- derive how each frame is presented to the bus
/// driver: whole frame in one buffer when it fits the host's per-buffer DMA limit
/// (Extension->MaxDmaBufferSize, queried at bring-up;
/// PHOTONIC_CAPTURE_MAX_CHUNK_BYTES when the query failed), otherwise chunks of
/// whole packets attached as one multi-descriptor request. A host without a
/// meaningful limit therefore never chunks. A multi-chunk geometry needs one
/// partial MDL per descriptor per chunk;
/// all are pre-allocated here, sized for the worst-case page alignment of a
/// chunk-sized range (chunk boundaries are packet multiples, not page multiples),
/// so the submit path stays allocation-free at DISPATCH_LEVEL. Must run at
/// PASSIVE_LEVEL, before the chunk geometry is consumed by resource allocation or
/// attach.
///
/// @param Capture  Capture engine whose chunk geometry and partial MDLs are set up.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicCapturePoolSetupChunks(_In_ PPHOTONIC_CAPTURE Capture) {
    ULONG64 maxBufferBytes = Capture->Extension->MaxDmaBufferSize;
    ULONG chunkBytes = Capture->FrameBytes;
    ULONG chunkCount = 1;
    ULONG i;

    if (maxBufferBytes == 0) {
        maxBufferBytes = PHOTONIC_CAPTURE_MAX_CHUNK_BYTES;
    } else if (maxBufferBytes > PAGE_SIZE) {
        //
        // The bus driver validates nMaxBufferSize at ISOCH_ALLOCATE_RESOURCES time,
        // before any buffer exists, so it must assume the least favourable buffer
        // alignment: a buffer that fills the reported limit exactly but starts
        // mid-page spans one page more than the limit allows, and the request fails
        // with STATUS_INSUFFICIENT_RESOURCES (observed: a 2,095,880-byte chunk is
        // refused against a reported 2,097,152-byte MaxDmaBufferSize). Chunk
        // boundaries are packet multiples, not page multiples, so the mid-page start
        // is the norm; keep one page of headroom under the reported limit.
        //
        maxBufferBytes -= PAGE_SIZE;
    }

    if (chunkBytes > maxBufferBytes) {
        chunkBytes = (ULONG) maxBufferBytes / Capture->BytesPerPacket * Capture->BytesPerPacket;
        if (chunkBytes == 0) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "packet size %u exceeds the %I64u-byte buffer limit\n",
                        Capture->BytesPerPacket, maxBufferBytes);
            return STATUS_INVALID_PARAMETER;
        }
        chunkCount = (Capture->FrameBytes + chunkBytes - 1) / chunkBytes;
        if (chunkCount > PHOTONIC_CAPTURE_MAX_CHUNKS) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "frame of %u bytes needs %u chunks (max %u)\n",
                        Capture->FrameBytes, chunkCount, PHOTONIC_CAPTURE_MAX_CHUNKS);
            return STATUS_INVALID_PARAMETER;
        }
    }
    Capture->ChunkBytes = chunkBytes;
    Capture->ChunkCount = chunkCount;

    //
    // No descriptor is at the bus driver here (the engine is settled), so any
    // chunk MDLs left over from an earlier session can be freed safely. The
    // start path releases them before this runs, and freeing here as well
    // keeps a stale slot from being overwritten and leaked.
    //
    PhotonicCapturePoolFreeChunkMdls(Capture);

    if (chunkCount > 1) {
        for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
            ULONG chunk;

            for (chunk = 0; chunk < chunkCount; chunk++) {
                Capture->Frames[i].ChunkMdls[chunk] =
                    IoAllocateMdl((PVOID) (ULONG_PTR) (PAGE_SIZE - 1), chunkBytes, FALSE, FALSE, NULL);
                if (Capture->Frames[i].ChunkMdls[chunk] == NULL) {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISO, "failed to allocate chunk MDL %u of descriptor %u\n",
                                chunk, i);
                    PhotonicCapturePoolFreeChunkMdls(Capture);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
            }
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ISO,
                "frame geometry: %u bytes in %u chunk(s) of <= %u bytes (host buffer limit %I64u)\n",
                Capture->FrameBytes, chunkCount, chunkBytes, maxBufferBytes);
    return STATUS_SUCCESS;
}

BOOLEAN PhotonicCaptureHasQuarantinedFrames(_In_ PPHOTONIC_CAPTURE Capture) {
    ULONG i;

    for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
        if (Capture->Frames[i].State == PhotonicFrameDetachFailed) {
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN PhotonicCaptureCookieQuarantined(_In_ PPHOTONIC_CAPTURE Capture, _In_ ULONG_PTR Cookie) {
    ULONG i;

    for (i = 0; i < PHOTONIC_CAPTURE_FRAME_COUNT; i++) {
        if (Capture->Frames[i].State == PhotonicFrameDetachFailed && Capture->Frames[i].Cookie == Cookie) {
            return TRUE;
        }
    }
    return FALSE;
}
