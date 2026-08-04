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
/// Format advertisement for the Photonic stream minidriver: the KS data
/// ranges the capture pin advertises, the tables behind them (GUIDs, stream
/// topology, mediums) and the SRB_GET_STREAM_INFO handler that describes the
/// streams to the class driver.
///
/// The capture pin's KS data ranges are built from the DCAM mode table the
/// driver enumerates from the camera (see PhotonicStreamFormatBuild below);
/// when enumeration yields no usable mode the camera advertises no formats
/// at all.

// clang-format off
// formats.tmh (WPP-generated) must come last.
#include "photonic.h"
#include "properties.h"
#include "stream_private.h"
#include "formats.tmh"
// clang-format on

BOOLEAN PhotonicStreamRangeIntervalOk(_In_ PKS_DATARANGE_VIDEO Range, _In_ LONGLONG Interval) {
    return Interval == 0 ||
           (Interval >= Range->ConfigCaps.MinFrameInterval && Interval <= Range->ConfigCaps.MaxFrameInterval);
}

BOOLEAN PhotonicStreamRangeDefaultSizeIs(_In_ PKS_DATARANGE_VIDEO Range, _In_ LONG Width, _In_ LONG Height) {
    return Range->VideoInfoHeader.bmiHeader.biWidth == Width &&
           abs(Range->VideoInfoHeader.bmiHeader.biHeight) == Height;
}

/// KS / DirectShow GUIDs this driver needs, defined locally (same bytes as the
/// standard ks.h / ksmedia.h / uuids.h definitions) so it links no GUID library.

/// KSDATAFORMAT_TYPE_VIDEO {73646976-0000-0010-8000-00AA00389B71}
static const GUID PHOTONIC_FORMAT_TYPE_VIDEO = {
    0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/// MEDIASUBTYPE_RGB24 {E436EB7D-524F-11CE-9F53-0020AF0BA770}
static const GUID PHOTONIC_FORMAT_SUBTYPE_RGB24 = {
    0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};

/// MEDIASUBTYPE_RGB8 {E436EB7A-524F-11CE-9F53-0020AF0BA770} -- 8-bit palettised RGB.
/// DCAM MONO8 is advertised as this (with a 256-entry greyscale palette supplied in
/// the connection format) rather than the 'Y800' FOURCC: stock DirectShow has no
/// renderer for Y800, so a Y800 capture pin cannot build a windowed render graph and
/// falls back to headless, whereas RGB8 renders through the built-in Color Space
/// Converter. The streamed bytes are identical (one 8-bit luma sample per pixel,
/// which doubles as the palette index into the grey ramp), so no pixel conversion is
/// needed in the data path.
static const GUID PHOTONIC_FORMAT_SUBTYPE_RGB8 = {
    0xe436eb7a, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};

/// FOURCC-based MEDIASUBTYPEs follow the pattern {FOURCC-0000-0010-8000-00AA00389B71},
/// where Data1 is the little-endian FOURCC. These cover the DCAM codings the
/// camera advertises beyond RGB24.
///
/// MEDIASUBTYPE_UYVY  {59565955-0000-0010-8000-00AA00389B71} ('UYVY') -- YUV 4:2:2
static const GUID PHOTONIC_FORMAT_SUBTYPE_UYVY = {
    0x59565955, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/// MEDIASUBTYPE_Y16   {20363159-0000-0010-8000-00AA00389B71} ('Y16 ') -- 16-bit greyscale
static const GUID PHOTONIC_FORMAT_SUBTYPE_Y16 = {
    0x20363159, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/// MEDIASUBTYPE_Y411  {31313459-0000-0010-8000-00AA00389B71} ('Y411') -- YUV 4:1:1
static const GUID PHOTONIC_FORMAT_SUBTYPE_Y411 = {
    0x31313459, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/// MEDIASUBTYPE_RGGB  {42474752-0000-0010-8000-00AA00389B71} ('RGGB') -- 8-bit Bayer RGGB
/// (DCAM RAW8, used by Format 7). There is no standard DirectShow Bayer subtype,
/// so a FOURCC-derived one naming the mosaic order is used.
static const GUID PHOTONIC_FORMAT_SUBTYPE_RGGB = {
    0x42474752, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/// FOURCC values placed in KS_BITMAPINFOHEADER.biCompression for the non-RGB
/// codings above (RGB24 uses KS_BI_RGB == 0).
#define PHOTONIC_FOURCC_UYVY 0x59565955
#define PHOTONIC_FOURCC_Y16  0x20363159
#define PHOTONIC_FOURCC_Y411 0x31313459
#define PHOTONIC_FOURCC_RGGB 0x42474752

/// KSDATAFORMAT_SPECIFIER_VIDEOINFO {05589F80-C356-11CE-BF01-00AA0055595A}
static const GUID PHOTONIC_FORMAT_SPECIFIER_VIDEOINFO = {
    0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};

/// PINNAME_VIDEO_CAPTURE {FB6C4281-0353-11D1-905F-0000C0CC16BA}
static const GUID PHOTONIC_PINNAME_VIDEO_CAPTURE = {
    0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};

/// The advertised KS data ranges, their pointer array, format count, max sample
/// size and the capture stream information are all per camera and live in the
/// device extension (see PHOTONIC_DEVICE_EXTENSION): the system may have several
/// cameras present, each supporting a different set of modes.

static const GUID g_PhotonicCategories[2] = {
    // KSCATEGORY_CAPTURE {65E8773D-8F56-11D0-A3B9-00A0C9223196}
    {0x65e8773d, 0x8f56, 0x11d0, {0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}},
    // KSCATEGORY_VIDEO {6994AD05-93EF-11D0-A3CC-00A0C9223196}
    {0x6994ad05, 0x93ef, 0x11d0, {0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}},
};

static const KSTOPOLOGY g_PhotonicTopology = {
    RTL_NUMBER_OF(g_PhotonicCategories), // CategoriesCount
    g_PhotonicCategories,                // Categories
    0,                                   // TopologyNodesCount
    NULL,                                // TopologyNodes
    0,                                   // TopologyConnectionsCount
    NULL,                                // TopologyConnections
    NULL,                                // TopologyNodesNames
    0                                    // Reserved
};

static const KSPIN_MEDIUM g_PhotonicMediums[1] = {
    // KSMEDIUMSETID_Standard {4747B320-62CE-11CF-A5D6-28DB04C10000}, wildcard Id / Flags.
    {{{
        {0x4747b320, 0x62ce, 0x11cf, {0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00}},
        0, // Id
        0  // Flags
    }}},
};

/// PhotonicStreamFormatPixelToMedia -- map a DCAM colour coding (PHOTONIC_DCAM_PIX_*)
/// to the DirectShow subtype GUID, bit depth and biCompression value the camera
/// would deliver it as.
///
/// @param PixelFormat   DCAM pixel format constant (PHOTONIC_DCAM_PIX_*).
/// @param SubType       Receives the matching DirectShow subtype GUID pointer.
/// @param BitCount      Receives the bit depth for this coding.
/// @param Compression   Receives the biCompression value (KS_BI_RGB or a FOURCC).
/// @return TRUE if the format has a DirectShow mapping, FALSE otherwise.
static BOOLEAN PhotonicStreamFormatPixelToMedia(_In_ ULONG PixelFormat, _Out_ const GUID **SubType,
                                                _Out_ PULONG BitCount, _Out_ PULONG Compression) {
    switch (PixelFormat) {
        case PHOTONIC_DCAM_PIX_YUV444:
            //
            // The only YUV444 mode is 160x120. The camera streams it as packed
            // 24-bit BGR -- the same byte layout as RGB24 -- so it is advertised as
            // RGB24 and renders directly. (A true
            // 4:4:4 YUV source would need conversion, but this camera never emits
            // one.) Falls through to the RGB24 mapping below.
            //
        case PHOTONIC_DCAM_PIX_RGB24:
            *SubType = &PHOTONIC_FORMAT_SUBTYPE_RGB24;
            *BitCount = 24;
            *Compression = KS_BI_RGB;
            return TRUE;
        case PHOTONIC_DCAM_PIX_YUV422:
            *SubType = &PHOTONIC_FORMAT_SUBTYPE_UYVY;
            *BitCount = 16;
            *Compression = PHOTONIC_FOURCC_UYVY;
            return TRUE;
        case PHOTONIC_DCAM_PIX_MONO8:
            //
            // 8-bit greyscale exposed as palettised RGB8 (BI_RGB) so DirectShow can
            // render it in a window via the Color Space Converter. The matching
            // greyscale palette is appended to the connection format in
            // PhotonicStreamGetDataIntersection.
            //
            *SubType = &PHOTONIC_FORMAT_SUBTYPE_RGB8;
            *BitCount = 8;
            *Compression = KS_BI_RGB;
            return TRUE;
        case PHOTONIC_DCAM_PIX_MONO16:
            *SubType = &PHOTONIC_FORMAT_SUBTYPE_Y16;
            *BitCount = 16;
            *Compression = PHOTONIC_FOURCC_Y16;
            return TRUE;
        case PHOTONIC_DCAM_PIX_YUV411:
            *SubType = &PHOTONIC_FORMAT_SUBTYPE_Y411;
            *BitCount = 12;
            *Compression = PHOTONIC_FOURCC_Y411;
            return TRUE;
        case PHOTONIC_DCAM_PIX_RAW8:
            *SubType = &PHOTONIC_FORMAT_SUBTYPE_RGGB;
            *BitCount = 8;
            *Compression = PHOTONIC_FOURCC_RGGB;
            return TRUE;
        default:
            *SubType = NULL;
            *BitCount = 0;
            *Compression = 0;
            return FALSE;
    }
}

/// PhotonicStreamFormatFillRange -- populate one KS_DATARANGE_VIDEO describing a single
/// coding the capture pin can produce. Width x Height is the maximum output size;
/// the advertised output-size window runs from MinWidth x MinHeight up to
/// Width x Height in steps of GranX x GranY, and the sample buffer is sized for
/// this maximum. DefaultWidth x DefaultHeight is the size the returned
/// VIDEOINFOHEADER connects at by default. A fixed format passes MinWidth/MinHeight
/// and DefaultWidth/DefaultHeight all equal to Width/Height (and granularity equal
/// to the size), so its window collapses to that one size; a Format 7 mode passes
/// the unit step as the minimum and granularity and the camera's current size as
/// the default, which the pin (and the test) reads as a scalable range.
/// MinInterval / MaxInterval bound the advertised frame rate (100ns units); both
/// default to 30 fps when a mode reports no rates.
///
/// @param Range          KS_DATARANGE_VIDEO structure to fill.
/// @param SubType        DirectShow subtype GUID for this coding.
/// @param Width          Maximum output width in pixels.
/// @param Height         Maximum output height in pixels.
/// @param DefaultWidth   Default connection width in pixels.
/// @param DefaultHeight  Default connection height in pixels.
/// @param MinWidth       Minimum output width for the advertised size window.
/// @param MinHeight      Minimum output height for the advertised size window.
/// @param GranX          Output width granularity (step size).
/// @param GranY          Output height granularity (step size).
/// @param BitCount       Bits per pixel for this coding.
/// @param Compression    biCompression value (KS_BI_RGB or a FOURCC).
/// @param MinInterval    Fastest advertised frame interval in 100ns units.
/// @param MaxInterval    Slowest advertised frame interval in 100ns units.
static VOID PhotonicStreamFormatFillRange(_Out_ PKS_DATARANGE_VIDEO Range, _In_ const GUID *SubType, _In_ ULONG Width,
                                          _In_ ULONG Height, _In_ ULONG DefaultWidth, _In_ ULONG DefaultHeight,
                                          _In_ ULONG MinWidth, _In_ ULONG MinHeight, _In_ ULONG GranX, _In_ ULONG GranY,
                                          _In_ ULONG BitCount, _In_ ULONG Compression, _In_ ULONG MinInterval,
                                          _In_ ULONG MaxInterval) {
    PKS_VIDEOINFOHEADER header = &Range->VideoInfoHeader;
    ULONG imageSize = PhotonicImageBytes(Width, Height, BitCount);
    ULONG defImageSize = PhotonicImageBytes(DefaultWidth, DefaultHeight, BitCount);

    RtlZeroMemory(Range, sizeof(*Range));

    if (MinInterval == 0) {
        MinInterval = PHOTONIC_DEFAULT_FRAME_TIME;
    }
    if (MaxInterval == 0) {
        MaxInterval = MinInterval;
    }

    //
    // KSDATARANGE / KSDATAFORMAT header describing this video format carried in
    // a VIDEOINFOHEADER.
    //
    Range->DataRange.FormatSize = sizeof(KS_DATARANGE_VIDEO);
    Range->DataRange.SampleSize = imageSize;
    Range->DataRange.MajorFormat = PHOTONIC_FORMAT_TYPE_VIDEO;
    Range->DataRange.SubFormat = *SubType;
    Range->DataRange.Specifier = PHOTONIC_FORMAT_SPECIFIER_VIDEOINFO;

    Range->bFixedSizeSamples = TRUE;
    Range->bTemporalCompression = FALSE;
    Range->StreamDescriptionFlags = 0;
    Range->MemoryAllocationFlags = 0;

    //
    // Capability advertisement: an output-size window from MinWidth x MinHeight
    // up to Width x Height (a single fixed size for the standard formats, a
    // scalable range for Format 7), frame-rate range bounded by the camera's
    // advertised rates.
    //
    Range->ConfigCaps.guid = PHOTONIC_FORMAT_TYPE_VIDEO;
    Range->ConfigCaps.VideoStandard = KS_AnalogVideo_None;
    Range->ConfigCaps.InputSize.cx = Width;
    Range->ConfigCaps.InputSize.cy = Height;
    Range->ConfigCaps.MinCroppingSize.cx = MinWidth;
    Range->ConfigCaps.MinCroppingSize.cy = MinHeight;
    Range->ConfigCaps.MaxCroppingSize.cx = Width;
    Range->ConfigCaps.MaxCroppingSize.cy = Height;
    Range->ConfigCaps.CropGranularityX = GranX;
    Range->ConfigCaps.CropGranularityY = GranY;
    //
    // Crop alignment must be non-zero: kswdmcap!IsMediaTypeInRange divides the
    // requested crop origin by these fields, so leaving them at the
    // zero-initialized default triggers an integer divide-by-zero in the proxy
    // during SetFormat. A value of 1 means "no alignment constraint".
    //
    Range->ConfigCaps.CropAlignX = 1;
    Range->ConfigCaps.CropAlignY = 1;
    Range->ConfigCaps.MinOutputSize.cx = MinWidth;
    Range->ConfigCaps.MinOutputSize.cy = MinHeight;
    Range->ConfigCaps.MaxOutputSize.cx = Width;
    Range->ConfigCaps.MaxOutputSize.cy = Height;
    Range->ConfigCaps.OutputGranularityX = GranX;
    Range->ConfigCaps.OutputGranularityY = GranY;
    Range->ConfigCaps.MinFrameInterval = MinInterval;
    Range->ConfigCaps.MaxFrameInterval = MaxInterval;
    Range->ConfigCaps.MinBitsPerSecond = PhotonicImageBitsPerSecond(imageSize, MaxInterval);
    Range->ConfigCaps.MaxBitsPerSecond = PhotonicImageBitsPerSecond(imageSize, MinInterval);

    //
    // Default VIDEOINFOHEADER returned for this range at the default output size
    // (the fastest advertised rate as the default interval). A scalable Format 7
    // mode defaults to the camera's current size, below its advertised maximum;
    // the sample buffer (SampleSize above) is still sized for that maximum.
    //
    // The camera transmits frames top scanline first (IIDC ordering) and the
    // driver passes the data through unmodified, so a BI_RGB coding must be
    // advertised as a top-down bitmap (negative biHeight); a positive biHeight
    // would declare it bottom-up and the renderer would display it upside down.
    // FOURCC codings are top-down by definition and keep a positive biHeight.
    //
    header->AvgTimePerFrame = MinInterval;
    header->dwBitRate = PhotonicImageBitsPerSecond(defImageSize, MinInterval);
    header->rcSource.right = DefaultWidth;
    header->rcSource.bottom = DefaultHeight;
    header->rcTarget.right = DefaultWidth;
    header->rcTarget.bottom = DefaultHeight;
    header->bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
    header->bmiHeader.biWidth = DefaultWidth;
    header->bmiHeader.biHeight = (Compression == KS_BI_RGB) ? -(LONG) DefaultHeight : (LONG) DefaultHeight;
    header->bmiHeader.biPlanes = 1;
    header->bmiHeader.biBitCount = (WORD) BitCount;
    header->bmiHeader.biCompression = Compression;
    header->bmiHeader.biSizeImage = defImageSize;
    //
    // An 8-bit BI_RGB format is palettised: advertise a full 256-entry palette so
    // the proxy treats it as RGB8 (the greyscale ramp itself is supplied with the
    // connection format in PhotonicStreamGetDataIntersection). Other codings are
    // non-palettised and leave biClrUsed at 0.
    //
    if (BitCount == 8 && Compression == KS_BI_RGB) {
        header->bmiHeader.biClrUsed = PHOTONIC_RGB8_PALETTE_COLORS;
    }
}

/// PhotonicStreamFormatBuild -- turn the modes enumerated into Extension->Modes
/// into the KS data ranges this camera's capture pin advertises. Each mappable
/// mode becomes one entry in Extension->VideoRanges / Extension->Formats. When no
/// enumerated mode maps to a usable format, the camera advertises nothing: no
/// built-in fallback is published.
///
/// @param Extension  Device extension holding the enumerated mode table.
VOID PhotonicStreamFormatBuild(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    PHW_STREAM_INFORMATION videoStream = &Extension->VideoStreamInfo;
    ULONG i;

    FuncEntry(TRACE_FLAG_STREAM);

    Extension->FormatCount = 0;
    Extension->MaxSampleSize = 0;

    for (i = 0; i < Extension->ModeCount && Extension->FormatCount < PHOTONIC_MAX_VIDEO_MODES; i++) {
        PPHOTONIC_VIDEO_MODE mode = &Extension->Modes[i];
        const GUID *subType;
        ULONG bitCount;
        ULONG compression;
        ULONG minWidth;
        ULONG minHeight;
        ULONG granX;
        ULONG granY;
        PKS_DATARANGE_VIDEO range;
        ULONG sampleSize;

        if (!PhotonicStreamFormatPixelToMedia(mode->PixelFormat, &subType, &bitCount, &compression)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM,
                        "mode %u: DCAM pixel format %u has no DirectShow mapping; skipped\n", i, mode->PixelFormat);
            continue;
        }

        //
        // A Format 7 mode advertises a scalable output-size window from its unit
        // step up to its maximum; a standard fixed format collapses the window to
        // its single resolution (min == max, granularity == size).
        //
        if (mode->IsFormat7) {
            minWidth = mode->UnitWidth;
            minHeight = mode->UnitHeight;
            granX = mode->UnitWidth;
            granY = mode->UnitHeight;
        } else {
            minWidth = mode->Width;
            minHeight = mode->Height;
            granX = mode->Width;
            granY = mode->Height;
        }

        range = &Extension->VideoRanges[Extension->FormatCount];
        PhotonicStreamFormatFillRange(range, subType, mode->Width, mode->Height, mode->DefaultWidth,
                                      mode->DefaultHeight, minWidth, minHeight, granX, granY, bitCount, compression,
                                      mode->MinFrameInterval, mode->MaxFrameInterval);
        Extension->Formats[Extension->FormatCount] = (PKSDATAFORMAT) range;

        //
        // Remember which enumerated mode this advertised range came from so the
        // data path can recover the DCAM (format, mode, coding) the pin negotiated
        // and program the camera to stream it.
        //
        Extension->RangeModeIndex[Extension->FormatCount] = i;

        sampleSize = range->DataRange.SampleSize;
        if (sampleSize > Extension->MaxSampleSize) {
            Extension->MaxSampleSize = sampleSize;
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM,
                    "format[%u]: def %ux%u max %ux%u %s %u-bit comp=0x%08x interval=[%u..%u]\n", Extension->FormatCount,
                    mode->DefaultWidth, mode->DefaultHeight, mode->Width, mode->Height,
                    mode->IsFormat7 ? "(F7 scalable)" : "(fixed)", bitCount, compression, mode->MinFrameInterval,
                    mode->MaxFrameInterval);
        Extension->FormatCount++;
    }

    if (Extension->FormatCount == 0) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_STREAM, "no usable enumerated modes; advertising no formats\n");
    }

    //
    // The single capture output stream that references the formats above.
    //
    RtlZeroMemory(videoStream, sizeof(*videoStream));
    videoStream->NumberOfPossibleInstances = 1;
    videoStream->DataFlow = KSPIN_DATAFLOW_OUT;
    videoStream->DataAccessible = TRUE;
    videoStream->NumberOfFormatArrayEntries = Extension->FormatCount;
    videoStream->StreamFormatsArray = Extension->Formats;
    videoStream->Category = (GUID *) &PHOTONIC_PINNAME_VIDEO_CAPTURE;
    videoStream->Name = (GUID *) &PHOTONIC_PINNAME_VIDEO_CAPTURE;

    //
    // Connection medium the capture pin advertises (KSMEDIUMSETID_Standard).
    //
    videoStream->MediumsCount = RTL_NUMBER_OF(g_PhotonicMediums);
    videoStream->Mediums = g_PhotonicMediums;

    //
    // Per-stream KS property sets.
    //
    PhotonicGetStreamPropertySet(&videoStream->StreamPropertiesArray, &videoStream->NumStreamPropArrayEntries);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM, "advertising %u video format(s); max sample %u bytes\n",
                Extension->FormatCount, Extension->MaxSampleSize);
}

/// SRB_GET_STREAM_INFO -- describe the streams the device supports. The class
/// driver has already allocated a buffer of the size the driver reported in
/// SRB_INITIALIZE_DEVICE.
///
/// @param Srb   Stream request block for SRB_GET_STREAM_INFO.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicStreamGetInfo(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PHW_STREAM_DESCRIPTOR descriptor = Srb->CommandData.StreamBuffer;
    PHW_STREAM_INFORMATION info = &descriptor->StreamInfo;

    FuncEntry(TRACE_FLAG_STREAM);

    RtlZeroMemory(&descriptor->StreamHeader, sizeof(descriptor->StreamHeader));
    descriptor->StreamHeader.NumberOfStreams = PHOTONIC_STREAM_COUNT;
    descriptor->StreamHeader.SizeOfHwStreamInformation = sizeof(HW_STREAM_INFORMATION);
    descriptor->StreamHeader.Topology = (PKSTOPOLOGY) &g_PhotonicTopology;

    PhotonicGetDevicePropertySet(extension, &descriptor->StreamHeader.DevicePropertiesArray,
                                 &descriptor->StreamHeader.NumDevPropArrayEntries);

    info[PHOTONIC_VIDEO_STREAM] = extension->VideoStreamInfo;

    Srb->ActualBytesTransferred = sizeof(HW_STREAM_HEADER) + PHOTONIC_STREAM_COUNT * sizeof(HW_STREAM_INFORMATION);

    return STATUS_SUCCESS;
}
