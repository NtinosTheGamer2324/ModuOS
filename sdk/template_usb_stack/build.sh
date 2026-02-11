#!/usr/bin/env sh
set -eu

# Build all USB skeleton modules.
# Requires: x86_64-elf-gcc

CFLAGS="-I .. -ffreestanding -fPIC -mno-red-zone -nostdlib"
LDFLAGS="-Wl,-shared -Wl,-e,sqrm_module_init"

build_one() {
  src="$1"
  out="$2"
  echo "Building $out"
  x86_64-elf-gcc $CFLAGS $LDFLAGS "$src" -o "$out"
}

build_one uhci_sqrm.c uhci.sqrm
build_one usb_sqrm.c usb.sqrm
build_one hid_sqrm.c hid.sqrm

echo "NOTE: This template starts with UHCI only. Add OHCI/EHCI later if needed."

echo "Done"
