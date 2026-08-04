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
/// IEEE 1394 CSR address-space layout of the emulated camera: where the DCAM
/// register region, the FCP command register and the command mailbox CSR live
/// in the 48-bit node address space.

#ifndef CSR_H
#define CSR_H

#include <stdint.h>

/// IEEE 1394 CSR base for the 48-bit node address space.
constexpr uint64_t CSR_BASE = UINT64_C(0xfffff0000000);

/// FCP command register (IEC 61883-1): AV/C commands land here.
constexpr uint64_t FCP_COMMAND_ADDR = CSR_BASE + 0x0B00; ///< 0xfffff0000b00
constexpr int FCP_REGION_LENGTH = 512;

/// DCAM CSR register space.  The kernel picks a free slot in a wide range above
/// the kernel-managed Config ROM and FCP areas; the actual address (its low
/// 32 bits = CsrBase) is advertised in the Config ROM unit-dependent directory.
constexpr uint64_t DCAM_SEARCH_START = CSR_BASE + 0x1000ULL;
constexpr uint64_t DCAM_SEARCH_END = CSR_BASE + 0x100000ULL;
constexpr uint32_t DCAM_ADDR_LENGTH = 0x1000U; ///< 4 KB covers all registers

/// Command mailbox CSR.  The host block-writes command packets to this fixed
/// address (low 32 bits 0xF0204000, i.e. CSR offset 0x204000) and reads the
/// response back from the same address.  Sits above the DCAM search window.
constexpr uint64_t DCAM_MAILBOX_ADDR = CSR_BASE + 0x204000ULL; ///< 0xfffff0204000
constexpr uint32_t DCAM_MAILBOX_LENGTH = 0x1000U;

#endif // CSR_H
