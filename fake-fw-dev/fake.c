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
/// Entry point for the fake AV/C / DCAM FireWire camera.  Wires together the
/// generic FireWire device object (fwdev) and the device-specific DCAM camera
/// (dcam), then runs the event loop.
///
/// What it does:
///   1. Opens /dev/fw0 (or argv[1]) as a local-node device.
///   2. Creates a DCAM camera on it: allocates the DCAM CSR register region and
///      the FCP command region and registers their handlers.
///   3. Publishes the Configuration ROM (vendor/model leaves + unit directory
///      advertising a generic DCAM camera), which triggers a bus reset so the
///      remote host re-reads the ROM and discovers the unit.
///   4. Runs the event loop (bus resets, FCP/AV/C commands, DCAM register
///      reads/writes, isochronous video transmit).
///
/// Build:
///   cmake -S . -B build && cmake --build build
///
/// Run (as root or with /dev/fw* permissions):
///   sudo ./fw_fake_camera          # uses /dev/fw0
///   sudo ./fw_fake_camera /dev/fw1 # uses /dev/fw1
///
/// Environment overrides (diagnostics):
///   PHOTONIC_TAG=<0..3>  isochronous tag field (default 0; DCAM video is raw).
///   PHOTONIC_SY=all      put sy=1 on every packet instead of only the first.
///   PHOTONIC_ROM_MODE=name|ids   Config ROM identity style (default: ids).
///                        name -> 1394\Vitana&PixeLINK(tm)_-_Photonic
///                        ids  -> 1394\A02D&100 (unit_spec_id & unit_sw_version)
///   PHOTONIC_MODEL_ID=<n>  24-bit Model_Id published in the Config ROM (decimal
///                        or 0x-hex, default 0x000001).  Give each instance a
///                        distinct value when running several fake cameras on
///                        the same node so their unit directories differ.
///   PHOTONIC_TRIGGER_PORT=<path>  serial port whose modem input lines carry
///                        the host's DTR hardware-trigger pulse (default
///                        /dev/ttyS0; see trigger.c).
///   PHOTONIC_STREAM=1|<bpp>  start streaming Format 7 mode 0 at launch without
///                        waiting for a host to program the video registers.
///                        1 keeps the power-up BYTE_PER_PACKET (the packet
///                        unit, slowest rate); any larger value is programmed
///                        as BYTE_PER_PACKET and must be a legal unit multiple
///                        (e.g. 2776 for the 1388x1032 MONO8 mode).  Pair with
///                        the irlisten tool on another controller to verify
///                        packet-level delivery on the wire.
///
/// Advertised mode set (default = the real "Photonic" camera from the trace):
///   PHOTONIC_STD_FORMATS=1   also advertise the standard DCAM Formats 0/1/2
///                            (default 0: Format 7 only, as the real camera does).
///   PHOTONIC_F7_MODES=<n>    number of Format 7 modes to expose, 1..4 (default 1):
///                            1 -> 1388x1032 greyscale (real camera) only;
///                            higher values add the fake's 1280x960/640x480/320x240
///                            modes for multi-mode testing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcam.h"
#include "fwdev.h"
#include "log.h"

/// Configuration ROM identity for the Photonic camera.
/// Windows builds the hardware ID 1394\Vitana&PixeLINK(tm)_-_Photonic from the
/// vendor + model textual leaves in the unit directory.  PHOTONIC_VENDOR_ID only
/// anchors the vendor textual leaf (key 0x03); its numeric value does not appear
/// in the textual hardware ID.
constexpr char PHOTONIC_VENDOR[] = "Vitana";
constexpr uint32_t PHOTONIC_VENDOR_ID = 0x000001;
constexpr char PHOTONIC_MODEL_NAME[] = "PixeLINK(tm) - Photonic";
constexpr uint32_t PHOTONIC_MODEL_ID = 0x000001;
constexpr uint32_t PHOTONIC_UNIT_SPEC_ID = 0x00A02D;    ///< 1394 TA / IIDC-DCAM (Image class)
constexpr uint32_t PHOTONIC_UNIT_SW_VERSION = 0x000100; ///< DCAM 1.0

static void parse_iso_config(dcam_iso_config_t *cfg) {
    const char *tag_env = getenv("PHOTONIC_TAG");
    const char *sy_env = getenv("PHOTONIC_SY");
    const char *std_env = getenv("PHOTONIC_STD_FORMATS");
    const char *f7_env = getenv("PHOTONIC_F7_MODES");

    cfg->tag = tag_env ? (atoi(tag_env) & 0x3) : 0;
    cfg->sy_all = (sy_env && strcmp(sy_env, "all") == 0) ? 1 : 0;
    cfg->advertise_std_formats = (std_env && atoi(std_env) != 0) ? 1 : 0;
    cfg->num_f7_modes = f7_env ? atoi(f7_env) : 1; // dcam_create clamps the range
    LOG(INFO, "iso packet header: tag=%d  sy=%s", cfg->tag, cfg->sy_all ? "all-packets" : "first-packet-only");
}

/// Returns the Model_Id to publish: the PHOTONIC_MODEL_ID override (decimal or
/// 0x-hex, truncated to the 24-bit key payload) or the built-in default.
///
/// @return  24-bit Model_Id.
static uint32_t parse_model_id(void) {
    const char *env = getenv("PHOTONIC_MODEL_ID");
    char *end;
    unsigned long value;

    if (env == NULL || *env == '\0') {
        return PHOTONIC_MODEL_ID;
    }

    value = strtoul(env, &end, 0);
    if (*end != '\0') {
        LOG(ERROR, "PHOTONIC_MODEL_ID=\"%s\" is not a number; using default 0x%06x", env, PHOTONIC_MODEL_ID);
        return PHOTONIC_MODEL_ID;
    }
    if (value > 0x00FFFFFFUL) {
        LOG(ERROR, "PHOTONIC_MODEL_ID=0x%lx exceeds 24 bits; truncating to 0x%06lx", value, value & 0x00FFFFFFUL);
        value &= 0x00FFFFFFUL;
    }
    LOG(INFO, "model_id override: 0x%06lx", value);
    return (uint32_t) value;
}

/// Selects the Config ROM identity style from PHOTONIC_ROM_MODE (default: name).
///
/// @return  FW_ROM_MODE_IDS if PHOTONIC_ROM_MODE is "ids", FW_ROM_MODE_NAME
///          otherwise.
static fw_rom_mode_t parse_rom_mode(void) {
    const char *env = getenv("PHOTONIC_ROM_MODE");
    if (env != NULL && strcmp(env, "ids") == 0) {
        return FW_ROM_MODE_IDS;
    }
    return FW_ROM_MODE_NAME;
}

int main(int argc, char *argv[]) {
    const char *devpath = (argc > 1) ? argv[1] : "/dev/fw0";

    dcam_iso_config_t iso_cfg;
    parse_iso_config(&iso_cfg);

    LOG(INFO, "opening %s", devpath);
    fw_device_t *dev = create_device(devpath, PHOTONIC_VENDOR, PHOTONIC_VENDOR_ID, PHOTONIC_MODEL_NAME,
                                     parse_model_id(), PHOTONIC_UNIT_SPEC_ID, PHOTONIC_UNIT_SW_VERSION);
    if (dev == NULL) {
        return EXIT_FAILURE;
    }

    fw_device_set_rom_mode(dev, parse_rom_mode());

    dcam_camera_t *cam = dcam_create(dev, &iso_cfg);
    if (cam == NULL) {
        fw_device_destroy(dev);
        return EXIT_FAILURE;
    }

    // Publish the Config ROM (must happen after the DCAM region is allocated
    // so the unit-dependent directory encodes the real CsrBase).
    fw_device_publish(dev);

    // Host-free diagnostic streaming: start transmitting immediately so a
    // passive listener can verify the wire without any controlling host.
    const char *stream_env = getenv("PHOTONIC_STREAM");
    if (stream_env != NULL && *stream_env != '\0') {
        unsigned long value = strtoul(stream_env, NULL, 0);
        if (value >= 1) {
            dcam_force_stream(cam, value > 1 ? (uint32_t) value : 0);
        }
    }

    fw_device_run(dev);

    dcam_destroy(cam);
    fw_device_destroy(dev);
    return EXIT_SUCCESS;
}
