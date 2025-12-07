#!/bin/bash
# build.sh - organized build for ModuOS userland

set -euo pipefail  # strict mode

# Colors
GREEN="\033[0;32m"
RED="\033[0;31m"
NC="\033[0m"

# Compiler/Linker flags
GCC_FLAGS="-ffreestanding -c -mno-red-zone -O2 -nostdlib"
LD_FLAGS="-T user.ld"

# Directories
BUILD_DIR="build"
DIST_DIR="dist"

echo -e "${GREEN}[BUILD] Cleaning previous builds...${NC}"
rm -rf "$BUILD_DIR" "$DIST_DIR"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

# Find all C source files
SRC_FILES=(*.c)
if [ ${#SRC_FILES[@]} -eq 0 ]; then
    echo -e "${RED}[BUILD] No C source files found!${NC}"
    exit 1
fi

# Compile each .c file into build/
for src in "${SRC_FILES[@]}"; do
    obj="$BUILD_DIR/${src%.c}.o"
    echo -e "${GREEN}[BUILD] Compiling $src -> $obj${NC}"
    gcc $GCC_FLAGS "$src" -o "$obj"
done

# Link each .o file from build/ into dist/
for obj in "$BUILD_DIR"/*.o; do
    bin="$DIST_DIR/$(basename "${obj%.o}.sqr")"
    echo -e "${GREEN}[BUILD] Linking $obj -> $bin${NC}"
    ld "$obj" $LD_FLAGS -o "$bin"
done

echo -e "${GREEN}[BUILD] Build complete! Binaries are in '$DIST_DIR'${NC}"
