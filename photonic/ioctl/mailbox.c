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
/// Photonic IOCTL handlers: mailbox / external I2C register access.
/// See ioctl/ioctl.c for the dispatch mechanism.

// clang-format off
// Include order is significant and must not be sorted: photonic.h pulls in the
// WPP config, and mailbox.tmh (WPP-generated) must come after it. ioctl.h needs
// CTL_CODE and friends, provided by wdm.h via photonic.h -> strmini.h.
#include "photonic.h"
#include "p1394.h"
#include "ioctl.h"
#include "ioctl_private.h"
#include "mailbox.tmh"
// clang-format on

/// MAILBOX_CMD_WRITE_REGISTER / MAILBOX_CMD_READ_REGISTER: direct camera CSR
/// register access through PHOTONIC_MAILBOX_REGISTER_IN. Flags must be -1
/// (the CSR-access selector); Address is the register offset from the camera's
/// CSR base. A read returns the structure with Value filled in.
///
/// @param Request  Decoded IOCTL request.
/// @param IsWrite  TRUE to write the register, FALSE to read it.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicMailboxRegisterAccess(_Inout_ PPHOTONIC_IOCTL_REQUEST Request, _In_ BOOLEAN IsWrite) {
    PHOTONIC_MAILBOX_REGISTER_IN *reg = Request->Buffer;
    ULONG value;
    NTSTATUS status;

    FuncEntry(TRACE_FLAG_IOCTL);

    //
    // A write consumes the whole structure from the input. A read consumes
    // Flags and Address from the input and returns the whole structure.
    //
    if (IsWrite ? Request->InputLength < sizeof(*reg)
                : (Request->InputLength < RTL_SIZEOF_THROUGH_FIELD(PHOTONIC_MAILBOX_REGISTER_IN, Address) ||
                   Request->OutputLength < sizeof(*reg))) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "buffer too small: inLen=%u outLen=%u write=%u\n",
                    Request->InputLength, Request->OutputLength, IsWrite);
        Request->Error = PL_ERROR_INVALID_COUNT;
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (reg->Flags != -1) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "flags 0x%04x: only CSR access (-1) is supported\n",
                    (USHORT) reg->Flags);
        Request->Error = PL_ERROR;
        return STATUS_INVALID_PARAMETER;
    }

    if (IsWrite) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "write reg 0x%x <- 0x%08x\n", reg->Address, reg->Value);
        status = Photonic1394WriteRegister(Request->Extension, reg->Address, reg->Value);
    } else {
        status = Photonic1394ReadRegister(Request->Extension, reg->Address, &value);
        if (NT_SUCCESS(status)) {
            reg->Value = value;
            Request->Information = sizeof(*reg);
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "read reg 0x%x -> 0x%08x\n", reg->Address,
                        reg->Value);
        }
    }

    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_HARDWARE;
    }
    return status;
}

/// Upper bound on a raw mailbox packet, input and output alike. Real packets
/// are tens of bytes; the bound only keeps a corrupt caller from costing two
/// nonpaged allocations of an arbitrary caller-chosen size (the I/O manager's
/// system buffer plus the transfer copy) and an oversized block transaction
/// held under the interface mutex.
#define PHOTONIC_MAILBOX_MAX_PACKET_BYTES 4096u

/// Mailbox commands >= MAILBOX_CMD_RAW_MIN: the whole input buffer (command
/// word included) is a raw mailbox packet written to the camera's mailbox CSR;
/// when the caller expects a response, it is read back from the same address.
/// The packet is quadlet-structured, so every DWORD is byte-swapped to 1394
/// bus order on the way out and back on the way in.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicMailboxRawPacket(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    ULONGLONG address = ((ULONGLONG) 0xFFFF << 32) | MAILBOX_BASE_CSR_ADDR;
    PULONG buffer = Request->Buffer;
    NTSTATUS status;
    ULONG i;

    FuncEntry(TRACE_FLAG_IOCTL);

    //
    // The DLL sends DWORD-aligned packets ((len + 0x0B) & ~3, see ioctl.h);
    // the DWORD swap below cannot handle a partial trailing word.
    //
    if ((Request->InputLength % sizeof(ULONG)) != 0 || (Request->OutputLength % sizeof(ULONG)) != 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "packet not DWORD-aligned: inLen=%u outLen=%u\n",
                    Request->InputLength, Request->OutputLength);
        Request->Error = PL_ERROR_INVALID_COUNT;
        return STATUS_INVALID_BUFFER_SIZE;
    }

    if (Request->InputLength > PHOTONIC_MAILBOX_MAX_PACKET_BYTES ||
        Request->OutputLength > PHOTONIC_MAILBOX_MAX_PACKET_BYTES) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "packet too large: inLen=%u outLen=%u (max %u)\n",
                    Request->InputLength, Request->OutputLength, PHOTONIC_MAILBOX_MAX_PACKET_BYTES);
        Request->Error = PL_ERROR_INVALID_COUNT;
        return STATUS_INVALID_BUFFER_SIZE;
    }

    for (i = 0; i < Request->InputLength / sizeof(ULONG); i++) {
        buffer[i] = RtlUlongByteSwap(buffer[i]);
    }

    status = Photonic1394WriteBlock(Request->Extension, address, buffer, Request->InputLength);
    if (!NT_SUCCESS(status)) {
        Request->Error = PL_ERROR_HARDWARE;
        return status;
    }

    if (Request->OutputLength > 0) {
        status = Photonic1394ReadBlock(Request->Extension, address, buffer, Request->OutputLength);
        if (!NT_SUCCESS(status)) {
            Request->Error = PL_ERROR_HARDWARE;
            return status;
        }

        for (i = 0; i < Request->OutputLength / sizeof(ULONG); i++) {
            buffer[i] = RtlUlongByteSwap(buffer[i]);
        }
        Request->Information = Request->OutputLength;
    }

    return STATUS_SUCCESS;
}

/// PHOTONIC_IOCTL_MAILBOX -- multiplexed command gate selected by the first
/// UINT32 of the buffer: CSR register read/write (0x10 / 0x14) and raw
/// mailbox packets (>= 0x1000, including external I2C access).
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlMailbox(_Inout_ PPHOTONIC_IOCTL_REQUEST Request) {
    PHOTONIC_MAILBOX_CMD_HDR *hdr = Request->Buffer;
    ULONG command = hdr->Command;

    FuncEntry(TRACE_FLAG_IOCTL);

    if (Request->Extension->Removed) {
        Request->Error = PL_ERROR_DEVICE_NOT_FOUND;
        return STATUS_DEVICE_REMOVED;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL, "command 0x%x\n", command);

    switch (command) {
        case MAILBOX_CMD_GET_ADDRESSES:
            //
            // Returns the camera serial-number words. The driver does not read
            // the serial number from the camera yet, and nothing issues this
            // command (no DLL wrapper exists).
            //
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL, "GET_ADDRESSES not implemented\n");
            return STATUS_NOT_IMPLEMENTED;

        case MAILBOX_CMD_WRITE_REGISTER:
            return PhotonicMailboxRegisterAccess(Request, TRUE);

        case MAILBOX_CMD_READ_REGISTER:
            return PhotonicMailboxRegisterAccess(Request, FALSE);

        default:
            if (command >= MAILBOX_CMD_RAW_MIN) {
                return PhotonicMailboxRawPacket(Request);
            }

            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL, "unknown command 0x%x\n", command);
            Request->Error = PL_ERROR;
            return STATUS_INVALID_PARAMETER;
    }
}
