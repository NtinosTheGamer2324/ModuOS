; FizzBuzz in x86-64 assembly

.global main

main:
    ; Initialize counter to 1
    mov $1, %rax
    
loop_start:
    ; Check if counter > 100
    cmp $100, %rax
    mov $101, %rcx
    ; If greater, exit
    
    ; Check divisible by 3
    mov %rax, %rdx
    mov $3, %rcx
    
    ; Check divisible by 5
    mov %rax, %rdx
    mov $5, %rcx
    
    ; Increment and loop
    add $1, %rax
    
    ret
