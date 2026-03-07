; moduos_crt.asm - ModuOS C Runtime Entry Point for NTASM
; Provides _start -> md_main() -> sys_exit (int 0x80, SYS_EXIT=0)
;
; The ModuOS kernel trampoline sets up:
;   rdi = argc  (long)
;   rsi = argv  (char **)
; which matches the SysV AMD64 calling convention — so we can call
; md_main(argc, argv) directly without any register shuffling.

bits 64
.text
.global _start
.extern md_main

_start:
    ; rdi=argc, rsi=argv already loaded by the kernel trampoline
    call md_main
    ; md_main returns exit code in rax; move to rdi for SYS_EXIT
    mov rdi, rax
    xor rax, rax        ; SYS_EXIT = 0
    int 0x80
    hlt                 ; should never reach here
