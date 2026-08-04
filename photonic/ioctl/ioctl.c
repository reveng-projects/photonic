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
/// Photonic IOCTL dispatch for the Photonic minidriver.
///
/// The stream class driver (stream.sys) owns the driver object's WDM dispatch
/// table after StreamClassRegisterAdapter, so the Photonic IOCTLs cannot be
/// routed through an SRB handler: the class driver fails IOCTLs it does not
/// recognize before the minidriver ever sees them. PhotonicHookDeviceControl
/// therefore replaces DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] with
/// PhotonicDeviceControl, which services the PHOTONIC_IOCTL_* codes (ioctl.h)
/// and forwards every other request -- most importantly the KS
/// property/streaming IOCTLs -- to the class driver's original handler.
/// IRP_MJ_CLEANUP is hooked the same way, so the IOCTL capture context is
/// torn down when the handle that prepared it closes.
///
/// The IOCTLs arrive on the stream-class FDO, whose device extension belongs
/// to the class driver, not to this minidriver. A global device list maps the
/// FDO back to the PHOTONIC_DEVICE_EXTENSION; SRB_INITIALIZE_DEVICE registers
/// the mapping and SRB_UNINITIALIZE_DEVICE removes it. An IRP whose FDO has
/// no mapping is forwarded untouched.
///
/// The handlers live in the sibling files of this folder, grouped by
/// functional area (identity.c, format.c, frames.c, video.c, imager.c,
/// framerate.c, trigger.c, property.c, mailbox.c) and declared in
/// ioctl_private.h. A handler not yet implemented traces the call and fails
/// the request with STATUS_NOT_IMPLEMENTED. The buffer layouts each handler
/// must implement are documented per IOCTL in ioctl.h.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and ioctl.tmh (WPP-generated) must come after it. ioctl.h needs
// CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "ioctl.tmh"
// clang-format on

/// The class driver's original IRP_MJ_DEVICE_CONTROL and IRP_MJ_CLEANUP
/// handlers, saved by PhotonicHookDeviceControl. Every IRP that is not a
/// Photonic IOCTL targeting a registered photonic FDO is passed on to the
/// former; every cleanup is passed on to the latter after the IOCTL capture
/// held by the closing handle (if any) has been released.
static PDRIVER_DISPATCH g_PhotonicNextDeviceControl;
static PDRIVER_DISPATCH g_PhotonicNextCleanup;

/// Registered devices: PHOTONIC_DEVICE_EXTENSIONs linked through their
/// IoctlDeviceListEntry, keyed by Extension->ClassDeviceObject. Guarded by
/// g_PhotonicIoctlDevicesLock; both initialized in PhotonicHookDeviceControl
/// before the hook can execute.
static LIST_ENTRY g_PhotonicIoctlDevices;
static KSPIN_LOCK g_PhotonicIoctlDevicesLock;

/// IOCTL code -> handler map, including the I/O contract from ioctl.h:
/// MinInputLength / MinOutputLength are the sizes of the structures the
/// handler reads from and writes back to the shared buffer, enforced by the
/// dispatcher before the handler runs (short buffers fail with
/// STATUS_BUFFER_TOO_SMALL / PL_ERROR_INVALID_COUNT). A minimum of 0 means
/// the direction carries no payload, or that its size is variable / conditional
/// and the handler validates it against the actual length itself.
///
/// PHOTONIC_IOCTL_GET_NAMES is deliberately absent: it is serviced by the
/// separate "\\.\vitdcam" control device, not by this driver's dispatch (see
/// ioctl.h), so it is forwarded like any foreign IOCTL.
typedef struct _PHOTONIC_IOCTL_DISPATCH_ENTRY {
    ULONG IoControlCode;
    const char *Name;
    PHOTONIC_IOCTL_HANDLER *Handler;
    ULONG MinInputLength;
    ULONG MinOutputLength;
    BOOLEAN Requires32BitCaller; ///< TRUE when the input embeds a 32-bit user pointer or handle (PHOTONIC_*_IN32); only
                                 ///< 32-bit (WOW64) callers are accepted. Omitted entries default to FALSE.
} PHOTONIC_IOCTL_DISPATCH_ENTRY;

static const PHOTONIC_IOCTL_DISPATCH_ENTRY g_PhotonicIoctlDispatch[] = {
    {PHOTONIC_IOCTL_SDK_VERSION_1, "SDK_VERSION_1", PhotonicIoctlSdkVersion, 0, 0},
    {PHOTONIC_IOCTL_SDK_VERSION_2, "SDK_VERSION_2", PhotonicIoctlSdkVersion, 0, 0},
    {PHOTONIC_IOCTL_SERIAL_NUMBER, "SERIAL_NUMBER", PhotonicIoctlSerialNumber, sizeof(PHOTONIC_SERIAL_NUMBER_IN),
     sizeof(PHOTONIC_SERIAL_NUMBER_OUT)},
    //
    // SET_CANCEL_TIMEOUT: the Status echo is written only when the caller
    // supplied room for it, so no output minimum is enforced.
    //
    {PHOTONIC_IOCTL_SET_CANCEL_TIMEOUT, "SET_CANCEL_TIMEOUT", PhotonicIoctlSetCancelTimeout,
     sizeof(PHOTONIC_SET_CANCEL_TIMEOUT_IN), 0},
    //
    // GET_VENDOR_NAME / GET_MODEL_NAME: variable-length string output; the
    // handler checks the actual length against the name it returns.
    //
    {PHOTONIC_IOCTL_GET_VENDOR_NAME, "GET_VENDOR_NAME", PhotonicIoctlGetVendorName, 0, 0},
    {PHOTONIC_IOCTL_GET_MODEL_NAME, "GET_MODEL_NAME", PhotonicIoctlGetModelName, 0, 0},
    {PHOTONIC_IOCTL_SUBWINDOW_GET, "SUBWINDOW_GET", PhotonicIoctlSubwindowGet, 0, sizeof(PHOTONIC_SUBWINDOW_GET_OUT)},
    {PHOTONIC_IOCTL_SUBWINDOW_SET, "SUBWINDOW_SET", PhotonicIoctlSubwindowSet, sizeof(PHOTONIC_SUBWINDOW_SET_IN), 0},
    //
    // START_VIDEO: the DLL passes out-length 0, so the Status echo is written
    // only when the caller supplied room for it and no output minimum is
    // enforced.
    //
    {PHOTONIC_IOCTL_START_VIDEO, "START_VIDEO", PhotonicIoctlStartVideo, sizeof(PHOTONIC_START_VIDEO_IN), 0},
    {PHOTONIC_IOCTL_STOP_VIDEO, "STOP_VIDEO", PhotonicIoctlStopVideo, 0, 0},
    //
    // REGISTER_EVENT / UNREGISTER_EVENT: the DLL passes out-length 0 (the
    // Status echo is never returned), so no output minimum is enforced. The
    // input carries a 32-bit user-mode event handle, so only 32-bit callers
    // are accepted.
    //
    {PHOTONIC_IOCTL_REGISTER_EVENT, "REGISTER_EVENT", PhotonicIoctlRegisterEvent, sizeof(PHOTONIC_REGISTER_EVENT_IN32),
     0, TRUE},
    {PHOTONIC_IOCTL_UNREGISTER_EVENT, "UNREGISTER_EVENT", PhotonicIoctlUnregisterEvent,
     sizeof(PHOTONIC_UNREGISTER_EVENT_IN32), 0, TRUE},
    {PHOTONIC_IOCTL_GET_TRANSFER_INFO, "GET_TRANSFER_INFO", PhotonicIoctlGetTransferInfo, 0,
     sizeof(PHOTONIC_GET_TRANSFER_INFO_OUT)},
    //
    // PREPARE_VIDEO: the DLL passes out-length 0, so the Status echo is
    // written only when the caller supplied room for it and no output minimum
    // is enforced.
    //
    {PHOTONIC_IOCTL_PREPARE_VIDEO, "PREPARE_VIDEO", PhotonicIoctlPrepareVideo, sizeof(PHOTONIC_PREPARE_VIDEO_IN), 0},
    {PHOTONIC_IOCTL_UNPREPARE_VIDEO, "UNPREPARE_VIDEO", PhotonicIoctlUnprepareVideo, 0, 0},
    //
    // MAP_VIDEO_FRAME: the DLL passes out-length 0, so the Status echo is
    // written only when the caller supplied room for it and no output minimum
    // is enforced. The input carries a 32-bit user-mode base VA, so only
    // 32-bit callers are accepted.
    //
    {PHOTONIC_IOCTL_MAP_VIDEO_FRAME, "MAP_VIDEO_FRAME", PhotonicIoctlMapVideoFrame,
     sizeof(PHOTONIC_MAP_VIDEO_FRAME_IN32), 0, TRUE},
    {PHOTONIC_IOCTL_UNMAP_VIDEO_FRAME, "UNMAP_VIDEO_FRAME", PhotonicIoctlUnmapVideoFrame, 0, 0},
    {PHOTONIC_IOCTL_INITIALIZE, "INITIALIZE", PhotonicIoctlInitialize, 0, 0},
    {PHOTONIC_IOCTL_PROPERTY_GET, "PROPERTY_GET", PhotonicIoctlPropertyGet, sizeof(PHOTONIC_PROPERTY_IN),
     sizeof(PHOTONIC_PROPERTY_OUT)},
    {PHOTONIC_IOCTL_PROPERTY_SET, "PROPERTY_SET", PhotonicIoctlPropertySet, sizeof(PHOTONIC_PROPERTY_SET_IN), 0},
    {PHOTONIC_IOCTL_TRIGGER_SET, "TRIGGER_SET", PhotonicIoctlTriggerSet, sizeof(PHOTONIC_TRIGGER_SET_IN),
     sizeof(PHOTONIC_TRIGGER_SET_OUT)},
    {PHOTONIC_IOCTL_STROBE_SET, "STROBE_SET", PhotonicIoctlStrobeSet, sizeof(PHOTONIC_STROBE_SET_IN),
     sizeof(PHOTONIC_STROBE_SET_OUT)},
    {PHOTONIC_IOCTL_SW_TRIGGER, "SW_TRIGGER", PhotonicIoctlSwTrigger, 0, 0},
    {PHOTONIC_IOCTL_ENUM_FRAME_RATE, "ENUM_FRAME_RATE", PhotonicIoctlEnumFrameRate, sizeof(PHOTONIC_ENUM_FRAME_RATE_IN),
     sizeof(PHOTONIC_ENUM_FRAME_RATE_OUT)},
    {PHOTONIC_IOCTL_SET_FRAME_RATE, "SET_FRAME_RATE", PhotonicIoctlSetFrameRate, sizeof(PHOTONIC_SET_FRAME_RATE_IN), 0},
    {PHOTONIC_IOCTL_IMAGE_FLIP, "IMAGE_FLIP", PhotonicIoctlImageFlip, sizeof(PHOTONIC_IMAGE_FLIP_IN), 0},
    {PHOTONIC_IOCTL_GET_FRAME_RATE, "GET_FRAME_RATE", PhotonicIoctlGetFrameRate, 0,
     sizeof(PHOTONIC_GET_FRAME_RATE_OUT)},
    {PHOTONIC_IOCTL_GET_CHANNEL, "GET_CHANNEL", PhotonicIoctlGetChannel, 0, sizeof(PHOTONIC_GET_CHANNEL_OUT)},
    {PHOTONIC_IOCTL_GET_PACKET_SIZE, "GET_PACKET_SIZE", PhotonicIoctlGetPacketSize, 0,
     sizeof(PHOTONIC_GET_PACKET_SIZE_OUT)},
    {PHOTONIC_IOCTL_GET_PIXEL_FORMAT, "GET_PIXEL_FORMAT", PhotonicIoctlGetPixelFormat, 0,
     sizeof(PHOTONIC_GET_PIXEL_FORMAT_OUT)},
    //
    // SET_PIXEL_FORMAT: the DLL passes out-length 0 (the Status echo is never
    // returned), so no output minimum is enforced.
    //
    {PHOTONIC_IOCTL_SET_PIXEL_FORMAT, "SET_PIXEL_FORMAT", PhotonicIoctlSetPixelFormat,
     sizeof(PHOTONIC_SET_PIXEL_FORMAT_IN), 0},
    {PHOTONIC_IOCTL_PREPARE_IMAGER, "PREPARE_IMAGER", PhotonicIoctlPrepareImager, sizeof(PHOTONIC_PREPARE_IMAGER_IN),
     0},
    {PHOTONIC_IOCTL_UNPREPARE_IMAGER, "UNPREPARE_IMAGER", PhotonicIoctlUnprepareImager, 0, 0},
    {PHOTONIC_IOCTL_START_IMAGER, "START_IMAGER", PhotonicIoctlStartImager, sizeof(PHOTONIC_START_IMAGER_IN), 0},
    {PHOTONIC_IOCTL_STOP_IMAGER, "STOP_IMAGER", PhotonicIoctlStopImager, 0, 0},
    {PHOTONIC_IOCTL_GET_DCAM_VERSION, "GET_DCAM_VERSION", PhotonicIoctlGetDcamVersion, 0,
     sizeof(PHOTONIC_GET_DCAM_VERSION_OUT)},
    //
    // GET_NAMES_LENGTH: both output words are optional (buf[0] written when the
    // caller supplied >= 4 bytes, buf[1] when >= 8), so the handler checks.
    //
    {PHOTONIC_IOCTL_GET_NAMES_LENGTH, "GET_NAMES_LENGTH", PhotonicIoctlGetNamesLength, 0, 0},
    {PHOTONIC_IOCTL_INVALIDATE_FORMAT, "INVALIDATE_FORMAT", PhotonicIoctlInvalidateCurrentFormat, 0, 0},
    {PHOTONIC_IOCTL_GET_LAST_ERROR, "GET_LAST_ERROR", PhotonicIoctlGetLastError, 0,
     sizeof(PHOTONIC_GET_LAST_ERROR_OUT)},
    //
    // MAILBOX: multiplexed; only the command header is common. The handler
    // validates the per-command payload sizes against the actual lengths.
    //
    {PHOTONIC_IOCTL_MAILBOX, "MAILBOX", PhotonicIoctlMailbox, sizeof(PHOTONIC_MAILBOX_CMD_HDR), 0},
};

/// Find the dispatch entry for an IOCTL code.
///
/// @param IoControlCode  IOCTL code to look up.
/// @return Pointer to the matching dispatch entry, or NULL if the code is not
///         a Photonic IOCTL.
static const PHOTONIC_IOCTL_DISPATCH_ENTRY *PhotonicIoctlLookup(_In_ ULONG IoControlCode) {
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(g_PhotonicIoctlDispatch); i++) {
        if (g_PhotonicIoctlDispatch[i].IoControlCode == IoControlCode) {
            return &g_PhotonicIoctlDispatch[i];
        }
    }

    return NULL;
}

/// Return TRUE when the IRP originates from a 32-bit process.
/// IoIs32bitProcess only exists on 64-bit kernels; on a 32-bit build every
/// caller is 32-bit.
///
/// @param Irp  The I/O request packet to test.
/// @return TRUE if the caller is a 32-bit process, FALSE otherwise.
static BOOLEAN PhotonicIoctlIs32BitCaller(_In_ PIRP Irp) {
#if defined(_WIN64)
    return IoIs32bitProcess(Irp) ? TRUE : FALSE;
#else
    UNREFERENCED_PARAMETER(Irp);
    return TRUE;
#endif
}

/// Map a stream-class FDO to the registered PHOTONIC_DEVICE_EXTENSION.
///
/// @param DeviceObject  FDO to look up.
/// @return Pointer to the matching device extension, or NULL if the FDO is not
///         registered.
static PPHOTONIC_DEVICE_EXTENSION PhotonicIoctlFindDevice(_In_ PDEVICE_OBJECT DeviceObject) {
    PPHOTONIC_DEVICE_EXTENSION found = NULL;
    PLIST_ENTRY entry;
    KIRQL irql;

    KeAcquireSpinLock(&g_PhotonicIoctlDevicesLock, &irql);

    for (entry = g_PhotonicIoctlDevices.Flink; entry != &g_PhotonicIoctlDevices; entry = entry->Flink) {
        PPHOTONIC_DEVICE_EXTENSION extension =
            CONTAINING_RECORD(entry, PHOTONIC_DEVICE_EXTENSION, IoctlDeviceListEntry);

        if (extension->ClassDeviceObject == DeviceObject) {
            found = extension;
            break;
        }
    }

    KeReleaseSpinLock(&g_PhotonicIoctlDevicesLock, irql);

    return found;
}

/// Link a device extension on the FDO map. Called from SRB_INITIALIZE_DEVICE
/// once ClassDeviceObject is captured.
///
/// @param Extension  Device extension to register.
VOID PhotonicIoctlRegisterDevice(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    KIRQL irql;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "fdo=%p extension=%p\n", Extension->ClassDeviceObject,
                Extension);

    KeAcquireSpinLock(&g_PhotonicIoctlDevicesLock, &irql);
    InsertTailList(&g_PhotonicIoctlDevices, &Extension->IoctlDeviceListEntry);
    KeReleaseSpinLock(&g_PhotonicIoctlDevicesLock, irql);
}

/// Unlink a device extension from the FDO map. Called from
/// SRB_UNINITIALIZE_DEVICE; after this no Photonic IOCTL reaches the
/// extension anymore. Safe on an extension that was never registered:
/// SRB_INITIALIZE_DEVICE can fail before PhotonicIoctlRegisterDevice runs
/// (the work-item allocation), and unlinking the still-zeroed list entry
/// would write through its NULL links.
///
/// @param Extension  Device extension to deregister.
VOID PhotonicIoctlDeregisterDevice(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    KIRQL irql;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "fdo=%p extension=%p\n", Extension->ClassDeviceObject,
                Extension);

    if (Extension->IoctlDeviceListEntry.Flink == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "extension was never registered; nothing to unlink\n");
        return;
    }

    KeAcquireSpinLock(&g_PhotonicIoctlDevicesLock, &irql);
    RemoveEntryList(&Extension->IoctlDeviceListEntry);
    Extension->IoctlDeviceListEntry.Flink = NULL;
    Extension->IoctlDeviceListEntry.Blink = NULL;
    KeReleaseSpinLock(&g_PhotonicIoctlDevicesLock, irql);
}

/// Hooked IRP_MJ_DEVICE_CONTROL dispatch. A Photonic IOCTL targeting a
/// registered photonic FDO is decoded (METHOD_BUFFERED shared system buffer)
/// and dispatched through g_PhotonicIoctlDispatch; everything else is
/// forwarded verbatim to the stream class driver's original handler.
///
/// @param DeviceObject  The FDO receiving the request.
/// @param Irp           The device-control IRP.
/// @return NTSTATUS from the handler or the forwarded call.
static NTSTATUS PhotonicDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    const PHOTONIC_IOCTL_DISPATCH_ENTRY *entry;
    PPHOTONIC_DEVICE_EXTENSION extension;
    PHOTONIC_IOCTL_REQUEST request;
    NTSTATUS status;

    //
    // Fast path: every Photonic IOCTL uses FILE_DEVICE_UNKNOWN, so KS traffic
    // (FILE_DEVICE_KS) skips the table scan entirely.
    //
    if (DEVICE_TYPE_FROM_CTL_CODE(code) != FILE_DEVICE_UNKNOWN) {
        return g_PhotonicNextDeviceControl(DeviceObject, Irp);
    }

    entry = PhotonicIoctlLookup(code);
    if (entry == NULL) {
        return g_PhotonicNextDeviceControl(DeviceObject, Irp);
    }

    extension = PhotonicIoctlFindDevice(DeviceObject);
    if (extension == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL, "%s (0x%x) on unregistered fdo=%p; forwarding\n",
                    entry->Name, code, DeviceObject);
        return g_PhotonicNextDeviceControl(DeviceObject, Irp);
    }

    request.Extension = extension;
    request.FileObject = irpSp->FileObject;
    request.Buffer = Irp->AssociatedIrp.SystemBuffer;
    request.InputLength = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    request.OutputLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    request.Information = 0;
    request.Error = PL_SUCCESS;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "%s (0x%x): inLen=%u outLen=%u\n", entry->Name, code,
                request.InputLength, request.OutputLength);

    //
    // Serialize with every other Photonic IOCTL, the cleanup hook and the SRB
    // open/teardown paths. Handlers read Extension->IoctlCapture without
    // further locking; the mutex is what keeps that pointer (and the slot
    // behind it) alive for the duration of the call. The contract checks and
    // the LastError commit share the hold, so a concurrent
    // PHOTONIC_IOCTL_GET_LAST_ERROR on another handle observes a request and
    // its committed error code atomically. IOCTL dispatch runs at
    // PASSIVE_LEVEL, so the wait is legal.
    //
    // The contract itself comes from the dispatch table: an IOCTL whose input
    // embeds a 32-bit user pointer/handle is rejected unless the caller is a
    // 32-bit (WOW64) process, and the handler only runs when the caller
    // supplied at least the documented input and output structures, so
    // handlers access them without re-checking.
    //
    KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);
    if (entry->Requires32BitCaller && !PhotonicIoctlIs32BitCaller(Irp)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "%s (0x%x) rejected: input embeds a 32-bit user pointer/handle but the caller is a 64-bit "
                    "process\n",
                    entry->Name, code);
        status = STATUS_NOT_SUPPORTED;
    } else if (request.InputLength < entry->MinInputLength || request.OutputLength < entry->MinOutputLength) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "buffer too small: inLen=%u (min %u) outLen=%u (min %u)\n",
                    request.InputLength, entry->MinInputLength, request.OutputLength, entry->MinOutputLength);
        request.Error = PL_ERROR_INVALID_COUNT;
        status = STATUS_BUFFER_TOO_SMALL;
    } else {
        status = entry->Handler(&request);
    }

    //
    // Commit the PL error code so PHOTONIC_IOCTL_GET_LAST_ERROR can report it
    // to pixelinkapi.dll.
    //
    if (request.Error != PL_SUCCESS) {
        extension->LastError = request.Error;
    }
    KeReleaseMutex(&extension->InterfaceMutex, FALSE);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL, "%s -> %!STATUS! info=%u plerror=%u\n", entry->Name, status,
                (ULONG) request.Information, (ULONG) request.Error);

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = request.Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

/// Hooked IRP_MJ_CLEANUP dispatch. Cleanup arrives when the last handle to a
/// file object closes; when that file object owns the IOCTL capture slot (it
/// issued PREPARE_VIDEO or PREPARE_IMAGER), the slot is released -- camera
/// stopped (including an armed one-shot cancelled), isochronous resources
/// freed, ring pages unlocked -- before the IRP is forwarded to the class
/// driver. This is the backstop for a client that exits without stop /
/// unprepare: without it the camera keeps transmitting and the channel,
/// bandwidth and resource handle stay allocated until the device is torn down.
/// Cleanup on any other handle (the DirectShow pins included) is forwarded
/// untouched.
///
/// @param DeviceObject  The FDO receiving the request.
/// @param Irp           The cleanup IRP.
/// @return NTSTATUS from the forwarded cleanup call.
static NTSTATUS PhotonicIoctlCleanup(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    PPHOTONIC_DEVICE_EXTENSION extension;

    extension = PhotonicIoctlFindDevice(DeviceObject);
    if (extension != NULL) {
        //
        // The ownership test and the release must be one atomic step: without
        // the mutex a concurrent unprepare could free the slot between them,
        // or a handler still inside the slot could be pulled out from under
        // this release.
        //
        KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);
        if (PhotonicIoctlCaptureIsOwner(extension, irpSp->FileObject)) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL,
                        "cleanup on the preparing handle (fileobj=%p); releasing IOCTL capture\n", irpSp->FileObject);
            PhotonicIoctlCaptureRelease(extension);
        }
        KeReleaseMutex(&extension->InterfaceMutex, FALSE);
    }

    return g_PhotonicNextCleanup(DeviceObject, Irp);
}

/// Install PhotonicDeviceControl and PhotonicIoctlCleanup in front of the
/// IRP_MJ_DEVICE_CONTROL and IRP_MJ_CLEANUP handlers the stream class driver
/// put in the dispatch table. Called once from DriverEntry, after
/// StreamClassRegisterAdapter succeeded and before any device can receive an
/// IRP.
///
/// @param DriverObject  The driver object whose dispatch table is patched.
VOID PhotonicHookDeviceControl(_In_ PDRIVER_OBJECT DriverObject) {
    FuncEntry(TRACE_FLAG_IOCTL);

    InitializeListHead(&g_PhotonicIoctlDevices);
    KeInitializeSpinLock(&g_PhotonicIoctlDevicesLock);

    g_PhotonicNextDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PhotonicDeviceControl;

    g_PhotonicNextCleanup = DriverObject->MajorFunction[IRP_MJ_CLEANUP];
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = PhotonicIoctlCleanup;
}
