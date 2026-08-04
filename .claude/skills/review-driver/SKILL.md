---
name: review-driver
description: Review the photonic driver for bugs (crashes, leaks, races, hangs) and hardening against broken hardware
---

# Scope

The driver lives in `photonic/` (Stream Class front-end, capture engine, 1394 plumbing)
and `photonic/ioctl/` (direct-buffer IOCTL front-end). Before reviewing, read
`docs/architecture.md` for the design.

Review for bugs such as crashes, resource leaks, race conditions, and infinite loops.
Error codes must be checked. Loops must be bounded. Blocking waits must have a timeout
and should generally be avoided.

# Concurrency model

The driver sets `TurnOffSynchronization = TRUE`, so the class driver does not serialize
anything. Device SRBs, stream control SRBs, stream data SRBs, hooked IOCTLs, and the
`IRP_MJ_CLEANUP` hook can all run concurrently on different threads. Check that:

- Every shared pointer in the device or stream extension (capture slots, stream
  back-pointers, engine state) is published and torn down under a lock, not with a bare
  check-then-use. Look for TOCTOU windows between a NULL check and the later publish,
  especially in the DirectShow versus IOCTL mutual-exclusion checks.
- Teardown is synchronous for every caller. A second stopper that observes a stop
  already in progress must wait for it to finish, not return early while DMA is still
  draining.
- State transitions that decide ownership of a descriptor or slot happen under the
  relevant spinlock or fast mutex, and racing completion paths claim a descriptor
  exactly once.
- Frame counters and other state touched by concurrent deliveries use interlocked
  operations or run under a lock.

# Stream Class protocol

- Every SRB is completed exactly once, and the matching `ReadyForNext*Request`
  notification is issued exactly once, on every path including all error paths.
- SRB handlers must not block for long. Anything that waits on hardware belongs on an
  asynchronous path.
- Cancel must find its SRB in every state, including SRBs parked between the pending
  list and the capture engine. A cancelled SRB must not be lost or completed twice.
- Surprise removal can race any user-initiated stop or unprepare. Both paths must
  serialize on the same lock and tolerate running in either order.

# IRQL discipline

- Isoch completion callbacks run as DPCs at DISPATCH_LEVEL in an arbitrary context.
  They must not touch paged memory, allocate from paged pool, wait on dispatcher
  objects, or call anything passive-only. IRPs and IRBs used from a DPC must be
  pre-allocated.
- Every `KeAcquireSpinLock` has a matching release on every path, the saved IRQL is
  restored, and no function returns or completes an IRP with a lock held.
- Fast mutexes are acquired only at passive level, never from a completion routine.
- Check documented IRQL ceilings in function comments against what the code does.

# 1394 and isochronous DMA rules

- Prefer standard bus-driver IRB requests over hand-parsing the configuration ROM or
  reimplementing what the bus driver already provides.
- IRP and IRB ownership: memory handed to the bus driver (IRPs, IRBs, isoch
  descriptors, MDLs) must not be freed or reused until the bus driver completes the
  request. A timeout that abandons a request must not free memory still owned by an
  in-flight IRP. Reusing one pre-allocated IRP for attach and detach is only safe if
  the two can never overlap on the same descriptor.
- Detach discipline: stop the isoch context before detaching buffers whose DMA did not
  complete, and check the detach status. Freeing resources with buffers still attached
  lets `ISOCH_FREE_RESOURCES` flush descriptors into freed pages (memcpy bugcheck).
  Pages backing a buffer stay locked until the bus driver has confirmed the detach.
- Buffer size limit: the 1394ohci stack rejects isoch attaches larger than about
  2 MiB. Whole frames above that limit must be split into sub-buffer (chunk) attaches,
  and per-chunk accounting must reassemble exactly one completion per frame.
- Resource lifetime: isoch channel, bandwidth, and resource handles must be released
  on every error path, in the right order, and never overwritten while a previous
  allocation is still recorded (that leaks the first set permanently). A failed
  release-IRB allocation must not silently drop the underlying bus resources.
- Bus resets can happen at any time. Node IDs, the allocated channel, and bandwidth
  can all become stale. Check that reset notification is handled and that in-flight
  requests failing with reset-related status do not spin, leak, or double-free.

# Hardening against broken hardware

Treat every value read from the camera (DCAM registers, configuration ROM, Format 7
CSRs) as hostile input:

- All polling of camera registers must have a bounded retry count or timeout.
- Pointer or offset arithmetic derived from hardware input must be bounds-checked
  before use. Watch for unsigned wrap when subtracting base addresses and for 32-bit
  overflow when multiplying geometry fields (width, height, bytes per pixel).
- A register value that fails validation must abort the operation, not merely log a
  warning and proceed with the bad value.
- The driver must keep working against `fake-fw-dev`, which can simulate misbehaving
  hardware.

# User buffers and MDLs

- User-mode addresses are probed and locked inside `__try/__except`, at the right IRQL,
  with the correct access mode.
- `MmUnlockPages` and `IoFreeMdl` run only after the bus driver no longer references
  the MDL (see the detach discipline above).
- Process exit and handle cleanup (the `IRP_MJ_CLEANUP` hook) must unmap and unlock
  everything a crashed client left behind, without racing an in-progress stop.

# Error recovery

- Error recovery should be good but not excessive. It massively increases the driver complexity
  and at some point becomes overkill. It is OK to fail capture and report to user space,
  which will deal with it as necessary. The driver should only ensure basic
  correctness (no crashes, resource leaks, race conditions, infinite loops, etc.).
- Spot excessive error recovery code that is not needed for basic correctness and propose a simpler alternative.

# Design and code quality

- Check the general design and code quality against `docs/architecture.md`.
- Check that the code is readable and maintainable. Complex logic and synchronization
  should be avoided. Prefer one lock with a clear ownership story over several
  fine-grained ones.
- Flag any code that is hard to understand or maintain and propose a simpler
  alternative.
- Error paths must emit WPP traces with enough state to diagnose a failure from a
  trace log alone.
- Flag code duplication between the streaming and IOCTL front-ends that belongs in the
  shared capture engine.

# Reporting

- Verify every candidate finding against the actual call paths before reporting it.
  Report only findings you can support with a concrete interleaving or input.
- Give each finding a severity, the file and line, the failure mode, and a suggested
  fix. Merge overlapping findings into one root cause.

# Documentation and comments

- Verifiy that the documentation in the docs folder matches the code. Flag any discrepancies.
- Make sure that the code is properly commented. Comments must be concise and precise.
