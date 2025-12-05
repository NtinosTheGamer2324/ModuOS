# Makefile for ModuOS x86_64

# Target architecture
ARCH := AMD64

# ========================
# Source file definitions
# ========================

# Kernel C files
kernel_source_files := $(shell find src/kernel -name '*.c')
kernel_object_files := $(patsubst src/kernel/%.c, build/kernel/%.o, $(kernel_source_files))

# Driver C files
drivers_c_source_files := $(shell find src/drivers -name '*.c')
drivers_object_files := $(patsubst src/drivers/%.c, build/drivers/%.o, $(drivers_c_source_files))

# Filesystem C files
fs_c_source_files := $(shell find src/fs -name '*.c')
fs_object_files := $(patsubst src/fs/%.c, build/fs/%.o, $(fs_c_source_files))

# Architecture-specific C files
arch_c_source_files := $(shell find src/arch/$(ARCH) -name '*.c')
arch_c_object_files := $(patsubst src/arch/$(ARCH)/%.c, build/arch/$(ARCH)/%.o, $(arch_c_source_files))

# Architecture-specific ASM files
arch_asm_source_files := $(shell find src/arch/$(ARCH) -name '*.asm')
arch_asm_object_files := $(patsubst src/arch/$(ARCH)/%.asm, build/arch/$(ARCH)/%.o, $(arch_asm_source_files))

# All architecture object files
arch_object_files := $(arch_c_object_files) $(arch_asm_object_files)

# ========================
# Compilation rules
# ========================

# Kernel C files
$(kernel_object_files): build/kernel/%.o : src/kernel/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding $< -o $@

# Drivers C files
$(drivers_object_files): build/drivers/%.o : src/drivers/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding $< -o $@

# FS C files
$(fs_object_files): build/fs/%.o : src/fs/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding $< -o $@

# Arch C files
$(arch_c_object_files): build/arch/$(ARCH)/%.o : src/arch/$(ARCH)/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding $< -o $@

# Arch ASM files
$(arch_asm_object_files): build/arch/$(ARCH)/%.o : src/arch/$(ARCH)/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

# ========================
# Build target
# ========================
.PHONY: build-$(ARCH)
build-$(ARCH): $(kernel_object_files) $(drivers_object_files) $(fs_object_files) $(arch_object_files)
	mkdir -p dist/$(ARCH)
	x86_64-elf-ld -n -o dist/$(ARCH)/mdsys.sqr -T targets/$(ARCH)/linker.ld \
		$(kernel_object_files) $(drivers_object_files) $(fs_object_files) $(arch_object_files)
	cp dist/$(ARCH)/mdsys.sqr targets/$(ARCH)/iso/ModuOS/System64/mdsys.sqr
	grub-mkrescue -o dist/$(ARCH)/kernel.iso targets/$(ARCH)/iso

# ========================
# Clean build artifacts
# ========================
.PHONY: clean
clean:
	rm -rf build dist
