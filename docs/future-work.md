# Future work

Known gaps between the documented design and the current implementation,
plus planned work that has a spec but no code yet. Each item links to the
document that specifies the required behavior.

## Driver: capture session fixes

These are deviations from the client contract specified in
[ioctl-interface-design.md](ioctl-interface-design.md); the first one
is user-visible and breaks the client's snap loop.

1. **UNMAP_VIDEO_FRAME must not stop a running session** (critical).
   `PhotonicIoctlUnmapVideoFrame`
   ([ioctl/frames.c](../photonic/ioctl/frames.c)) currently calls
   `PhotonicIoctlCaptureStop` before unmapping. Walking the snap loop
   through that: shot 1 works; its UNMAP releases the isochronous
   resources and stops the stream; shot 2's MAP no longer attaches,
   `SW_TRIGGER` skips the requeue and the listen (the engine is
   draining), and arms the camera into a channel nobody listens on, so
   the fetch times out. **Every snap after the first fails.** Continuous
   streaming is unaffected only because the client unmaps after
   `STOP_VIDEO` there.

   Required behavior (ioctl-interface-design.md §6): cancel and detach
   the mapped frames per-buffer, leave the isochronous resources, the
   channel listen (the engine's `Listening` state), `IsoEnabled`,
   `OneShot` and `StreamState` untouched, and reset the ring bookkeeping
   (`MappedFrameCount`, `NextSubmitIndex`, frame states) so a subsequent
   MAP starts a fresh ring and re-attaches into the live session.
   Teardown and cleanup paths keep their explicit stop; only the UNMAP
   handler loses it. Regression test: N ≥ 3 snaps, shot 2 must succeed,
   no stop/start between shots in the log.

2. **Error code on bpp-resolution failure.** In the shared capture
   prepare ([ioctl/frames.c](../photonic/ioctl/frames.c)),
   `PhotonicDcamCodingBpp` returning 0 sets `PL_ERROR_INVALID_STATE`
   (0x17); the error-code convention says format-resolution failures are
   `PL_ERROR_HARDWARE` (0x0E). Change the code to 0x0E (the path is
   unreachable with a known coding either way).

## Driver: unimplemented IOCTLs

Specified in full in
[remaining-ioctls-design.md](remaining-ioctls-design.md); all are stubs
returning `STATUS_NOT_IMPLEMENTED` today:

- Identity: SDK_VERSION_1/2, SERIAL_NUMBER, SET_CANCEL_TIMEOUT,
  GET_VENDOR_NAME, GET_MODEL_NAME, GET_DCAM_VERSION, GET_NAMES_LENGTH.
  Requires capturing the config-ROM identity (EUI-64, textual leaves,
  unit_sw_version) at device start.
- Properties: INITIALIZE, PROPERTY_GET, PROPERTY_SET. Requires the DCAM
  feature table built at device start.
- Format extras: IMAGE_FLIP, GET_CHANNEL, GET_PACKET_SIZE.
- Trigger/strobe: TRIGGER_SET, STROBE_SET. Requires the vendor
  extended-command primitive
  ([dcam-registers.md](dcam-registers.md#vendor-extended-commands)).
- Mailbox: MAILBOX_CMD_GET_ADDRESSES.

## Fake device

1. **Vendor extended-command channel**: model block writes to
   `0xFFFF:0x00000000` (validate the header, log and store per-command
   payloads, ACK). Needed to test TRIGGER_SET, STROBE_SET and
   IMAGE_FLIP.
2. **Config-ROM identity**: vendor and model TEXTUAL_LEAF entries and a
   stable, predictable EUI-64. Needed to test the identity IOCTLs
   positively (the absent-leaf error paths are testable today).
3. **One-shot trigger fidelity**: in external-trigger mode, hold the
   one-shot frame until a trigger pulse is pending instead of
   transmitting immediately on the `ONE_SHOT` write, so the fake
   reproduces the real arm → trigger → transmit ordering and tests can
   assert it (ioctl-interface-design.md §1).
4. Optional: absolute-value CSR blocks for one feature, to exercise the
   PROPERTY_GET/SET absolute paths.

## Tests

- Extend the IOCTL test sequences to the corrected snap shape: prepare
  with FrameCount 1, per-shot map/arm/fetch/unmap, the shot-2
  regression, and a log assertion that UNMAP does not stop a running
  session (ioctl-interface-design.md §9).
- The test plan for the unimplemented IOCTLs is in
  remaining-ioctls-design.md §9.
