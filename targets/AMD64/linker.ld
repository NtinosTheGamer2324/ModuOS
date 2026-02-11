ENTRY(start)

/* Higher-half kernel mapping.
 *
 * - Bootstrap runs identity-mapped at 1MiB in low memory.
 * - The main kernel is linked to run at a higher-half virtual address, but is
 *   still loaded physically at 1MiB (via AT()).
 *
 * This enables true per-process address spaces: all processes share only the
 * higher-half kernel mappings, while the lower half is free for userland.
 */

BOOT_PHYS_BASE   = 0x00100000;
KERNEL_PHYS_BASE = 0x00200000; /* must be 2MiB-aligned for early huge-page mapping */
KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;
KERNEL_VIRT_OFFSET = KERNEL_VIRT_BASE - KERNEL_PHYS_BASE;

SECTIONS
{
    /* --------------------------------------------------------------------- */
    /* Boot/entry: low identity-mapped region                                */
    /* --------------------------------------------------------------------- */
    . = BOOT_PHYS_BASE;

    _boot_start = .;

    .multiboot_header : {
        KEEP(*(.multiboot_header))
    }

    /* Keep boot code/data in low memory so we can enable paging and then
     * jump to the higher-half kernel.
     */
    .boot.text : {
        KEEP(*(.boot.text*))
    }
    .boot.rodata : {
        KEEP(*(.boot.rodata*))
    }
    .boot.data : {
        KEEP(*(.boot.data*))
    }
    .boot.bss (NOLOAD) : {
        KEEP(*(.boot.bss*))
    }

    _boot_end = .;

    /* Physical load base for the higher-half kernel (must not overlap boot sections). */
    KERNEL_LMA_BASE = ALIGN(_boot_end, 0x200000);
    /* Keep the linker math consistent with the early paging mapping. */
    KERNEL_LMA_BASE = KERNEL_PHYS_BASE;

    /* --------------------------------------------------------------------- */
    /* Main kernel: higher-half virtual addresses                             */
    /* --------------------------------------------------------------------- */
    . = KERNEL_VIRT_BASE;

    _kernel_start = .;

    /* Place higher-half sections physically starting at KERNEL_LMA_BASE. */
    .text : AT(KERNEL_LMA_BASE + (ADDR(.text) - KERNEL_VIRT_BASE))
    {
        *(.text*)
        _text_end = .;
    }

    .rodata : AT(KERNEL_LMA_BASE + (ADDR(.rodata) - KERNEL_VIRT_BASE))
    {
        *(.rodata*)

        /* Export layout constants as real 64-bit data (avoid ABS relocations).
         * We define each symbol at the current location, then emit the value.
         */
        . = ALIGN(8);
        __kernel_phys_base = .;   QUAD(KERNEL_PHYS_BASE);
        __kernel_virt_base = .;   QUAD(KERNEL_VIRT_BASE);
        __kernel_virt_offset = .; QUAD(KERNEL_VIRT_OFFSET);
        __boot_phys_base = .;     QUAD(BOOT_PHYS_BASE);
    }

    .data : AT(KERNEL_LMA_BASE + (ADDR(.data) - KERNEL_VIRT_BASE))
    {
        *(.data*)
    }

    .bss (NOLOAD) : AT(KERNEL_LMA_BASE + (ADDR(.bss) - KERNEL_VIRT_BASE))
    {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    }

    _kernel_end = .;

    /* Export constants for C code (phys allocator, etc.) */
    PROVIDE(__boot_end = _boot_end);
}
