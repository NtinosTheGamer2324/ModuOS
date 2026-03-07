; test_full.asm - Comprehensive NTASM feature test
; Runs on Linux (syscall) or ModuOS (int 0x80)
; Build: ntasm test_full.asm -o test_full.o -f elf64
;        ld -m elf_x86_64 -e _start test_full.o -o test_full
; Run:   ./test_full  (should print "NTASM OK" and exit 0)

bits 64
.text
.global _start

_start:
    ; ---- Basic MOV ----
    mov rax, 0
    mov rbx, 1
    mov rcx, 2
    mov rdx, 3

    ; ---- ADD / SUB ----
    add rax, rbx        ; rax = 1
    add rax, rcx        ; rax = 3
    sub rax, rbx        ; rax = 2
    sub rax, 1          ; rax = 1
    cmp rax, 1
    jne fail

    ; ---- AND / OR / XOR ----
    mov rax, 0xFF
    and rax, 0x0F       ; rax = 0x0F
    or  rax, 0x10       ; rax = 0x1F
    xor rax, 0x1F       ; rax = 0
    cmp rax, 0
    jne fail

    ; ---- INC / DEC ----
    mov rax, 5
    inc rax             ; 6
    dec rax             ; 5
    inc rax             ; 6
    cmp rax, 6
    jne fail

    ; ---- NEG / NOT ----
    mov rax, 10
    neg rax             ; rax = -10
    neg rax             ; rax = 10
    cmp rax, 10
    jne fail

    ; ---- IMUL ----
    mov rax, 6
    imul rax, 7         ; rax = 42
    cmp rax, 42
    jne fail

    ; ---- SHL / SHR ----
    mov rax, 1
    shl rax, 4          ; rax = 16
    shr rax, 2          ; rax = 4
    cmp rax, 4
    jne fail

    ; ---- SAR (arithmetic shift preserves sign) ----
    mov rax, -8
    sar rax, 1          ; rax = -4
    cmp rax, -4
    jne fail

    ; ---- PUSH / POP ----
    mov rax, 0xDEAD
    push rax
    mov rax, 0
    pop rax
    cmp rax, 0xDEAD
    jne fail

    ; ---- Stack frame idiom ----
    push rbp
    mov rbp, rsp
    sub rsp, 16
    mov qword ptr [rbp-8], 0x1234
    mov rax, qword ptr [rbp-8]
    cmp rax, 0x1234
    jne .frame_fail
    mov rsp, rbp
    pop rbp
    jmp .frame_ok
.frame_fail:
    jmp fail
.frame_ok:

    ; ---- TEST instruction ----
    mov rax, 0xFF
    test rax, rax
    jz fail             ; should not be zero

    ; ---- Forward jump ----
    jmp .skip_bad
    jmp fail            ; should never execute
.skip_bad:

    ; ---- Conditional jumps ----
    mov rax, 10
    cmp rax, 10
    jne fail
    cmp rax, 9
    jle fail
    cmp rax, 11
    jge fail

    ; ---- CALL / RET ----
    call .add_one
    cmp rax, 1
    jne fail

    ; ---- LEA ----
    lea rax, [rbp]      ; just test it encodes without crash

    ; ---- 32-bit operand (zero-extends to 64-bit) ----
    mov eax, 0x12345678
    mov ecx, 0x12345678
    cmp eax, ecx
    jne fail

    ; ---- 8-bit operand ----
    mov al, 0x42
    cmp al, 0x42
    jne fail

    ; ---- MOVZX ----
    mov al, 0xFF
    movzx rax, al
    cmp rax, 0xFF
    jne fail

    ; ---- MOVSX ----
    mov al, 0xFF        ; -1 as signed byte
    movsx rax, al
    cmp rax, -1
    jne fail

    ; ---- XOR self (zeroing idiom) ----
    mov rax, 0xDEADBEEF
    xor rax, rax
    test rax, rax
    jnz fail

    ; ---- Print "NTASM OK\n" via write syscall ----
    mov rax, 1          ; sys_write
    mov rdi, 1          ; stdout
    mov rsi, msg
    mov rdx, 9          ; length
    syscall

    ; ---- Exit 0 ----
    mov rax, 60         ; sys_exit
    mov rdi, 0
    syscall

fail:
    ; Exit with code 1 to signal test failure
    mov rax, 60
    mov rdi, 1
    syscall

; --- Subroutine: set rax=0 then return 1 ---
.add_one:
    mov rax, 0
    inc rax
    ret

.data
msg:
    db "NTASM OK", 10
