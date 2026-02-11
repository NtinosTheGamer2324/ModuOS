# NET SQRM modules Makefile (NIC drivers)
# Invoked by top-level Makefile

ARCH ?= AMD64
CC   ?= x86_64-elf-gcc

OUT_DIR := ../../dist/$(ARCH)/md

CFLAGS  := -I ../../include -I . -ffreestanding -fPIC -mno-red-zone -nostdlib -Wall -Wextra
LDFLAGS := -Wl,-shared -Wl,-e,sqrm_module_init

SRC := $(wildcard *_sqrm.c)
OUT := $(patsubst %_sqrm.c,$(OUT_DIR)/%.sqrm,$(SRC))

.PHONY: all clean

all: $(OUT)

$(OUT_DIR)/%.sqrm: %_sqrm.c
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	@echo "[SQRM] Built NET module -> $@"

clean:
	rm -f $(OUT)
