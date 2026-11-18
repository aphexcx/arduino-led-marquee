# Sign emulator. Run everything from this directory (or with make -C emulator).
#
#   make            build the firmware image and the emulator
#   make firmware   build only the firmware (build/firmware/LED_Marquee.elf and .hex)
#   make run        live terminal view (start ./fake_app.py in another terminal)
#   make test       the smoke test (no human needed)
#   make test-ads   only the ads' test: the O and C styles on the real firmware (half a minute)
#   make test-ads-budget   the same test on images built with a current limit and a ramp (the
#                   sketch has neither): build/firmware-budget/ and build/firmware-budget-side/
#   make sign-images   the images for trying the supply's settings on the sign: ramp only, a limit
#                   only, both, and a limit per side (build/firmware-<name>/)
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

test-ads: all
	python3 tests/ads_test.py $(EMU) $(FW_HEX) .. $(BUILD)/tests/ads

# The same sketch with the supply's settings on (in the sketch they are all 0 = off), each in its
# own build tree: build/firmware-<name>/. $(1) = name, $(2) = the settings, separated by ;
define SIGN_IMAGE
$(BUILD)/firmware-$(1)/Makefile: firmware/CMakeLists.txt firmware/toolchain.cmake Makefile
	cmake -S firmware -B $(BUILD)/firmware-$(1) -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug \
	      -DARDUINO_SDK_PATH=$(ARDUINO_SDK) "-DSIGN_FLAGS=$(2)"

.PHONY: firmware-$(1)
firmware-$(1): $(BUILD)/firmware-$(1)/Makefile
	$(MAKE) --no-print-directory -C $(BUILD)/firmware-$(1) LED_Marquee
	@$(AVR_SIZE) $(BUILD)/firmware-$(1)/LED_Marquee.elf
	@shasum -a 256 $(BUILD)/firmware-$(1)/LED_Marquee.hex
endef

# Two images for the tests: a limit for the two sides together with a ramp, and a limit per side.
# The numbers are low so that most of the test's texts are dimmed; they are not a recommendation.
FW_BUDGET_LIMIT_MA ?= 3000
FW_BUDGET_RAMP_MS  ?= 100
FW_BUDGET_SIDE_MA  ?= 1200
$(eval $(call SIGN_IMAGE,budget,-DLED_CURRENT_LIMIT_MA=$(FW_BUDGET_LIMIT_MA);-DAD_RAMP_MS=$(FW_BUDGET_RAMP_MS)))
$(eval $(call SIGN_IMAGE,budget-side,-DLED_CURRENT_LIMIT_SIDE_MA=$(FW_BUDGET_SIDE_MA)))

test-ads-budget: emu firmware-budget firmware-budget-side
	python3 tests/ads_test.py $(EMU) $(BUILD)/firmware-budget/LED_Marquee.hex .. $(BUILD)/tests/ads-budget 14445 \
	    --limit-ma $(FW_BUDGET_LIMIT_MA) --ramp-ms $(FW_BUDGET_RAMP_MS)
	python3 tests/ads_test.py $(EMU) $(BUILD)/firmware-budget-side/LED_Marquee.hex .. $(BUILD)/tests/ads-budget-side \
	    14450 --limit-side-ma $(FW_BUDGET_SIDE_MA)

# Images for trying the settings on the sign. What a setting should be has to be found there.
$(eval $(call SIGN_IMAGE,ramp100,-DAD_RAMP_MS=100))
$(eval $(call SIGN_IMAGE,cap6000,-DLED_CURRENT_LIMIT_MA=6000))
$(eval $(call SIGN_IMAGE,cap6000-ramp100,-DLED_CURRENT_LIMIT_MA=6000;-DAD_RAMP_MS=100))
$(eval $(call SIGN_IMAGE,side5500,-DLED_CURRENT_LIMIT_SIDE_MA=5500))

sign-images: firmware-ramp100 firmware-cap6000 firmware-cap6000-ramp100 firmware-side5500

clean:
	cmake -E remove_directory $(BUILD)
