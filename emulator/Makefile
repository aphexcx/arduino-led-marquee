# Sign emulator. Run everything from this directory (or with make -C emulator).
#
#   make            build the firmware image and the emulator
#   make firmware   build only the firmware (build/firmware/LED_Marquee.elf and .hex)
#   make run        live terminal view (start ./fake_app.py in another terminal)
#   make test       the smoke test (no human needed)
#   make test-ads   only the ads' test: the O and C styles on the real firmware (half a minute);
#                   ADS_PORT=n for other TCP ports than 14440 and up (also for test-ads-budget)
#   make test-ads-budget   the same test on images built with a current limit and a ramp (the
#                   sketch has neither): FW_BUDGET_LIMIT_MA, FW_BUDGET_RAMP_MS, FW_BUDGET_SIDE_MA,
#                   and on one whose limit is never reached, against the image without limits
#                   (three and a half minutes for the three)
#   make sign-images   the images for trying the supply's settings on the sign: ramp only, a limit
#                   only, both, and a limit per side (SIGN_RAMP_MS, SIGN_CAP_MA, SIGN_SIDE_MA);
#                   each in build/firmware-<name>/, the name says the settings
#   make clean      delete build/ (third_party/ is kept)
#
# Everything is written under build/ and third_party/, both gitignored.
# Nothing here uploads to a board: the firmware build tree has no upload target.

ARDUINO_SDK ?= /Applications/Arduino.app/Contents/Java
AVR_INC     := $(ARDUINO_SDK)/hardware/tools/avr/avr/include
AVR_SIZE    := $(ARDUINO_SDK)/hardware/tools/avr/bin/avr-size

BUILD   := build
FW_DIR  := $(BUILD)/firmware
FW_ELF  := $(FW_DIR)/LED_Marquee.elf
FW_HEX  := $(FW_DIR)/LED_Marquee.hex
EMU     := $(BUILD)/sign-emu

SIMAVR  := third_party/simavr/simavr
OBJ     := $(BUILD)/obj

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations

# simavr: every sim/ source except its own command-line program, and the one
# core the sign uses. The lists are fixed so a build never depends on what
# happens to be in third_party/ when make starts.
SIM_NAMES := avr_acomp avr_adc avr_bitbang avr_eeprom avr_extint avr_flash \
             avr_ioport avr_lin avr_spi avr_timer avr_twi avr_uart avr_usb avr_usi \
             avr_watchdog sim_avr sim_cmds sim_core sim_cycle_timers sim_dwarf sim_elf \
             sim_gdb sim_hex sim_interrupts sim_io sim_irq sim_utils sim_vcd_file
CORE_NAMES := sim_mega328 sim_megax8
SIM_OBJS  := $(SIM_NAMES:%=$(OBJ)/simavr/%.o) $(CORE_NAMES:%=$(OBJ)/simavr/core/%.o)
SIM_INC   := -Isrc/simavr_config -I$(SIMAVR)/sim -I$(SIMAVR)
SIM_WARN  := -w

EMU_NAMES := main ihex reply ws2812 softuart link view record
EMU_OBJS  := $(EMU_NAMES:%=$(OBJ)/%.o)

.PHONY: all firmware firmware-budget sign-images emu run test test-ads test-ads-budget clean
all: firmware emu

# ---- firmware ---------------------------------------------------------------
# The unmodified sketch, through the repo's own arduino-cmake toolchain file.
$(FW_DIR)/Makefile: firmware/CMakeLists.txt firmware/toolchain.cmake
	cmake -S firmware -B $(FW_DIR) -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug \
	      -DARDUINO_SDK_PATH=$(ARDUINO_SDK)

firmware: $(FW_DIR)/Makefile
	$(MAKE) --no-print-directory -C $(FW_DIR) LED_Marquee
	@$(AVR_SIZE) $(FW_ELF)
	@shasum -a 256 $(FW_HEX)

# ---- emulator ---------------------------------------------------------------
emu: $(EMU)

$(SIMAVR)/sim/sim_avr.h:
	./fetch_simavr.sh

$(OBJ)/simavr/core/%.o: $(SIMAVR)/sim/sim_avr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SIM_WARN) -std=gnu99 $(SIM_INC) -I$(SIMAVR)/cores -I$(AVR_INC) \
	      -c $(SIMAVR)/cores/$*.c -o $@

$(OBJ)/simavr/%.o: $(SIMAVR)/sim/sim_avr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SIM_WARN) -std=gnu99 $(SIM_INC) -c $(SIMAVR)/sim/$*.c -o $@

$(OBJ)/%.o: src/%.c src/emu.h $(SIMAVR)/sim/sim_avr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -std=gnu11 $(SIM_INC) -c $< -o $@

$(EMU): $(EMU_OBJS) $(SIM_OBJS)
	$(CC) $(CFLAGS) $^ -lz -lm -lpthread -o $@

# ---- test programs ----------------------------------------------------------
# Small programs that drive one source file directly. They are built with the
# address and undefined-behaviour sanitizers, so a stray read or write stops
# the test instead of passing by luck.
TESTS    := $(BUILD)/tests
SANITIZE := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined

.PHONY: test-programs
test-programs: $(TESTS)/test_ihex $(TESTS)/gif_case

$(TESTS)/test_ihex: tests/test_ihex.c src/ihex.c src/emu.h $(SIMAVR)/sim/sim_avr.h
	@mkdir -p $(TESTS)
	$(CC) $(SANITIZE) $(WARN) -std=gnu11 -Isrc $(SIM_INC) tests/test_ihex.c src/ihex.c -o $@

$(TESTS)/gif_case: tests/gif_case.c src/record.c src/view.c src/emu.h $(SIMAVR)/sim/sim_avr.h
	@mkdir -p $(TESTS)
	$(CC) $(SANITIZE) $(WARN) -std=gnu11 -Isrc $(SIM_INC) tests/gif_case.c src/record.c src/view.c \
	      -lz -lm -o $@

# ---- use --------------------------------------------------------------------
run: all
	$(EMU) $(FW_HEX)

test:
	./test.sh

# ADS_PORT: the first of the TCP ports on 127.0.0.1 the ads' tests listen on (ads_test.py says how many)
ADS_PORT ?= 14440

test-ads: all
	python3 tests/ads_test.py $(EMU) $(FW_HEX) .. $(BUILD)/tests/ads $(ADS_PORT)

# The same sketch with the supply's settings on (in the sketch they are all 0 = off), each in its
# own build tree: build/firmware-<name>/. $(1) = name, $(2) = the settings, separated by ;
#
# A tree is configured once, and its NAME carries its settings (cap6000, budget-3000-ramp100):
# another number is another tree. So a tree is never asked to change its settings, which cmake and
# make do not do reliably (cmake keeps the first value it was given unless told again, and make
# does not see a file changed within the same second). The last line of the recipe checks that the
# tree really has the settings its name says, and fails if not.
define SIGN_IMAGE
$(BUILD)/firmware-$(1)/Makefile: firmware/CMakeLists.txt firmware/toolchain.cmake
	cmake -S firmware -B $(BUILD)/firmware-$(1) -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug \
	      -DARDUINO_SDK_PATH=$(ARDUINO_SDK) "-DSIGN_FLAGS=$(2)"

.PHONY: firmware-$(1)
firmware-$(1): $(BUILD)/firmware-$(1)/Makefile
	$(MAKE) --no-print-directory -C $(BUILD)/firmware-$(1) LED_Marquee
	@$(AVR_SIZE) $(BUILD)/firmware-$(1)/LED_Marquee.elf
	@shasum -a 256 $(BUILD)/firmware-$(1)/LED_Marquee.hex
	@grep -qxF "SIGN_FLAGS:UNINITIALIZED=$(2)" $(BUILD)/firmware-$(1)/CMakeCache.txt || \
	    { echo "$(BUILD)/firmware-$(1) was configured with other settings than $(2): delete it"; exit 1; }
endef

# Three images for the tests: a limit for the two sides together with a ramp, a limit per side, and
# a limit so high that no picture reaches it (60 A: that image must draw what the image without
# limits draws). The other numbers are low so that most of the test's texts are dimmed; they are
# not a recommendation. FW_BUDGET_RANDOM: how many messages with random texts each run adds;
# FW_BUDGET_SEED: which ones (the same seed gives the same messages).
FW_BUDGET_LIMIT_MA ?= 3000
FW_BUDGET_RAMP_MS  ?= 100
FW_BUDGET_SIDE_MA  ?= 1200
FW_BUDGET_RANDOM   ?= 40
FW_BUDGET_SEED     ?= 1
BUDGET      := budget-$(FW_BUDGET_LIMIT_MA)-ramp$(FW_BUDGET_RAMP_MS)
BUDGET_SIDE := budget-side$(FW_BUDGET_SIDE_MA)
BUDGET_HIGH := budget-60000
$(eval $(call SIGN_IMAGE,$(BUDGET),-DLED_CURRENT_LIMIT_MA=$(FW_BUDGET_LIMIT_MA);-DAD_RAMP_MS=$(FW_BUDGET_RAMP_MS)))
$(eval $(call SIGN_IMAGE,$(BUDGET_SIDE),-DLED_CURRENT_LIMIT_SIDE_MA=$(FW_BUDGET_SIDE_MA)))
$(eval $(call SIGN_IMAGE,$(BUDGET_HIGH),-DLED_CURRENT_LIMIT_MA=60000))

firmware-budget: firmware-$(BUDGET) firmware-$(BUDGET_SIDE) firmware-$(BUDGET_HIGH)

# (one run after the other, so the three can use the same ports)
test-ads-budget: emu firmware firmware-budget
	python3 tests/ads_test.py $(EMU) $(BUILD)/firmware-$(BUDGET)/LED_Marquee.hex .. $(BUILD)/tests/ads-$(BUDGET) \
	    $(ADS_PORT) --limit-ma $(FW_BUDGET_LIMIT_MA) --ramp-ms $(FW_BUDGET_RAMP_MS) --random $(FW_BUDGET_RANDOM) \
	    --seed $(FW_BUDGET_SEED)
	python3 tests/ads_test.py $(EMU) $(BUILD)/firmware-$(BUDGET_SIDE)/LED_Marquee.hex .. \
	    $(BUILD)/tests/ads-$(BUDGET_SIDE) $(ADS_PORT) --limit-side-ma $(FW_BUDGET_SIDE_MA) --random $(FW_BUDGET_RANDOM) \
	    --seed $(FW_BUDGET_SEED)
	python3 tests/ads_test.py $(EMU) $(BUILD)/firmware-$(BUDGET_HIGH)/LED_Marquee.hex .. \
	    $(BUILD)/tests/ads-$(BUDGET_HIGH) $(ADS_PORT) --limit-ma 60000 --same-as $(FW_HEX)

# Images for trying the settings on the sign: build/firmware-ramp100, -cap6000, -cap6000-ramp100
# and -side5500 with these numbers. What a setting should be has to be found on the sign.
SIGN_RAMP_MS ?= 100
SIGN_CAP_MA  ?= 6000
SIGN_SIDE_MA ?= 5500
$(eval $(call SIGN_IMAGE,ramp$(SIGN_RAMP_MS),-DAD_RAMP_MS=$(SIGN_RAMP_MS)))
$(eval $(call SIGN_IMAGE,cap$(SIGN_CAP_MA),-DLED_CURRENT_LIMIT_MA=$(SIGN_CAP_MA)))
$(eval $(call SIGN_IMAGE,cap$(SIGN_CAP_MA)-ramp$(SIGN_RAMP_MS),-DLED_CURRENT_LIMIT_MA=$(SIGN_CAP_MA);-DAD_RAMP_MS=$(SIGN_RAMP_MS)))
$(eval $(call SIGN_IMAGE,side$(SIGN_SIDE_MA),-DLED_CURRENT_LIMIT_SIDE_MA=$(SIGN_SIDE_MA)))

sign-images: firmware-ramp$(SIGN_RAMP_MS) firmware-cap$(SIGN_CAP_MA) firmware-cap$(SIGN_CAP_MA)-ramp$(SIGN_RAMP_MS) \
             firmware-side$(SIGN_SIDE_MA)

clean:
	cmake -E remove_directory $(BUILD)
