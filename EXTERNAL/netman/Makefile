# ============================================================================
#  netman - ModuOS network manager
#  Usage:
#    make          - build netman.elf
#    make clean    - remove build artefacts
# ============================================================================

# ── Toolchain ────────────────────────────────────────────────────────────────
CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld

# ── Output ───────────────────────────────────────────────────────────────────
TARGET  := netman.sqr
BUILD   := build

# ── Flags ────────────────────────────────────────────────────────────────────
CFLAGS  := \
    -std=c11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-builtin \
    -nostdlib \
    -mno-red-zone \
    -O2 \
    -Wall \
    -Wextra \
    -Iinclude

LDFLAGS := \
    -nostdlib \
    -static \
    -T user.ld

# ── Sources ───────────────────────────────────────────────────────────────────
SRCS := \
    src/core/netman.c \
    src/core/dhcp.c \
    src/protocol/arp.c \
    src/protocol/ip/ipv4.c

# ── Objects (mirror src/ tree under build/) ──────────────────────────────────
OBJS := $(patsubst src/%.c, $(BUILD)/%.o, $(SRCS))

# ── Default target ────────────────────────────────────────────────────────────
.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# ── Compile each .c → build/.../.o (create dirs on demand) ───────────────────
$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Dependency tracking ───────────────────────────────────────────────────────
-include $(OBJS:.o=.d)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD) $(TARGET)