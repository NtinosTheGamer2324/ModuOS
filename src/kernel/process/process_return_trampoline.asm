global process_return_trampoline
; Call do_exit(0) when a process returns from its entry point.
; do_exit uses the new-system 'current' global, matching both
; the new process_new.h API and the legacy process.c path.
extern do_exit

section .text
process_return_trampoline:
    xor rdi, rdi        ; status = 0
    call do_exit
    hlt                 ; do_exit never returns; halt if it somehow does
    jmp process_return_trampoline
