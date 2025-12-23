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
REM Boot the kernel ISO in QEMU (AHCI with forced PS/2 keyboard)
REM -----------------------------
echo. > com1.log
echo. > com2.log
timeout /t 1 /nobreak >nul

REM Start QEMU
start "cmdQEMU" qemu-system-x86_64 ^
    -machine pc ^
   -smbios type=1,manufacturer="ASRock",product="ASRock B450 Gaming" ^
    -m 1024M ^
    -smp 2 ^
    -serial file:com1.log ^
    -serial file:com2.log ^
    -drive file=dist\AMD64\kernel.iso,format=raw,media=cdrom,if=none,id=cdrom0 ^
    -drive file=.\disk.img,format=raw,media=disk,if=none,id=disk0 ^
    -device ahci,id=ahci0 ^
    -device ide-cd,drive=cdrom0,bus=ahci0.0 ^
    -device ide-hd,drive=disk0,bus=ahci0.1 ^
    -boot d


timeout /t 1 /nobreak >nul

".\vendor\NTSoftware\Log Viewer.exe" com1.log com2.log

endlocal