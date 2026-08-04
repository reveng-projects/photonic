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
/// Format negotiation for the Photonic stream minidriver: the
/// SRB_GET_DATA_INTERSECTION handler that turns one of the peer pin's data
/// ranges into a concrete format the camera can produce, and the matching
/// logic that picks the best advertised range.

// clang-format off
// intersect.tmh (WPP-generated) must come last.
#include "photonic.h"
#include "stream_private.h"
#include "intersect.tmh"
// clang-format on

/// PhotonicStreamIntersectGuidMatch -- TRUE if Candidate equals Wanted, or
/// Wanted is GUID_NULL (a wildcard the peer uses to mean "any"). Used to match
/// a requested data range loosely against the formats the camera advertises.
///
/// @param Candidate  The GUID being tested (from the advertised range).
/// @param Wanted     The GUID to match against (from the peer's request).
/// @return TRUE if the GUIDs match or Wanted is GUID_NULL, FALSE otherwise.
static BOOLEAN PhotonicStreamIntersectGuidMatch(_In_ const GUID *Candidate, _In_ const GUID *Wanted) {
    static const GUID guidNull = {0};
    return IsEqualGUID(Wanted, &guidNull) || IsEqualGUID(Candidate, Wanted);
}

/// PhotonicStreamIntersectFindRange -- find the advertised range best matching the
/// requested data range. Major/sub/specifier are matched (wildcards allowed);
/// when the request is itself a KS_DATARANGE_VIDEO, the requested output size
/// and frame interval weigh into the selection. Returns NULL on no match.
///
/// @param Extension  Device extension holding the advertised range table.
/// @param Requested  Data range from the peer pin to match against.
/// @return Pointer to the best matching advertised range, or NULL if none match.
static PKS_DATARANGE_VIDEO PhotonicStreamIntersectFindRange(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                                                            _In_ PKSDATARANGE Requested) {
    PKS_DATARANGE_VIDEO reqVideo = NULL;
    PKS_DATARANGE_VIDEO preferred = NULL;
    PKS_DATARANGE_VIDEO windowMatch = NULL;
    PKS_DATARANGE_VIDEO fallback = NULL;
    REFERENCE_TIME reqInterval = 0;
    LONG reqWidth = 0;
    LONG reqHeight = 0;
    ULONG i;

    if (Requested->FormatSize >= sizeof(KS_DATARANGE_VIDEO)) {
        reqVideo = (PKS_DATARANGE_VIDEO) Requested;
        reqInterval = reqVideo->VideoInfoHeader.AvgTimePerFrame;
        reqWidth = reqVideo->VideoInfoHeader.bmiHeader.biWidth;
        reqHeight = abs(reqVideo->VideoInfoHeader.bmiHeader.biHeight);
    }

    for (i = 0; i < Extension->FormatCount; i++) {
        PKS_DATARANGE_VIDEO range = &Extension->VideoRanges[i];
        LONG width;
        LONG height;
        BOOLEAN intervalOk;

        if (!PhotonicStreamIntersectGuidMatch(&range->DataRange.MajorFormat, &Requested->MajorFormat) ||
            !PhotonicStreamIntersectGuidMatch(&range->DataRange.SubFormat, &Requested->SubFormat) ||
            !PhotonicStreamIntersectGuidMatch(&range->DataRange.Specifier, &Requested->Specifier)) {
            continue;
        }

        //
        // A bare KSDATARANGE (no video caps) imposes no size constraint: the first
        // subtype match wins.
        //
        if (reqVideo == NULL) {
            return range;
        }

        intervalOk = PhotonicStreamRangeIntervalOk(range, reqInterval);
        width = range->VideoInfoHeader.bmiHeader.biWidth;
        height = abs(range->VideoInfoHeader.bmiHeader.biHeight);

        //
        // The peer described a video output-size window. Prefer the advertised
        // range whose window is identical to the request: during capability
        // enumeration KsProxy passes one of the driver's own ranges back as the
        // request, and (SubFormat, MinOutputSize, MaxOutputSize) identifies exactly
        // the capability being queried. Returning merely the first same-subtype
        // range that fits inside the window would collapse a large Format 7 mode
        // onto a smaller standard format that shares its colour coding -- e.g. the
        // 1388x1032 greyscale F7 mode reported (and connected) as 640x480, because
        // the standard 640x480 MONO8 range fits the F7 window and precedes it.
        //
        // The window alone is not decisive: when connecting a format chosen via
        // IAMStreamConfig::SetFormat, KsProxy stamps the chosen VideoInfoHeader
        // into a copy of whichever advertised range's window covers the requested
        // size first -- for a small size that is the first scalable Format 7 mode,
        // not the capability the caller picked. So an identical window only wins
        // outright when the range can also produce the requested frame interval;
        // otherwise keep scanning for a range that supports both, e.g. 320x240 at
        // 60 fps must land on the 320x240 F7 mode ([25000..300000] interval window)
        // and not on the 1572x1052 F7 mode whose [645000..1290000] window would
        // clamp the rate.
        //
        if (reqVideo->ConfigCaps.MaxOutputSize.cx == range->ConfigCaps.MaxOutputSize.cx &&
            reqVideo->ConfigCaps.MaxOutputSize.cy == range->ConfigCaps.MaxOutputSize.cy &&
            reqVideo->ConfigCaps.MinOutputSize.cx == range->ConfigCaps.MinOutputSize.cx &&
            reqVideo->ConfigCaps.MinOutputSize.cy == range->ConfigCaps.MinOutputSize.cy) {
            if (intervalOk) {
                return range;
            }
            if (windowMatch == NULL) {
                windowMatch = range;
            }
            continue;
        }

        //
        // A range whose default resolution equals the requested size and whose
        // interval window covers the requested frame interval is the capability
        // the caller intended; it beats a window match that cannot do the rate.
        //
        if (preferred == NULL && reqWidth > 0 && reqHeight > 0 &&
            PhotonicStreamRangeDefaultSizeIs(range, reqWidth, reqHeight) && intervalOk) {
            preferred = range;
            continue;
        }

        //
        // Otherwise remember the first range that fits within the requested window
        // as a fallback for a genuine peer request (a renderer that asks for any
        // size up to some maximum), but keep scanning for a better match.
        //
        if (fallback == NULL) {
            if (reqVideo->ConfigCaps.MaxOutputSize.cx != 0 &&
                (width > reqVideo->ConfigCaps.MaxOutputSize.cx || height > reqVideo->ConfigCaps.MaxOutputSize.cy)) {
                continue;
            }
            if (reqVideo->ConfigCaps.MinOutputSize.cx != 0 &&
                (width < reqVideo->ConfigCaps.MinOutputSize.cx || height < reqVideo->ConfigCaps.MinOutputSize.cy)) {
                continue;
            }
            fallback = range;
        }
    }

    if (preferred != NULL) {
        return preferred;
    }
    return windowMatch != NULL ? windowMatch : fallback;
}

/// SRB_GET_DATA_INTERSECTION -- given one of the peer pin's data ranges, return a
/// concrete format the camera can produce. The KS pin graph uses this to
/// negotiate the connection format; the best-matching advertised range wins
/// (see PhotonicStreamIntersectFindRange).
///
/// @param Srb  Stream request block for SRB_GET_DATA_INTERSECTION.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicStreamGetDataIntersection(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PSTREAM_DATA_INTERSECT_INFO intersect = Srb->CommandData.IntersectInfo;
    PKSDATARANGE requested = intersect->DataRange;
    PKS_DATARANGE_VIDEO match;
    PKS_DATAFORMAT_VIDEOINFOHEADER format;
    ULONG paletteColors;
    ULONG formatSize;

    FuncEntry(TRACE_FLAG_STREAM);

    if (intersect->StreamNumber != PHOTONIC_VIDEO_STREAM) {
        return STATUS_NOT_SUPPORTED;
    }

    match = PhotonicStreamIntersectFindRange(extension, requested);
    if (match == NULL) {
        return STATUS_NO_MATCH;
    }

    //
    // An RGB8 (DCAM MONO8) format carries a 256-entry greyscale palette appended
    // after the VIDEOINFOHEADER so DirectShow can render it; every other coding is a
    // bare KS_DATAFORMAT_VIDEOINFOHEADER. The size-only query below must report this
    // palette-inclusive size, so compute it before answering.
    //
    paletteColors = (match->VideoInfoHeader.bmiHeader.biBitCount == 8 &&
                     match->VideoInfoHeader.bmiHeader.biCompression == KS_BI_RGB)
                        ? PHOTONIC_RGB8_PALETTE_COLORS
                        : 0;
    formatSize = sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) + paletteColors * sizeof(KS_RGBQUAD);

    //
    // A size-only query passes a buffer of sizeof(ULONG); answer with the size
    // the caller must allocate.
    //
    if (intersect->SizeOfDataFormatBuffer == sizeof(ULONG)) {
        *(PULONG) intersect->DataFormatBuffer = formatSize;
        Srb->ActualBytesTransferred = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    if (intersect->SizeOfDataFormatBuffer < formatSize) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_STREAM, "format buffer %u bytes, need %u\n",
                    intersect->SizeOfDataFormatBuffer, formatSize);
        return STATUS_BUFFER_TOO_SMALL;
    }

    format = (PKS_DATAFORMAT_VIDEOINFOHEADER) intersect->DataFormatBuffer;
    RtlZeroMemory(format, formatSize);
    format->DataFormat.FormatSize = formatSize;
    format->DataFormat.SampleSize = match->DataRange.SampleSize;
    format->DataFormat.MajorFormat = match->DataRange.MajorFormat;
    format->DataFormat.SubFormat = match->DataRange.SubFormat;
    format->DataFormat.Specifier = match->DataRange.Specifier;
    format->VideoInfoHeader = match->VideoInfoHeader;

    //
    // The advertised VIDEOINFOHEADER defaults AvgTimePerFrame to the fastest
    // advertised rate. When the peer requests a specific frame rate (the test
    // app via IAMStreamConfig::SetFormat passes a KS_DATARANGE_VIDEO whose
    // VideoInfoHeader carries the desired interval), honour it so the connection
    // format reflects the chosen rate rather than collapsing every rate to the
    // fastest one. Clamp to the mode's advertised interval window; SRB_OPEN_STREAM
    // reads this interval back and the capture engine programs the matching DCAM
    // frame rate.
    //
    if (requested->FormatSize >= sizeof(KS_DATARANGE_VIDEO)) {
        REFERENCE_TIME requestedInterval = ((PKS_DATARANGE_VIDEO) requested)->VideoInfoHeader.AvgTimePerFrame;

        if (requestedInterval != 0) {
            if (requestedInterval < (REFERENCE_TIME) match->ConfigCaps.MinFrameInterval) {
                requestedInterval = match->ConfigCaps.MinFrameInterval;
            }
            if (requestedInterval > (REFERENCE_TIME) match->ConfigCaps.MaxFrameInterval) {
                requestedInterval = match->ConfigCaps.MaxFrameInterval;
            }
            format->VideoInfoHeader.AvgTimePerFrame = requestedInterval;
        }
    }

    //
    // A scalable (Format 7) range advertises an output-size window; the stored
    // VideoInfoHeader defaults to the mode's maximum size. When the peer requests a
    // specific smaller size inside that window, honour it -- clamped to
    // [Min..Max]OutputSize and aligned down to the output granularity -- rather than
    // connecting at the maximum. Without this a request for e.g. 320x240 on a
    // 1280x960-max mode streams a full 1280x960 frame, oversizing every buffer and
    // the isochronous resource allocation. A fixed range collapses its window to one
    // size, so this leaves it unchanged. Recompute the dependent size fields to match.
    //
    if (requested->FormatSize >= sizeof(KS_DATARANGE_VIDEO) &&
        (match->ConfigCaps.MaxOutputSize.cx != match->ConfigCaps.MinOutputSize.cx ||
         match->ConfigCaps.MaxOutputSize.cy != match->ConfigCaps.MinOutputSize.cy)) {
        PKS_VIDEOINFOHEADER reqHeader = &((PKS_DATARANGE_VIDEO) requested)->VideoInfoHeader;
        LONG reqWidth = reqHeader->bmiHeader.biWidth;
        //
        // The peer's requested height may itself be negative (a top-down media
        // type); the size negotiation below works on the magnitude.
        //
        LONG reqHeight =
            reqHeader->bmiHeader.biHeight < 0 ? -reqHeader->bmiHeader.biHeight : reqHeader->bmiHeader.biHeight;

        if (reqWidth > 0 && reqHeight > 0) {
            ULONG granX = match->ConfigCaps.OutputGranularityX ? match->ConfigCaps.OutputGranularityX : 1;
            ULONG granY = match->ConfigCaps.OutputGranularityY ? match->ConfigCaps.OutputGranularityY : 1;
            LONG bitCount = format->VideoInfoHeader.bmiHeader.biBitCount;
            ULONG imageSize;

            if (reqWidth > match->ConfigCaps.MaxOutputSize.cx) {
                reqWidth = match->ConfigCaps.MaxOutputSize.cx;
            }
            if (reqWidth < match->ConfigCaps.MinOutputSize.cx) {
                reqWidth = match->ConfigCaps.MinOutputSize.cx;
            }
            if (reqHeight > match->ConfigCaps.MaxOutputSize.cy) {
                reqHeight = match->ConfigCaps.MaxOutputSize.cy;
            }
            if (reqHeight < match->ConfigCaps.MinOutputSize.cy) {
                reqHeight = match->ConfigCaps.MinOutputSize.cy;
            }
            reqWidth = (LONG) ((reqWidth / granX) * granX);
            reqHeight = (LONG) ((reqHeight / granY) * granY);

            imageSize = PhotonicImageBytes((ULONG) reqWidth, (ULONG) reqHeight, (ULONG) bitCount);
            format->VideoInfoHeader.bmiHeader.biWidth = reqWidth;
            //
            // Keep the orientation the matched range advertises: negative
            // (top-down) for BI_RGB, positive for FOURCC codings.
            //
            format->VideoInfoHeader.bmiHeader.biHeight =
                format->VideoInfoHeader.bmiHeader.biHeight < 0 ? -reqHeight : reqHeight;
            format->VideoInfoHeader.bmiHeader.biSizeImage = imageSize;
            format->VideoInfoHeader.rcSource.right = reqWidth;
            format->VideoInfoHeader.rcSource.bottom = reqHeight;
            format->VideoInfoHeader.rcTarget.right = reqWidth;
            format->VideoInfoHeader.rcTarget.bottom = reqHeight;
            format->DataFormat.SampleSize = imageSize;
            if (format->VideoInfoHeader.AvgTimePerFrame != 0) {
                //
                // The interval is the advertised default or the request
                // clamped to the mode's window above, so it fits a ULONG.
                //
                format->VideoInfoHeader.dwBitRate =
                    PhotonicImageBitsPerSecond(imageSize, (ULONG) format->VideoInfoHeader.AvgTimePerFrame);
            }
        }
    }

    //
    // For RGB8, append the greyscale palette directly after the VIDEOINFOHEADER
    // (where KS_VIDEOINFO carries bmiColors). Colour index i is grey (i, i, i), so a
    // MONO8 luma sample of value i renders as the matching shade.
    //
    if (paletteColors != 0) {
        PKS_RGBQUAD palette = (PKS_RGBQUAD) ((PUCHAR) format + sizeof(KS_DATAFORMAT_VIDEOINFOHEADER));
        ULONG i;

        format->VideoInfoHeader.bmiHeader.biClrUsed = paletteColors;
        format->VideoInfoHeader.bmiHeader.biClrImportant = paletteColors;
        for (i = 0; i < paletteColors; i++) {
            palette[i].rgbBlue = (BYTE) i;
            palette[i].rgbGreen = (BYTE) i;
            palette[i].rgbRed = (BYTE) i;
            palette[i].rgbReserved = 0;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM,
                "intersection %ldx%ld interval=%I64d (mode window [%I64d..%I64d], max %ldx%ld)\n",
                format->VideoInfoHeader.bmiHeader.biWidth, format->VideoInfoHeader.bmiHeader.biHeight,
                format->VideoInfoHeader.AvgTimePerFrame, match->ConfigCaps.MinFrameInterval,
                match->ConfigCaps.MaxFrameInterval, match->ConfigCaps.MaxOutputSize.cx,
                match->ConfigCaps.MaxOutputSize.cy);

    Srb->ActualBytesTransferred = formatSize;
    return STATUS_SUCCESS;
}
