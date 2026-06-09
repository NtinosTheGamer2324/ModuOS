# netctl/Makefile

CC := x86_64-elf-gcc

# Locate the cross-compiler's own freestanding headers (stdarg.h, stdint.h, …)
# These live inside the GCC installation, not the host system include path.
GCC_INCLUDES := $(shell $(CC) -print-file-name=include)

CFLAGS := -std=c11 -ffreestanding -nostdlib -nostdinc \
          -fno-stack-protector -mno-red-zone \
          -O2 -Wall -Wextra \
          -isystem $(GCC_INCLUDES) \
          -I include \
          -I include/internal \
          -I include/internal/protocol \
          -I ../../include

LD      := x86_64-elf-ld
LDFLAGS := -nostdlib -static

# ── netctl service ────────────────────────────────────────────────────
SERVICE_TARGET := netctl.elf
SERVICE_SRCS   := \
    src/main.c           \
    src/driver.c         \
    src/netctl.c         \
    src/protocol/arp.c   \
    src/protocol/ip.c    \
    src/protocol/icmp.c  \
    src/protocol/udp.c

SERVICE_OBJS := $(patsubst src/%.c, build/%.o, $(SERVICE_SRCS))

# ── example client ────────────────────────────────────────────────────
EXAMPLE_TARGET := test.elf
EXAMPLE_SRCS   := examples/test.c
EXAMPLE_OBJS   := build/examples/test.o

# ── targets ───────────────────────────────────────────────────────────
.PHONY: all clean examples

all: $(SERVICE_TARGET)

examples: $(EXAMPLE_TARGET)

# Service
$(SERVICE_TARGET): $(SERVICE_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Example client
$(EXAMPLE_TARGET): $(EXAMPLE_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Generic object rules
build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/examples/%.o: examples/%.c
	@mkdir -p build/examples
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(SERVICE_TARGET) $(EXAMPLE_TARGET)