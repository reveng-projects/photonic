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
/// 1394 bus transport for the Photonic minidriver. See p1394.h.

// clang-format off
#include "p1394.h"
#include "p1394.tmh"
// clang-format on

/// 1394 register-space layout. The DCAM register block lives inside the initial
/// node register space whose 48-bit base is 0xFFFFF0000000 (high 16 bits 0xFFFF,
/// low 32 bits 0xF0000000). The command-base entry in the Configuration ROM
/// encodes only the low 32 bits as a quadlet offset (value << 2 above
/// 0xF0000000); the high 16 bits are always 0xFFFF.
#define PHOTONIC_REGISTER_SPACE_HIGH 0xFFFFULL
#define PHOTONIC_REGISTER_SPACE_LOW  0xF0000000UL

/// Plausible range for a recovered DCAM command base (low 32 bits). The camera
/// places its CSR block just above the kernel-managed Config ROM / FCP areas;
/// this window is used to validate the key-0x40 entry parsed from the ROM and to
/// reject stray quadlets.
#define PHOTONIC_CSR_BASE_MIN 0xF0000000UL
#define PHOTONIC_CSR_BASE_MAX 0xF1000000UL

/// IEEE 1212 / DCAM command-base directory key. A unit-dependent directory entry
/// is one quadlet: high 8 bits = key, low 24 bits = value. Key 0x40 carries the
/// DCAM command register base as a quadlet offset above 0xF0000000.
#define PHOTONIC_KEY_COMMAND_BASE 0x40

/// Bound on how long a single 1394 transaction may take. A healthy bus completes
/// register reads/writes in milliseconds; if the bus driver never completes (for
/// instance because the device was surprise-removed), the wait gives up after
/// this interval, cancels the IRP, and returns an error rather than blocking the
/// thread -- which would otherwise stall device removal and driver unload.
/// Expressed in 100ns units as a relative (negative) timeout.
#define PHOTONIC_IRB_TIMEOUT_100NS (-5LL * 1000 * 1000 * 10) ///< 5 seconds

/// Completion handshake for one submitted IRB. The whole request lives in
/// non-paged pool (never on the caller's stack) so it can be safely abandoned
/// on timeout: whichever of the waiter and the completion routine transitions
/// State last performs the cleanup, so nothing here is touched after it is
/// freed.
///
/// IrbCopy is the request the bus driver actually sees. The caller's IRB is
/// copied in before submit and the result fields are copied back after
/// completion, so the caller's IRB can live anywhere (typically the stack)
/// and the caller has no cleanup duties: an abandoned request keeps writing
/// into this block, never into caller memory. Mdl, Buffer and Buffer2 record
/// any memory IrbCopy points at (DMA targets and staging buffers); on the
/// abandon path they must survive until the wedged request actually
/// completes, so the completion routine frees them together with the IRP and
/// this block. On every other path the submitting function keeps ownership
/// of them and the completion routine leaves them alone.
///
/// RemoveLock points at the device extension's IrbRemoveLock. Each request
/// holds one acquisition from submit until whichever side performs cleanup
/// releases it, so device teardown waits for abandoned requests too.
typedef enum _PHOTONIC_IRB_STATE {
    PhotonicIrbPending = 0,
    PhotonicIrbCompleted,
    PhotonicIrbAbandoned,
} PHOTONIC_IRB_STATE;

typedef struct _PHOTONIC_IRB_SYNC {
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP Irp;
    IRB IrbCopy;                ///< Pool-resident copy of the caller's IRB; the bus driver only sees this.
    PMDL Mdl;                   ///< Optional MDL IrbCopy references; freed here only on the abandon path.
    PVOID Buffer;               ///< Optional pool buffer IrbCopy references; freed here only on the abandon path.
    PVOID Buffer2;              ///< Second optional pool buffer; freed here only on the abandon path.
    PIO_REMOVE_LOCK RemoveLock; ///< Device extension's IrbRemoveLock, held for the life of the request.
    LONG State;                 ///< PHOTONIC_IRB_STATE, updated with InterlockedExchange
} PHOTONIC_IRB_SYNC, *PPHOTONIC_IRB_SYNC;

/// PhotonicIrbComplete -- IRB completion routine. Records the status, then either
/// wakes the waiter (normal path) or, if the waiter already abandoned the
/// request, frees the IRP, the referenced MDL and buffers, and the sync block
/// (which embeds the submitted IRB) here: the bus driver has only now stopped
/// touching them. The remove lock acquisition held for this request is then
/// released, allowing a pending device teardown wait to proceed. Returns
/// STATUS_MORE_PROCESSING_REQUIRED so the I/O manager leaves the IRP for
/// whoever owns cleanup to free with IoFreeIrp.
///
/// @param DeviceObject  Device object (unused).
/// @param Irp           Completed IRP.
/// @param Context       PHOTONIC_IRB_SYNC block for this request.
/// @return STATUS_MORE_PROCESSING_REQUIRED always.
static IO_COMPLETION_ROUTINE PhotonicIrbComplete;
static NTSTATUS PhotonicIrbComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context) {
    PPHOTONIC_IRB_SYNC sync = (PPHOTONIC_IRB_SYNC) Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    sync->IoStatus = Irp->IoStatus;

    if (InterlockedExchange(&sync->State, PhotonicIrbCompleted) == PhotonicIrbAbandoned) {
        PIO_REMOVE_LOCK removeLock = sync->RemoveLock;

        //
        // The waiter gave up; this routine owns cleanup of the IRP and of
        // everything the request referenced.
        //
        IoFreeIrp(Irp);
        if (sync->Mdl != NULL) {
            IoFreeMdl(sync->Mdl);
        }
        if (sync->Buffer != NULL) {
            ExFreePoolWithTag(sync->Buffer, PHOTONIC_POOL_TAG);
        }
        if (sync->Buffer2 != NULL) {
            ExFreePoolWithTag(sync->Buffer2, PHOTONIC_POOL_TAG);
        }
        ExFreePoolWithTag(sync, PHOTONIC_POOL_TAG);

        //
        // Dropping the remove lock is the last action here. It can unblock
        // the teardown wait in SRB_UNINITIALIZE_DEVICE, after which this
        // routine's code may be unloaded, so nothing may run after the
        // release. The tag is only compared by value, so passing the freed
        // sync pointer is fine.
        //
        IoReleaseRemoveLock(removeLock, sync);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    KeSetEvent(&sync->Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/// PhotonicSubmitIrbEx -- send one IRB synchronously to the 1394 bus driver and
/// wait (with a timeout) for completion. The IRB travels *down* the device stack
/// to the camera's unit PDO (PhysicalDeviceObject), whose dispatch routines are
/// owned by the 1394 bus driver; it consumes IRBs as
/// IRP_MJ_INTERNAL_DEVICE_CONTROL / IOCTL_1394_CLASS requests carrying the IRB
/// pointer in Parameters.Others.Argument1. The request must not be sent to the
/// stream-class FDO (ClassDeviceObject): that driver does not understand the
/// IOCTL and completes it with STATUS_INVALID_DEVICE_REQUEST. Must run at
/// PASSIVE_LEVEL.
///
/// The caller's IRB is copied into the pool-resident sync block and the bus
/// driver only ever sees that copy; on completion the copy, now carrying the
/// result fields, is copied back. The caller's IRB can therefore live on the
/// stack and is never touched after this returns, whatever happens to the
/// request.
///
/// Mdl, Buffer and Buffer2 name whatever memory the IRB references (the DMA
/// target MDL, a staging buffer, a directory buffer); they exist so the
/// abandon path knows what the wedged request can still touch. Everything the
/// IRB points at must be listed here, and it must live in non-paged pool,
/// never on the stack.
///
/// Ownership: when the call returns with *Abandoned == FALSE the caller keeps
/// ownership of Mdl, Buffer and Buffer2. When both the wait and the
/// post-cancel wait time out, the request is abandoned: the call returns
/// STATUS_IO_TIMEOUT with *Abandoned == TRUE and ownership of them has passed
/// to the completion routine, which frees them if the wedged request ever
/// completes. If it never completes they leak deliberately, like the IRP and
/// sync block: freeing memory the bus driver may still write is worse.
///
/// Every submission holds the device extension's IrbRemoveLock until its
/// cleanup side releases it, so the teardown wait in SRB_UNINITIALIZE_DEVICE
/// covers abandoned requests and a late completion cannot run in an unloaded
/// image. Once that wait has begun, submission fails with
/// STATUS_DELETE_PENDING.
///
/// @param Extension  Device extension.
/// @param Irb        IRB to submit; receives the result fields on completion.
/// @param Mdl        Optional MDL the IRB references.
/// @param Buffer     Optional pool buffer the IRB references.
/// @param Buffer2    Second optional pool buffer the IRB references.
/// @param Abandoned  Receives TRUE when the request was abandoned and
///                   ownership of Mdl, Buffer and Buffer2 passed to the
///                   completion machinery.
/// @param Submitted  Optionally receives whether the request reached the bus
///                   driver (FALSE when it failed before IoCallDriver, so it
///                   may safely be re-issued).
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicSubmitIrbEx(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Inout_ PIRB Irb, _In_opt_ PMDL Mdl,
                                    _In_opt_ PVOID Buffer, _In_opt_ PVOID Buffer2, _Out_ PBOOLEAN Abandoned,
                                    _Out_opt_ PBOOLEAN Submitted) {
    PPHOTONIC_IRB_SYNC sync;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    NTSTATUS wait;

    *Abandoned = FALSE;
    if (Submitted != NULL) {
        *Submitted = FALSE;
    }

    if (Extension->PhysicalDeviceObject == NULL) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    sync = (PPHOTONIC_IRB_SYNC) ExAllocatePoolZero(NonPagedPoolNx, sizeof(*sync), PHOTONIC_POOL_TAG);
    if (sync == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "failed to allocate the sync block for IRB function 0x%x\n",
                    Irb->FunctionNumber);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&sync->Event, NotificationEvent, FALSE);
    sync->State = PhotonicIrbPending;
    sync->IrbCopy = *Irb;
    sync->Mdl = Mdl;
    sync->Buffer = Buffer;
    sync->Buffer2 = Buffer2;

    irp = IoAllocateIrp(Extension->PhysicalDeviceObject->StackSize, FALSE);
    if (irp == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "failed to allocate the IRP for IRB function 0x%x\n",
                    Irb->FunctionNumber);
        ExFreePoolWithTag(sync, PHOTONIC_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    sync->Irp = irp;
    sync->RemoveLock = &Extension->IrbRemoveLock;

    //
    // Count this request against the remove lock so device teardown waits for
    // it, even if it ends up abandoned at the bus driver. Whichever side
    // performs cleanup releases the acquisition. Fails with
    // STATUS_DELETE_PENDING once the teardown wait has begun.
    //
    status = IoAcquireRemoveLock(sync->RemoveLock, sync);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394,
                    "remove lock refused IRB function 0x%x: %!STATUS! (device teardown has begun)\n",
                    Irb->FunctionNumber, status);
        IoFreeIrp(irp);
        ExFreePoolWithTag(sync, PHOTONIC_POOL_TAG);
        return status;
    }

    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_1394_CLASS;
    stack->Parameters.Others.Argument1 = &sync->IrbCopy;

    //
    // Invoke the completion routine on success, error, and cancel so the wait is
    // always released.
    //
    IoSetCompletionRoutine(irp, PhotonicIrbComplete, sync, TRUE, TRUE, TRUE);

    if (Submitted != NULL) {
        *Submitted = TRUE;
    }
    (VOID) IoCallDriver(Extension->PhysicalDeviceObject, irp);

    timeout.QuadPart = PHOTONIC_IRB_TIMEOUT_100NS;
    wait = KeWaitForSingleObject(&sync->Event, Executive, KernelMode, FALSE, &timeout);
    if (wait == STATUS_TIMEOUT) {
        //
        // The transaction is overdue. Cancel it and give the cancel a bounded
        // chance to complete.
        //
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "IRB function %u timed out; cancelling\n", Irb->FunctionNumber);
        IoCancelIrp(irp);

        timeout.QuadPart = PHOTONIC_IRB_TIMEOUT_100NS;
        wait = KeWaitForSingleObject(&sync->Event, Executive, KernelMode, FALSE, &timeout);
        if (wait == STATUS_TIMEOUT) {
            //
            // Even the cancel did not complete. Abandon the request. If the
            // completion routine has not run yet, it frees the IRP, the
            // referenced MDL and buffers, and the sync block (embedding the
            // submitted IRB) once the request finally completes, then drops
            // the remove lock that holds off device teardown. Otherwise this
            // thread owns cleanup (handled below). The caller's IRB is left
            // without result fields.
            //
            if (InterlockedExchange(&sync->State, PhotonicIrbAbandoned) != PhotonicIrbCompleted) {
                *Abandoned = TRUE;
                return STATUS_IO_TIMEOUT;
            }

            //
            // The completion routine won the claim but may still be between
            // its exchange and the KeSetEvent. Wait for the set so the IRP
            // and sync block are not freed under it. The set is at most a few
            // instructions away, so the wait is effectively bounded.
            //
            KeWaitForSingleObject(&sync->Event, Executive, KernelMode, FALSE, NULL);
        }
    }

    //
    // Completed (normally or after cancel): this thread owns the IRP and sync
    // block. Hand the result fields back before the block is freed; the
    // attached buffers stay with the caller.
    //
    status = sync->IoStatus.Status;
    *Irb = sync->IrbCopy;
    IoFreeIrp(irp);
    ExFreePoolWithTag(sync, PHOTONIC_POOL_TAG);
    IoReleaseRemoveLock(&Extension->IrbRemoveLock, sync);
    return status;
}

NTSTATUS PhotonicSubmitIrb(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Inout_ PIRB Irb) {
    BOOLEAN abandoned;

    //
    // With no attached resources the machinery owns everything either way, so
    // abandonment needs no caller-visible signal beyond the failure status.
    //
    return PhotonicSubmitIrbEx(Extension, Irb, NULL, NULL, NULL, &abandoned, NULL);
}

NTSTATUS PhotonicSubmitIrbTracked(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Inout_ PIRB Irb,
                                  _Out_ PBOOLEAN Submitted) {
    BOOLEAN abandoned;

    return PhotonicSubmitIrbEx(Extension, Irb, NULL, NULL, NULL, &abandoned, Submitted);
}

/// PhotonicSpeedFlagsToScode -- map a SPEED_FLAGS_* bitmask (as returned by
/// GetMaxSpeedBetweenDevices) to the highest SCODE_*_RATE it advertises. The
/// async-transaction nSpeed field is an SCODE, not a flag. Falls back to
/// SCODE_400_RATE when no known speed bit is set.
///
/// @param SpeedFlags  SPEED_FLAGS_* bitmask from GetMaxSpeedBetweenDevices.
/// @return SCODE_*_RATE value for the highest advertised speed.
static UCHAR PhotonicSpeedFlagsToScode(_In_ ULONG SpeedFlags) {
    if (SpeedFlags & SPEED_FLAGS_3200) {
        return SCODE_3200_RATE;
    }
    if (SpeedFlags & SPEED_FLAGS_1600) {
        return SCODE_1600_RATE;
    }
    if (SpeedFlags & SPEED_FLAGS_800) {
        return SCODE_800_RATE;
    }
    if (SpeedFlags & SPEED_FLAGS_400) {
        return SCODE_400_RATE;
    }
    if (SpeedFlags & SPEED_FLAGS_200) {
        return SCODE_200_RATE;
    }
    if (SpeedFlags & SPEED_FLAGS_100) {
        return SCODE_100_RATE;
    }
    return SCODE_400_RATE;
}

/// Photonic1394SpeedName -- map an SCODE to the IEEE 1394 speed name it stands
/// for (e.g. SCODE_400_RATE -> "S400"), for trace output.
///
/// @param Scode  SCODE_*_RATE value.
/// @return Pointer to a string literal naming the speed.
const char *Photonic1394SpeedName(_In_ UCHAR Scode) {
    switch (Scode) {
        case SCODE_100_RATE:
            return "S100";
        case SCODE_200_RATE:
            return "S200";
        case SCODE_400_RATE:
            return "S400";
        case SCODE_800_RATE:
            return "S800";
        case SCODE_1600_RATE:
            return "S1600";
        case SCODE_3200_RATE:
            return "S3200";
        default:
            return "S-unknown";
    }
}

/// Photonic1394StreamScode -- the SCODE the isochronous stream runs at: the
/// maximum speed to the camera, capped at S400 (the fastest DCAM isochronous
/// transmit speed).
///
/// @param Extension  Device extension.
/// @return SCODE_*_RATE value for the stream.
UCHAR Photonic1394StreamScode(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    UCHAR scode = Extension->MaxSpeedCode;

    if (scode > SCODE_400_RATE) {
        scode = SCODE_400_RATE;
    }
    return scode;
}

/// Photonic1394MaxIsochPayload -- the largest isochronous packet payload the
/// stream speed carries: 1024 bytes per packet at S100, doubling with each
/// speed step (IEEE 1394). The bus driver rejects bandwidth and resource
/// allocations that ask for more with STATUS_INVALID_PARAMETER, so every
/// packet-size choice is bounded by this.
///
/// @param Extension  Device extension.
/// @return Maximum isochronous payload in bytes.
ULONG Photonic1394MaxIsochPayload(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    return 1024ul << Photonic1394StreamScode(Extension);
}

/// PhotonicQueryGenerationCount -- read the current 1394 bus generation into the
/// device extension. Async transactions carry this; the bus driver rejects them
/// with STATUS_INVALID_GENERATION if it is stale, so this must be current before
/// any register access and is re-read whenever a bus reset is pending.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicQueryGenerationCount(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    IRB irb;
    NTSTATUS status;

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_GET_GENERATION_COUNT;
    status = PhotonicSubmitIrb(Extension, &irb);
    if (NT_SUCCESS(status)) {
        Extension->BusGeneration = irb.u.GetGenerationCount.GenerationCount;
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "GET_GENERATION_COUNT failed: %!STATUS!\n", status);
    }
    return status;
}

/// PhotonicQueryLocalHostInfo -- one GET_LOCAL_HOST_INFO query. The bus driver
/// writes the result through the IRB's Information pointer when the request
/// completes, which on an abandoned request can be long after this function
/// returned. The result is therefore staged through a pool buffer registered
/// with the submit (so an abandoned request keeps it alive) and copied into
/// the caller's buffer only on success, letting the caller pass a stack
/// buffer.
///
/// @param Extension  Device extension.
/// @param Level      GET_LOCAL_HOST_INFO level to query.
/// @param Info       Receives the result on success.
/// @param InfoBytes  Size of the result structure for this level.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicQueryLocalHostInfo(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Level,
                                           _Out_writes_bytes_(InfoBytes) PVOID Info, _In_ ULONG InfoBytes) {
    IRB irb;
    PVOID staging;
    BOOLEAN abandoned;
    NTSTATUS status;

    staging = ExAllocatePoolZero(NonPagedPoolNx, InfoBytes, PHOTONIC_POOL_TAG);
    if (staging == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394,
                    "failed to allocate the %u-byte staging buffer for host info level %u\n", InfoBytes, Level);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_GET_LOCAL_HOST_INFO;
    irb.u.GetLocalHostInformation.nLevel = Level;
    irb.u.GetLocalHostInformation.Information = staging;
    status = PhotonicSubmitIrbEx(Extension, &irb, NULL, staging, NULL, &abandoned, NULL);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(Info, staging, InfoBytes);
    }

    if (!abandoned) {
        ExFreePoolWithTag(staging, PHOTONIC_POOL_TAG);
    }
    return status;
}

/// PhotonicQueryHostInfo -- read the host controller capabilities (asynchronous
/// read/write payload limits, level 2) and the DMA capabilities (the largest
/// single buffer the port driver accepts, level 7) into the device extension.
/// The capture engine splits frames larger than MaxDmaBufferSize into chunks
/// (PhotonicCapturePoolSetupChunks); both queries are informational, so a failure
/// here is logged and tolerated rather than fatal -- MaxDmaBufferSize stays 0
/// and the capture engine falls back to PHOTONIC_CAPTURE_MAX_CHUNK_BYTES.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicQueryHostInfo(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    GET_LOCAL_HOST_INFO2 hostInfo;
    GET_LOCAL_HOST_INFO7 dmaInfo;
    NTSTATUS status;
    NTSTATUS dmaStatus;

    RtlZeroMemory(&hostInfo, sizeof(hostInfo));
    status = PhotonicQueryLocalHostInfo(Extension, GET_HOST_CAPABILITIES, &hostInfo, sizeof(hostInfo));
    if (NT_SUCCESS(status)) {
        Extension->HostCapabilities = hostInfo.HostCapabilities;
        Extension->MaxAsyncReadRequest = hostInfo.MaxAsyncReadRequest;
        Extension->MaxAsyncWriteRequest = hostInfo.MaxAsyncWriteRequest;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_1394, "host caps=0x%08x maxAsyncRead=%u maxAsyncWrite=%u\n",
                    Extension->HostCapabilities, Extension->MaxAsyncReadRequest, Extension->MaxAsyncWriteRequest);
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_1394, "GET_LOCAL_HOST_INFO level %u failed: %!STATUS!\n",
                    GET_HOST_CAPABILITIES, status);
    }

    RtlZeroMemory(&dmaInfo, sizeof(dmaInfo));
    dmaStatus = PhotonicQueryLocalHostInfo(Extension, GET_HOST_DMA_CAPABILITIES, &dmaInfo, sizeof(dmaInfo));
    if (NT_SUCCESS(dmaStatus)) {
        Extension->MaxDmaBufferSize = dmaInfo.MaxDmaBufferSize.QuadPart;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_1394, "host dmaCaps=0x%08x maxDmaBufferSize=%I64u\n",
                    dmaInfo.HostDmaCapabilities, Extension->MaxDmaBufferSize);
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_1394,
                    "GET_LOCAL_HOST_INFO level %u failed: %!STATUS!; capture falls back to the built-in chunk limit\n",
                    GET_HOST_DMA_CAPABILITIES, dmaStatus);
    }

    return status;
}

/// PhotonicQueryMaxSpeed -- read the maximum 1394 speed between the local host
/// and the camera into the device extension as an SCODE used by every async
/// transaction. A failure is logged and tolerated: register access still works
/// at the SCODE_400_RATE default.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicQueryMaxSpeed(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    IRB irb;
    NTSTATUS status;

    //
    // Default before the query so a failure leaves a usable speed in place.
    //
    Extension->MaxSpeedCode = SCODE_400_RATE;

    //
    // USE_LOCAL_NODE includes the local host in the path-speed computation.
    // Without it the bus driver reports the speed among the destinations
    // only, which on a topology with a slower hop between host and camera
    // overstates the achievable link speed and breaks the bandwidth and
    // packet-size ceilings derived from it.
    //
    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_GET_SPEED_BETWEEN_DEVICES;
    irb.u.GetMaxSpeedBetweenDevices.fulFlags = USE_LOCAL_NODE;
    irb.u.GetMaxSpeedBetweenDevices.ulNumberOfDestinations = 1;
    irb.u.GetMaxSpeedBetweenDevices.hDestinationDeviceObjects[0] = Extension->PhysicalDeviceObject;
    status = PhotonicSubmitIrb(Extension, &irb);
    if (NT_SUCCESS(status)) {
        Extension->MaxSpeedCode = PhotonicSpeedFlagsToScode(irb.u.GetMaxSpeedBetweenDevices.fulSpeed);
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_1394, "max speed flags=0x%08x -> %s\n",
                    irb.u.GetMaxSpeedBetweenDevices.fulSpeed, Photonic1394SpeedName(Extension->MaxSpeedCode));
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_1394, "GET_SPEED_BETWEEN_DEVICES failed: %!STATUS!\n", status);
    }
    return status;
}

/// PhotonicBusResetRoutine -- bus-reset notification callback. Runs at
/// DISPATCH_LEVEL, so it only flags the cached generation count stale; the next
/// async transaction (at PASSIVE_LEVEL) re-reads it before submitting. The bus
/// driver issues this only while the camera is still present after the reset, so
/// no presence check is needed here.
///
/// @param Context  Device extension passed as the reset context at registration.
static VOID PhotonicBusResetRoutine(_In_ PVOID Context) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Context;

    InterlockedExchange(&extension->BusResetPending, TRUE);
}

NTSTATUS Photonic1394RegisterBusResetNotification(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    IRB irb;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_1394);

    if (Extension->BusResetRegistered) {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_BUS_RESET_NOTIFICATION;
    irb.u.BusResetNotification.fulFlags = REGISTER_NOTIFICATION_ROUTINE;
    irb.u.BusResetNotification.ResetRoutine = PhotonicBusResetRoutine;
    irb.u.BusResetNotification.ResetContext = Extension;
    status = PhotonicSubmitIrb(Extension, &irb);
    if (NT_SUCCESS(status)) {
        Extension->BusResetRegistered = TRUE;
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_1394, "register bus-reset notification failed: %!STATUS!\n",
                    status);
    }
    return status;
}

VOID Photonic1394DeregisterBusResetNotification(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    ULONG attempt;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    FuncEntry(TRACE_FLAG_1394);

    if (!Extension->BusResetRegistered) {
        return;
    }

    //
    // The only caller is SRB_UNINITIALIZE_DEVICE, which runs exactly once: a
    // deregistration that fails here is never attempted again, and the bus
    // driver would keep a callback pointing into the device extension that is
    // about to be freed (a bugcheck on the next bus reset if the driver
    // unloads while the bus stays alive). The plausible failures are
    // allocation failures in the submit path, so retry a few times with a
    // short pause instead of giving up on the first.
    //
    for (attempt = 0; attempt < 3; attempt++) {
        IRB irb;

        if (attempt != 0) {
            LARGE_INTEGER delay;

            delay.QuadPart = -10 * 1000 * 10; // 10 ms, relative
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }

        RtlZeroMemory(&irb, sizeof(irb));
        irb.FunctionNumber = REQUEST_BUS_RESET_NOTIFICATION;
        irb.u.BusResetNotification.fulFlags = DEREGISTER_NOTIFICATION_ROUTINE;
        irb.u.BusResetNotification.ResetRoutine = PhotonicBusResetRoutine;
        irb.u.BusResetNotification.ResetContext = Extension;
        status = PhotonicSubmitIrb(Extension, &irb);
        if (NT_SUCCESS(status)) {
            Extension->BusResetRegistered = FALSE;
            return;
        }
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_1394,
                    "deregister bus-reset notification failed (attempt %u): %!STATUS!\n", attempt + 1, status);
    }

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394,
                "bus-reset notification stays registered after %u attempts (%!STATUS!); the next bus reset may call "
                "into a torn-down device\n",
                attempt, status);
}

NTSTATUS Photonic1394RefreshBusState(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_1394);

    //
    // The generation count is mandatory: it rides on every async transaction.
    //
    status = PhotonicQueryGenerationCount(Extension);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Host capabilities and the max speed are best-effort; their failures fall
    // back to defaults and must not block register access.
    //
    (VOID) PhotonicQueryHostInfo(Extension);
    (VOID) PhotonicQueryMaxSpeed(Extension);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_1394, "bus state: generation=%u speed=%s\n",
                Extension->BusGeneration, Photonic1394SpeedName(Extension->MaxSpeedCode));
    return STATUS_SUCCESS;
}

/// PhotonicAsyncTransfer -- synchronous async-transaction core shared by the
/// quadlet register accessors and the block transfers. Data is staged through a
/// fresh non-paged bounce buffer (the caller's buffer may be any memory, e.g. a
/// METHOD_BUFFERED system buffer), transferred verbatim -- no byte-swapping --
/// at the absolute 48-bit 1394 address, and copied back on a successful read.
///
/// @param Extension  Device extension.
/// @param Address    Absolute 48-bit 1394 address.
/// @param IsWrite    TRUE for a write transaction, FALSE for a read.
/// @param Data       Buffer to send (write) or receive into (read).
/// @param Length     Number of bytes to transfer.
/// @param TCode      1394 transaction code (TCODE_*).
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicAsyncTransfer(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONGLONG Address,
                                      _In_ BOOLEAN IsWrite, _Inout_updates_bytes_(Length) PVOID Data, _In_ ULONG Length,
                                      _In_ UCHAR TCode) {
    IRB irb;
    PMDL mdl;
    PVOID buffer;
    BOOLEAN abandoned;
    NTSTATUS status;

    if (Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    //
    // A bus reset since the last transaction invalidated the cached generation
    // count. Re-read it (at PASSIVE_LEVEL, here) before building the IRB so this
    // transaction carries the current generation instead of failing with
    // STATUS_INVALID_GENERATION. The destination node is implied by the device
    // stack, so the changed node number after the reset needs no fix-up.
    //
    if (InterlockedExchange(&Extension->BusResetPending, FALSE)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_1394, "bus reset pending; refreshing generation count\n");
        if (!NT_SUCCESS(PhotonicQueryGenerationCount(Extension))) {
            //
            // The cached generation is still stale. Re-arm the flag so the
            // next transaction retries the refresh. Without this, every
            // transaction until the next bus reset would carry the stale
            // generation and fail with STATUS_INVALID_GENERATION.
            //
            InterlockedExchange(&Extension->BusResetPending, TRUE);
        }
    }

    buffer = ExAllocatePoolZero(NonPagedPoolNx, Length, PHOTONIC_POOL_TAG);
    if (buffer == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "failed to allocate the %u-byte transfer bounce buffer\n",
                    Length);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mdl = IoAllocateMdl(buffer, Length, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "failed to allocate the MDL for a %u-byte transfer\n", Length);
        ExFreePoolWithTag(buffer, PHOTONIC_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(mdl);

    //
    // The destination ID is left zero: the bus driver implies the target node
    // from the device stack the IRB is sent down. Only the 48-bit offset is set
    // (its high 16 bits are 0xFFFF for the initial register space), which keeps
    // the addressing valid across bus resets without re-resolving a node number.
    //
    RtlZeroMemory(&irb, sizeof(irb));
    if (IsWrite) {
        RtlCopyMemory(buffer, Data, Length);

        irb.FunctionNumber = REQUEST_ASYNC_WRITE;
        irb.u.AsyncWrite.DestinationAddress.IA_Destination_Offset.Off_High = (USHORT) (Address >> 32);
        irb.u.AsyncWrite.DestinationAddress.IA_Destination_Offset.Off_Low = (ULONG) Address;
        irb.u.AsyncWrite.nNumberOfBytesToWrite = Length;
        irb.u.AsyncWrite.nBlockSize = 0;
        irb.u.AsyncWrite.fulFlags = 0;
        irb.u.AsyncWrite.Mdl = mdl;
        irb.u.AsyncWrite.ulGeneration = Extension->BusGeneration;
        irb.u.AsyncWrite.nSpeed = Extension->MaxSpeedCode;
        irb.u.AsyncWrite.tCode = TCode;
    } else {
        irb.FunctionNumber = REQUEST_ASYNC_READ;
        irb.u.AsyncRead.DestinationAddress.IA_Destination_Offset.Off_High = (USHORT) (Address >> 32);
        irb.u.AsyncRead.DestinationAddress.IA_Destination_Offset.Off_Low = (ULONG) Address;
        irb.u.AsyncRead.nNumberOfBytesToRead = Length;
        irb.u.AsyncRead.nBlockSize = 0;
        irb.u.AsyncRead.fulFlags = 0;
        irb.u.AsyncRead.Mdl = mdl;
        irb.u.AsyncRead.ulGeneration = Extension->BusGeneration;
        irb.u.AsyncRead.nSpeed = Extension->MaxSpeedCode;
        irb.u.AsyncRead.tCode = TCode;
    }

    //
    // The MDL and bounce buffer are the transaction's DMA target, so they ride
    // along in the submit: an abandoned transfer keeps them alive until the
    // wedged request completes instead of letting the bus driver write into
    // freed pool.
    //
    status = PhotonicSubmitIrbEx(Extension, &irb, mdl, buffer, NULL, &abandoned, NULL);

    //
    // A stale generation can race the refresh above: a bus reset after the
    // check, or a concurrent transaction that cleared BusResetPending before
    // its refresh completed, leaves this transaction stamped with the old
    // generation and the bus driver rejects it. Refresh and retry once so the
    // transient rejection is not surfaced to the caller. When the refresh
    // itself fails, the pending flag is re-armed for the next transaction and
    // the rejection stands.
    //
    if (status == STATUS_INVALID_GENERATION && !abandoned) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_1394,
                    "transaction rejected with a stale generation; refreshing and retrying once\n");
        if (NT_SUCCESS(PhotonicQueryGenerationCount(Extension))) {
            if (IsWrite) {
                irb.u.AsyncWrite.ulGeneration = Extension->BusGeneration;
            } else {
                irb.u.AsyncRead.ulGeneration = Extension->BusGeneration;
            }
            status = PhotonicSubmitIrbEx(Extension, &irb, mdl, buffer, NULL, &abandoned, NULL);
        } else {
            InterlockedExchange(&Extension->BusResetPending, TRUE);
        }
    }

    if (NT_SUCCESS(status) && !IsWrite) {
        RtlCopyMemory(Data, buffer, Length);
    }

    if (!abandoned) {
        IoFreeMdl(mdl);
        ExFreePoolWithTag(buffer, PHOTONIC_POOL_TAG);
    }
    return status;
}

/// PhotonicAsyncQuadlet -- single-quadlet register transaction. Values are
/// exchanged in host byte order; the swap to and from 1394 bus order happens
/// here, unlike the block transfers which are verbatim.
///
/// @param Extension  Device extension.
/// @param Address    Absolute 48-bit 1394 address of the quadlet.
/// @param IsWrite    TRUE for a write transaction, FALSE for a read.
/// @param Value      On write: value to send (host byte order). On read: receives
///                   the value in host byte order.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicAsyncQuadlet(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONGLONG Address,
                                     _In_ BOOLEAN IsWrite, _Inout_ PULONG Value) {
    ULONG raw = RtlUlongByteSwap(*Value);
    NTSTATUS status;

    status = PhotonicAsyncTransfer(Extension, Address, IsWrite, &raw, sizeof(raw),
                                   IsWrite ? TCODE_WRITE_REQUEST_QUADLET : TCODE_READ_REQUEST_QUADLET);

    if (NT_SUCCESS(status) && !IsWrite) {
        *Value = RtlUlongByteSwap(raw);
    }
    return status;
}

NTSTATUS Photonic1394ReadRegister(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Offset, _Out_ PULONG Value) {
    ULONGLONG address = Extension->CsrBaseAddress + Offset;
    NTSTATUS status;

    *Value = 0;
    status = PhotonicAsyncQuadlet(Extension, address, FALSE, Value);

    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_1394, "read  reg 0x%03x -> 0x%08x\n", Offset, *Value);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "read  reg 0x%03x failed: %!STATUS!\n", Offset, status);
    }
    return status;
}

NTSTATUS Photonic1394WriteRegister(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Offset, _In_ ULONG Value) {
    ULONGLONG address = Extension->CsrBaseAddress + Offset;
    ULONG value = Value;
    NTSTATUS status;

    status = PhotonicAsyncQuadlet(Extension, address, TRUE, &value);

    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_1394, "write reg 0x%03x <- 0x%08x\n", Offset, Value);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "write reg 0x%03x <- 0x%08x failed: %!STATUS!\n", Offset, Value,
                    status);
    }
    return status;
}

NTSTATUS Photonic1394ReadBlock(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONGLONG Address,
                               _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length) {
    NTSTATUS status = PhotonicAsyncTransfer(Extension, Address, FALSE, Buffer, Length, TCODE_READ_REQUEST_BLOCK);

    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_1394, "read  block 0x%I64x len=%u\n", Address, Length);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "read  block 0x%I64x len=%u failed: %!STATUS!\n", Address,
                    Length, status);
    }
    return status;
}

NTSTATUS Photonic1394WriteBlock(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONGLONG Address,
                                _In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length) {
    NTSTATUS status = PhotonicAsyncTransfer(Extension, Address, TRUE, Buffer, Length, TCODE_WRITE_REQUEST_BLOCK);

    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_1394, "write block 0x%I64x len=%u\n", Address, Length);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "write block 0x%I64x len=%u failed: %!STATUS!\n", Address,
                    Length, status);
    }
    return status;
}

/// PhotonicGetUnitDependentDirectory -- ask the 1394 bus driver for this unit's
/// unit-dependent directory via REQUEST_GET_CONFIGURATION_INFO. The bus driver
/// has already parsed the Configuration ROM (root -> unit -> unit-dependent
/// directory) while enumerating the node, so this avoids walking the ROM by hand
/// over async reads. Done in two passes: the first (null directory buffers)
/// returns the required directory size; the second copies the directory into a
/// freshly allocated buffer. On success *Directory (caller frees with
/// PHOTONIC_POOL_TAG) holds the directory quadlets in host byte order and
/// *SizeInBytes its size.
///
/// The bus driver copies the bus-info block into the ConfigRom field on every
/// call -- that field has no size member, so it is a fixed sizeof(CONFIG_ROM)
/// copy and must always point at a valid buffer. Leaving it null faults inside
/// the bus driver (it writes the block unconditionally), so a ConfigRom buffer
/// is supplied on both passes even though only the unit-dependent directory is
/// of interest here.
///
/// @param Extension   Device extension.
/// @param Directory   Receives a pointer to the allocated directory quadlet array
///                    in host byte order (caller frees with PHOTONIC_POOL_TAG).
/// @param SizeInBytes Receives the size of the directory buffer in bytes.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicGetUnitDependentDirectory(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Outptr_ PULONG *Directory,
                                                  _Out_ PULONG SizeInBytes) {
    IRB irb;
    PCONFIG_ROM configRom;
    PULONG buffer;
    ULONG size;
    ULONG count;
    ULONG i;
    BOOLEAN abandoned;
    NTSTATUS status;

    *Directory = NULL;
    *SizeInBytes = 0;

    configRom = (PCONFIG_ROM) ExAllocatePoolZero(NonPagedPoolNx, sizeof(CONFIG_ROM), PHOTONIC_POOL_TAG);
    if (configRom == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "failed to allocate the config ROM header buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // First pass: null directory/leaf buffers with zero sizes make this a size
    // query. The bus driver fills the *BufferSize fields with the sizes it needs
    // (and copies the bus-info block into ConfigRom).
    //
    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_GET_CONFIGURATION_INFO;
    irb.u.GetConfigurationInformation.ConfigRom = configRom;
    status = PhotonicSubmitIrbEx(Extension, &irb, NULL, configRom, NULL, &abandoned, NULL);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "GET_CONFIGURATION_INFO (size query) failed: %!STATUS!\n",
                    status);
        if (!abandoned) {
            ExFreePoolWithTag(configRom, PHOTONIC_POOL_TAG);
        }
        return status;
    }

    size = irb.u.GetConfigurationInformation.UnitDependentDirectoryBufferSize;
    if (size < sizeof(ULONG)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "no unit-dependent directory in config ROM\n");
        ExFreePoolWithTag(configRom, PHOTONIC_POOL_TAG);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    buffer = (PULONG) ExAllocatePoolZero(NonPagedPoolNx, size, PHOTONIC_POOL_TAG);
    if (buffer == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394,
                    "failed to allocate the %u-byte unit-dependent directory buffer\n", size);
        ExFreePoolWithTag(configRom, PHOTONIC_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Second pass: only the unit-dependent directory is needed, so leave the
    // other directory and leaf buffers null; the bus driver copies just that
    // directory into the buffer (and the bus-info block into ConfigRom again).
    //
    RtlZeroMemory(&irb, sizeof(irb));
    irb.FunctionNumber = REQUEST_GET_CONFIGURATION_INFO;
    irb.u.GetConfigurationInformation.ConfigRom = configRom;
    irb.u.GetConfigurationInformation.UnitDependentDirectoryBufferSize = size;
    irb.u.GetConfigurationInformation.UnitDependentDirectory = buffer;
    status = PhotonicSubmitIrbEx(Extension, &irb, NULL, configRom, buffer, &abandoned, NULL);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394, "GET_CONFIGURATION_INFO (fetch) failed: %!STATUS!\n", status);
        if (!abandoned) {
            ExFreePoolWithTag(buffer, PHOTONIC_POOL_TAG);
            ExFreePoolWithTag(configRom, PHOTONIC_POOL_TAG);
        }
        return status;
    }

    //
    // Normalize byte order from the bus-info-block signature. The bus driver
    // hands back quadlets in whatever order it caches the ROM; if the signature
    // (which must read as CONFIG_ROM_SIGNATURE) comes back byte-reversed, the
    // unit-dependent directory quadlets are byte-reversed too, so swap them to
    // host order. Callers can then read key (bits 31..24) and value (23..0)
    // directly.
    //
    count = size / sizeof(ULONG);
    if (configRom->CR_Signiture == RtlUlongByteSwap(CONFIG_ROM_SIGNATURE)) {
        for (i = 0; i < count; i++) {
            buffer[i] = RtlUlongByteSwap(buffer[i]);
        }
    } else if (configRom->CR_Signiture != CONFIG_ROM_SIGNATURE) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_1394,
                    "unexpected config ROM signature 0x%08x; assuming host byte order\n", configRom->CR_Signiture);
    }

    for (i = 0; i < count; i++) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_1394, "udep[%u]=0x%08x\n", i, buffer[i]);
    }

    *Directory = buffer;
    *SizeInBytes = size;
    ExFreePoolWithTag(configRom, PHOTONIC_POOL_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS Photonic1394CsrPointerToOffset(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Pointer,
                                        _In_ ULONG BlockBytes, _Out_ PULONG Offset) {
    //
    // The pointer is a quadlet offset from the initial register space base
    // (high 16 bits 0xFFFF, low 32 bits 0xF0000000). Resolve it to an absolute
    // 48-bit address, then express it relative to the camera's CSR base so the
    // existing register helpers (which add CsrBaseAddress) reach it.
    //
    ULONGLONG base = (PHOTONIC_REGISTER_SPACE_HIGH << 32) | PHOTONIC_REGISTER_SPACE_LOW;
    ULONGLONG absolute = base + (ULONGLONG) Pointer * sizeof(ULONG);
    ULONGLONG windowMin = (PHOTONIC_REGISTER_SPACE_HIGH << 32) | PHOTONIC_CSR_BASE_MIN;
    ULONGLONG windowMax = (PHOTONIC_REGISTER_SPACE_HIGH << 32) | PHOTONIC_CSR_BASE_MAX;

    *Offset = 0;

    //
    // The pointer is device-supplied input. One whose block (the caller
    // accesses registers up to BlockBytes past the base) does not fit the
    // plausible register window, or that lies below the discovered CSR base
    // (where the subtraction would wrap unsigned), would alias arbitrary
    // camera registers on every later access, so it is rejected and the
    // caller skips whatever advertised it.
    //
    if (absolute < windowMin || absolute + BlockBytes > windowMax || absolute < Extension->CsrBaseAddress) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394,
                    "CSR pointer 0x%08x resolves to 0x%012llx, outside the register window (base 0x%012llx)\n", Pointer,
                    absolute, Extension->CsrBaseAddress);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    *Offset = (ULONG) (absolute - Extension->CsrBaseAddress);
    return STATUS_SUCCESS;
}

NTSTATUS Photonic1394DiscoverCsrBase(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    PULONG directory;
    ULONG sizeInBytes;
    ULONG count;
    ULONG i;
    ULONG csrBaseLow;
    BOOLEAN found;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_1394);

    status = PhotonicGetUnitDependentDirectory(Extension, &directory, &sizeInBytes);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // The unit-dependent directory is a quadlet array in host byte order: a
    // directory header (its high 16 bits are the entry count) followed by one
    // quadlet per entry (key in bits 31..24, value in 23..0). Scan it for the
    // DCAM command-base entry (key 0x40); its value is a quadlet offset above
    // the initial register space, so the command base low-32 = 0xF0000000 +
    // value*4. The header's key byte is always 0, so scanning from index 0 will
    // not mistake it for an entry regardless of whether it is included.
    //
    count = sizeInBytes / sizeof(ULONG);
    found = FALSE;
    csrBaseLow = 0;
    for (i = 0; i < count; i++) {
        if (((directory[i] >> 24) & 0xFF) == PHOTONIC_KEY_COMMAND_BASE) {
            csrBaseLow = PHOTONIC_REGISTER_SPACE_LOW + ((directory[i] & 0x00FFFFFFUL) << 2);
            found = TRUE;
            break;
        }
    }

    ExFreePoolWithTag(directory, PHOTONIC_POOL_TAG);

    if (!found) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394,
                    "no command-base (key 0x40) entry in unit-dependent directory\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //
    // The command base is device-supplied input. A value outside the
    // plausible window would redirect every subsequent register read and
    // write to arbitrary node addresses, and the garbage read back would
    // flow into mode and feature discovery, so a broken ROM fails the
    // device here rather than being used with a warning.
    //
    if (csrBaseLow < PHOTONIC_CSR_BASE_MIN || csrBaseLow >= PHOTONIC_CSR_BASE_MAX) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_1394,
                    "command base 0x%08x outside the expected window [0x%08x..0x%08x); rejecting the device\n",
                    csrBaseLow, PHOTONIC_CSR_BASE_MIN, PHOTONIC_CSR_BASE_MAX);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Extension->CsrBaseAddress = (PHOTONIC_REGISTER_SPACE_HIGH << 32) | csrBaseLow;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_1394, "CSR base discovered: low=0x%08x full=0x%012llx\n",
                csrBaseLow, Extension->CsrBaseAddress);
    return STATUS_SUCCESS;
}
