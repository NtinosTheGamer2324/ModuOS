global long_mode_start
extern kernel_main
extern multiboot_info_ptr
global pm_start32 

section .boot.text
bits 64

long_mode_start:
    ; We are now in long mode and have paging enabled.
    ; Call into the higher-half kernel directly (kernel_main is linked at KERNEL_VIRT_BASE).

.higher_half_entry:
    ; null all data segment registers
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Load the saved Multiboot2 pointer and pass it as first argument in RDI.
    ; multiboot_info_ptr lives in the boot data section and is identity-mapped.
    mov rdi, [rel multiboot_info_ptr]

    ; Call higher-half kernel entry (kernel_main is in the higher half; avoid rel32 call)
    mov rax, kernel_main
    call rax

    ; kernel_main should never return, but if it does, don't run into garbage.
.hang:
    cli
    hlt
    jmp .hang
