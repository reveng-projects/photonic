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
/// Isochronous video transmit over the Linux firewire-cdev API: geometry
/// selection for the current mode, the DMA frame ring, packet queueing and the
/// completion-driven refill loop.

#include "iso.h"

#include "dcam_internal.h"
#include "format7.h"
#include "frame.h"
#include "log.h"
#include "trigger.h"
#include "video.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/// Queue one frame (cam->iso_ppf packets) for transmission from ring `slot`.
///
/// Interrupts are flagged at ISO_INTR_STRIDE boundaries and on the frame's last
/// packet so the kernel flushes its per-packet timestamp buffer before it
/// overflows (otherwise frames with >1024 packets deliver no completion event at
/// all). The packets are submitted in chunks of at most ISO_MAX_PKTS_PER_QUEUE;
/// the sy and interrupt flags are placed by absolute packet index so they land
/// on the correct packets regardless of where the chunk boundaries fall.
///
/// @param cam   Camera whose iso context and ring to queue into.
/// @param slot  Ring-buffer slot index to transmit.
static void iso_queue_frame(dcam_camera_t *cam, unsigned slot) {
    uint32_t controls[ISO_MAX_PKTS_PER_QUEUE];
    int fd = cam->iso_fd;
    uint8_t *frame_base = cam->iso_buffer + (size_t) slot * cam->frame_bytes;
    unsigned queued = 0;

    while (queued < cam->iso_ppf) {
        unsigned chunk = cam->iso_ppf - queued;
        unsigned i;

        if (chunk > ISO_MAX_PKTS_PER_QUEUE) {
            chunk = ISO_MAX_PKTS_PER_QUEUE;
        }
        for (i = 0; i < chunk; i++) {
            unsigned pkt = queued + i;
            uint32_t sy = (cam->iso_sy_all || pkt == 0) ? 1 : 0; // sy=1 = start-of-frame
            uint32_t control =
                FW_CDEV_ISO_PAYLOAD_LENGTH(cam->iso_bpp) | FW_CDEV_ISO_TAG(cam->iso_tag) | FW_CDEV_ISO_SY(sy);
            // Interrupt on the last packet of the frame and at least every
            // ISO_INTR_STRIDE packets, so the kernel flushes its per-packet
            // timestamp buffer before it overflows (see ISO_INTR_STRIDE). The
            // completion handler counts packets across these interrupts and
            // refills a whole frame per ppf packets acknowledged.
            if (pkt == cam->iso_ppf - 1 || (pkt + 1) % ISO_INTR_STRIDE == 0) {
                control |= FW_CDEV_ISO_INTERRUPT;
            }
            controls[i] = control;
        }

        // header_size is 0, so each packet descriptor is one quadlet (the
        // control word) and the OHCI builds the iso packet header from the
        // tag/sy/length fields. The kernel may accept only part of a submit, so
        // advance past what it took and resubmit the remainder.
        struct fw_cdev_queue_iso q = {
            .packets = (uintptr_t) controls,
            .data = (uintptr_t) (frame_base + (size_t) queued * cam->iso_bpp),
            .size = chunk * (uint32_t) sizeof(controls[0]),
            .handle = cam->iso_handle,
        };
        unsigned done = 0; // packets of this chunk the kernel has taken
        for (;;) {
            uint32_t before = q.size;
            if (ioctl(fd, FW_CDEV_IOC_QUEUE_ISO, &q) < 0) {
                LOG(ERROR, "QUEUE_ISO failed: %s", strerror(errno));
                return;
            }
            if (q.size == before) {
                break; // no progress (context buffer full)
            }
            done += (before - q.size) / (uint32_t) sizeof(controls[0]);
            if (q.size == 0) {
                break; // whole chunk queued
            }
            // Advance past the packets the kernel accepted and resubmit.
            q.packets = (uintptr_t) (controls + done);
            q.data = (uintptr_t) (frame_base + (size_t) (queued + done) * cam->iso_bpp);
        }
        if (q.size != 0) {
            LOG(WARN, "[iso] QUEUE_ISO stalled with %u of %u packets left (slot %u)", q.size / 4, cam->iso_ppf, slot);
            return;
        }
        queued += chunk;
    }
    LOG(TRACE, "[iso] queued frame slot %u (%u pkts in <=%u-pkt submits)", slot, cam->iso_ppf, ISO_MAX_PKTS_PER_QUEUE);
}

/// Compute iso geometry for a Format 7 stream from the selected mode's programmed
/// CSR state.  The F7 mode is latched into cam->cur_mode by the VIDEO_MODE write.
/// The frame is width*height*bits_per_pixel/8 bytes, split into bytes_per_packet
/// chunks, giving floor(frameBytes/bpp) packets per frame.
///
/// @param cam  Camera whose cur_mode and f7[] state to use.
/// @return     1 if the geometry is valid and streamable, 0 otherwise.
static int iso_compute_geometry_f7(dcam_camera_t *cam) {
    int mode = (int) cam->cur_mode;
    const struct dcam_f7_live *live;
    uint32_t bits;
    unsigned image_bytes, bpp, ppf;

    if (mode < 0 || mode >= DCAM_NUM_F7_MODES) {
        LOG(WARN, "[iso] F7 mode %d out of range", mode);
        return 0;
    }
    live = &cam->f7[mode];
    bits = dcam_f7_bits_per_pixel(live->color_coding);
    if (bits == 0) {
        LOG(WARN, "[iso] F7 mode %d colour coding %u has no bits-per-pixel", mode, live->color_coding);
        return 0;
    }
    bpp = live->bytes_per_packet;
    if (bpp == 0 || live->width == 0 || live->height == 0) {
        LOG(WARN, "[iso] F7 mode %d geometry not programmed (w=%u h=%u bpp=%u)", mode, live->width, live->height, bpp);
        return 0;
    }
    image_bytes = (unsigned) (((uint64_t) live->width * live->height * bits) >> 3);
    ppf = image_bytes / bpp;
    if (ppf == 0 || ppf > ISO_MAX_PACKETS_PER_FRAME) {
        LOG(WARN, "[iso] F7 mode %d %ux%u bpp=%u gives ppf=%u (unstreamable)", mode, live->width, live->height, bpp,
            ppf);
        return 0;
    }

    cam->iso_bpp = bpp;
    cam->iso_ppf = ppf;
    // Size each ring slot to a full rendered image; the leading bpp*ppf bytes
    // are transmitted, which never exceeds the slot.
    cam->frame_bytes = image_bytes;
    switch (live->color_coding) {
        case DCAM_F7_CODING_YUV422:
            cam->pixfmt = DCAM_PIX_YUV422;
            break;
        case DCAM_F7_CODING_MONO16:
            cam->pixfmt = DCAM_PIX_MONO16;
            break;
        case DCAM_F7_CODING_RAW8:
            cam->pixfmt = DCAM_PIX_RAW8;
            break;
        case DCAM_F7_CODING_MONO8: // fall through
        default:
            cam->pixfmt = DCAM_PIX_MONO8;
            break;
    }
    cam->img_width = live->width;
    cam->img_height = live->height;
    return 1;
}

/// Compute geometry for the selected (format,mode,rate).
///
/// @param cam  Camera whose cur_format/cur_mode/cur_rate to use.
/// @return     1 if the geometry is valid and streamable, 0 otherwise.
static int iso_compute_geometry(dcam_camera_t *cam) {
    const dcam_mode_info_t *mi;
    unsigned bpp, ppf;

    if (cam->cur_format == DCAM_F7_FORMAT) {
        return iso_compute_geometry_f7(cam);
    }

    if (cam->cur_format < 0 || cam->cur_format >= DCAM_NUM_FORMATS || cam->cur_mode < 0 ||
        cam->cur_mode >= DCAM_NUM_MODES || cam->cur_rate < 0 || cam->cur_rate >= DCAM_NUM_RATES) {
        LOG(WARN, "[iso] unsupported format/mode/rate %d/%d/%d", cam->cur_format, cam->cur_mode, cam->cur_rate);
        return 0;
    }

    mi = dcam_mode_info_get(cam->cur_format, cam->cur_mode);
    bpp = mi->rate[cam->cur_rate].bytes_per_packet;
    ppf = mi->rate[cam->cur_rate].packets_per_frame;
    if (bpp == 0 || ppf == 0 || ppf > ISO_MAX_PACKETS_PER_FRAME) {
        LOG(WARN, "[iso] format/mode/rate %d/%d/%d has no valid geometry (bpp=%u ppf=%u)", cam->cur_format,
            cam->cur_mode, cam->cur_rate, bpp, ppf);
        return 0;
    }

    cam->iso_bpp = bpp;
    cam->iso_ppf = ppf;
    cam->frame_bytes = bpp * ppf;
    cam->pixfmt = mi->pixfmt;
    cam->img_width = mi->width;
    cam->img_height = mi->height;
    return 1;
}

/// Map the iso DMA buffer for the current iso fd.  Tries ISO_BUFFER_MAX_BYTES
/// and halves down to ISO_BUFFER_MIN_BYTES on ENOMEM.  The buffer is bound to
/// the iso context on first mmap and freed when the iso fd is closed (on stop),
/// so it is remapped on every start.
///
/// @param cam  Camera whose iso_fd and iso_buffer to populate.
/// @return     1 on success, 0 on failure.
static int iso_map_buffer_once(dcam_camera_t *cam) {
    int fd = cam->iso_fd;
    size_t want;

    if (cam->iso_buffer != NULL) {
        return 1;
    }
    for (want = ISO_BUFFER_MAX_BYTES; want >= ISO_BUFFER_MIN_BYTES; want /= 2) {
        void *p = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            cam->iso_buffer = (uint8_t *) p;
            cam->iso_buffer_bytes = want;
            LOG(INFO, "iso DMA buffer mapped: %zu bytes", cam->iso_buffer_bytes);
            return 1;
        }
        if (errno != ENOMEM) {
            break; // EBUSY or other: halving will not help
        }
    }
    LOG(ERROR, "mmap (iso buffer) failed: %s", strerror(errno));
    return 0;
}

/// Pick how many frame slots of the current size fit in the mapped buffer.
///
/// @param cam  Camera whose iso_buffer_bytes and frame_bytes to use.
/// @return     1 on success, 0 if the frame is too large for the buffer.
static int iso_setup_ring(dcam_camera_t *cam) {
    unsigned ring = (unsigned) (cam->iso_buffer_bytes / cam->frame_bytes);

    if (ring == 0) {
        LOG(WARN,
            "[iso] frame is %u bytes but iso buffer is only %zu bytes — "
            "cannot stream this mode",
            cam->frame_bytes, cam->iso_buffer_bytes);
        return 0;
    }
    if (ring > ISO_RING_FRAMES_MAX) {
        ring = ISO_RING_FRAMES_MAX;
    }
    if (ring < 2) {
        LOG(WARN, "[iso] only %u frame buffer fits %u-byte frames; stream may underrun", ring, cam->frame_bytes);
    }
    cam->ring_frames = ring;
    return 1;
}

void dcam_iso_start(dcam_camera_t *cam) {
    unsigned slot;

    if (cam->iso_running) {
        return;
    }
    if (!iso_compute_geometry(cam)) {
        return;
    }

    // Open a dedicated fd and create a fresh iso context on every start.  A
    // fw-cdev transmit context cannot be reset or destroyed except by closing
    // its fd, and its DMA program keeps any queued-but-untransmitted packets
    // across STOP_ISO/START_ISO (the kernel's context_run resumes from
    // ctx->last, which still points at the leftover descriptors).  Reusing one
    // context would therefore replay the previous mode's look-ahead frames on
    // the next start with the old packet geometry, producing torn images for
    // the first frames after each mode switch.  A brand-new context per start
    // has an empty queue and also lets channel/speed be chosen afresh.
    if (!cam->iso_created) {
        // Non-blocking for the same reason as the control fd: the run loop
        // reads this fd on poll() readiness, and stale readiness after a
        // close/reopen must not hang it.
        cam->iso_fd = open(fw_device_path(cam->dev), O_RDWR | O_NONBLOCK);
        if (cam->iso_fd < 0) {
            LOG(ERROR, "open (iso fd) failed: %s", strerror(errno));
            return;
        }
        struct fw_cdev_create_iso_context cctx = {
            .type = FW_CDEV_ISO_CONTEXT_TRANSMIT,
            .header_size = 0,
            .channel = (uint32_t) cam->iso_channel,
            .speed = (uint32_t) cam->iso_speed,
            .closure = 0,
        };
        if (ioctl(cam->iso_fd, FW_CDEV_IOC_CREATE_ISO_CONTEXT, &cctx) < 0) {
            LOG(ERROR, "CREATE_ISO_CONTEXT (transmit) failed: %s", strerror(errno));
            close(cam->iso_fd);
            cam->iso_fd = -1;
            return;
        }
        cam->iso_handle = cctx.handle;
        cam->iso_created = 1;
        // Let the run loop poll this fd for iso completion interrupts.
        fw_device_set_iso_fd(cam->dev, cam->iso_fd);
        LOG(INFO, "iso transmit context created  channel=%d  speed=%d (SCODE)", cam->iso_channel, cam->iso_speed);
    }

    if (!iso_map_buffer_once(cam)) {
        return;
    }
    if (!iso_setup_ring(cam)) {
        return;
    }

    // A new stream restarts the frame index at 0 so the test can verify frames
    // arrive starting from 0; frames_transmitted is the diagnostic byte count.
    cam->frame_counter = 0;
    cam->frames_transmitted = 0;
    cam->iso_acked_packets = 0;

    // Queue the initial frame(s).  Continuous streaming (ISO_EN) prefills the
    // whole ring and refills on every completion; a one-shot (reg 0x61C) queues
    // exactly one frame and dcam_on_iso_interrupt stops after it transmits.
    unsigned prefill = cam->iso_oneshot ? 1u : cam->ring_frames;
    for (slot = 0; slot < prefill; slot++) {
        dcam_fill_frame_image(cam, cam->iso_buffer + (size_t) slot * cam->frame_bytes, cam->frame_counter++);
        iso_queue_frame(cam, slot);
    }
    cam->refill_slot = prefill % cam->ring_frames;
    LOG(TRACE, "[iso] prefilled %u frame(s)%s, %u pkts each (interrupt every %u pkts); starting iso", prefill,
        cam->iso_oneshot ? " (one-shot)" : "", cam->iso_ppf, ISO_INTR_STRIDE);

    struct fw_cdev_start_iso s = {
        .cycle = -1, // start as soon as possible
        .sync = 0,
        .tags = 0,
        .handle = cam->iso_handle,
    };
    if (ioctl(cam->iso_fd, FW_CDEV_IOC_START_ISO, &s) < 0) {
        LOG(ERROR, "START_ISO failed: %s", strerror(errno));
        return;
    }
    cam->iso_running = 1;
    LOG(TRACE, "[iso] START_ISO ok (handle=%d); awaiting completion interrupts to refill", cam->iso_handle);
    LOG(INFO,
        "ISO_EN set -> streaming F%d/M%d/R%d  %ux%u  pixfmt=%d  "
        "%u pkts/frame x %u B = %u B/frame  (%u-frame ring) on channel %d",
        cam->cur_format, cam->cur_mode, cam->cur_rate, cam->img_width, cam->img_height, cam->pixfmt, cam->iso_ppf,
        cam->iso_bpp, cam->frame_bytes, cam->ring_frames, cam->iso_channel);
}

void dcam_iso_stop(dcam_camera_t *cam) {
    if (!cam->iso_running && !cam->iso_created) {
        return;
    }

    if (cam->iso_created) {
        struct fw_cdev_stop_iso st = {.handle = cam->iso_handle};
        if (ioctl(cam->iso_fd, FW_CDEV_IOC_STOP_ISO, &st) < 0) {
            LOG(ERROR, "STOP_ISO failed: %s", strerror(errno));
        }
    }

    // Stop polling the iso fd before closing it.
    fw_device_set_iso_fd(cam->dev, -1);

    if (cam->iso_buffer != NULL) {
        munmap(cam->iso_buffer, cam->iso_buffer_bytes);
        cam->iso_buffer = NULL;
        cam->iso_buffer_bytes = 0;
    }
    if (cam->iso_fd >= 0) {
        close(cam->iso_fd);
        cam->iso_fd = -1;
    }
    cam->iso_created = 0;
    cam->iso_handle = -1;
    cam->iso_running = 0;
    LOG(INFO, "ISO_EN cleared -> streaming stopped (iso context torn down); %u frame(s) transmitted this stream",
        cam->frames_transmitted);
}

void dcam_on_iso_interrupt(fw_device_t *dev, void *ctx, unsigned completed_packets) {
    dcam_camera_t *cam = ctx;
    (void) dev;

    if (!cam->iso_running || cam->iso_ppf == 0 || cam->ring_frames == 0) {
        return;
    }

    // Bound the refill work by what can actually be outstanding: the ring is
    // ring_frames deep, so one completion batch can never cover more packets
    // than the whole ring. A corrupt count would otherwise drive the refill
    // loop below for millions of iterations, each rendering and checksumming
    // a full frame, freezing the run loop and with it every pending bus
    // request.
    unsigned outstanding = cam->ring_frames * cam->iso_ppf;
    if (completed_packets > outstanding) {
        LOG(WARN, "[iso] completion reports %u packets but at most %u can be outstanding; clamping", completed_packets,
            outstanding);
        completed_packets = outstanding;
    }

    cam->iso_acked_packets += completed_packets;
    while (cam->iso_acked_packets >= cam->iso_ppf) {
        cam->iso_acked_packets -= cam->iso_ppf;
        cam->frames_transmitted++;
        // In external trigger mode each transmitted frame consumes one pending
        // trigger pulse, clearing STATUS FRAME_READY once the last pending
        // frame has been sent.
        if (cam->i2c_regs[CAMREG_TRIGGER_MODE] == CAMREG_TRIGGER_EXTERNAL) {
            dcam_trigger_consume(cam);
        }

        if (cam->iso_oneshot) {
            // One-shot: the single queued frame has now transmitted.  Tear the
            // context down so the next ONE_SHOT re-arms with a fresh frame.  Safe
            // to stop from inside the interrupt handler: process_one_event does
            // not touch the iso fd after this returns and the run loop re-checks
            // it.  Return immediately — dcam_iso_stop frees cam->iso_buffer.
            LOG(TRACE, "[iso] one-shot frame %u transmitted on channel %d; stopping", cam->frames_transmitted,
                cam->iso_channel);
            dcam_iso_stop(cam);
            return;
        }

        // Continuous: regenerate and re-queue one ring slot to keep the
        // look-ahead queue exactly one ring deep.
        unsigned slot = cam->refill_slot;
        dcam_fill_frame_image(cam, cam->iso_buffer + (size_t) slot * cam->frame_bytes, cam->frame_counter++);
        iso_queue_frame(cam, slot);
        cam->refill_slot = (slot + 1) % cam->ring_frames;
        LOG(TRACE, "[iso] frame %u transmitted (refilled slot %u) on channel %d", cam->frames_transmitted, slot,
            cam->iso_channel);
        if (cam->frames_transmitted % 30 == 0) {
            LOG(INFO, "[iso] %u frames transmitted on channel %d", cam->frames_transmitted, cam->iso_channel);
        }
    }
}

void dcam_on_iso_error(fw_device_t *dev, void *ctx) {
    dcam_camera_t *cam = ctx;
    (void) dev;

    // The run loop reported a level-triggered error condition on the iso fd
    // (device shutdown or a dead transmit context). Closing the fd is what
    // clears the condition, so tear the whole context down; the next ISO_EN
    // or ONE_SHOT write starts a fresh one.
    dcam_iso_stop(cam);
}
