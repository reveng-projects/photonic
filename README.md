# Vitana Pixelink Photonic Camera Compatible Driver

This is a modern Windows driver for Pixelink Photonic FireWire cameras.
The original driver dates from the Windows XP era, does not work on 64-bit
systems, and is no longer supported by the manufacturer.

This driver:

- is a drop-in replacement for the original driver, so legacy 32-bit
  applications built against the vendor's user-mode library keep working
  unmodified,
- implements a subset of the original driver's IOCTLs,
- additionally exposes the camera through DirectShow, so standard capture
  applications work out of the box,
- runs on both 32-bit and 64-bit Windows 10 and 11.

![DirectShow capture from the emulated camera: the test app renders a
1388x1032 test pattern in a live video window while Device Manager shows
the Photonic Camera device](docs/screenshot1.png)

*DirectShow capture in action: the test suite sweeps the advertised video
modes and renders the emulated camera's test pattern (here 1388x1032
RGB8/Y8 at 15 fps) in a live preview window. Device Manager shows the
camera bound to the driver under Imaging devices.*

> [!IMPORTANT]
> This is an independent project, in no way affiliated with or endorsed
> by the original manufacturer.

> [!WARNING]
> The camera protocol described in this repository is a best-effort
> reconstruction. The specification may be incomplete or incorrect, and
> the driver itself may have further bugs. The driver also writes to the
> camera's internal registers, and incorrect values could in the worst
> case damage the hardware. Validate this software extensively before
> using any of it in production.

# Development methodology

Claude wrote close to 100% of the code, following a clean-room process split
across several agents:

1. **Virtual camera.** An agent extracted the hardware↔driver
   specification from the original driver and encoded it as a virtual
   device: [fake-fw-dev/](fake-fw-dev/), a Linux application that emulates
   the camera on a real FireWire bus. It emulates enough of the camera's
   functionality that the original driver and its user-space applications
   work against it unmodified.
2. **User-space protocol.** Another agent extracted the driver↔user-space
   protocol specification, aided by API Monitor traces of the
   vendor's applications. The result is a header file with the IOCTL
   codes, structures, and function definitions that the test suite can
   exercise.
3. **Test suite.** Another agent wrote the test suite that exercises the
   full behavior of the original driver through that protocol, turning
   the original driver's observable behavior into executable checks.
4. **New driver.** A final agent wrote the new driver from scratch using
   only the virtual device, the user-space protocol specification,
   the test suite, and actual I/O traces from the real hardware.
   It never had access to the original driver or user libraries.

These four steps were iterative, the specifications went through many refinement rounds.
First until the original driver and user-space libraries ran correctly against the emulated camera,
then until the new driver did too. From there, bringing up the new driver on the
real camera was mostly a formality. A few more rounds of
refining the specification against actual hardware traces closed the
remaining gaps.

# Development setup

Two machines connected by a FireWire cable were used in order to
develop the driver without access to the actual camera:

- An old laptop with a built-in FireWire port runs the emulated camera
  ([fake-fw-dev/](fake-fw-dev/)) under Linux.
- A development machine running Debian 13 hosts a Windows VM with a
  FireWire controller passed through. The driver under test runs in the
  VM against the emulated camera, so no real camera is needed for
  day-to-day development.

Two FireWire controllers were tried for the passthrough:

- VIA Technologies VT6315 (rev 01): works well in Windows XP guests but
  hangs on Windows 10/11.
- Texas Instruments XIO2213A/B/XIO2221 IEEE-1394b OHCI controller:
  works well on all OSes.

QEMU does not emulate a FireWire interface, so physical devices were required.

# Building

## Windows driver and test application

The Windows components build with MSBuild from [photonic.sln](photonic.sln).
The solution contains three projects:

- [prebuild/](prebuild/) — runs before the other projects and generates
  `version.h` at the solution root, embedding the driver version and the
  current git commit hash into the version resources.
- [photonic/](photonic/) — the WDM kernel driver, buildable for x86, x64,
  ARM and ARM64. The kernel API surface is pinned to Windows 10 1507 so a
  single binary loads on every Windows 10/11 version.
- [test/](test/) — the test application, Win32 only.

Required tooling:

- Visual Studio 2019 (the Community edition works) with the C++ desktop
  development workload.
- The Windows 10 SDK and the Windows Driver Kit (WDK) for Windows 10.
  The WDK provides the `WindowsKernelModeDriver10.0` platform toolset and
  the WPP tracing preprocessor that the driver project uses.
- The `v140_xp` platform toolset (a Visual Studio installer component)
  for the test application, so the same test binary also runs on
  Windows XP against the original driver.

To build, open the solution in Visual Studio, or run MSBuild from a
developer command prompt at the solution root:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat"
msbuild /property:Configuration=Debug /property:Platform=x64
```

The build produces the driver package (`photonic.sys`, `photonic.inf` and
the catalog file) in the configuration's output directory, for example
`x64/Debug/photonic/`. The package is signed with a WDK test certificate,
so the target machine must have test signing enabled
(`bcdedit /set testsigning on`) to install it.

## Emulated camera (Linux)

The emulated camera ([fake-fw-dev/](fake-fw-dev/)) builds with CMake 3.16
or newer and a C compiler that supports C23. It talks to the FireWire
controller through the Linux firewire character-device interface, so the
kernel headers must be installed:

```sh
apt install linux-headers-$(uname -r)
cmake -S fake-fw-dev -B fake-fw-dev-build
cmake --build fake-fw-dev-build
```

The resulting `fw_fake_camera` binary is statically linked, so it can be
copied as is to the machine that has the FireWire port. Run it as root,
or as a user with access to the FireWire device nodes:

```sh
sudo ./fw_fake_camera           # uses /dev/fw0
sudo ./fw_fake_camera /dev/fw1  # uses a specific device node
```

# Documentation

The hardware documents give register-level specifications and access sequences.
The driver documents give the architecture and the client-facing contracts.

## Hardware

- [dcam-registers.md](docs/dcam-registers.md) — the camera's DCAM/IIDC
  interface: configuration ROM discovery, the DCAM CSR map, Format 7
  blocks, feature (image control) registers with the real camera's
  measured behavior, control sequences (reset, enumeration, stream
  start, single-frame arming), and the vendor extended-command channel.
- [camera-head-registers.md](docs/camera-head-registers.md) — the camera-head
  controller (gains, exposure, binning, readout window, pixel clock,
  trigger mode): the mailbox transport packets and the full 8-bit
  register map with programming notes.

## Driver

- [architecture.md](docs/architecture.md) — conceptual
  architecture of the zero-copy capture engine: descriptors and queues,
  bus bring-up, isochronous resource acquisition, bus-reset survival,
  and the Stream Class (DirectShow) front-end. Written to be
  re-implementable without reference to this code base's symbols.
- [ioctl-interface-design.md](docs/ioctl-interface-design.md) — the
  direct-buffer IOCTL interface: the imager/video session model, the
  client contract fixed by the existing user-mode library (streaming,
  triggered snap, teardown), single-frame mode, per-handler semantics,
  state machine and test plan.
- [remaining-ioctls-design.md](docs/remaining-ioctls-design.md) —
  implementation specification for the IOCTL handlers that are still
  stubs (identity, properties, trigger/strobe, mailbox addresses),
  including the compatibility quirks a reimplementation must keep.

Wire formats (IOCTL codes, struct layouts, buffer sizes) are
authoritative in [photonic/ioctl.h](photonic/ioctl.h); shared
constants live in [common/](common/).

## Status

- [future-work.md](docs/future-work.md) — known gaps between the documents
  above and the current implementation, and the planned work to close
  them.

## Components

- `photonic/` — the Windows Stream Class minidriver: DirectShow
  streaming front-end, direct-buffer IOCTL front-end, shared zero-copy
  capture engine, DCAM control, 1394 bus transport.
- `fake-fw-dev/` — a Linux application that emulates the camera on a
  FireWire bus (DCAM registers, mailbox gate, camera-head register file,
  isochronous streaming, external trigger via a serial port). The driver
  must work against it unmodified.
- `test/` — Windows test suites for both front-ends: a DirectShow sweep
  over every advertised video mode and an IOCTL-level suite for the
  direct-buffer interface.
- `common/` — headers shared between the driver, the fake device and the
  tests (camera-head register map, frame metadata layout).
