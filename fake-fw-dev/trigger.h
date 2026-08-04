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
/// External hardware-trigger input for the fake camera.  The controlling host
/// pulses DTR on a COM port that is wired to the camera's trigger input; on
/// the fake, that wire is a serial port on this machine whose modem input
/// lines (DSR/DCD/CTS, depending on the cabling) carry the pulse.
///
/// A watcher thread blocks in TIOCMIWAIT on that port and counts each pulse
/// into cam->trigger_pending.  With TRIGGER_MODE = CAMREG_TRIGGER_EXTERNAL the
/// STATUS FRAME_READY bit reports pending > 0, and the isochronous transmit
/// path consumes one pending trigger per frame sent (see dcam_i2c_status and
/// dcam_on_iso_interrupt).
///
/// The port is selected with the PHOTONIC_TRIGGER_PORT environment variable
/// (default /dev/ttyUSB0).

#ifndef TRIGGER_H
#define TRIGGER_H

#include "dcam.h"

/// Opens the trigger serial port and starts the watcher thread.  A missing or
/// unusable port is logged and leaves the camera without a trigger input
/// (pending count stays 0); it does not fail camera creation.
///
/// @param cam  Camera to arm with a trigger watcher.
void dcam_trigger_start(dcam_camera_t *cam);

/// Stops watching the trigger port.  The watcher thread is detached and may
/// stay blocked in TIOCMIWAIT until the process exits; this only flags it to
/// stop and closes the port so no further triggers are counted.
///
/// @param cam  Camera whose trigger watcher to stop.
void dcam_trigger_stop(dcam_camera_t *cam);

/// Returns the number of trigger pulses whose frame has not been read out yet.
///
/// @param cam  Camera to query.
/// @return     Pending pulse count.
unsigned dcam_trigger_pending(const dcam_camera_t *cam);

/// Consumes one pending trigger pulse, if any (called per transmitted frame).
///
/// @param cam  Camera whose pending count to decrement.
void dcam_trigger_consume(dcam_camera_t *cam);

/// Drops all pending trigger pulses.  Called when TRIGGER_MODE leaves external
/// mode: switching to free-run cancels any armed-but-unread exposure.
///
/// @param cam  Camera whose pending count to clear.
void dcam_trigger_disarm(dcam_camera_t *cam);

#endif // TRIGGER_H
