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
/// Stream Class minidriver for the Photonic IEEE 1394 (firewire) camera.
///
/// The driver is a streaming minidriver: it registers with the stream class
/// driver (stream.sys) through StreamClassRegisterAdapter and is driven entirely
/// by Stream Request Blocks (SRBs). The class driver owns the WDM device object,
/// PnP, power and the KS pin/filter plumbing; this minidriver only answers SRBs.

#ifndef PHOTONIC_H
#define PHOTONIC_H

//
// strmini.h pulls in wdm.h and ks.h; ksmedia.h adds the video format types
// (KS_DATARANGE_VIDEO, KS_VIDEOINFOHEADER, ...) used to describe the capture stream.
//
// clang-format off
// Include order is significant and must not be sorted: strmini.h pulls in wdm.h
// and ks.h, which ksmedia.h depends on. 1394.h (the bus-driver IRB interface)
// depends on wdm.h and so must follow strmini.h.
#include "strmini.h"
#include "ksmedia.h"
#include "1394.h"
// stdlib.h declares abs, which the compiler emits as an intrinsic.
#include <stdlib.h>
// clang-format on

/// Pool tag used for all allocations made by this driver ('ohPp', shown as
/// "pPho" by pool tools).
#define PHOTONIC_POOL_TAG 'ohPp'

//
// WPP software tracing definitions (control GUID, trace flags, the Trace*
// macros). Driver code calls FuncEntry/FuncExit/TraceEvents; the WPP
// preprocessor turns those into ETW events at build time. See trace.h.
//
#include "trace.h"

//
// PL_RETURN_CODE, the status code space shared with pixelinkapi.dll. The device
// extension carries the last error in it (see LastError below).
//
#include "return_code.h"

/// Human-readable name for an SRB Command code, used in the dispatcher traces
/// (defined in dispatch.c).
///
/// @param Command  SRB Command code to look up.
/// @return         A short string literal naming the command, or `"SRB_<unknown>"`.
const char *PhotonicSrbCommandName(_In_ ULONG Command);

/// The minidriver exposes a single video capture output stream. The DCAM mode
/// table (resolutions / pixel formats / frame rates) enumerated from the camera
/// is turned into the stream's format array in stream/formats.c.
#define PHOTONIC_VIDEO_STREAM 0
#define PHOTONIC_STREAM_COUNT 1

/// Upper bound on the number of video modes enumerated from the camera. The
/// standard DCAM Formats 0/1/2 contribute up to 3 x 8 = 24 (format, mode) slots;
/// Format 7 (scalable image) adds one entry per (mode, colour coding) it
/// advertises (up to 8 modes x several codings). The table is oversized so all
/// of these fit without resizing. Stored inline in the device extension, no
/// dynamic allocation.
#define PHOTONIC_MAX_VIDEO_MODES 48

/// DCAM colour-coding ids (the standard IIDC/DCAM codings selected by a
/// (format, mode) pair). Stored in PHOTONIC_VIDEO_MODE.PixelFormat and mapped to
/// a DirectShow subtype in stream/formats.c. PHOTONIC_DCAM_PIX_INVALID marks an
/// unsupported mode slot in the geometry table.
#define PHOTONIC_DCAM_PIX_MONO8   0
#define PHOTONIC_DCAM_PIX_YUV411  1
#define PHOTONIC_DCAM_PIX_YUV422  2
#define PHOTONIC_DCAM_PIX_YUV444  3
#define PHOTONIC_DCAM_PIX_RGB24   4
#define PHOTONIC_DCAM_PIX_MONO16  5
#define PHOTONIC_DCAM_PIX_RAW8    7
#define PHOTONIC_DCAM_PIX_INVALID 0xFFFFFFFF

/// Frame byte size of a Width x Height image at BitsPerPixel. The product is
/// formed in 64 bits before the divide, so the 12-bit coding (YUV411) sizes
/// to 1.5 bytes per pixel instead of truncating, and device- or
/// client-supplied geometry cannot wrap the 32-bit intermediate. Callers
/// validate the geometry against the advertised modes (or the 256 MB cap)
/// before the result is used, so the final cast cannot truncate.
///
/// @param Width         Image width in pixels.
/// @param Height        Image height in pixels.
/// @param BitsPerPixel  Colour-coding depth in bits per pixel.
/// @return Frame size in bytes.
FORCEINLINE ULONG PhotonicImageBytes(_In_ ULONG Width, _In_ ULONG Height, _In_ ULONG BitsPerPixel) {
    return (ULONG) (((ULONGLONG) Width * Height * BitsPerPixel) / 8);
}

/// Average bit rate of a stream delivering ImageBytes-sized frames every
/// Interval 100ns units, as the format advertisement and intersection report
/// it. The multiply runs in 64 bits so a large frame at a fast rate cannot
/// wrap the 32-bit intermediate. The caller passes a nonzero interval taken
/// from (or clamped to) the validated mode tables.
///
/// @param ImageBytes  Frame size in bytes.
/// @param Interval    Frame interval in 100-nanosecond units, nonzero.
/// @return Average bit rate in bits per second.
FORCEINLINE ULONG PhotonicImageBitsPerSecond(_In_ ULONG ImageBytes, _In_ ULONG Interval) {
    return (ULONG) (((ULONGLONG) ImageBytes * 8 * 10000000) / Interval);
}

/// One enumerated DCAM video mode: a single (format, mode) pair the camera
/// advertised, decoded into the geometry and pixel coding DirectShow needs plus
/// the set of frame rates the camera supports for it.
typedef struct _PHOTONIC_VIDEO_MODE {
    /// DCAM format id (0, 1, 2 for the standard formats) and the mode id within
    /// that format, as latched into the VIDEO_FORMAT / VIDEO_MODE registers.
    UCHAR Format;
    UCHAR Mode;

    /// DCAM colour coding for this mode (PHOTONIC_DCAM_PIX_*) and the decoded
    /// image geometry. Width x Height is the maximum output size the mode can
    /// produce: for the standard formats it is the fixed resolution from the DCAM
    /// format table; for Format 7 (scalable image) it is the maximum image size
    /// (MAX_IMAGE_SIZE) aligned down to the unit grid, the ceiling of the scalable
    /// output-size window. DefaultWidth x DefaultHeight is the size the pin
    /// connects at by default: the same fixed resolution for the standard formats,
    /// and the camera's current IMAGE_SIZE for Format 7.
    ULONG PixelFormat;
    ULONG Width;
    ULONG Height;
    ULONG DefaultWidth;
    ULONG DefaultHeight;

    /// TRUE for a Format 7 (scalable image) mode. Such a mode advertises a range
    /// of output sizes from UnitWidth x UnitHeight up to Width x Height in steps
    /// of the unit size, rather than a single fixed resolution. UnitWidth /
    /// UnitHeight are the unit step (UNIT_SIZE); they are zero for the standard
    /// fixed formats.
    BOOLEAN IsFormat7;
    ULONG UnitWidth;
    ULONG UnitHeight;

    /// For a Format 7 mode, the offset (relative to the camera's CSR base) of the
    /// mode's CSR block, resolved from V_CSR_INQ_7_m during enumeration. The
    /// capture path reprograms IMAGE_SIZE / COLOR_CODING_ID / BYTE_PER_PACKET in
    /// this block before streaming. Zero for the standard fixed formats.
    ULONG Format7CsrOffset;

    /// Frame rates the camera advertised for this mode: bit (31 - rate) is set
    /// per supported DCAM rate id, exactly as read from FRAME_RATE_INQ. Unused
    /// (zero) for Format 7, whose rate range comes from the packet parameters
    /// (PACKET_PARA_INQ) and is carried only in the frame-interval bounds below.
    ULONG RateMask;

    /// Frame interval bounds in 100ns units: the fastest supported rate gives the
    /// minimum interval, the slowest the maximum. For the standard formats these
    /// are derived from RateMask; for Format 7 they are derived from the camera's
    /// packet parameters (PACKET_PARA_INQ) at the current (default) image size.
    ULONG MinFrameInterval;
    ULONG MaxFrameInterval;
} PHOTONIC_VIDEO_MODE, *PPHOTONIC_VIDEO_MODE;

/// Number of DirectShow image controls (VideoProcAmp + CameraControl) the driver
/// can map to DCAM feature registers, and the maximum number of device (filter)
/// property sets built from them (VideoControl + VideoProcAmp + CameraControl).
#define PHOTONIC_IMAGER_FEATURE_COUNT    13
#define PHOTONIC_DEVICE_PROPERTY_SET_MAX 3

/// One DirectShow image control (VideoProcAmp / CameraControl), discovered from
/// the camera during initialization by PhotonicBuildDevicePropertySets
/// (properties.c). Present is FALSE when the camera does not implement the
/// backing DCAM feature register; such a control is not advertised. For a
/// present control, Minimum/Maximum/DefaultValue is the range the camera
/// actually supports and AutoSupported whether it honours auto mode. Stepping /
/// Members / Values are the KS range data the advertised property item points
/// at, so the stream class driver answers GetRange from the discovered range.
typedef struct _PHOTONIC_IMAGER_FEATURE {
    BOOLEAN Present;
    BOOLEAN AutoSupported;
    LONG Minimum;
    LONG Maximum;
    LONG DefaultValue;
    KSPROPERTY_STEPPING_LONG Stepping;
    KSPROPERTY_MEMBERSLIST Members[2];
    KSPROPERTY_VALUES Values;
} PHOTONIC_IMAGER_FEATURE, *PPHOTONIC_IMAGER_FEATURE;

/// Per-adapter context. The stream class driver allocates this (its size is
/// advertised in HW_INITIALIZATION_DATA.DeviceExtensionSize) and hands it back
/// as Srb->HwDeviceExtension on every SRB.
typedef struct _PHOTONIC_DEVICE_EXTENSION {
    /// Serializes the device's control plane. The hooked Photonic IOCTL path
    /// has no synchronization of its own: user threads can issue IOCTLs
    /// concurrently with each other, with the IRP_MJ_CLEANUP hook and with
    /// device SRBs (TurnOffSynchronization). This mutex is held across every
    /// Photonic IOCTL handler (by the dispatcher in ioctl/ioctl.c), by the
    /// cleanup hook, by SRB_OPEN_STREAM / SRB_CLOSE_STREAM /
    /// SRB_SET_STREAM_STATE, by the SRB teardown paths (surprise removal,
    /// uninitialize) and by the control work item, so exactly one context at
    /// a time performs a control transition: the lookup and use of
    /// IoctlCapture is atomic with its release, the mutual-exclusion checks
    /// between VideoStream and IoctlCapture are atomic with their
    /// publication, and every start, stop, listen and cancel resolution of
    /// the capture engine is serialized against all the others. A KMUTEX
    /// rather than a FAST_MUTEX: holders stay at PASSIVE_LEVEL, which
    /// ObReferenceObjectByHandle (REGISTER_EVENT) requires. All acquiring
    /// paths run at PASSIVE_LEVEL.
    KMUTEX InterfaceMutex;

    /// Control work item and its coalescing flag (capture/work.c). DISPATCH_LEVEL
    /// triggers that need a control transition (a read cancel, a listen that
    /// became possible after an attach) queue this work item through
    /// PhotonicCaptureRequestWork; the work item acquires InterfaceMutex and
    /// services the active engine at PASSIVE_LEVEL. Allocated at
    /// SRB_INITIALIZE_DEVICE and freed at SRB_UNINITIALIZE_DEVICE after the
    /// IrbRemoveLock wait, which covers queued and running work items.
    PIO_WORKITEM ControlWorkItem;
    volatile LONG ControlWorkQueued;

    /// The stream extension the active capture front-end drives, NULL when
    /// neither interface owns the camera: the DirectShow pin's extension
    /// while VideoStream is open, the IOCTL slot's embedded extension while
    /// IoctlCapture is prepared. Published and cleared under InterfaceMutex
    /// alongside those pointers; the control work item uses it to reach the
    /// engine without knowing which front-end owns it.
    struct _PHOTONIC_STREAM_EXTENSION *ActiveCaptureStream;

    /// Stream object for the capture stream while it is open, NULL otherwise.
    /// Only one instance of the stream may be open at a time. Published and
    /// cleared under InterfaceMutex.
    PHW_STREAM_OBJECT VideoStream;

    /// Capture slot of the Photonic IOCTL interface (ioctl/frames.c), allocated
    /// by PHOTONIC_IOCTL_PREPARE_VIDEO or PHOTONIC_IOCTL_PREPARE_IMAGER and freed
    /// by either unprepare or device teardown; NULL otherwise. One slot per
    /// device, shared by both IOCTL families (video drives the whole session,
    /// imager the transmit half). The IOCTL and DirectShow capture interfaces are
    /// mutually exclusive: both prepares fail while the DirectShow stream is open
    /// (VideoStream != NULL) and SRB_OPEN_STREAM fails while this is non-NULL.
    /// Both drive the common capture engine (capture/) through the
    /// PHOTONIC_STREAM_EXTENSION this slot embeds. Guarded by InterfaceMutex:
    /// every reader and writer of this pointer holds the mutex, so no handler
    /// can dereference a slot a concurrent unprepare is freeing.
    struct _PHOTONIC_IOCTL_CAPTURE *IoctlCapture;

    /// Stream-class FDO at the top of this device's stack, captured from
    /// PORT_CONFIGURATION_INFORMATION.ClassDeviceObject during
    /// SRB_INITIALIZE_DEVICE. It is not an IRB target: the stream class driver
    /// does not handle IOCTL_1394_CLASS and would fail it with
    /// STATUS_INVALID_DEVICE_REQUEST. Kept as a reference to the stack top.
    PDEVICE_OBJECT ClassDeviceObject;

    /// The camera's physical device object (the 1394 unit PDO), captured from
    /// PORT_CONFIGURATION_INFORMATION.PhysicalDeviceObject. Every IRB (async
    /// register reads/writes, bus queries) is submitted here with IoCallDriver:
    /// the PDO's dispatch routines are owned by the 1394 bus driver, which
    /// services the IOCTL_1394_CLASS request interface. It also names the camera
    /// node as the destination of the GetMaxSpeedBetweenDevices query.
    PDEVICE_OBJECT PhysicalDeviceObject;

    /// Tracks every IRB submission in flight at the 1394 unit PDO, including
    /// requests the timeout path abandoned there. SRB_UNINITIALIZE_DEVICE
    /// waits on this lock before returning, so a wedged request that completes
    /// late still finds the completion routine's code and its request context
    /// in memory. Once that wait begins, new submissions fail with
    /// STATUS_DELETE_PENDING.
    IO_REMOVE_LOCK IrbRemoveLock;

    /// Full 48-bit base address of the camera's DCAM CSR register block in 1394
    /// node address space, recovered from the unit-dependent directory of the
    /// Configuration ROM. All DCAM register offsets are relative to this base.
    /// Zero until discovered.
    ULONGLONG CsrBaseAddress;

    /// 1394 bus generation count as of the last refresh. Every async transaction
    /// carries it; the bus driver rejects a transaction with
    /// STATUS_INVALID_GENERATION if it is stale. A registered bus-reset
    /// notification sets BusResetPending so the next transaction re-reads it.
    /// The camera's node is not addressed explicitly: the destination ID is left
    /// zero and the bus driver implies the node from the device stack the IRB
    /// travels down.
    ULONG BusGeneration;

    /// Host controller capabilities (GET_LOCAL_HOST_INFO level 2), the largest
    /// single DMA buffer the port driver accepts (level 7 MaxDmaBufferSize; 0
    /// when the query fails, making the capture engine fall back to
    /// PHOTONIC_CAPTURE_MAX_CHUNK_BYTES) and the maximum 1394 speed to the camera
    /// as an SCODE_*_RATE (GetMaxSpeedBetweenDevices), queried once during
    /// bring-up. The speed code is used for every async transaction; MaxSpeedCode
    /// defaults to SCODE_400_RATE if the query fails.
    ULONG HostCapabilities;
    ULONG MaxAsyncReadRequest;
    ULONG MaxAsyncWriteRequest;
    ULONG64 MaxDmaBufferSize;
    UCHAR MaxSpeedCode;

    /// Bus-reset bookkeeping. BusResetRegistered tracks whether the notification
    /// routine is registered (so it is deregistered exactly once at teardown).
    /// BusResetPending is set at DISPATCH_LEVEL from the notification routine and
    /// cleared at PASSIVE_LEVEL by the next async transaction, which re-reads the
    /// generation count before proceeding.
    BOOLEAN BusResetRegistered;
    volatile LONG BusResetPending;

    /// Video modes enumerated from the camera's inquiry registers. ModeCount is
    /// the number of valid entries in Modes[].
    PHOTONIC_VIDEO_MODE Modes[PHOTONIC_MAX_VIDEO_MODES];
    ULONG ModeCount;

    /// KS data ranges advertised on this camera's capture pin, built from Modes[]
    /// by PhotonicStreamFormatBuild. Formats[] holds pointers into VideoRanges[]
    /// for the stream information array; FormatCount is how many are valid.
    /// MaxSampleSize is the largest frame across the advertised modes, used to
    /// size the capture pin's allocator framing. VideoStreamInfo is the single
    /// capture output stream that references these formats. All per device: each
    /// camera advertises only the modes it actually supports, so this state must
    /// not be shared across cameras.
    KS_DATARANGE_VIDEO VideoRanges[PHOTONIC_MAX_VIDEO_MODES];
    PKSDATAFORMAT Formats[PHOTONIC_MAX_VIDEO_MODES];
    ULONG FormatCount;
    ULONG MaxSampleSize;
    HW_STREAM_INFORMATION VideoStreamInfo;

    /// For each advertised range (0..FormatCount-1), the index into Modes[] it
    /// was built from. The data path uses this to recover the DCAM (format, mode,
    /// coding) backing the format the pin negotiated, so it can program the camera
    /// and stream that exact mode.
    ULONG RangeModeIndex[PHOTONIC_MAX_VIDEO_MODES];

    /// DirectShow image controls discovered from the camera's DCAM feature
    /// registers, and the device (filter) property sets built from them by
    /// PhotonicBuildDevicePropertySets (properties.c): VideoControl always,
    /// VideoProcAmp / CameraControl only when at least one of their controls is
    /// present on the camera. ImagerPropertyItems holds the advertised property
    /// items, grouped per set; the sets point into it. All per device: the fake
    /// camera implements every control with the full 12-bit range while the real
    /// camera implements only a few, each with its own narrower range.
    PHOTONIC_IMAGER_FEATURE ImagerFeatures[PHOTONIC_IMAGER_FEATURE_COUNT];
    KSPROPERTY_ITEM ImagerPropertyItems[PHOTONIC_IMAGER_FEATURE_COUNT];
    KSPROPERTY_SET DevicePropertySets[PHOTONIC_DEVICE_PROPERTY_SET_MAX];
    ULONG DevicePropertySetCount;

    /// Cached device power state (updated on SRB_CHANGE_POWER_STATE).
    DEVICE_POWER_STATE DevicePowerState;

    /// Set by SRB_SURPRISE_REMOVAL before the data path is torn down. New reads
    /// fail immediately with STATUS_DEVICE_REMOVED (checked under the stream's
    /// PendingLock so none can park after the removal drain) and capture cannot
    /// be started.
    BOOLEAN Removed;

    /// Linkage on the global device list ioctl/ioctl.c uses to map the
    /// stream-class FDO a Photonic IOCTL targets back to this extension. Linked
    /// between PhotonicIoctlRegisterDevice (SRB_INITIALIZE_DEVICE) and
    /// PhotonicIoctlDeregisterDevice (SRB_UNINITIALIZE_DEVICE).
    LIST_ENTRY IoctlDeviceListEntry;

    /// PL_RETURN_CODE of the most recent failed Photonic IOCTL, committed by the
    /// IOCTL dispatcher (ioctl/ioctl.c) and returned to pixelinkapi.dll by
    /// PHOTONIC_IOCTL_GET_LAST_ERROR. Kept until the next failure (a successful
    /// IOCTL does not clear it); the DLL only reads it right after a failed call.
    PL_RETURN_CODE LastError;
} PHOTONIC_DEVICE_EXTENSION, *PPHOTONIC_DEVICE_EXTENSION;

/// Per-request workspace the class driver allocates for every SRB from
/// HW_INITIALIZATION_DATA.PerRequestExtensionSize and exposes through
/// Srb->SRBExtension. Used to thread a pended SRB_READ_DATA onto the stream's
/// PendingReads list while it waits for a frame; Srb points back at the owning
/// request so the delivery path can recover it from the list entry.
typedef struct _PHOTONIC_SRB_EXTENSION {
    LIST_ENTRY ListEntry;
    PHW_STREAM_REQUEST_BLOCK Srb;
    /// TRUE while the request is linked on PendingReads. Set on insert and
    /// cleared on removal, both under PendingLock, so exactly one of the
    /// acquire, drain, and cancel paths removes (and therefore completes) any
    /// given request. A read that is not parked is owned by the capture
    /// engine (or on its way into it), where a cancel is resolved by the
    /// control work item's quiesce.
    BOOLEAN Queued;
} PHOTONIC_SRB_EXTENSION, *PPHOTONIC_SRB_EXTENSION;

/// Per-stream context. The class driver allocates this from
/// HW_INITIALIZATION_DATA.PerStreamExtensionSize and exposes it through
/// StreamObject->HwStreamExtension.
typedef struct _PHOTONIC_STREAM_EXTENSION {
    /// Current KS state of the stream (Stop / Acquire / Pause / Run).
    KSSTATE StreamState;

    /// Geometry of the format the pin is connected with, captured from the
    /// connection format at SRB_OPEN_STREAM. The data path fills frames of
    /// exactly ImageSize bytes (capped to the caller's buffer) using these
    /// dimensions, so it stays correct whichever enumerated mode was negotiated.
    ULONG Width;
    ULONG Height;
    ULONG BitCount;
    ULONG ImageSize;
    ULONG Compression;

    /// Negotiated frame interval (100ns units) reported back on each delivered
    /// frame as its duration.
    ULONG FrameInterval;

    /// TRUE when the stream was started in single-frame mode (start value 0 in
    /// START_VIDEO / START_IMAGER): PhotonicCaptureStart must not enable
    /// continuous transmission (ISO_EN); each acquisition is individually armed
    /// through the ONE_SHOT register, and the camera captures one frame,
    /// transmits it and stops itself. Always FALSE on the DirectShow path.
    BOOLEAN OneShot;

    /// Running count of frames delivered to the renderer, used to fill the
    /// KS_FRAME_INFO PictureNumber on each completed SRB_READ_DATA. Claimed
    /// with InterlockedIncrement (two detach completions can finish
    /// concurrently on different CPUs) by delivered frames only, and reset
    /// when a run transition starts a new capture session.
    ULONG FrameNumber;

    /// The enumerated DCAM mode the pin negotiated (a stable pointer into
    /// Extension->Modes), recovered from the connection format at SRB_OPEN_STREAM,
    /// and the DCAM frame-rate id chosen for it. The capture engine programs the
    /// camera from these. Mode is NULL when the open format matched no mode (the
    /// data path then stays idle).
    PPHOTONIC_VIDEO_MODE Mode;
    ULONG RateId;

    /// Isochronous capture engine for this stream (capture.h). Allocated at
    /// SRB_OPEN_STREAM and freed at SRB_CLOSE_STREAM, so it is stable and
    /// non-NULL for the life of the open stream; the isochronous resources it
    /// manages are held only between KSSTATE_RUN and KSSTATE_STOP.
    struct _PHOTONIC_CAPTURE *Capture;

    /// Parked reads. SRB_READ_DATA parks every read here and runs the engine's
    /// pump, which pulls parked reads through the stream's acquire callback
    /// and attaches their buffers (the whole data path is DISPATCH-safe); a
    /// read stays parked under back-pressure (every descriptor in the engine's
    /// pool is attached) or while the engine is idle, and is pulled again from
    /// the engine's detach completion when a descriptor recycles. Guarded by
    /// PendingLock; both persist across capture start/stop.
    LIST_ENTRY PendingReads;
    KSPIN_LOCK PendingLock;

    /// Data-path gate. With TurnOffSynchronization the class driver can run
    /// the data and cancel handlers concurrently with SRB_CLOSE_STREAM, and
    /// completing a data SRB does not mean its dispatching thread has left
    /// the handler, so the close cannot infer from SRB accounting that no
    /// thread still holds the Capture pointer it is about to free. The gate
    /// makes it an invariant instead: DataPathRefs counts threads inside the
    /// data and cancel handlers (PhotonicStreamEnterDataPath /
    /// PhotonicStreamLeaveDataPath), Rundown refuses new entries once the
    /// close has begun, and DataPathIdle is set when the last thread leaves
    /// after Rundown. The close publishes the stopped state, sets Rundown,
    /// and waits for the count to drain before it destroys the engine. All
    /// transitions run under PendingLock; only the close's wait blocks, at
    /// PASSIVE_LEVEL.
    LONG DataPathRefs;
    BOOLEAN Rundown;
    KEVENT DataPathIdle;
} PHOTONIC_STREAM_EXTENSION, *PPHOTONIC_STREAM_EXTENSION;

/// Initialize the stream-extension fields the capture engine relies on
/// whichever front-end owns the stream: the stopped state, the parked-read
/// list and its lock. The DirectShow open and the IOCTL prepare both call
/// this over zeroed memory, so the two front-ends cannot drift apart on the
/// read-queue contract. Front-end-specific fields (the DirectShow data-path
/// gate) stay with their front-end.
///
/// @param Stream  Zeroed stream extension to initialize.
FORCEINLINE VOID PhotonicStreamExtensionInit(_Inout_ PPHOTONIC_STREAM_EXTENSION Stream) {
    Stream->StreamState = KSSTATE_STOP;
    InitializeListHead(&Stream->PendingReads);
    KeInitializeSpinLock(&Stream->PendingLock);
}

/// DriverEntry must have C linkage so the CRT's GsDriverEntry can find it.
EXTERN_C DRIVER_INITIALIZE DriverEntry;

/// Device-level SRB handlers (dispatch.c). HwReceivePacket is the entry point
/// the class driver calls for every device/instance SRB.
VOID STREAMAPI PhotonicReceivePacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
VOID STREAMAPI PhotonicCancelPacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
VOID STREAMAPI PhotonicTimeout(_In_ PHW_STREAM_REQUEST_BLOCK Srb);

/// Stream-level SRB handlers (stream/). These are installed in the stream
/// object by PhotonicStreamOpen and invoked by the class driver per stream.
VOID STREAMAPI PhotonicStreamReceiveDataPacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
VOID STREAMAPI PhotonicStreamReceiveControlPacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb);

/// Read-delivery plumbing (stream/read.c), shared with the capture engine.
/// PhotonicStreamCompletePendingReads drains every parked read (completing it
/// with zero bytes) and is called when capture stops, on flush, and at close.
/// PhotonicStreamCancelRead completes a parked SRB cancelled directly; a read
/// that is not parked is owned by the capture engine (or on its way into it)
/// and is resolved by the control work item.
VOID PhotonicStreamCompletePendingReads(_In_ PPHOTONIC_STREAM_EXTENSION Stream);
VOID PhotonicStreamCancelRead(_In_ PPHOTONIC_STREAM_EXTENSION Stream, _In_ PHW_STREAM_REQUEST_BLOCK Srb);

/// Data-path gate (stream/read.c; see the gate fields in
/// PHOTONIC_STREAM_EXTENSION). Enter returns FALSE once the close has begun,
/// and the caller must then not touch the stream extension or the capture
/// engine. Both are safe at DISPATCH_LEVEL.
BOOLEAN PhotonicStreamEnterDataPath(_In_ PPHOTONIC_STREAM_EXTENSION Stream);
VOID PhotonicStreamLeaveDataPath(_In_ PPHOTONIC_STREAM_EXTENSION Stream);

/// Stream-descriptor helpers (stream/) called from the device SRB dispatcher.
/// PhotonicStreamFormatBuild turns the modes enumerated into the device extension
/// into the KS data ranges the capture pin advertises; it must run before
/// SRB_GET_STREAM_INFO. If no enumerated mode maps to a usable format the camera
/// advertises no formats at all rather than a built-in fallback.
VOID PhotonicStreamFormatBuild(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);
NTSTATUS PhotonicStreamGetInfo(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
NTSTATUS PhotonicStreamOpen(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
NTSTATUS PhotonicStreamClose(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
NTSTATUS PhotonicStreamGetDataIntersection(_In_ PHW_STREAM_REQUEST_BLOCK Srb);

/// Photonic IOCTL interface (ioctl/ioctl.c). The stream class driver owns the
/// WDM dispatch table, so PhotonicHookDeviceControl (called once from DriverEntry
/// after StreamClassRegisterAdapter) replaces the driver object's
/// IRP_MJ_DEVICE_CONTROL handler with one that services the PHOTONIC_IOCTL_*
/// codes and forwards everything else to the class driver's original handler, and
/// its IRP_MJ_CLEANUP handler with one that releases the IOCTL capture context
/// when the handle that prepared it closes. The IOCTLs arrive on the stream-class
/// FDO, whose device extension belongs to the class driver;
/// PhotonicIoctlRegisterDevice / PhotonicIoctlDeregisterDevice maintain the FDO
/// -> PHOTONIC_DEVICE_EXTENSION mapping the hook uses to recover this
/// minidriver's context.
VOID PhotonicHookDeviceControl(_In_ PDRIVER_OBJECT DriverObject);
VOID PhotonicIoctlRegisterDevice(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);
VOID PhotonicIoctlDeregisterDevice(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// IOCTL-path capture release (ioctl/frames.c): stop the capture and free the
/// slot (Extension->IoctlCapture set back to NULL). Called from the IOCTL
/// unprepare handlers, the video re-prepare (which rebuilds a stopped slot),
/// the cleanup hook, and SRB_UNINITIALIZE_DEVICE (after the device left the
/// IOCTL routing), so a client that never sent an unprepare leaks nothing.
/// Idempotent; must run at PASSIVE_LEVEL with Extension->InterfaceMutex held.
VOID PhotonicIoctlCaptureRelease(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

#endif // PHOTONIC_H
