# P4 Display Driver - Hardware Testing Guide

## Pin Mapping

### Raspberry Pi Physical to BCM GPIO Translation

| RPi Physical Pin | BCM GPIO | Notes      |
|------------------|----------|------------|
| Pin 3            | GPIO2    | I2C SDA    |
| Pin 5            | GPIO3    | I2C SCL    |
| Pin 7            | GPIO4    | GPCLK0     |
| Pin 8            | GPIO14   | UART TXD   |
| Pin 10           | GPIO15   | UART RXD   |
| Pin 11           | GPIO17   |            |
| Pin 12           | GPIO18   | PCM CLK    |
| Pin 13           | GPIO27   |            |
| Pin 15           | GPIO22   |            |
| Pin 16           | GPIO23   |            |
| Pin 18           | GPIO24   |            |
| Pin 19           | GPIO10   | SPI0 MOSI  |
| Pin 21           | GPIO9    | SPI0 MISO  |
| Pin 22           | GPIO25   |            |
| Pin 23           | GPIO11   | SPI0 SCLK  |
| Pin 24           | GPIO8    | SPI0 CE0   |
| Pin 26           | GPIO7    | SPI0 CE1   |
| Pin 29           | GPIO5    |            |
| Pin 31           | GPIO6    |            |
| Pin 32           | GPIO12   | PWM0       |
| Pin 33           | GPIO13   | PWM1       |
| Pin 35           | GPIO19   | PCM FS     |
| Pin 36           | GPIO16   |            |
| Pin 37           | GPIO26   |            |
| Pin 38           | GPIO20   | PCM DIN    |
| Pin 40           | GPIO21   | PCM DOUT   |

Power pins: 1, 17 (3.3V); 2, 4 (5V); 6, 9, 14, 20, 25, 30, 34, 39 (GND)

### Required Signals

The driver requires these signals (actual pin assignments are in the device tree):

| Signal   | Direction | Description                            |
|----------|-----------|----------------------------------------|
| SPI MOSI | Out       | SPI data to display                    |
| SPI SCLK | Out       | SPI clock                              |
| SPI CS   | Out       | SPI chip select                        |
| RESET    | Out       | Active low device reset                |
| ENABLE   | Out       | Enable signal for transfers            |
| READY    | In        | Device ready (rising edge IRQ)         |
| COLD     | In        | Device needs full refresh              |
| VSYNC    | In        | Optional hardware vsync (rising edge)  |

### Device Tree Configuration

Pin assignments are defined in `p4_display.dtsi`. Modify the `*-gpios` properties to match your wiring:

```dts
reset-gpios = <&gpio NN GPIO_ACTIVE_LOW>;
enable-gpios = <&gpio NN GPIO_ACTIVE_HIGH>;
ready-gpios = <&gpio NN GPIO_ACTIVE_HIGH>;
cold-gpios = <&gpio NN GPIO_ACTIVE_HIGH>;
vsync-gpios = <&gpio NN GPIO_ACTIVE_HIGH>;  /* optional */
```

## Building for Hardware

```bash
# Build the driver
make clean
make

# Or for in-tree build
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
```

## SPI Calibration (First-Time Setup)

Before loading the driver for the first time, run the calibration tool to verify
wiring and determine the optimal SPI speed:

```bash
# Build calibration tool
make calibrate

# Run (driver must NOT be loaded)
sudo ./p4_calibrate --verbose

# Test VSYNC signal (if GPIO_VSYNC is configured)
sudo ./p4_calibrate --vblank
```

The tool:
1. Loads spidev module temporarily
2. Opens `/dev/gpiochipN` (chardev interface, no sysfs required)
3. Verifies GPIO connectivity (ENABLE, READY, RESET, COLD - all required)
4. Runs handshake/COLD test 3 times:
   - Verify ENABLE/READY handshake
   - Verify COLD=1, send clear packet, verify COLD=0
   - Verify ENABLE/READY handshake again
   - Reset, verify COLD=1
5. If `--vblank`: measures VSYNC frequency (expects 40-70 Hz)
6. Discovers supported SPI speeds from the driver
7. Tests each speed with SPI echo
8. Reports maximum reliable speed
9. Cleans up (releases GPIO lines, unloads spidev)

**How SPI echo works:** When ENABLE is low, the FPGA echoes each byte received
on MOSI back on MISO, delayed by one byte. Corruption indicates the speed is
too high for your wiring.

Options:
- `--config FILE` - Use alternate config file (default: p4_pins.conf)
- `--verbose` - Show detailed mismatch information

If calibration fails at all speeds, check:
- Device power supply
- SPI wiring (MOSI, MISO, SCLK, CS)
- GPIO wiring (ENABLE, READY, RESET)

## Loading the Driver

```bash
# Load the module
sudo insmod drm_p4.ko

# Verify it loaded
lsmod | grep drm_p4
dmesg | tail -20

# Check DRM device created
ls -la /dev/dri/

# Disable fbcon to avoid conflicts
for vt in /sys/class/vtconsole/vtcon*/bind; do
    echo 0 | sudo tee "$vt"
done
```

## Debug Mode

### Log Levels

The driver has three log levels:

| Level | Build Command | What's Logged |
|-------|---------------|---------------|
| 0 (production) | `make P4_DEBUG=0` | Only essential events: probe, suspend, resume, DPMS, timeouts, errors |
| 1 (debug) | `make P4_DEBUG=1` | Above + state transitions, full refresh, send_clear |
| 2 (verbose) | `make P4_DEBUG=1 P4_DEBUG_VERBOSE=1` | Above + per-vblank traces, warm retries |

### Building with Debug

```bash
# Production build (minimal logging)
make clean && make P4_DEBUG=0

# Debug build (default, recommended for testing)
make clean && make P4_DEBUG=1

# Verbose build (for deep debugging)
make clean && make P4_DEBUG=1 P4_DEBUG_VERBOSE=1
```

### Enabling Dynamic Debug

After loading, enable DRM debug output:

```bash
# Enable dynamic debug for the module
echo 'module drm_p4 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control

# Enable all DRM debug categories
echo 0x1ff | sudo tee /sys/module/drm/parameters/debug
```

### Using the Hardware Test Suite

The test suite supports log level selection:

```bash
# Default (debug level 1)
./hw_test_suite.sh

# Production (no debug)
./hw_test_suite.sh --log-level 0

# Verbose (all traces)
./hw_test_suite.sh --log-level 2
```

### Kernel Log Messages

**Always logged (any build):**
- `registered /dev/dri/cardN (WxH @ Hz SPI)` - successful probe
- `removing` - driver removal
- `suspending` / `resuming` - power management
- `DPMS powerdown` / `DPMS powerup` - display power
- `READY timeout, waiting for device` - device not responding
- `device cold, resetting` - device was reset/replugged
- `device reconnected (was cold)` - recovery after replug
- `device ready (warm), retrying` - recovery from missed IRQ
- `SPI transfer error` - communication failure

Monitor debug output:
```bash
# Follow kernel messages
sudo dmesg -wH

# Or with timestamps
sudo dmesg -wT
```

## FPS Measurement

### Method 1: Using debugfs (if available)

```bash
# Check if packet counter exists
cat /sys/kernel/debug/p4/stats

# Monitor packets per second
watch -n 1 'cat /sys/kernel/debug/p4/stats'
```

### Method 2: Using dmesg timestamps

```bash
# Enable debug mode, then look for vblank/frame markers
sudo dmesg -wT | grep -E "(vblank|frame|send_diff)"
```

### Method 3: FPS test script

See `test_fps.sh`.

## Error Monitoring

### Watch for errors
```bash
sudo dmesg -wT | grep -iE "(error|fail|timeout|offline|cold|recover)"
```

### Common messages and meanings

| Message | Level | Meaning | Action |
|---------|-------|---------|--------|
| `registered /dev/dri/cardN` | info | Probe succeeded | Normal |
| `suspending` / `resuming` | info | Power state change | Normal |
| `DPMS powerdown` / `powerup` | info | Display power change | Normal |
| `READY timeout, waiting` | warn | Device not responding | Check wiring, will auto-retry |
| `device cold, resetting` | warn | Device was reset/replugged | Will auto-recover |
| `device reconnected (was cold)` | info | Recovered after replug | Normal recovery |
| `device ready (warm), retrying` | info | Missed IRQ, retrying | Normal recovery |
| `SPI transfer error` | error | SPI communication failed | Check wiring, SPI speed |
| `device not ready at probe` | warn | Device offline at load | Drainer will retry |
| `device not ready at resume` | warn | Device offline at wake | Drainer will retry |

## Basic Functionality Tests

### 1. Display a test pattern
```bash
# Using p4_setmode (recommended)
sudo ./p4_setmode normal --pattern checker

# Or using modetest (from libdrm)
modetest -M p4 -s 32:720x280 -P 31@32:720x280
```

### 2. Test all rotations
```bash
for rot in normal left right inverted; do
    sudo ./p4_setmode $rot --pattern arrow
    sleep 1
done
```

### 3. Hardware stress test
```bash
sudo ./hw_test_suite.sh
```

## Performance Tuning

### SPI Speed
The default SPI speed is set in the device tree. To test different speeds:

```bash
# Check current speed
cat /sys/class/spi_master/spi0/spi0.0/max_speed_hz

# Typical values: 8000000 (8MHz), 28000000 (28MHz), 32000000 (32MHz)
```

### Vblank Rate
Software vblank runs at 50Hz. Hardware vsync uses the display's native rate.

## Troubleshooting

### No display output
1. Check module loaded: `lsmod | grep drm_p4`
2. Check DRM device: `ls /dev/dri/card*`
3. Check dmesg for errors: `dmesg | grep -i p4`
4. Verify SPI is enabled: `ls /dev/spidev*`

### Garbled display
1. Check SPI speed (may be too fast)
2. Verify bit order (bitrev flag)
3. Check rotation setting matches physical orientation

### Framebuffer Byte Order and Orientation

The hardware framebuffer is 280×720 pixels (35 bytes × 720 rows = 25,200 bytes).

**Byte 0** corresponds to the **top-left 8 pixels** (row 0, columns 0-7) when viewing
the display in portrait orientation (280 wide × 720 tall). In the arrow test pattern,
this is where the vertical bar marker appears.

Byte layout:
- Byte 0: row 0, pixels 0-7 (top-left)
- Byte 34: row 0, pixels 272-279 (top-right)
- Byte 35: row 1, pixels 0-7
- Byte 25165: row 719, pixels 0-7 (bottom-left)
- Byte 25199: row 719, pixels 272-279 (bottom-right)

Within each byte, **MSB-first** ordering: bit 7 = leftmost pixel, bit 0 = rightmost.

**If the display appears mirrored:**
- Horizontal flip: reverse bit order within bytes (toggle bitrev flag)
- Vertical flip: reverse row order
- 180° rotation: both of the above

The arrow pattern (`p4_setmode normal --pattern arrow`) has distinct corner markers
to help diagnose orientation issues:
- TL (in portrait): vertical bar (16×48)
- TR (in portrait): square (~24×24 or ~40×40)
- BL (in portrait): square (~40×40 or ~24×24)
- BR (in portrait): horizontal bar (48×16)

### Display freezes
1. Check READY pin connection
2. Monitor for timeout errors in dmesg
3. Check if device went offline (should auto-recover)

### Kernel panic / oops
1. Check dmesg for stack trace
2. Verify kernel headers match running kernel
3. Try with debug build for more info
