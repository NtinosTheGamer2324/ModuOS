; Loop example - Count from 1 to 10
; Uses cmp, conditional jump, and loop

.global count_loop

count_loop:
    mov $1, %rcx      ; Counter = 1
    mov $0, %rax      ; Sum = 0
    
loop:
    ; Check if counter > 10
    cmp $10, %rcx
    jg done           ; Jump if greater
    
    ; Add counter to sum
    add %rcx, %rax
    
    ; Increment counter
    inc %rcx
    
    ; Jump back to loop
    jmp loop
    
done:
    ; rax now contains sum 1+2+...+10 = 55
    ret
