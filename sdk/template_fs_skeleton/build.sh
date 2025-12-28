#!/usr/bin/env sh
set -eu

OUT=fs_skel.sqrm

x86_64-elf-gcc -I .. -ffreestanding -fPIC -mno-red-zone -nostdlib \
  -Wl,-shared -Wl,-e,sqrm_module_init \
  fs_skeleton_sqrm.c -o "$OUT"

echo "Built $OUT"
