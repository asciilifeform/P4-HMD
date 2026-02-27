# Interface Board and Linux Support for Reflection Technology's "Private Eye P4" electromechanical HMD.

This project allows connecting the [Private Eye P4 head-mounted display](https://www.loper-os.org/?p=752) to an RPI-compatible Linux machine via an FPGA interface board connected through SPI.

![Photo of Test Unit](/photos/rpi.jpeg)
![Midnight Commander seen through ocular](/photos/mc.jpeg)

Required:

* The [Private Eye P4](/p4docs/p4.pdf) itself. (No, I don't know where you could get one. Sadly, it is entirely conceivable that I have the very last surviving unit.)
* An RPI or any similar machine with a compatible I/O pinout.
* [The interface PCB](/pcb/p4_schem.png) (I had 5 made for ~50 USD at JLC, but it's a 2-layer, so potentially hand-etchable. You will need the level shifter, passives, connector pins, etc., and perhaps half an hour to solder.)
* [TinyFPGA BX](https://www.crowdsupply.com/tinyfpga/tinyfpga-ax-bx) (may be out of print now!)
* The code in this repo ([FPGA firmware](/fpga/) and [Linux kernel module](/kmod/).)

The display is 720x280 pixels but in fact natively rotated to 90 degrees, so it appears
as 280x720 to the hardware. The driver handles adjustable rotation, power management, brightness (1 bit!), RLE compression,
and differential updates. It may be used with xorg or in console mode. Refresh of the picture up to 30Hz is possible, though for small deltas it is in practice faster (we use run-length encoding in the FPGA and send only differences from the previous frame, when there was one.) Animations look reasonable.

# Caution

**The Linux kernel module and its documentation was generated entirely using an LLM!** It runs stably (including when unit is unplugged and replugged) but could contain bugs! Including exploitable bugs! Use at your own risk.

# Possibly Interesting Aspects

* LLM ("Claude Opus 4.5", "Max" variant) required ~2000 prompts to produce the Linux kernel module for this device.
* The kernel module makes correct use of DMA and does not substantially burden the CPU. It works with the current Linux kernel and can be used with very modest irons. Maximum SPI speed is limited to ~25MHz by the board's I/O impedance, but this is unimportant, as the maximum bit rate at which the P4 is able to receive differential picture updates is 8MHz. The FPGA performs the necessary buffering and clock domain crossing.
* There is support for powering down/waking and dimming/undimming the P4, as well as rotation.
* [Vblank](https://www.chiark.greenend.org.uk/doc/linux-doc-3.16/html/drm/drm-vertical-blank.html) is handled! 
* The FPGA code (hand-written, builds on [Yosys](https://github.com/YosysHQ/yosys)) includes a working fully-asynchronous SPI slave controller for the [iCE40](https://www.latticesemi.com/en/Products/FPGAandCPLD/iCE40) -- an item AFAIK not previously published anywhere.
* The picture quality is not adequately represented by the above photo, as the P4 is a persistence-of-vision device (it contains a vibrating mirror and a column of 280 micro-LEDs.) The contrast ratio and sharpness (though not the resolution or colour depth!) are IMHO well ahead of today's OLED glasses, even though the P4 was released in 1990!
* The P4 originally included a CGA-compatible 8-bit ISA interface card (I threw mine out, as it did not appear to work reliably even with period hardware, or rather with any such as I was able to unearth.)
* The [vendor docs](/p4docs/p4.pdf) hint that horizontal resolutions in excess of 720 columns may be available (the 32kB SRAM framebuffer installed in the P4 suggests that this may be true: the official resolution 720x280 uses only 25200 bytes), but I have not been able to discover a means for setting any useful undocumented modes. Certain values of the reserved command bits do appear to produce a corrupted image where the swing of the mirror clearly exceeds what is seen when using the standard resolution.

## License

SPDX-License-Identifier: GPL-2.0
