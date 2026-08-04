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
/// Capture pin lifecycle for the Photonic stream minidriver: the
/// SRB_OPEN_STREAM handler that wires up the per-stream callbacks and
/// recovers the DCAM mode behind the negotiated connection format, and the
/// SRB_CLOSE_STREAM handler that runs the data-path rundown and tears the
/// capture engine down.

// clang-format off
// lifecycle.tmh (WPP-generated) must come last.
#include "photonic.h"
#include "capture.h"
#include "stream_private.h"
#include "lifecycle.tmh"
// clang-format on

/// PhotonicStreamFindNegotiatedMode -- recover the enumerated DCAM mode behind the
/// connection format the pin opened with. Matches the negotiated coding (bit depth
/// and biCompression) against the advertised ranges and, for the matching coding,
/// checks the negotiated resolution falls inside the range's output-size window
/// (an exact size for a standard format, a scalable window for Format 7).
///
/// Several ranges can cover the same resolution: a large Format 7 mode's scalable
/// window spans the sizes of the smaller modes, and it precedes them in the range
/// table. Taking the first size match would stream e.g. a 320x240 connection as a
/// window into the 1572x1052 F7 mode, whose interval envelope cannot honour the
/// negotiated rate. So among the covering ranges, prefer one whose default
/// resolution equals the negotiated size and whose interval window contains the
/// negotiated frame interval, then one that merely supports the interval, before
/// settling for the first size match. PhotonicStreamIntersectFindRange shares
/// the size and interval predicates but ladders them differently (it has a
/// ConfigCaps window-identity tier this lookup lacks, and lacks this lookup's
/// interval-only tier); the two are deliberately not merged. Returns a stable
/// pointer into Extension->Modes, or NULL when nothing matches.
///
/// @param Extension  Device extension holding the advertised range and mode tables.
/// @param Stream     Stream extension carrying the negotiated format fields.
/// @return Pointer to the matching enumerated mode on success, NULL on failure.
static PPHOTONIC_VIDEO_MODE PhotonicStreamFindNegotiatedMode(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                                                             _In_ PPHOTONIC_STREAM_EXTENSION Stream) {
    PPHOTONIC_VIDEO_MODE intervalMatch = NULL;
    PPHOTONIC_VIDEO_MODE sizeMatch = NULL;
    ULONG i;

    for (i = 0; i < Extension->FormatCount; i++) {
        PKS_VIDEOINFOHEADER header = &Extension->VideoRanges[i].VideoInfoHeader;
        PKS_DATARANGE_VIDEO range = &Extension->VideoRanges[i];
        PPHOTONIC_VIDEO_MODE mode;
        BOOLEAN intervalOk;

        if ((ULONG) header->bmiHeader.biBitCount != Stream->BitCount ||
            header->bmiHeader.biCompression != Stream->Compression) {
            continue;
        }
        if ((LONG) Stream->Width < range->ConfigCaps.MinOutputSize.cx ||
            (LONG) Stream->Width > range->ConfigCaps.MaxOutputSize.cx ||
            (LONG) Stream->Height < range->ConfigCaps.MinOutputSize.cy ||
            (LONG) Stream->Height > range->ConfigCaps.MaxOutputSize.cy) {
            continue;
        }

        mode = &Extension->Modes[Extension->RangeModeIndex[i]];
        intervalOk = PhotonicStreamRangeIntervalOk(range, (LONGLONG) Stream->FrameInterval);

        if (intervalOk && PhotonicStreamRangeDefaultSizeIs(range, (LONG) Stream->Width, (LONG) Stream->Height)) {
            return mode;
        }
        if (intervalOk && intervalMatch == NULL) {
            intervalMatch = mode;
        }
        if (sizeMatch == NULL) {
            sizeMatch = mode;
        }
    }

    return intervalMatch != NULL ? intervalMatch : sizeMatch;
}

/// SRB_OPEN_STREAM -- a client (the KS proxy / DirectShow) is connecting to the
/// capture pin. Wire up the per-stream callbacks and remember the stream object.
///
/// @param Srb  Stream request block for SRB_OPEN_STREAM.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicStreamOpen(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PHW_STREAM_OBJECT stream = Srb->StreamObject;
    PPHOTONIC_STREAM_EXTENSION streamExt;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_STREAM);

    if (stream->StreamNumber != PHOTONIC_VIDEO_STREAM) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM, "bad stream number %u\n", stream->StreamNumber);
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Hold the interface mutex from the ownership check through the publish
    // at the end. The device has one capture session whichever interface
    // owns it: an open stream and a prepared IOCTL slot both publish
    // ActiveCaptureStream, so this single check gates a second open and the
    // IOCTL interface alike, and the IOCTL prepare performs the mirror check
    // under the same mutex. Without the mutex both checks could pass before
    // either publish and both interfaces would stream at once. Device SRBs
    // run at PASSIVE_LEVEL, so the wait is legal.
    //
    KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);

    //
    // An open racing past a surprise removal would succeed, allocate an
    // engine and publish a stream that can never run. Fail it fast instead,
    // like the read and start paths do.
    //
    if (extension->Removed) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "device removed; open rejected\n");
        KeReleaseMutex(&extension->InterfaceMutex, FALSE);
        return STATUS_DEVICE_REMOVED;
    }

    if (extension->ActiveCaptureStream != NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "%s; open rejected\n",
                    extension->IoctlCapture != NULL ? "IOCTL capture is prepared" : "stream already open");
        KeReleaseMutex(&extension->InterfaceMutex, FALSE);
        return STATUS_DEVICE_BUSY;
    }

    streamExt = (PPHOTONIC_STREAM_EXTENSION) stream->HwStreamExtension;
    RtlZeroMemory(streamExt, sizeof(*streamExt));
    PhotonicStreamExtensionInit(streamExt);
    KeInitializeEvent(&streamExt->DataPathIdle, NotificationEvent, FALSE);

    //
    // The capture engine (descriptor pool and reusable IRPs) lives for the whole
    // open stream; KSSTATE_RUN/STOP only acquire and release the isochronous
    // resources it streams with.
    //
    status =
        PhotonicCaptureCreate(extension, streamExt, PhotonicStreamDeliverFrame, PhotonicStreamAcquireBuffer, streamExt);
    if (!NT_SUCCESS(status)) {
        KeReleaseMutex(&extension->InterfaceMutex, FALSE);
        return status;
    }

    //
    // Capture the geometry of the format the pin is connecting with. The class
    // driver passes the negotiated connection format in OpenFormat; for a video
    // capture pin it is a KS_DATAFORMAT_VIDEOINFOHEADER. The data path uses this
    // to fill frames of the correct size whichever enumerated mode was chosen.
    //
    if (Srb->CommandData.OpenFormat != NULL &&
        Srb->CommandData.OpenFormat->FormatSize >= sizeof(KS_DATAFORMAT_VIDEOINFOHEADER)) {
        PKS_DATAFORMAT_VIDEOINFOHEADER format = (PKS_DATAFORMAT_VIDEOINFOHEADER) Srb->CommandData.OpenFormat;
        PKS_VIDEOINFOHEADER header = &format->VideoInfoHeader;

        streamExt->Width = header->bmiHeader.biWidth;
        streamExt->Height = abs(header->bmiHeader.biHeight);
        streamExt->BitCount = header->bmiHeader.biBitCount;
        streamExt->Compression = header->bmiHeader.biCompression;
        //
        // Derive the image size from the negotiated geometry instead of
        // trusting biSizeImage. The connection format comes from user mode,
        // and this size becomes the Format 7 frame size the isochronous
        // packetisation is derived from, so a lying header would silently
        // degrade the stream. The geometry itself is validated against the
        // advertised ranges by the mode lookup below.
        //
        streamExt->ImageSize = PhotonicImageBytes(streamExt->Width, streamExt->Height, streamExt->BitCount);

        //
        // A nonzero biSizeImage must agree with the geometry it came with
        // (the driver's own advertised formats do). Rejecting the mismatch
        // here beats streaming frames of a size the consumer does not expect.
        //
        if (header->bmiHeader.biSizeImage != 0 && header->bmiHeader.biSizeImage != streamExt->ImageSize) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM,
                        "biSizeImage %u does not match %ux%u at %u bpp (%u bytes); open rejected\n",
                        header->bmiHeader.biSizeImage, streamExt->Width, streamExt->Height, streamExt->BitCount,
                        streamExt->ImageSize);
            (VOID) PhotonicCaptureDestroy(streamExt);
            KeReleaseMutex(&extension->InterfaceMutex, FALSE);
            return STATUS_INVALID_PARAMETER;
        }
        streamExt->FrameInterval = (ULONG) header->AvgTimePerFrame;
        if (streamExt->FrameInterval == 0) {
            streamExt->FrameInterval = PHOTONIC_DEFAULT_FRAME_TIME;
        }

        //
        // Recover the DCAM mode behind this format so the capture engine can
        // program the camera to stream it when the pin runs.
        //
        streamExt->Mode = PhotonicStreamFindNegotiatedMode(extension, streamExt);
        if (streamExt->Mode == NULL) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM,
                        "open format matched no enumerated mode; data path idle\n");
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM,
                    "open format %ux%u %u-bit comp=0x%08x size=%u interval=%u\n", streamExt->Width, streamExt->Height,
                    streamExt->BitCount, streamExt->Compression, streamExt->ImageSize, streamExt->FrameInterval);
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "open without a video format; data path idle\n");
    }

    //
    // Install the routines the class driver calls for control and data SRBs on
    // this stream.
    //
    stream->ReceiveDataPacket = PhotonicStreamReceiveDataPacket;
    stream->ReceiveControlPacket = PhotonicStreamReceiveControlPacket;
    //
    // Dma and Pio declare how the minidriver itself accesses each read's frame
    // buffer, so the class driver knows what preparation to do. Both are FALSE:
    // the driver neither programs its own busmaster hardware against the buffer
    // (the 1394 bus driver does the DMA via isoch attach requests) nor touches
    // the pixels with the CPU (zero copy). The class driver builds a locked MDL
    // over the buffer regardless, which is all the data path needs.
    //
    stream->Dma = FALSE;
    stream->Pio = FALSE;
    stream->HwDeviceExtension = extension;
    //
    // DirectShow's video data type handler (kswdmcap) appends a KS_FRAME_INFO
    // extended header to every read it submits on a video pin. The class driver
    // validates each KSSTREAM_HEADER's Size against
    // sizeof(KSSTREAM_HEADER) + StreamHeaderMediaSpecific, so this must be
    // sizeof(KS_FRAME_INFO) or every user-mode read is rejected with
    // STATUS_INVALID_BUFFER_SIZE before reaching the driver. (On x64 a 32-bit
    // client's reads are re-issued by ksthunk at kernel mode, which skips that
    // validation and masks a mismatch; native 32-bit hosts have no thunk.)
    //
    stream->StreamHeaderMediaSpecific = sizeof(KS_FRAME_INFO);
    stream->StreamHeaderWorkspace = 0;
    stream->Allocator = FALSE;

    extension->VideoStream = stream;
    extension->ActiveCaptureStream = streamExt;
    KeReleaseMutex(&extension->InterfaceMutex, FALSE);
    return STATUS_SUCCESS;
}

/// SRB_CLOSE_STREAM -- the client has disconnected from the pin.
///
/// @param Srb  Stream request block for SRB_CLOSE_STREAM.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicStreamClose(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PPHOTONIC_STREAM_EXTENSION streamExt = (PPHOTONIC_STREAM_EXTENSION) Srb->StreamObject->HwStreamExtension;
    BOOLEAN busy;
    KIRQL irql;

    FuncEntry(TRACE_FLAG_STREAM);

    //
    // Serialize with the IOCTL handlers and the SRB teardown paths, and clear
    // VideoStream under the mutex so an IOCTL prepare never observes a stream
    // that is mid-teardown as closed.
    //
    KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);

    //
    // Publish the stopped state and begin the data-path rundown under
    // PendingLock, before anything is torn down. The client may close a
    // running pin without a stop, and a data SRB or a cancel can race this
    // close (TurnOffSynchronization): a read that observes KSSTATE_STOP is
    // rejected instead of parking after the final drain below, where nothing
    // would ever complete it, and a request arriving after Rundown is refused
    // at the gate without touching the stream. Completing a data SRB does not
    // mean its dispatching thread has returned, so the wait below is what
    // guarantees no thread still holds the Capture pointer when the engine is
    // freed: threads already inside the gate finish and leave, new ones are
    // refused, and the wait ends only when the count drains.
    //
    KeAcquireSpinLock(&streamExt->PendingLock, &irql);
    streamExt->StreamState = KSSTATE_STOP;
    streamExt->Rundown = TRUE;
    busy = streamExt->DataPathRefs != 0;
    KeReleaseSpinLock(&streamExt->PendingLock, irql);

    while (busy) {
        LARGE_INTEGER timeout;

        timeout.QuadPart = -10 * 1000 * 1000 * 10; // 10 seconds, relative
        busy =
            KeWaitForSingleObject(&streamExt->DataPathIdle, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT;
        if (busy) {
            //
            // A data or cancel handler should leave within microseconds; log
            // per period so a wedged thread is diagnosable, like the IRP
            // drain. Proceeding would free the engine under it.
            //
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM,
                        "still waiting for %d data-path thread(s) to leave before the close\n",
                        streamExt->DataPathRefs);
        }
    }

    //
    // Stop capturing if the client closed the pin without first moving it to
    // KSSTATE_STOP, so no isochronous resources are leaked. This also completes any
    // reads still parked on the queue.
    //
    PhotonicCaptureStop(extension, streamExt);
    if (!PhotonicCaptureDestroy(streamExt)) {
        //
        // The engine was leaked: a quarantined buffer still holds a DMA
        // mapping the resource free never confirmed flushed. Its read SRB
        // stays uncompleted by the same policy. The leaked engine is inert
        // (the context was stopped and the IRP drain completed), so the close
        // itself can proceed.
        //
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM,
                    "stream closed over a leaked capture engine; quarantined read(s) stay uncompleted\n");
    }

    //
    // Complete anything that slipped onto the parked-read queue after the stop
    // drain.
    //
    PhotonicStreamCompletePendingReads(streamExt);

    extension->VideoStream = NULL;
    extension->ActiveCaptureStream = NULL;
    KeReleaseMutex(&extension->InterfaceMutex, FALSE);
    return STATUS_SUCCESS;
}
