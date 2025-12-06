# ModuOS

ModuOS is a hobby operating system written in C and assembly for the AMD64 architecture. It includes a custom kernel, drivers, file system support, and basic applications and games. The project uses Docker for building and QEMU for testing.

## Requirements

- [Docker Desktop](https://www.docker.com/products/docker-desktop)
- [QEMU](https://www.qemu.org/)
- Windows (tested) or Linux/macOS with modifications
- Optional: Visual Studio Code or other editors for development

---

## Getting Started

1. **Clone the repository:**

git clone https://github.com/NtinosTheGamer2324/ModuOS.git
cd ModuOS

2. **Build the Docker image for ModuOS:**

Before running the build script, you need to create the Docker image used for compiling the kernel:

docker build buildenv -t modu-os

This will use the Dockerfile in the `buildenv` folder to create the `modu-os` image.

3. **Ensure Docker is running.**  
   The `run.bat` script will automatically start Docker if it is not running.

4. **Provide a disk image manually:**  
   Create a **1GB FAT32-formatted disk image** named `disk.img` in the project root.  
   Example tools you can use on Windows:
   - [Win32 Disk Imager](https://sourceforge.net/projects/win32diskimager/)
   - `diskpart` + `fsutil` (advanced)

> ⚠️ The script **does not create the disk image automatically**. You must provide it manually.

5. **Run the build and test script:**

run.bat

This script will:
- Build the kernel inside Docker using the `modu-os` image.
- Boot the kernel in QEMU using your provided `disk.img`.
- Output logs to `com1.log` and `com2.log`.

> Note: ISO files and build artifacts are **not included** in the repository.

---

## Project Structure

``` YAML
ModuOS/
├── buildenv/ # Docker build environment
├── include/ # Header files for kernel and drivers
├── src/ # Source code for kernel, drivers, apps
├── targets/ # Build targets and ISO layout
├── run.bat # Build and test script
├── Makefile # Build instructions
└── LICENSE # Project license
```

### Log Viewer for COM Logs


To view the ModuOS log files (com1.log, com2.log), you need the Log Viewer application:

1. Download the Log Viewer executable from SourceForge:
   https://sourceforge.net/projects/log-viewer-v1/

2. Create the following folder structure inside your ModuOS project:
   vendor/
       NTSoftware/
           Log Viewer.exe

3. Place the downloaded Log Viewer.exe inside the NTSoftware folder.

⚠️ This step is required for the COM log viewing functionality. The repository does not include the executable due to size and licensing considerations.


---

## Contributing

Contributions are welcome! Please fork the repository, create a branch for your feature, and submit a pull request.

---

## License

This project is licensed under the GPLv2 License. See [LICENSE](LICENSE) for details.
