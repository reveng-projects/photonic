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
/// Generic local-node FireWire device object.  See fwdev.h for the API.

#include "fwdev.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/firewire-constants.h>

/// IEEE 1394 CSR base address for the 48-bit node address space.
constexpr uint64_t CSR_BASE = UINT64_C(0xfffff0000000);

/// Largest asynchronous block payload the device object accepts inline with an
/// incoming request event.  A 1394 async block at S400 tops out at 2048 bytes;
/// the read buffer for events reserves this much past the event header so a
/// block-write request's inline payload is never truncated.
constexpr int FW_MAX_ASYNC_PAYLOAD = 2048;

/// FCP response register (IEC 61883-1) — target of AV/C responses.
constexpr uint64_t FCP_RESPONSE_ADDR = CSR_BASE + 0x0D00; ///< 0xfffff0000d00

/// One registered CSR address region with its read/write handlers.
typedef struct fw_region {
    uint64_t base;   ///< 48-bit base address the kernel assigned
    uint32_t length; ///< region size in bytes
    fw_request_handler_t on_read;
    fw_request_handler_t on_write;
    void *ctx;
    struct fw_region *next;
} fw_region_t;

struct fw_device {
    int fd; ///< firewire-cdev fd for the local node

    const char *path; ///< device path (e.g. "/dev/fw0"); outlives the device

    /// Dedicated fd owning the iso transmit context, or -1.  Polled alongside
    /// fd so the iso context can be recreated (close/open) on each mode switch
    /// without disturbing the control fd's address regions / Config ROM.
    int iso_fd;

    /// Node information from the most recent GET_INFO / bus reset.
    uint16_t node_id;
    uint16_t local_node_id;
    uint32_t generation;

    /// Configuration ROM identity (published by fw_device_publish).
    const char *vendor;
    const char *model_name;
    uint32_t vendor_id;       ///< key 0x03 payload (24-bit); anchors the vendor leaf
    uint32_t model_id;        ///< key 0x17 payload (24-bit)
    uint32_t unit_spec_id;    ///< key 0x12 payload (24-bit)
    uint32_t unit_sw_version; ///< key 0x13 payload (24-bit)
    int have_csr_base;        ///< publish a unit-dependent directory
    uint32_t csr_base;        ///< key 0x40 payload source
    fw_rom_mode_t rom_mode;   ///< identity style published by fw_device_publish

    /// Registered address regions.
    fw_region_t *regions;

    /// Cached fd to the remote node, for sending AV/C FCP responses.
    int remote_fd;
    uint16_t remote_node_id;

    /// Isochronous transmit completion callback.
    fw_iso_interrupt_handler_t on_iso_interrupt;
    void *iso_ctx;

    /// Isochronous fd error callback (POLLERR/POLLHUP on the iso fd).
    fw_iso_error_handler_t on_iso_error;
    void *iso_err_ctx;
};

/// IEEE 1212 / 1394 Configuration ROM directory-entry keys.  Copied from the
/// kernel's include/linux/firewire.h, which is not part of the userspace uapi
/// (firewire-cdev.h / firewire-constants.h expose only the cdev ABI and the
/// transaction-layer TCODE_/RCODE_ constants).
///
/// A directory entry is one quadlet: the high 8 bits are the key — a 2-bit
/// key_type (the CSR_OFFSET/CSR_LEAF/CSR_DIRECTORY bits) OR-ed with a 6-bit
/// key_value — and the low 24 bits are the value.
constexpr int CSR_OFFSET = 0x40;    ///< key_type: value is a CSR-space offset
constexpr int CSR_LEAF = 0x80;      ///< key_type: value points to a leaf
constexpr int CSR_DIRECTORY = 0xc0; ///< key_type: value points to a directory

constexpr int CSR_DESCRIPTOR = 0x01; ///< textual leaf -> CSR_LEAF | CSR_DESCRIPTOR (0x81)
constexpr int CSR_VENDOR = 0x03;     ///< Module/Unit Vendor_Id
constexpr int CSR_HARDWARE_VERSION = 0x04;
constexpr int CSR_UNIT = 0x11;           ///< unit dir -> CSR_DIRECTORY | CSR_UNIT (0xd1)
constexpr int CSR_SPECIFIER_ID = 0x12;   ///< unit_spec_id
constexpr int CSR_VERSION = 0x13;        ///< unit_sw_version
constexpr int CSR_DEPENDENT_INFO = 0x14; ///< udep dir -> CSR_DIRECTORY | CSR_DEPENDENT_INFO (0xd4)
constexpr int CSR_MODEL = 0x17;          ///< Model_Id
constexpr int CSR_DIRECTORY_ID = 0x20;

/// Builds a directory entry quadlet from an 8-bit key and a 24-bit value.
#define ROM_ENTRY(key, value) (((uint32_t) (key) << 24) | ((uint32_t) (value) & 0x00FFFFFFU))

/// Config ROM block CRCs are deliberately left zero everywhere in this file.
/// firewire-core walks the whole leaf area block-by-block when it publishes the
/// ROM and OR-s the correct IEEE 1212 CRC-16 into each block header — so any
/// value pre-computed here would be OR-ed with the kernel's and corrupt the
/// block.  Windows XP tolerates bad ROM CRCs; the Windows 8+ stack rejects the
/// block and synthesises a junk device ID, so the headers must be left for the
/// kernel.

/// Packs an ASCII string into big-endian quadlets (zero padded).
///
/// @param s    NUL-terminated string to pack.
/// @param out  Output quadlet array.
/// @param cap  Maximum quadlets to write.
/// @return     Number of quadlets written.
static size_t pack_ascii(const char *s, uint32_t *out, size_t cap) {
    size_t n = strlen(s);
    size_t quads = (n + 3) / 4;
    size_t q;
    if (quads > cap) {
        quads = cap;
    }
    for (q = 0; q < quads; q++) {
        uint32_t v = 0;
        int b;
        for (b = 0; b < 4; b++) {
            size_t idx = q * 4 + (size_t) b;
            uint8_t c = idx < n ? (uint8_t) s[idx] : 0;
            v = (v << 8) | c;
        }
        out[q] = v;
    }
    return quads;
}

/// Builds a minimal-ASCII textual-leaf block into out[]:
///   [0] header: (body_len << 16) | CRC (CRC left 0 for the kernel to fill)
///   [1] specifier_id=0, width=0
///   [2] char_set=0 (ASCII), lang=0
///   [3..] packed ASCII text
///
/// @param s    Text to encode.
/// @param out  Output buffer; must hold at least 16 quadlets.
/// @return     Total quadlets written.
static size_t build_text_leaf(const char *s, uint32_t *out) {
    size_t text_quads = pack_ascii(s, out + 3, 13); // out[3..15]
    size_t body_len = 2 + text_quads;               // specifier + charset + text
    out[1] = 0;
    out[2] = 0;
    out[0] = (uint32_t) body_len << 16;
    return 1 + body_len;
}

/// Computes the unit-dependent directory key-0x40 entry encoding csr_base.
/// Hosts recover CsrBase = 0xF0000000 + (V << 2) where V is the 24-bit value.
///
/// @param csr_base  Low 32 bits of the DCAM CSR region base address.
/// @return          The ROM_ENTRY quadlet for the key-0x40 CsrBase field.
static uint32_t dcam_udep_entry(uint32_t csr_base) {
    return ((uint32_t) CSR_OFFSET << 24) | ((csr_base >> 2) & 0x03FFFFFFU);
}

/// Builds the unit directory (block A), its textual leaves, and — when a CSR
/// base is set — a unit-dependent directory (a single key-0x40 entry).
///
/// In FW_ROM_MODE_NAME the unit carries a Vendor_Id (0x03) + vendor textual leaf
/// and a Model_Id (0x17) + model textual leaf, so Windows builds the textual
/// hardware ID 1394\<vendor>&<model> (e.g. 1394\Vitana&PixeLINK(tm)_-_Photonic).
/// Windows reads the model name from this unit directory but the vendor name from
/// the ROOT Module_Vendor_Id leaf (which firewire-core owns and fills with
/// "Linux Firewire"), so fw_device_publish also injects vendor and model leaves
/// into the root (see inject_root_named_entry); these unit-directory copies stay
/// for stacks that consult the unit's own Vendor_Id.  In FW_ROM_MODE_IDS the
/// Vendor_Id/Model_Id and leaves are omitted — exactly like the kernel's
/// IP-over-1394 units — so Windows identifies the unit purely by
/// 1394\<unit_spec_id>&<unit_sw_version> (e.g. 1394\A02D&100).
///
/// A descriptor leaf (key 0x81) directly follows the entry it describes — the
/// standard IEEE 1212 exception to ascending key order.  All block CRCs are left
/// 0 for firewire-core to fill in.
///
/// @param dev  Device whose identity and CSR base to encode.
/// @param out  Output buffer; must hold at least 32 quadlets.
/// @return     Total quadlets written.
static size_t build_unit_dir(const fw_device_t *dev, uint32_t *out) {
    int named = (dev->rom_mode == FW_ROM_MODE_NAME);

    uint32_t vendor_leaf[16];
    uint32_t model_leaf[16];
    size_t vendor_quads = named ? build_text_leaf(dev->vendor, vendor_leaf) : 0;
    size_t model_quads = named ? build_text_leaf(dev->model_name, model_leaf) : 0;

    // entries after the header:
    // [vendor_id, vendor ptr], spec_id, sw_version, [model_id, model ptr], [udep ptr]
    size_t num_a = 2 + (named ? 4 : 0) + (dev->have_csr_base ? 1 : 0);
    size_t block_vendor = 1 + num_a;                  // vendor textual leaf
    size_t block_model = block_vendor + vendor_quads; // model textual leaf
    size_t block_udep = block_model + model_quads;    // unit-dependent directory
    size_t i, k, n;

    // ---- Block A: unit directory (kernel fills its CRC) ----
    out[0] = (uint32_t) num_a << 16; // header: length=num_a, CRC=0
    k = 1;
    if (named) {
        out[k] = ROM_ENTRY(CSR_VENDOR, dev->vendor_id); // vendor_id
        k++;
        out[k] = ROM_ENTRY(CSR_LEAF | CSR_DESCRIPTOR, block_vendor - k); // vendor textual leaf ptr
        k++;
    }
    out[k++] = ROM_ENTRY(CSR_SPECIFIER_ID, dev->unit_spec_id); // unit_spec_id
    out[k++] = ROM_ENTRY(CSR_VERSION, dev->unit_sw_version);   // unit_sw_version
    if (named) {
        out[k] = ROM_ENTRY(CSR_MODEL, dev->model_id); // model_id
        k++;
        out[k] = ROM_ENTRY(CSR_LEAF | CSR_DESCRIPTOR, block_model - k); // model textual leaf ptr
        k++;
    }
    if (dev->have_csr_base) {
        out[k] = ROM_ENTRY(CSR_DIRECTORY | CSR_DEPENDENT_INFO, block_udep - k); // udep dir ptr
        k++;
    }

    // ---- Textual leaves (named mode only) ----
    for (i = 0; i < vendor_quads; i++) {
        out[block_vendor + i] = vendor_leaf[i];
    }
    for (i = 0; i < model_quads; i++) {
        out[block_model + i] = model_leaf[i];
    }
    n = block_udep;

    // ---- Unit-dependent directory ----
    if (dev->have_csr_base) {
        uint32_t entry = dcam_udep_entry(dev->csr_base);
        out[block_udep] = (1u << 16); // header: length=1, CRC filled by kernel
        out[block_udep + 1] = entry;  // key 0x40: CsrBase
        n = block_udep + 2;
    }
    return n;
}

fw_device_t *create_device(const char *path, const char *vendor, uint32_t vendor_id, const char *name,
                           uint32_t model_id, uint32_t unit_spec_id, uint32_t unit_sw_version) {
    fw_device_t *dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return NULL;
    }

    // Non-blocking: the run loop reads only after poll() reports readiness,
    // and a read on stale readiness must fail with EAGAIN rather than hang
    // the event loop (and with it every pending bus request).
    dev->fd = open(path, O_RDWR | O_NONBLOCK);
    if (dev->fd < 0) {
        LOG(ERROR, "open %s failed: %s", path, strerror(errno));
        free(dev);
        return NULL;
    }

    dev->path = path;
    dev->iso_fd = -1;
    dev->vendor = vendor;
    dev->model_name = name;
    dev->vendor_id = vendor_id;
    dev->model_id = model_id;
    dev->unit_spec_id = unit_spec_id;
    dev->unit_sw_version = unit_sw_version;
    dev->remote_fd = -1;
    dev->remote_node_id = 0xFFFF;

    // Get node info / start the bus-reset event stream.
    struct fw_cdev_event_bus_reset br = {0};
    struct fw_cdev_get_info info = {
        .version = 4,
        .bus_reset = (uintptr_t) &br,
        .bus_reset_closure = 0xdeadbeef,
    };
    if (ioctl(dev->fd, FW_CDEV_IOC_GET_INFO, &info) < 0) {
        LOG(ERROR, "GET_INFO failed: %s", strerror(errno));
        close(dev->fd);
        free(dev);
        return NULL;
    }
    dev->node_id = br.node_id;
    dev->local_node_id = br.local_node_id;
    dev->generation = br.generation;
    LOG(INFO, "opened %s  card=%u  node_id=0x%04x  local_node_id=0x%04x  abi=%u", path, info.card, br.node_id,
        br.local_node_id, info.version);
    return dev;
}

void fw_device_destroy(fw_device_t *dev) {
    if (dev == NULL) {
        return;
    }
    fw_region_t *r = dev->regions;
    while (r != NULL) {
        fw_region_t *next = r->next;
        free(r);
        r = next;
    }
    if (dev->remote_fd >= 0) {
        close(dev->remote_fd);
    }
    if (dev->fd >= 0) {
        close(dev->fd);
    }
    free(dev);
}

int fw_device_fd(const fw_device_t *dev) {
    return dev->fd;
}

const char *fw_device_path(const fw_device_t *dev) {
    return dev->path;
}

void fw_device_set_iso_fd(fw_device_t *dev, int iso_fd) {
    dev->iso_fd = iso_fd;
}

void fw_device_set_csr_base(fw_device_t *dev, uint32_t csr_base) {
    dev->have_csr_base = 1;
    dev->csr_base = csr_base;
}

void fw_device_set_rom_mode(fw_device_t *dev, fw_rom_mode_t mode) {
    dev->rom_mode = mode;
}

void fw_device_set_iso_interrupt_handler(fw_device_t *dev, fw_iso_interrupt_handler_t handler, void *ctx) {
    dev->on_iso_interrupt = handler;
    dev->iso_ctx = ctx;
}

void fw_device_set_iso_error_handler(fw_device_t *dev, fw_iso_error_handler_t handler, void *ctx) {
    dev->on_iso_error = handler;
    dev->iso_err_ctx = ctx;
}

uint64_t fw_device_allocate(fw_device_t *dev, uint64_t offset, uint64_t region_end, uint32_t length,
                            fw_request_handler_t on_read, fw_request_handler_t on_write, void *ctx) {
    struct fw_cdev_allocate alloc = {
        .offset = offset,
        .closure = (uintptr_t) ctx,
        .length = length,
        .region_end = region_end,
    };
    if (ioctl(dev->fd, FW_CDEV_IOC_ALLOCATE, &alloc) < 0) {
        LOG(ERROR, "ALLOCATE failed: %s", strerror(errno));
        return 0;
    }

    fw_region_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return 0;
    }
    r->base = alloc.offset;
    r->length = length;
    r->on_read = on_read;
    r->on_write = on_write;
    r->ctx = ctx;
    r->next = dev->regions;
    dev->regions = r;
    return alloc.offset;
}

/// Finds the registered region containing `offset`, or NULL.
///
/// @param dev     Device whose region list to search.
/// @param offset  48-bit address to look up.
/// @return        The matching region, or NULL if none covers the address.
static fw_region_t *find_region(fw_device_t *dev, uint64_t offset) {
    fw_region_t *r;
    for (r = dev->regions; r != NULL; r = r->next) {
        if (offset >= r->base && offset < r->base + r->length) {
            return r;
        }
    }
    return NULL;
}

/// Injects an immediate identity key (Vendor_Id / Model_Id) plus its textual
/// leaf into the ROOT directory of the local node's config ROM.  Windows derives
/// the displayed vendor name from the root Module_Vendor_Id leaf (not the
/// unit's), so the vendor must be published at root scope to override
/// firewire-core's own "Linux Firewire" Module_Vendor_Id.  The leaf is copied
/// by the kernel at add time, so the caller's stack buffer need not outlive the
/// call.
///
/// @param fd        Local firewire-cdev fd.
/// @param immediate Immediate key quadlet (e.g. ROM_ENTRY(CSR_VENDOR, id)).
/// @param text      Textual leaf string.
/// @param what      Name used in log messages (e.g. "vendor").
static void inject_root_named_entry(int fd, uint32_t immediate, const char *text, const char *what) {
    uint32_t leaf[16];
    size_t leaf_quads = build_text_leaf(text, leaf);
    struct fw_cdev_add_descriptor desc = {
        .immediate = immediate,
        .key = (uint32_t) (CSR_LEAF | CSR_DESCRIPTOR) << 24, // textual leaf ptr
        .data = (uintptr_t) leaf,
        .length = (uint32_t) leaf_quads,
    };
    if (ioctl(fd, FW_CDEV_IOC_ADD_DESCRIPTOR, &desc) < 0) {
        LOG(ERROR, "ADD_DESCRIPTOR (root %s) failed: %s", what, strerror(errno));
        return;
    }
    LOG(INFO, "root %s leaf injected (handle=%u)  %s=\"%s\"", what, desc.handle, what, text);
}

int fw_device_publish(fw_device_t *dev) {
    // The unit directory carries spec/version and, in FW_ROM_MODE_NAME, a
    // Vendor_Id + vendor leaf and a Model_Id + model leaf; it doubles as the
    // local-node probe descriptor.  Windows reads the model name from the unit
    // directory but the vendor name from the ROOT Module_Vendor_Id leaf, so in
    // name mode vendor and model are also published at root scope below. The
    // unit-directory copies stay put.
    uint32_t unit_dir[32];
    size_t unit_quads = build_unit_dir(dev, unit_dir);
    struct fw_cdev_add_descriptor unit_desc = {
        .immediate = 0,
        .key = (uint32_t) (CSR_DIRECTORY | CSR_UNIT) << 24, // unit directory pointer
        .data = (uintptr_t) unit_dir,
        .length = (uint32_t) unit_quads,
    };

    // FW_CDEV_IOC_ADD_DESCRIPTOR only succeeds on the local node, so the
    // device path given on the command line must refer to it.
    int local_fd = dev->fd;
    if (ioctl(local_fd, FW_CDEV_IOC_ADD_DESCRIPTOR, &unit_desc) < 0) {
        LOG(ERROR, "ADD_DESCRIPTOR on %s (length=%u quads) failed: %s", dev->path, unit_desc.length, strerror(errno));
        LOG(ERROR, "Config ROM will not be injected; remote host will not detect the device.");
        return -1;
    }

    // Root-directory Vendor_Id/Model_Id + textual leaves (name mode only).
    if (dev->rom_mode == FW_ROM_MODE_NAME) {
        inject_root_named_entry(local_fd, ROM_ENTRY(CSR_VENDOR, dev->vendor_id), dev->vendor, "vendor");
        inject_root_named_entry(local_fd, ROM_ENTRY(CSR_MODEL, dev->model_id), dev->model_name, "model");
    }

    LOG(INFO, "unit directory injected (handle=%u)  mode=%s", unit_desc.handle,
        dev->rom_mode == FW_ROM_MODE_NAME ? "name (1394\\Vendor&Model)" : "ids (1394\\SpecId&SwVersion)");
    LOG(INFO, "vendor=\"%s\" (0x%06x)  model=\"%s\" (0x%06x)", dev->vendor, dev->vendor_id, dev->model_name,
        dev->model_id);
    LOG(INFO, "unit_spec_id=0x%06x  unit_sw_version=0x%06x", dev->unit_spec_id, dev->unit_sw_version);
    if (dev->have_csr_base) {
        LOG(INFO, "unit-dependent dir: key 0x40 entry=0x%08x  (CsrBase=0x%08x)", dcam_udep_entry(dev->csr_base),
            dev->csr_base);
    }
    LOG(INFO, "A bus reset was triggered; remote host will re-read ROM.");
    return 0;
}

void fw_device_send_response(fw_device_t *dev, const struct fw_cdev_event_request2 *req, int rcode, const void *data,
                             uint32_t len) {
    struct fw_cdev_send_response resp = {
        .rcode = (uint32_t) rcode,
        .length = len,
        .data = (uintptr_t) data,
        .handle = req->handle,
    };
    if (ioctl(dev->fd, FW_CDEV_IOC_SEND_RESPONSE, &resp) < 0) {
        LOG(ERROR, "SEND_RESPONSE failed: %s", strerror(errno));
    }
}

/// Opens the /dev/fw* whose current node_id matches target_node_id.
///
/// @param target_node_id  Node ID to match.
/// @return                Open fd for the matching device, or -1 if not found.
static int open_fw_dev_for_node(uint16_t target_node_id) {
    char path[16];
    int i;
    for (i = 0; i < 16; i++) {
        struct fw_cdev_event_bus_reset br = {0};
        struct fw_cdev_get_info info = {.version = 4, .bus_reset = (uintptr_t) &br};
        snprintf(path, sizeof(path), "/dev/fw%d", i);
        int tryfd = open(path, O_RDWR);
        if (tryfd < 0) {
            continue;
        }
        if (ioctl(tryfd, FW_CDEV_IOC_GET_INFO, &info) == 0 && (uint16_t) br.node_id == target_node_id) {
            LOG(INFO, "remote node 0x%04x -> %s", target_node_id, path);
            return tryfd;
        }
        close(tryfd);
    }
    return -1;
}

/// Closes and invalidates the cached remote node fd.
///
/// @param dev  Device whose cached remote fd to invalidate.
static void invalidate_remote_fd(fw_device_t *dev) {
    if (dev->remote_fd >= 0) {
        close(dev->remote_fd);
        dev->remote_fd = -1;
        dev->remote_node_id = 0xFFFF;
    }
}

void fw_device_send_fcp_response(fw_device_t *dev, uint16_t dest_node_id, uint32_t generation, const uint8_t *resp,
                                 uint32_t len) {
    if (dev->remote_fd < 0 || dev->remote_node_id != dest_node_id) {
        invalidate_remote_fd(dev);
        dev->remote_fd = open_fw_dev_for_node(dest_node_id);
        if (dev->remote_fd < 0) {
            LOG(ERROR, "[FCP resp] cannot find /dev/fw* for node 0x%04x", dest_node_id);
            return;
        }
        dev->remote_node_id = dest_node_id;
    }

    struct fw_cdev_send_request req = {
        .tcode = TCODE_WRITE_BLOCK_REQUEST,
        .length = len,
        .offset = FCP_RESPONSE_ADDR,
        .closure = 0,
        .data = (uintptr_t) resp,
        .generation = generation,
    };
    if (ioctl(dev->remote_fd, FW_CDEV_IOC_SEND_REQUEST, &req) < 0) {
        LOG(ERROR, "[FCP resp] SEND_REQUEST failed: %s", strerror(errno));
        invalidate_remote_fd(dev);
    }
}

/// Returns non-zero if tcode is a read transaction code.
///
/// @param tcode  1394 transaction code.
/// @return       Non-zero for read quadlet or read block requests, zero otherwise.
static int tcode_is_read(uint32_t tcode) {
    return tcode == TCODE_READ_QUADLET_REQUEST || tcode == TCODE_READ_BLOCK_REQUEST;
}

/// Returns non-zero if tcode is a write transaction code.
///
/// @param tcode  1394 transaction code.
/// @return       Non-zero for write quadlet or write block requests, zero otherwise.
static int tcode_is_write(uint32_t tcode) {
    return tcode == TCODE_WRITE_QUADLET_REQUEST || tcode == TCODE_WRITE_BLOCK_REQUEST;
}

/// Dispatches a request to a matching region handler, or answers generically.
///
/// @param dev  Device the request arrived on.
/// @param req  The incoming request event.
static void dispatch_request(fw_device_t *dev, const struct fw_cdev_event_request2 *req) {
    LOG(TRACE, "[request ] tcode=0x%x  offset=0x%012llx  len=%u  src=0x%04x", req->tcode,
        (unsigned long long) req->offset, req->length, req->source_node_id);

    fw_region_t *r = find_region(dev, req->offset);
    if (r != NULL) {
        if (tcode_is_read(req->tcode) && r->on_read != NULL) {
            r->on_read(dev, req, r->ctx);
            return;
        }
        if (tcode_is_write(req->tcode) && r->on_write != NULL) {
            r->on_write(dev, req, r->ctx);
            return;
        }
    }

    // No handler: respond COMPLETE (reads echo zeros of the requested size).
    uint8_t zeros[512] = {0};
    uint32_t resp_len = tcode_is_read(req->tcode) ? req->length : 0;
    if (resp_len > sizeof(zeros)) {
        resp_len = sizeof(zeros);
    }
    fw_device_send_response(dev, req, RCODE_COMPLETE, zeros, resp_len);
}

/// Reads and handles one event from `evfd` (either the control fd or the
/// dedicated iso fd).
///
/// @param dev    Device to dispatch events for.
/// @param evfd   File descriptor to read from.
/// @return       0 to continue the loop, -1 on fatal read error.
static int process_one_event(fw_device_t *dev, int evfd) {
    // A block-write request delivers its payload inline, immediately after the
    // fixed fw_cdev_event_request2 header.  union fw_cdev_event is sized to the
    // headers alone and leaves only 8 payload bytes past the request2 header,
    // so reading into a bare union truncates any longer write: a 12-byte
    // mailbox register write (0x1009) loses its final quadlet — the register
    // value — while an 8-byte read command still fits.  Back the read with a
    // buffer large enough for the header plus a full S400 async block payload
    // and alias the union over it.
    union {
        union fw_cdev_event ev;
        uint8_t bytes[sizeof(union fw_cdev_event) + FW_MAX_ASYNC_PAYLOAD];
    } buf;
    union fw_cdev_event *evp = &buf.ev;
    ssize_t len = read(evfd, &buf, sizeof(buf));
    if (len < 0) {
        if (errno == EINTR) {
            return 0;
        }
        if (errno == EAGAIN) {
            // The fds are non-blocking: readiness observed by poll() can be
            // stale by the time of this read (for example when a register
            // handler closed and reopened the iso fd under the same number),
            // and that must not hang the event loop.
            return 0;
        }
        LOG(ERROR, "read event failed: %s", strerror(errno));
        return -1;
    }

    switch (evp->common.type) {
        case FW_CDEV_EVENT_BUS_RESET:
            LOG(INFO, "[bus reset] generation=%u  node_id=0x%04x  root=0x%04x", evp->bus_reset.generation,
                evp->bus_reset.node_id, evp->bus_reset.root_node_id);
            dev->node_id = evp->bus_reset.node_id;
            dev->local_node_id = evp->bus_reset.local_node_id;
            dev->generation = evp->bus_reset.generation;
            // Node IDs may have changed; re-resolve the remote fd next time.
            invalidate_remote_fd(dev);
            break;

        case FW_CDEV_EVENT_REQUEST2:
            dispatch_request(dev, &evp->request2);
            break;

        case FW_CDEV_EVENT_REQUEST:
            // Older kernel; shouldn't happen with ABI v4 but handle gracefully.
            LOG(TRACE, "[request ] (v1 event, tcode=0x%x  offset=0x%012llx)", evp->request.tcode,
                (unsigned long long) evp->request.offset);
            {
                struct fw_cdev_send_response resp = {
                    .rcode = RCODE_COMPLETE,
                    .length = 0,
                    .data = 0,
                    .handle = evp->request.handle,
                };
                ioctl(dev->fd, FW_CDEV_IOC_SEND_RESPONSE, &resp);
            }
            break;

        case FW_CDEV_EVENT_RESPONSE:
            LOG(TRACE, "[response] rcode=0x%x  len=%u", evp->response.rcode, evp->response.length);
            break;

        case FW_CDEV_EVENT_ISO_INTERRUPT:
            if (dev->on_iso_interrupt != NULL) {
                // For a transmit context the kernel buffers one 4-byte
                // timestamp per completed packet and reports the batch length in
                // header_length, so header_length / 4 is the number of packets
                // transmitted since the previous interrupt. (dequeue_event
                // truncates the header payload to this read buffer but the
                // header_length field itself is always delivered.)
                dev->on_iso_interrupt(dev, dev->iso_ctx, evp->iso_interrupt.header_length / 4);
            }
            break;

        default:
            LOG(DEBUG, "[event   ] type=0x%x", evp->common.type);
            break;
    }
    return 0;
}

void fw_device_run(fw_device_t *dev) {
    LOG(INFO, "device is live on the bus.");
    LOG(INFO, "Press Ctrl+C to remove the device and exit.");

    for (;;) {
        struct pollfd pfds[2];
        nfds_t nfds = 1;
        pfds[0].fd = dev->fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        if (dev->iso_fd >= 0) {
            pfds[1].fd = dev->iso_fd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            nfds = 2;
        }

        int r = poll(pfds, nfds, -1);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR, "poll failed: %s", strerror(errno));
            return;
        }

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            // The control fd is dead (device shutdown or card removal). The
            // condition is level-triggered and poll() would report it again
            // on every call, so exit instead of spinning.
            LOG(ERROR, "control fd error condition (revents=0x%x); exiting run loop", pfds[0].revents);
            return;
        }
        if (pfds[0].revents & POLLIN) {
            if (process_one_event(dev, dev->fd) < 0) {
                return;
            }
        }
        // The control event above may have closed the iso fd (set to -1), or
        // closed and reopened it, in which case the fresh fd can reuse the
        // old number while pfds[1].revents still describes the old context.
        // The fd-number check below cannot tell those apart, so the
        // non-blocking read in process_one_event is what turns leftover
        // stale readiness into a harmless EAGAIN.
        if (nfds == 2 && dev->iso_fd == pfds[1].fd && pfds[1].revents != 0) {
            LOG(TRACE, "[iso] poll revents=0x%x on iso fd (POLLIN=%d POLLERR=%d POLLHUP=%d)", pfds[1].revents,
                (pfds[1].revents & POLLIN) != 0, (pfds[1].revents & POLLERR) != 0, (pfds[1].revents & POLLHUP) != 0);
            if ((pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                // Level-triggered error: poll() reports it again on every
                // call until the fd is closed, so logging and moving on
                // would spin the loop. Hand it to the error handler, which
                // tears the iso context down; without one, at least stop
                // polling the fd.
                LOG(WARN, "[iso] iso fd error condition (revents=0x%x); stopping the iso context", pfds[1].revents);
                if (dev->on_iso_error != NULL) {
                    dev->on_iso_error(dev, dev->iso_err_ctx);
                } else {
                    dev->iso_fd = -1;
                }
            } else if (pfds[1].revents & POLLIN) {
                if (process_one_event(dev, dev->iso_fd) < 0) {
                    return;
                }
            }
        }
    }
}
