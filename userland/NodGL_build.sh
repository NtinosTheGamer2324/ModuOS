#!/bin/bash
# Build script for NodGL (NodGL) demo

echo "Building NodGL (NodGL) Demo..."

# Compile the NodGL library
x86_64-elf-gcc -c lib_rdy.c -o lib_rdy.o -Iinclude -I. -std=c11 -O2 -Wall -Wextra -ffreestanding -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2

# Compile the demo application
x86_64-elf-gcc -c rdy_demo.c -o rdy_demo.o -Iinclude -I. -std=c11 -O2 -Wall -Wextra -ffreestanding -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2

# Link the demo
x86_64-elf-gcc -T user.ld -o rdy_demo.elf rdy_demo.o lib_rdy.o lib_gfx2d.o -nostdlib -lgcc -L. -lc

echo "Build complete! Output: rdy_demo.elf"

