# Remaining IOCTLs — implementation specification

This document specifies the IOCTL handlers that are still stubs returning
`STATUS_NOT_IMPLEMENTED`, plus one mailbox sub-command. It is the work
plan for completing the IOCTL surface; none of it is implemented yet (see
[future-work.md](future-work.md) for the consolidated gap list).

The specified behavior is the wire contract required for compatibility
with the original driver and its client DLL. Where the original behavior
contains a bug or a quirk, the spec says **replicate + comment**: the
reimplementation must behave identically, because the quirk is part of
the observable contract, and must carry a code comment noting the oddity.

Wire formats (struct layouts, IOCTL codes, per-IOCTL minimum buffer
sizes) are already authoritative in [ioctl.h](../photonic/ioctl.h) and
enforced by the dispatch table (ioctl/ioctl.c); this document specifies
handler *behavior* and the device state behind it. A few ioctl.h comments
were wrong and have already been corrected (§10 records them). Camera-side
register layouts (config ROM, DCAM CSRs, vendor extended commands) are in
[dcam-registers.md](dcam-registers.md).

Scope, by file:

- **ioctl/identity.c** — SDK_VERSION_1/2, SERIAL_NUMBER,
  SET_CANCEL_TIMEOUT, GET_VENDOR_NAME, GET_MODEL_NAME, GET_DCAM_VERSION,
  GET_NAMES_LENGTH (§4)
- **ioctl/property.c** — INITIALIZE, PROPERTY_GET, PROPERTY_SET (§5, §6)
- **ioctl/format.c** — IMAGE_FLIP, GET_CHANNEL, GET_PACKET_SIZE (§7)
- **ioctl/trigger.c** — TRIGGER_SET, STROBE_SET (§7)
- **ioctl/mailbox.c** — MAILBOX_CMD_GET_ADDRESSES (§8)

Highest value for the client DLL: SERIAL_NUMBER,
GET_VENDOR_NAME/GET_MODEL_NAME, GET_DCAM_VERSION (all used by
`PlInitialize`/`PlGetDeviceInfo`), then PROPERTY_GET/SET.

---

## 1. Handler conventions

Follow the existing finished handlers (`PhotonicIoctlGetLastError`,
`PhotonicIoctlSubwindowGet/Set`):

- `FuncEntry(TRACE_FLAG_IOCTL)` plus parameter/state traces.
- `Request->Buffer` is the shared METHOD_BUFFERED buffer;
  `Request->Information` = bytes returned; `Request->Error` (if set) is
  committed to `Extension->LastError` by the dispatcher.
- The removed-device check is per-handler. **Important**: the original
  driver is inconsistent about it, and the differences are
  client-visible; each per-IOCTL section below states exactly what to
  do. Do not blanket-apply the usual `return STATUS_DEVICE_REMOVED`
  pattern.
- Several handlers report errors **only** in the buffer's `Status` field
  while returning `STATUS_SUCCESS` and leaving LastError untouched.
  Others set LastError without touching the buffer. Each section states
  which. When a handler must set LastError *without* failing the IRP
  (GET_CHANNEL, GET_PACKET_SIZE), set `Request->Error` as usual: the
  dispatcher commits it to `Extension->LastError` regardless of the
  returned NTSTATUS, so it works on a succeeding IRP too.

NTSTATUS codes used below: `STATUS_SUCCESS`, `STATUS_UNSUCCESSFUL`
(0xC0000001), `STATUS_NOT_IMPLEMENTED` (0xC0000002),
`STATUS_INSUFFICIENT_RESOURCES` (0xC000009A), `STATUS_BUFFER_TOO_SMALL`
(0xC0000023), `STATUS_NOT_SUPPORTED` (0xC00000BB), `STATUS_IO_TIMEOUT`
(0xC00000B5), and the removed-device status the original driver uses:
0xC00002B6 (`STATUS_DEVICE_REMOVED`).

---

## 2. New device-extension state and its population

Add to `PHOTONIC_DEVICE_EXTENSION`:

```c
UINT32 SerialNumber[2];      // EUI-64 words from the bus-info block, CPU order.
                             // [0] = quadlet 3 (node_vendor_id | chip_id_hi),
                             // [1] = quadlet 4 (chip_id_lo).
PUCHAR VendorLeaf;           // raw vendor TEXTUAL_LEAF (pool copy), NULL if absent
ULONG  VendorLeafLength;     // total leaf bytes incl. 12-byte header, 0 if absent
PUCHAR ModelLeaf;            // raw model TEXTUAL_LEAF
ULONG  ModelLeafLength;
UINT32 DcamUnitSwVersion;    // 24-bit unit_sw_version from the unit directory
                             // (0x000100 = DCAM 1.04, 0x000101, 0x000102, ...)
ULONG  CancelTimeout100ns;   // SET_CANCEL_TIMEOUT product; NEVER consumed (see 4.2)
PHOTONIC_DCAM_FEATURE Features[PHOTONIC_DCAM_FEATURE_COUNT]; // 19 entries, see 6.1
UINT32 FeatureHiMask;        // FEATURE_HI_INQ (CSR 0x404) as read at start, pruned
UINT32 FeatureLoMask;        // FEATURE_LO_INQ (CSR 0x408), pruned
```

### Population (device start, after CSR-base discovery)

Everything comes from the bus driver's `REQUEST_GET_CONFIGURATION_INFO`
IRB, issued twice (first call with the directory/leaf buffer pointers
NULL to learn the sizes, second call with pool buffers to fetch the
data). That IRB returns, in one shot: the first 0x14 bytes of the config
ROM (header + bus-info block), the unit directory, the unit-dependent
directory (absence is fatal for device start, `STATUS_UNSUCCESSFUL`), and
the vendor and model TEXTUAL_LEAF blocks (either may be absent, size 0).
The ROM layout is described in
[dcam-registers.md](dcam-registers.md#configuration-rom).

From those, populate (ROM data arrives big-endian; byte-swap each quadlet
to CPU order where noted):

1. `SerialNumber[0]` = byte-swapped bus-info-block quadlet 3 (the EUI-64
   high word); `SerialNumber[1]` = byte-swapped quadlet 4. These are ROM
   header offsets 0x0C and 0x10.
2. `VendorLeaf`/`VendorLeafLength` and `ModelLeaf`/`ModelLeafLength` =
   verbatim pool copies of the two leaves and their total byte lengths.
   No parsing at population time; the 12-byte TEXTUAL_LEAF header is
   skipped at read time (§4.3). A leaf allocation failure just forces
   that leaf's length to 0 (feature absent), not a start failure.
3. `DcamUnitSwVersion`: scan the unit directory quadlets (skip quadlet
   0): for the entry whose key byte is 0x13, take its 24-bit value
   byte-swapped to CPU order.
4. CSR base: already implemented in `Photonic1394DiscoverCsrBase`
   ([p1394.c](../photonic/p1394.c)), which reads the key-0x40 entry of
   the unit-dependent directory. Extend it (or a sibling) to capture
   items 1-3 in the same pass.

The fake device publishes the key-0x40 entry but **no textual leaves and
no EUI-64 the driver currently captures**. With the fake, the string
IOCTLs fail exactly as the original driver does against a leaf-less
camera (§4.3), and the serial words read 0. See §9.

The DCAM feature table population is specified in §6.1.

---

## 3. The vendor extended-command channel

`TRIGGER_SET`, `STROBE_SET` and `IMAGE_FLIP` do not use DCAM CSRs alone;
they send **vendor extended commands** as asynchronous block writes to a
fixed node address. The wire protocol (destination addresses, packet
layout, byte order, known command ids) is specified in
[dcam-registers.md](dcam-registers.md#vendor-extended-commands).

Driver-side requirements:

- Build the header and payload in CPU order, then swap every dword
  before the write. (A trailing `N % 4` byte tail would be copied
  unswapped, but all existing commands have dword-multiple payloads.)
- If a response length is given, block-read that many bytes from the
  response address after a successful write and dword-swap them back. No
  currently specified command requests a response.
- **No handshake**: no ack polling, no delay between write and read. The
  transport (existing `Photonic1394WriteBlock`/`ReadBlock`) makes a single
  attempt with a 5 s timeout and the cancel/abandon protocol of
  `PhotonicSubmitIrbEx`; a transaction rejected for a stale bus generation
  is refreshed and retried once, and a persistent timeout surfaces as the
  submit path's timeout status.
- **Errors**: removed device → `STATUS_DEVICE_REMOVED` without touching
  the bus; pool-allocation failure → `STATUS_INSUFFICIENT_RESOURCES`;
  transport errors propagate.

New primitive (dcam.c or a new vendor.c):

```c
//
// Send a vendor extended command: an async block write of
// {0x00016800, (CommandId << 16) | 0xF064, Payload...} to node address
// 0xFFFF:0x00000000, all dwords big-endian. ResponseLength != 0 reads
// the reply from 0xFFFF:0x00000008. Must run at PASSIVE_LEVEL.
//
NTSTATUS PhotonicVendorCommand(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                               _In_ ULONG CommandId,
                               _In_reads_bytes_(PayloadLength) const VOID *Payload,
                               _In_ ULONG PayloadLength,
                               _Out_writes_bytes_opt_(ResponseLength) PVOID Response,
                               _In_ ULONG ResponseLength);
```

The fake device does **not** model this channel at all (nothing handles
writes to address 0). See §9.

---

## 4. Identity / naming (ioctl/identity.c)

### 4.1 SERIAL_NUMBER

Buffer: `PHOTONIC_SERIAL_NUMBER_IN/OUT` `{Status, Index, Value}`.

1. If `Index >= 2`: `out->Status = PL_ERROR_INVALID_COUNT` (7). Do
   **not** set LastError, do **not** fail: `Information = sizeof(out)`,
   return `STATUS_SUCCESS`. (Replicate + comment: the error travels only
   in the buffer.)
2. Else `out->Value = RtlUlongByteSwap(Extension->SerialNumber[Index])`,
   `out->Status = PL_SUCCESS`, `Information = sizeof(out)`,
   `STATUS_SUCCESS`.

Note the double swap is intentional: population stores the EUI-64 words
CPU-ordered (§2), and this IOCTL byte-swaps them again on output, so the
DLL receives ROM byte order. No removed-device check (replicate).

### 4.2 SET_CANCEL_TIMEOUT

Buffer: `PHOTONIC_SET_CANCEL_TIMEOUT_IN` `{Status, Reserved, TimeoutMs}`.

`Extension->CancelTimeout100ns = TimeoutMs * 10000;` then the Status
echo per the §1 convention: written only when the caller supplied output
room (the DLL passes a zero output length); `STATUS_SUCCESS`. No
validation, no removed check.

**The stored value is dead**: the original driver initializes it to 0 at
device start and nothing ever reads it; the "cancel timeout" is stored
and forgotten. Replicate (store it) + comment; do not invent a consumer.

### 4.3 GET_VENDOR_NAME / GET_MODEL_NAME

Output: raw ANSI bytes (`PHOTONIC_GET_*_NAME_OUT`), not NUL-terminated by
contract (the leaf payload is NUL-padded to a quadlet boundary and those
pad bytes are copied out as part of the length).

Using vendor (`VendorLeaf`/`VendorLeafLength`) or model fields
respectively:

1. If leaf length == 0 (leaf absent / never captured): return
   `STATUS_NOT_IMPLEMENTED` (0xC0000002), `Information = 0`, no
   LastError. (Replicate: this is the original driver's "no such string"
   answer, odd status and all.)
2. `len = LeafLength - 12` (strip the 3-quadlet TEXTUAL_LEAF header).
3. If `Request->OutputLength < len`: return `STATUS_BUFFER_TOO_SMALL`,
   `Information = 0`, no LastError.
4. `RtlCopyMemory(out, Leaf + 12, len)`; `Information = len`;
   `STATUS_SUCCESS`.

The leaf bytes are copied verbatim; the caller gets the string in leaf
(big-endian ROM) order, which for ASCII text is natural order. No
removed check.

### 4.4 GET_DCAM_VERSION

Output: `PHOTONIC_GET_DCAM_VERSION_OUT`, one UINT32.

1. If `Request->OutputLength < 4`: `Request->Error =
   PL_ERROR_INVALID_COUNT` (7); return `STATUS_BUFFER_TOO_SMALL`. (The
   dispatch table's min-output check already guarantees ≥ 4; keep the
   in-handler check anyway to stay faithful and defensive.)
2. `out->Value = Extension->DcamUnitSwVersion`; `Information = 4`;
   `STATUS_SUCCESS`. No removed check.

ioctl.h fix (done): the field, once named `UnitDirCsrBase`, is now
`UnitSwVersion` and documented as unit_sw_version (the 0x000100/
0x000101/0x000102 family), §10.

### 4.5 GET_NAMES_LENGTH

Output: up to two UINT32s, written independently, gated by output room:

```c
Request->Information = 0;
if (Request->OutputLength >= 4) {
    out->VendorNameLen = VendorLeafLength ? VendorLeafLength - 12 : 0;
    Request->Information = 4;
    if (Request->OutputLength >= 8) {
        out->ModelNameLen = ModelLeafLength ? ModelLeafLength - 12 : 0;
        Request->Information = 8;
    }
}
return STATUS_SUCCESS;   // always; no LastError, no removed check
```

Lengths are the §4.3 copy lengths (header already deducted, quadlet
padding included).

### 4.6 SDK_VERSION_1 / SDK_VERSION_2

Change the stub's return code: `return STATUS_NOT_SUPPORTED;`
(0xC00000BB, not 0xC0000002). `Information = 0`, no LastError, buffer
untouched. That is the entire behavior.

---

## 5. INITIALIZE (ioctl/property.c)

Camera soft reset plus invalidation of the driver's cached format state:

1. Write `0x80000000` to DCAM CSR offset 0x000 (the DCAM INITIALIZE
   register). Use the existing `PhotonicDcamReset` if its behavior is
   exactly that write; otherwise `Photonic1394WriteRegister(Extension,
   0x000, 0x80000000)`.
2. In photonic-ng this step is a no-op: the driver caches no format state
   (every format query re-reads the camera, which is also why the
   already-implemented `PHOTONIC_IOCTL_INVALIDATE_FORMAT` handler is a
   no-op), so there is nothing to clear.
3. If the write failed: `Request->Error = PL_ERROR_HARDWARE` (0xE) and
   return the write's status. Else `STATUS_SUCCESS`.

No input is read, no output is written (`Information = 0`). No removed
check in the original driver; rely on the register write failing on a
removed device.

---

## 6. PROPERTY_GET / PROPERTY_SET (ioctl/property.c)

These back `PlGetFeature`/`PlSetFeature` and operate on the DCAM feature
CSRs (register layout: [dcam-registers.md](dcam-registers.md)).
Terminology: **feature id** = the wire value in
`PHOTONIC_PROPERTY_IN.FeatureId`; **feature index** `m` = the driver's
internal 0..0x12 index into the feature table.

### 6.1 Feature table and its population

```c
typedef struct _PHOTONIC_DCAM_FEATURE {
    UINT32 InqWord;       // feature INQ register as read at start (0 = absent)
    UINT32 InitialValue;  // value register as read at start (diagnostic; unused by handlers)
    UINT32 AbsCsrLow32;   // absolute-value CSR block address (low32; hi = 0xFFFF),
                          // valid when InqWord bit 30 set
} PHOTONIC_DCAM_FEATURE;
#define PHOTONIC_DCAM_FEATURE_COUNT 19
```

Population, at device start after CSR discovery (new
`PhotonicDcamBuildFeatureTable`):

1. Read CSR 0x404 → `FeatureHiMask` (read failure → 0); 0x408 →
   `FeatureLoMask` (failure → 0).
2. For each set bit of `FeatureHiMask`, MSB-first (bit 31 = first
   feature): INQ register = `0x500 + i*4`, feature index `m` = `i`
   (valid for 0x500..0x530, i.e. m 0..0xC). Read INQ → `InqWord`; read
   `INQ + 0x300` (the value register) → `InitialValue`. If `InqWord`
   bit 30 (absolute capable): read `INQ + 0x200` (the ABS CSR offset
   register) and store `AbsCsrLow32 = value * 4 + 0xF0000000`. Any read
   failure: clear bit i in the stored `FeatureHiMask` and leave the
   record zeroed.
3. Same loop for `FeatureLoMask` starting at register 0x580: index
   mapping 0x580..0x58C → m 0xD..0x10, 0x5C0..0x5C4 → m 0x11..0x12.
   Registers outside those windows (0x590..0x5BC and beyond 0x5C4) have
   **no valid index — skip the bit**. (Deviation + comment: the original
   driver computes an invalid index for such bits and would write out of
   bounds; the masks of real cameras never set them. Guarding is
   required, silently skipping keeps behavior identical for real masks.)

### 6.2 Feature-id → index mapping

```c
m = id;
if (id > 0x3)  m--;
if (id > 0xC)  m--;
if (id > 0xE)  m--;
if (id > 0xF)  m--;
// valid iff m <= 0x12
```

This intentionally maps several ids onto one index (shared register,
different field):

| Feature id | m | DCAM feature (value CSR) | Field accessed |
|---|---|---|---|
| 0 | 0 | BRIGHTNESS (0x800) | bits 0-11 |
| 1 | 1 | AUTO_EXPOSURE (0x804) | bits 0-11 |
| 2 | 2 | SHARPNESS (0x808) | bits 0-11 |
| 3 | 3 | WHITE_BALANCE (0x80C) | bits 12-23 (U/B) |
| 4 | 3 | WHITE_BALANCE (0x80C) | bits 0-11 (V/R) |
| 5 | 4 | HUE (0x810) | bits 0-11 |
| 6 | 5 | SATURATION (0x814) | bits 0-11 |
| 7 | 6 | GAMMA (0x818) | bits 0-11 |
| 8 | 7 | SHUTTER (0x81C) | bits 0-11 |
| 9 | 8 | GAIN (0x820) | bits 0-11 |
| 0xA | 9 | IRIS (0x824) | bits 0-11 |
| 0xB | 0xA | FOCUS (0x828) | bits 0-11 |
| 0xC | 0xB | TEMPERATURE (0x82C) | bits 0-11, current — **read-only** |
| 0xD | 0xB | TEMPERATURE (0x82C) | bits 12-23, target |
| 0xE | 0xC | TRIGGER (0x830) | bits 0-11, parameter |
| 0xF | 0xC | TRIGGER (0x830) | bit 24, polarity |
| 0x10 | 0xC | TRIGGER (0x830) | bits 16-19, mode |
| 0x11-0x16 | 0xD-0x12 | LO-block features | bits 0-11 (see bug below) |

Register-from-index (`PhotonicDcamFeatureRegister(m)`, returns the INQ
register; the value register is always `+0x300`):

```c
if (m < 0xD)      reg = 0x500 + m*4;
else if (m < 0x11) reg = 0x580 + (m - 0xC)*4;   // BUG: yields 0x584..0x590
else               reg = 0x5B4 + (m - 0xC)*4;   // BUG: yields 0x5C8..0x5CC
```

**Replicate + comment**: for m ≥ 0xD this disagrees with the population
mapping by one slot (population stores zoom under m=0xD from register
0x580, but Get/Set for m=0xD access 0x584, the next feature). The
original driver ships this off-by-one; the known client only uses the
0x500 block, so it is latent. Do not fix silently.

### 6.3 PROPERTY_GET

Input `{FeatureId, Flags, Value}`; the handler writes only offsets 4
(Flags) and 8 (Value): **offset 0 keeps the caller's FeatureId**; it is
not a Status field (ioctl.h comment fix, §10).

1. Map id → m. If `m > 0x12`: `Request->Error = PL_ERROR_INVALID_COUNT`
   (7); return `STATUS_UNSUCCESSFUL`; `Information = 0`.
2. If `Features[m].InqWord` bit 31 clear: return `STATUS_NOT_SUPPORTED`,
   `Information = 0`, **no LastError** (replicate).
3. `out->Flags = 0x80000000`. Read the value register
   (`FeatureRegister(m) + 0x300`) → `reg`. Failure → `Request->Error =
   PL_ERROR_HARDWARE`; return the read status; `Information = 0`.
4. If `reg` bit 30 (absolute mode active) clear, extract `out->Value`:
   - id 3 or 0xD: `(reg >> 12) & 0xFFF`
   - id 0x10: `(reg >> 16) & 0xF`
   - id 0xF: `(reg >> 24) & 1`
   - all other ids: `reg & 0xFFF`
5. Else (bit 30 set): `out->Flags |= 0x40000000`; read the 32-bit value
   at 1394 address `0xFFFF:(Features[m].AbsCsrLow32 + 8)` (the absolute
   VALUE register; IEEE-754 float per DCAM) into `out->Value`. Failure →
   `PL_ERROR_HARDWARE`, return status.
6. Flag pass-through from `reg`: for ids **not** in {0xE, 0xF, 0x10}:
   copy bit 24 (0x01000000, auto) and bit 26 (0x04000000, one-push) into
   `out->Flags` when set. For **all** ids: copy bit 25 (0x02000000,
   ON) when set.
7. `Information = sizeof(PHOTONIC_PROPERTY_OUT)`; `STATUS_SUCCESS`.

Flag semantics (DCAM): 0x80000000 present, 0x40000000 absolute-active,
0x04000000 one-push, 0x02000000 on/off, 0x01000000 auto. ioctl.h's
current comment assigns these differently; fix it (§10). No removed
check (the register read fails naturally).

### 6.4 PROPERTY_SET

Input `{FeatureId, Flags, Value}`; no output (`Information` only echoes
per the standard convention when room exists; the original driver writes
nothing on success beyond the echo).

1. Map id → m. If `m > 0x12` **or id == 0xC** (current temperature is
   read-only): `Request->Error = PL_ERROR_INVALID_COUNT` (7);
   `STATUS_UNSUCCESSFUL`.
2. If `Features[m].InqWord` bit 31 clear: `Request->Error =
   PL_ERROR_FEATURE_NOT_PRESENT` (0xC); return `STATUS_NOT_SUPPORTED`.
3. Build the register value from input `Flags`:
   - bit 31 (0x80000000) set → `regval |= 0x80000000`
   - bit 25 (0x02000000) set → `regval |= 0x02000000` (ON)
   - bit 24 (0x01000000) set → `regval |= 0x01000000` (auto)
   - bit 26 (0x04000000) set → `regval |= 0x10000000`
     (**replicate + comment**: one-push lands on bit 28, which is not a
     DCAM bit — original quirk)
4. Value packing (`cur` = a fresh read of the value register where
   noted; a failed read → `PL_ERROR_HARDWARE`, return status):
   - If input `Flags` bit 30 (0x40000000, absolute): `regval |=
     0x40000000` and **skip packing** (the numeric value goes to the
     absolute block in step 6).
   - Else if (auto requested) or (ON not requested): **skip packing** —
     the write carries flags only (turning a feature off or handing it
     to auto does not program a value).
   - Else pack by id:
     - 3: read `cur`; `regval |= ((Value & 0xFFF) << 12) | (cur & 0xFFF)`
     - 4: read `cur`; `regval |= (Value & 0xFFF) | (cur & 0xFFF000)`
     - 0xD: `regval |= (Value & 0xFFF) << 12` (no read)
     - 0xE: read `cur`; `regval |= (Value & 0xFFF) | (cur & 0x1FFF000)`
     - 0xF: read `cur`; `regval |= (cur & 0xFFFFFF)`; if `Value != 0`
       `regval |= 0x01000000`
     - 0x10: read `cur`; `regval |= ((Value & 0xF) << 16) | (cur &
       0x10F0000)` (**replicate + comment**: the mask preserves the old
       mode bits, so the new mode is OR-ed over them)
     - all other ids: `regval |= Value & 0xFFF` (no read)
5. Write `regval` to the value register. Failure → `PL_ERROR_HARDWARE`,
   return status.
6. If input `Flags` bit 30 set: write `Value` to 1394 address
   `0xFFFF:(Features[m].AbsCsrLow32 + 8)`. **Replicate + comment**: a
   failure here returns the error status but sets **no** LastError.
7. `STATUS_SUCCESS`.

---

## 7. Format extras and trigger/strobe

### 7.1 IMAGE_FLIP (ioctl/format.c)

Input `PHOTONIC_IMAGE_FLIP_IN` `{FlipHorizontal, FlipVertical}` (two
UINT32s). Send them, in input order, as the 8-byte payload of vendor
command 0xD (§3), no response. Failure → `Request->Error =
PL_ERROR_HARDWARE`, return the command status, `Information = 0`.
Success → `STATUS_SUCCESS` (status echo per convention).

### 7.2 GET_CHANNEL / GET_PACKET_SIZE (ioctl/format.c)

Both share one quirky shape — **replicate + comment**:

1. If `Extension->Removed`: set `Request->Error =
   PL_ERROR_DEVICE_NOT_FOUND` and **fall through**. The dispatcher
   commits `Request->Error` to `LastError` even though the IRP
   succeeds (§1), which is exactly the quirky shape required.
2. Write the value and return `STATUS_SUCCESS`, `Information = 4`, even
   on a removed device.

Values:

- **GET_CHANNEL**: the isochronous channel currently held by the capture
  engine; `0xFFFFFFFF` when none is allocated (the original driver keeps
  the field initialized to -1 and only assigns it while transmit-side
  resources are held). Map to the photonic-ng capture engine's channel
  field, with `0xFFFFFFFF` when `IoctlCapture`/capture resources are
  absent.
- **GET_PACKET_SIZE**: the isoch bytes-per-packet computed when the
  transmit-side resources were last acquired (the value validated
  against bus speed during prepare/start); 0 if that never happened.
  Expose the capture engine's computed bytes-per-packet in the device
  extension if it is not already reachable.

### 7.3 TRIGGER_SET (ioctl/trigger.c)

Input `PHOTONIC_TRIGGER_SET_IN` (five UINT32s, in dword order): `Enable`,
`Type`, `Polarity`, `Delay`, `Parameter`.

1. Build the DCAM TRIGGER_MODE value: `reg = 0x80000000; if (Type != 0)
   reg |= 0x01000000; if (Enable != 0) reg |= 0x02000000;` and write it
   to CSR 0x830. (**Replicate + comment**: bit 31 is written even though
   DCAM defines it as the read-only presence bit; the trigger-mode field
   bits 16-19 are always written 0.) Failure → step 5.
2. Vendor command 9, payload = `Delay` (dword 3). Failure → step 5.
3. Vendor command 10, payload = `Polarity` (dword 2). Failure → step 5.
4. Vendor command 11, payload = `Parameter` (dword 4). Success →
   `STATUS_SUCCESS`, status echo.
5. On any failure: `Request->Error = PL_ERROR_HARDWARE`, return the
   failing status, `Information = 0`. Short-circuit — later commands are
   not sent.

Note the payload order (9←Delay, 10←Polarity, 11←Parameter) does not
follow the input dword order; it is fixed by the wire contract.

### 7.4 STROBE_SET (ioctl/trigger.c)

Input `PHOTONIC_STROBE_SET_IN`: bytes `Enable` (offset 0), `Polarity`
(offset 4), `Mode` (offset 8), dwords `Delay` (offset 0xC), `Duration`
(offset 0x10). Build the 12-byte payload of vendor command 0xC:

| Payload offset | Content |
|---|---|
| 0 | 0x00 (fixed) |
| 1 | `Mode` byte |
| 2 | `Polarity` byte |
| 3 | `Enable` byte |
| 4-7 | `Delay` dword |
| 8-11 | `Duration` dword |

(That is the CPU-order memory layout; §3's per-dword swap applies on the
wire, so quadlet 0 transmits as `Enable, Polarity, Mode, 0x00`.) No
response. Failure → `Request->Error = PL_ERROR_HARDWARE`, return the
command status. Success → `STATUS_SUCCESS`, status echo.

---

## 8. MAILBOX_CMD_GET_ADDRESSES (ioctl/mailbox.c)

Sub-command 0x0000 of `PHOTONIC_IOCTL_MAILBOX`, output
`PHOTONIC_MAILBOX_GET_ADDRESSES_OUT` `{Command, SerialNumberLow,
SerialNumberHigh}`:

1. If `Request->OutputLength < 12`: return `STATUS_BUFFER_TOO_SMALL`, no
   LastError.
2. `out->SerialNumberLow = Extension->SerialNumber[0]`;
   `out->SerialNumberHigh = Extension->SerialNumber[1]` — **raw, no
   byte swap** (unlike SERIAL_NUMBER §4.1, which swaps; the two IOCTLs
   intentionally return different byte orders of the same words).
   `out->Command` is left as echoed input. `Information = 12`,
   `STATUS_SUCCESS`.

---

## 9. Fake-device gaps and test plan

Currently modeled and usable as-is: DCAM INITIALIZE register (0x000
reset), the feature INQ/value blocks (13 features with per-feature
ranges, value offset +0x300), TRIGGER_MODE at 0x830.

**Gaps to close (fake work items):**

1. **Vendor extended-command channel**: nothing handles block writes to
   address `0xFFFF:0x00000000`. Add a handler that validates the
   `{0x00016800, (cmd<<16)|0xF064}` header, logs command id + payload,
   stores the last-seen payload per command id (for test assertions),
   and ACKs. Model commands 9/0xA/0xB/0xC/0xD; keep unknown ids
   log-and-ACK.
2. **Config-ROM identity**: add vendor and model TEXTUAL_LEAF entries
   and ensure the bus-info block carries a stable EUI-64 the tests can
   predict. Until this lands, GET_VENDOR/MODEL_NAME must return
   0xC0000002 and GET_NAMES_LENGTH zeros against the fake; that path is
   itself a test.
3. Optional: absolute-value CSR blocks for one feature to exercise the
   PROPERTY absolute paths (bit 30 is currently never advertised by the
   fake's INQ word, so the relative paths are the testable ones).

**Tests (extend test/):** wrappers already exist for every IOCTL here.

- SDK_VERSION_1/2 → fail with the Win32 mapping of STATUS_NOT_SUPPORTED.
- SERIAL_NUMBER index 0/1 (byte-swap check against the fake's EUI-64),
  index 2 → success with `Status = 7`, GET_LAST_ERROR unchanged.
- MAILBOX GET_ADDRESSES: same words unswapped; short buffer → error.
- Names: absent-leaf behavior now; content + exact lengths (quadlet
  padding included) after fake item 2; short buffer → error with 0 bytes.
- GET_NAMES_LENGTH with output 0/4/8 bytes → Information 0/4/8.
- GET_DCAM_VERSION = fake's unit_sw_version.
- INITIALIZE: fake log shows 0x000 ← 0x80000000 and video state resets;
  driver-side cached format re-reads after (verify via PIXEL_FORMAT_GET).
- PROPERTY_GET/SET over every advertised feature: relative value
  read/write round-trip, ON/auto flag round-trip, auto-without-value and
  off-without-value writes, id 0xC set → error 7, unknown id → error 7,
  unadvertised feature → STATUS_NOT_SUPPORTED (Get: no LastError; Set:
  LastError 0xC). White-balance pair (ids 3/4) and temperature pair
  (0xC/0xD) field packing verified via mailbox register reads of
  0x80C/0x82C.
- TRIGGER_SET: fake log shows the 0x830 write with the expected bits,
  then vendor commands 9, 10, 11 with the right payloads, in order.
- STROBE_SET / IMAGE_FLIP: vendor command 0xC (12-byte layout above) /
  0xD (two dwords).
- GET_CHANNEL / GET_PACKET_SIZE: 0xFFFFFFFF / 0 before any prepare;
  real values while the IOCTL capture slot holds resources; values
  still returned (with LastError 3) after surprise removal.

---

## 10. ioctl.h corrections (done)

All six corrections below have landed in ioctl.h; the list stays as the
record of what changed and why.

1. `PHOTONIC_GET_DCAM_VERSION_OUT.UnitDirCsrBase`: the value is the unit
   directory's **unit_sw_version** (DCAM spec revision), not a CSR base.
2. `PHOTONIC_PROPERTY_OUT` offset 0 is **not** written by the driver (it
   still holds the caller's FeatureId); flags at offset 4 mean:
   0x80000000 present, 0x40000000 absolute, 0x04000000 one-push,
   0x02000000 on/off, 0x01000000 auto.
3. `PHOTONIC_PROPERTY_SET_IN.Flags` comment likewise (bit 26 = one-push
   → written to register bit 28 quirk; bit 25 = on/off; bit 24 = auto).
4. The per-id field comments in `PHOTONIC_PROPERTY_OUT` /
   `PHOTONIC_PROPERTY_SET_IN` name ids 13-16 Pan/Tilt/Optical
   Filter/Capture Size; per the §6.2 mapping they are the temperature
   target and the three trigger fields. Align them with the table.
5. `PHOTONIC_SERIAL_NUMBER` comment: note the words are the node EUI-64
   (bus-info-block quadlets 3-4), returned byte-swapped here and raw in
   MAILBOX GET_ADDRESSES.
6. TRIGGER_SET comment: document the 0x830 value actually written
   (0x80000000 | polarity 0x01000000 | enable 0x02000000) and the
   command↔field pairing of §7.3.

## 11. Suggested implementation order

1. §2 population (config-ROM identity) + §4 identity handlers — unblocks
   `PlInitialize`/`PlGetDeviceInfo`; testable against the fake
   immediately (absent-leaf paths) and fully after fake item 2.
2. §3 vendor-command primitive + §7 trigger/strobe/flip + fake item 1.
3. §6 feature table + PROPERTY_GET/SET (largest item; fake already
   models the registers).
4. §5 INITIALIZE, §7.2 channel/packet-size, §8 MAILBOX GET_ADDRESSES —
   small, independent.
5. §10 ioctl.h comment fixes alongside whichever handler touches them.
