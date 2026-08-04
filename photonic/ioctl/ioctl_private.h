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

#pragma once

/// @file
/// Declarations shared by the Photonic IOCTL handler files in this folder.
/// The dispatcher (ioctl/ioctl.c) decodes each METHOD_BUFFERED request into a
/// PHOTONIC_IOCTL_REQUEST and calls the matching handler; the handlers live in
/// sibling files grouped by functional area. Nothing outside ioctl/ includes
/// this header: the driver-wide interface stays in photonic.h and the wire
/// contract in ioctl.h.
///
/// The includer must provide photonic.h and ioctl.h first (kernel types and
/// PL_RETURN_CODE).

/// Decoded METHOD_BUFFERED request handed to each Photonic IOCTL handler.
/// Buffer is the shared system buffer (input and output overlap); Information
/// is the number of bytes to return to the caller, preset to 0 by the
/// dispatcher. InputLength / OutputLength are the caller's actual lengths,
/// already validated by the dispatcher against the minimums in the dispatch
/// table, so a handler may access its documented structures without
/// re-checking. Error is the PL_RETURN_CODE a failing handler reports back to
/// pixelinkapi.dll: preset to PL_SUCCESS, and committed to
/// Extension->LastError by the dispatcher when the handler leaves it non-zero
/// (so PHOTONIC_IOCTL_GET_LAST_ERROR can return it). FileObject is the handle
/// the request arrived on; the prepare verbs record it as the owner of the
/// IOCTL capture slot so the cleanup hook can tear the slot down when that
/// handle closes.
typedef struct _PHOTONIC_IOCTL_REQUEST {
    PPHOTONIC_DEVICE_EXTENSION Extension;
    PFILE_OBJECT FileObject;
    PVOID Buffer;
    ULONG InputLength;
    ULONG OutputLength;
    ULONG_PTR Information;
    PL_RETURN_CODE Error;
} PHOTONIC_IOCTL_REQUEST, *PPHOTONIC_IOCTL_REQUEST;

typedef NTSTATUS PHOTONIC_IOCTL_HANDLER(_Inout_ PPHOTONIC_IOCTL_REQUEST Request);

/// Upper bound on the DMA ring depth a prepare may request and MAP_VIDEO_FRAME
/// may map. The DLL prepares small rings (a handful of frames); the bound only
/// keeps a corrupt caller from committing the driver to an absurd per-frame
/// state allocation in MAP_VIDEO_FRAME.
#define PHOTONIC_IOCTL_MAX_VIDEO_FRAMES 256

/// Ownership of one mapped DMA ring-buffer frame, DirectShow-style: a slot the
/// engine owns is either awaiting attach (Queued) or attached / awaiting
/// delivery (InFlight); a slot whose frame was delivered belongs to the caller
/// (Delivered) and is never re-attached until the caller hands it back --
/// keeping the bus away from pixels the caller is still reading (a detach at
/// teardown flushes the bus driver's staging memory into whatever pages are
/// still attached). The hand-back is implicit in the existing protocol: the
/// next SW_TRIGGER (the client arms only after consuming the previous shot)
/// or a video start (the client restarts the stream over the whole ring)
/// requeues every Delivered slot. A cancelled or failed frame was never handed
/// to the caller, so it returns to Queued directly.
typedef enum _PHOTONIC_IOCTL_FRAME_STATE {
    PhotonicIoctlFrameQueued = 0, ///< Engine-owned, eligible for the next attach.
    PhotonicIoctlFrameInFlight,   ///< Claimed by the pump / attached to the bus.
    PhotonicIoctlFrameDelivered,  ///< Delivered; caller-owned until requeued.
} PHOTONIC_IOCTL_FRAME_STATE;

/// One mapped DMA ring-buffer frame: the caller's frame buffer, probed and
/// locked by MAP_VIDEO_FRAME, and its ownership state. Guarded by
/// PHOTONIC_IOCTL_CAPTURE.FrameLock.
typedef struct _PHOTONIC_IOCTL_FRAME {
    PMDL Mdl;                         ///< MDL describing the locked user-mode frame buffer.
    PHOTONIC_IOCTL_FRAME_STATE State; ///< Current ownership state.
} PHOTONIC_IOCTL_FRAME;

/// Capture slot of the Photonic IOCTL interface, prepared by
/// PHOTONIC_IOCTL_PREPARE_VIDEO or PHOTONIC_IOCTL_PREPARE_IMAGER and released
/// by either unprepare or device teardown (Extension->IoctlCapture). One slot
/// per device, shared by both IOCTL families: the video verbs drive both the
/// transmit (camera) and receive (host) halves of the session, the imager
/// verbs the transmit half only, and all state-bearing IOCTLs (map/unmap,
/// transfer info, events, SW_TRIGGER, both stops and unprepares) operate on
/// this slot whichever family created it. It embeds the same per-stream state
/// the DirectShow pin allocates at SRB_OPEN_STREAM, so both interfaces drive
/// the one 1394 capture engine (capture/) unchanged -- the engine neither
/// knows nor cares which front-end owns the stream extension it captures for.
/// Non-paged: the engine touches the stream extension at DISPATCH_LEVEL.
///
/// Where the DirectShow pin feeds the engine one framework-built MDL per read,
/// this front-end feeds it the mapped ring: the pump attaches the ring frames
/// in index order (as many as the engine's descriptor pool takes). Slot
/// ownership follows the DirectShow model (PHOTONIC_IOCTL_FRAME_STATE above):
/// in single-frame mode a delivered slot stays with the caller until the next
/// SW_TRIGGER or start requeues it; in continuous mode there is no per-frame
/// verb in the protocol, so a delivered slot recycles straight back into the
/// rotation and frames land round-robin for as long as the stream runs.
typedef struct _PHOTONIC_IOCTL_CAPTURE {
    PHOTONIC_STREAM_EXTENSION Stream;

    /// DMA ring depth requested by PREPARE_VIDEO (0 for an imager-prepared
    /// slot, whose 8-byte input carries no frame count). Informational: the
    /// ring the engine streams into is whatever MAP_VIDEO_FRAME later maps.
    ULONG FrameCount;

    /// The handle the prepare arrived on, owning this slot. Compared, not
    /// dereferenced: when IRP_MJ_CLEANUP carries this file object -- its last
    /// handle closed -- PhotonicIoctlCleanup releases the slot, so a client
    /// that dies without stop / unprepare cannot leave the camera streaming
    /// and the isochronous resources held forever.
    PFILE_OBJECT FileObject;

    /// The mapped DMA ring: MappedFrameCount frames locked by MAP_VIDEO_FRAME
    /// (0 while nothing is mapped), NextSubmitIndex the ring slot the pump
    /// attaches next. CurrentFrameIndex / TotalFrameCount are the delivery
    /// bookkeeping GET_TRANSFER_INFO reports: the slot of the last delivered
    /// frame ((ULONG)-1 until the first) and the running delivered total.
    /// FrameLock guards all of it together with the per-frame ownership
    /// states; the delivery callback takes it at DISPATCH_LEVEL.
    KSPIN_LOCK FrameLock;
    PHOTONIC_IOCTL_FRAME Frames[PHOTONIC_IOCTL_MAX_VIDEO_FRAMES];
    ULONG MappedFrameCount;
    ULONG NextSubmitIndex;
    ULONG CurrentFrameIndex;
    ULONG TotalFrameCount;

    /// Whether the engine's pump callback feeds the ring. Set by a video
    /// start (the receive half is in play: the ring must be attached before
    /// the engine enables the camera, so no leading frame is lost), cleared
    /// by an imager start (transmit half only: nothing is attached, the
    /// deferred listen is never issued and the camera free-runs with nobody
    /// listening).
    BOOLEAN PumpRingOnStart;

    /// Events registered by PHOTONIC_IOCTL_REGISTER_EVENT
    /// (PHOTONIC_IOCTL_EVENT entries, frames.c), each holding a referenced
    /// user event and a frame countdown; the delivery callback decrements the
    /// countdowns at DISPATCH_LEVEL and signals / retires an entry that
    /// reaches zero. EventLock guards the list.
    KSPIN_LOCK EventLock;
    LIST_ENTRY EventList;
} PHOTONIC_IOCTL_CAPTURE, *PPHOTONIC_IOCTL_CAPTURE;

/// identity.c -- identity / version / device naming.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSdkVersion;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSerialNumber;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSetCancelTimeout;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetVendorName;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetModelName;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetDcamVersion;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetNamesLength;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetLastError;

/// format.c -- sub-window (ROI) / pixel format / channel.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSubwindowGet;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSubwindowSet;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetPixelFormat;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSetPixelFormat;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlImageFlip;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlInvalidateCurrentFormat;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetChannel;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetPacketSize;

/// video.c -- thin wrappers for the video-family lifecycle verbs (prepare /
/// start / stop / unprepare), which drive both halves of the capture session
/// through the shared helpers in frames.c.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlStartVideo;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlStopVideo;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlPrepareVideo;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlUnprepareVideo;

/// frames.c -- everything family-independent about the shared capture slot:
/// the mapped DMA ring and its pump, transfer info and completion events, and
/// the common prepare/start/stop/unprepare machinery both families wrap.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlMapVideoFrame;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlUnmapVideoFrame;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetTransferInfo;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlRegisterEvent;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlUnregisterEvent;

/// Return every Delivered ring slot to the pump and attach as many queued
/// slots as the engine takes. The caller-owned-slot hand-back of the
/// single-frame flow: SW_TRIGGER calls it before arming the camera (the arm
/// is the client's statement that the previous frame has been consumed) and
/// the video start calls it in place of the bare pump (a start reclaims the
/// whole ring for the new stream). Safe at DISPATCH_LEVEL; a no-op when
/// nothing is mapped or no slot is Delivered.
///
/// @param Capture  IOCTL capture slot whose delivered frames are requeued.
VOID PhotonicIoctlFramesRequeue(_In_ PPHOTONIC_IOCTL_CAPTURE Capture);

/// Write the PL_SUCCESS status echo into the shared buffer, but only when the
/// caller supplied room for it (pixelinkapi.dll passes a zero output length
/// on most verbs). Handlers whose output begins with a PL_RETURN_CODE Status
/// field call this instead of writing the echo inline.
///
/// @param Request  Decoded IOCTL request.
VOID PhotonicIoctlEchoStatus(_Inout_ PPHOTONIC_IOCTL_REQUEST Request);

/// Shared prepare: removed-device check, DirectShow exclusion, slot-state
/// checks, slot allocation, current-format resolution, PhotonicCaptureCreate,
/// and publish Extension->IoctlCapture.
///
/// @param Request         Decoded IOCTL request.
/// @param FrameCount      Ring depth the video prepare requested (already
///                        validated); 0 for the imager prepare.
/// @param AllowReprepare  TRUE to release and rebuild a stopped slot (video
///                        rule); FALSE to fail whenever a slot exists
///                        (imager rule).
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlCapturePrepare(_Inout_ PPHOTONIC_IOCTL_REQUEST Request, _In_ ULONG FrameCount,
                                     _In_ BOOLEAN AllowReprepare);

/// Shared start: validates the slot, sets Stream.OneShot from StartValue,
/// calls PhotonicCaptureStart, and advances StreamState to KSSTATE_RUN.
///
/// @param Request     Decoded IOCTL request.
/// @param StartValue  Zero selects single-frame mode (no ISO_EN, each
///                    acquisition armed through SW_TRIGGER); non-zero enables
///                    continuous transmission.
/// @param PumpRing    TRUE for START_VIDEO (attach the mapped ring, issuing
///                    the deferred listen); FALSE for START_IMAGER (transmit
///                    half only, no listen).
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicIoctlCaptureStart(_Inout_ PPHOTONIC_IOCTL_REQUEST Request, _In_ ULONG StartValue,
                                   _In_ BOOLEAN PumpRing);

/// Stop the capture session while keeping the slot prepared, so it can be
/// restarted without a re-prepare. Either family's stop acts on the whole
/// session. Fails with STATUS_UNSUCCESSFUL and no LastError when no slot
/// exists.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or STATUS_UNSUCCESSFUL if no slot exists.
NTSTATUS PhotonicIoctlCaptureStopRequest(_Inout_ PPHOTONIC_IOCTL_REQUEST Request);

/// Release the capture slot (stop, unmap, engine destroy) and free it. Either
/// family's unprepare acts on the whole session. Fails with
/// STATUS_UNSUCCESSFUL and no LastError when no slot exists.
///
/// @param Request  Decoded IOCTL request.
/// @return STATUS_SUCCESS on success, or STATUS_UNSUCCESSFUL if no slot exists.
NTSTATUS PhotonicIoctlCaptureUnprepareRequest(_Inout_ PPHOTONIC_IOCTL_REQUEST Request);

/// Stop the IOCTL-path capture (releasing the isochronous resources) while
/// keeping the slot for a later restart or unprepare. Idempotent; must run at
/// PASSIVE_LEVEL with Extension->InterfaceMutex held.
///
/// @param Extension  Device extension.
VOID PhotonicIoctlCaptureStop(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Return TRUE when FileObject is the handle that prepared the IOCTL capture
/// slot. The cleanup hook uses this to release the slot when that handle's
/// last reference closes.
///
/// @param Extension   Device extension.
/// @param FileObject  File object to test.
/// @return TRUE if FileObject owns the capture slot, FALSE otherwise.
BOOLEAN PhotonicIoctlCaptureIsOwner(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PFILE_OBJECT FileObject);

/// Owner gate for the state-bearing verbs: TRUE when the request arrived on
/// the handle that prepared the capture slot, FALSE (with a warning trace)
/// otherwise. The device allows multiple opens, so without this gate a second
/// handle could stop, unmap or remap another client's live session, the very
/// damage the unprepare owner check exists to prevent. A rejected caller gets
/// STATUS_UNSUCCESSFUL and no LastError, like the no-slot case, so a client's
/// redundant teardown calls degrade gracefully. The caller must have checked
/// that the slot exists.
///
/// @param Request  Decoded IOCTL request naming the calling file object.
/// @param Verb     Verb name for the warning trace.
/// @return TRUE when the calling handle owns the capture slot.
BOOLEAN PhotonicIoctlCaptureCheckOwner(_In_ PPHOTONIC_IOCTL_REQUEST Request, _In_ PCSTR Verb);

/// imager.c -- thin wrappers for the imager-family lifecycle verbs, which
/// drive the transmit (camera) half of the capture session through the shared
/// helpers in frames.c.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlPrepareImager;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlUnprepareImager;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlStartImager;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlStopImager;

/// framerate.c -- frame rate enumeration / selection.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlEnumFrameRate;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSetFrameRate;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlGetFrameRate;

/// trigger.c -- trigger / strobe.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlTriggerSet;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlStrobeSet;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlSwTrigger;

/// property.c -- initialize / property (feature) get-set.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlInitialize;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlPropertyGet;
PHOTONIC_IOCTL_HANDLER PhotonicIoctlPropertySet;

/// mailbox.c -- mailbox / external I2C register access.
PHOTONIC_IOCTL_HANDLER PhotonicIoctlMailbox;
