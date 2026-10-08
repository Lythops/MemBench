BUILD_DIR = build
export N64_INST = /pyrite64-sdk
include $(N64_INST)/include/n64.mk

N64_CFLAGS += -O2 -std=gnu11

src = src/main.c

all: membench.z64

$(BUILD_DIR)/membench.elf: $(src:%.c=$(BUILD_DIR)/%.o)

membench.z64: N64_ROM_TITLE = "MemBench"

clean:
	rm -rf $(BUILD_DIR) membench.z64

-include $(wildcard $(BUILD_DIR)/src/*.d)

.PHONY: all clean
