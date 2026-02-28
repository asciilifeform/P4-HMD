# Interface Board and Linux Support for Reflection Technology's "Private Eye P4" electromechanical HMD.

This project allows connecting the [Private Eye P4 head-mounted display](https://www.loper-os.org/?p=752) to an RPI-compatible Linux machine via an FPGA interface board connected through SPI.

![Photo of Test Unit](/photos/rpi.jpeg)
![Midnight Commander seen through ocular](/photos/mc.jpeg)

# You will need:

* The [Private Eye P4](/p4docs/p4.pdf) itself. (No, I don't know where you could get one. Sadly, it is entirely conceivable that I have the very last surviving unit.)
* An RPI or any similar machine with a compatible I/O pinout.
* [The interface PCB](/pcb/p4_schem.png) (I had 5 made for ~50 USD at JLC, but it's a 2-layer, so potentially hand-etchable. You will need the level shifter, passives, connectors (standard 2.54mm headers; as for the P4's data cable, the PCB footprint presumes [this](https://www.amazon.com/dp/B007PQ0ZG6) socket or its equivalent), etc., and perhaps half an hour to solder.)
* [TinyFPGA BX](https://www.crowdsupply.com/tinyfpga/tinyfpga-ax-bx) (may be out of print now!)
* The code in this repo ([FPGA firmware](/fpga/) and [Linux kernel module](/kmod/).)

The display is 720x280 pixels but in fact natively rotated to 90 degrees, so it appears
as 280x720 to the hardware. The driver handles adjustable rotation, power management, brightness (1 bit!), RLE compression,
and differential updates. It may be used with xorg or in console mode. Refresh of the picture up to 30Hz is possible, though for small deltas it is in practice faster (we use run-length encoding in the FPGA and send only differences from the previous frame, when there was one.) Animations look reasonable.

# Caution

**The Linux kernel module and its documentation was generated entirely using an LLM!** It runs stably (including when unit is unplugged and replugged) but could contain bugs! Including exploitable bugs! Use at your own risk.

# Possibly-interesting aspects

* LLM ("Claude Opus 4.5", "Max" variant) required ~2000 prompts to produce the Linux kernel module for this device.
* The [Linux kernel driver](/kmod/) makes correct use of DMA and does not substantially burden the CPU. It works with the current kernel, and can be used with very modest irons. Maximum SPI speed is limited to ~25MHz by the board's I/O impedance, but this is unimportant, as the maximum bit rate at which the P4 is able to receive differential picture updates is 8MHz. The FPGA performs the necessary buffering and clock domain crossing.
* There is support for [powering down/waking](/kmod/dpms.c) and [dimming/undimming](/kmod/backlight.c) the P4, as well as rotation.
* Display [rotation](/kmod/rotation.c) [uses NEON instructions where available](/kmod/rotate_neon.c). (Confirmed to work on RPI4.)
* [Vblank](https://www.chiark.greenend.org.uk/doc/linux-doc-3.16/html/drm/drm-vertical-blank.html) is [handled](/kmod/vblank.c)! 
* The [FPGA firmware](/fpga/) (hand-written, builds on [Yosys](https://github.com/YosysHQ/yosys)) includes a working fully-[asynchronous SPI slave controller](/fpga/spi_slave_async.v) for the [iCE40](https://www.latticesemi.com/en/Products/FPGAandCPLD/iCE40) -- an item AFAIK not previously published anywhere.
* The iCE40 is clocked at 16MHz and no PLL are used. However, the controller can receive just short of 16kB of buffer data in one shot at up to 100MHz (definitely not supported on RPI! and not particularly useful, but possible!) as the SPI slave and FIFO receiver are asynchronous! They are clocked with the host-supplied SPI clock, without any oversampling.
* The picture quality is not adequately represented by the above photo, as the P4 is a persistence-of-vision device (it contains a vibrating mirror and a column of 280 micro-LEDs.) The contrast ratio and sharpness (though not the resolution or colour depth!) are IMHO well ahead of today's OLED glasses, even though the P4 was released in 1990!
* The P4 originally included a CGA-compatible 8-bit ISA interface card (I threw mine out, as it did not appear to work reliably even with period hardware, or rather with any such as I was able to unearth.)
* The [vendor docs](/p4docs/p4.pdf) hint that horizontal resolutions in excess of 720 columns may be available (the 32kB SRAM framebuffer installed in the P4 suggests that this may be true: the official resolution 720x280 uses only 25200 bytes), but I have not been able to discover a means for setting any useful undocumented modes. Certain values of the reserved command bits do appear to produce a corrupted image where the swing of the mirror clearly exceeds what is seen when using the standard resolution. It is in fact possible to send a full 32kB to the P4 (any more and it crashes/resets) -- but the extra bits do not appear anywhere in the visible picture.

# More about the Private Eye

* [Vendor brochure.](/p4docs/Private_Eye_Brochure.pdf)
* [Collection of original vendor docs](https://www.eventhorizons.com/projects/P4/p4.html) where I found the interface data sheet.
* [Museum exhibit.](https://artsandculture.google.com/asset/reflection-technology-private-eye-display/QgFnZtDAdVz0CQ?hl=en)
* A [paper](https://sid.onlinelibrary.wiley.com/doi/full/10.1002/j.2637-496X.1990.tb05924.x) by the designers.
* [Photos of prototype.](https://www.virtual-boy.com/images/972309/)
* [Article](https://arstechnica.com/gaming/2024/05/virtual-boy-the-bizarre-rise-and-quick-fall-of-nintendos-enigmatic-red-console/) about the "Virtual Boy", Nintendo's attempt at integrating the Private Eye into a game console. Before I purchased my P4 in 2004, I had obtained and barbarically destroyed a "Virtual Boy" in a failed attempt to create a usable PC HMD (don't do this! they're valuable antiques now. And the HMD optics are not usable without the intact plastic chassis; and similarly, the driving chipset is not cleanly separable from the rest of the Virtual Boy.)
* [My ancient parallel port interface to the P4.](https://www.loper-os.org/vintage/paralleleye/eye.html)

# License

The driver: SPDX-License-Identifier: GPL-2.0

The FPGA code and PCB are public domain.
