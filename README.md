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

The display is 720x280 pixels but mounted rotated 90 degrees, so it appears
as 280x720 to the hardware. The driver handles rotation, RLE compression,
and differential updates. It may be used with xorg or in console mode. Refresh of the picture up to 30Hz is possible, though for small deltas it is in practice faster (we use run-length encoding in the FPGA and send only differences from the previous frame, when there was one.)

# Caution

**The Linux kernel module and its documentation was generated entirely using an LLM!** It runs stably (including when unit is unplugged and replugged) but could contain bugs! Including exploitable bugs! Use at your own risk.

# Possibly Interesting Aspects

* LLM ("Claude Opus 4.5", "Max" variant) required ~2000 prompts to produce the Linux kernel module for this device.
* The FPGA code (hand-written, builds on [Yosys](https://github.com/YosysHQ/yosys)) includes a working fully-asynchronous SPI slave controller for the [iCE40](https://www.latticesemi.com/en/Products/FPGAandCPLD/iCE40) -- an item AFAIK not previously published anywhere.
* The picture quality is not adequately represented by the above photo, as the P4 is a persistence-of-vision device (it contains a vibrating mirror and a column of 280 micro-LEDs.) The contrast ratio and sharpness (though not the resolution or colour depth!) are IMHO well ahead of today's OLED glasses, even though the P4 was released in 1990!

## License

SPDX-License-Identifier: GPL-2.0
