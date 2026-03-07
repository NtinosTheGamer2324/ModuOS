; Test far call instructions
bits 16
    lcall $0x0800, $0x0100
    
bits 32
    lcall $0x1000, $0x8000

bits 64
    lcall $0x2000, $0x4000
