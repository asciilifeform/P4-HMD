# P4 DRM Display Driver

DRM driver for the [Private Eye P4 head-mounted display](https://www.loper-os.org/?p=752) FPGA interface board connected via SPI.

The display is 720x280 pixels but mounted rotated 90 degrees, so it appears
as 280x720 to the hardware. The driver handles rotation, RLE compression,
and differential updates.

# Caution

The driver and the documentation was generated with the aid of LLMs!!!!!!! It runs stably (including when unit is unplugged and replugged) but may contain bugs! Including exploitable bugs! **All of the text below this point was bot-generated!!!**

## Requirements

- Linux kernel 6.x with DRM, SPI, and GPIO support
- ARM64 with NEON (for optimized rotation)
- Raspberry Pi 4 (or similar device with SPI and GPIO)

## Quick Start

```bash
# Build the module
make

# Run tests (userspace, no hardware needed)
make test

# Generate device tree overlay
make dtsi

# Load the driver
make load

# Unload the driver
make unload
```

## Hardware Setup

Edit `p4_pins.conf` to match your wiring:

```
SPI_BUS         = 0
SPI_CS          = 0
SPI_SPEED       = 28000000
GPIO_ENABLE     = 5
GPIO_READY      = 6
GPIO_NRESET     = 13
GPIO_COLD       = 22
GPIO_NSLEEP     = 17
GPIO_VSYNC      = 27
```

Then generate the device tree overlay:

```bash
make dtsi
```

## SPI Calibration

Before loading the driver for the first time, use the calibration tool to verify
connectivity and determine the optimal SPI speed:

```bash
# Build the calibration tool
make calibrate

# Run calibration (driver must NOT be loaded)
sudo ./p4_calibrate

# Test VSYNC signal
sudo ./p4_calibrate --vblank
```

The tool will:
1. Verify GPIO connectivity (NRESET, ENABLE, READY, COLD, NSLEEP - all required)
2. Ensure NSLEEP is deasserted (FPGA clock running) for calibration
3. Run handshake test (3 iterations):
   - Verify ENABLE/READY handshake
   - Verify COLD=1
   - Send clear packet, verify COLD falls to 0
   - Verify ENABLE/READY handshake again
   - Reset device, verify COLD rises to 1
3. If `--vblank`: measure VSYNC frequency (expects 40-70 Hz)
4. Test SPI echo at speeds from 1-50 MHz
5. Report the maximum reliable speed

The FPGA echoes SPI data when ENABLE is low: each byte sent on MOSI appears on
MISO delayed by one byte. The calibration tool uses this to detect corruption
at various speeds.

### Pattern Test Mode

The calibration tool can also send test patterns to verify the encoder and
transmission independently from the kernel driver:

```bash
# Send checkerboard pattern
sudo ./p4_calibrate --pattern checker

# Send 100 frames of animation
sudo ./p4_calibrate --pattern animate --frames 100

# With verbose packet output and bit reversal
sudo ./p4_calibrate --pattern checker --verbose --bitrev
```

Pattern types: `white`, `black`, `checker`, `vstripes`, `hstripes`, `gradient`, `animate`

This mode uses the same packet encoder (`update.h`) as the driver, sending
properly formatted packets via SPI with edge-triggered READY signal handling.
Useful for debugging display issues without involving the kernel driver.

### Frame Sync Test

The `--test-frames` option verifies the FPGA's new_frame synchronization logic:

```bash
sudo ./p4_calibrate --test-frames
```

This test rapidly fills and clears screen regions using RLE packets. Each
iteration sends:
1. Fill region white with `new_frame=1` (triggers vblank presentation)
2. Clear region black with `new_frame=0` (does NOT trigger presentation)

If the new_frame logic works correctly, the clear operation completes before
the next vblank and the region appears solid white without flickering. The test
uses 1/4 screen regions (~6.3ms each at 8MHz) to stay well under the ~16ms
vblank period.

Example output:
```
P4 Display SPI Calibration Utility
===================================

Resetting device...
  ENABLE=0, READY=0: OK
  ENABLE=1, READY=1: OK
  COLD=1 after reset: OK
Device connectivity verified.

Testing ENABLE/READY handshake and COLD flag (3 iterations)...
  Iteration 1:
    ENABLE/READY handshake: OK
    COLD=1 before send_clear: OK
    send_clear packet sent: OK
    COLD=0 after send_clear: OK
    ENABLE/READY handshake: OK
    COLD=1 after reset: OK
  ...
Handshake and COLD flag behavior verified.

Discovering supported SPI speeds...
  Found 12 supported speeds (0.5 MHz - 62.5 MHz)

Testing SPI speeds...
  Testing   0.5 MHz... OK
  Testing   1.0 MHz... OK
  Testing   2.0 MHz... OK
  Testing   4.0 MHz... OK
  Testing   8.0 MHz... OK
  Testing  28.0 MHz... OK
  Testing  32.0 MHz... ERRORS

===================================
Maximum reliable SPI speed: 28000000 Hz (28.0 MHz)

Recommendation: Update p4_pins.conf:
  SPI_SPEED = 28000000
===================================
```

Update `p4_pins.conf` with the recommended speed before loading the driver.

## Build Targets

| Target | Description |
|--------|-------------|
| `make` | Build kernel module (debug enabled by default) |
| `make P4_DEBUG=0` | Build production (minimal logging) |
| `make P4_DEBUG=1 P4_DEBUG_VERBOSE=1` | Build with verbose logging |
| `make test` | Build and run userspace tests (115 tests) |
| `make calibrate` | Build SPI calibration tool |
| `make dtsi` | Generate device tree overlay from p4_pins.conf |
| `make load` | Load overlay and module |
| `make unload` | Unload module and overlay |
| `make fire` | Crash-safe loading with logging and test patterns |
| `make install` | Install module and overlay for loading on boot |
| `make uninstall` | Remove installed module and overlay |
| `make check-syntax` | Flymake syntax checking (see below) |
| `make patch` | Generate in-tree kernel patch |
| `make clean` | Clean build artifacts |

## Installation for Boot

To install the driver so it loads automatically on boot:

```bash
# Build and install (requires root)
make
sudo make install
```

This will:
1. Install the kernel module to `/lib/modules/$(uname -r)/`
2. Run `depmod -a` to update module dependencies
3. Install the device tree overlay to `/boot/overlays/`
4. Create `/etc/modules-load.d/drm_p4.conf` for auto-loading

After installation, add to `/boot/config.txt` (or `/boot/firmware/config.txt`):

```
dtoverlay=p4_display
```

Then reboot.

To uninstall:

```bash
sudo make uninstall
# Remove dtoverlay=p4_display from /boot/config.txt
# Reboot
```

## Crash-Safe Debugging (`make fire`)

For debugging crashes, `make fire` will:

1. Rebuild everything
2. Clear dmesg and enable full DRM debug output
3. Load the driver in the background
4. Stream dmesg to both terminal and timestamped log file
5. Send test patterns to the display periodically (logged)
6. Sync each line to disk immediately (survives crashes)
7. Clean up when you Ctrl-C

```bash
make fire
# Watch output... Ctrl-C when done
# Log saved to p4_fire_YYYYMMDD_HHMMSS.log
```

If the machine crashes, check the log file after reboot for debug output
up to the crash point.

## Test Patterns

Generate and display test patterns:

```bash
# Generate all patterns to files
./gen_patterns.py

# List available patterns
./gen_patterns.py --list

# Send a pattern directly to the display
./gen_patterns.py --pattern checkerboard > /dev/fb1

# Or generate files first, then send
./gen_patterns.py
cat pattern_checkerboard.raw > /dev/fb1
```

Available patterns:
- `white` - All white (0x00)
- `black` - All black (0xFF)
- `checkerboard` - 8x8 pixel checkerboard
- `hstripes` - Horizontal stripes (8px)
- `vstripes` - Vertical stripes (8px)
- `gradient_h` - Horizontal gradient (dithered)
- `gradient_v` - Vertical gradient (dithered)
- `grid` - Single-pixel grid lines every 8 pixels

## Flymake Support (Emacs)

For syntax checking in Emacs with flymake:

```bash
# Native compilation
make check-syntax CHK_SOURCES=sender.c

# Cross-compile for ARM64
make check-syntax CHK_SOURCES=sender.c CROSS_COMPILE=aarch64-linux-gnu-

# Custom kernel headers location
make check-syntax CHK_SOURCES=sender.c KERNEL_HEADERS=/path/to/linux/include

# Minimal mode (headers only, no full kernel tree)
make check-syntax CHK_SOURCES=sender.c \
    KERNEL_HEADERS_ONLY=1 \
    KERNEL_HEADERS=/path/to/headers
```

Add to your Emacs config:

```elisp
(setq flymake-cc-command "make check-syntax CHK_SOURCES=")
```

## In-Tree Kernel Patch

To generate a patch suitable for kernel submission:

```bash
make patch
```

This creates `p4-display-driver.patch` containing all source files
(except tests.c) in `drivers/gpu/drm/p4/` format with proper
Makefile and Kconfig.

To apply:

```bash
cd /path/to/linux
patch -p1 < p4-display-driver.patch
```

Then add to `drivers/gpu/drm/Makefile`:

```makefile
obj-$(CONFIG_DRM_P4) += p4/
```

And to `drivers/gpu/drm/Kconfig`:

```
source "drivers/gpu/drm/p4/Kconfig"
```

## Module Structure

```
drm_p4_main.c      - DRM callbacks, probe/remove
├── sender.c       - SPI, GPIO, FIFO, drainer thread
├── vblank.c       - vsync IRQ handling with watchdog
├── backlight.c    - Brightness control (0=blank, 1=dim, 2=full)
├── sleep.c        - suspend/resume coordination
├── dpms.c         - power management states
├── rotation.c     - 90° block rotation (scalar)
├── rotate_neon.c  - 90° block rotation (NEON optimized)
└── update.c       - RLE encoding, diff detection, packet generation

drm_boilerplate.h  - DRM connector/pipe/driver structs
p4.h               - Shared definitions, device struct
types.h            - Portable type definitions
display.h          - Display dimensions and timing constants
bitrev.h           - Bit reversal primitive
rotation_ops.h     - Rotation primitives (gather, scatter, transpose)
```

## Protocol

The driver communicates with the display via SPI packets. Each packet has a
command byte followed by type-specific fields.

### Command Byte (first byte of every packet)

```
Bit 7: rle        - Data is RLE encoded (single byte repeated)
Bit 6: new_frame  - First packet of a new frame
Bit 5: bitrev     - Bit-reverse data bytes (FPGA-selectable)
Bits 4..3: reserved (zero)
Bits 2..1: cmd_len - Bytes following cmd before len/data (0, 1, or 3)
Bit 0: always 1
```

### Packet Types

**Flags-only (cmd_len=1, cmd=0x03)** - Update display flags without data:
```
[0x03] [flags]
```
Header size: 2 bytes. Used for DPMS/backlight state changes.

**Data (cmd_len=3, cmd=0x07)** - Data at arbitrary address with flags:
```
[0x07] [flags] [addr_hi] [addr_lo] [len_lo] [len_hi] [data...]
```
Header size: 6 bytes. Used for full refreshes and differential updates.

Note: The cmd byte values shown are base values. OR in 0x80 for RLE, 0x40 for
new_frame, 0x20 for bitrev. Example: RLE data packet with new_frame = 0xC7.

### Length Encoding

The 16-bit length field is shifted left by 1 bit on the wire (LSB of len_lo
is always 0). This gives a maximum payload length of 32767 bytes.

Wire format: `wire_len = len << 1`
Decode: `len = wire_len >> 1`

Example: length 25200 (0x6270) → wire 50400 (0xC4E0) → len_lo=0xE0, len_hi=0xC4

### Display Flags Byte

```
Bit 7: upside_down   - 180° hardware rotation
Bit 6: standby       - Deep sleep (pauses VSYNC)
Bit 5: blank         - Screen blank (fast resume, pauses VSYNC)
Bit 4: low_intensity - Half brightness
Bits 3..0: reserved (zero)
```

### Data Encoding

- **RLE packets** (rle=1): Single data byte repeated `len` times
- **Literal packets** (rle=0): `len` bytes of raw pixel data
- Address is in bytes (big-endian), length is in bytes (little-endian, shifted)
- Maximum packet data size: 4090 bytes (fits in 4KB device buffer with header)

### RLE Efficiency

The encoder uses RLE for runs of 8+ identical bytes. Packet overhead analysis:

| Run Length | Packet Size | Savings |
|------------|-------------|---------|
| 8 bytes    | 7 bytes     | 12.5%   |
| 32 bytes   | 7 bytes     | 78%     |
| 35 bytes (1 row) | 7 bytes | 80%  |
| 25206 bytes (full screen) | 7 bytes | 99.97% |

Each RLE packet has 6-byte header + 1-byte value = 7 bytes total. This means:
- 8-byte runs barely break even (RLE_MIN_RUN threshold)
- Longer runs yield dramatically better compression
- Solid screens compress to just 7 bytes regardless of size

Corner cases (solid screen with disruptions):
- 1 corner pixel changed: ~14 bytes total
- 4 corner pixels changed: ~49 bytes total

### Packet Selection

The encoder automatically selects the optimal packet type:
- Image data → data packet with flags and address
- Flag changes only → flags-only packet

## DPMS / Power States

The driver supports fbdev blanking via the DPMS subsystem. Power states are
managed through `p4_set_power_mode()` which sets mutually exclusive flags:

| Mode | Flags | NSLEEP | Behavior |
|------|-------|--------|----------|
| `P4_POWER_NORMAL` | all clear | High | Full power, VSYNC active |
| `P4_POWER_LOW` | low_intensity=1 | High | Dim display (half brightness) |
| `P4_POWER_BLANK` | blank=1 | High | Screen blank, fast resume |
| `P4_POWER_OFF` | standby=1 | Low* | Deep sleep, FPGA clock stopped |

*NSLEEP is only asserted in `FB_BLANK_POWERDOWN`, not `FB_BLANK_VSYNC_SUSPEND`.

### DPMS State Mapping

| FB Blank State | Power Mode | Notes |
|----------------|------------|-------|
| `FB_BLANK_UNBLANK` | `P4_POWER_NORMAL` | Respects brightness ceiling |
| `FB_BLANK_NORMAL` | `P4_POWER_BLANK` | Screen off, fast resume |
| `FB_BLANK_VSYNC_SUSPEND` | `P4_POWER_OFF` | Deep sleep |
| `FB_BLANK_POWERDOWN` | `P4_POWER_OFF` | Deep sleep + NSLEEP asserted |

### Brightness and Power Interaction

The backlight subsystem controls brightness via `p4_set_brightness()`:
- **Level 0**: Blank (screen off)
- **Level 1**: Low intensity (dim)
- **Level 2**: Full intensity (default)

Brightness changes are ignored while the display is in standby (DPMS off).
The `target_power` from before powerdown is preserved and restored on wake.

This means if the display was at full brightness when it went to sleep, it
will wake up at full brightness regardless of any brightness change attempts
while sleeping.

## Hotplug Support

The driver detects display disconnect/reconnect events and reports them to
userspace via DRM hotplug events. This allows X11/Wayland compositors to
automatically switch to other displays when the P4 is unplugged.

### Detection Mechanism

**Disconnect:** When the READY signal times out during a transfer, the drainer
thread marks the display as disconnected and sends a hotplug event.

**Reconnect:** When READY rises again after being offline, the driver debounces
(50ms), resets the device, and sends a hotplug event.

### DRM Connector Behavior

| State | connector_detect() | get_modes() |
|-------|-------------------|-------------|
| Connected | connected | 1 mode (720x280) |
| Disconnected | disconnected | 0 modes |

The CRTC atomic_check rejects enabling when disconnected (returns -ENODEV),
but allows disabling. This follows the DRM DP MST pattern and ensures X11
can cleanly tear down the display pipeline.

### VSYNC Behavior

When the display is disconnected, VSYNC signals stop (no display to sync with).
The vblank watchdog recognizes disconnected state and suppresses warnings.

## User Flip (180° Rotation)

The driver supports a user-controlled 180° flip that is independent of the DRM
rotation property. This allows switching orientation without affecting X11/Wayland
rotation settings - useful for head-mounted displays that can be used with either eye.

### Runtime Control

```bash
# Read current state
cat /sys/bus/spi/devices/spi*.0/user_flip

# Flip the display 180°
echo 1 | sudo tee /sys/bus/spi/devices/spi*.0/user_flip

# Return to normal
echo 0 | sudo tee /sys/bus/spi/devices/spi*.0/user_flip
```

### Boot-Time Configuration

Set `UPSIDE_DOWN = 1` in `p4_pins.conf` and regenerate the device tree overlay:

```bash
# Edit p4_pins.conf
UPSIDE_DOWN = 1

# Regenerate overlay
make dtsi
```

Or manually add `upside-down;` to the device tree overlay.

### Interaction with xrandr

The user flip is XORed with the DRM rotation:
- User flip = 0, xrandr normal → normal display
- User flip = 1, xrandr normal → 180° flipped
- User flip = 0, xrandr inverted → 180° flipped
- User flip = 1, xrandr inverted → normal display (flips cancel)

This allows:
- Changing user flip without disrupting X11 rotation settings
- X11 rotation changes work as expected
- User flip persists across X11 start/stop

## GPIO Signals

| Signal | Direction | Active | Description |
|--------|-----------|--------|-------------|
| ENABLE | Output | High | Assert during SPI transfer |
| READY | Input | High | High when device can accept data |
| NRESET | Output | Low | Active-low hardware reset |
| COLD | Input | High | High after reset until first data received |
| NSLEEP | Output | Low | Active-low FPGA sleep (stops clock when asserted) |
| VSYNC | Input | High | Hardware vblank signal |

**Note on DT GPIO flags:** All GPIOs use flag=0 in the device tree. The driver
uses physical values (0=low, 1=high) directly with gpiod, so NRESET and NSLEEP
are asserted by writing 0 and deasserted by writing 1.

The driver polls READY before each SPI transfer. If READY doesn't rise within
the timeout (600ms), the device is considered offline and the FIFO is flushed.

**Why 600ms?** The FPGA FIFO can hold ~4KB of data. In the worst case, this could
be filled with minimal 7-byte packets, each with `new_frame=1`. Since each
`new_frame` packet waits for a vblank (20ms at 50Hz), draining a full FIFO of
such packets could take up to 4096/7 × 20ms ≈ 585ms. The 600ms timeout provides
margin for this pathological case.

### NSLEEP Behavior

The NSLEEP pin controls the FPGA's clock. When driven low, the FPGA stops its
internal clock and enters a low-power state. The pin is externally pulled high.

NSLEEP is automatically managed by the driver:
- **Asserted (low)** after FIFO drains during system suspend or DPMS powerdown
- **Deasserted (high)** before any SPI communication on resume or DPMS powerup

During FPGA sleep, vblank interrupts are not generated and READY remains low.
The calibration tool ensures NSLEEP is deasserted before testing.

## Fakemode (Testing Without Hardware)

### Test Scripts

```bash
# Unit tests (no hardware needed)
make test                    # 115 algorithm tests

# FPS measurement
sudo ./test_fps.sh           # Measure frame rate
```

## Debug Output

### Log Levels

| Level | Build Command | Description |
|-------|---------------|-------------|
| 0 | `make P4_DEBUG=0` | Production - only essential events |
| 1 | `make P4_DEBUG=1` | Debug (default) - state transitions |
| 2 | `make P4_DEBUG=1 P4_DEBUG_VERBOSE=1` | Verbose - per-vblank traces |

### Enable DRM Debug

```bash
# Enable DRM driver debug messages
echo 0x1ff | sudo tee /sys/module/drm/parameters/debug

# Enable dynamic debug for module
echo 'module drm_p4 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control

# Follow output
dmesg -wT
```

### Always-On Messages (any build)

These appear in dmesg regardless of debug level:
- Probe success/failure
- Suspend/resume
- DPMS powerdown/powerup
- Device timeouts and recovery
- SPI errors

See `HARDWARE_TESTING.md` for full list.

## Files

| File | Description |
|------|-------------|
| `Makefile` | Out-of-tree build system |
| `Makefile.intree` | In-tree kernel Makefile (used by `make patch`) |
| `Kconfig` | Kernel config options (used by `make patch`) |
| `p4_pins.conf` | Pin configuration |
| `gen_dtsi.sh` | DTSI generator script |
| `gen_patterns.py` | Test pattern generator (stdout-based) |
| `fb_pattern.py` | Test pattern sender (mmap-based, preferred) |
| `verify_output.py` | Output verification script |
| `test_fps.sh` | FPS measurement script |
| `hw_test_suite.sh` | Hardware test suite |
| `p4_setmode.c` | Display rotation and pattern tool |
| `p4_calibrate.c` | SPI calibration and pattern test tool |
| `display.h` | Display dimensions and timing constants |
| `bitrev.h` | Bit reversal macro (shared) |
| `rotation_ops.h` | Rotation primitives (shared) |
| `tests.c` | Userspace test suite (not included in patch) |
| `CHANGELOG.md` | Version history and changes |
| `HARDWARE_TESTING.md` | Hardware testing guide |
| `README.md` | This file |

## License

SPDX-License-Identifier: GPL-2.0
