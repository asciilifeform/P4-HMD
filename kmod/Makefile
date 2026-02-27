# SPDX-License-Identifier: GPL-2.0
#
# DRM driver for P4 rotated monochrome SPI display
#
# Build (native on ARM64):
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
#
# Cross-compile:
#   make KDIR=/path/to/kernel CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64
#
# Flymake syntax checking (for Emacs):
#   make check-syntax                     # native
#   make check-syntax CROSS_COMPILE=aarch64-linux-gnu-  # cross-compile
#   make check-syntax KERNEL_HEADERS=/path/to/linux/include  # custom headers
#
# Generate in-tree patch:
#   make patch
#

# ===== Userland exclusion list =====
# These files are NOT part of the kernel driver and should be excluded from
# both the kbuild object list and the in-tree patch. Keep this list in sync!
#
# Userspace tools:
#   tests.c        - userspace test suite
#   p4_setmode.c   - DRM mode setter tool
#   p4_calibrate.c - SPI calibration and test tool
#
# Build artifacts (not in distribution):
#   bench_*.c, debug_*.c, *_tests.c, update_full.c
#
USERLAND_EXCLUDE := tests.c p4_setmode.c p4_calibrate.c \
                    bench_%.c debug_%.c %_tests.c update_full.c

ifneq ($(KERNELRELEASE),)

obj-m := drm_p4.o

# Find all driver .c files, excluding userland and special cases:
#   rotate_neon.c - handled separately below (ARM64/ARM only)
# Use $(src) for correct path in kbuild context
DRIVER_SRCS := $(filter-out rotate_neon.c $(USERLAND_EXCLUDE), $(notdir $(wildcard $(src)/*.c)))
drm_p4-y := $(DRIVER_SRCS:.c=.o)

# NEON-optimized rotation (ARM64 and ARM with NEON only)
drm_p4-$(CONFIG_ARM64) += rotate_neon.o
drm_p4-$(CONFIG_ARM) += rotate_neon.o

# Build ID for module identification (auto-generated timestamp)
P4_BUILD_ID := "v130-$(shell date +%Y%m%d-%H%M%S)"
ccflags-y += -DP4_BUILD_ID='$(P4_BUILD_ID)'

# Debug level control (passed from command line)
# P4_DEBUG=0: disable debug output
# P4_DEBUG=1: enable debug output (default in p4.h)
# P4_DEBUG_VERBOSE=1: enable verbose per-vblank traces
ifdef P4_DEBUG
ccflags-y += -DP4_DEBUG=$(P4_DEBUG)
endif
ifdef P4_DEBUG_VERBOSE
ccflags-y += -DP4_DEBUG_VERBOSE=$(P4_DEBUG_VERBOSE)
endif

# Enable NEON intrinsics for rotate_neon.c:
#  - Remove kernel's -mgeneral-regs-only that blocks SIMD
#  - Add compiler's include path for arm_neon.h
CFLAGS_rotate_neon.o := -ffreestanding -isystem $(shell $(CC) -print-file-name=include)
CFLAGS_REMOVE_rotate_neon.o := -mgeneral-regs-only

else

KDIR ?= /lib/modules/$(shell uname -r)/build
CROSS_COMPILE ?=
ARCH ?=

# Pass cross-compile variables to kernel build system
MAKE_OPTS := -C $(KDIR) M=$(CURDIR)
ifdef CROSS_COMPILE
MAKE_OPTS += CROSS_COMPILE=$(CROSS_COMPILE)
endif
ifdef ARCH
MAKE_OPTS += ARCH=$(ARCH)
endif

modules:
ifeq ($(shell uname -m),aarch64)
	@echo "P4: Building kernel module with NEON support (ARM64)"
else ifeq ($(shell uname -m),armv7l)
	@echo "P4: Building kernel module with NEON support (ARM)"
else
	@echo "P4: Building kernel module without NEON ($(shell uname -m))"
endif
	$(MAKE) $(MAKE_OPTS) modules

clean:
	$(MAKE) $(MAKE_OPTS) clean
	rm -f tests p4_setmode p4_calibrate p4_display.dtsi p4_display.dtbo p4-display-driver.patch
	rm -rf .patch-tmp

# Install module and overlay for loading on boot
# Run as: sudo make install
install: modules p4_display.dtbo
	@echo "Installing P4 display driver..."
	@# Install kernel module
	$(MAKE) $(MAKE_OPTS) modules_install
	@depmod -a
	@# Install device tree overlay
	@if [ -d /boot/overlays ]; then \
		cp p4_display.dtbo /boot/overlays/; \
		echo "Installed overlay to /boot/overlays/p4_display.dtbo"; \
	elif [ -d /boot/firmware/overlays ]; then \
		cp p4_display.dtbo /boot/firmware/overlays/; \
		echo "Installed overlay to /boot/firmware/overlays/p4_display.dtbo"; \
	else \
		echo "Warning: Could not find overlays directory"; \
	fi
	@# Create modules-load.d entry for auto-loading
	@mkdir -p /etc/modules-load.d
	@echo "drm_p4" > /etc/modules-load.d/drm_p4.conf
	@echo "Created /etc/modules-load.d/drm_p4.conf"
	@# Remind user to configure config.txt
	@echo ""
	@echo "=== Installation complete ==="
	@echo ""
	@echo "To enable on boot, add the following to /boot/config.txt"
	@echo "(or /boot/firmware/config.txt on newer systems):"
	@echo ""
	@echo "  dtoverlay=p4_display"
	@echo ""
	@echo "Then reboot."

uninstall:
	@echo "Uninstalling P4 display driver..."
	@# Remove module (if loaded)
	-rmmod drm_p4 2>/dev/null || true
	@# Remove modules-load.d entry
	@rm -f /etc/modules-load.d/drm_p4.conf
	@# Remove overlay
	@rm -f /boot/overlays/p4_display.dtbo 2>/dev/null || true
	@rm -f /boot/firmware/overlays/p4_display.dtbo 2>/dev/null || true
	@# Note: we don't run modules_install --remove as it's complex
	@echo ""
	@echo "=== Uninstallation complete ==="
	@echo ""
	@echo "Remember to remove 'dtoverlay=p4_display' from /boot/config.txt"
	@echo "and reboot."

test: tests
	./tests

# Mode setter tool (requires libdrm-dev)
setmode: p4_setmode
	@echo "Mode setter built. Run with: sudo ./p4_setmode --list"

p4_setmode: p4_setmode.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ $< -ldrm -I/usr/include/drm

# SPI calibration tool
calibrate: p4_calibrate
	@echo "Calibration tool built. Run with: sudo ./p4_calibrate"

p4_calibrate: p4_calibrate.c update.c update.h display.h types.h
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ p4_calibrate.c update.c

# Tests depend on all sources and headers (wildcard), but only compile testable units
# Include rotate_neon.c on ARM for NEON testing
tests: $(filter-out p4_setmode.c p4_calibrate.c, $(wildcard *.c)) $(wildcard *.h)
ifeq ($(shell uname -m),aarch64)
	@echo "Building tests with NEON support (aarch64)"
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -g -o tests tests.c update.c rotation.c rotate_neon.c
else ifeq ($(shell uname -m),armv7l)
	@echo "Building tests with NEON support (armv7l)"
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -g -mfpu=neon -o tests tests.c update.c rotation.c rotate_neon.c
else
	@echo "Building tests without NEON ($(shell uname -m))"
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -g -o tests tests.c update.c rotation.c
endif

dtsi: p4_display.dtsi

p4_display.dtsi: p4_pins.conf gen_dtsi.sh
	./gen_dtsi.sh

p4_display.dtbo: p4_display.dtsi
	dtc -@ -I dts -O dtb -o $@ $<

load: modules p4_display.dtbo
	@echo "Loading P4 display driver..."
	-sudo rmmod drm_p4 2>/dev/null || true
	-sudo rmmod spidev 2>/dev/null || true
	-sudo dtoverlay -r p4_display 2>/dev/null || true
	sudo modprobe drm
	echo 0x1ff | sudo tee /sys/module/drm/parameters/debug > /dev/null
	sudo dtoverlay p4_display.dtbo
	sudo insmod drm_p4.ko
	@echo "Driver loaded. Debug output: dmesg -w"

unload:
	@echo "Unloading P4 display driver..."
	-sudo rmmod drm_p4 2>/dev/null || true
	-sudo rmmod spidev 2>/dev/null || true
	-sudo dtoverlay -r p4_display 2>/dev/null || true
	@echo "Driver unloaded."

# Crash-safe debugging: rebuild, load in background, tail dmesg until Ctrl-C, then unload
# Logs to p4_fire.log with unbuffered writes for crash recovery
# Periodically sends test patterns to exercise the driver
FIRE_LOG := p4_fire_$(shell date +%Y%m%d_%H%M%S).log
PATTERN_INTERVAL := 3

fire: modules p4_display.dtbo
	@echo "=== FIRE: Crash-safe driver loading ==="
	@echo "Log file: $(FIRE_LOG)"
	@echo "=== FIRE started at $$(date) ===" | sudo tee $(FIRE_LOG)
	@echo "Unloading any existing driver..."
	@-sudo rmmod drm_p4 2>/dev/null || true
	@-sudo rmmod spidev 2>/dev/null || true
	@-sudo dtoverlay -r p4_display 2>/dev/null || true
	@echo "Recording existing fb devices..."
	@ls /dev/fb* 2>/dev/null | sort > /tmp/fb_before.txt || true
	@echo "Setting up kernel debug output..."
	sudo dmesg -C
	sudo modprobe drm
	echo 0x1ff | sudo tee /sys/module/drm/parameters/debug > /dev/null
	@sync
	@echo "Loading overlay and driver..."
	sudo dtoverlay p4_display.dtbo
	sudo insmod drm_p4.ko
	@sleep 0.5
	@echo "Detecting new fb device..."
	@ls /dev/fb* 2>/dev/null | sort > /tmp/fb_after.txt || true
	@cleanup() { \
		echo ""; \
		echo "=== Stopped at $$(date) ===" | sudo tee -a $(FIRE_LOG); \
		echo "=== Cleaning up ==="; \
		kill $$PATTERN_PID 2>/dev/null || true; \
		sudo rmmod drm_p4 2>/dev/null || true; \
		sudo rmmod spidev 2>/dev/null || true; \
		sudo dtoverlay -r p4_display 2>/dev/null || true; \
		sleep 0.5; \
		if [ -n "$$FB_DEV" ] && [ -e "$$FB_DEV" ]; then \
			echo "ERROR: $$FB_DEV still exists after unload!" | tee -a $(FIRE_LOG); \
		else \
			echo "Verified fb device removed" | tee -a $(FIRE_LOG); \
		fi; \
		echo "Done. Log saved to $(FIRE_LOG)"; \
	}; \
	trap cleanup EXIT; \
	FB_DEV=$$(comm -13 /tmp/fb_before.txt /tmp/fb_after.txt | head -1); \
	if [ -z "$$FB_DEV" ]; then \
		echo "ERROR: No new framebuffer device created!" | tee -a $(FIRE_LOG); \
		echo "=== FIRE FAILED ===" | tee -a $(FIRE_LOG); \
		exit 1; \
	fi; \
	echo "Found new fb device: $$FB_DEV" | tee -a $(FIRE_LOG); \
	echo "=== Driver loaded, starting pattern loop on $$FB_DEV ===" | tee -a $(FIRE_LOG); \
	echo "=== dmesg output ===" | sudo tee -a $(FIRE_LOG); \
	( \
		patterns=$$(python3 gen_patterns.py --list); \
		while true; do \
			for p in $$patterns; do \
				if [ -e "$$FB_DEV" ]; then \
					echo "[PATTERN] Sending $$p to $$FB_DEV" | sudo tee -a $(FIRE_LOG); \
					python3 gen_patterns.py --pattern "$$p" | sudo tee "$$FB_DEV" > /dev/null 2>&1 || true; \
				fi; \
				sleep $(PATTERN_INTERVAL); \
			done; \
		done \
	) & PATTERN_PID=$$!; \
	sudo dmesg -w | while IFS= read -r line; do \
		echo "$$line"; \
		echo "$$line" | sudo tee -a $(FIRE_LOG) > /dev/null; \
		sudo sync; \
	done

# ===== Flymake support =====
# Usage:
#   make check-syntax CHK_SOURCES=file.c
#   make check-syntax CROSS_COMPILE=aarch64-linux-gnu-
#   make check-syntax KERNEL_HEADERS=/path/to/kernel/include
#   make check-syntax KERNEL_HEADERS_ONLY=1 KERNEL_HEADERS=/path/to/headers

CROSS_COMPILE ?=
KERNEL_HEADERS ?= $(KDIR)/include

FLYMAKE_CC := $(CROSS_COMPILE)gcc
FLYMAKE_CFLAGS := -fsyntax-only -std=gnu11 -Wall -Wextra \
	-Wno-unused-function -Wno-declaration-after-statement \
	-D__KERNEL__ -DMODULE -DKBUILD_MODNAME='"drm_p4"' \
	-I$(KERNEL_HEADERS) \
	-I$(KERNEL_HEADERS)/uapi \
	-I$(KDIR)/arch/arm64/include \
	-I$(KDIR)/arch/arm64/include/generated \
	-I$(KDIR)/arch/arm64/include/uapi \
	-I$(KDIR)/include/generated/uapi \
	-include $(KDIR)/include/linux/compiler-version.h \
	-include $(KDIR)/include/linux/kconfig.h

# For cross-compiling without full kernel tree, minimal mode
ifdef KERNEL_HEADERS_ONLY
FLYMAKE_CFLAGS := -fsyntax-only -std=gnu11 -Wall -Wextra \
	-Wno-unused-function -Wno-declaration-after-statement \
	-D__KERNEL__ -DMODULE -DKBUILD_MODNAME='"drm_p4"' \
	-I$(KERNEL_HEADERS)
endif

# CHK_SOURCES is set by flymake
check-syntax:
	$(FLYMAKE_CC) $(FLYMAKE_CFLAGS) $(CHK_SOURCES)

# ===== In-tree patch generation =====
# Creates a patch for drivers/gpu/drm/p4/ suitable for kernel submission

PATCH_DIR := drivers/gpu/drm/p4
# All kernel driver .c and .h files (excludes userland via USERLAND_EXCLUDE)
# Also excludes *.mod.c (kbuild generated)
PATCH_SRCS := $(filter-out %.mod.c $(USERLAND_EXCLUDE), $(wildcard *.c)) $(wildcard *.h)
PATCH_FILES := $(PATCH_SRCS) Kconfig Makefile.intree

patch: p4-display-driver.patch

p4-display-driver.patch: $(PATCH_FILES)
	@echo "Generating in-tree patch..."
	@rm -rf .patch-tmp
	@mkdir -p .patch-tmp/a .patch-tmp/b/$(PATCH_DIR)
	@for f in $(PATCH_FILES); do \
		case $$f in \
			Makefile.intree) cp $$f .patch-tmp/b/$(PATCH_DIR)/Makefile ;; \
			*) cp $$f .patch-tmp/b/$(PATCH_DIR)/$$f ;; \
		esac; \
	done
	@cd .patch-tmp && diff -ruN a b > ../p4-display-driver.patch || true
	@rm -rf .patch-tmp
	@echo "Generated p4-display-driver.patch"
	@echo ""
	@echo "To apply to kernel tree:"
	@echo "  cd /path/to/linux"
	@echo "  patch -p1 < p4-display-driver.patch"
	@echo ""
	@echo "Then add to drivers/gpu/drm/Makefile:"
	@echo '  obj-$$(CONFIG_DRM_P4) += p4/'
	@echo ""
	@echo "And to drivers/gpu/drm/Kconfig:"
	@echo '  source "drivers/gpu/drm/p4/Kconfig"'

.PHONY: modules clean install test dtsi load unload fire check-syntax patch setmode calibrate

endif
