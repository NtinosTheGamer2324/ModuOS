; main.asm - cleaned bootstrap
global start
global multiboot_info_ptr
extern long_mode_start

section .text
bits 32
start:
    ; Immediately save the multiboot2 pointer (EBX) while still in flat mode.
    ; Do this before touching memory, stack or enabling paging.
    mov dword [multiboot_info_ptr], ebx
    mov dword [multiboot_info_ptr + 4], 0
    ; set stack (stack_top defined in .bss)
    mov esp, stack_top
    call check_multiboot
    call check_cpuid
    call check_long_mode
    call setup_page_tables
    call enable_paging
    lgdt [gdt64.pointer]
    jmp gdt64.code_segment:long_mode_start
    hlt

; ------------------------------------------------------------
; sanity checks / helpers
; ------------------------------------------------------------
check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov esi, msg_no_multiboot
    jmp error

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov esi, msg_no_cpuid
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    mov esi, msg_no_long_mode
    jmp error

; ------------------------------------------------------------
; setup identity 2MiB mappings (early)
; ------------------------------------------------------------
setup_page_tables:
    mov eax, page_table_l3
    or eax, 0b11        ; present, writable
    mov [page_table_l4], eax
    mov eax, page_table_l2
    or eax, 0b11
    mov [page_table_l3], eax
    mov ecx, 0
.loop:
    mov eax, 0x200000   ; 2 MiB increment
    mul ecx
    or eax, 0b10000011  ; present, writable, huge page flag
    mov [page_table_l2 + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

enable_paging:
    mov eax, page_table_l4
    mov cr3, eax
    ; enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    ; enable long mode (IA32_EFER)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    ; enable paging (CR0.PG)
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

error:
    ; Print error message pointed to by ESI (bios text)
    mov edi, 0xb8000
    mov ah, 0x4f  ; White on red
.errloop:
    lodsb
    test al, al
    jz .done
    stosw
    jmp .errloop
.done:
    hlt

; ------------------------------------------------------------
; .data: initialized writable data
; ------------------------------------------------------------
section .data
align 8
multiboot_info_ptr:
    dq 0    ; Initialize to 0 (will be written by bootloader)

; ------------------------------------------------------------
; BSS: only reserves / no initializers
; ------------------------------------------------------------
section .bss
; align the page tables to page boundary
align 4096
page_table_l4:    resb 4096
page_table_l3:    resb 4096
page_table_l2:    resb 4096

; stack region
align 16
stack_bottom:     resb 16384
stack_top equ stack_bottom + 16384

; ------------------------------------------------------------
; read-only data
; ------------------------------------------------------------
section .rodata
gdt64:
    dq 0
.code_segment: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) ; code segment descriptor flags
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

msg_no_multiboot: db "MDBOOT: ERROR: Not loaded by Multiboot2 bootloader", 0
msg_no_cpuid:      db "MDBOOT: ERROR: CPUID instruction not supported", 0
msg_no_long_mode:  db "MDBOOT: ERROR: CPU does not support 64-bit long mode", 0