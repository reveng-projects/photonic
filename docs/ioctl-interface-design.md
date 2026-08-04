# Imager and video IOCTL interface

The direct-buffer IOCTL protocol exposes **one capture session per device**
through two families of lifecycle verbs:

- the **video family** (`PREPARE` / `START` / `STOP` /
  `UNPREPARE_VIDEO`, plus `MAP`/`UNMAP_VIDEO_FRAME`,
  `GET_TRANSFER_INFO`, `REGISTER`/`UNREGISTER_EVENT`), and
- the **imager family** (`PREPARE` / `START` / `STOP` /
  `UNPREPARE_IMAGER`), plus the free-standing `SW_TRIGGER`.

Despite the names, these are **not two capture interfaces**. The imager is
not a still-capture mode and the video family is not only for streaming.
They are two *control surfaces over the same session*, split along the
FireWire bus:

- The **imager family controls the camera/transmit side**: isochronous
  bandwidth, the channel, and whether the camera transmits continuously
  (`ISO_EN`).
- The **video family controls the host/receive side** (receive resources,
  the mapped user frame ring, the channel listen, frame delivery and
  events) and its prepare/start verbs drive *both* sides, so a client
  using only the video family gets a complete session.

Both families operate on the **same capture slot**
(`Extension->IoctlCapture`); there is no per-family split of the slot, and
either unprepare tears the whole session down. A video prepare over an
existing slot is allowed only while the slot is stopped and only for the
handle that prepared it, and it releases and rebuilds the slot rather than
topping it up (§3).

Orthogonal to that split, the 4-byte value passed to either start verb
selects **continuous vs single-frame transmission**: non-zero enables
`ISO_EN`; zero leaves it off, and each frame is then individually armed
via `SW_TRIGGER` (the `ONE_SHOT` register) and exposed by an external
hardware trigger. The configurations the client actually uses:

| Use case | IOCTLs | Camera behavior |
|---|---|---|
| continuous streaming | video family, start value 1 | free-running transmission into the mapped ring |
| triggered snap | video family, start value 0, per shot: map, arm, fetch, unmap | one armed, externally triggered frame per shot |
| transmit-only free-run | imager family alone, start value 1 | continuous transmission, nobody listening (latent, see §2) |

Note the corollary: the triggered *snap* runs on the **video** family; the
imager family plays no part in it beyond the shared teardown.

This document specifies the session model, the client contract fixed by
the existing user-mode library, and the handler semantics. The conceptual
background (zero-copy capture, the shared capture engine) is in
[architecture.md](architecture.md); the camera-side
registers are in [dcam-registers.md](dcam-registers.md). Wire formats
(struct layouts, IOCTL codes, buffer sizes) are authoritative in
[photonic/ioctl.h](../photonic/ioctl.h).

---

## 1. Two halves, one session

A capture session on the wire decomposes into two independent halves:

- **Transmit half (imager)**: everything on the camera/bus side. IRM
  bandwidth, an isochronous channel number, the ISOCH_CHANNEL register
  (CSR 0x60C) programmed into the camera, and the ISO_EN bit (CSR 0x614,
  bit 31) that makes the camera transmit continuously.
- **Receive half (video/system)**: everything on the host side. OS
  isochronous receive resources, DMA descriptor buffers, the MDL-locked
  user frame ring, the channel listen, and the delivery/event machinery.

The IOCTL verbs map onto these halves:

| Verb | Imager family | Video family |
|---|---|---|
| prepare | transmit half only | **both** halves; a stopped slot the handle owns is released and rebuilt (§3) |
| start | transmit half only (`ISO_EN` when the start value ≠ 0; **no listen**, no ring pump) | both halves (listen + ring pump, `ISO_EN` when the start value ≠ 0) |
| stop | **both** halves (isoch stop + `ISO_EN` clear) | **both** halves, identical to the imager stop (deviation note in §5.3) |
| unprepare | release everything, free the slot | release everything, free the slot |

### One-shot vs continuous

The dword in the `START_VIDEO` / `START_IMAGER` input buffer selects the
transmission mode.

- **non-zero** → continuous: the start writes `ISO_EN` and the camera
  transmits at the programmed rate until stopped.
- **zero** → single-frame: `ISO_EN` is never written. Each frame is an
  individually **armed, externally triggered** acquisition driven through
  `SW_TRIGGER` (§4.2).

### Single-frame acquisitions are armed, then externally triggered

In single-frame mode every frame proceeds in three steps, only the first
of which involves the driver:

1. **Arm**: the driver writes the DCAM `ONE_SHOT` register (CSR 0x61C,
   bit 31). This does **not** capture anything. It instructs the camera
   to wait for its external trigger input, capture one frame when the
   trigger fires, transmit that frame isochronously, and stop by itself.
2. **Trigger (exposure)**: happens entirely **out of band**, outside the
   driver. The camera's trigger input is wired to a serial port's DTR
   line; user space arms the head for external triggering (head register
   `CAMREG_TRIGGER_MODE` = `CAMREG_TRIGGER_EXTERNAL`, written over the
   mailbox gate via `PHOTONIC_IOCTL_MAILBOX`, see
   [camera-head-registers.md](camera-head-registers.md)) and pulses DTR
   once per frame.
3. **Transmission**: the captured frame goes out on the wire as a single
   isochronous frame and the camera returns to idle, waiting for the next
   arm.

Two timing consequences shape the design: the receive path must be
listening **before** the arm (once armed, the pulse and the frame can
arrive at any moment), and the interval between arm and frame is
**unbounded** (the pulse may come seconds later, or never; the driver
imposes no timeout, completion timeouts belong to the client).

### Fake-device model

The fake device (fake-fw-dev) models this with one simplification:

- [trigger.c](../fake-fw-dev/trigger.c) watches the serial port
  (`PHOTONIC_TRIGGER_PORT`, default `/dev/ttyUSB0`) and counts DTR pulses
  into `trigger_pending`, but only while `CAMREG_TRIGGER_MODE` is
  external, like the real head. [i2c.c](../fake-fw-dev/i2c.c) reports
  `STATUS FRAME_READY` while `trigger_pending > 0`, and leaving external
  mode drops pending pulses (the abort path).
- [dcam.c](../fake-fw-dev/dcam.c) (`DCAM_REG_ONE_SHOT` handler) /
  [iso.c](../fake-fw-dev/iso.c) (`iso_oneshot`): a bit-31 write to 0x61C
  tears down any prior idle shot, queues a single frame and the iso
  completion handler stops the context once that frame has gone out,
  consuming one pending trigger per transmitted frame.
- **Simplification**: the fake transmits the frame immediately on the
  `ONE_SHOT` write instead of holding it until the trigger pulse arrives.
  The driver cannot tell the difference (it just receives the frame
  earlier than a real camera would deliver it), but tests must not assert
  arm→pulse→frame ordering against the fake's log. Closing this gap is a
  fake-device work item ([future-work.md](future-work.md)).

---

## 2. Client contract (fixed by the user-mode library)

The wire protocol is fixed: the only client is the 32-bit
`pixelinkapi.dll`. It keeps two per-handle state variables (video and
imager family), but the video-family calls advance **both**. That is why
the teardown sends `STOP_IMAGER`/`UNPREPARE_IMAGER` even though
`PREPARE_IMAGER`/`START_IMAGER` are **never sent** in practice: the imager
up-path exists in the DLL but is unreachable. All capture, streaming and
snap alike, runs on the video family.

### Continuous streaming

| Step | IOCTL | Notes |
|---|---|---|
| 1 | `PREPARE_VIDEO` | 8-byte input, FrameCount = **4** |
| 2 | `MAP_VIDEO_FRAME` | one map, 4-frame user ring |
| 3 | `START_VIDEO` | input dword = **1** → continuous, `ISO_EN` on (the dword is literally `TriggerMode != 2` in the DLL) |
| 4 | per fetch: `REGISTER_EVENT` (Type 0), wait, `GET_TRANSFER_INFO` | worker thread |
| 5 | teardown | see below |

### Triggered snap (single-frame)

The client does **not** map a ring up front and does **not** keep a
mapping across shots. In trigger mode (software trigger, mode 2):

| Step | Actor / IOCTL | Notes |
|---|---|---|
| 1 | `PREPARE_VIDEO` | 8-byte input, FrameCount = **1**; **no MAP at prepare time** |
| 2 | `START_VIDEO` | input dword = **0** → single-frame mode, no `ISO_EN` |
| 3 | *out of band:* mailbox write `TRIGGER_MODE = EXTERNAL` | puts the head in external-trigger mode (user space) |
| 4.1 | `MAP_VIDEO_FRAME` | **per shot**, count = 1, mapping the **caller's buffer** for this shot |
| 4.2 | `REGISTER_EVENT` | Type **0** (fires on the next delivered frame) |
| 4.3 | `SW_TRIGGER` | **arms** one acquisition (§4.2) |
| 4.4 | *out of band:* DTR pulse on the trigger COM port | the camera captures and transmits; the DLL waits on the event with its own timeout; on timeout it calls `UNREGISTER_EVENT` instead of fetching |
| 4.5 | `GET_TRANSFER_INFO` | index is always 0 (ring depth 1) |
| 4.6 | `UNMAP_VIDEO_FRAME` | ends the shot |
| 5 | repeat 4.1–4.6 per frame | **no stop/start between shots**: the per-shot MAP and UNMAP happen while the stream is running |
| 6 | teardown | see below |

Steps 3 and 4.4 never touch the driver: the trigger wiring is a separate
serial device owned by user space. The driver only ever sees the arm
request, then the frame arriving on the wire. (The application may also
pulse before the arm; the head holds the exposure and the arm then reads
it out. The driver is agnostic to that ordering.)

UNMAP-while-running is therefore not an error-recovery corner case: it is
the hot path of the snap loop, and it must leave the session running
(§6).

### Transmit-only (imager family alone)

The DLL contains senders for `PREPARE_IMAGER` (8-byte input) →
`START_IMAGER` (input dword = 1), giving a camera transmitting
continuously with no host listen. The receive half can be added only
after a stop: `PREPARE_VIDEO` over the running slot fails
`PL_ERROR_INVALID_STATE`, and after `STOP_IMAGER` a `PREPARE_VIDEO` from
the owning handle releases and rebuilds the slot with the receive half
included (§3). This path is **latent**: the DLL's imager state
machine is only ever driven downward, so no real client reaches it. The
handlers are part of the wire surface and are implemented per §5, but the
use case is untested by the real client.

### Teardown is redundant and error-tolerant

The teardown interleaves both families: `STOP_VIDEO` → `STOP_IMAGER` →
`UNMAP_VIDEO_FRAME` (streaming only; in snap mode nothing is mapped at
teardown time) → `UNPREPARE_VIDEO` → `UNPREPARE_IMAGER`. All five
statuses are ignored by the client, so trailing calls only need to fail
*gracefully*: error return, no side effects, no LastError.

### Other client facts the design respects

- `PlSetCurrentFrameRate` rebuilds the video session in place:
  `STOP_VIDEO` → `UNMAP` → `UNPREPARE_VIDEO` → set-frame-rate IOCTL →
  `PREPARE_VIDEO` → `MAP` → `START_VIDEO`, without touching the imager
  IOCTLs. `UNPREPARE_VIDEO` followed by a fresh `PREPARE_VIDEO` must
  therefore work.
- `REGISTER_EVENT` is only ever sent with Type 0; Type 1 is unexercised.
- After any failed capture IOCTL (outside teardown) the client fetches
  the error code via `GET_LAST_ERROR` (0x2223F8); the output-buffer
  Status echo is not how errors reach the client (§5.1).
- The trigger mode is locked before `PREPARE_VIDEO` (`PlSetTriggerMode`
  refuses while the video state is non-idle), so a session never changes
  mode between prepare and start.

Three consequences drive the whole design:

- All state-bearing IOCTLs (`MAP`/`UNMAP_VIDEO_FRAME`,
  `GET_TRANSFER_INFO`, `REGISTER`/`UNREGISTER_EVENT`, `SW_TRIGGER`, both
  start verbs, both stop/unprepare verbs) operate on **one shared slot**,
  whichever family created it. There is no per-family ownership; there is
  per-handle ownership: every state-bearing verb except the read-only
  `GET_TRANSFER_INFO` rejects a handle other than the one that prepared
  the slot with `STATUS_UNSUCCESSFUL` and no LastError, the same shape as
  the stop/unprepare no-slot failure (§5.4). The no-slot failures
  themselves differ per verb (§7): MAP reports `PL_ERROR_NOT_PREPARED`
  and start reports `PL_ERROR_NOT_STREAMING`.
- `SW_TRIGGER`'s only preconditions are that the slot exists and the
  caller owns it: the real client fires it against a *video*-prepared
  slot. Gating it on an imager-prepared slot would break every snap.
- Single-frame mode must key off the **start value**, because that is the
  only wire-visible signal the client sends (`START_VIDEO(0)`).

---

## 3. One shared capture slot

The capture slot is `PHOTONIC_IOCTL_CAPTURE`
([ioctl/ioctl_private.h](../photonic/ioctl/ioctl_private.h)): an embedded
stream extension, the mapped DMA ring (`Frames[]`, `FrameLock`, pump and
delivery bookkeeping) and the owning `FILE_OBJECT` for cleanup. One slot
per device, published as `Extension->IoctlCapture`.

### Prepare semantics

| Call | Slot free | Slot exists (stopped) | Slot exists (running) |
|---|---|---|---|
| `PREPARE_VIDEO` | create slot, resolve format, create the capture engine | **succeeds** for the handle that prepared the slot: release and rebuild (any other handle fails `PL_ERROR_INVALID_STATE`) | fail `PL_ERROR_INVALID_STATE` |
| `PREPARE_IMAGER` | create slot, resolve format, create the capture engine | fail `PL_ERROR_INVALID_STATE` | fail `PL_ERROR_INVALID_STATE` |

Both prepares resolve the camera's current format selection identically
(mode lookup, bpp, geometry, frame interval); the frame size later
validated by `MAP_VIDEO_FRAME` is the same regardless of which prepare
created the slot. The 8-byte `PREPARE_IMAGER` input is not read; the
Status echo is written only when the caller supplied room, as everywhere
else. Neither prepare carries a ring depth that matters: the ring is
whatever `MAP_VIDEO_FRAME` later maps.

In photonic-ng, the bus/OS resources (channel, bandwidth, isoch resource
handle) are acquired at **start**, not at prepare, on both paths. The
half-split above describes wire-visible behavior, not internal
acquisition order, which the client cannot observe.

### Exclusion with DirectShow

Two-way exclusion: `SRB_OPEN_STREAM` ([lifecycle.c](../photonic/stream/lifecycle.c))
rejects with `STATUS_DEVICE_BUSY` while `Extension->IoctlCapture != NULL`,
and both prepares reject with `PL_ERROR_INVALID_STATE` /
`STATUS_DEVICE_BUSY` while `Extension->ActiveCaptureStream != NULL` (the
DirectShow pin publishes it while open). There is **no** video↔imager
exclusion. All checks run at PASSIVE_LEVEL under the device's
`InterfaceMutex`: the hooked IOCTL dispatch and `SRB_OPEN_STREAM` take
the same mutex (the driver turns Stream Class synchronization off), which
is what makes the two-way check atomic.

### File layout

- [ioctl/frames.c](../photonic/ioctl/frames.c): everything
  family-independent. The ring pump, the delivery callback, map/unmap,
  `GET_TRANSFER_INFO`, `REGISTER_EVENT`/`UNREGISTER_EVENT`, the shared
  prepare/start/stop/release helpers.
- [ioctl/video.c](../photonic/ioctl/video.c) and
  [ioctl/imager.c](../photonic/ioctl/imager.c): thin per-family wrappers
  over the shared helpers.
- [ioctl/trigger.c](../photonic/ioctl/trigger.c): `PhotonicIoctlSwTrigger`
  (§4.2).

---

## 4. Single-frame mode in the capture engine

### 4.1 `OneShot` keyed off the start value

`PHOTONIC_STREAM_EXTENSION.OneShot` ([photonic.h](../photonic/photonic.h))
is TRUE when the stream was started in single-frame mode. It is set by
the shared start helper from the start value and is a property of the
*start*, not of the prepare: re-starting with a different value flips the
mode, matching the wire protocol.

`PhotonicCaptureStart` ([capture.c](../photonic/capture/capture.c)): when
`Stream->OneShot` is set, skip the `ISO_EN` enable and leave the engine's
`IsoEnabled` FALSE. Everything else (mode programming, resource
acquisition, deferred listen, submit path, chunking) is unchanged; the
receive side does not care whether frames arrive continuously or one at a
time.

Because `IsoEnabled` stays FALSE, `PhotonicCaptureStop` already skips the
`ISO_EN` clear. One addition in the stop path: when `Stream->OneShot` is
set and the device is not removed, write 0 to the `ONE_SHOT` register to
cancel an armed acquisition whose trigger has not fired yet. Otherwise a
later stray pulse would make the camera transmit into a channel whose
resources the driver has already released. (Per DCAM, writing 0 to
ONE_SHOT clears it; the fake treats a non-bit-31 write as a harmless
no-op.) *This is an addition relative to the original driver, which never
writes this register outside the trigger path; it is invisible to the
client and guards a real hazard of the arm-first model.*

Known gap: the stop path clears `ONE_SHOT` only when `Stream->OneShot` is
set, so an arm fired at a never-started or continuous-started slot is
never cancelled. Acceptable because the client only arms
single-frame-started sessions.

### 4.2 Arming a shot: `SW_TRIGGER`

`SW_TRIGGER` does **not** capture anything (§1): writing `ONE_SHOT`
instructs the camera to wait for its external trigger input, capture one
frame when the pulse arrives, transmit it, and stop. The pulse itself is
generated by user space over the serial port. The register write is
`PhotonicDcamOneShot` ([dcam.c](../photonic/dcam.c)).

`PhotonicIoctlSwTrigger` ([ioctl/trigger.c](../photonic/ioctl/trigger.c)):

1. Reject when removed (`PL_ERROR_DEVICE_NOT_FOUND`), the photonic-ng
   convention shared by all handlers.
2. Reject when `Extension->IoctlCapture == NULL` with
   `PL_ERROR_NOT_PREPARED` (0x0D), `STATUS_UNSUCCESSFUL`. That is the
   **only** state check: no stream-state, no mapped-frame, and no
   family/origin condition. The real client fires it against a
   video-prepared, video-started slot, and the original driver accepted
   the write in any slot state. An arm with no frames mapped simply loses
   the frame on the wire (the fake logs an unmatched transmission).
   Sequencing (exposure vs readout, one arm per completed frame) is the
   client's responsibility.
3. **Requeue the caller's consumed slots** (skipped unless the stream is
   running): in single-frame mode a delivered slot belongs to the caller
   and stays detached from the bus (§6). Requeuing on arm is a safety
   net: in the real snap loop the handback is the per-shot UNMAP/MAP, and
   the delivered slot is unmapped before the next arm, so this step
   normally finds nothing to requeue.
4. **Ensure the listen** (`PhotonicCaptureEnsureListen`, bounded by the
   same 1-second first-attach wait the capture start uses,
   trace-and-continue on timeout; returns immediately when nothing is
   attached yet or the engine already listens). This is a
   photonic-ng-internal addition, not client-visible protocol: the
   original driver issues its listen synchronously during start, while
   photonic-ng's first ring attach issues it asynchronously, and once the
   camera is armed the pulse (and the frame) can arrive at any moment, so
   the host end must be listening before the arm. In the snap loop the
   per-shot MAP's pump issues the listen, so this normally returns at
   once.
5. Write the arm; on failure set `PL_ERROR_HARDWARE` (0x0E).

`SW_TRIGGER` then returns; the frame arrives whenever user space fires
the trigger. The wait is unbounded and the driver imposes no timeout
(§1). The frame lands in the next ring slot (`NextSubmitIndex` order),
the delivery callback bumps `CurrentFrameIndex` / `TotalFrameCount`,
signals the registered events and parks the slot with the caller
(Delivered, detached from the bus). The client observes the shot exactly
as it observes a streamed frame.

The driver does not serialize back-to-back arms: the client waits for
the frame event before arming the next shot, matching the original
driver, which forwarded the register write unconditionally. Re-arming
before the previous shot triggered replaces it (the fake's `ONE_SHOT`
handler tears down the prior idle shot first; the trigger pulse then
serves the new arm).

---

## 5. Imager family handlers (ioctl/imager.c)

### 5.1 `PREPARE_IMAGER`

The shared prepare with the imager slot rules (§3): fails with
`PL_ERROR_INVALID_STATE` (0x17) whenever a slot already exists; the
imager prepare has **no** re-prepare path. Allocation failure →
`PL_ERROR_OUT_OF_MEMORY` (0x09); format-resolution failure →
`PL_ERROR_HARDWARE` (0x0E); resource failure → `PL_ERROR_NO_RESOURCES`
(0x16).

These error codes are LastError values: they reach the client through
`GET_LAST_ERROR`, not through the output-buffer Status echo, which only
ever carries 0 on success (and is written only when the caller supplied
room).

### 5.2 `START_IMAGER`

The shared start without the ring pump:

- No slot → `PL_ERROR_NOT_STREAMING` (0x1F), `STATUS_UNSUCCESSFUL` (the
  same code the video start uses in that state).
- Input dword ≠ 0 (the DLL always sends 1): continuous, the engine starts
  and enables `ISO_EN`. Zero: single-frame, `ISO_EN` stays off.
- **No ring pump**: this start drives the transmit half only. If frames
  are mapped they stay unattached until a video start pumps them; with
  nothing mapped, the deferred listen is never issued, so a continuous
  `START_IMAGER` produces a free-running camera nobody listens to,
  matching the transmit-only use case (§2).
- Start failure → `PL_ERROR_HARDWARE` (0x0E). Success →
  `StreamState = KSSTATE_RUN`.

### 5.3 `STOP_IMAGER`

Full stop of **both** halves: cancel attached buffers, drain, release
isochronous resources, clear `ISO_EN` when enabled,
`StreamState = KSSTATE_STOP`. The slot stays prepared, so a start can run
again without re-prepare. Idempotent while the slot exists; **fails**
(`STATUS_UNSUCCESSFUL`, no LastError) when it does not, and the client's
teardown relies on that failure being harmless. Runs on a
surprise-removed device (register writes skipped). An armed shot whose
trigger never fired, or whose frame is still in flight, is cancelled like
any in-flight frame (`STATUS_CANCELLED` recycle) and the arm itself is
cleared in the camera (§4.1).

### 5.4 `UNPREPARE_IMAGER`

Stop, unmap, destroy the engine and free the slot when the slot exists;
otherwise **fail** with `STATUS_UNSUCCESSFUL` and no LastError. Identical
behavior for `UNPREPARE_VIDEO`, so whichever unprepare runs second in the
teardown fails benignly. Both unprepares are owner-checked: a handle other
than the one that prepared the slot is rejected with the same
`STATUS_UNSUCCESSFUL`-and-no-LastError failure. This symmetry (either unprepare frees the whole
slot) is a photonic-ng design choice; the contract the client actually
imposes is only the §2 teardown (statuses ignored, trailing calls fail
gracefully) plus the frame-rate rebuild (`UNPREPARE_VIDEO` followed by a
fresh `PREPARE_VIDEO` must work). Both are satisfied.

---

## 6. Ring, transfer info and events

Nothing in frames.c depends on which family created the slot:

- **Mapping** (`MAP_VIDEO_FRAME`): validated against the prepared frame
  size (`PL_ERROR_BAD_FRAME_SIZE` on mismatch), count
  `1..PHOTONIC_IOCTL_MAX_VIDEO_FRAMES`. The streaming client maps a
  4-frame ring once; the snap client maps a single caller buffer per
  shot (§2). When mapped while running, the ring is pumped immediately,
  which also issues the deferred listen and re-attaches into the live
  session.
- **Unmapping** (`UNMAP_VIDEO_FRAME`): detaches and unlocks the mapped
  frames **without stopping the session**. In-flight frames are cancelled
  per-buffer; the isochronous resources, the channel listen (the engine's
  `Listening` state), the `ISO_EN` state and `StreamState` are
  untouched. A subsequent MAP re-attaches into the still-running session
  and pumps immediately. This is the hot path of the snap loop (§2):
  stopping here would leave shot 2 armed into a channel nobody listens
  on. Unmap with nothing mapped or nothing prepared is a no-op; the
  DLL's cleanup paths call it unconditionally.

  **Implementation gap**: the current handler still stops a running
  session before unmapping, which breaks every snap after the first. See
  [future-work.md](future-work.md).
- **Slot ownership** (`PHOTONIC_IOCTL_FRAME_STATE`): DirectShow-style. A
  slot is either the engine's (Queued, awaiting attach; InFlight,
  attached to the bus) or the caller's (Delivered). The guarantee that
  matters: a Delivered slot's pages are not written between delivery and
  the caller's UNMAP, not even by the teardown's detach flush of the bus
  driver's staging memory. In the snap loop the handback is the per-shot
  UNMAP/MAP; the requeue-on-arm of §4.2 remains as a safety net. The
  protocol has no per-frame verb in continuous mode, so there a delivered
  slot recycles straight back into the rotation, matching the original
  driver's free-running ring. Cancelled or failed frames were never
  handed over and return to Queued directly, so a stopped stream restarts
  without a re-map.
- **`GET_TRANSFER_INFO`**: fills `PHOTONIC_GET_TRANSFER_INFO_OUT`
  (ioctl.h): `Status`, `CurrentFrameIndex` (last delivered ring slot),
  `TotalFrameCount`. The client calls it once per shot or fetch, after
  its registered event fires, and uses the last-delivered index to
  address the mapped ring. The index must be the ring slot of the frame
  that completed, in `NextSubmitIndex` order; in the snap loop the ring
  depth is 1 and the index is always 0.
- **`REGISTER_EVENT`** (`PHOTONIC_REGISTER_EVENT_IN32`): requires the
  slot **and** `MappedFrameCount > 0`, else `PL_ERROR_NOT_STREAMING`
  (0x1F). `Type` must be 0 or 1, else `PL_ERROR_FORMAT_UNAVAILABLE`
  (0x20). Semantics by countdown: **Type 0** arms a countdown of 1, the
  event fires on the next delivered frame and the entry is retired. This
  is the client's per-shot completion signal, re-registered before every
  shot and before every fetch while streaming. **Type 1** arms a
  countdown of `MappedFrameCount`: fires once that many frames have been
  delivered. (Type 1 is unexercised by the known client.)
- **`UNREGISTER_EVENT`**: removes a registered entry by handle; the
  client only calls it on a shot timeout, so a Type-0 entry must be
  individually removable before it fires.

---

## 7. State machine (shared slot)

```
                PREPARE_VIDEO |                 START_VIDEO(v) |
                PREPARE_IMAGER                  START_IMAGER(v)
   Unprepared ───────────────▶ Prepared ─────────────────────▶ Running ──┐
       ▲                        ▲   │                            │       │ SW_TRIGGER
       │  UNPREPARE_VIDEO |     │   │ PREPARE_VIDEO              │       │ (v==0: arm one
       │  UNPREPARE_IMAGER      │   │ (re-prepare,               │       │  shot per write;
       └────────────────────────┘   │  video only,               │       │  slot merely has
                                    ▼  stopped only)             │       │  to exist)
                                 Prepared ◀──────────────────────┘ ◀─────┘
                                            STOP_VIDEO | STOP_IMAGER
```

- `MAP_VIDEO_FRAME`: Prepared or Running (ring empty).
  `UNMAP_VIDEO_FRAME`: any prepared state, running included (§6).
- Errors: prepare over an existing slot → `PL_ERROR_INVALID_STATE`
  (except the video stopped re-prepare); start without a slot →
  `PL_ERROR_NOT_STREAMING`; `SW_TRIGGER` without a slot →
  `PL_ERROR_NOT_PREPARED`; stop/unprepare without a slot →
  `STATUS_UNSUCCESSFUL`, no LastError; start failure →
  `PL_ERROR_HARDWARE`; prepare resource failure →
  `PL_ERROR_NO_RESOURCES`.
- Repeated *identical* starts and repeated stops while the slot exists are
  harmless (the engine start/stop is idempotent). A start whose value or
  family would change the delivery mode of a running session is rejected
  with `PL_ERROR_INVALID_STATE`, since the engine start is a no-op while
  streaming and could not honor the change.
- Every handler traces entry (`FuncEntry(TRACE_FLAG_IOCTL)`) and its
  parameters/state per the project's WPP convention.

## 8. Lifecycle and failure paths

- **Handle cleanup**: the prepare records the owning file object;
  `PhotonicIoctlCleanup` ([ioctl/ioctl.c](../photonic/ioctl/ioctl.c))
  releases the slot when the owning handle's last reference closes. A
  client that dies mid-snap leaves no locked pages, no isochronous
  resources and no armed one-shot (the release path runs the stop, which
  clears ONE_SHOT, §4.1).
- **Surprise removal**: dispatch.c stops the session; the slot
  deliberately stays published and is released later by the client's
  unprepare, the handle cleanup, or `SRB_UNINITIALIZE_DEVICE`. The stop
  path skips camera register writes on a removed device.
- **DirectShow**: independent front-end, related only through the
  exclusion check of §3.

## 9. Testing against the fake device

The fake device works as-is, with the §1 caveat: it transmits the frame
on the arm itself instead of holding it for the trigger pulse, so tests
can assert per-arm delivery but not arm→pulse→frame ordering until the
fake-fidelity work item lands. The test host drives the out-of-band
trigger by pulsing DTR on the COM port wired to the fake device's
`PHOTONIC_TRIGGER_PORT` (back-to-back SETDTR/CLRDTR). Test wrappers exist
for every IOCTL involved; the mailbox wrapper covers the head-register
accesses.

1. **Snap bring-up (the real client's sequence)**: `PREPARE_VIDEO` with
   FrameCount = 1, **no MAP**, then `START_VIDEO(0)`; verify **no
   `ISO_EN` write** in the fake log and the iso context created but idle.
2. **Full triggered snap**: mailbox write `TRIGGER_MODE = EXTERNAL`; then
   per shot: `MAP_VIDEO_FRAME(1)`, `REGISTER_EVENT(Type 0)`, `SW_TRIGGER`
   (arm), pulse DTR, wait on the event, `GET_TRANSFER_INFO`,
   `UNMAP_VIDEO_FRAME`. Verify exactly one frame delivered (totals 1,
   index 0), pixel content matches the fake's generated frame, and the
   fake log shows the pulse counted and the one-shot transmit followed by
   the context auto-stop.
3. **Repeat** the per-shot sequence N ≥ 3 times with the stream left
   running: N distinct deliveries, `TotalFrameCount` incrementing, index
   always 0, and **no stop/start between shots** in the log. The explicit
   regression assertion is that **shot 2 succeeds** (this is what the
   UNMAP implementation gap of §6 fails today).
4. **Trigger gating** (fake behavior, via the mailbox): pulse DTR while
   `TRIGGER_MODE` is free-run and verify the fake ignores it
   (`FRAME_READY` stays clear); switch out of external mode with a pulse
   pending and verify the pending exposure is dropped (abort path).
5. **Transmit-only path**: `PREPARE_IMAGER` → `START_IMAGER(1)`; verify
   `ISO_EN` **is** written, no listen/attach occurs, and the fake logs
   unmatched transmissions; `STOP_IMAGER` clears `ISO_EN`.
6. **Composition**: `PREPARE_IMAGER` → `PREPARE_VIDEO` from the same
   handle succeeds (the stopped slot is released and rebuilt); after
   `START_IMAGER` the same `PREPARE_VIDEO` fails 0x17 until a stop;
   `PREPARE_IMAGER` with any existing slot fails 0x17; `SW_TRIGGER` with
   no slot fails 0x0D; `SW_TRIGGER` on a video slot succeeds.
7. **Teardown in the client's order**: `STOP_VIDEO`, `STOP_IMAGER`,
   `UNMAP`, `UNPREPARE_VIDEO`, `UNPREPARE_IMAGER`, verifying the trailing
   imager calls fail with `STATUS_UNSUCCESSFUL` and no side effects; plus
   handle-close with the stream running and a shot armed (cleanup path,
   including the ONE_SHOT clear of §4.1).
8. **Exclusion**: with the IOCTL slot prepared (either family), a
   DirectShow pin open must fail, and vice versa.
