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
/// Emulated 1394 DCAM/IIDC camera (the "Photonic" device) layered on top of a
/// generic fw_device_t.  Owns all device-specific behaviour: the DCAM CSR
/// register map, the video-format inquiry tables, isochronous video transmit,
/// and the AV/C FCP command handler.  All state is held in dcam_camera_t — no
/// globals.

#ifndef DCAM_H
#define DCAM_H

#include "fwdev.h"

/// Opaque DCAM camera object.
typedef struct dcam_camera dcam_camera_t;

/// Tunables for the emulated camera (diagnostic / shape overrides).
typedef struct dcam_iso_config {
    int tag;    ///< isochronous tag field (DCAM video is unformatted => 0)
    int sy_all; ///< sy=1 on every packet instead of only the first of a frame

    /// Which video modes the camera advertises.  The defaults reproduce the real
    /// "Photonic" camera from the capture trace: Format 7 only, a single greyscale
    /// 1388x1032 mode.  Raise these to expose the fake's fuller mode set.
    int advertise_std_formats; ///< advertise standard DCAM Formats 0/1/2 (default 0)
    int num_f7_modes;          ///< number of Format 7 modes to expose (default 1)
} dcam_iso_config_t;

/// Creates the camera and wires it onto `dev`: allocates the DCAM CSR register
/// region and the FCP command region, registers the request and iso-interrupt
/// handlers, and records the CSR base on the device so fw_device_publish()
/// advertises it in the unit-dependent directory.
///
/// The caller still owns `dev` and must call fw_device_publish() afterwards.
///
/// @param dev      The device to attach the camera to.
/// @param iso_cfg  Isochronous configuration overrides, or NULL for defaults.
/// @return         New camera object, or NULL on failure.
dcam_camera_t *dcam_create(fw_device_t *dev, const dcam_iso_config_t *iso_cfg);

/// Releases the camera (does not destroy the underlying device).
///
/// @param cam  Camera to destroy.
void dcam_destroy(dcam_camera_t *cam);

/// Starts streaming Format 7 mode 0 without waiting for a host to program the
/// video registers (PHOTONIC_STREAM).  Selects F7/M0 at its power-up geometry,
/// optionally programs BYTE_PER_PACKET through the same validated CSR path a
/// host write takes, and raises ISO_EN.  Intended for wire-level diagnostics
/// where a passive listener verifies the transmitted stream and no controlling
/// host is present.
///
/// @param cam               Camera to start.
/// @param bytes_per_packet  BYTE_PER_PACKET to program, or 0 to keep the
///                          power-up default (the packet unit, slowest rate).
void dcam_force_stream(dcam_camera_t *cam, uint32_t bytes_per_packet);

#endif // DCAM_H
