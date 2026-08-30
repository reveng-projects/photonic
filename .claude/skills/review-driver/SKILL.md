---
name: review-driver
description: Review the photonic driver for bugs (crashes, leaks, races, hangs) and hardening against broken hardware
---

# Scope

The driver lives in `photonic/` (Stream Class front-end, capture engine, 1394 plumbing)
and `photonic/ioctl/` (direct-buffer IOCTL front-end). Before reviewing, read
`docs/architecture.md` for the design.

Review for bugs such as crashes, resource leaks, race conditions, and infinite loops.
Error codes must be checked. Loops must be bounded. Blocking waits must have a timeout,
or their unboundedness must be justified in a comment and the wait must log on every
period so a hang is diagnosable (acceptable only when timing out would free memory the
hardware can still write). Blocking waits should generally be avoided.

# Process

- Fan out parallel review agents, one per domain: concurrency and teardown, Stream
  Class protocol, isochronous DMA and IRQL, hardening against broken hardware, and
  documentation versus code. Each agent stays in its domain and verifies its own
  candidate findings against the call paths before reporting them.
- Give every agent the prior reports (`docs/bugs*.md`) to dedup against, and the duty
  to re-check previously claimed fixes at their cited locations (see the next
  section).
- While agents run, read the most implicated files yourself. Before publishing,
  independently verify every high and medium finding at its file and line. Do not
  relay an agent finding unverified. Two agents independently converging on the same
  bug is a confidence signal, not a duplicate to discard silently.
- Merge overlapping findings into one root cause before writing the report.

# Prior reports and open items

- Reconcile every finding of every previous report (`docs/bugs.md` and successors) to
  one of: fixed (verify the fix at its location), deferred (must appear in
  `docs/future-work.md`), or rejected with a recorded reason. A finding with none of
  these states has been lost.
- Re-check that prior fixes are complete, not just present.
- Treat prior "verified clean" sections as scope-narrowing, not immunity. Re-derive
  the most contended handoffs each round.

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
- For lock-free handoffs, the atomic operation is not the last access: audit every
  instruction both the winning and the losing side execute after the atomic. When one
  thread publishes a structure for another to find, the field the reader checks must
  be written last, and the comment must state the ordering the read depends on.
- Frame counters and other state touched by concurrent deliveries use interlocked
  operations or run under a lock.
- Functions establish their postconditions on every path (a stop routine publishes
  the stopped state on every exit, not just the running path), so no caller has to
  compensate. All per-session flags are cleared at session start; a flag set for one
  session must not carry meaning into the next.
- Start-up is ordered by events, not by delays or readiness checks: the data source
  is enabled only from the completion event that proves the receive side is ready.

# Stream Class protocol

- Every SRB is completed exactly once, and the matching `ReadyForNext*Request`
  notification is issued exactly once, on every path including all error paths.
- SRB handlers must not block for long. Anything that waits on hardware belongs on an
  asynchronous path.
- Cancel must find its SRB in every state, including SRBs parked between the pending
  list and the capture engine. A cancelled SRB must not be lost or completed twice.
- Surprise removal can race any user-initiated stop or unprepare. Both paths must
  serialize on the same lock and tolerate running in either order.
- Counters reported through the framework (frame counts, drop statistics) must count
  what the framework's documentation defines, not what the code path finds convenient,
  and must reset where the framework expects. Wrong counting makes downstream quality
  management report problems that never happened.

# IRQL discipline

- Isoch completion callbacks run as DPCs at DISPATCH_LEVEL in an arbitrary context.
  They must not touch paged memory, allocate from paged pool, wait on dispatcher
  objects, or call anything passive-only. IRPs and IRBs used from a DPC must be
  pre-allocated.
- An IRQL-safety argument must hold for the worst path of a call, not the common one.
  Releasing an object reference at DISPATCH_LEVEL is unsafe when it can be the last
  reference, because deletion runs at passive level only: such paths need the
  deferred-deletion variant.
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
- Follow every failure-handling policy through to process exit, device removal, and
  driver unload. The OS enforces rules at those points (locked pages charged to the
  process, pending I/O, remove locks) that turn what looks like an accepted leak into
  a bugcheck, such as PROCESS_HAS_LOCKED_PAGES when a process exits with pages still
  locked.

# Hardening against broken hardware

Treat every value read from the camera (DCAM registers, configuration ROM, Format 7
CSRs) as hostile input:

- All polling of camera registers must have a bounded retry count or timeout.
- Pointer or offset arithmetic derived from hardware input must be bounds-checked
  before use. Watch for unsigned wrap when subtracting base addresses and for 32-bit
  overflow when multiplying geometry fields (width, height, bytes per pixel). Doing
  the math in 64 bits is not enough when the result is cast back to 32 bits: check
  that the result fits before narrowing.
- Every value written into a hardware register field must fit that field's width. A
  wider value silently keeps only the low bits, programming the device with a
  different value than the driver logged, which produces a silent bug instead of an
  error.
- When splitting data into fixed-size units (packets, chunks), check divisibility at
  configure time. Rounding down where the device rounds up loses the tail of every
  transfer.
- Validate related device-reported fields against each other: a "current" value must
  lie within the reported "maximum". Neither report is trustworthy alone.
- A register value that fails validation must abort the operation, not merely log a
  warning and proceed with the bad value.
- A status value that is only logged is not checked. Look for status fields that
  appear in a trace call but never in a branch: one such case delivered DMA-failed
  frames to the application as good frames.
- Probing sequences that write device selection registers (mode enumeration) must
  save and restore the selection, or set a known state, on every exit path including
  errors. Finishing on the last probed value leaves the device in a state later
  queries cannot interpret.
- The driver must keep working against `fake-fw-dev`, which can simulate misbehaving
  hardware.

# Client input and IOCTL surface

- Client-supplied values get the same validate-and-reject treatment as device values.
  Never silently correct or ignore a mismatch between what the client declared and
  what the driver computed.
- With buffered I/O, every byte inside the reported output length must actually be
  written by the handler. Input and output share one system buffer, so a skipped
  field hands the caller leftover input, or uninitialized kernel memory when the
  input is short.
- A permission or ownership check must cover every operation that reaches the state
  it protects, applied through one shared helper. A check added to the two operations
  from an incident left four others reaching the same teardown unguarded.
- Validation must sit above every use of the value, including trace statements that
  dereference it. WPP evaluates trace arguments only when the flag is enabled, so a
  dereference inside a trace call crashes exactly in the diagnosing configuration.

# Loops and retries

- A guard against one loop shape does not cover the others. Code that prevents
  recursion can still revisit the same work item through an iterative wraparound.
  Enumerate every way control can return to the same item, and prefer checking
  whether the previous attempt took effect over capping iterations.
- Audit every comment that promises a retry or recovery by tracing which caller
  actually triggers the promised path. Retry hooks whose trigger condition is
  consumed by partial success never fire again.
- For any retry loop, ask what actually improves on the next attempt. If nothing
  improves, one attempt is the design. Count-based budgets also burn out in
  microseconds when the failure mode rejects quickly instead of timing out.

# Error recovery

- Error recovery should be good but not excessive. It massively increases the driver complexity
  and at some point becomes overkill. It is OK to fail capture and report to user space,
  which will deal with it as necessary. The driver should only ensure basic
  correctness (no crashes, resource leaks, race conditions, infinite loops, etc.).
- Spot excessive error recovery code that is not needed for basic correctness and propose a simpler alternative.
- Apply the same filter to candidate findings and their proposed fixes: do not
  recommend retry machinery, fallback searches, or tolerance code where failing the
  operation is acceptable. Record findings removed by this filter in the report (see
  Reporting) so future rounds do not rediscover them.

# Design and code quality

- Check the general design and code quality against `docs/architecture.md`.
- Check that the code is readable and maintainable. Complex logic and synchronization
  should be avoided. Prefer one lock with a clear ownership story over several
  fine-grained ones.
- When findings cluster in one region across rounds, say so: repeated point fixes in
  the same area are a design problem. Propose the structural change that removes the
  coordination instead of another patch, and prefer proposals that delete state and
  code over proposals that add them.
- Flag any code that is hard to understand or maintain and propose a simpler
  alternative.
- Error paths must emit WPP traces with enough state to diagnose a failure from a
  trace log alone.
- Flag code duplication between the streaming and IOCTL front-ends that belongs in the
  shared capture engine.

# Reporting

- Verify every candidate finding against the actual call paths before reporting it.
  Report only findings you can support with a concrete interleaving or input. A race
  report without a step-by-step schedule is a guess.
- Give each finding a severity, the file and line, the failure mode, and a suggested
  fix. Merge overlapping findings into one root cause. State reachability caveats
  plainly (reachable today, latent behind a contract, one refactor from live).
- Write the report to the next `docs/bugsN.md`: dated header, a status section
  updated as fixes land, numbered findings, and a suggested fix order.
- Include a section listing what was checked and found correct, so the next round
  narrows its scope instead of re-deriving everything. Include a section of rejected
  candidates with the reason for each rejection, and a section of findings filtered
  out as excessive error recovery.
- End with the reconciliation of prior reports: each earlier finding marked fixed,
  deferred (with its `docs/future-work.md` entry), or rejected.

# Documentation and comments

- Verify that the documentation in the docs folder matches the code. Flag any
  discrepancy, and state for each whether the code or the document is the authority
  (a design document for unimplemented features is the authority; elsewhere the code
  is).
- Grep the documentation and the test suite for symbols, file paths, and trace
  strings that the code no longer contains. Test assertions keyed to a deleted trace
  string fail silently.
- When a wrong statement is found, grep for its phrasing across all documents,
  headers, and comments: duplicated claims drift independently, and fixing one copy
  leaves the others asserting the old behavior.
- Make sure that the code is properly commented per the comment rules in CLAUDE.md.
  Comments must be concise and precise. Verify comment claims against the code:
  a comment that promises behavior must name a mechanism that exists, and constants
  with units in their comment deserve re-derivation.
