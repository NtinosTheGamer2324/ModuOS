CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -g

# ModuOS cross-compiler (set CROSS_PREFIX to override, e.g. x86_64-elf-)
CROSS_PREFIX ?= x86_64-elf-
CROSS_CC     = $(CROSS_PREFIX)gcc
CROSS_LD     = $(CROSS_PREFIX)ld
CROSS_NASM   = nasm

# Paths relative to this Makefile (NTASM is inside EXTERNAL/NTCCO/NTASM/)
REPO_ROOT    = ../../..
USERLAND_INC = $(REPO_ROOT)/userland
LINKER_SCRIPT= $(REPO_ROOT)/userland/user.ld

# Host build: ntasm.exe / ntasm
SOURCES = ntasm.c lexer.c encoder.c symbols.c
HEADERS = lexer.h encoder.h symbols.h
OBJECTS = $(SOURCES:.c=.o)
TARGET  = ntasm

# ModuOS build: ntasm.sqr
MODUOS_SRCS  = ntasm.c lexer.c encoder.c symbols.c ntasm_moduos_compat.c
MODUOS_OBJS  = $(MODUOS_SRCS:.c=.moduos.o)
MODUOS_CFLAGS= -Wall -Wextra -std=c99 -O2         \
               -ffreestanding -mno-red-zone        \
               -nostdlib -nostdinc                 \
               -I$(USERLAND_INC)                   \
               -Dmain=ntasm_main                   \
               -DMODUOS_BUILD
CRT_OBJ      = moduos_crt.o

all: $(TARGET)

# ---- Host build ----
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- ModuOS build ----
moduos: ntasm.sqr

ntasm.sqr: $(CRT_OBJ) $(MODUOS_OBJS)
	$(CROSS_LD) -T $(LINKER_SCRIPT) -o $@ $^
	@echo "Built ntasm.sqr for ModuOS"

$(CRT_OBJ): moduos_crt.asm
	$(CROSS_NASM) -f elf64 -o $@ $<

%.moduos.o: %.c $(HEADERS)
	$(CROSS_CC) $(MODUOS_CFLAGS) -c $< -o $@

# ---- Utilities ----
clean:
	rm -f $(OBJECTS) $(MODUOS_OBJS) $(CRT_OBJ) $(TARGET) ntasm.sqr *.bin *.o

test: $(TARGET)
	./$(TARGET) examples/fibonacci.asm -o test_fib.bin
	./$(TARGET) examples/fibonacci.asm -o test_fib.o -f elf64
	@echo "All tests passed"
	@rm -f test_fib.bin test_fib.o

.PHONY: all clean test moduos
