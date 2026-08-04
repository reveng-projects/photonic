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
/// Zero-copy isochronous image acquisition for the Photonic minidriver.
///
/// Drives the 1394 bus driver's isochronous receive path to pull live frames
/// from the DCAM camera directly into the consumer's frame buffers -- the driver's
/// CPU never copies a pixel. The engine (the frame descriptor pool, each
/// descriptor's reusable IRP and the listen IRP) is allocated when the stream
/// opens and freed when it closes; the transition to KSSTATE_RUN programs the
/// camera for the negotiated mode (dcam.c), acquires an isochronous channel,
/// bandwidth and a non-circular resource handle from the bus driver, opens the
/// engine for reads, pumps the buffers the front-end holds ready, and enables
/// the camera only once the listen is confirmed -- so the first frame
/// the camera transmits is captured, not lost on the wire.
///
/// The zero-copy handoff is one buffer per read request, and the whole data path
/// runs at DISPATCH_LEVEL on pre-allocated IRPs: the engine's pump pulls the
/// next ready buffer from the front-end's acquire callback -- the memory
/// descriptor list (MDL) the Stream Class framework already built over the
/// read's frame buffer, together with an opaque cookie -- and attaches that MDL
/// to the bus as the DMA target with an asynchronous IRB. A frame larger than
/// the bus driver's per-buffer limit (queried at
/// bring-up; PHOTONIC_CAPTURE_MAX_CHUNK_BYTES is the fallback) is attached as
/// several packet-aligned chunks -- partial MDLs over the same consumer pages, so
/// the path stays zero-copy. When the
/// frame's DMA finishes the bus driver fires the descriptor
/// callback; the engine detaches the buffer asynchronously and invokes the
/// front-end's delivery callback, which completes the read, and the pump
/// attaches the next parked read right from the detach completion. On
/// KSSTATE_STOP the engine stops the camera, detaches every attached buffer
/// through the same asynchronous detach path, waits for the in-flight IRPs to
/// drain and releases the isochronous resources; the pool survives for the next
/// run.
///
/// The engine is split into two planes. The data plane (submit, attach and
/// detach completions, frame delivery) runs at DISPATCH_LEVEL under the
/// engine spinlock and issues only the per-descriptor attach and detach on
/// each descriptor's own IRP. Everything else -- start, stop, listen, cancel
/// resolution -- is a control transition, and every control transition runs
/// at PASSIVE_LEVEL with the device's InterfaceMutex held, so exactly one
/// context at a time changes the engine's control state. Completion routines
/// never issue listen or stop requests; when a transition is needed from
/// DISPATCH_LEVEL (a cancel, or a listen becoming possible after the first
/// attach), the trigger records one fact and queues the device's control
/// work item (PhotonicCaptureRequestWork), which performs the transition
/// synchronously under the mutex.

#ifndef PHOTONIC_CAPTURE_H
#define PHOTONIC_CAPTURE_H

#include "photonic.h"

/// Number of frame descriptors the engine keeps. One per frame that can be
/// outstanding (attached to the bus) at once; matched to the Stream Class proxy
/// allocator's small frame ring so every framework buffer can be attached
/// simultaneously.
#define PHOTONIC_CAPTURE_FRAME_COUNT 4

/// Fallback per-buffer size limit. The largest single isochronous buffer the bus
/// driver accepts is queried at bring-up (GET_HOST_DMA_CAPABILITIES, stored in
/// PHOTONIC_DEVICE_EXTENSION.MaxDmaBufferSize); a frame larger than that limit
/// is split into chunks of at most the limit (rounded down to whole packets)
/// and attached as one multi-descriptor request, and a frame within it -- always,
/// on a host without a meaningful limit -- goes to the bus as one buffer. The
/// chunks are partial MDLs over the consumer's frame buffer, so the DMA still
/// lands directly in the consumer's pages -- no copy. When the query fails this
/// constant stands in for the limit: the inbox 1394 bus driver rejects
/// ISOCH_ALLOCATE_RESOURCES with STATUS_INSUFFICIENT_RESOURCES once the
/// per-buffer size approaches 2 MB (observed on the test rig: 1,920,000-byte
/// frames work, 2,359,296-byte frames fail), and 1 MB keeps a comfortable
/// margin below that.
#define PHOTONIC_CAPTURE_MAX_CHUNK_BYTES (1024u * 1024u)

/// Upper bound on chunks per frame, sizing the per-descriptor ISOCH_DESCRIPTOR
/// and partial-MDL arrays. At the 1 MB fallback chunk size, 8 chunks cover
/// frames up to 8 MB; the largest mode the camera advertises (1600x1200 RGB24)
/// is 5,760,000 bytes = 6 chunks. PhotonicCapturePoolSetupChunks rejects a mode
/// whose frame needs more chunks than this (possible only if the host reports
/// an unusually small MaxDmaBufferSize).
#define PHOTONIC_CAPTURE_MAX_CHUNKS 8

/// Front-end delivery callback. Invoked by the engine at IRQL <= DISPATCH_LEVEL
/// when a read it owns is finished: from the detach completion once a frame's
/// buffer has been detached and its descriptor recycled, from the attach
/// completion when the attach failed, or directly from the pump when a buffer
/// it acquired cannot be attached. Cookie is the value the acquire callback
/// returned (the read SRB pointer, or the ring index); BytesUsed is the
/// captured frame size on success (the front-end caps it to the read buffer)
/// or zero; Status is STATUS_SUCCESS for a delivered frame or an
/// error/STATUS_CANCELLED for a dropped one. The callback fills the frame
/// metadata in place and completes the read; it must not touch the pixel MDL
/// (the frame is already there) and the engine touches neither after this
/// returns.
///
/// @param Context    Context value supplied to PhotonicCaptureCreate.
/// @param Cookie     Opaque cookie identifying the read (the SRB pointer).
/// @param BytesUsed  Captured frame size on success, zero on failure or cancel.
/// @param Status     STATUS_SUCCESS for a delivered frame, or an error/STATUS_CANCELLED.
typedef VOID (*PPHOTONIC_CAPTURE_DELIVER)(_In_ PVOID Context, _In_ ULONG_PTR Cookie, _In_ ULONG BytesUsed,
                                          _In_ NTSTATUS Status);

/// Front-end buffer source. Invoked by the engine's pump (PhotonicCapturePump)
/// at IRQL <= DISPATCH_LEVEL, with no engine lock held, each time a free
/// descriptor is available: return the next ready buffer -- the head of the
/// stream's parked reads, or the next queued ring slot in index order -- or
/// FALSE when nothing is ready. Returning TRUE transfers ownership of the
/// buffer to the engine: the read completes through the delivery callback,
/// whether the frame arrives, the attach fails, or the buffer cannot be
/// attached at all. Context is the same value passed for Deliver.
///
/// @param Context  Context value supplied to PhotonicCaptureCreate.
/// @param Cookie   Receives the opaque cookie identifying the buffer.
/// @param Mdl      Receives the consumer's locked MDL, the zero-copy DMA target.
/// @return TRUE when a buffer was produced, FALSE when none is ready.
typedef BOOLEAN (*PPHOTONIC_CAPTURE_ACQUIRE)(_In_ PVOID Context, _Out_ PULONG_PTR Cookie, _Out_ PMDL *Mdl);

/// Lifecycle state of one frame descriptor. State arbitrates ownership of the
/// descriptor between the attach completion, the frame-complete callback and
/// the teardown; membership in the free/pending list follows it. All
/// transitions run under PHOTONIC_CAPTURE.Lock.
typedef enum _PHOTONIC_FRAME_STATE {
    PhotonicFrameFree = 0,
    PhotonicFrameAttaching,               ///< attach IRP at the bus driver
    PhotonicFrameCompletedWhileAttaching, ///< frame DMA finished before the attach IRP completed
    PhotonicFramePending,                 ///< attached, awaiting its frame
    PhotonicFrameDetaching,               ///< detach IRP at the bus driver
    PhotonicFrameDetachFailed,            ///< detach failed: buffer still attached, read parked until resource free
} PHOTONIC_FRAME_STATE;

/// One frame descriptor: a control block for a single in-flight frame. The bus
/// ISOCH_DESCRIPTOR array MUST be the first member -- the bus driver writes results
/// back into it, so it must sit at a stable non-paged address. One frame is
/// described by PHOTONIC_CAPTURE.ChunkCount consecutive entries (one per chunk),
/// attached and detached as a single multi-descriptor request; entries beyond
/// ChunkCount are unused. Around it the block carries the list linkage, the engine
/// back-pointer, the front-end cookie, and a pre-allocated IRP + embedded IRB
/// reused for every attach and detach so the steady-state path allocates nothing.
typedef struct _PHOTONIC_CAPTURE_FRAME {
    ISOCH_DESCRIPTOR Isoch[PHOTONIC_CAPTURE_MAX_CHUNKS];

    /// Links the descriptor into FreeList or PendingList (never both). Guarded by
    /// PHOTONIC_CAPTURE.Lock.
    LIST_ENTRY ListEntry;

    struct _PHOTONIC_CAPTURE *Capture;
    ULONG_PTR Cookie;
    PHOTONIC_FRAME_STATE State;

    /// Reusable request packet + request block for this descriptor's attach and
    /// detach. Allocating at DPC time is not allowed, so both are built once. The
    /// attach and the detach never overlap on one descriptor: the detach begins only
    /// after the attach IRP has completed (PhotonicCapturePumpAttachComplete resolves the
    /// race where the frame finishes first).
    PIRP Irp;
    IRB Irb;

    /// Reusable partial MDLs, one per chunk, allocated by PhotonicCaptureStart when
    /// the mode's frame needs more than one chunk (NULL otherwise -- a single-chunk
    /// frame attaches the consumer's MDL directly). Each is rebuilt over the
    /// consumer's MDL by every submit, so the chunk DMA targets are the consumer's
    /// own pages (zero copy). Freed when capture stops.
    PMDL ChunkMdls[PHOTONIC_CAPTURE_MAX_CHUNKS];

    /// How the read this descriptor is carrying should complete: STATUS_SUCCESS for
    /// a delivered frame, STATUS_CANCELLED when torn down, or the failed
    /// ISOCH_DESCRIPTOR status for a frame the bus driver completed with an
    /// error. Read by the single completer once it wins the find-and-remove
    /// from the pending list.
    NTSTATUS CompletionStatus;
} PHOTONIC_CAPTURE_FRAME, *PPHOTONIC_CAPTURE_FRAME;

/// Isochronous capture engine for one stream. Allocated by PhotonicCaptureCreate at
/// SRB_OPEN_STREAM and freed by PhotonicCaptureDestroy at SRB_CLOSE_STREAM, so
/// PHOTONIC_STREAM_EXTENSION.Capture is stable for the life of the open stream; the
/// isochronous resources are held only between PhotonicCaptureStart and
/// PhotonicCaptureStop.
typedef struct _PHOTONIC_CAPTURE {
    PPHOTONIC_DEVICE_EXTENSION Extension;

    /// The front-end's callbacks with their shared context. The engine is
    /// interface-agnostic: it correlates frames by cookie, hands finished
    /// frames back through Deliver and pulls buffers through Acquire.
    PPHOTONIC_CAPTURE_DELIVER Deliver;
    PPHOTONIC_CAPTURE_ACQUIRE Acquire;
    PVOID CallbackContext;

    /// Single-pumper election for PhotonicCapturePump, guarded by Lock. The
    /// pump can be requested concurrently (a read arriving, a descriptor
    /// recycling, a start); one runner at a time keeps the front-end's buffer
    /// order equal to the attach order. A request that loses the election
    /// sets PumpAgain and the runner takes another lap instead.
    BOOLEAN PumpActive;
    BOOLEAN PumpAgain;

    /// Geometry the camera streams this mode with: BytesPerPacket is the
    /// isochronous payload per packet, PacketsPerFrame packets make one frame, and
    /// FrameBytes == BytesPerPacket * PacketsPerFrame is the size of one captured
    /// frame (reported as the delivered byte count).
    ULONG FrameBytes;
    ULONG BytesPerPacket;
    ULONG PacketsPerFrame;

    /// How each frame is presented to the bus driver: ChunkCount buffers of at most
    /// ChunkBytes each (a multiple of BytesPerPacket; the last chunk carries the
    /// remainder). ChunkCount is 1 and ChunkBytes == FrameBytes unless the frame
    /// exceeds the host's per-buffer DMA limit (Extension->MaxDmaBufferSize, with
    /// PHOTONIC_CAPTURE_MAX_CHUNK_BYTES as the fallback). Set by
    /// PhotonicCaptureStart before any resource allocation or attach uses it.
    ULONG ChunkBytes;
    ULONG ChunkCount;

    /// 1394 isochronous resources held while streaming, each with a flag tracking
    /// whether it has been acquired so teardown releases exactly what was taken.
    HANDLE BandwidthHandle;
    HANDLE ResourceHandle;
    ULONG Channel;
    BOOLEAN ChannelAllocated;
    BOOLEAN BandwidthAllocated;
    BOOLEAN ResourceAllocated;
    BOOLEAN Listening;
    BOOLEAN IsoEnabled;

    /// TRUE when the start deferred the camera enable: buffers were expected
    /// but none was attached yet, so setting ISO_EN would have transmitted
    /// the leading frames with nobody listening. The control work item
    /// enables the camera once the first attach has completed and the listen
    /// is active. Written and read only under InterfaceMutex; cleared by the
    /// stop.
    BOOLEAN EnableWanted;

    /// Consecutive control work item runs that left a wanted listen or a
    /// deferred camera enable unfinished. While the count stays under the
    /// work item's retry cap the run requeues itself, so a transient failure
    /// (a bus reset mid-transaction) recovers even when no further attach
    /// completion would queue another run. Written only under
    /// InterfaceMutex, reset by the start and by any run with nothing left
    /// to retry.
    ULONG ControlRetries;

    /// Set by the attach completion when a buffer has been attached (the bus
    /// driver rejects a listen on a resource with no attached buffer, so this
    /// is the earliest a listen can succeed). PhotonicCaptureEnsureListen
    /// waits on it briefly before issuing the synchronous listen. Reset while
    /// the engine is settled (at start and at the end of a quiesce), both
    /// under InterfaceMutex.
    KEVENT FirstAttachDone;

    /// Set (interlocked, from DISPATCH_LEVEL) when a cancelled read is held
    /// by the engine and the control work item must quiesce the stream to
    /// complete it. Consumed by the work item under InterfaceMutex.
    volatile LONG CancelPending;

    /// The descriptor pool and the two lists that partition it. Every descriptor is
    /// on at most one list at a time: a Detaching descriptor is on neither list
    /// while its detach IRP is in flight, and a quarantined one stays off both
    /// lists until the release recycles it. Lock guards both lists and the
    /// descriptor states, and everything under it runs at DISPATCH_LEVEL or
    /// below.
    KSPIN_LOCK Lock;
    PHOTONIC_CAPTURE_FRAME Frames[PHOTONIC_CAPTURE_FRAME_COUNT];
    LIST_ENTRY FreeList;
    LIST_ENTRY PendingList;

    /// Teardown coordination. Draining is TRUE whenever the engine is not running
    /// (set at create, cleared by Start once the resources are in hand, set again
    /// by the quiesce): the pump attaches nothing and an in-flight attach
    /// completion publishes its descriptor for the quiesce's claim loop, which
    /// alone detaches incomplete buffers. Every transition
    /// of Draining happens under Lock with InterfaceMutex held, and every
    /// teardown completes before the mutex is released, so outside a transition
    /// Draining == TRUE means the engine is settled idle: no attach or detach
    /// IRP is in flight and no pump can attach. A stop that could not finish
    /// its release may still leave a quarantined descriptor holding a buffer
    /// attached at the bus driver, or an unsubmitted resource free pending;
    /// both stop and start check for and retry that leftover state before
    /// touching the session's resources.
    ///
    /// InFlightIrps counts every attach and detach IRP currently at the bus
    /// driver. The invariant: it is incremented under Lock, paired with the
    /// transition that commits the IRP (descriptor onto the pending list for an
    /// attach, off it for a detach), so once the quiesce has walked the pending
    /// list under Lock the counter accounts for every IRP that could still touch
    /// the resource handle; the quiesce then waits on DrainDone for it to reach
    /// zero before the resources are released. An attach completion that
    /// immediately begins a detach keeps its count (net zero).
    BOOLEAN Draining;
    volatile LONG InFlightIrps;
    KEVENT DrainDone;
} PHOTONIC_CAPTURE, *PPHOTONIC_CAPTURE;

/// Allocate the capture engine for an opening stream: the descriptor pool and
/// each descriptor's reusable IRP. Deliver and Acquire are the front-end's
/// per-frame completion callback and buffer source, CallbackContext the
/// context passed to both. Sets Stream->Capture on success. The engine idles
/// (the pump attaches nothing) until PhotonicCaptureStart. Must run at
/// PASSIVE_LEVEL.
///
/// @param Extension       Device extension for the camera being opened.
/// @param Stream          Stream extension that will own the engine.
/// @param Deliver         Per-frame delivery callback invoked at IRQL <= DISPATCH_LEVEL.
/// @param Acquire         Buffer source invoked by the pump at IRQL <= DISPATCH_LEVEL.
/// @param CallbackContext Context value passed to Deliver and Acquire.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicCaptureCreate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                               _In_ PPHOTONIC_CAPTURE_DELIVER Deliver, _In_ PPHOTONIC_CAPTURE_ACQUIRE Acquire,
                               _In_ PVOID CallbackContext);

/// Free the engine at stream close (Stream->Capture set back to NULL). The stream
/// must already be stopped (PhotonicCaptureStop), so no descriptor or listen IRP is
/// at the bus driver. When a quarantined descriptor survived the stop (its
/// detach failed and the resource free never completed, so the bus driver may
/// still hold a DMA mapping into the engine's descriptor arrays and the
/// consumer's pages), the engine is deliberately leaked instead of freed and
/// FALSE is returned: the caller must then leak whatever the engine's
/// callbacks reference too. Freeing memory the bus driver may still write is
/// worse than the leak. Must run at PASSIVE_LEVEL.
///
/// @param Stream  Stream extension whose capture engine is freed.
/// @return TRUE when the engine was freed, FALSE when it was leaked.
BOOLEAN PhotonicCaptureDestroy(_In_ PPHOTONIC_STREAM_EXTENSION Stream);

/// TRUE when a descriptor sits in the detach-failed quarantine (its buffer
/// still attached with a live DMA mapping). Meaningful only on a settled
/// engine (after a stop), where every descriptor is Free or DetachFailed and
/// no state transition can race the scan.
///
/// @param Capture  Settled capture engine whose descriptor pool is scanned.
/// @return TRUE when at least one descriptor is quarantined.
BOOLEAN PhotonicCaptureHasQuarantinedFrames(_In_ PPHOTONIC_CAPTURE Capture);

/// TRUE when a quarantined descriptor (detach failed, buffer still attached
/// with a live DMA mapping) holds the given front-end cookie. Meaningful only
/// on a settled engine after a stop, where no state transition can race the
/// scan; the IOCTL unmap uses it to skip unlocking ring pages the bus driver
/// may still flush into.
///
/// @param Capture  Settled capture engine whose quarantine is scanned.
/// @param Cookie   Front-end cookie to look for (the ring index).
/// @return TRUE when a quarantined descriptor holds Cookie.
BOOLEAN PhotonicCaptureCookieQuarantined(_In_ PPHOTONIC_CAPTURE Capture, _In_ ULONG_PTR Cookie);

/// Start capturing for the stream: program the camera for Stream->Mode, acquire
/// the isochronous channel, bandwidth and resource handle, open the engine for
/// submits, attach the buffers the front-end holds ready, issue the listen and
/// enable the camera. The receive path is fully armed before the camera
/// transmits, so the very first frame lands in a buffer.
///
/// ExpectBuffers says the front-end supplies buffers to receive into (a
/// DirectShow run, a video-family IOCTL start), possibly only after this
/// routine returns: reads arrive asynchronously around the run transition and
/// the ring may be mapped after the start. When none is attached by the time
/// the camera would be enabled, the enable itself is deferred -- EnableWanted
/// is recorded and the control work item sets ISO_EN once the first attach
/// has completed and the listen is active. Nothing is lost by not
/// transmitting while nobody can receive, the caller never waits, and the
/// stream's first frame cannot be lost on the wire. Pass FALSE for a
/// transmit-only session (an imager start), where the camera must free-run
/// with nothing attached and no listen.
///
/// Publishes KSSTATE_RUN under the stream's PendingLock on every success
/// path, including a start that finds the engine already streaming, so no
/// front-end has to post-publish the running state (the counterpart of the
/// stop's KSSTATE_STOP publish). Returns an error, leaving the engine idle
/// and never publishing KSSTATE_RUN, if the stream has no negotiated mode or
/// the camera/bus could not be brought up. Must run at PASSIVE_LEVEL with
/// Extension->InterfaceMutex held.
///
/// @param Extension      Device extension for the camera to start streaming.
/// @param Stream         Stream extension carrying the negotiated mode and parked reads.
/// @param ExpectBuffers  TRUE when the front-end supplies receive buffers, so the
///                       camera enable may be deferred until the first is armed.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicCaptureStart(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                              _In_ BOOLEAN ExpectBuffers);

/// Stop capturing: publish KSSTATE_STOP under the stream's PendingLock on
/// every path, including a stop that finds the engine already settled or not
/// created (so a racing data request parks in time for the drain or fails
/// fast, and no caller has to pre-publish), stop the camera, quiesce the
/// engine (stop the isochronous context, detach every attached buffer
/// completing its read cancelled, wait for the in-flight attach and detach
/// IRPs to drain) and release the isochronous resources. A stop that finds
/// the engine already settled retries any release a previous stop could not
/// finish. The engine and its pool stay allocated for a later
/// restart. Idempotent and synchronous: on return no engine IRP references the
/// stream's buffers. Must run at PASSIVE_LEVEL with Extension->InterfaceMutex
/// held, which is what makes concurrent stops impossible.
///
/// @param Extension  Device extension for the camera to stop streaming.
/// @param Stream     Stream extension whose engine and parked reads are drained.
VOID PhotonicCaptureStop(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream);

/// The engine's buffer pump: while the engine is running, claim free
/// descriptors and fill them with buffers pulled from the front-end's acquire
/// callback (in the front-end's order), attaching each to the bus as the
/// zero-copy DMA target. Bounded to one lap of the descriptor pool per run; a
/// single-pumper election keeps concurrent requests from interleaving the
/// attach order, a losing request just makes the runner take another lap.
/// Call it whenever buffers may have become ready (a read parked, a ring
/// mapped or requeued); the engine calls it itself at start and on every
/// recycled descriptor. Non-blocking; safe at DISPATCH_LEVEL. A no-op while
/// the engine is idle, so parked buffers wait for the start to attach them.
///
/// A pump call is fire-and-forget: it may be coalesced into a concurrent
/// runner whose next lap covers it, so on return the buffers are not
/// necessarily attached yet. No caller may peek at the engine right after
/// pumping and act on what it sees; anything that depends on a buffer being
/// armed is driven from the attach completion (FirstAttachDone, the control
/// work item's listen and deferred camera enable).
///
/// @param Capture  Capture engine to pump.
VOID PhotonicCapturePump(_In_ PPHOTONIC_CAPTURE Capture);

/// Record that a read the engine holds (or is about to hold) has been
/// cancelled and queue the control work item to resolve it. The work item
/// quiesces the engine, which detaches and completes every in-flight read,
/// the cancelled one included, then resumes the stream. Safe at
/// DISPATCH_LEVEL.
///
/// @param Capture  Capture engine holding the cancelled read.
VOID PhotonicCaptureNotifyCancel(_In_ PPHOTONIC_CAPTURE Capture);

/// Queue the device's control work item. The work item acquires
/// InterfaceMutex and services whatever the engine needs (a pending cancel, a
/// listen that became possible). Requests are coalesced; queuing while the
/// device teardown has begun is refused via IrbRemoveLock, so
/// SRB_UNINITIALIZE_DEVICE waits out any queued run. Safe at DISPATCH_LEVEL.
///
/// @param Extension  Device extension whose work item is queued.
VOID PhotonicCaptureRequestWork(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Issue the isochronous listen if the engine is running, not yet listening,
/// and holds at least one submitted buffer. Waits briefly for the first
/// attach to complete (the bus driver rejects a listen with no attached
/// buffer), then sends the listen synchronously. A no-op in every other
/// state; a failed listen is retried by the control work item, which the
/// next attach completion queues and which requeues itself a bounded number
/// of times when no further attach completion is coming. Must run at
/// PASSIVE_LEVEL with Extension->InterfaceMutex held.
///
/// @param Capture  Capture engine to start listening on.
VOID PhotonicCaptureEnsureListen(_In_ PPHOTONIC_CAPTURE Capture);

#endif // PHOTONIC_CAPTURE_H
