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
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -DFBCON_DEBUG=1 $< -o $@

# Drivers C files
$(drivers_object_files): build/drivers/%.o : src/drivers/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -DFBCON_DEBUG=1 $< -o $@

# FS C files
$(fs_object_files): build/fs/%.o : src/fs/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -DFBCON_DEBUG=1 $< -o $@

# Arch C files
$(arch_c_object_files): build/arch/$(ARCH)/%.o : src/arch/$(ARCH)/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -DFBCON_DEBUG=1 $< -o $@

# Arch ASM files
$(arch_asm_object_files): build/arch/$(ARCH)/%.o : src/arch/$(ARCH)/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

# ========================
# Kernel modules (.sqrm)
# ========================

sqrm_modules_src := $(shell find modules -name '*_sqrm.c')
sqrm_modules_out := $(patsubst modules/%_sqrm.c, dist/$(ARCH)/md/%.sqrm, $(sqrm_modules_src))

$(sqrm_modules_out): dist/$(ARCH)/md/%.sqrm : modules/%_sqrm.c
	mkdir -p $(dir $@)
	# Build as ELF64 ET_DYN (shared-object style module)
	x86_64-elf-gcc -I include -ffreestanding -fPIC -mno-red-zone -nostdlib -Wl,-shared -Wl,-e,sqrm_module_init $< -o $@

# ========================
# Build target
# ========================
.PHONY: build-$(ARCH)
build-$(ARCH): $(kernel_object_files) $(drivers_object_files) $(fs_object_files) $(arch_object_files) $(sqrm_modules_out)
	@echo Building kernel
	mkdir -p dist/$(ARCH)
	x86_64-elf-ld -n -o dist/$(ARCH)/mdsys.sqr -T targets/$(ARCH)/linker.ld -Map dist/$(ARCH)/mdsys.map \
		$(kernel_object_files) $(drivers_object_files) $(fs_object_files) $(arch_object_files) 
	cp dist/$(ARCH)/mdsys.sqr targets/$(ARCH)/iso/ModuOS/System64/mdsys.sqr
	# Copy kernel modules into the ISO
	mkdir -p targets/$(ARCH)/iso/ModuOS/System64/md
	cp -f dist/$(ARCH)/md/*.sqrm targets/$(ARCH)/iso/ModuOS/System64/md/ 2>/dev/null || true
	@echo Building userland apps
	chmod +x userland/build.sh
	# Normalize line endings in case repo is checked out with CRLF (Windows)
	sed -i 's/\r$$//' userland/build.sh
	cd userland && sh ./build.sh
	cp -f userland/dist/*.sqr targets/$(ARCH)/iso/Apps/
	# Copy userland dynamic linker + shared libraries
	mkdir -p targets/$(ARCH)/iso/ModuOS/shared/usr/lib
	cp -f userland/dist/*.sqrl targets/$(ARCH)/iso/ModuOS/shared/usr/lib/ 2>/dev/null || true
	cp -f userland/dist/ld-moduos.sqr targets/$(ARCH)/iso/ModuOS/shared/usr/lib/ 2>/dev/null || true
	@echo Building ISO
	grub-mkrescue -o dist/$(ARCH)/kernel.iso targets/$(ARCH)/iso

# ========================
# Clean build artifacts
# ========================
.PHONY: clean
clean:
	rm -rf build dist
