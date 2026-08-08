@echo off
setlocal
cls

echo made in the hellenic republic :)

REM -----------------------------
REM Check for arguments
REM -----------------------------
if "%~1"=="--run-only" (
    echo [Run-Only Mode] Skipping Docker check and Kernel build.
    goto start_qemu
)

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


:start_qemu
REM -----------------------------
REM Boot the kernel ISO in QEMU (AHCI with forced PS/2 keyboard)
REM -----------------------------
echo. > com1.log
echo. > com2.log
echo. > com3.log
timeout /t 1 /nobreak >nul

REM Start QEMU
start "cmdQEMU" qemu-system-x86_64 ^
    -machine pc ^
    -smbios type=1,manufacturer="NTLLC",product="DevmanPC",version="1.0",serial="MDMDMDMDMDMD" ^
    -m 4096M ^
    -smp 2 ^
    -serial file:com1.log ^
    -serial file:com2.log ^
    -serial file:com3.log ^
    -audiodev dsound,id=snd0 ^
    -device intel-hda -device hda-duplex,audiodev=snd0 ^
    -device piix3-usb-uhci,id=usb0 ^
    -device usb-mouse,bus=usb0.0 ^
    -netdev user,id=u1 -device e1000,netdev=u1,mac=52:54:00:12:34:56 ^
    -drive file=dist\AMD64\kernel.iso,format=raw,media=cdrom,if=none,id=cdrom0 ^
    -drive file=.\disk.img,format=raw,media=disk,if=none,id=disk0 ^
    -drive file=.\ext2.img,format=raw,media=disk,if=none,id=disk1 ^
    -drive file=.\mdfs_disk.img,format=raw,media=disk,if=none,id=disk2 ^
    -device ahci,id=ahci0 ^
    -device ide-cd,drive=cdrom0,bus=ahci0.0 ^
    -device ide-hd,drive=disk0,bus=ahci0.1 ^
    -device ide-hd,drive=disk1,bus=ahci0.2 ^
    -device ide-hd,drive=disk2,bus=ahci0.3 ^
    -boot d

timeout /t 1 /nobreak >nul

".\vendor\NTSoftware\Log Viewer.exe" com1.log com2.log com3.log

endlocal