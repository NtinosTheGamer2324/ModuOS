bits 64
.text
.global main
main:
    push rbp
    mov rbp, rsp
    sub rsp, 16
    lea rax, [rbp-8]
    push rax
    lea rax, [rbp-16]
    pop rdi
    mov qword ptr [rdi], rax
    lea rax, [rbp-8]
    mov rax, qword ptr [rax]
    add rax, 0
    push rax
    mov rax, 5
    pop rdi
    mov dword ptr [rdi], eax
    lea rax, [rbp-8]
    mov rax, qword ptr [rax]
    add rax, 4
    push rax
    mov rax, 7
    pop rdi
    mov dword ptr [rdi], eax
    lea rax, [rbp-8]
    mov rax, qword ptr [rax]
    add rax, 0
    movsxd rax, dword ptr [rax]
    push rax
    lea rax, [rbp-8]
    mov rax, qword ptr [rax]
    add rax, 4
    movsxd rax, dword ptr [rax]
    mov rdi, rax
    pop rax
    add rax, rdi
    push rax
    mov rax, 12
    mov rdi, rax
    pop rax
    cmp rax, rdi
    sete al
    movzx rax, al
    cmp rax, 0
    je .Lelse0
    mov rax, 0
    jmp .Lret_main
    jmp .Lend0
.Lelse0:
.Lend0:
    mov rax, 1
    jmp .Lret_main
.Lret_main:
    mov rsp, rbp
    pop rbp
    ret
.data
