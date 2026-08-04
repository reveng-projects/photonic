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
/// Declarations shared by the capture engine files in this folder. The engine
/// (see capture.h for its design) is split by role: capture.c holds the public
/// entry points, pool.c the descriptor pool and chunk geometry, pump.c the
/// buffer pump and attach/detach state machine, resource.c the isochronous
/// resource lifecycle, teardown.c the quiesce and session release, and work.c
/// the control work item. Nothing outside capture/ includes this header: the
/// public contract stays in capture.h.
///
/// The includer must provide capture.h first (the engine types and the
/// public API). Full behavioral contracts live at each definition.

/// pool.c -- descriptor pool and chunk geometry.
NTSTATUS PhotonicCapturePoolAllocate(_In_ PPHOTONIC_CAPTURE Capture);
VOID PhotonicCapturePoolFree(_In_ PPHOTONIC_CAPTURE Capture);
NTSTATUS PhotonicCapturePoolSetupChunks(_In_ PPHOTONIC_CAPTURE Capture);
VOID PhotonicCapturePoolFreeChunkMdls(_In_ PPHOTONIC_CAPTURE Capture);

/// pump.c -- attach/detach state machine. Only the quiesce calls this from
/// outside the pump, to recycle a descriptor it has already claimed under
/// Lock.
VOID PhotonicCapturePumpBeginDetach(_In_ PPHOTONIC_CAPTURE Capture, _In_ PPHOTONIC_CAPTURE_FRAME Desc);

/// resource.c -- isochronous resource lifecycle (channel, bandwidth, resource
/// handle), plus the synchronous ISOCH_STOP the quiesce issues before any
/// detach.
NTSTATUS PhotonicCaptureResourceAllocateChannel(_In_ PPHOTONIC_CAPTURE Capture);
NTSTATUS PhotonicCaptureResourceAllocateBandwidth(_In_ PPHOTONIC_CAPTURE Capture, _In_ ULONG SpeedFlags);
NTSTATUS PhotonicCaptureResourceAllocateHandle(_In_ PPHOTONIC_CAPTURE Capture, _In_ ULONG SpeedFlags);
VOID PhotonicCaptureResourceIsochStop(_In_ PPHOTONIC_CAPTURE Capture);
BOOLEAN PhotonicCaptureResourceReleaseIsoch(_In_ PPHOTONIC_CAPTURE Capture);

/// teardown.c -- quiesce, session release and the shared shutdown, called by
/// the public start/stop paths and the control work item's cancel resolution.
VOID PhotonicCaptureTeardownWaitPumpIdle(_In_ PPHOTONIC_CAPTURE Capture);
VOID PhotonicCaptureTeardownQuiesce(_In_ PPHOTONIC_CAPTURE Capture);
VOID PhotonicCaptureTeardownReleaseSessionResources(_In_ PPHOTONIC_CAPTURE Capture);
VOID PhotonicCaptureTeardownShutdown(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _In_ PPHOTONIC_STREAM_EXTENSION Stream,
                                     _In_ PPHOTONIC_CAPTURE Capture);
