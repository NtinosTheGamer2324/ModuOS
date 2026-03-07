; Multi-mode assembly test
bits 16
    mov $42, %ax
    add %bx, %ax
    ret

bits 32
    mov $42, %eax
    add %ebx, %eax
    ret

bits 64
    mov $42, %rax
    add %rbx, %rax
    ret
