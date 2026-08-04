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
/// DCAM (IIDC 1394-based Digital Camera) bring-up for the Photonic minidriver.
///
/// Sits on top of the 1394 transport (p1394.c) and speaks the DCAM CSR register
/// map: it resets the camera and enumerates the video formats / modes / frame
/// rates it advertises in its inquiry registers, decoding each supported
/// (format, mode) pair into a PHOTONIC_VIDEO_MODE in the device extension. The
/// stream layer (stream/formats.c) then turns that table into the KS data ranges the
/// capture pin exposes to DirectShow.

#ifndef PHOTONIC_DCAM_H
#define PHOTONIC_DCAM_H

#include "photonic.h"

/// Reset the camera to a known state via the DCAM INITIALIZE register, polling
/// until the reset bit self-clears (bounded, so a stuck camera cannot hang
/// bring-up). Must run after the CSR base has been discovered and the bus
/// state refreshed.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicDcamReset(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Read the camera's inquiry registers (VIDEO_FORMAT_INQ, VIDEO_MODE_INQ_f,
/// FRAME_RATE_INQ) and populate extension->Modes / extension->ModeCount with one
/// entry per supported standard (format, mode) pair. If the camera advertises
/// Format 7 (scalable image), its modes are enumerated too -- one entry per
/// (mode, colour coding) -- from the per-mode Format 7 CSR blocks. Returns
/// STATUS_SUCCESS when at least one mode was enumerated.
///
/// @param Extension  Device extension.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicDcamEnumerateModes(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Program the camera for the stream's negotiated mode so it is ready to stream:
/// for a standard format this writes VIDEO_FORMAT / VIDEO_MODE / FRAME_RATE; for a
/// Format 7 mode it writes VIDEO_FORMAT / VIDEO_MODE and the per-mode CSR block
/// (IMAGE_SIZE, COLOR_CODING_ID, BYTE_PER_PACKET). On success *BytesPerPacket /
/// *PacketsPerFrame return the isochronous packetisation the camera will use,
/// which the capture engine needs to size its receive buffers (BytesPerPacket is
/// the per-packet payload, PacketsPerFrame * BytesPerPacket is the frame size).
/// Must run at PASSIVE_LEVEL.
///
/// @param Extension       Device extension.
/// @param Stream          Stream extension holding the negotiated mode and frame interval.
/// @param BytesPerPacket  Receives the per-packet isochronous payload in bytes.
/// @param PacketsPerFrame Receives the number of isochronous packets per frame.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicDcamConfigureStream(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                                     _Out_ PULONG BytesPerPacket, _Out_ PULONG PacketsPerFrame);

/// Read the camera's live VIDEO_FORMAT / VIDEO_MODE selection and decode it to
/// the DCAM colour coding (PHOTONIC_DCAM_PIX_*) via the enumerated mode table;
/// for a Format 7 mode the coding is read from the mode's COLOR_CODING_ID
/// register instead, since several codings share one (format, mode) pair.
/// Returns STATUS_NOT_FOUND when the camera's selection matches no enumerated
/// mode. Must run at PASSIVE_LEVEL.
///
/// @param Extension   Device extension.
/// @param PixelFormat Receives the PHOTONIC_DCAM_PIX_* colour coding.
/// @return STATUS_SUCCESS on success, STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamGetCurrentPixelFormat(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG PixelFormat);

/// Read the camera's live VIDEO_FORMAT / VIDEO_MODE selection and return the
/// matching enumerated mode entry (a stable pointer into Extension->Modes).
/// For a Format 7 selection the match is refined with the live
/// COLOR_CODING_ID, since several entries (one per colour coding) share one
/// (format, mode) pair. Returns STATUS_NOT_FOUND when the selection matches no
/// enumerated mode. Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Mode       Receives a pointer to the matching enumerated mode entry.
/// @return STATUS_SUCCESS on success, STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamGetCurrentMode(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Outptr_ PPHOTONIC_VIDEO_MODE *Mode);

/// Derive the camera's current ROI in full-resolution sensor coordinates from
/// the live VIDEO_FORMAT / VIDEO_MODE selection. Scale is the decimation
/// factor relative to the full-resolution mode; offsets and size are
/// multiplied by it so they are expressed in full-sensor units. A standard
/// fixed mode reports its default full frame at offset 0,0; a Format 7 mode
/// reports the live IMAGE_POSITION / IMAGE_SIZE from its CSR block. Returns
/// STATUS_NOT_FOUND when the camera's selection matches no enumerated mode.
/// Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Scale      Receives the decimation factor relative to the full-resolution mode.
/// @param OffsetX    Receives the horizontal ROI offset in full-sensor units.
/// @param OffsetY    Receives the vertical ROI offset in full-sensor units.
/// @param Width      Receives the ROI width in full-sensor units.
/// @param Height     Receives the ROI height in full-sensor units.
/// @return STATUS_SUCCESS on success, STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamGetCurrentSubwindow(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG Scale,
                                         _Out_ PULONG OffsetX, _Out_ PULONG OffsetY, _Out_ PULONG Width,
                                         _Out_ PULONG Height);

/// Bits per pixel of a DCAM colour coding (PHOTONIC_DCAM_PIX_*), the depth a
/// frame's byte size is derived from (width * height * bpp / 8). Returns 0 for
/// a coding the driver does not recognise.
///
/// @param Coding  PHOTONIC_DCAM_PIX_* colour coding id.
/// @return Bits per pixel, or 0 for an unrecognised coding.
ULONG PhotonicDcamCodingBpp(_In_ ULONG Coding);

/// Select the first enumerated mode whose colour coding (PHOTONIC_DCAM_PIX_*)
/// matches PixelFormat: latch its VIDEO_FORMAT / VIDEO_MODE and, for a Format 7
/// mode, its COLOR_CODING_ID (several codings share one (format, mode) pair).
/// The ROI (IMAGE_SIZE) and frame rate are left untouched; the stream
/// configuration programs them at capture start. Returns STATUS_NOT_FOUND when
/// no enumerated mode has the requested coding. Must run at PASSIVE_LEVEL.
///
/// @param Extension   Device extension.
/// @param PixelFormat PHOTONIC_DCAM_PIX_* colour coding to select.
/// @return STATUS_SUCCESS on success, STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamSelectPixelFormat(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG PixelFormat);

/// Return the frame interval (100ns units) of the Index-th frame rate the
/// camera supports in its current VIDEO_FORMAT / VIDEO_MODE selection, in
/// FRAME_RATE_INQ order (slowest first). A Format 7 mode has no discrete rate
/// table and enumerates a single entry: the fastest rate its packet parameters
/// allow. Returns STATUS_NO_MORE_ENTRIES when Index is past the last supported
/// rate and STATUS_NOT_FOUND when the camera's selection matches no enumerated
/// mode. Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Index      Zero-based index into the supported frame-rate list (slowest first).
/// @param Interval   Receives the frame interval in 100ns units.
/// @return STATUS_SUCCESS on success, STATUS_NO_MORE_ENTRIES when exhausted,
///         STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamEnumFrameRate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Index, _Out_ PULONG Interval);

/// Select the active frame rate of the camera's current mode by its
/// enumeration index (same ordering as PhotonicDcamEnumFrameRate), writing the
/// matching DCAM rate id to the FRAME_RATE register. For a Format 7 mode only
/// index 0 is accepted, as a no-op (there is no rate register to program).
/// Returns STATUS_NO_MORE_ENTRIES when Index is past the last supported rate
/// and STATUS_NOT_FOUND when the camera's selection matches no enumerated
/// mode. Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Index      Zero-based index of the frame rate to select (same order as
///                   PhotonicDcamEnumFrameRate).
/// @return STATUS_SUCCESS on success, STATUS_NO_MORE_ENTRIES when exhausted,
///         STATUS_NOT_FOUND when no match, or an error code.
NTSTATUS PhotonicDcamSetFrameRate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Index);

/// Return the frame interval (100ns units) of the camera's current frame rate,
/// read live from the FRAME_RATE register. For a Format 7 mode the fastest
/// rate the packet parameters allow is reported, matching what
/// PhotonicDcamEnumFrameRate enumerates. Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Interval   Receives the current frame interval in 100ns units.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicDcamGetFrameRate(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PULONG Interval);

/// Write the ISOCH_CHANNEL register: the isochronous channel the camera transmits
/// on (allocated from the 1394 bus driver by the capture engine) and the transmit
/// speed (an SCODE_*_RATE). Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Channel    Isochronous channel number allocated by the capture engine.
/// @param SpeedCode  Transmit speed as an SCODE_*_RATE value.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicDcamSetIsochChannel(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ ULONG Channel,
                                     _In_ ULONG SpeedCode);

/// Write the ISO_EN register to start (Enable == TRUE) or stop isochronous
/// transmission. Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Enable     TRUE to start isochronous transmission, FALSE to stop.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicDcamSetIsochEnable(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ BOOLEAN Enable);

/// Write the ONE_SHOT register: Arm == TRUE (bit 31) arms the camera for one
/// acquisition -- it captures a frame, transmits it and stops; FALSE cancels an
/// armed acquisition that has not fired yet.
/// Must run at PASSIVE_LEVEL.
///
/// @param Extension  Device extension.
/// @param Arm        TRUE to arm a single-frame acquisition, FALSE to cancel.
/// @return STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicDcamOneShot(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ BOOLEAN Arm);

#endif // PHOTONIC_DCAM_H
