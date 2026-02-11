# QXL GPU SQRM module Makefile
# Invoked by top-level Makefile

ARCH ?= AMD64
CC ?= x86_64-elf-gcc

# Output matches kernel module destination folder
OUT_DIR := ../../dist/$(ARCH)/md
OUT     := $(OUT_DIR)/qxl_gpu.sqrm

CFLAGS  := -I ../../include -I ./include -ffreestanding -fPIC -mno-red-zone -nostdlib -Wall -Wextra
LDFLAGS := -Wl,-shared -Wl,-e,sqrm_module_init

SRC := $(shell find src -name '*.c')

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC) -o $(OUT)
	@echo "[SQRM] Built QXL GPU module -> $(OUT)"

clean:
	rm -f $(OUT)
