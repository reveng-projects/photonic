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
/// Device-level SRB dispatch for the Photonic stream minidriver.
///
/// PhotonicReceivePacket is the HwReceivePacket routine registered in
/// DriverEntry. The stream class driver calls it for every device/instance SRB
/// (initialize, power, open/close stream, stream-info, ...). Each SRB is handled
/// synchronously: the handler sets Srb->Status, then the dispatcher signals
/// readiness for the next request and completes the SRB.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and dispatch.tmh (WPP-generated) must come after it.
#include "photonic.h"
#include "properties.h"
#include "p1394.h"
#include "dcam.h"
#include "capture.h"
#include "dispatch.tmh"
// clang-format on

/// PhotonicSrbCommandName -- map an SRB Command code to a short name so the
/// dispatch traces are readable in WinDbg / TraceView. Covers both the device
/// and stream command codes (they share the enumeration).
///
/// @param Command  SRB Command code to look up.
/// @return         A short string literal naming the command, or `"SRB_<unknown>"`.
const char *PhotonicSrbCommandName(_In_ ULONG Command) {
    switch (Command) {
        case SRB_INITIALIZE_DEVICE:
            return "SRB_INITIALIZE_DEVICE";
        case SRB_UNINITIALIZE_DEVICE:
            return "SRB_UNINITIALIZE_DEVICE";
        case SRB_OPEN_STREAM:
            return "SRB_OPEN_STREAM";
        case SRB_CLOSE_STREAM:
            return "SRB_CLOSE_STREAM";
        case SRB_OPEN_DEVICE_INSTANCE:
            return "SRB_OPEN_DEVICE_INSTANCE";
        case SRB_CLOSE_DEVICE_INSTANCE:
            return "SRB_CLOSE_DEVICE_INSTANCE";
        case SRB_GET_STREAM_INFO:
            return "SRB_GET_STREAM_INFO";
        case SRB_GET_DATA_INTERSECTION:
            return "SRB_GET_DATA_INTERSECTION";
        case SRB_GET_DEVICE_PROPERTY:
            return "SRB_GET_DEVICE_PROPERTY";
        case SRB_SET_DEVICE_PROPERTY:
            return "SRB_SET_DEVICE_PROPERTY";
        case SRB_CHANGE_POWER_STATE:
            return "SRB_CHANGE_POWER_STATE";
        case SRB_SURPRISE_REMOVAL:
            return "SRB_SURPRISE_REMOVAL";
        case SRB_INITIALIZATION_COMPLETE:
            return "SRB_INITIALIZATION_COMPLETE";
        case SRB_UNKNOWN_DEVICE_COMMAND:
            return "SRB_UNKNOWN_DEVICE_COMMAND";
        case SRB_PAGING_OUT_DRIVER:
            return "SRB_PAGING_OUT_DRIVER";
        case SRB_DEVICE_METHOD:
            return "SRB_DEVICE_METHOD";
        case SRB_STREAM_METHOD:
            return "SRB_STREAM_METHOD";
        case SRB_NOTIFY_IDLE_STATE:
            return "SRB_NOTIFY_IDLE_STATE";
        case SRB_SET_STREAM_STATE:
            return "SRB_SET_STREAM_STATE";
        case SRB_GET_STREAM_STATE:
            return "SRB_GET_STREAM_STATE";
        case SRB_SET_DATA_FORMAT:
            return "SRB_SET_DATA_FORMAT";
        case SRB_GET_DATA_FORMAT:
            return "SRB_GET_DATA_FORMAT";
        case SRB_PROPOSE_DATA_FORMAT:
            return "SRB_PROPOSE_DATA_FORMAT";
        case SRB_GET_STREAM_PROPERTY:
            return "SRB_GET_STREAM_PROPERTY";
        case SRB_SET_STREAM_PROPERTY:
            return "SRB_SET_STREAM_PROPERTY";
        case SRB_SET_STREAM_RATE:
            return "SRB_SET_STREAM_RATE";
        case SRB_PROPOSE_STREAM_RATE:
            return "SRB_PROPOSE_STREAM_RATE";
        case SRB_OPEN_MASTER_CLOCK:
            return "SRB_OPEN_MASTER_CLOCK";
        case SRB_INDICATE_MASTER_CLOCK:
            return "SRB_INDICATE_MASTER_CLOCK";
        case SRB_CLOSE_MASTER_CLOCK:
            return "SRB_CLOSE_MASTER_CLOCK";
        case SRB_BEGIN_FLUSH:
            return "SRB_BEGIN_FLUSH";
        case SRB_END_FLUSH:
            return "SRB_END_FLUSH";
        case SRB_READ_DATA:
            return "SRB_READ_DATA";
        case SRB_WRITE_DATA:
            return "SRB_WRITE_DATA";
        case SRB_UNKNOWN_STREAM_COMMAND:
            return "SRB_UNKNOWN_STREAM_COMMAND";
        default:
            return "SRB_<unknown>";
    }
}

/// PhotonicBringUpDevice -- bring the 1394 camera up far enough to enumerate it:
/// refresh the bus generation/node, discover the DCAM CSR base from the
/// Configuration ROM, reset the camera, then read its inquiry registers into the
/// device extension's mode table. Any failure is logged and propagated so the
/// caller can fall back to a built-in default format.
///
/// @param Extension  Device extension for the camera being brought up.
/// @return           STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicBringUpDevice(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    NTSTATUS status;

    //
    // Register for bus-reset notifications first so a reset during the rest of
    // bring-up is caught and the generation count recovered.
    //
    status = Photonic1394RegisterBusResetNotification(Extension);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = Photonic1394RefreshBusState(Extension);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = Photonic1394DiscoverCsrBase(Extension);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = PhotonicDcamReset(Extension);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return PhotonicDcamEnumerateModes(Extension);
}

/// SRB_INITIALIZE_DEVICE -- first SRB the class driver sends once the device has
/// started. The lower 1394 device object is captured, the camera is reset and
/// its video modes enumerated, and the KS data ranges are built from the result.
/// All of this runs synchronously: the driver sets TurnOffSynchronization, so
/// this handler is invoked at PASSIVE_LEVEL and the blocking 1394 IRPs are legal
/// here. Completing synchronously keeps the PnP start/stop sequence simple so
/// device removal and driver unload are not held off. Initialization succeeds
/// even when the camera could not be enumerated -- the device then advertises no
/// formats, and failing the SRB would tear the device down.
///
/// @param Srb  The SRB_INITIALIZE_DEVICE request block.
/// @return     STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicInitializeDevice(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PPORT_CONFIGURATION_INFORMATION config = Srb->CommandData.ConfigInfo;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_DISPATCH);

    RtlZeroMemory(extension, sizeof(*extension));
    extension->DevicePowerState = PowerDeviceD0;
    KeInitializeMutex(&extension->InterfaceMutex, 0);
    IoInitializeRemoveLock(&extension->IrbRemoveLock, PHOTONIC_POOL_TAG, 0, 0);

    //
    // Capture both device objects from the port configuration. Every IRB is
    // submitted down the stack to the unit PDO (PhysicalDeviceObject), whose
    // dispatch routines are owned by the 1394 bus driver that services the
    // IOCTL_1394_CLASS request interface; the PDO also names the camera node as
    // the destination of the max-speed query. The stream-class FDO
    // (ClassDeviceObject) does not handle the IOCTL and is kept only as a
    // reference to the top of the stack.
    //
    extension->ClassDeviceObject = config->ClassDeviceObject;
    extension->PhysicalDeviceObject = config->PhysicalDeviceObject;

    //
    // The control work item is the engine's PASSIVE_LEVEL executor for
    // transitions triggered at DISPATCH_LEVEL (read cancels, listen retries).
    // Without it those transitions cannot run, so its allocation failing fails
    // device initialization.
    //
    extension->ControlWorkItem = IoAllocateWorkItem(config->ClassDeviceObject);
    if (extension->ControlWorkItem == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DISPATCH, "failed to allocate the control work item\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    config->StreamDescriptorSize = sizeof(HW_STREAM_HEADER) + PHOTONIC_STREAM_COUNT * sizeof(HW_STREAM_INFORMATION);

    status = PhotonicBringUpDevice(extension);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DISPATCH,
                    "camera bring-up failed (%!STATUS!); device will advertise no formats\n", status);
        extension->ModeCount = 0;
    }

    //
    // Discover the image controls this camera implements and build the device
    // property sets from them. Also runs after a failed bring-up so the
    // VideoControl set is still advertised (no image controls then).
    //
    PhotonicBuildDevicePropertySets(extension);

    PhotonicStreamFormatBuild(extension);

    //
    // Publish the FDO -> extension mapping so the IRP_MJ_DEVICE_CONTROL hook
    // (ioctl/ioctl.c) can route Photonic IOCTLs to this device. Registered
    // even when bring-up failed: the IOCTL handlers report their own errors.
    //
    PhotonicIoctlRegisterDevice(extension);

    return STATUS_SUCCESS;
}

/// SRB_CHANGE_POWER_STATE -- cache the requested device power state. There is no
/// hardware to actually power up or down.
///
/// @param Srb  The SRB_CHANGE_POWER_STATE request block.
/// @return     STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicChangePowerState(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;

    FuncEntry(TRACE_FLAG_DISPATCH);

    extension->DevicePowerState = Srb->CommandData.DeviceState;
    return STATUS_SUCCESS;
}

/// SRB_SURPRISE_REMOVAL -- the camera was unplugged. Set Removed first so the
/// read path and capture start fail fast from here on, then tear down the data
/// path: stopping capture stops the isochronous context, detaches every attached
/// buffer (completing its read cancelled), releases the isochronous resources
/// and drains the reads still parked in the front-end. Without this teardown the
/// attached and parked reads wait forever for frames the removed camera will
/// never send: the client blocks on them, its handles never close, and the
/// device stack -- and the driver -- cannot be torn down until the client exits.
///
/// @param Srb  The SRB_SURPRISE_REMOVAL request block.
/// @return     STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicSurpriseRemoval(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PPHOTONIC_STREAM_EXTENSION stream;

    FuncEntry(TRACE_FLAG_DISPATCH);

    //
    // Serialize the teardown with the other control transitions: a user
    // thread may be inside a start, stop or map right now, and this stop must
    // not run in the middle of it (nor may a handler start the engine again
    // while this teardown is under way).
    //
    KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);

    extension->Removed = TRUE;

    //
    // Stop the active capture session, whichever interface owns it. The final
    // drain inside the stop takes PendingLock after Removed was set above, so
    // every read either parked in time to be drained here or observes Removed
    // in the read path and fails immediately -- none can slip through and
    // wait forever. The session itself stays published (the DirectShow pin
    // closes through SRB_CLOSE_STREAM, an IOCTL slot through the client's
    // unprepare or the uninitialize release), so the handlers keep valid
    // pointers while the client winds down.
    //
    stream = extension->ActiveCaptureStream;
    if (stream != NULL) {
        PhotonicCaptureStop(extension, stream);
    }

    KeReleaseMutex(&extension->InterfaceMutex, FALSE);

    return STATUS_SUCCESS;
}

/// SRB_UNKNOWN_DEVICE_COMMAND -- the stream class driver routes IRPs it cannot
/// map to a known SRB here, exposing the original IRP in Srb->Irp, and then
/// completes that IRP with whatever Srb->Status this handler returns.
///
/// The PnP manager's IRP_MN_QUERY_PNP_DEVICE_STATE arrives this way during the
/// start sequence. If it is left at the default STATUS_NOT_IMPLEMENTED, the
/// class driver completes the PnP IRP with that failure: the PnP manager then
/// flags the device with a problem and tears down its capture device interface,
/// so DirectShow can no longer open the filter and no SRB ever reaches the
/// minidriver. Answer the query (no special PnP state) so the device stays
/// started and openable.
///
/// @param Srb  The unrecognised device SRB containing the original IRP.
/// @return     STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicUnknownDeviceCommand(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PIO_STACK_LOCATION irpSp;

    FuncEntry(TRACE_FLAG_DISPATCH);

    if (Srb->Irp == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DISPATCH, "unhandled SRB 0x%x: no IRP attached\n", Srb->Command);
        return STATUS_NOT_IMPLEMENTED;
    }

    irpSp = IoGetCurrentIrpStackLocation(Srb->Irp);

    if (irpSp->MajorFunction == IRP_MJ_PNP && irpSp->MinorFunction == IRP_MN_QUERY_PNP_DEVICE_STATE) {
        //
        // IoStatus.Information carries the PNP_DEVICE_STATE bitmask; 0 means the
        // device reports no special state (not disabled, not failed). Srb->Status
        // alone cannot convey it, so set it on the IRP directly before the class
        // driver completes the request.
        //
        Srb->Irp->IoStatus.Information = 0;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH,
                    "IRP_MN_QUERY_PNP_DEVICE_STATE -> 0 (no special state)\n");
        return STATUS_SUCCESS;
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DISPATCH, "unhandled SRB 0x%x: IRP major=0x%x minor=0x%x ioctl=0x%x\n",
                Srb->Command, irpSp->MajorFunction, irpSp->MinorFunction,
                irpSp->Parameters.DeviceIoControl.IoControlCode);

    return STATUS_NOT_IMPLEMENTED;
}

/// PhotonicReceivePacket -- HwReceivePacket. Device/instance SRB entry point.
///
/// @param Srb  The device or instance SRB to dispatch.
VOID STREAMAPI PhotonicReceivePacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH, "%s (0x%x)\n", PhotonicSrbCommandName(Srb->Command),
                Srb->Command);

    //
    // Assume success; individual handlers override Srb->Status as needed.
    //
    Srb->Status = STATUS_SUCCESS;

    switch (Srb->Command) {
        case SRB_INITIALIZE_DEVICE:
            Srb->Status = PhotonicInitializeDevice(Srb);
            break;

        case SRB_GET_STREAM_INFO:
            Srb->Status = PhotonicStreamGetInfo(Srb);
            break;

        case SRB_OPEN_STREAM:
            Srb->Status = PhotonicStreamOpen(Srb);
            break;

        case SRB_CLOSE_STREAM:
            Srb->Status = PhotonicStreamClose(Srb);
            break;

        case SRB_GET_DATA_INTERSECTION:
            Srb->Status = PhotonicStreamGetDataIntersection(Srb);
            break;

        case SRB_CHANGE_POWER_STATE:
            Srb->Status = PhotonicChangePowerState(Srb);
            break;

        case SRB_SURPRISE_REMOVAL:
            Srb->Status = PhotonicSurpriseRemoval(Srb);
            break;

        case SRB_UNINITIALIZE_DEVICE:
            //
            // Last SRB before the device object is torn down. Withdraw the
            // bus-reset notification so the bus driver drops its reference to this
            // driver's callback and the device stack can be released, and unlink
            // the device from the Photonic IOCTL routing so no Photonic IOCTL
            // reaches the dying extension. With the routing gone, release the
            // IOCTL capture slot a client that never sent an unprepare left
            // behind. The mutex lets an IOCTL that was already dispatched
            // before the routing was removed finish before the slot is freed.
            //
            PhotonicIoctlDeregisterDevice(extension);
            KeWaitForSingleObject(&extension->InterfaceMutex, Executive, KernelMode, FALSE, NULL);
            PhotonicIoctlCaptureRelease(extension);
            KeReleaseMutex(&extension->InterfaceMutex, FALSE);
            Photonic1394DeregisterBusResetNotification(extension);

            //
            // Last of all, wait for every IRB submission still in flight,
            // including requests the timeout path abandoned at the bus driver.
            // Returning from this SRB lets the device stack unwind and the
            // driver image unload, and a late completion would then run freed
            // code. The wait must follow the teardown steps above because they
            // submit IRBs of their own, and the lock rejects new submissions
            // once the wait begins. A request the bus driver never completes
            // blocks this wait forever, which is the intended trade-off: a
            // hung removal is diagnosable, a completion into an unloaded
            // image corrupts the system.
            //
            if (NT_SUCCESS(IoAcquireRemoveLock(&extension->IrbRemoveLock, Srb))) {
                IoReleaseRemoveLockAndWait(&extension->IrbRemoveLock, Srb);
            }

            //
            // The remove-lock wait above also covers the control work item
            // (queuing one acquires the lock), so no run is queued or
            // executing anymore and the work item can be freed.
            //
            if (extension->ControlWorkItem != NULL) {
                IoFreeWorkItem(extension->ControlWorkItem);
                extension->ControlWorkItem = NULL;
            }
            Srb->Status = STATUS_SUCCESS;
            break;

        case SRB_INITIALIZATION_COMPLETE:
            //
            // The class driver has finished the start sequence. Now that the
            // device interface is live, DirectShow can connect.
            //
            Srb->Status = STATUS_SUCCESS;
            break;

        case SRB_GET_DEVICE_PROPERTY:
            //
            // Device-level KS property GET. PhotonicGetDeviceProperty answers the
            // advertised PROPSETID_VIDCAP_VIDEOCONTROL set and returns
            // STATUS_NOT_FOUND for everything else. NOT_FOUND (rather than
            // NOT_IMPLEMENTED) matters: the KS proxy probes a filter for several
            // optional property sets while creating it and treats NOT_IMPLEMENTED as
            // a fatal "filter could not be created" error, but skips a set that is
            // merely not found.
            //
            Srb->Status = PhotonicGetDeviceProperty(Srb);
            break;

        case SRB_SET_DEVICE_PROPERTY:
            //
            // Device-level KS property SET. The VideoProcAmp / CameraControl image
            // controls are settable (mapped to DCAM feature registers); every other
            // set is reported absent (NOT_FOUND, see above).
            //
            Srb->Status = PhotonicSetDeviceProperty(Srb);
            break;

        case SRB_OPEN_DEVICE_INSTANCE:
        case SRB_CLOSE_DEVICE_INSTANCE:
            Srb->Status = STATUS_SUCCESS;
            break;

        case SRB_NOTIFY_IDLE_STATE:
            //
            // Sent on the first open and the last close of the device. There is no
            // power management to do; just acknowledge it. Failing it
            // (the default NOT_IMPLEMENTED) aborts the open.
            //
            Srb->Status = STATUS_SUCCESS;
            break;

        case SRB_PAGING_OUT_DRIVER:
            //
            // The class driver may page the driver out. There are no hardware
            // interrupts to disable; per the DDK contract, return success.
            //
            Srb->Status = STATUS_SUCCESS;
            break;

        case SRB_DEVICE_METHOD:
        case SRB_STREAM_METHOD:
            //
            // No KS method sets are advertised. Report absent (STATUS_NOT_FOUND) so
            // the KS proxy skips them, exactly as for device properties above.
            //
            Srb->Status = STATUS_NOT_FOUND;
            break;

        case SRB_UNKNOWN_DEVICE_COMMAND:
        default:
            Srb->Status = PhotonicUnknownDeviceCommand(Srb);
            break;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_DISPATCH, "%s -> %!STATUS!\n", PhotonicSrbCommandName(Srb->Command),
                Srb->Status);

    //
    // Tell the class driver the next device SRB can be taken, then complete
    // this one. Both notifications are required for the device to keep running.
    //
    StreamClassDeviceNotification(ReadyForNextDeviceRequest, extension);
    StreamClassDeviceNotification(DeviceRequestComplete, extension, Srb);
}

/// PhotonicCancelPacket -- HwCancelPacket. The only requests this driver holds
/// are SRB_READ_DATA, either parked on a stream's pending queue awaiting a frame,
/// attached to the bus inside the capture engine, or briefly in the read pump
/// between the two. A parked read is completed cancelled right here; a read the
/// engine or the pump holds is marked and resolved asynchronously (by the
/// control work item and the pump respectively) -- this routine runs at up
/// to DISPATCH_LEVEL and issues no bus requests. A read found nowhere has already
/// been completed, or is owned by the delivery or drain path, and there is
/// nothing to do.
///
/// @param Srb  The SRB to cancel.
VOID STREAMAPI PhotonicCancelPacket(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_STREAM_EXTENSION streamExt;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH, "%s (0x%x)\n", PhotonicSrbCommandName(Srb->Command),
                Srb->Command);

    if (Srb->Command != SRB_READ_DATA || Srb->StreamObject == NULL) {
        return;
    }

    streamExt = (PPHOTONIC_STREAM_EXTENSION) Srb->StreamObject->HwStreamExtension;
    if (streamExt == NULL) {
        return;
    }

    //
    // The gate keeps the cancel inside the close's rundown, so the search may
    // touch the capture engine freely. A cancel refused here arrived after
    // the close began; its teardown drain owns and completes every read.
    //
    if (!PhotonicStreamEnterDataPath(streamExt)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH,
                    "cancel refused: the stream is closing, the teardown drain completes the read\n");
        return;
    }
    PhotonicStreamCancelRead(streamExt, Srb);
    PhotonicStreamLeaveDataPath(streamExt);
}

/// PhotonicTimeout -- HwRequestTimeoutHandler. Device/stream SRBs complete
/// synchronously and a parked or attached SRB_READ_DATA has its timeout disabled
/// (stream/read.c zeroes TimeoutCounter), so a request reaching this handler is
/// unexpected; it is only logged.
///
/// @param Srb  The timed-out SRB.
VOID STREAMAPI PhotonicTimeout(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DISPATCH, "%s (0x%x)\n", PhotonicSrbCommandName(Srb->Command),
                Srb->Command);
}
