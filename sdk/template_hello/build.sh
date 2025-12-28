#!/usr/bin/env sh
set -eu

# Third-party build script
# Requires: x86_64-elf-gcc

OUT=hello.sqrm

x86_64-elf-gcc -I .. -ffreestanding -fPIC -mno-red-zone -nostdlib \
  -Wl,-shared -Wl,-e,sqrm_module_init \
  hello_sqrm.c -o "$OUT"

echo "Built $OUT"
