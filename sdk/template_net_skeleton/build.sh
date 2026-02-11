#!/usr/bin/env sh
set -eu

# Third-party build script
# Requires: x86_64-elf-gcc
# Produces: net_skel.sqrm

OUT=net_skel.sqrm

x86_64-elf-gcc -I .. -ffreestanding -fPIC -mno-red-zone -nostdlib \
  -Wl,-shared -Wl,-e,sqrm_module_init \
  net_skeleton_sqrm.c -o "$OUT"

echo "Built $OUT"
