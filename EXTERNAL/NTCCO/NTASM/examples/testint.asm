; Test int instruction in all modes
bits 16
    int $0x10
    int $0x13
    ret

bits 32
    int $0x80
    ret

bits 64
    int $0x03
    ret
