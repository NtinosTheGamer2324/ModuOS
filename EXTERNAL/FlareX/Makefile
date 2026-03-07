# FlareX Makefile
# Builds the display server, client library, and example applications

# Userland toolchain (from ModuOS)
CC = x86_64-elf-gcc
AR = x86_64-elf-ar
LD = x86_64-elf-ld

# Compiler flags
CFLAGS = -Wall -Wextra -O2 -ffreestanding -nostdlib -mcmodel=large \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -I../../userland
LDFLAGS = -T ../../userland/user.ld -nostdlib

# Source directories
SERVER_DIR = server
LIB_DIR = lib
EXAMPLES_DIR = examples

# Output directory
BUILD_DIR = build
ISO_APPS = ../../targets/AMD64/iso/Apps

# Targets
SERVER = $(BUILD_DIR)/FlareXd.sqr
LIBFlareX = $(BUILD_DIR)/libFlareX.a
EXAMPLES = $(BUILD_DIR)/hello_FlareX.sqr
# Note: xclock requires math library (cosf/sinf) - disabled for now
# EXAMPLES += $(BUILD_DIR)/xclock.sqr

.PHONY: all clean install server lib examples

all: $(BUILD_DIR) $(SERVER) $(LIBFlareX) $(EXAMPLES)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ========== Display Server ==========

$(SERVER): $(SERVER_DIR)/startx.c $(SERVER_DIR)/lib_fnt.c $(SERVER_DIR)/*.h
	@echo "Building FlareX display server..."
	$(CC) $(CFLAGS) -DLIBC_NO_START -c $(SERVER_DIR)/lib_fnt.c -o $(BUILD_DIR)/srv_lib_fnt.o
	$(CC) $(CFLAGS) -DLIBC_NO_START -c ../../userland/lib_NodGL.c -o $(BUILD_DIR)/lib_NodGL.o
	$(CC) $(CFLAGS) -DLIBC_NO_START -c ../../userland/lib_gfx2d.c -o $(BUILD_DIR)/lib_gfx2d.o
	$(CC) $(CFLAGS) -o $@ $(SERVER_DIR)/startx.c $(BUILD_DIR)/srv_lib_fnt.o $(BUILD_DIR)/lib_NodGL.o $(BUILD_DIR)/lib_gfx2d.o $(LDFLAGS)
	@echo "Built: $@"

# ========== Client Library ==========

$(LIBFlareX): $(LIB_DIR)/libFlareX.c $(LIB_DIR)/libFlareX.h $(LIB_DIR)/lib_fnt.c $(LIB_DIR)/lib_fnt.h
	@echo "Building libFlareX client library..."
	$(CC) $(CFLAGS) -DLIBC_NO_START -Wno-unused-function -c $(LIB_DIR)/libFlareX.c -o $(BUILD_DIR)/libFlareX.o
	$(CC) $(CFLAGS) -DLIBC_NO_START -Wno-unused-function -c $(LIB_DIR)/lib_fnt.c -o $(BUILD_DIR)/lib_fnt.o
	$(AR) rcs $@ $(BUILD_DIR)/libFlareX.o $(BUILD_DIR)/lib_fnt.o
	@echo "Built: $@"

# ========== Examples ==========

$(BUILD_DIR)/hello_FlareX.sqr: $(EXAMPLES_DIR)/hello_FlareX.c $(LIBFlareX)
	@echo "Building hello_FlareX example..."
	$(CC) $(CFLAGS) -o $@ $(EXAMPLES_DIR)/hello_FlareX.c $(LIBFlareX) $(LDFLAGS)
	@echo "Built: $@"

$(BUILD_DIR)/xclock.sqr: $(EXAMPLES_DIR)/xclock.c $(LIBFlareX)
	@echo "Building xclock example..."
	$(CC) $(CFLAGS) -o $@ $(EXAMPLES_DIR)/xclock.c $(LIBFlareX) $(LDFLAGS)
	@echo "Built: $@"

# ========== Installation ==========

install: all
	@echo "Installing FlareX to ISO..."
	@mkdir -p $(ISO_APPS)
	cp $(SERVER) $(ISO_APPS)/
	cp $(EXAMPLES) $(ISO_APPS)/
	@echo "Installed to $(ISO_APPS)"

# ========== Cleanup ==========

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory"

# ========== Help ==========

help:
	@echo "FlareX Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build everything (default)"
	@echo "  server   - Build only the display server"
	@echo "  lib      - Build only the client library"
	@echo "  examples - Build only example applications"
	@echo "  install  - Install to ISO directory"
	@echo "  clean    - Remove build artifacts"
	@echo "  help     - Show this help message"

