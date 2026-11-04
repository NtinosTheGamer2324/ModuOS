OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Applications that link against .sqrl libraries (PT_INTERP + PT_DYNAMIC).
 * Dynamic metadata is placed explicitly instead of relying on orphan
 * placement, and the headers stay inside the first PT_LOAD so ld-moduos can
 * find them through SYS_DLINFO. */
SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;

    .interp   : { *(.interp) }
    .hash     : { *(.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .rela.plt : { *(.rela.plt) *(.rela.iplt) }
    .rela.dyn : { *(.rela.*) }

    .plt    : { *(.plt) *(.iplt) *(.plt.*) }
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }

    . = ALIGN(0x1000);
    .init_array : { *(.init_array*) }
    .dynamic    : { *(.dynamic) }
    .got        : { *(.got) *(.igot) }
    .got.plt    : { *(.got.plt) *(.igot.plt) }
    .data       : { *(.data*) }
    .bss        : { *(.bss*) *(COMMON) }
}
