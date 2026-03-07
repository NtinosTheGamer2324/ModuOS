VineFM (VineDE) - Graphical file manager for ModuOS

Build: use the same flags/toolchain as other userland apps. Example:
  clang -ffreestanding -nostdlib -Iuserland -Iinclude -Iuserland/thirdparty \
        -T userland/ld_user.ld -Wl,-T,userland/user.ld \
        VineDE/VineFM/vinefm.c userland/lib_gfx2d.c -o vinefm.sqr

Notes:
  - Freestanding/static build: uses -nostartfiles -nodefaultlibs -static and --no-dynamic-linker.
  - Provides its own _start and links with -Wl,-e,_start.
  - Builds with -DLIBC_NO_START to avoid multiple _start when linking helper libs.
  - Uses a simple built-in sort to avoid qsort dependency.

Run from shell:
  /Apps/vinefm.sqr

Controls:
  Mouse click - select/open
  Enter - open
  Backspace - go up
  Delete - delete
  F2 - rename
  F5 - copy
  F6 - move
  F7 - new folder
  Esc - exit

Notes:
  - Uses /dev/input/event0 and /dev/graphics/video0 (graphics mode required).
  - For RGB565 modes, falls back to gfx_blit for present.
