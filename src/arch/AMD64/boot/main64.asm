global long_mode_start
extern kernel_main
extern multiboot_info_ptr
global pm_start32 

section .text
bits 64

long_mode_start:
    ; null all data segment registers
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Load the saved Multiboot2 pointer (written by 32-bit bootstrap)
    ; and pass it as first argument in RDI (SysV ABI).
    mov rdi, [rel multiboot_info_ptr]

    call kernel_main

    ; kernel_main should never return, but if it does, don't run into garbage.
.hang:
    cli
    hlt
    jmp .hang
