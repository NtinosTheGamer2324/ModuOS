global process_return_trampoline
extern process_exit

section .text
process_return_trampoline:
    mov rdi, 0
    call process_exit
    hlt
    jmp process_return_trampoline
