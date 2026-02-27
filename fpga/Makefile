# Project setup
PROJ      = eye
BUILD     = ./build
DEVICE    = 8k
FOOTPRINT = cm81
PCF       = tinyfpga-bx.pcf

# Files
FILES = $(wildcard *.v)

.PHONY: all clean burn timing

all $(BUILD)/$(PROJ).asc $(BUILD)/$(PROJ).bin:
	mkdir -p $(BUILD)
	yosys -p "synth_ice40 -top $(PROJ) -blif $(BUILD)/$(PROJ).blif -json $(BUILD)/$(PROJ).json" $(FILES)
	nextpnr-ice40 --hx$(DEVICE) --package $(FOOTPRINT) --json build/$(PROJ).json --pcf $(PCF) --asc build/$(PROJ).asc
	icepack $(BUILD)/$(PROJ).asc $(BUILD)/$(PROJ).bin

prog: $(BUILD)/$(PROJ).bin
	tinyprog -p $(BUILD)/$(PROJ).bin

timing: $(BUILD)/$(PROJ).asc
	icetime -tmd hx$(DEVICE) $(BUILD)/$(PROJ).asc

clean:
	rm build/*
