# Blit Engine - Properly Organized Makefile
# Copyright © 2026 ModuOS Project Contributors

# Compiler and linker
CC = gcc
LD = ld
AR = ar

# Directories
ROOT_DIR := ../..
SRC_DIR := src
ENGINE_SRC := $(SRC_DIR)/engine
STUDIO_SRC := $(SRC_DIR)/studio
EXAMPLES_DIR := examples
INCLUDE_DIR := include
BUILD_DIR := build
LIB_DIR := lib

# Output paths
LIB_OUT := $(LIB_DIR)/libBlit.a
ENGINE_OBJS := $(BUILD_DIR)/engine/Blit.o
STUDIO_OBJS := $(BUILD_DIR)/studio/BlitStudio.o $(BUILD_DIR)/studio/main.o
GFX2D_OBJ := $(BUILD_DIR)/lib_gfx2d.o

# Include paths
INC_FLAGS := -I$(INCLUDE_DIR) -I$(ROOT_DIR)/include -I$(ROOT_DIR)/userland

# Compiler flags
CFLAGS := -m64 -O2 -ffreestanding -nostdlib -Wall -Wextra $(INC_FLAGS)

# Linker flags
LDFLAGS := -T $(ROOT_DIR)/userland/user.ld --hash-style=sysv

# Libraries - NodGL only (libc is header-only)
NODGL_OBJ := $(ROOT_DIR)/userland/build/lib_NodGL.o

# Example executables
EXAMPLES := shooter pong platformer breakout

.PHONY: all clean engine studio examples install help

# Default target
all: engine studio examples

# Create directories
$(BUILD_DIR)/engine $(BUILD_DIR)/studio $(LIB_DIR):
	@mkdir -p $@

# Build lib_gfx2d.o from userland
$(GFX2D_OBJ): $(ROOT_DIR)/userland/lib_gfx2d.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -DLIBC_NO_START -c $< -o $@

# Build engine library
engine: $(LIB_DIR) $(BUILD_DIR)/engine $(LIB_OUT)

$(LIB_OUT): $(ENGINE_OBJS)
	$(AR) rcs $@ $^
	@echo "✓ Built Blit Engine library: $(LIB_OUT)"

$(BUILD_DIR)/engine/Blit.o: $(ENGINE_SRC)/Blit.c $(INCLUDE_DIR)/Blit/Blit.h
	$(CC) $(CFLAGS) -c $< -o $@

# Build BlitStudio editor
studio: engine $(BUILD_DIR)/studio $(GFX2D_OBJ) $(BUILD_DIR)/blitstudio.sqr

$(BUILD_DIR)/blitstudio.sqr: $(STUDIO_OBJS) $(LIB_OUT) $(GFX2D_OBJ)
	$(LD) $(LDFLAGS) $(STUDIO_OBJS) $(LIB_OUT) $(GFX2D_OBJ) $(NODGL_OBJ) -o $@
	@echo "✓ Built BlitStudio: $(BUILD_DIR)/blitstudio.sqr"

$(BUILD_DIR)/studio/BlitStudio.o: $(STUDIO_SRC)/BlitStudio.c $(INCLUDE_DIR)/Blit/BlitStudio.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/studio/main.o: $(STUDIO_SRC)/main.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build example games
examples: engine $(GFX2D_OBJ) $(EXAMPLES)

shooter: $(EXAMPLES_DIR)/shooter.c $(GFX2D_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/shooter.o
	$(LD) $(LDFLAGS) $(BUILD_DIR)/shooter.o $(LIB_OUT) $(GFX2D_OBJ) $(NODGL_OBJ) -o $(BUILD_DIR)/$@.sqr
	@echo "✓ Built example: shooter.sqr"

pong: $(EXAMPLES_DIR)/pong.c $(GFX2D_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/pong.o
	$(LD) $(LDFLAGS) $(BUILD_DIR)/pong.o $(LIB_OUT) $(GFX2D_OBJ) $(NODGL_OBJ) -o $(BUILD_DIR)/$@.sqr
	@echo "✓ Built example: pong.sqr"

platformer: $(EXAMPLES_DIR)/platformer.c $(GFX2D_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/platformer.o
	$(LD) $(LDFLAGS) $(BUILD_DIR)/platformer.o $(LIB_OUT) $(GFX2D_OBJ) $(NODGL_OBJ) -o $(BUILD_DIR)/$@.sqr
	@echo "✓ Built example: platformer.sqr"

breakout: $(EXAMPLES_DIR)/breakout.c $(GFX2D_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/breakout.o
	$(LD) $(LDFLAGS) $(BUILD_DIR)/breakout.o $(LIB_OUT) $(GFX2D_OBJ) $(NODGL_OBJ) -o $(BUILD_DIR)/$@.sqr
	@echo "✓ Built example: breakout.sqr"

# Install to system
install: all
	@echo "Installing Blit Engine to ModuOS..."
	@mkdir -p $(ROOT_DIR)/userland $(ROOT_DIR)/include/Blit
	cp $(LIB_OUT) $(ROOT_DIR)/userland/libBlit.a
	cp $(INCLUDE_DIR)/Blit/*.h $(ROOT_DIR)/include/Blit/
	cp $(BUILD_DIR)/*.sqr $(ROOT_DIR)/userland/ 2>/dev/null || true
	@echo "✓ Installed Blit Engine"

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR) $(LIB_DIR)
	@echo "✓ Cleaned build artifacts"

# Help target
help:
	@echo "Blit Engine Build System"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all          Build everything (default)"
	@echo "  engine       Build Blit Engine library"
	@echo "  studio       Build BlitStudio editor"
	@echo "  examples     Build example games"
	@echo "  install      Install to ModuOS system"
	@echo "  clean        Remove build artifacts"
	@echo "  help         Show this help"
	@echo ""
	@echo "Example games:"
	@echo "  shooter      Space shooter game"
	@echo "  pong         Classic pong"
	@echo "  platformer   Jump and run"
	@echo "  breakout     Brick breaker"
	@echo ""
	@echo "Output:"
	@echo "  lib/libBlit.a              - Engine library"
	@echo "  build/blitstudio.sqr       - Visual editor"
	@echo "  build/shooter.sqr          - Example game"
	@echo "  build/pong.sqr             - Example game"
	@echo "  build/platformer.sqr       - Example game"
	@echo "  build/breakout.sqr         - Example game"
