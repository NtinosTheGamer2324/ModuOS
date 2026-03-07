# Build sqrmlibc as a freestanding static archive for SQRM kernel modules.

ARCH ?= AMD64
CC   ?= x86_64-elf-gcc
AR   ?= x86_64-elf-ar
RANLIB ?= x86_64-elf-ranlib

OUT_DIR ?= ../../dist/$(ARCH)/md
LIB     := $(OUT_DIR)/libsqrmlibc.a

CFLAGS  := -I include -ffreestanding -fPIC -mno-red-zone -nostdlib -fno-builtin -fno-stack-protector -Wall -Wextra

SRC := mem.c string.c
OBJ := $(patsubst %.c,build/%.o,$(SRC))

.PHONY: all clean

all: $(LIB)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	@mkdir -p $(OUT_DIR)
	$(AR) rcs $@ $(OBJ)
	$(RANLIB) $@
	@echo "[SQRM] Built sqrmlibc -> $@"

clean:
	rm -rf build
	rm -f $(LIB)