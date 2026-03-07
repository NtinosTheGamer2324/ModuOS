# NTASM - x86/x86-64 Assembler

NTASM is a x86/x86-64 assembler written in C, for ModuOS

## Features

- **Full x86-64 Support**: Complete instruction set encoding
- **AT&T and Intel Syntax**: Flexible assembly syntax support
- **Symbol Management**: Label tracking and resolution
- **Two-Pass Assembly**: Proper forward reference handling
- **Binary Output**: Direct machine code generation
- **Efficient**: Fast compilation in C with minimal dependencies

## Building NTASM

```bash
make
```

This compiles all components and produces the `ntasm` executable.

## Usage

```bash
./ntasm <input.asm> [output.bin]
```

If no output file is specified, `output.bin` is used by default.

## Supported Instructions

### Data Movement
- `mov` - Move data (register-to-register, immediate-to-register)
- `movsx`, `movzx` - Sign/zero extended moves
- `movsxd`, `movabs` - Extended moves

### Arithmetic
- `add`, `sub` - Addition and subtraction
- `imul`, `mul` - Signed and unsigned multiplication
- `idiv`, `div` - Signed and unsigned division
- `inc`, `dec` - Increment and decrement
- `neg` - Negate

### Bitwise Operations
- `and`, `or`, `xor` - Bitwise AND, OR, XOR
- `not` - Bitwise NOT
- `shl`, `shr`, `sal`, `sar` - Shift left, right, arithmetic
- `rol`, `ror` - Rotate left, right

### Comparison and Branching
- `cmp` - Compare operands
- `test` - Test operands (bitwise AND without storing result)
- `jmp` - Unconditional jump
- `je`, `jne` - Conditional jumps (equal, not equal)
- `jl`, `jle`, `jg`, `jge` - Signed comparison jumps
- `ja`, `jbe` - Unsigned comparison jumps

### Stack and Control Flow
- `push`, `pop` - Stack operations
- `call` - Function call (relative)
- `ret` - Return from function
- `nop` - No operation
- `syscall` - System call
- `int` - Interrupt

### String Operations
- `movs`, `stos`, `lods`, `cmps`, `scas` - String operations

## Syntax

NTASM supports AT&T syntax with these conventions:

- **Registers**: Prefixed with `%` (e.g., `%rax`, `%r8`)
- **Immediates**: Prefixed with `$` (e.g., `$42`, `$0x1F`)
- **Labels**: Declared with `:` suffix (e.g., `main:`)
- **Comments**: Start with `;` (e.g., `; This is a comment`)

### Example

```asm
; Simple program
.global main

main:
    mov $42, %rax      ; Load 42 into rax
    add $10, %rax      ; Add 10
    ret                ; Return

loop_label:
    cmp $100, %rcx     ; Compare rcx with 100
    je exit            ; Jump if equal
    inc %rcx            ; Increment rcx
    jmp loop_label     ; Jump back

exit:
    ret
```

## Registers

All x86-64 registers are supported:

- **64-bit**: `rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `rbp`, `rsp`, `r8`-`r15`
- **32-bit**: `eax`, `ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp`, `esp`, `r8d`-`r15d`
- **16-bit**: `ax`, `bx`, `cx`, `dx`, `si`, `di`, `bp`, `sp`, `r8w`-`r15w`
- **8-bit**: `al`, `bl`, `cl`, `dl`, `sil`, `dil`, `bpl`, `spl`, `ah`, `bh`, `ch`, `dh`, `r8b`-`r15b`
- **Segment**: `cs`, `ds`, `es`, `fs`, `gs`, `ss`

## Architecture

NTASM uses a two-pass assembly model:

1. **Pass 1**: Scan source code, collect all labels and their addresses
2. **Pass 2**: Generate machine code, resolve label references

This approach ensures all forward references are handled correctly.

## Components

- `lexer.h/lexer.c` - Tokenizer for assembly syntax
- `encoder.h/encoder.c` - x86-64 instruction encoding
- `symbols.h/symbols.c` - Symbol table for label management
- `ntasm.c` - Main assembler orchestration

## Output Format

NTASM currently outputs raw binary machine code suitable for linking and execution. Future versions will support ELF object files.

## Roadmap

- [ ] Memory addressing modes (`[rax]`, `[rax + 8]`, etc.)
- [ ] More complete jump/conditional instruction set
- [ ] Section support (`.text`, `.data`, `.rodata`)
- [ ] ELF object file output
- [ ] Symbol exports and external symbol resolution
- [ ] Relaxation and optimization passes

## License

MIT License - Built with ❤️ for the assembly community

