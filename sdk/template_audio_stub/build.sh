#!/usr/bin/env sh
set -eu

OUT=audio_stub.sqrm

x86_64-elf-gcc -I .. -ffreestanding -fPIC -mno-red-zone -nostdlib \
  -Wl,-shared -Wl,-e,sqrm_module_init \
  audio_stub_sqrm.c -o "$OUT"

echo "Built $OUT"
