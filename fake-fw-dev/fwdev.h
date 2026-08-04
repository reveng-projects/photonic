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
/// Generic local-node FireWire device object built on the Linux
/// firewire-cdev userspace API.  Encapsulates everything that is NOT
/// specific to a particular emulated device:
///
///   - opening /dev/fw* and reading node information
///   - injecting a Configuration ROM (vendor/model textual leaves and a
///     unit directory) so a remote host enumerates the device
///   - allocating 48-bit CSR address regions and dispatching incoming
///     read/write requests to per-region handlers
///   - sending request responses and AV/C FCP responses
///   - the poll()/read() event loop
///
/// Device-specific behaviour (register maps, video formats, isochronous
/// streaming, AV/C semantics, ...) is provided by the caller through the
/// handler callbacks registered on each region.

#ifndef FWDEV_H
#define FWDEV_H

#include <stdint.h>

#include <linux/firewire-cdev.h>

/// Opaque generic FireWire device object.
typedef struct fw_device fw_device_t;

/// Configuration-ROM identity styles the device can advertise.  Selects how
/// the host builds the unit's hardware ID:
///
///   FW_ROM_MODE_NAME — the unit carries a Vendor_Id + vendor textual leaf and a
///       Model_Id + model textual leaf, so Windows builds the textual hardware ID
///       1394\<Vendor>&<Model> (e.g. 1394\Vitana&PixeLINK(tm)_-_Photonic) from
///       the unit directory.  The vendor leaf lives in the unit (not the root)
///       because firewire-core owns the root Module_Vendor_Id ("Linux Firewire");
///       the unit's own vendor leaf overrides it for this unit.
///
///   FW_ROM_MODE_IDS — publish only unit_spec_id + unit_sw_version (no Model_Id,
///       no textual leaves), like the kernel's IP-over-1394 units.  Windows
///       identifies the unit by 1394\<spec>&<version> (e.g. 1394\A02D&100),
///       which is vendor-independent and identical on XP and Win8+.
typedef enum {
    FW_ROM_MODE_NAME,
    FW_ROM_MODE_IDS,
} fw_rom_mode_t;

/// Handler invoked for an incoming read or write request that targets a
/// region registered with fw_device_allocate().  The handler is expected to
/// answer the request with fw_device_send_response().
typedef void (*fw_request_handler_t)(fw_device_t *dev, const struct fw_cdev_event_request2 *req, void *ctx);

/// Handler invoked on each isochronous transmit completion interrupt.
/// completed_packets is how many packets the kernel reports as transmitted since
/// the previous interrupt (derived from the event's header_length); a frame may
/// span several interrupts, so the handler must accumulate this rather than
/// assume one interrupt equals one frame.
typedef void (*fw_iso_interrupt_handler_t)(fw_device_t *dev, void *ctx, unsigned completed_packets);

/// Handler invoked when poll() reports an error condition (POLLERR/POLLHUP)
/// on the isochronous fd.  The condition is level-triggered, so the handler
/// must clear it by tearing the iso context down (closing the fd).  Without a
/// registered handler the run loop stops polling the fd instead, so a dead
/// context cannot spin the loop.
typedef void (*fw_iso_error_handler_t)(fw_device_t *dev, void *ctx);

/// Opens the local node device at `path` (e.g. "/dev/fw0") and records the
/// Configuration ROM identity to publish later.  The identity strings must
/// outlive the device (string literals are fine).  The Config ROM is not
/// injected until fw_device_publish(); this lets the caller first allocate
/// device-specific regions whose address the unit directory references.
///
/// @param path             Path to the firewire-cdev node (e.g. "/dev/fw0").
/// @param vendor           Vendor name string.
/// @param vendor_id        24-bit Vendor_Id.
/// @param name             Model name string.
/// @param model_id         24-bit Model_Id.
/// @param unit_spec_id     24-bit unit_spec_id (DCAM: 0x00A02D).
/// @param unit_sw_version  24-bit unit_sw_version (DCAM 1.0: 0x000100).
/// @return                 New device object, or NULL on failure.
fw_device_t *create_device(const char *path, const char *vendor, uint32_t vendor_id, const char *name,
                           uint32_t model_id, uint32_t unit_spec_id, uint32_t unit_sw_version);

/// Releases the device and all resources.  Safe to call with NULL.
///
/// @param dev  Device to destroy.
void fw_device_destroy(fw_device_t *dev);

/// Returns the underlying firewire-cdev file descriptor (for iso ioctls/mmap).
///
/// @param dev  Device to query.
/// @return     The fd.
int fw_device_fd(const fw_device_t *dev);

/// Returns the path of the local-node device (e.g. "/dev/fw0"), as passed to
/// create_device().  Used to open a dedicated fd for the iso context.
///
/// @param dev  Device to query.
/// @return     The device path string.
const char *fw_device_path(const fw_device_t *dev);

/// Registers a dedicated firewire-cdev fd that owns the isochronous transmit
/// context (separate from the control fd so the context can be torn down and
/// recreated on every mode switch by close()/open()).  The run loop polls this
/// fd for FW_CDEV_EVENT_ISO_INTERRUPT events in addition to the control fd.
/// Pass -1 to stop polling it (e.g. after the iso fd is closed).
///
/// @param dev     Device to update.
/// @param iso_fd  Dedicated iso fd, or -1 to stop polling.
void fw_device_set_iso_fd(fw_device_t *dev, int iso_fd);

/// Allocates a CSR address region.  `offset` and `region_end` bound the search
/// range the kernel uses to place the region (set offset==region_end-length for
/// a fixed address).  Incoming reads are dispatched to on_read and writes to
/// on_write (either may be NULL to fall back to a generic COMPLETE response).
///
/// @param dev         Device to register the region on.
/// @param offset      Start of the kernel's search range.
/// @param region_end  End of the search range.
/// @param length      Region size in bytes.
/// @param on_read     Handler for read requests, or NULL.
/// @param on_write    Handler for write requests, or NULL.
/// @param ctx         Caller context passed to the handlers.
/// @return            The 48-bit base address the kernel assigned, or 0 on failure.
uint64_t fw_device_allocate(fw_device_t *dev, uint64_t offset, uint64_t region_end, uint32_t length,
                            fw_request_handler_t on_read, fw_request_handler_t on_write, void *ctx);

/// Enables a unit-dependent directory (key 0xD4) carrying a single key-0x40
/// entry that encodes `csr_base` in the published unit directory.  Must be
/// called before fw_device_publish().
///
/// @param dev       Device to update.
/// @param csr_base  Low 32 bits of the DCAM CSR region base address.
void fw_device_set_csr_base(fw_device_t *dev, uint32_t csr_base);

/// Selects the Configuration-ROM identity style (see fw_rom_mode_t).  Must be
/// called before fw_device_publish().  Defaults to FW_ROM_MODE_NAME.
///
/// @param dev   Device to update.
/// @param mode  Identity style to use.
void fw_device_set_rom_mode(fw_device_t *dev, fw_rom_mode_t mode);

/// Registers the isochronous-transmit completion handler.
///
/// @param dev      Device to update.
/// @param handler  Callback invoked on each iso completion interrupt.
/// @param ctx      Caller context passed to the handler.
void fw_device_set_iso_interrupt_handler(fw_device_t *dev, fw_iso_interrupt_handler_t handler, void *ctx);

/// Registers the isochronous-fd error handler (see fw_iso_error_handler_t).
///
/// @param dev      Device to update.
/// @param handler  Callback invoked when the iso fd reports an error condition.
/// @param ctx      Caller context passed to the handler.
void fw_device_set_iso_error_handler(fw_device_t *dev, fw_iso_error_handler_t handler, void *ctx);

/// Injects the Configuration ROM (a single unit directory) which triggers a bus
/// reset so the remote host re-reads the ROM.  Nothing is added to the root
/// directory beyond the unit-directory pointer.  The published identity style is
/// set by fw_device_set_rom_mode().
///
/// @param dev  Device to publish.
/// @return     0 on success, -1 if no /dev/fw* accepted the descriptors.
int fw_device_publish(fw_device_t *dev);

/// Answers an incoming request.  For reads, `data`/`len` carry the payload; for
/// writes pass data=NULL/len=0.  Uses req->handle to release the kernel-side
/// pending request.
///
/// @param dev    Device to send the response on.
/// @param req    The request being answered.
/// @param rcode  1394 response code (e.g. RCODE_COMPLETE).
/// @param data   Response payload, or NULL for write responses.
/// @param len    Payload length in bytes.
void fw_device_send_response(fw_device_t *dev, const struct fw_cdev_event_request2 *req, int rcode, const void *data,
                             uint32_t len);

/// Sends an AV/C response as a 1394 write to the initiator's FCP_RESPONSE
/// register.  Locates and caches the remote node's /dev/fw* fd, re-resolving it
/// after each bus reset.
///
/// @param dev           Device to send the response from.
/// @param dest_node_id  Target 1394 node ID.
/// @param generation    Current bus generation.
/// @param resp          Response payload.
/// @param len           Payload length in bytes.
void fw_device_send_fcp_response(fw_device_t *dev, uint16_t dest_node_id, uint32_t generation, const uint8_t *resp,
                                 uint32_t len);

/// Runs the event loop forever (returns only on fatal error).
///
/// @param dev  Device to service.
void fw_device_run(fw_device_t *dev);

#endif // FWDEV_H
