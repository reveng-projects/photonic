# DCAM camera registers and control sequences

The camera is an IIDC/DCAM 1.x device on the 1394 bus. This document is the
hardware-side reference for everything the driver programs over the bus:
configuration ROM discovery, the DCAM control-and-status registers (CSRs)
this camera implements, the feature (image-control) registers, and the
read/write sequences that enumerate, configure and start the video stream.

The camera head (CCD sequencer, gains, exposure, binning, readout window) is
a separate controller reached through the mailbox gate and has its own
register map, documented in [camera-head-registers.md](camera-head-registers.md).

Register definitions live in [photonic/dcam.c](../photonic/dcam.c) and
[photonic/properties.c](../photonic/properties.c). The fake camera models the
same register file in [fake-fw-dev/dcam.c](../fake-fw-dev/dcam.c).

## Addressing and byte order

All camera registers live in the 1394 initial register space: a 48-bit node
address whose high 16 bits are `0xFFFF`. Register accesses are asynchronous
quadlet reads and writes; block transactions are used only for the mailbox
gate. Every quadlet is big-endian on the wire.

Three address regions matter:

| Region | Address (low 32 bits) | Access |
|---|---|---|
| DCAM CSR block | CSR base + offset (base discovered from the config ROM) | quadlet read/write |
| Mailbox gate | `0xF0204000` | block write/read, see [camera-head-registers.md](camera-head-registers.md) |
| Vendor extended commands | `0x00000000` (write), `0x00000008` (response) | block write/read, see below |

All DCAM register offsets in this document are relative to the CSR base.

## Configuration ROM

The config ROM provides the device identity and the DCAM CSR base. The
driver obtains it from the bus driver's `REQUEST_GET_CONFIGURATION_INFO`
IRB, which returns the ROM header, the bus-info block, the unit directory,
the unit-dependent directory and the two textual leaves in one call (issue
it twice: first with NULL buffers to learn the sizes, then with pool
buffers). The relevant entries:

| Item | Location | Meaning |
|---|---|---|
| EUI-64 / serial number | bus-info block quadlets 3 and 4 (ROM offsets 0x0C, 0x10) | quadlet 3 = node_vendor_id and chip_id_hi, quadlet 4 = chip_id_lo |
| DCAM software version | unit directory, key `0x13` | 24-bit unit_sw_version: `0x000100` = DCAM 1.04, `0x000101` = 1.20, `0x000102` = 1.30 |
| DCAM CSR base | unit-dependent directory, key `0x40` | low 32 bits of the register-space address = `value24 * 4 + 0xF0000000` |
| Vendor / model name | vendor and model TEXTUAL_LEAF | ASCII payload after the 12-byte leaf header (length/CRC, character set, language quadlets); either leaf may be absent |

A camera without a unit-dependent directory cannot be operated (there is no
CSR base). Absent textual leaves are a normal condition and only disable
the name queries.

## DCAM register map

Registers actually used by this camera. Values written to the format, mode
and rate registers are selector fields in the top bits, as defined by DCAM.

| Offset | Name | Access | Contents |
|---|---|---|---|
| 0x000 | INITIALIZE | W | bit 31: camera soft reset. Does not reset the camera head, which is a separate controller |
| 0x100 | VIDEO_FORMAT_INQ | R | bitmask of supported formats, bit `31 - f` for format f |
| 0x180 + f*4 | VIDEO_MODE_INQ_f | R | bitmask of supported modes of format f, bit `31 - m` |
| 0x200 + (f*8 + m)*4 | FRAME_RATE_INQ_f_m | R | bitmask of supported rates of a fixed mode, bit `31 - r` |
| 0x2E0 + m*4 | V_CSR_INQ_7_m | R | Format 7 mode m CSR block: quadlet offset, byte offset = `value * 4` |
| 0x404 | FEATURE_HI_INQ | R | presence bitmask of the 0x500 feature block, MSB first |
| 0x408 | FEATURE_LO_INQ | R | presence bitmask of the 0x580 feature block, MSB first |
| 0x500..0x530 | feature inquiry (high block) | R | per-feature capability word, see below |
| 0x580..0x58C, 0x5C0..0x5C4 | feature inquiry (low block) | R | same layout |
| 0x600 | FRAME_RATE | R/W | `rate << 29` |
| 0x604 | VIDEO_MODE | R/W | `mode << 29` |
| 0x608 | VIDEO_FORMAT | R/W | `format << 29` |
| 0x60C | ISOCH_CHANNEL | W | `channel << 28 \| speed << 24` (speed 0/1/2 = S100/S200/S400) |
| 0x614 | ISO_EN | W | bit 31: continuous isochronous transmission on/off |
| 0x61C | ONE_SHOT | W | bit 31: arm a single-frame acquisition; 0 cancels a pending arm |
| 0x800..0x8xx | feature value registers | R/W | value register of the inquiry register at `offset - 0x300`, see below |

### Format 7 mode CSR block

Each Format 7 (scalable image) mode has its own CSR block located by
`V_CSR_INQ_7_m`. Offsets within the block:

| Offset | Name | Access | Contents |
|---|---|---|---|
| 0x00 | MAX_IMAGE_SIZE | R | `max_width << 16 \| max_height` |
| 0x04 | UNIT_SIZE | R | `unit_width << 16 \| unit_height` (geometry granularity) |
| 0x08 | IMAGE_POSITION | R/W | `left << 16 \| top` |
| 0x0C | IMAGE_SIZE | R/W | `width << 16 \| height` |
| 0x10 | COLOR_CODING_ID | R/W | `coding << 24` |
| 0x14 | COLOR_CODING_INQ | R | bitmask of supported codings |
| 0x34 | PIXEL_NUMBER_INQ | R | pixels per frame for the current geometry |
| 0x38 | TOTAL_BYTES_HI | R | total bytes per frame, high 32 bits |
| 0x3C | TOTAL_BYTES_LO | R | total bytes per frame, low 32 bits |
| 0x40 | PACKET_PARA_INQ | R | `unit_bytes_per_packet << 16 \| max_bytes_per_packet` |
| 0x44 | BYTE_PER_PACKET | R/W | `bytes_per_packet << 16` |
| 0x48 | PACKET_PER_FRAME_INQ | R | packets per frame for the current packet size |

**Hardware quirk.** The real camera rejects an `IMAGE_SIZE` write with a
data error (`RCODE_DATA_ERROR`, surfacing as `STATUS_DEVICE_DATA_ERROR`)
in some states, notably when writing back the maximum size during
enumeration. Treat the Format 7 CSR block as read-only until a mode is
actually being configured for streaming.

### Feature (image control) registers

Each DCAM feature has an inquiry register in the 0x500/0x580 blocks and a
value register 0x300 above it. Features present on this camera family, with
their inquiry registers:

| Inquiry | Value | Feature |
|---|---|---|
| 0x500 | 0x800 | Brightness |
| 0x504 | 0x804 | Auto exposure |
| 0x508 | 0x808 | Sharpness |
| 0x50C | 0x80C | White balance (two 12-bit fields: U/B in bits 23:12, V/R in bits 11:0) |
| 0x510 | 0x810 | Hue |
| 0x514 | 0x814 | Saturation |
| 0x518 | 0x818 | Gamma |
| 0x51C | 0x81C | Shutter (exposure) |
| 0x520 | 0x820 | Gain (contrast) |
| 0x524 | 0x824 | Iris |
| 0x528 | 0x828 | Focus |
| 0x52C | 0x82C | Temperature (target bits 23:12, current bits 11:0, current is read-only) |
| 0x530 | 0x830 | Trigger (mode bits 19:16, polarity bit 24, parameter bits 11:0) |
| 0x580 | 0x880 | Zoom |
| 0x584 | 0x884 | Pan |
| 0x588 | 0x888 | Tilt |

Inquiry word layout: bit 31 presence, bit 30 absolute-value capable, bit 27
readout capable, bit 25 auto capable, bit 24 manual capable, bits 23:12
minimum, bits 11:0 maximum.

Value word layout: bit 31 presence, bit 30 absolute mode active, bit 26
one-push, bit 25 ON/OFF, bit 24 auto/manual, bits 11:0 value (plus the
split fields noted above for white balance, temperature and trigger).

When a feature is absolute-capable (inquiry bit 30), the register at
`inquiry + 0x200` holds the quadlet offset of an absolute-value CSR block
(byte address = `value * 4 + 0xF0000000`); its VALUE register at block
offset 8 is an IEEE-754 float.

### What the real camera implements

The production camera implements far less than the register map allows.
Measured behavior:

| Value register | Feature | Behavior |
|---|---|---|
| 0x800 | Brightness | present; only 4 value bits (0..15), larger writes are masked; auto bit never sticks |
| 0x81C | Shutter | presence bit set and writes accepted, but the value always reads back 0; the feature cannot round-trip and must be treated as absent by a GET-capable interface |
| 0x820 | Gain | present; value clamps to 0..383; auto bit never sticks |
| 0x808, 0x828 | Sharpness, Focus | reads return 0 (no presence bit); focus writes rejected |
| 0x80C..0x824 | White balance, Hue, Saturation, Gamma, Iris | reads return junk without the presence bit |
| 0x880, 0x884, 0x888 | Zoom, Pan, Tilt | reads and writes rejected with a data error |

Consequences for a driver: never advertise a control from a hardcoded
list. Probe presence (read the value register, require bit 31), take the
range from the inquiry word when it is coherent (presence, readout and
manual bits set, min < max), and otherwise probe empirically: write full
scale, read back what the camera kept (its maximum, whether it clamps or
masks), test whether the auto bit sticks, then restore the original mode
and value. The driver implements this in
`PhotonicBuildDevicePropertySets` ([photonic/properties.c](../photonic/properties.c)).

The fake camera advertises the 13 driver-mapped controls with per-feature ranges from the
shared `FRAME_META_FEATURE_RANGES` list ([common/frame_meta.h](../common/frame_meta.h)).
The `PHOTONIC_FEATURES` environment variable overrides the implemented set
(`all`, `none`, or a comma-separated name list) to reproduce real-camera
configurations, for example `PHOTONIC_FEATURES=brightness,contrast`.

## Control sequences

### Reset

Write bit 31 to INITIALIZE (0x000). This resets the DCAM state (format,
mode, rate, feature values) but not the camera head: head registers keep
their values across the soft reset and only reset at power-up. A driver
must invalidate any cached format state after issuing the reset.

### Mode enumeration

1. Read VIDEO_FORMAT_INQ (0x100). For each supported format f, read
   VIDEO_MODE_INQ_f (0x180 + f*4).
2. Fixed formats (0, 1, 2): for each supported mode, read
   FRAME_RATE_INQ_f_m to learn the supported rates. Geometry and pixel
   coding are implied by (format, mode) per the DCAM specification.
3. Format 7: write `7 << 29` to VIDEO_FORMAT, then for each supported mode
   write `mode << 29` to VIDEO_MODE before touching its registers (a camera
   that refreshes per-mode registers only for the selected mode would
   otherwise return stale values), read V_CSR_INQ_7_m to locate the mode's
   CSR block and read MAX_IMAGE_SIZE, UNIT_SIZE, COLOR_CODING_INQ,
   IMAGE_SIZE, COLOR_CODING_ID, PACKET_PARA_INQ and BYTE_PER_PACKET from
   it. Do not write IMAGE_SIZE during enumeration (see the hardware quirk
   above).
4. Restore or normalize the VIDEO_FORMAT/VIDEO_MODE selection onto an
   enumerated mode. Probing leaves the camera selected on the last-probed
   mode, which may have been skipped as unusable.

### Starting a fixed-format stream

1. Write VIDEO_FORMAT (`format << 29`), VIDEO_MODE (`mode << 29`),
   FRAME_RATE (`rate << 29`).
2. Allocate bus resources (bandwidth, channel, receive resources) on the
   host side.
3. Write ISOCH_CHANNEL: `channel << 28 | speed << 24`.
4. Start the host listen, then write bit 31 to ISO_EN. The camera
   transmits one frame per rate period, one isochronous packet per 125 us
   bus cycle, until ISO_EN is cleared.

### Configuring and starting a Format 7 stream

1. Write VIDEO_FORMAT (`7 << 29`) and VIDEO_MODE (`mode << 29`).
2. Write IMAGE_SIZE (`width << 16 | height`) and COLOR_CODING_ID
   (`coding << 24`) in the mode's CSR block. IMAGE_POSITION may be
   written for a sub-window; coordinates and size must be multiples of
   UNIT_SIZE.
3. Read PACKET_PARA_INQ, which the camera recomputes for the new geometry:
   high word = unit bytes per packet, low word = maximum bytes per packet.
4. Choose the packet size for the desired frame interval. Derive the
   target `bpp = frame_bytes * 125 us / frame_interval`, then snap it to
   a legal size: a multiple of the unit, within one unit and the
   advertised maximum, that **divides `frame_bytes` exactly** (the
   largest such divisor at or below the target, or the smallest above it
   when none is smaller). Without the exact-divisor requirement the
   floored quotient of step 5 would truncate the frame tail on every
   frame. A geometry with no legal divisor is rejected rather than
   streamed truncated. Cap the maximum at the negotiated bus speed's
   payload limit first (S100: 1024, S200: 2048, S400: 4096 bytes per
   packet); the camera advertises its S400 maximum regardless of the
   actual link speed.
5. Write BYTE_PER_PACKET (`bpp << 16`). The frame then streams as exactly
   `frame_bytes / bpp` packets.
6. Proceed as for a fixed stream: ISOCH_CHANNEL, host listen, ISO_EN.

### Single-frame (triggered) acquisition

With ISO_EN off, writing bit 31 to ONE_SHOT arms one acquisition: the
camera waits for its trigger condition, captures one frame, transmits it
isochronously on the programmed channel and returns to idle. Writing 0 to
ONE_SHOT cancels an armed acquisition that has not fired yet. Re-arming
before the pending shot fires replaces it.

The trigger itself is external: the camera head is put in external-trigger
mode (`CAMREG_TRIGGER_MODE = 0x31` over the mailbox gate) and the exposure
is started by a hardware pulse on the camera's trigger input, wired to a
serial port's DTR line. The interval between arm and frame is unbounded;
the host must be listening on the channel before arming.

## Vendor extended commands

Trigger timing, strobe and image-flip settings do not live in DCAM CSRs.
They are sent as vendor extended commands: an asynchronous block write to
node address `0xFFFF:0x00000000` (not in CSR space). When a command has a
response, it is block-read from `0xFFFF:0x00000008`. There is no handshake
or delay between write and read.

Packet layout, `8 + N` bytes, every dword big-endian on the wire:

| Offset | Size | Content |
|---|---|---|
| 0x00 | 4 | magic `0x00016800` |
| 0x04 | 4 | `(command_id << 16) \| 0x0000F064` |
| 0x08 | N | payload dwords |

Known commands:

| Id | Payload | Meaning |
|---|---|---|
| 0x9 | 4 bytes | trigger delay |
| 0xA | 4 bytes | trigger polarity |
| 0xB | 4 bytes | trigger parameter (exposure) |
| 0xC | 12 bytes | strobe configuration (see [remaining-ioctls-design.md](remaining-ioctls-design.md)) |
| 0xD | 8 bytes | image flip (horizontal dword, vertical dword) |

No currently specified command requests a response. This channel is not
yet implemented in the driver and not yet modeled by the fake camera; see
[future-work.md](future-work.md).
