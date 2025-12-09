@echo off
setlocal
cls

REM -----------------------------
REM Check if Docker is running
REM -----------------------------
docker info >nul 2>&1
if errorlevel 1 (
    echo Docker is not running. Starting Docker Desktop...
    start "" "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    
    REM Wait for Docker to start (poll docker info until it works)
    echo Waiting for Docker to start...
    :waitloop
    timeout /t 3 >nul
    docker info >nul 2>&1
    if errorlevel 1 goto waitloop
    echo Docker started.
) else (
    echo Docker is already running.
)

REM -----------------------------
REM Build the kernel in Docker
REM -----------------------------
docker run --rm -it --privileged -v /dev:/dev -v "%cd%":/root/env modu-os /bin/bash -c "cd /root/env && make -j12 clean && make -j12 build-AMD64"

REM -----------------------------
REM Boot the kernel ISO in QEMU
REM -----------------------------
echo. > com1.log
echo. > com2.log
timeout /t 1 /nobreak >nul

REM Start QEMU in background and wait shortly for log files to start populating
REM Start QEMU with real VBE support and USB 2.0 keyboard
start "QEMU" qemu-system-x86_64 ^
    -M pc-i440fx-6.2 ^
    -m 1024M ^
    -smp 2 ^
    -serial file:com1.log ^
    -serial file:com2.log ^
    -drive file=dist\AMD64\kernel.iso,format=raw,index=0,media=cdrom ^
    -drive id=disk,file=.\disk.img,if=none,format=raw ^
    -device ahci,id=ahci ^
    -device ide-hd,drive=disk,bus=ahci.0 ^
    -device usb-ehci,id=ehci ^
    -device usb-kbd,bus=ehci.0 ^
    -boot d
REM    -vga std ^
REM    -no-reboot ^
REM    -no-shutdown


timeout /t 1 /nobreak >nul

".\vendor\NTSoftware\Log Viewer.exe" com1.log com2.log

REM -----------------------------
REM Optional: boot ISO only without disk image
REM -----------------------------
REM qemu-system-x86_64 -cdrom dist/AMD64/kernel.iso -boot d

endlocal