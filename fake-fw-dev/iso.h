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
/// Isochronous transmit tunables: packet-per-frame limits, ring depth,
/// interrupt cadence and DMA buffer sizing for the fw-cdev transmit path.

#ifndef ISO_H
#define ISO_H

#include "dcam.h"
#include "fwdev.h"

/// Isochronous transmit limits / buffer sizing.  A frame is at most this many iso
/// packets (matches the standard DCAM packet table's maximum).
constexpr int ISO_MAX_PACKETS_PER_FRAME = 4096;
constexpr int ISO_RING_FRAMES_MAX = 4;
/// Request a transmit-completion interrupt at least this often (in packets)
/// within a frame, not only on its last packet.  The fw-cdev iso-transmit path
/// buffers a 4-byte timestamp per completed packet in a PAGE_SIZE (1024-entry)
/// buffer and only delivers the userspace event at an interrupt-flagged packet;
/// a frame with more than 1024 packets (low frame rates, e.g. 1920 pkts at
/// 3.75 fps) overflows that buffer before its last packet and the event is
/// dropped, so the refill loop never runs and the stream stalls. Flagging an
/// interrupt every ISO_INTR_STRIDE packets keeps each batch well under 1024 so
/// the kernel always flushes. Must be < 1024.
constexpr int ISO_INTR_STRIDE = 512;
/// Maximum packets submitted in a single QUEUE_ISO call (the packet-descriptor
/// scratch array is sized to this; a frame is submitted in chunks of this size).
constexpr int ISO_MAX_PKTS_PER_QUEUE = 512;
/// The iso DMA buffer is mapped ONCE per fd and never resized (firewire-cdev
/// binds it to the context on the first mmap and only frees it on close).  Map a
/// fixed maximum up front (1600x1200 RGB24, 2-frame ring ~= 11 MB) and use a
/// prefix sized to each mode.
constexpr uint32_t ISO_BUFFER_MAX_BYTES = 12u * 1024u * 1024u;
constexpr uint32_t ISO_BUFFER_MIN_BYTES = 256u * 1024u;

/// Starts (or resumes) isochronous video transmission of the selected mode.
///
/// @param cam  Camera to start streaming on.
void dcam_iso_start(dcam_camera_t *cam);

/// Stops iso video transmission and tears the iso context down completely.
/// Closing the dedicated fd discards any queued-but-untransmitted packets so
/// that the next dcam_iso_start() begins with an empty queue.
///
/// @param cam  Camera to stop.
void dcam_iso_stop(dcam_camera_t *cam);

/// Iso transmit-completion handler (fw_iso_interrupt_handler_t).  Accumulates
/// completed_packets and refills + re-queues one ring slot per cam->iso_ppf
/// packets acknowledged.
///
/// @param dev                Device the interrupt arrived on.
/// @param ctx                The dcam_camera_t cast to void *.
/// @param completed_packets  Packets reported as transmitted since the previous interrupt.
void dcam_on_iso_interrupt(fw_device_t *dev, void *ctx, unsigned completed_packets);

/// Iso-fd error handler (fw_iso_error_handler_t).  Invoked by the run loop
/// when poll() reports POLLERR/POLLHUP on the iso fd.  Tears the transmit
/// context down, which closes the fd and clears the level-triggered
/// condition.
///
/// @param dev  Device the error was reported on.
/// @param ctx  The dcam_camera_t cast to void *.
void dcam_on_iso_error(fw_device_t *dev, void *ctx);

#endif // ISO_H
