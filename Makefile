# tinyble host build: unit tests, sanitizers, and the Cortex-M4 size gate.
#
# CC is honoured for the host. ARM_CC and ARM_SIZE pick the cross compiler;
# the nRF Connect SDK's arm-zephyr-eabi-gcc is what the firmware uses.

export LC_ALL := C
BUILD := build/host

CSTD := -std=c11 -O1
# tinytalk's warning set (Zephyr's clang warning_base plus -Werror), and -Wextra.
WARN := -Wall -Wextra -Wformat -Wformat-security -Wdouble-promotion -Wpointer-arith -Werror
SAN := -g -fsanitize=address,undefined -fno-sanitize-recover=all

INCS := -Iinclude
SRCS := $(wildcard src/*.c)
HDRS := $(wildcard include/tinyble/*.h)
TESTS := $(wildcard tests/host/*.c)

ARM_CC ?= arm-none-eabi-gcc
ARM_SIZE ?= $(patsubst %gcc,%size,$(ARM_CC))
ARM_FLAGS := -std=c11 -Os -mcpu=cortex-m4 -mthumb -mabi=aapcs -ffunction-sections \
	-fdata-sections -fno-common $(WARN) $(INCS)
# The host-side budget is 6,429 B flash and 2,031 B RAM (RESEARCH.md); tinyble
# gets what is left after raw HCI and net_buf (582 + 556 B, 1,271 B RAM).
SIZE_FLASH_MAX := 5291
SIZE_RAM_MAX := 760

# Every optional behaviour on (tinyble.h); the tests run off, on, and as DFU.
FULL := -DTB_FIND_BY_TYPE=1 -DTB_READ_BLOB=1 -DTB_TYPE128=1 -DTB_SIG_REJECT=1 \
	-DTB_SMP_REFUSE=1 -DTB_ATT_TIMEOUT=1 -DTB_SCAN_RSP=1
# CONFIG_TINYBLE_TINYFOTA as the door-lock builds it (zephyr/CMakeLists.txt):
# tinyfota's services in the table, Find By Type Value, a scan response.
DFU := -DTB_MATTER_DFU=1 -DTB_FIND_BY_TYPE=1 -DTB_SCAN_RSP=1

.PHONY: test asan adapter size clean

$(BUILD)/test: $(SRCS) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(INCS) $(SRCS) $(TESTS) -o $@

$(BUILD)/test_full: $(SRCS) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(FULL) $(INCS) $(SRCS) $(TESTS) -o $@

$(BUILD)/test_dfu: $(SRCS) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(DFU) $(INCS) $(SRCS) $(TESTS) -o $@

$(BUILD)/test_asan: $(SRCS) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(SAN) $(INCS) $(SRCS) $(TESTS) -o $@

$(BUILD)/test_asan_full: $(SRCS) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(SAN) $(FULL) $(INCS) $(SRCS) $(TESTS) -o $@

$(BUILD)/test_asan_dfu: $(SRCS) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(SAN) $(DFU) $(INCS) $(SRCS) $(TESTS) -o $@

test: $(BUILD)/test $(BUILD)/test_full $(BUILD)/test_dfu
	$(BUILD)/test
	$(BUILD)/test_full
	$(BUILD)/test_dfu

asan: $(BUILD)/test_asan $(BUILD)/test_asan_full $(BUILD)/test_asan_dfu
	$(BUILD)/test_asan
	$(BUILD)/test_asan_full
	$(BUILD)/test_asan_dfu

# port/tinytalk against tinytalk's BTP codec, Zephyr stubbed:
# make adapter TINYTALK=<a tinytalk tree>
ADAPTER := $(SRCS) port/tinytalk/matter_ble_tinyble.c tests/tinytalk/test_adapter.c \
	$(TINYTALK)/src/matter_btp.c
ADAPTER_DEFS := -DCONFIG_TINYTALK_VENDOR_ID=0xFFF1 -DCONFIG_TINYTALK_PRODUCT_ID=0x8001 \
	-DCONFIG_TINYTALK_DISCRIMINATOR=0xF00
# Then with CONFIG_TINYBLE_TINYFOTA, as zephyr/CMakeLists.txt builds it.
ADAPTER_DFU := -DCONFIG_TINYBLE_TINYFOTA=1 -DTB_MATTER_DFU=1 -DTB_FIND_BY_TYPE=1

adapter:
	@test -n "$(TINYTALK)" || { echo "make adapter TINYTALK=<a tinytalk tree>"; exit 1; }
	@mkdir -p $(BUILD)
	$(CC) $(CSTD) $(WARN) $(SAN) $(ADAPTER_DEFS) $(INCS) -Itests/tinytalk/stub \
		-I$(TINYTALK)/include $(ADAPTER) -o $(BUILD)/test_adapter
	$(CC) $(CSTD) $(WARN) $(SAN) $(ADAPTER_DEFS) $(ADAPTER_DFU) $(INCS) -Itests/tinytalk/stub \
		-I$(TINYTALK)/include $(ADAPTER) -o $(BUILD)/test_adapter_dfu
	$(BUILD)/test_adapter
	$(BUILD)/test_adapter_dfu

# text + data is flash, data + bss is RAM, summed over tinyble's objects.
size: $(SRCS) $(HDRS)
	@rm -rf build/arm && mkdir -p build/arm
	@for f in $(SRCS); do \
		$(ARM_CC) $(ARM_FLAGS) -c $$f -o build/arm/$$(basename $$f .c).o || exit 1; \
	done
	@$(ARM_SIZE) build/arm/*.o
	@$(ARM_SIZE) build/arm/*.o | awk 'NR > 1 { f += $$1 + $$2; r += $$2 + $$3 } \
		END { printf "tinyble: %d B flash (max $(SIZE_FLASH_MAX)), %d B RAM (max $(SIZE_RAM_MAX))\n", f, r; \
		exit (f > $(SIZE_FLASH_MAX) || r > $(SIZE_RAM_MAX)) }'

clean:
	rm -rf build/host build/arm
