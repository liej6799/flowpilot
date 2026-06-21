CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Isrc
BUILD   := build

.PHONY: all probe capture clean

all: $(BUILD)/cam_probe $(BUILD)/spectra_capture

probe: $(BUILD)/cam_probe
	@echo "built $(BUILD)/cam_probe -- run as root after scripts/hal.sh stop"

capture: $(BUILD)/spectra_capture

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cam_probe: src/cam_probe.c src/cam_uapi.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/cam_probe.c

$(BUILD)/spectra_capture: src/spectra_capture.c src/cam_uapi.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/spectra_capture.c

clean:
	rm -rf $(BUILD)
