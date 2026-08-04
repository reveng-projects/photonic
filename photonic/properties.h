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
/// KS property handling for the Photonic stream minidriver.
///
/// Owns the driver's KS property sets: the per-stream connection properties
/// (allocator framing) the capture pin advertises, and the device (filter) level
/// VideoControl / VideoProcAmp / CameraControl property sets that expose the
/// camera's DCAM image-control feature registers through IAMVideoProcAmp /
/// IAMCameraControl. The property tables live in properties.c and are advertised
/// on the stream and in the stream descriptor through the accessors below.

#ifndef PHOTONIC_PROPERTIES_H
#define PHOTONIC_PROPERTIES_H

#include "photonic.h"

/// Device (filter) level property get/set, called from the device SRB dispatcher
/// (dispatch.c). GET answers VIDEOCONTROL_CAPS plus the VideoProcAmp /
/// CameraControl image controls; SET applies a settable image control.
NTSTATUS PhotonicGetDeviceProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
NTSTATUS PhotonicSetDeviceProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb);

/// Per-stream property get/set, called from PhotonicStreamReceiveControlPacket
/// (stream/control.c). GET answers KSPROPSETID_Connection / ALLOCATORFRAMING; there are
/// no settable stream-level properties.
NTSTATUS PhotonicGetStreamProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb);
NTSTATUS PhotonicSetStreamProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb);

/// Discover the image controls the camera implements and build the device
/// (filter) property sets from them into the device extension: VideoControl
/// always, VideoProcAmp / CameraControl restricted to the controls whose DCAM
/// feature register is present, each advertising the range the camera actually
/// supports. Called once from SRB_INITIALIZE_DEVICE after camera bring-up (also
/// after a failed bring-up, so the VideoControl set is always advertised). Must
/// run at PASSIVE_LEVEL.
VOID PhotonicBuildDevicePropertySets(_In_ PPHOTONIC_DEVICE_EXTENSION Extension);

/// Hand the advertised property sets to the stream-descriptor builder (stream/formats.c):
/// the per-stream connection set advertised on the capture pin, and the device
/// (filter) sets reported in the stream descriptor header (built per device by
/// PhotonicBuildDevicePropertySets).
VOID PhotonicGetStreamPropertySet(_Out_ PKSPROPERTY_SET *Set, _Out_ PULONG Count);
VOID PhotonicGetDevicePropertySet(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PKSPROPERTY_SET *Set,
                                  _Out_ PULONG Count);

#endif // PHOTONIC_PROPERTIES_H
