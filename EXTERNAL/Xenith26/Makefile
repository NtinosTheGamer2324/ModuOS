# Xenith26 Makefile
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
SERVER = $(BUILD_DIR)/xenith26d.sqr
LIBX26 = $(BUILD_DIR)/libX26.a
EXAMPLES = $(BUILD_DIR)/hello_x26.sqr
# Note: xclock requires math library (cosf/sinf) - disabled for now
# EXAMPLES += $(BUILD_DIR)/xclock.sqr

.PHONY: all clean install server lib examples

all: $(BUILD_DIR) $(SERVER) $(LIBX26) $(EXAMPLES)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ========== Display Server ==========

$(SERVER): $(SERVER_DIR)/startx.c $(SERVER_DIR)/*.h
	@echo "Building Xenith26 display server..."
	$(CC) $(CFLAGS) -o $@ $(SERVER_DIR)/startx.c $(LDFLAGS)
	@echo "Built: $@"

# ========== Client Library ==========

$(LIBX26): $(LIB_DIR)/libX26.c $(LIB_DIR)/libX26.h $(LIB_DIR)/lib_fnt.c $(LIB_DIR)/lib_fnt.h
	@echo "Building libX26 client library..."
	$(CC) $(CFLAGS) -DLIBC_NO_START -c $(LIB_DIR)/libX26.c -o $(BUILD_DIR)/libX26.o
	$(CC) $(CFLAGS) -DLIBC_NO_START -c $(LIB_DIR)/lib_fnt.c -o $(BUILD_DIR)/lib_fnt.o
	$(AR) rcs $@ $(BUILD_DIR)/libX26.o $(BUILD_DIR)/lib_fnt.o
	@echo "Built: $@"

# ========== Examples ==========

$(BUILD_DIR)/hello_x26.sqr: $(EXAMPLES_DIR)/hello_x26.c $(LIBX26)
	@echo "Building hello_x26 example..."
	$(CC) $(CFLAGS) -o $@ $(EXAMPLES_DIR)/hello_x26.c $(LIBX26) $(LDFLAGS)
	@echo "Built: $@"

$(BUILD_DIR)/xclock.sqr: $(EXAMPLES_DIR)/xclock.c $(LIBX26)
	@echo "Building xclock example..."
	$(CC) $(CFLAGS) -o $@ $(EXAMPLES_DIR)/xclock.c $(LIBX26) $(LDFLAGS)
	@echo "Built: $@"

# ========== Installation ==========

install: all
	@echo "Installing Xenith26 to ISO..."
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
	@echo "Xenith26 Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build everything (default)"
	@echo "  server   - Build only the display server"
	@echo "  lib      - Build only the client library"
	@echo "  examples - Build only example applications"
	@echo "  install  - Install to ISO directory"
	@echo "  clean    - Remove build artifacts"
	@echo "  help     - Show this help message"
