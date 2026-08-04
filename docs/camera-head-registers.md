# Camera-head controller registers

The camera head (CCD sequencer / ADC electronics) is a separate controller
behind the camera's FireWire interface, attached to the camera's internal I2C
bus at device address **0x14** (`CAMREG_I2C_DEV_ADDR`). It holds the
acquisition settings that are not part of the DCAM/IIDC CSR space
([dcam-registers.md](dcam-registers.md)): analog gains, exposure, binning,
readout window, pixel clock, ADC calibration and trigger mode.

The shared register definitions live in [common/camera_regs.h](../common/camera_regs.h);
the fake camera models the register file in
[fake-fw-dev/dcam_internal.h](../fake-fw-dev/dcam_internal.h) with handlers in
[fake-fw-dev/i2c.c](../fake-fw-dev/i2c.c), and streams its live contents to
the test program inside every frame's metadata block
([common/frame_meta.h](../common/frame_meta.h)).

## Transport

Head registers are reached through the camera's mailbox gate: a 4 KB CSR
block at bus address `0xFFFF_F020_4000`. The host block-writes a command
packet there and, when a response is expected, block-reads it back from the
same address. Every 32-bit word is byte-swapped on the wire (big-endian
quadlets); the layouts below are in logical (little-endian) byte order after
de-swapping. The driver exposes the gate to user space through
`PHOTONIC_IOCTL_MAILBOX` ([photonic/ioctl.h](../photonic/ioctl.h)).

Two commands access the head registers, one register per transaction
(8-bit register address, 8-bit data):

### Register write — command `0x1009`

```
offset  size  field
0x00    4     0x00001009          command
0x04    1     0x14                I2C device address
0x05    1     reg                 register address
0x06    1     1                   constant
0x07    1     0                   constant
0x08    1     value               register value
0x09    3     0                   padding
```

12 bytes written; an 8-byte response is read back (contents ignored, the
completed transaction signals success).

### Register read — command `0x1008`

```
request (8 bytes)                  response (12 bytes)
0x00  4  0x00001008                0x00  8  (ignored)
0x04  1  0x14  I2C device address  0x08  1  register value
0x05  1  reg   register address
0x06  1  1     constant
0x07  1  0     constant
```

## Register map

All registers are 8 bits wide. "LE16" pairs hold one 16-bit value split
across two consecutive registers, **low byte at the lower address**.
Unlisted addresses in 0x00..0x2F are unused and read as 0. Power-up defaults
are 0 except where noted.

| Reg | Name | R/W | Default | Description |
|-----|------|-----|---------|-------------|
| 0x00 | `VIDEO_GAIN` | R/W | 1 | Video/ADC gain. Valid range **1..100**. |
| 0x02 | `INTENSIFIER_GAIN` | R/W | 1 | Image-intensifier gain. Valid range **1..100**. Meaningful only on heads fitted with an intensifier. |
| 0x04 | `EXPOSURE_UNITS` | R/W | 0 | Time base for the exposure value: **0 = microseconds, 1 = milliseconds, 2 = seconds**. |
| 0x06 | `EXPOSURE_TIME_L` | R/W | 0 | Exposure time, LE16 low byte. |
| 0x07 | `EXPOSURE_TIME_H` | R/W | 0 | Exposure time, LE16 high byte. 14-bit value, valid **1..0x3FFF**, in units selected by `EXPOSURE_UNITS`. Effective exposure = value × unit. |
| 0x0C | `X_BINNING` | R/W | 1 | Horizontal binning factor, valid **1..63**. |
| 0x0D | `Y_BINNING` | R/W | 1 | Vertical binning factor, valid **1..63**. |
| 0x0E | `SPEED_L` | R/W | 0 | Readout pixel clock in kHz, LE16 low byte. |
| 0x0F | `SPEED_H` | R/W | 0 | Pixel clock, LE16 high byte. Supported values: **10000 (10 MHz)** and **20000 (20 MHz)**. The sensor `SETUP_VALUE` and `ADC_OFFSET` calibrations are speed-dependent and must be re-programmed after a speed change. |
| 0x1A | `SETUP_VALUE_L` | R/W | 0 | Sensor/ADC setup (calibration) word, LE16 low byte. |
| 0x1B | `SETUP_VALUE_H` | R/W | 0 | Setup word, LE16 high byte. Opaque per-head calibration constant, selected by speed and binning. At 20 MHz the value is written pre-shifted left by 6 bits. |
| 0x1C | `ROI_X_START_L` | R/W | 0 | Readout window X start (unbinned sensor pixels), LE16 low byte. |
| 0x1D | `ROI_X_START_H` | R/W | 0 | X start, LE16 high byte. |
| 0x1E | `ROI_Y_START_L` | R/W | 0 | Readout window Y start (unbinned rows), LE16 low byte. |
| 0x1F | `ROI_Y_START_H` | R/W | 0 | Y start, LE16 high byte. |
| 0x20 | `ROI_X_END_L` | R/W | 0 | Readout window X end (exclusive), LE16 low byte. |
| 0x21 | `ROI_X_END_H` | R/W | 0 | X end, LE16 high byte. `x_end = x_start + binned_width * x_bin`. |
| 0x22 | `ROI_Y_END_L` | R/W | 0 | Readout window Y end (exclusive), LE16 low byte. |
| 0x23 | `ROI_Y_END_H` | R/W | 0 | Y end, LE16 high byte. `y_end = y_start + binned_height * y_bin`. |
| 0x26 | `ADC_OFFSET` | R/W | 0 | ADC black-level offset. Per-head calibration value, selected by speed and binning. |
| 0x2A | `STATUS` | **R** | — | Status register, read-only. **Bit 2 (`0x04`, `FRAME_READY`)**: an exposed frame is waiting for readout. Other bits are reserved and read as 0. Writes are ignored. |
| 0x2C | `TRIGGER_MODE` | R/W | 0 | Acquisition mode: **0x00 = free-run**, **0x31 = externally triggered, single frame per trigger pulse**. |

### Binning

Binning combines a rectangular block of adjacent sensor pixels into a single
output pixel: with `X_BINNING = n` and `Y_BINNING = m`, the charge of each
n-wide × m-high block is summed during readout and delivered as one pixel.
The frame therefore shrinks to `width / n × height / m` pixels, which

- increases sensitivity and signal-to-noise ratio (the summed charge is read
  out — and picks up read noise — only once per block), and
- speeds up readout and lowers the FireWire bandwidth per frame,

at the cost of spatial resolution. `1 × 1` (the power-up default) means no
binning; the two factors are independent, so asymmetric modes such as `1 × 2`
are valid.

Binning is applied to the readout window selected by the ROI registers, whose
coordinates always stay in **unbinned** sensor pixels. The window must span a
whole number of blocks: `x_end - x_start` a multiple of `X_BINNING`,
`y_end - y_start` a multiple of `Y_BINNING` (hence
`x_end = x_start + binned_width * x_bin` in the table above). The
`SETUP_VALUE` and `ADC_OFFSET` calibrations depend on the binning factors and
must be re-programmed after changing them.

### Programming notes

- The canonical setup order for the readout geometry is: window registers
  first (`0x1C..0x23`), then the binning factors (`0x0C`, `0x0D`).
- Multi-byte values have no atomic latch; write both halves of an LE16 pair
  before starting an acquisition.
- To abort a pending triggered exposure: write `TRIGGER_MODE = 0` (free-run
  lets the sequencer terminate), reprogram exposure, then restore
  `TRIGGER_MODE = 0x31` and drain `STATUS.FRAME_READY` by reading out frames
  until the bit clears.

## Fake-camera model

The fake implements the register file as `i2c_regs[CAMREG_COUNT]`
([fake-fw-dev/dcam_internal.h](../fake-fw-dev/dcam_internal.h)); the mailbox
command decode in [fake-fw-dev/mailbox.c](../fake-fw-dev/mailbox.c) routes the
two commands to the handlers in [fake-fw-dev/i2c.c](../fake-fw-dev/i2c.c):

- Command `0x1009` to device `0x14` latches `value` into the addressed
  register (`dcam_i2c_write_reg`). Writes to `STATUS` or out-of-range
  addresses are logged and dropped. Writes to other I2C device addresses
  complete without effect.
- Command `0x1008` to device `0x14` answers from the register file
  (`dcam_i2c_read_reg`); other device addresses report 1, which readers of
  unmodelled parameters treat as the identity default.
- `STATUS` is synthesised live. In free-run mode `FRAME_READY` mirrors the
  isochronous streaming state (the fake produces frames continuously while
  streaming, so a frame is always ready exactly when the stream runs); in
  external-trigger mode it reflects a pending trigger (`trigger_pending > 0`
  in [i2c.c](../fake-fw-dev/i2c.c)), clearing once the armed frame has been
  produced.
- The head keeps its settings across the DCAM `INITIALIZE` software reset —
  it is a separate controller that only resets on power-up (fake process
  start), where binning and both gains default to 1 and everything else to 0.

## Frame metadata reflection

Every streamed frame carries the head register file in its verification
header (`frame_meta_t`, version 3):

```
uint32_t i2c_reg_count;          // == CAMREG_COUNT (layout-drift guard)
uint8_t  i2c_regs[CAMREG_COUNT]; // registers 0x00..0x2F, STATUS live-patched
```

The block is a byte-for-byte snapshot taken when the frame is rendered, so
the test program sees the settings that were in force for that exact frame
and can verify end-to-end that register writes reached the camera head. The
DirectShow test decodes it in the `frame-i2c:` diagnostic line
([test/directshow/directshow.cpp](../test/directshow/directshow.cpp)) and
validates `i2c_reg_count` alongside the feature-count layout guard
([test/directshow/framestats.h](../test/directshow/framestats.h)).
