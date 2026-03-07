; Test far jump instructions
bits 16
    ljmp $0x0800, $0x0000
    
bits 32
    ljmp $0x1000, $0x8000

bits 64
    ljmp $0x2000, $0x4000
