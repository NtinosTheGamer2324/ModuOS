; Fibonacci sequence - Calculate fibonacci(10)
; Returns value in rax

.global fibonacci

fibonacci:
    ; rdi = n (input parameter)
    ; Returns fib(n) in rax
    cmp $2, %rdi
    jle fib_base
    
    ; rdi >= 2: recursion needed
    dec %rdi
    push %rdi
    call fibonacci  ; fib(n-1)
    pop %rdi
    
    ; fib(n-1) is in rax, save it
    push %rax
    
    ; Calculate fib(n-2)
    dec %rdi
    call fibonacci
    
    ; Now we have fib(n-2) in rax
    ; Add fib(n-1) which is on stack
    pop %rcx
    add %rcx, %rax
    ret
    
fib_base:
    ; Base cases: fib(0)=0, fib(1)=1
    mov %rdi, %rax
    ret
