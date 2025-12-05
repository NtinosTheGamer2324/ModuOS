#!/bin/bash

echo "Building userland applications..."

# Build hello application
x86_64-elf-gcc -ffreestanding -nostdlib -static -fno-pie -c hello.c -o hello.o
x86_64-elf-ld -T user.ld -o hello.elf hello.o
x86_64-elf-objcopy -O binary hello.elf hello.bin

echo "Built hello.elf and hello.bin"