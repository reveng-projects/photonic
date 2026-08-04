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
/// External hardware-trigger input over a serial port.  See trigger.h.
///
/// The host side generates the pulse as back-to-back SETDTR/CLRDTR calls with
/// no delay, so the pulse can be well under a millisecond and polling the line
/// state would miss it.  TIOCMIWAIT instead sleeps until the UART reports a
/// modem-status interrupt, which catches the edge itself.  One wake-up counts
/// as one trigger; the settle delay afterwards keeps the second edge of the
/// same pulse (and cabling-induced jitter across DSR/DCD/CTS) from being
/// counted as another trigger, since edges that occur while the thread is not
/// waiting are not queued.

#include "trigger.h"

#include "dcam_internal.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

constexpr char TRIGGER_PORT_ENV[] = "PHOTONIC_TRIGGER_PORT";
constexpr char TRIGGER_PORT_DEFAULT[] = "/dev/ttyUSB0";

/// Modem input lines that may carry the host's DTR pulse: a null-modem cable
/// routes peer DTR to DSR and/or DCD, some adapters loop it to CTS.
constexpr int TRIGGER_LINES = TIOCM_DSR | TIOCM_CD | TIOCM_CTS;

/// Time to wait after a pulse edge before re-arming, to ignore the trailing
/// edge and cabling jitter.
constexpr int TRIGGER_SETTLE_US = 20000;

/// Watcher thread: blocks in TIOCMIWAIT and increments trigger_pending on each
/// modem-line edge while the camera is in external trigger mode.
///
/// @param arg  The dcam_camera_t cast to void *.
/// @return     Always NULL.
static void *trigger_thread(void *arg) {
    dcam_camera_t *cam = arg;

    while (!cam->trigger_stop) {
        if (ioctl(cam->trigger_fd, TIOCMIWAIT, TRIGGER_LINES) < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR, "[trigger] TIOCMIWAIT failed: %s; trigger input disabled", strerror(errno));
            break;
        }
        if (cam->trigger_stop) {
            break;
        }
        // A pulse only arms a frame when the sequencer is in external trigger
        // mode, like the real head.  This also rejects line noise before
        // arming: opening the COM port on the host side and its initial
        // CLRDTR toggle the modem lines too, and counting those would leave
        // STATUS FRAME_READY stuck high with no frame ever consuming it.
        // (The mode byte is written by the main thread; the unlocked
        // single-byte read is benign.)
        if (cam->i2c_regs[CAMREG_TRIGGER_MODE] != CAMREG_TRIGGER_EXTERNAL) {
            LOG(DEBUG, "[trigger] pulse ignored (trigger mode 0x%02x, not armed)", cam->i2c_regs[CAMREG_TRIGGER_MODE]);
            usleep(TRIGGER_SETTLE_US);
            continue;
        }
        pthread_mutex_lock(&cam->trigger_lock);
        cam->trigger_pending++;
        unsigned pending = cam->trigger_pending;
        pthread_mutex_unlock(&cam->trigger_lock);
        LOG(INFO, "[trigger] pulse received (%u pending)", pending);
        usleep(TRIGGER_SETTLE_US);
    }
    return NULL;
}

void dcam_trigger_start(dcam_camera_t *cam) {
    const char *env = getenv(TRIGGER_PORT_ENV);
    const char *port = (env != NULL && *env != '\0') ? env : TRIGGER_PORT_DEFAULT;
    pthread_t thread;
    pthread_attr_t attr;
    int err;

    cam->trigger_fd = -1;
    // The lock is needed even without a usable port: the status/consume
    // accessors below run regardless of whether the watcher thread exists.
    pthread_mutex_init(&cam->trigger_lock, NULL);
    cam->trigger_pending = 0;

    // O_NONBLOCK so the open does not wait for carrier on a real modem line.
    int fd = open(port, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG(WARN, "[trigger] cannot open %s: %s; running without trigger input", port, strerror(errno));
        return;
    }

    // CLOCAL is essential: the trigger pulse itself may arrive on DCD, and
    // without CLOCAL the tty core treats the pulse's falling edge as loss of
    // carrier and hangs up the port, after which every ioctl (including
    // TIOCMIWAIT) fails with EIO.  Raw mode keeps line discipline processing
    // out of the way; nothing is ever read from the port.
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL;
        if (tcsetattr(fd, TCSANOW, &tio) < 0) {
            LOG(WARN,
                "[trigger] cannot set CLOCAL on %s: %s; a DCD pulse may "
                "hang up the port",
                port, strerror(errno));
        }
    } else {
        LOG(WARN,
            "[trigger] tcgetattr on %s failed: %s; a DCD pulse may hang "
            "up the port",
            port, strerror(errno));
    }

    // The watcher never returns from TIOCMIWAIT on an idle line, so it cannot
    // be joined on shutdown; run it detached and let process exit reap it.
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    cam->trigger_fd = fd;
    err = pthread_create(&thread, &attr, trigger_thread, cam);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        LOG(ERROR,
            "[trigger] cannot start watcher thread: %s; running without "
            "trigger input",
            strerror(err));
        close(fd);
        cam->trigger_fd = -1;
        return;
    }

    LOG(INFO, "[trigger] watching %s (DSR/DCD/CTS) for trigger pulses", port);
}

void dcam_trigger_stop(dcam_camera_t *cam) {
    if (cam->trigger_fd < 0) {
        return;
    }
    cam->trigger_stop = 1;
    close(cam->trigger_fd);
    cam->trigger_fd = -1;
}

unsigned dcam_trigger_pending(const dcam_camera_t *cam) {
    // The cast only drops const; the lock and counter are mutable state.
    dcam_camera_t *mut = (dcam_camera_t *) cam;
    pthread_mutex_lock(&mut->trigger_lock);
    unsigned pending = mut->trigger_pending;
    pthread_mutex_unlock(&mut->trigger_lock);
    return pending;
}

void dcam_trigger_consume(dcam_camera_t *cam) {
    pthread_mutex_lock(&cam->trigger_lock);
    if (cam->trigger_pending > 0) {
        cam->trigger_pending--;
    }
    pthread_mutex_unlock(&cam->trigger_lock);
}

void dcam_trigger_disarm(dcam_camera_t *cam) {
    pthread_mutex_lock(&cam->trigger_lock);
    unsigned dropped = cam->trigger_pending;
    cam->trigger_pending = 0;
    pthread_mutex_unlock(&cam->trigger_lock);
    if (dropped > 0) {
        LOG(INFO, "[trigger] disarmed, %u pending pulse(s) dropped", dropped);
    }
}
