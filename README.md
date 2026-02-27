# "Private Eye P4" Interface Board

This project allows connecting the [Private Eye P4 head-mounted display](https://www.loper-os.org/?p=752) to an RPI-compatible Linux machine via an FPGA interface board connected through SPI.

Required:

* The interface PCB (I had 5 made for ~50 USD at JLC, but it's a 2-layer, so potentially hand-etchable. You will need the level shifter, passives, connector pins, etc., and perhaps half an hour to solder.)
* [TinyFPGA BX](https://www.crowdsupply.com/tinyfpga/tinyfpga-ax-bx) (may be out of print now!)
* The code in this repo (FPGA firmware and Linux kernel module.)

The display is 720x280 pixels but mounted rotated 90 degrees, so it appears
as 280x720 to the hardware. The driver handles rotation, RLE compression,
and differential updates. It may be used with xorg or in console mode. Refresh of the picture up to 30Hz is possible, though for small deltas it is in practice faster (we use run-length encoding in the FPGA and send only differences from the previous frame, when there was one.)

# Caution

**The Linux kernel module and its documentation was generated entirely using an LLM!** It runs stably (including when unit is unplugged and replugged) but could contain bugs! Including exploitable bugs! Use at your own risk.

## License

SPDX-License-Identifier: GPL-2.0
