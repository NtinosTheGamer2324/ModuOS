; Test bootloader-style code with BIOS interrupts and mode switches
bits 16
    ; Set video mode using BIOS interrupt
    mov $0x13, %ah
    int $0x10
    
    ; Load kernel from disk
    mov $0x02, %ah
    int $0x13
    
    ; Far jump to protected mode setup
    ljmp $0x0800, $0x0000

bits 32
    ; Protected mode code (simplified)
    nop
    nop
    
    ; Far call to some routine
    lcall $0x1000, $0x5000
    
    ret

bits 16
    ; Back to real mode
    int $0x12  ; Memory size interrupt
    ret
