#!/bin/bash
# Generate p4_display.dtsi from p4_pins.conf
# Usage: ./gen_dtsi.sh [config_file] [output_file]

set -e

CONFIG="${1:-p4_pins.conf}"
OUTPUT="${2:-p4_display.dtsi}"

if [ ! -f "$CONFIG" ]; then
    echo "Error: Config file '$CONFIG' not found" >&2
    exit 1
fi

# Parse config file (ignore comments and blank lines)
eval "$(grep -v '^\s*#' "$CONFIG" | grep -v '^\s*$' | sed 's/\s*=\s*/=/')"

# Validate required pins
for var in SPI_BUS SPI_CS SPI_SPEED GPIO_ENABLE GPIO_READY GPIO_NRESET GPIO_COLD GPIO_NSLEEP GPIO_VSYNC; do
    if [ -z "${!var}" ]; then
        echo "Error: $var not set in $CONFIG" >&2
        exit 1
    fi
done

# Optional: UPSIDE_DOWN (default 0)
UPSIDE_DOWN="${UPSIDE_DOWN:-0}"

# Build pin list and function list for GPIO overlay
# Enable, NRESET, and NSLEEP are outputs (1), Ready, Cold, and VSYNC are inputs (0)
PINS="${GPIO_ENABLE} ${GPIO_READY} ${GPIO_COLD} ${GPIO_NRESET} ${GPIO_NSLEEP} ${GPIO_VSYNC}"
FUNCS="1 0 0 1 1 0"
PULLS="0 0 0 0 0 0"

# Generate upside-down line (commented out if 0)
if [ "$UPSIDE_DOWN" = "1" ]; then
    UPSIDE_DOWN_LINE="				upside-down;"
else
    UPSIDE_DOWN_LINE="				/* upside-down; */  /* Uncomment for 180° flip at boot */"
fi

# Generate DTSI
cat > "$OUTPUT" << EOF
// SPDX-License-Identifier: GPL-2.0
/*
 * Device Tree Overlay for P4 display on RPi SPI${SPI_BUS}
 * Generated from $CONFIG - do not edit directly
 * Regenerate with: make dtsi
 *
 * Compile:
 *   dtc -@ -I dts -O dtb -o p4_display.dtbo p4_display.dtsi
 *
 * Install:
 *   sudo cp p4_display.dtbo /boot/overlays/
 *
 * Enable in /boot/config.txt:
 *   dtoverlay=p4_display
 *
 * Pin Configuration:
 *   ENABLE  = GPIO${GPIO_ENABLE}
 *   READY   = GPIO${GPIO_READY}
 *   COLD    = GPIO${GPIO_COLD}
 *   NRESET  = GPIO${GPIO_NRESET} (active low)
 *   NSLEEP  = GPIO${GPIO_NSLEEP} (active low)
 *   VSYNC   = GPIO${GPIO_VSYNC}
 */

/dts-v1/;
/plugin/;

/ {
	compatible = "brcm,bcm2711";

	/* Enable SPI${SPI_BUS} */
	fragment@0 {
		target = <&spi${SPI_BUS}>;
		__overlay__ {
			status = "okay";
			#address-cells = <1>;
			#size-cells = <0>;

			p4_display: p4-display@${SPI_CS} {
				compatible = "example,p4-display";
				reg = <${SPI_CS}>;  /* CE${SPI_CS} */
				spi-max-frequency = <${SPI_SPEED}>;
				/* SPI mode 0 (CPOL=0, CPHA=0) - no spi-cpol/spi-cpha */

				enable-gpios = <&gpio ${GPIO_ENABLE} 0>;
				ready-gpios = <&gpio ${GPIO_READY} 0>;
				cold-gpios = <&gpio ${GPIO_COLD} 0>;
				nreset-gpios = <&gpio ${GPIO_NRESET} 0>;  /* active-low HW, flag=0: driver uses physical values */
				nsleep-gpios = <&gpio ${GPIO_NSLEEP} 0>;  /* active-low HW, flag=0: driver uses physical values */
				vsync-gpios = <&gpio ${GPIO_VSYNC} 0>;

${UPSIDE_DOWN_LINE}
			};
		};
	};

	/* Configure GPIO pin modes */
	fragment@1 {
		target = <&gpio>;
		__overlay__ {
			p4_pins: p4_pins {
				brcm,pins = <${PINS}>;
				brcm,function = <${FUNCS}>;  /* out, in, in, out, out, in */
				brcm,pull = <${PULLS}>;      /* none */
			};
		};
	};

	/* Parameters for runtime customization */
	__overrides__ {
		speed = <&p4_display>, "spi-max-frequency:0";
		enable_pin = <&p4_display>, "enable-gpios:4";
		ready_pin = <&p4_display>, "ready-gpios:4";
		cold_pin = <&p4_display>, "cold-gpios:4";
		nreset_pin = <&p4_display>, "nreset-gpios:4";
		nsleep_pin = <&p4_display>, "nsleep-gpios:4";
		vsync_pin = <&p4_display>, "vsync-gpios:4";
	};
};
EOF

echo "Generated $OUTPUT from $CONFIG"
echo "  SPI${SPI_BUS}.${SPI_CS} @ ${SPI_SPEED} Hz"
echo "  enable=GPIO${GPIO_ENABLE} ready=GPIO${GPIO_READY} cold=GPIO${GPIO_COLD} nreset=GPIO${GPIO_NRESET} nsleep=GPIO${GPIO_NSLEEP} vsync=GPIO${GPIO_VSYNC}"
if [ "$UPSIDE_DOWN" = "1" ]; then
    echo "  upside-down=yes (180° flip at boot)"
fi
