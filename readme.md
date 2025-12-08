# ModuOS

<p align="center">
  <img src="https://img.shields.io/badge/OS-Hobby%20Project-blue" alt="OS Type">
  <img src="https://img.shields.io/badge/Arch-AMD64-green" alt="Architecture">
  <img src="https://img.shields.io/badge/License-GPLv2-orange" alt="License">
  <img src="https://img.shields.io/badge/Language-C%20%2B%20Assembly-yellow" alt="Language">
</p>

**ModuOS** is a hobby operating system written in C and x86-64 assembly, designed for learning and experimentation. It features a custom kernel with interrupt handling, memory management (paging, physical memory allocator), multitasking with process management, and support for multiple file systems (FAT32, ISO9660).

## ✨ Features

- **Custom 64-bit Kernel**: Long mode x86-64 kernel with Multiboot2 support
- **Memory Management**: Physical memory allocator, virtual memory with paging, kernel heap
- **Process Management**: Multitasking, context switching, ELF executable loading, syscall interface
- **Drivers**: 
  - Storage: ATA/ATAPI, AHCI/SATA support
  - Graphics: VGA text mode
  - Input: PS/2 keyboard and mouse
  - System: PCI enumeration, ACPI, RTC, PIC/Timer
- **File Systems**: FAT32 and ISO9660 (read-only) with virtual file system layer
- **Userland**: Basic applications and games (cat, echo, shell, games)
- **Built-in Games**: Raycaster FPS, Snake (Eat Fruit), Stack Blocks, Vertical Ping Pong, Mine Sweeper

---

## 📋 Requirements

### Build Environment
- **[Docker Desktop](https://www.docker.com/products/docker-desktop)** - For cross-compilation environment
- **[QEMU](https://www.qemu.org/)** - For testing and running the OS
- **Windows** - Primary tested platform (Linux/macOS may work with modifications)

### Optional
- **Visual Studio Code** or any code editor for development
- **Git** for version control

---

## 🚀 Quick Start

### 1. Clone the Repository

```bash
git clone https://github.com/NtinosTheGamer2324/ModuOS.git
cd ModuOS
```

### 2. Build the Docker Build Environment

The project uses a Docker container with a cross-compiler toolchain. Build the image:

```bash
docker build buildenv -t modu-os
```

This creates a `modu-os` image containing:
- x86_64-elf-gcc cross-compiler
- NASM assembler
- GRUB bootloader tools
- xorriso for ISO creation

### 3. Create a Disk Image

ModuOS requires a 1GB FAT32-formatted disk image named `disk.img` in the project root.

**Option A: Using PowerShell (Windows)**
```powershell
# Create 1GB file
fsutil file createnew disk.img 1073741824

# Use Disk Management or diskpart to format as FAT32
```

**Option B: Using third-party tools**
- [Win32 Disk Imager](https://sourceforge.net/projects/win32diskimager/)
- [Rufus](https://rufus.ie/) (create a FAT32 formatted file)

> ⚠️ **Important**: The build script does **not** create `disk.img` automatically. You must provide it.

### 4. Build and Run

Simply run the batch script:

```bash
run.bat
```

This will:
1. ✅ Check if Docker is running (and start it if needed)
2. 🔨 Build the kernel inside the Docker container
3. 📦 Create a bootable ISO with GRUB2
4. 🚀 Launch QEMU with the ISO and disk image
5. 📄 Open log viewer for `com1.log` and `com2.log`

### 5. Log Viewer Setup (Optional)

To view COM port logs during runtime:

1. Download [Log Viewer v1](https://sourceforge.net/projects/log-viewer-v1/)
2. Place it at: `vendor/NTSoftware/Log Viewer.exe`
3. The `run.bat` script will automatically open it

---

## 📁 Project Structure

```
ModuOS/
├── buildenv/           # Docker build environment configuration
├── include/            # Header files
│   └── moduos/         # Kernel, driver, and filesystem headers
│       ├── arch/       # Architecture-specific (AMD64)
│       ├── drivers/    # Hardware driver headers
│       ├── fs/         # File system headers
│       └── kernel/     # Kernel subsystem headers
├── src/                # Source code
│   ├── arch/AMD64/     # x86-64 specific code (boot, interrupts, syscalls)
│   ├── drivers/        # Hardware drivers (ATA, AHCI, VGA, PCI, etc.)
│   ├── fs/             # File system implementations
│   └── kernel/         # Core kernel (memory, process, scheduler, etc.)
├── targets/AMD64/      # Build target configuration
│   ├── iso/            # ISO layout for bootable image
│   └── linker.ld       # Linker script
├── userland/           # User-space programs
├── LICENSES/           # Third-party licenses
├── Makefile            # Build system
├── run.bat             # Build and test script
└── LICENSE             # Project license (GPLv2)
```

---

## 🛠️ Manual Build Steps

If you prefer to build manually without `run.bat`:

```bash
# Build the kernel
docker run --rm -it -v "%cd%":/root/env modu-os make clean build-AMD64

# Run in QEMU
qemu-system-x86_64 ^
    -M pc-i440fx-6.2 ^
    -m 1024M ^
    -smp 2 ^
    -serial file:com1.log ^
    -serial file:com2.log ^
    -drive file=dist/AMD64/kernel.iso,format=raw,media=cdrom ^
    -drive id=disk,file=disk.img,if=none,format=raw ^
    -device ahci,id=ahci ^
    -device ide-hd,drive=disk,bus=ahci.0 ^
    -boot d
```

---

## 🎮 Available Applications & Games

Once booted, you can run:

### Applications
- `cat <file>` - Display file contents
- `echo <text>` - Print text to console
- `sh` - Simple shell
- `memtest` - Memory testing utility
- `zsfetch` - System information display

### Games
- **Raycaster FPS** - First-person perspective game
- **Eat Fruit** (Snake) - Classic snake game
- **Stack Blocks** - Block stacking puzzle
- **Vertical Ping Pong** - Pong variant
- **Mine Sweeper** - Minesweeper clone

---

## 🧪 Development

### Building Userland Programs

```bash
cd userland
./build.sh  # Linux/WSL
```

User programs are compiled separately and placed in `targets/AMD64/iso/Apps/`.

### Debugging

- **Serial Output**: Kernel outputs debug information to COM1 and COM2 ports
- **QEMU Monitor**: Press `Ctrl+Alt+2` in QEMU to access the monitor
- **GDB Debugging**: Add `-s -S` to QEMU args and connect with `gdb`

```bash
qemu-system-x86_64 -cdrom dist/AMD64/kernel.iso -s -S &
gdb dist/AMD64/mdsys.sqr
(gdb) target remote localhost:1234
(gdb) continue
```

---

## 🤝 Contributing

Contributions are welcome! Here's how you can help:

1. 🍴 **Fork** the repository
2. 🌿 **Create** a feature branch (`git checkout -b feature/amazing-feature`)
3. 💾 **Commit** your changes (`git commit -m 'Add amazing feature'`)
4. 📤 **Push** to the branch (`git push origin feature/amazing-feature`)
5. 🔃 **Open** a Pull Request

### Contribution Guidelines
- Follow existing code style and conventions
- Comment your code where necessary
- Test your changes in QEMU before submitting
- Update documentation if you add new features

---

## 📄 License

This project is licensed under the **GNU General Public License v2.0 only** - see the [LICENSE](LICENSE) file for details.

### Third-Party Components

ModuOS uses several third-party components:
- **GRUB2** - Bootloader (GPLv3+)
- **Multiboot2** - Boot protocol specification
- **GCC Cross-Compiler** - Build toolchain (GPLv3+)

See the [LICENSES](LICENSES/) folder for full license texts of third-party components.

---

## 🙏 Acknowledgments

- **OSDev Community** - Invaluable resources at [wiki.osdev.org](https://wiki.osdev.org)
- **Multiboot2 Specification** - Boot protocol by GNU GRUB
- **randomdude/gcc-cross-x86_64-elf** - Docker base image for cross-compilation

---

## 📞 Contact & Links

- **Project Repository**: [github.com/NtinosTheGamer2324/ModuOS](https://github.com/NtinosTheGamer2324/ModuOS)
- **Issues & Bug Reports**: [GitHub Issues](https://github.com/NtinosTheGamer2324/ModuOS/issues)
- **Developer**: New Technologies Software

---

<p align="center">Made with ❤️ for learning and fun</p>