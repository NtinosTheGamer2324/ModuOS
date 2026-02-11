# Makefile for ModuOS x86_64

# Target architecture
ARCH := AMD64

# ========================
# Source file definitions
# ========================

# Kernel C files
kernel_c_source_files := $(shell find src/kernel -name '*.c')
kernel_c_object_files := $(patsubst src/kernel/%.c, build/kernel/%.o, $(kernel_c_source_files))

# Kernel ASM files
kernel_asm_source_files := $(shell find src/kernel -name '*.asm')
kernel_asm_object_files := $(patsubst src/kernel/%.asm, build/kernel/%.o, $(kernel_asm_source_files))

# All kernel object files
kernel_object_files := $(kernel_c_object_files) $(kernel_asm_object_files)

# Driver C files
# NOTE: USB stack is provided via SQRM modules; do not compile the in-kernel USB drivers.
drivers_c_source_files := $(shell find src/drivers -name '*.c' ! -path 'src/drivers/USB/*')
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
$(kernel_c_object_files): build/kernel/%.o : src/kernel/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie -DFBCON_DEBUG=1 $< -o $@

# Kernel ASM files
$(kernel_asm_object_files): build/kernel/%.o : src/kernel/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

# Drivers C files
$(drivers_object_files): build/drivers/%.o : src/drivers/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie -DFBCON_DEBUG=1 $< -o $@

# FS C files
$(fs_object_files): build/fs/%.o : src/fs/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie -DFBCON_DEBUG=1 $< -o $@

# Arch C files
$(arch_c_object_files): build/arch/$(ARCH)/%.o : src/arch/$(ARCH)/%.c
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I include -ffreestanding -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie -DFBCON_DEBUG=1 $< -o $@

# Arch ASM files
$(arch_asm_object_files): build/arch/$(ARCH)/%.o : src/arch/$(ARCH)/%.asm
	mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

# ========================
# SQRM module static libraries
# ========================

SQRM_LIBC := dist/$(ARCH)/md/libsqrmlibc.a

$(SQRM_LIBC):
	$(MAKE) -C sdk/sqrmlibc ARCH=$(ARCH) OUT_DIR=../../dist/$(ARCH)/md

# ========================
# Kernel modules (.sqrm)
# ========================

# SQRM modules built by the generic rule (exclude modules with custom Makefiles)
sqrm_modules_src := $(shell find modules -name '*_sqrm.c' ! -path 'modules/QXL/*')
sqrm_modules_out_generic := $(patsubst modules/%_sqrm.c, dist/$(ARCH)/md/%.sqrm, $(sqrm_modules_src))

# Special SQRM modules with their own build systems
sqrm_modules_out_special := dist/$(ARCH)/md/qxl_gpu.sqrm $(shell [ -f modules/NET/Makefile ] && echo dist/$(ARCH)/md/e1000.sqrm)

# QXL GPU module uses its own Makefile
# It outputs to dist/$(ARCH)/md/qxl_gpu.sqrm

# NET NIC driver modules (e1000 now; future *_sqrm.c in modules/NET will be built by that Makefile)
dist/$(ARCH)/md/e1000.sqrm:
	$(MAKE) -C modules/NET ARCH=$(ARCH)

$(filter dist/$(ARCH)/md/qxl_gpu.sqrm,$(sqrm_modules_out_special)):
	$(MAKE) -C modules/QXL ARCH=$(ARCH)

$(sqrm_modules_out_generic): dist/$(ARCH)/md/%.sqrm : modules/%_sqrm.c
	mkdir -p $(dir $@)
	# Build as ELF64 ET_DYN (shared-object style module)
	# Link against sqrmlibc so modules get freestanding memset/memcpy/etc without relying on loader symbol resolution.
	x86_64-elf-gcc -I include -I sdk/sqrmlibc/include -ffreestanding -fPIC -mno-red-zone -nostdlib -fno-builtin -fno-stack-protector \
	  -Wl,-shared -Wl,-e,sqrm_module_init $< $(SQRM_LIBC) -o $@

sqrm_modules_out := $(sqrm_modules_out_generic) $(sqrm_modules_out_special)

# ========================
# Build target
# ========================
.PHONY: build-$(ARCH)
build-$(ARCH): $(kernel_object_files) $(drivers_object_files) $(fs_object_files) $(arch_object_files) $(SQRM_LIBC) $(sqrm_modules_out)
	@echo Building kernel
	mkdir -p dist/$(ARCH)
	x86_64-elf-ld -n -o dist/$(ARCH)/mdsys.sqr -T targets/$(ARCH)/linker.ld -Map dist/$(ARCH)/mdsys.map \
		$(kernel_object_files) $(drivers_object_files) $(fs_object_files) $(arch_object_files) 
	cp dist/$(ARCH)/mdsys.sqr targets/$(ARCH)/iso/ModuOS/System64/mdsys.sqr
	# Copy kernel modules into the ISO (flattened)
	mkdir -p targets/$(ARCH)/iso/ModuOS/System64/md
	# Purge old modules to avoid stale .sqrm binaries persisting across builds
	rm -f targets/$(ARCH)/iso/ModuOS/System64/md/*.sqrm 2>/dev/null || true
	find dist/$(ARCH)/md -name '*.sqrm' -exec cp -f {} targets/$(ARCH)/iso/ModuOS/System64/md/ \; 2>/dev/null || true
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
	@echo Building BIOS ISO
	grub-mkrescue -o dist/$(ARCH)/kernel.iso targets/$(ARCH)/iso

.PHONY: iso-$(ARCH)-uefi
iso-$(ARCH)-uefi: build-$(ARCH)
	@echo Building UEFI ISO
	grub-mkrescue -o dist/$(ARCH)/kernel_uefi.iso targets/$(ARCH)/iso

.PHONY: build-$(ARCH)-uefi
build-$(ARCH)-uefi: iso-$(ARCH)-uefi

# ========================
# Clean build artifacts
# ========================
.PHONY: clean
clean:
	rm -rf build dist

.PHONY: check
check:
	@echo "Running Static Analysis for memory leaks on ModuOS..."
	@$(foreach file, $(kernel_c_source_files) $(drivers_c_source_files) $(fs_c_source_files) $(arch_c_source_files), \
		x86_64-elf-gcc $(file) -I include -ffreestanding -mcmodel=kernel $(ANALYZER_FLAGS) || true; \
	)
	@echo "Analysis complete."


