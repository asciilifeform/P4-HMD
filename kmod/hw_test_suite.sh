#!/bin/bash
# Complete hardware test suite for P4 display driver
# Run this on the actual hardware to verify everything works
#
# Usage: ./hw_test_suite.sh [--log-level LEVEL]
#   LEVEL: 0 = production (no debug)
#          1 = debug (default, P4_DEBUG=1)
#          2 = verbose (P4_DEBUG=1, P4_DEBUG_VERBOSE=1)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
LOG_LEVEL=1  # Default to debug

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --log-level|-l)
            LOG_LEVEL="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--log-level LEVEL]"
            echo "  LEVEL: 0 = production (no debug)"
            echo "         1 = debug (default)"
            echo "         2 = verbose (debug + per-vblank traces)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

pass() { echo -e "${GREEN}PASS${NC}: $1"; ((PASS++)) || true; }
fail() { echo -e "${RED}FAIL${NC}: $1"; ((FAIL++)) || true; }
info() { echo -e "${YELLOW}INFO${NC}: $1"; }

echo "========================================"
echo "P4 Hardware Test Suite"
echo "========================================"
echo

# Test 1: Module loading
test_module_load() {
    info "Test: Module loading (log level $LOG_LEVEL)"
    
    # Unload module if present
    sudo rmmod drm_p4 2>/dev/null || true
    sleep 0.5
    
    # Ensure fakemode overlay is NOT loaded
    if ls /proc/device-tree/soc/spi@7e215*/p4* >/dev/null 2>&1; then
        info "Removing fakemode overlay (spi1)..."
        sudo dtoverlay -r p4_fakemode 2>/dev/null || true
        sleep 0.5
    fi
    
    # Check if production overlay is already loaded on spi0
    local need_overlay=1
    if ls /proc/device-tree/soc/spi@7e204*/p4* >/dev/null 2>&1; then
        info "Production overlay already loaded on spi0"
        need_overlay=0
    fi
    
    # CRITICAL: Check if spi0.0 exists and what compatible string it has
    # Even if our DT overlay is loaded, spi0.0 might still be the old spidev device
    # This can happen if spidev was created before our overlay was applied
    if [ -e /sys/bus/spi/devices/spi0.0 ]; then
        local spi_compat=$(cat /sys/bus/spi/devices/spi0.0/of_node/compatible 2>/dev/null | tr '\0' ' ')
        if echo "$spi_compat" | grep -q "spidev"; then
            echo
            fail "spi0.0 is using spidev device tree node (missing GPIO definitions)"
            echo
            info "The default spidev device was created at boot from 'dtparam=spi=on'."
            info "Our overlay must be loaded AT BOOT to replace spidev, not at runtime."
            echo
            
            # Check if overlay is already in config.txt
            local config_file=""
            if [ -f /boot/firmware/config.txt ]; then
                config_file="/boot/firmware/config.txt"
            elif [ -f /boot/config.txt ]; then
                config_file="/boot/config.txt"
            fi
            
            local overlay_installed=0
            if [ -f /boot/overlays/p4_display.dtbo ] || [ -f /boot/firmware/overlays/p4_display.dtbo ]; then
                overlay_installed=1
            fi
            
            local overlay_in_config=0
            if [ -n "$config_file" ] && grep -q "dtoverlay=p4_display" "$config_file" 2>/dev/null; then
                overlay_in_config=1
            fi
            
            if [ "$overlay_installed" -eq 1 ] && [ "$overlay_in_config" -eq 1 ]; then
                info "Overlay is installed and in config.txt - a reboot is needed."
                info "Run: sudo reboot"
            else
                info "TO FIX: Install the overlay for boot-time loading:"
                echo
                
                # Build overlay if needed
                if [ ! -f p4_display.dtbo ]; then
                    info "Building overlay..."
                    ./gen_dtsi.sh p4_pins.conf p4_display.dtsi > /dev/null 2>&1
                    dtc -@ -I dts -O dtb -o p4_display.dtbo p4_display.dtsi 2>/dev/null
                fi
                
                # Determine overlay directory
                local overlay_dir="/boot/overlays"
                [ -d /boot/firmware/overlays ] && overlay_dir="/boot/firmware/overlays"
                
                echo "  sudo cp p4_display.dtbo $overlay_dir/"
                if [ -n "$config_file" ]; then
                    echo "  # Add to $config_file (after dtparam=spi=on):"
                    echo "  dtoverlay=p4_display"
                fi
                echo "  sudo reboot"
                echo
                
                # Offer to do it automatically
                read -p "Install overlay now and add to config.txt? [y/N] " -n 1 -r
                echo
                if [[ $REPLY =~ ^[Yy]$ ]]; then
                    info "Installing overlay..."
                    sudo cp p4_display.dtbo "$overlay_dir/" || { fail "Could not copy overlay"; return; }
                    
                    if [ -n "$config_file" ] && ! grep -q "dtoverlay=p4_display" "$config_file" 2>/dev/null; then
                        info "Adding to $config_file..."
                        echo "dtoverlay=p4_display" | sudo tee -a "$config_file" > /dev/null
                    fi
                    
                    pass "Overlay installed"
                    info "Reboot required. Run: sudo reboot"
                fi
            fi
            return
        fi
    fi
    
    if [ "$need_overlay" -eq 1 ]; then
        info "Building and loading production overlay..."
        
        # Generate dtsi from p4_pins.conf
        if [ ! -f p4_pins.conf ]; then
            fail "p4_pins.conf not found"
            return
        fi
        
        if ! ./gen_dtsi.sh p4_pins.conf p4_display.dtsi > /dev/null 2>&1; then
            fail "Failed to generate p4_display.dtsi"
            return
        fi
        
        # Compile overlay
        if ! dtc -@ -I dts -O dtb -o p4_display.dtbo p4_display.dtsi 2>/dev/null; then
            fail "Failed to compile device tree overlay"
            return
        fi
        
        # Unbind spidev from spi0.0 if it's claiming the chipselect
        # This is necessary because the DT overlay's "status=disabled" only works
        # if applied before spidev binds
        if [ -e /sys/bus/spi/devices/spi0.0 ]; then
            info "Unbinding spidev from spi0.0..."
            # First try to unbind from spidev driver
            if [ -e /sys/bus/spi/drivers/spidev/spi0.0 ]; then
                echo "spi0.0" | sudo tee /sys/bus/spi/drivers/spidev/unbind > /dev/null 2>&1 || true
            fi
            # Also try generic spi device unbind
            echo "spi0.0" | sudo tee /sys/bus/spi/devices/spi0.0/driver/unbind > /dev/null 2>&1 || true
            sleep 0.2
        fi
        
        # Unload spidev module if loaded (frees the device)
        sudo rmmod spidev 2>/dev/null || true
        sleep 0.2
        
        # Load overlay
        if ! sudo dtoverlay p4_display.dtbo 2>/dev/null; then
            # Try alternate location
            sudo cp p4_display.dtbo /tmp/
            if ! sudo dtoverlay /tmp/p4_display.dtbo 2>/dev/null; then
                fail "Failed to load device tree overlay"
                info "Check dmesg for details - spidev may still be bound to spi0.0"
                return
            fi
        fi
        
        sleep 0.5
        
        # Verify overlay loaded on spi0
        if ! ls /proc/device-tree/soc/spi@7e204*/p4* >/dev/null 2>&1; then
            fail "Overlay did not load correctly (not found on spi0)"
            return
        fi
        
        pass "Device tree overlay loaded on spi0"
    fi
    
    # Double-check we're on spi0, not spi1
    if ls /proc/device-tree/soc/spi@7e215*/p4* >/dev/null 2>&1; then
        fail "Fakemode overlay still present on spi1"
        return
    fi
    
    # Verify device tree node exists and has correct compatible string
    local dt_node=$(ls -d /proc/device-tree/soc/spi@7e204*/p4* 2>/dev/null | head -1)
    if [ -z "$dt_node" ]; then
        fail "No P4 device tree node found under spi0"
        info "Check: ls /proc/device-tree/soc/spi@7e204*/"
        ls /proc/device-tree/soc/spi@7e204*/ 2>/dev/null || true
        return
    fi
    
    info "Device tree node: $dt_node"
    
    # Check compatible string
    local compat=$(cat "$dt_node/compatible" 2>/dev/null | tr '\0' ' ')
    info "Compatible string: '$compat'"
    if ! echo "$compat" | grep -q "example,p4-display"; then
        fail "Wrong compatible string in device tree (expected 'example,p4-display')"
        return
    fi
    
    # Check if device is already bound to a driver
    local spi_dev="/sys/bus/spi/devices/spi0.0"
    if [ -e "$spi_dev" ]; then
        local bound_drv=$(basename $(readlink "$spi_dev/driver" 2>/dev/null) 2>/dev/null || echo "none")
        info "spi0.0 currently bound to: $bound_drv"
        if [ "$bound_drv" != "none" ] && [ "$bound_drv" != "p4-rotate" ]; then
            info "Unbinding $bound_drv from spi0.0..."
            echo "spi0.0" | sudo tee /sys/bus/spi/drivers/$bound_drv/unbind > /dev/null 2>&1 || true
            sleep 0.2
        fi
    else
        # SPI device doesn't exist - this is the problem!
        fail "spi0.0 device does not exist in sysfs"
        info "Device tree node exists but SPI device wasn't created"
        info "Checking SPI controller status..."
        
        # Check if spi0 controller exists
        if [ -e /sys/bus/spi/devices/spi0.0 ]; then
            info "spi0 controller exists"
        else
            info "Available SPI devices:"
            ls -la /sys/bus/spi/devices/ 2>/dev/null || echo "  (none)"
        fi
        
        # Check if SPI is enabled in config
        info "SPI overlays loaded:"
        sudo dtoverlay -l 2>/dev/null || true
        
        # Check spi0 status in device tree
        local spi0_status=$(cat /proc/device-tree/soc/spi@7e204000/status 2>/dev/null | tr -d '\0')
        info "spi0 status in device tree: '$spi0_status'"
        
        if [ "$spi0_status" != "okay" ]; then
            info "SPI0 is not enabled! Add 'dtparam=spi=on' to /boot/config.txt"
        fi
        
        return
    fi
    
    # Build module with appropriate debug flags
    make clean > /dev/null 2>&1
    
    local make_flags=""
    case $LOG_LEVEL in
        0)
            make_flags="P4_DEBUG=0"
            info "Building with debug disabled"
            ;;
        1)
            make_flags="P4_DEBUG=1"
            info "Building with debug enabled"
            ;;
        2)
            make_flags="P4_DEBUG=1 P4_DEBUG_VERBOSE=1"
            info "Building with verbose debug enabled"
            ;;
    esac
    
    if ! make $make_flags > /dev/null 2>&1; then
        fail "Module build"
        return
    fi
    
    # Enable dynamic debug for DRM if using debug build
    if [ "$LOG_LEVEL" -ge 1 ]; then
        echo "module drm_p4 +p" | sudo tee /sys/kernel/debug/dynamic_debug/control > /dev/null 2>&1 || true
    fi
    
    # Load
    if sudo insmod drm_p4.ko; then
        pass "Module load"
    else
        fail "Module load"
        return
    fi
    
    # If device exists but wasn't auto-bound, try manual bind
    sleep 0.2
    if [ -e /sys/bus/spi/devices/spi0.0 ] && [ ! -e /sys/bus/spi/devices/spi0.0/driver ]; then
        info "Device not auto-bound, attempting manual bind..."
        
        # Check driver_override
        local override=$(cat /sys/bus/spi/devices/spi0.0/driver_override 2>/dev/null)
        if [ -n "$override" ]; then
            info "driver_override is set to: '$override'"
        fi
        
        # Try to bind
        if echo "spi0.0" | sudo tee /sys/bus/spi/drivers/p4-rotate/bind > /dev/null 2>&1; then
            sleep 0.3
            if [ -e /sys/bus/spi/devices/spi0.0/driver ]; then
                local bound=$(basename $(readlink /sys/bus/spi/devices/spi0.0/driver))
                pass "Manual bind successful: $bound"
            else
                fail "Bind command succeeded but device still unbound"
                dmesg | tail -10 | grep -iE "p4|spi" || true
            fi
        else
            fail "Manual bind command failed"
            info "Checking dmesg for errors..."
            dmesg | tail -10 | grep -iE "p4|spi" || true
        fi
    elif [ -e /sys/bus/spi/devices/spi0.0/driver ]; then
        local bound=$(basename $(readlink /sys/bus/spi/devices/spi0.0/driver))
        if [ "$bound" = "p4-rotate" ]; then
            pass "Device auto-bound to p4-rotate"
        else
            fail "Device bound to wrong driver: $bound"
        fi
    fi
    
    # Disable fbcon immediately after load
    for vt in /sys/class/vtconsole/vtcon*/bind; do
        echo 0 | sudo tee "$vt" > /dev/null 2>&1 || true
    done
    
    # Verify
    if lsmod | grep -q drm_p4; then
        pass "Module in lsmod"
    else
        fail "Module not in lsmod"
        return
    fi
    
    # Check DRM device - must be from OUR driver, not stale from previous run
    sleep 1  # Give probe time to complete
    local found_p4=0
    for card in /dev/dri/card*; do
        if [ -e "$card" ]; then
            local cardname=$(basename $card)
            local sysfs_path="/sys/class/drm/$cardname/device/driver/name"
            local drv_name=""
            
            # Try different paths to find driver name
            if [ -e "$sysfs_path" ]; then
                drv_name=$(cat "$sysfs_path" 2>/dev/null || echo "")
            elif [ -L "/sys/class/drm/$cardname/device/driver" ]; then
                drv_name=$(basename $(readlink "/sys/class/drm/$cardname/device/driver") 2>/dev/null || echo "")
            fi
            
            info "Checking $card: driver='$drv_name'"
            
            if [ "$drv_name" = "p4-rotate" ]; then
                found_p4=1
                pass "DRM device created: $card"
                break
            fi
        fi
    done
    
    if [ "$found_p4" -eq 0 ]; then
        fail "No P4 DRM device found (probe may have failed - check dmesg)"
        info "Listing all DRM devices:"
        ls -la /sys/class/drm/ 2>/dev/null | grep card || true
        dmesg | tail -30 | grep -iE "p4|spi" || true
        # Don't return - try to continue anyway to check if display works
    fi
    
    # Verify probe completed by checking for success message
    if dmesg | tail -30 | grep -q "p4.*registered"; then
        pass "Probe completed successfully"
    else
        fail "Probe did not complete (no 'registered' message in dmesg)"
        dmesg | tail -30 | grep -iE "p4|spi" || true
        return
    fi
}

# Test 2: GPIO connectivity
test_gpio() {
    info "Test: GPIO signals"
    
    # Check dmesg for GPIO errors
    if dmesg | tail -50 | grep -qi "gpio.*error\|gpio.*fail"; then
        fail "GPIO errors in dmesg"
    else
        pass "No GPIO errors"
    fi
    
    # Verify IRQs registered
    if dmesg | tail -50 | grep -qi "IRQ OK\|ready.*irq\|vsync.*irq"; then
        pass "IRQs registered"
    else
        info "IRQ registration not confirmed (check dmesg)"
    fi
}

# Test 3: Basic display output
test_display_output() {
    info "Test: Basic display output"
    
    if [ ! -x ./p4_setmode ]; then
        make p4_setmode > /dev/null 2>&1
    fi
    
    # White pattern
    if sudo ./p4_setmode normal --pattern white; then
        pass "White pattern sent"
    else
        fail "White pattern failed"
        return
    fi
    
    sleep 1
    
    # Black pattern
    if sudo ./p4_setmode normal --pattern black; then
        pass "Black pattern sent"
    else
        fail "Black pattern failed"
    fi
    
    sleep 1
    
    # Checker pattern
    if sudo ./p4_setmode normal --pattern checker; then
        pass "Checker pattern sent"
    else
        fail "Checker pattern failed"
    fi
    
    echo "  >> Visually verify: display should show checker pattern"
}

# Test 4: Rotation
test_rotation() {
    info "Test: Rotation modes"
    
    local rotations=("normal" "left" "inverted" "right")
    
    for rot in "${rotations[@]}"; do
        if sudo ./p4_setmode "$rot" --pattern grid; then
            pass "Rotation: $rot"
        else
            fail "Rotation: $rot"
        fi
        sleep 0.5
    done
    
    echo "  >> Visually verify: grid pattern rotated correctly for each mode"
}

# Test 5: DPMS / power management
test_dpms() {
    info "Test: DPMS power management"
    
    # Find card
    local card=$(ls /dev/dri/card* 2>/dev/null | head -1)
    if [ -z "$card" ]; then
        fail "No DRM card for DPMS test"
        return
    fi
    
    # Try xset if available
    if command -v xset > /dev/null 2>&1 && [ -n "$DISPLAY" ]; then
        xset dpms force off 2>/dev/null && sleep 1 && xset dpms force on 2>/dev/null
        pass "DPMS cycle (xset)"
    else
        info "DPMS test skipped (no X display)"
    fi
}

# Test 6: Stress test
test_stress() {
    info "Test: Stress test (10 seconds)"
    
    local errors_before
    errors_before=$(dmesg | grep -ciE "p4.*error|spi.*error" 2>/dev/null) || errors_before=0
    
    # Rapid pattern changes
    for i in $(seq 1 50); do
        sudo ./p4_setmode normal --pattern white 2>/dev/null
        sudo ./p4_setmode normal --pattern black 2>/dev/null
    done
    
    local errors_after
    errors_after=$(dmesg | grep -ciE "p4.*error|spi.*error" 2>/dev/null) || errors_after=0
    local new_errors=$((errors_after - errors_before))
    
    if [ "$new_errors" -eq 0 ]; then
        pass "Stress test (no new errors)"
    else
        fail "Stress test ($new_errors new errors)"
    fi
}

# Test 7: Module unload
test_module_unload() {
    info "Test: Module unload"
    
    if sudo rmmod drm_p4; then
        pass "Module unload"
    else
        fail "Module unload"
    fi
    
    if ! lsmod | grep -q drm_p4; then
        pass "Module removed from lsmod"
    else
        fail "Module still in lsmod"
    fi
}

# Run all tests
main() {
    test_module_load
    
    # Check if module loaded and probed successfully before continuing
    if [ $FAIL -gt 0 ]; then
        echo
        echo "========================================"
        echo -e "${RED}Module load/probe failed - skipping remaining tests${NC}"
        echo "========================================"
        echo -e "Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
        echo "========================================"
        echo
        echo "Recent dmesg (p4/spi related):"
        dmesg | tail -50 | grep -iE "p4|spi" || echo "  (none)"
        return 1
    fi
    
    echo
    test_gpio
    echo
    test_display_output
    echo
    test_rotation
    echo
    test_dpms
    echo
    test_stress
    echo
    test_module_unload
    
    echo
    echo "========================================"
    echo -e "Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
    echo "========================================"
    
    # Show any errors from dmesg
    echo
    echo "Recent dmesg errors (if any):"
    dmesg | tail -100 | grep -iE "p4.*error|p4.*fail|spi.*error" || echo "  (none)"
    
    [ $FAIL -eq 0 ]
}

main "$@"
