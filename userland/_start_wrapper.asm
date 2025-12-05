; _start_wrapper.asm - Handles argument passing
; 
; When the kernel starts a process:
; - If the process was created with args, RDI = argc, RSI = argv
; - If no args, RDI and RSI will be 0
;
; This wrapper checks and calls _start appropriately

section .text
global _start_entry
extern _start

_start_entry:
    ; Arguments are already in RDI (argc) and RSI (argv) from kernel
    ; Stack is aligned to 16 bytes by kernel
    
    ; Check if we have arguments (argc > 0)
    test rdi, rdi
    jz .no_args
    
    ; We have arguments - call _start(argc, argv)
    ; RDI = argc (already set)
    ; RSI = argv (already set)
    call _start
    jmp .done
    
.no_args:
    ; No arguments - call _start() with no params
    ; (For backwards compatibility with apps that don't use args)
    xor rdi, rdi
    xor rsi, rsi
    call _start
    
.done:
    ; _start should call exit(), but just in case...
    mov rdi, 0
    mov rax, 60  ; exit syscall number (if you have it)
    syscall
    
    ; If syscall doesn't work, infinite loop
    cli
.hang:
    hlt
    jmp .hang