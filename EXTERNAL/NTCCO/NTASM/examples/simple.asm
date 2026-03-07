; Simple x86-64 assembly example
; mov $42, %rax
; ret

main:
    mov $42, %rax
    ret

loop_test:
    mov $0, %rcx
loop:
    add $1, %rcx
    mov $10, %rax
    mov %rcx, %rdx
    push %rcx
    pop %rcx
    ret
