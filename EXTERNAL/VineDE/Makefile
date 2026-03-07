# VineDE Makefile (separate from root Makefile)
# Usage: make -C VineDE
# Note: You asked not to run make automatically; this file only provides commands.

CC ?= clang
CFLAGS ?= -ffreestanding -nostdlib -nostartfiles -nodefaultlibs -DLIBC_NO_START -I../userland -I../include -I../userland/thirdparty
LDFLAGS ?= -T ../userland/ld_user.ld -Wl,-T,../userland/user.ld -static -Wl,--no-dynamic-linker -Wl,-e,_start

VINEFM_SRC = VineFM/vinefm.c ../userland/lib_gfx2d.c
VINEFM_OUT = vinefm.sqr

all: $(VINEFM_OUT)

$(VINEFM_OUT): $(VINEFM_SRC)
	$(CC) $(CFLAGS) $(VINEFM_SRC) $(LDFLAGS) -o $@

clean:
	rm -f $(VINEFM_OUT)
