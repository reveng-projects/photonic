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
/// Passive isochronous receive verifier.  Joins one isochronous channel on a
/// local OHCI controller and checks, packet by packet, that a stream that
/// transmits every 125us bus cycle actually arrives on every cycle.  The
/// controller stamps each received packet with the bus cycle timer, so a jump
/// of more than one cycle between consecutive packets pinpoints exactly how
/// many packets were lost and when.  Frames are delimited by the sy field of
/// the packet header (sy=1 marks the first packet of a frame).
///
/// The tool reports, per frame and per stream segment: packets received,
/// cycles spanned, packets lost, and the loss bursts (clusters of nearby
/// losses) with their spacing.  A transmitter that emits back-to-back frames
/// at one packet per cycle should show zero lost cycles end to end; any other
/// result localises the loss to the path between the transmitting controller
/// and this listener.
///
/// Listening is passive: no channel or bandwidth allocation is performed, so
/// the tool can observe a channel that two other nodes are already using.

#include "log.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/firewire-cdev.h>
#include <linux/firewire-constants.h>

/// Per-packet header bytes delivered by the kernel: the 1394 isochronous
/// packet header quadlet followed by a timestamp quadlet (ABI version 2+).
constexpr uint32_t IRL_HEADER_BYTES = 8;

/// Payload bytes reserved per packet slot.  4096 covers the largest S400
/// isochronous payload; override with -p for S800 streams.
constexpr uint32_t IRL_DEFAULT_SLOT_BYTES = 4096;

/// Packet slots in the receive ring.  2048 slots is 256ms of look-ahead at one
/// packet per cycle, ample margin over the interrupt cadence below.
constexpr unsigned IRL_RING_SLOTS = 2048;

/// Request a completion interrupt every this many packets (8ms of stream).
/// Small enough that the kernel's per-context header buffer never overflows,
/// large enough to keep the event rate trivial.
constexpr unsigned IRL_INTR_STRIDE = 64;

/// Maximum packet control words submitted per QUEUE_ISO call.
constexpr unsigned IRL_MAX_PKTS_PER_QUEUE = 512;

/// The bus cycle timer wraps every 8 seconds (8 x 8000 cycles).
constexpr uint32_t IRL_CYCLES_PER_WRAP = 8 * 8000;

/// Losses closer than this many cycles apart are counted as one burst.
constexpr uint64_t IRL_BURST_GAP_CYCLES = 16;

/// Wall-clock silence that ends a stream segment and flushes its summary.
constexpr int64_t IRL_IDLE_NS = 1000000000;

/// Set by SIGINT to make the main loop print the final summary and exit.
static volatile sig_atomic_t g_stop;

static void on_sigint(int sig) {
    (void) sig;
    g_stop = 1;
}

/// Loss and framing statistics for one stream segment (a run of packets with
/// no wall-clock silence).  Frame counters restart at every sy=1 packet.
typedef struct listener_stats {
    int in_segment;       ///< A segment is open (at least one packet seen)
    uint32_t prev_ts;     ///< Cycle timestamp of the previous packet (sec:3 cycle:13)
    uint64_t abs_cycle;   ///< Unwrapped cycle counter across the segment
    uint64_t seg_packets; ///< Packets received this segment
    uint64_t seg_lost;    ///< Cycles that carried no packet this segment
    uint64_t seg_bursts;  ///< Loss bursts this segment
    uint64_t seg_frames;  ///< sy=1 markers this segment
    uint64_t last_loss;   ///< abs_cycle of the most recent loss
    uint64_t burst_start; ///< abs_cycle of the current burst's first loss
    uint64_t prev_burst;  ///< burst_start of the previous burst
    uint64_t space_sum;   ///< Sum of burst-to-burst spacings (cycles)
    uint64_t space_min;   ///< Smallest burst spacing seen
    uint64_t space_max;   ///< Largest burst spacing seen
    // Current frame (since the last sy=1 packet)
    int frame_open;         ///< A sy=1 packet has been seen this segment
    uint64_t frame_start;   ///< abs_cycle of the frame's first packet
    uint64_t frame_pkts;    ///< Packets received this frame
    uint64_t frame_lost;    ///< Cycles lost this frame
    uint64_t frame_bursts;  ///< Loss bursts this frame
    uint32_t frame_len_min; ///< Smallest payload length this frame
    uint32_t frame_len_max; ///< Largest payload length this frame
    // Grand totals across segments
    uint64_t total_packets;
    uint64_t total_lost;
    uint64_t total_bursts;
    uint64_t total_frames;
    uint64_t total_segments;
} listener_stats_t;

/// Emit the summary line for the frame in progress, if any, and reset the
/// per-frame counters.  Clean frames log at TRACE, lossy frames at INFO.
///
/// @param st  Statistics to flush.
static void flush_frame(listener_stats_t *st) {
    if (!st->frame_open) {
        return;
    }
    uint64_t cycles = st->abs_cycle - st->frame_start + 1;
    log_write(st->frame_lost != 0 ? LOGLEVEL_INFO : LOGLEVEL_TRACE, __FILE__, __LINE__,
              "[frame] %" PRIu64 " pkts in %" PRIu64 " cycles, %" PRIu64 " lost (%" PRIu64 " bursts), payload %u..%u B",
              st->frame_pkts, cycles, st->frame_lost, st->frame_bursts, st->frame_len_min, st->frame_len_max);
    st->frame_open = 0;
    st->frame_pkts = 0;
    st->frame_lost = 0;
    st->frame_bursts = 0;
    st->frame_len_min = UINT32_MAX;
    st->frame_len_max = 0;
}

/// Close the current segment: flush the open frame, log the segment summary
/// and fold the counters into the grand totals.
///
/// @param st  Statistics to flush.
static void flush_segment(listener_stats_t *st) {
    if (!st->in_segment) {
        return;
    }
    flush_frame(st);
    double pct = st->seg_packets + st->seg_lost > 0
                     ? 100.0 * (double) st->seg_lost / (double) (st->seg_packets + st->seg_lost)
                     : 0.0;
    if (st->seg_bursts >= 2) {
        LOG(INFO,
            "[segment] %" PRIu64 " pkts, %" PRIu64 " frames, %" PRIu64 " lost (%.2f%%) in %" PRIu64
            " bursts, burst spacing min/avg/max %" PRIu64 "/%" PRIu64 "/%" PRIu64 " cycles",
            st->seg_packets, st->seg_frames, st->seg_lost, pct, st->seg_bursts, st->space_min,
            st->space_sum / (st->seg_bursts - 1), st->space_max);
    } else {
        LOG(INFO, "[segment] %" PRIu64 " pkts, %" PRIu64 " frames, %" PRIu64 " lost (%.2f%%) in %" PRIu64 " bursts",
            st->seg_packets, st->seg_frames, st->seg_lost, pct, st->seg_bursts);
    }
    st->total_packets += st->seg_packets;
    st->total_lost += st->seg_lost;
    st->total_bursts += st->seg_bursts;
    st->total_frames += st->seg_frames;
    st->total_segments++;
    st->in_segment = 0;
    st->seg_packets = 0;
    st->seg_lost = 0;
    st->seg_bursts = 0;
    st->seg_frames = 0;
    st->space_sum = 0;
    st->space_min = UINT64_MAX;
    st->space_max = 0;
}

/// Account one received packet.
///
/// @param st       Statistics to update.
/// @param length   Payload length from the packet header.
/// @param sy       sy field from the packet header.
/// @param ts       Timestamp quadlet's low 16 bits (3-bit seconds, 13-bit cycle).
static void account_packet(listener_stats_t *st, uint32_t length, uint32_t sy, uint32_t ts) {
    uint32_t now = ((ts >> 13) & 7) * 8000 + (ts & 0x1fff);

    if (!st->in_segment) {
        st->in_segment = 1;
        st->abs_cycle = 0;
        LOG(INFO, "[segment] stream started (payload %u B, sy=%u)", length, sy);
    } else {
        uint32_t delta = (now + IRL_CYCLES_PER_WRAP - st->prev_ts) % IRL_CYCLES_PER_WRAP;
        if (delta == 0) {
            LOG(WARN, "[loss] two packets share cycle timestamp 0x%04x", ts);
            delta = 1;
        }
        st->abs_cycle += delta;
        if (delta > 1) {
            uint64_t missing = delta - 1;
            st->seg_lost += missing;
            st->frame_lost += missing;
            // Cluster losses into bursts and track burst-to-burst spacing,
            // which exposes periodic interference at a glance.
            if (st->last_loss == 0 || st->abs_cycle - st->last_loss > IRL_BURST_GAP_CYCLES) {
                if (st->burst_start != 0) {
                    uint64_t space = st->abs_cycle - st->burst_start;
                    st->space_sum += space;
                    if (space < st->space_min) {
                        st->space_min = space;
                    }
                    if (space > st->space_max) {
                        st->space_max = space;
                    }
                }
                st->burst_start = st->abs_cycle;
                st->seg_bursts++;
                st->frame_bursts++;
            }
            st->last_loss = st->abs_cycle;
            LOG(DEBUG, "[loss] %" PRIu64 " packet(s) missing before cycle %" PRIu64 " (frame pkt %" PRIu64 ")", missing,
                st->abs_cycle, st->frame_pkts);
        }
    }
    st->prev_ts = now;
    st->seg_packets++;

    if (sy != 0) {
        flush_frame(st);
        st->frame_open = 1;
        st->frame_start = st->abs_cycle;
        st->seg_frames++;
    }
    st->frame_pkts++;
    if (length < st->frame_len_min) {
        st->frame_len_min = length;
    }
    if (length > st->frame_len_max) {
        st->frame_len_max = length;
    }
}

/// Queue `count` packet slots starting at ring slot `first`, splitting at the
/// ring wrap point and at the QUEUE_ISO submission limit.  Every slot reserves
/// IRL_HEADER_BYTES of header and slot_bytes of payload; an interrupt is
/// requested every IRL_INTR_STRIDE slots (by absolute slot index, so the
/// cadence is independent of where submissions split).
///
/// @param fd          Iso context fd.
/// @param handle      Iso context handle.
/// @param buffer      mmap'ed DMA buffer base.
/// @param slot_bytes  Payload bytes per ring slot.
/// @param first       First ring slot to queue.
/// @param count       Number of slots to queue.
/// @return            0 on success, -1 on ioctl failure or a stalled queue.
static int queue_slots(int fd, int handle, uint8_t *buffer, uint32_t slot_bytes, unsigned first, unsigned count) {
    uint32_t controls[IRL_MAX_PKTS_PER_QUEUE];

    while (count > 0) {
        unsigned slot = first % IRL_RING_SLOTS;
        unsigned chunk = IRL_RING_SLOTS - slot; // stop at the ring wrap
        unsigned i;

        if (chunk > count) {
            chunk = count;
        }
        if (chunk > IRL_MAX_PKTS_PER_QUEUE) {
            chunk = IRL_MAX_PKTS_PER_QUEUE;
        }
        for (i = 0; i < chunk; i++) {
            uint32_t control = FW_CDEV_ISO_HEADER_LENGTH(IRL_HEADER_BYTES) | FW_CDEV_ISO_PAYLOAD_LENGTH(slot_bytes);
            if ((slot + i + 1) % IRL_INTR_STRIDE == 0) {
                control |= FW_CDEV_ISO_INTERRUPT;
            }
            controls[i] = control;
        }

        struct fw_cdev_queue_iso q = {
            .packets = (uintptr_t) controls,
            .data = (uintptr_t) (buffer + (size_t) slot * slot_bytes),
            .size = chunk * (uint32_t) sizeof(controls[0]),
            .handle = handle,
        };
        unsigned done = 0;
        for (;;) {
            uint32_t before = q.size;
            if (ioctl(fd, FW_CDEV_IOC_QUEUE_ISO, &q) < 0) {
                LOG(ERROR, "QUEUE_ISO failed: %s", strerror(errno));
                return -1;
            }
            if (q.size == 0) {
                break;
            }
            if (q.size == before) {
                LOG(ERROR, "QUEUE_ISO stalled with %u of %u slots left", q.size / 4, chunk);
                return -1;
            }
            done += (before - q.size) / (uint32_t) sizeof(controls[0]);
            q.packets = (uintptr_t) (controls + done);
            q.data = (uintptr_t) (buffer + (size_t) (slot + done) * slot_bytes);
        }
        first += chunk;
        count -= chunk;
    }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [-d /dev/fwN] [-c channel] [-p payload_bytes] [-s seconds] [-v]\n"
            "  -d  firewire device of the listening controller (default /dev/fw0)\n"
            "  -c  isochronous channel to listen on (default 0)\n"
            "  -p  payload bytes reserved per packet (default %u)\n"
            "  -s  stop after this many seconds (default: run until Ctrl+C)\n"
            "  -v  verbose: log every loss and every frame\n",
            argv0, IRL_DEFAULT_SLOT_BYTES);
}

int main(int argc, char **argv) {
    const char *dev_path = "/dev/fw0";
    uint32_t channel = 0;
    uint32_t slot_bytes = IRL_DEFAULT_SLOT_BYTES;
    long run_seconds = 0;
    int opt;

    log_set_level(LOGLEVEL_INFO);
    while ((opt = getopt(argc, argv, "d:c:p:s:vh")) != -1) {
        switch (opt) {
            case 'd':
                dev_path = optarg;
                break;
            case 'c':
                channel = (uint32_t) strtoul(optarg, NULL, 0);
                break;
            case 'p':
                slot_bytes = (uint32_t) strtoul(optarg, NULL, 0);
                break;
            case 's':
                run_seconds = strtol(optarg, NULL, 0);
                break;
            case 'v':
                log_set_level(LOGLEVEL_TRACE);
                break;
            default:
                usage(argv[0]);
                return opt == 'h' ? 0 : 2;
        }
    }
    // Accept the device as a positional argument too, like the fake camera.
    if (optind < argc) {
        dev_path = argv[optind];
    }
    if (channel > 63 || slot_bytes == 0 || slot_bytes > 8192) {
        usage(argv[0]);
        return 2;
    }

    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        LOG(ERROR, "open %s failed: %s", dev_path, strerror(errno));
        return 1;
    }

    // Negotiate ABI version 4 so each packet's header block carries the
    // timestamp quadlet (version 1 would deliver payload quadlets instead).
    struct fw_cdev_get_info info = {.version = 4};
    if (ioctl(fd, FW_CDEV_IOC_GET_INFO, &info) < 0) {
        LOG(ERROR, "GET_INFO failed: %s", strerror(errno));
        return 1;
    }

    struct fw_cdev_create_iso_context cctx = {
        .type = FW_CDEV_ISO_CONTEXT_RECEIVE,
        .header_size = IRL_HEADER_BYTES,
        .channel = channel,
        .speed = 0, // ignored for receive contexts
        .closure = 0,
    };
    if (ioctl(fd, FW_CDEV_IOC_CREATE_ISO_CONTEXT, &cctx) < 0) {
        LOG(ERROR, "CREATE_ISO_CONTEXT (receive) failed: %s", strerror(errno));
        return 1;
    }

    size_t buffer_bytes = (size_t) IRL_RING_SLOTS * slot_bytes;
    uint8_t *buffer = mmap(NULL, buffer_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer == MAP_FAILED) {
        LOG(ERROR, "mmap (%zu bytes) failed: %s", buffer_bytes, strerror(errno));
        return 1;
    }

    if (queue_slots(fd, cctx.handle, buffer, slot_bytes, 0, IRL_RING_SLOTS) < 0) {
        return 1;
    }

    struct fw_cdev_start_iso start = {
        .cycle = -1, // start immediately, no cycle match
        .sync = 0,
        .tags = FW_CDEV_ISO_CONTEXT_MATCH_ALL_TAGS,
        .handle = cctx.handle,
    };
    if (ioctl(fd, FW_CDEV_IOC_START_ISO, &start) < 0) {
        LOG(ERROR, "START_ISO failed: %s", strerror(errno));
        return 1;
    }
    LOG(INFO, "listening on %s channel %u (%u slots x %u B, interrupt every %u pkts)", dev_path, channel,
        IRL_RING_SLOTS, slot_bytes, IRL_INTR_STRIDE);
    LOG(INFO, "abi=%u card version=0x%08x", info.version, info.card);

    signal(SIGINT, on_sigint);

    listener_stats_t st = {.frame_len_min = UINT32_MAX, .space_min = UINT64_MAX};
    unsigned tail = 0; // next ring slot to requeue
    struct timespec now, last_pkt = {0}, deadline = {0};

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (run_seconds > 0) {
        deadline = now;
        deadline.tv_sec += run_seconds;
    }

    while (!g_stop) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int rv = poll(&pfd, 1, 250);

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (run_seconds > 0 &&
            (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))) {
            break;
        }
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR, "poll failed: %s", strerror(errno));
            break;
        }
        // Flush the segment after a second of silence so per-run summaries
        // appear as soon as the transmitter stops.
        int64_t idle_ns = (now.tv_sec - last_pkt.tv_sec) * 1000000000 + (now.tv_nsec - last_pkt.tv_nsec);
        if (st.in_segment && idle_ns > IRL_IDLE_NS) {
            flush_segment(&st);
        }
        if (rv == 0) {
            continue;
        }

        // The event carries IRL_HEADER_BYTES of header per completed packet;
        // size the read buffer for a full interrupt stride plus slack.
        union {
            union fw_cdev_event ev;
            uint8_t bytes[sizeof(union fw_cdev_event) + 2 * IRL_INTR_STRIDE * IRL_HEADER_BYTES];
        } buf;
        ssize_t len = read(fd, &buf, sizeof(buf));
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR, "read event failed: %s", strerror(errno));
            break;
        }

        if (buf.ev.common.type == FW_CDEV_EVENT_BUS_RESET) {
            LOG(INFO, "[bus reset] generation=%u node_id=0x%04x", buf.ev.bus_reset.generation,
                buf.ev.bus_reset.node_id);
            continue;
        }
        if (buf.ev.common.type != FW_CDEV_EVENT_ISO_INTERRUPT) {
            LOG(DEBUG, "[event] type=0x%x ignored", buf.ev.common.type);
            continue;
        }

        struct fw_cdev_event_iso_interrupt *it = &buf.ev.iso_interrupt;
        unsigned packets = it->header_length / IRL_HEADER_BYTES;
        unsigned i;

        last_pkt = now;
        for (i = 0; i < packets; i++) {
            // Both quadlets arrive big-endian: the 1394 isochronous packet
            // header (length:16 tag:2 channel:6 tcode:4 sy:4) and the
            // timestamp (seconds:3 cycle:13 in the low 16 bits).
            uint32_t hdr = be32toh(it->header[i * 2]);
            uint32_t ts = be32toh(it->header[i * 2 + 1]) & 0xffff;
            uint32_t length = hdr >> 16;
            uint32_t sy = hdr & 0xf;

            if (((hdr >> 8) & 0x3f) != channel) {
                LOG(WARN, "[pkt] unexpected channel %u in header 0x%08x", (hdr >> 8) & 0x3f, hdr);
            }
            if (length > slot_bytes) {
                LOG(WARN, "[pkt] payload %u B exceeds the %u B slot (truncated)", length, slot_bytes);
            }
            account_packet(&st, length, sy, ts);
        }

        if (queue_slots(fd, cctx.handle, buffer, slot_bytes, tail, packets) < 0) {
            break;
        }
        tail = (tail + packets) % IRL_RING_SLOTS;
    }

    flush_segment(&st);
    if (st.total_packets + st.total_lost > 0) {
        LOG(INFO,
            "[total] %" PRIu64 " segments: %" PRIu64 " pkts, %" PRIu64 " frames, %" PRIu64 " lost (%.2f%%) in %" PRIu64
            " bursts",
            st.total_segments, st.total_packets, st.total_frames, st.total_lost,
            100.0 * (double) st.total_lost / (double) (st.total_packets + st.total_lost), st.total_bursts);
    } else {
        LOG(INFO, "[total] no packets received");
    }

    munmap(buffer, buffer_bytes);
    close(fd);
    return 0;
}
