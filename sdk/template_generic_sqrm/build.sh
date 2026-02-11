#!/usr/bin/env sh
set -eu

# Generic SQRM module template
# Requires: x86_64-elf-gcc
# Produces: generic.sqrm

OUT=generic.sqrm

x86_64-elf-gcc -I .. -ffreestanding -fPIC -mno-red-zone -nostdlib \
  -Wl,-shared -Wl,-e,sqrm_module_init \
  generic_sqrm.c -o "$OUT"

echo "Built $OUT"
