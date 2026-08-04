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
/// 1394 bus transport for the Photonic minidriver.
///
/// Thin synchronous wrapper over the 1394 bus driver's IRB interface
/// (IOCTL_1394_CLASS). The minidriver does not own a device object of its own;
/// the stream class driver hands it the camera's unit PDO in
/// PORT_CONFIGURATION_INFORMATION.PhysicalDeviceObject, which is stored in the
/// device extension and used here as the target of every IoCallDriver. The PDO's
/// dispatch routines are owned by the 1394 bus driver, which services the IRB
/// interface; sending the IOCTL up to the stream-class FDO instead would fail
/// with STATUS_INVALID_DEVICE_REQUEST because that driver does not handle it.
///
/// Everything in this module runs synchronously at PASSIVE_LEVEL: each call
/// builds an IRB, submits it, and blocks until the bus driver completes it.
/// That matches how the camera is brought up -- during SRB_INITIALIZE_DEVICE,
/// which the class driver issues at PASSIVE_LEVEL.

#ifndef PHOTONIC_P1394_H
#define PHOTONIC_P1394_H

#include "photonic.h"

/// Send an IRB down to the camera's unit PDO and block until the bus driver
/// completes it. Exposed so the isochronous capture engine (capture/) can
/// submit isoch resource IRBs without duplicating the synchronous
/// submit/timeout plumbing. Runs at PASSIVE_LEVEL.
///
/// The IRB is passed by value under the hood: the caller's IRB (stack is
/// fine) is copied into a pool-resident request, the bus driver only ever
/// sees the copy, and on completion the copy -- now carrying the result
/// fields -- is copied back. The caller therefore has no cleanup duties on
/// any path. When the request must be abandoned (both the wait and the
/// post-cancel wait time out) the call returns STATUS_IO_TIMEOUT without
/// result fields, and the machinery frees the pool request if the wedged
/// request ever completes. Any memory the IRB points at must be pool
/// resident and registered with the submit so it survives an abandoned
/// request; that variant is internal to p1394.c.
///
/// Every submission holds the device extension's IrbRemoveLock, so the
/// teardown wait in SRB_UNINITIALIZE_DEVICE covers abandoned requests. A
/// submit attempted after that wait has begun fails with
/// STATUS_DELETE_PENDING.
///
/// @param Extension  Device extension.
/// @param Irb        IRB to submit; receives the result fields on completion.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicSubmitIrb(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Inout_ PIRB Irb);

/// Refresh the cached 1394 bus state into the device extension: the bus
/// generation count (required on every async transaction, so this must succeed
/// before any register access), the host controller capabilities, and the
/// maximum speed to the camera. Only the generation count is fatal on failure;
/// the capability and speed queries fall back to defaults.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394RefreshBusState(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Return the SCODE the isochronous stream runs at: the maximum speed between
/// the host and the camera capped at SCODE_400_RATE, the fastest DCAM
/// isochronous transmit rate. The same scode drives the camera's
/// ISOCH_CHANNEL register and the host's isochronous allocations (capture/),
/// so the two sides cannot disagree.
///
/// @param Extension  Device extension.
/// @return SCODE_*_RATE value for the stream.
UCHAR Photonic1394StreamScode(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Return the largest isochronous packet payload the stream speed carries:
/// 1024 bytes per packet at S100, doubling with each speed step (IEEE 1394).
/// The bus driver rejects bandwidth and resource allocations that ask for more,
/// so every packet-size choice is bounded by this.
///
/// @param Extension  Device extension.
/// @return Maximum isochronous payload in bytes.
ULONG Photonic1394MaxIsochPayload(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Map an SCODE to the IEEE 1394 speed name it stands for (e.g. SCODE_400_RATE
/// -> "S400"), for trace output.
///
/// @param Scode  SCODE_*_RATE value.
/// @return Pointer to a string literal naming the speed.
const char *Photonic1394SpeedName(_In_ UCHAR Scode);

/// Register the bus-reset notification routine with the 1394 bus driver.
/// Done during bring-up so a bus reset marks the cached generation count stale
/// (recovered on the next transaction). Idempotent with respect to Extension
/// state.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394RegisterBusResetNotification(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Deregister the bus-reset notification routine from the 1394 bus driver.
/// Done at teardown so the bus driver drops its reference and the device stack
/// can be torn down. Retries a failed deregistration a bounded number of
/// times: the caller runs exactly once, and a stranded registration would
/// leave the bus driver holding a callback into memory about to be freed.
/// Idempotent with respect to Extension state.
///
/// @param Extension  Device extension.
VOID Photonic1394DeregisterBusResetNotification(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Read a single 32-bit DCAM register. Offset is relative to the camera's CSR
/// base (extension->CsrBaseAddress), e.g. 0x100 for VIDEO_FORMAT_INQ. Values
/// are exchanged in logical (host) byte order: this routine byte-swaps from
/// 1394 bus order, matching how a quadlet read off the bus must be swapped to
/// be interpreted on a little-endian host.
///
/// @param Extension  Device extension.
/// @param Offset     Register offset relative to the camera's CSR base.
/// @param Value      Receives the register value in host byte order.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394ReadRegister(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Offset, _Out_ PULONG Value);

/// Write a single 32-bit DCAM register. Offset is relative to the camera's CSR
/// base (extension->CsrBaseAddress), e.g. 0x100 for VIDEO_FORMAT_INQ. Values
/// are exchanged in logical (host) byte order: this routine byte-swaps to
/// 1394 bus order, matching how a quadlet must be swapped before being sent
/// on the bus from a little-endian host.
///
/// @param Extension  Device extension.
/// @param Offset     Register offset relative to the camera's CSR base.
/// @param Value      Value to write in host byte order.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394WriteRegister(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Offset, _In_ ULONG Value);

/// Read a block of bytes from an absolute 48-bit 1394 address. Unlike the
/// register routines above, this takes an absolute address rather than an
/// offset relative to the camera's DCAM CSR base. Bytes are transferred
/// verbatim (no byte-swapping), so a caller exchanging quadlet-structured
/// data swaps DWORDs itself. Buffer may be any memory; it is staged through
/// a non-paged bounce buffer internally. Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Address    Absolute 48-bit 1394 address to read from.
/// @param Buffer     Receives the data read from the bus.
/// @param Length     Number of bytes to read.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394ReadBlock(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONGLONG Address,
                               _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length);

/// Write a block of bytes to an absolute 48-bit 1394 address. Unlike the
/// register routines above, this takes an absolute address rather than an
/// offset relative to the camera's DCAM CSR base. Bytes are transferred
/// verbatim (no byte-swapping), so a caller exchanging quadlet-structured
/// data swaps DWORDs itself. Buffer may be any memory; it is staged through
/// a non-paged bounce buffer internally. Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Address    Absolute 48-bit 1394 address to write to.
/// @param Buffer     Data to write to the bus.
/// @param Length     Number of bytes to write.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394WriteBlock(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONGLONG Address,
                                _In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length);

/// Discover the camera's DCAM CSR base address from the Configuration ROM and
/// store it in extension->CsrBaseAddress. The base is encoded as a key-0x40
/// (command base) entry in the unit-dependent directory; this asks the bus
/// driver for that directory and parses it. Returns STATUS_SUCCESS and sets the
/// base on success.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394DiscoverCsrBase(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Convert a DCAM CSR pointer -- a quadlet offset from the 1394 initial register
/// space base (0xFFFFF0000000), as stored in a V_CSR_INQ register -- into an
/// offset relative to the camera's CSR base, usable directly with
/// Photonic1394ReadRegister / Photonic1394WriteRegister. Format 7 advertises each
/// mode's CSR block this way. The pointer is device-supplied input: one whose
/// BlockBytes-sized register block does not fit the plausible register window
/// fails with STATUS_DEVICE_CONFIGURATION_ERROR and the caller skips the mode
/// that advertised it.
///
/// @param Extension   Device extension.
/// @param Pointer     Quadlet offset from the 1394 initial register space base.
/// @param BlockBytes  Size of the register block accessed through the pointer,
///                    so its tail registers are validated too.
/// @param Offset      Receives the offset relative to the camera's CSR base.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS Photonic1394CsrPointerToOffset(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Pointer,
                                        _In_ ULONG BlockBytes, _Out_ PULONG Offset);

/// PhotonicSubmitIrb variant reporting whether the request reached the bus
/// driver at all. *Submitted == FALSE means the request could not even be
/// built (pool or IRP allocation failed, or device teardown has begun), so
/// the bus driver never saw it and the caller may safely re-issue it later.
/// *Submitted == TRUE means the bus driver saw the request exactly once,
/// whatever the returned status, so it must not be re-issued.
///
/// @param Extension  Device extension.
/// @param Irb        IRB to submit; receives the result fields on completion.
/// @param Submitted  Receives whether the request reached the bus driver.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicSubmitIrbTracked(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Inout_ PIRB Irb,
                                  _Out_ PBOOLEAN Submitted);

#endif // PHOTONIC_P1394_H
