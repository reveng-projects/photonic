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
/// DriverEntry for the Photonic stream class minidriver.
///
/// Instead of building a WDM dispatch table and an AddDevice callback, the
/// driver hands a HW_INITIALIZATION_DATA block to StreamClassRegisterAdapter.
/// From that point on the stream class driver owns the device object and PnP,
/// and calls back into this minidriver through the SRB handlers wired up below.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and driver.tmh (WPP-generated) must come after it.
#include "photonic.h"
#include "driver.tmh"
// clang-format on

/// Saved pointer to the unload routine the stream class driver installs (if any)
/// so PhotonicUnload can chain to it and still run WPP_CLEANUP at unload.
static PDRIVER_UNLOAD g_PhotonicNextUnload;

/// PhotonicUnload -- driver unload. Flush WPP tracing last, after chaining to
/// the stream class driver's own unload handler.
///
/// @param DriverObject  The driver object being unloaded.
static VOID PhotonicUnload(_In_ PDRIVER_OBJECT DriverObject) {
    PDRIVER_UNLOAD next = g_PhotonicNextUnload;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DRIVER, "unloading\n");

    if (next != NULL) {
        next(DriverObject);
    }

    WPP_CLEANUP(DriverObject);
}

/// DriverEntry -- driver load entry point. Describes the minidriver to the
/// stream class driver and registers it. No hardware is touched here; the class
/// driver calls PhotonicReceivePacket with SRB_INITIALIZE_DEVICE once the 1394
/// bus reports a matching device.
///
/// @param DriverObject   The driver object created by the I/O manager.
/// @param RegistryPath   Registry path for this driver's service key.
/// @return               STATUS_SUCCESS on success, or an error code.
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
    HW_INITIALIZATION_DATA hwInitData;
    NTSTATUS status;

    //
    // Bring up WPP tracing first so every routine below can trace, then mark
    // entry into the driver.
    //
    WPP_INIT_TRACING(DriverObject, RegistryPath);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DRIVER, "loading\n");

    //
    // The stream/format descriptors are built per device in
    // SRB_INITIALIZE_DEVICE, once the camera's modes have been enumerated, so
    // there is nothing format-related to set up at driver load time.
    //
    RtlZeroMemory(&hwInitData, sizeof(hwInitData));
    hwInitData.HwInitializationDataSize = sizeof(hwInitData);

    //
    // Minidriver callbacks. There is no hardware interrupt, so HwInterrupt is
    // left NULL.
    //
    hwInitData.HwReceivePacket = PhotonicReceivePacket;
    hwInitData.HwCancelPacket = PhotonicCancelPacket;
    hwInitData.HwRequestTimeoutHandler = PhotonicTimeout;

    //
    // Sizes of the contexts the class driver allocates for the minidriver.
    //
    hwInitData.DeviceExtensionSize = sizeof(PHOTONIC_DEVICE_EXTENSION);
    //
    // Per-request workspace holds the list linkage used to park a pended
    // SRB_READ_DATA on the stream's PendingReads queue (see stream/read.c).
    //
    hwInitData.PerRequestExtensionSize = sizeof(PHOTONIC_SRB_EXTENSION);
    hwInitData.PerStreamExtensionSize = sizeof(PHOTONIC_STREAM_EXTENSION);
    hwInitData.FilterInstanceExtensionSize = 0;

    //
    // The class driver's bus-master DMA support is not used: the 1394 bus
    // driver performs the isochronous DMA itself on the frame MDLs the capture
    // engine attaches (capture/).
    //
    // TurnOffSynchronization is TRUE so the class driver calls the SRB handlers
    // at PASSIVE_LEVEL instead of holding its spinlock at DISPATCH_LEVEL. The
    // camera bring-up in SRB_INITIALIZE_DEVICE issues synchronous 1394 IRPs and
    // blocks on them, which is only legal at PASSIVE_LEVEL; completing that SRB
    // synchronously (rather than deferring it) keeps the PnP start/stop sequence
    // simple so device removal -- and driver unload -- is not held off.
    //
    hwInitData.BusMasterDMA = FALSE;
    hwInitData.Dma24BitAddresses = FALSE;
    hwInitData.BufferAlignment = 0;
    hwInitData.TurnOffSynchronization = TRUE;
    hwInitData.DmaBufferSize = 0;

    status = StreamClassRegisterAdapter(DriverObject, RegistryPath, &hwInitData);

    if (NT_SUCCESS(status)) {
        //
        // StreamClassRegisterAdapter installs its own dispatch and unload
        // handler. Chain ours in front of it so WPP_CLEANUP runs at unload.
        //
        g_PhotonicNextUnload = DriverObject->DriverUnload;
        DriverObject->DriverUnload = PhotonicUnload;

        //
        // Hook IRP_MJ_DEVICE_CONTROL in front of the class driver's handler so
        // the Photonic IOCTLs (ioctl/) can be serviced; everything
        // else is forwarded to the class driver.
        //
        PhotonicHookDeviceControl(DriverObject);
    } else {
        //
        // Registration failed: there will be no unload callback, so tear the
        // trace session down here.
        //
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_DRIVER, "StreamClassRegisterAdapter failed: %!STATUS!\n", status);
        WPP_CLEANUP(DriverObject);
    }

    return status;
}
