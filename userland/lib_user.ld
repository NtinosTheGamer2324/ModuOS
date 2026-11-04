OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

/* .sqrl shared libraries are ET_DYN with VAs relative to 0; ld-moduos picks
 * the base. Headers stay inside the first PT_LOAD. */
SECTIONS
{
    . = SIZEOF_HEADERS;

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
