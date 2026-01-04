ENTRY(start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    .boot :
    {
        KEEP(*(.multiboot_header))
    }

    .text :
    {
        *(.text*)
        _text_end = .;
    }

    .rodata :
    {
        *(.rodata*)
    }

    .data :
    {
        *(.data*)
    }

    .bss :
    {
        *(COMMON)
        *(.bss*)
    }

    _kernel_end = .;
}

