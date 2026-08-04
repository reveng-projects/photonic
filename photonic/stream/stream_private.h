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
/// Declarations shared by the Photonic stream handler files in this folder.
/// The KS stream minidriver code lives in sibling files grouped by
/// functional area: format advertisement (formats.c), format negotiation
/// (intersect.c), pin open and close (lifecycle.c), control SRBs (control.c)
/// and the read data path (read.c). Nothing outside stream/ includes this
/// header: the driver-wide interface stays in photonic.h.
///
/// The includer must provide photonic.h first (kernel and KS types).

/// Default frame interval (100ns units, 30 fps) used as a per-mode fallback when
/// an enumerated mode or a negotiated connection format reports no frame rate.
#define PHOTONIC_DEFAULT_FRAME_TIME 333333

/// Number of palette entries carried by the RGB8 (DCAM MONO8) connection format: a
/// full 256-entry greyscale ramp where colour index i maps to grey (i, i, i), so an
/// 8-bit luma sample renders as the matching shade.
#define PHOTONIC_RGB8_PALETTE_COLORS 256

/// Delivery callback the capture engine invokes to complete a read once its frame
/// has been captured (defined in read.c with the rest of the read path).
///
/// @param Context    Stream extension passed as the callback context.
/// @param Cookie     Opaque cookie identifying the read (the SRB pointer).
/// @param BytesUsed  Number of bytes captured into the frame buffer.
/// @param Status     Completion status for the frame.
VOID PhotonicStreamDeliverFrame(_In_ PVOID Context, _In_ ULONG_PTR Cookie, _In_ ULONG BytesUsed, _In_ NTSTATUS Status);

/// Buffer source the capture engine's pump pulls from (defined in read.c):
/// pops the next parked read and hands its framework-built MDL to the engine
/// as the zero-copy DMA target.
///
/// @param Context  Stream extension passed as the callback context.
/// @param Cookie   Receives the read SRB pointer as the frame cookie.
/// @param Mdl      Receives the read's framework-built MDL.
/// @return TRUE when a read was produced, FALSE when none is parked.
BOOLEAN PhotonicStreamAcquireBuffer(_In_ PVOID Context, _Out_ PULONG_PTR Cookie, _Out_ PMDL *Mdl);

/// Range-matching predicates shared by format negotiation (intersect.c) and
/// open-time mode recovery (lifecycle.c), so the two preference ladders
/// cannot drift apart (defined in formats.c with the range table they
/// describe). biHeight is negative for the top-down BI_RGB codings, so
/// heights are compared as abs(biHeight).

/// TRUE when Interval is zero (the peer left it unspecified) or falls inside
/// Range's advertised frame-interval window.
///
/// @param Range     Advertised video range.
/// @param Interval  Requested frame interval in 100ns units.
/// @return TRUE when the range can produce the interval.
BOOLEAN PhotonicStreamRangeIntervalOk(_In_ PKS_DATARANGE_VIDEO Range, _In_ LONGLONG Interval);

/// TRUE when Range's default resolution equals Width x Height, comparing
/// heights by magnitude.
///
/// @param Range   Advertised video range.
/// @param Width   Requested width in pixels.
/// @param Height  Requested height in pixels (magnitude).
/// @return TRUE when the default resolution matches exactly.
BOOLEAN PhotonicStreamRangeDefaultSizeIs(_In_ PKS_DATARANGE_VIDEO Range, _In_ LONG Width, _In_ LONG Height);
