OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* ld-moduos.sqrl: linked at a fixed address (DL_INTERP_BASE in
 * kernel/loader/dlinfo.h) because the kernel maps it without a load bias,
 * and it must stay clear of the executable at 0x400000. */
SECTIONS
{
    . = 0x04000000;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
