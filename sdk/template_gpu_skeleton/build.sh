#!/bin/sh
set -e

# Generic SQRM GPU module template build script
#
# This produces: template_gpu.sqrm
# Copy it to your ISO module folder (see your build system) and load it via SQRM.

CC=${CC:-x86_64-elf-gcc}
LD=${LD:-x86_64-elf-ld}

CFLAGS="-ffreestanding -fPIC -O2 -Wall -Wextra -I../../include -I../../include/moduos"
LDFLAGS="-T ../../sdk/template_fs_skeleton/ld_moduos_sqrm.ld"

$CC $CFLAGS -c template_gpu_sqrm.c -o template_gpu_sqrm.o
$LD $LDFLAGS template_gpu_sqrm.o -o template_gpu.sqrm

echo "Built template_gpu.sqrm"
