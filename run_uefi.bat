@echo off
setlocal
cls

echo made in greece :)

echo [UEFI] Build + boot ModuOS using OVMF

REM -----------------------------
REM Check if Docker is running
REM -----------------------------
docker info >nul 2>&1
if errorlevel 1 (
    echo Docker is not running. Starting Docker Desktop...
    start "" "C:\Program Files\Docker\Docker\Docker Desktop.exe"
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
REM Build in Docker (UEFI ISO)
REM -----------------------------
docker run --rm -it --privileged -v /dev:/dev -v "%cd%":/root/env modu-os /bin/bash -c "cd /root/env && make -j12 clean && make -j12 build-AMD64 && make -j12 iso-AMD64-uefi"

REM -----------------------------
REM Locate OVMF
REM -----------------------------
set "OVMF_CODE=%ProgramFiles%\qemu\share\edk2-x86_64-code.fd"
set "OVMF_VARS=%ProgramFiles%\qemu\share\edk2-i386-vars.fd"

if not exist "%OVMF_CODE%" (
    set "OVMF_CODE=%ProgramFiles%\qemu\share\edk2\x86_64\OVMF_CODE.fd"
)
if not exist "%OVMF_VARS%" (
    set "OVMF_VARS=%ProgramFiles%\qemu\share\edk2\x86_64\OVMF_VARS.fd"
)

if not exist "%OVMF_CODE%" (
    echo.
    echo ERROR: Could not find OVMF firmware.
    echo Please install QEMU with OVMF/EDK2 firmware files and update run_uefi.bat paths.
    echo Expected one of:
    echo   %ProgramFiles%\qemu\share\edk2-x86_64-code.fd
    echo   %ProgramFiles%\qemu\share\edk2\x86_64\OVMF_CODE.fd
    pause
    exit /b 1
)

REM Copy VARS to a writable file (OVMF vars must not be read-only)
set "OVMF_VARS_RW=%cd%\ovmf_vars.tmp.fd"
if exist "%OVMF_VARS%" (
    copy /y "%OVMF_VARS%" "%OVMF_VARS_RW%" >nul
) else (
    echo.
    echo WARNING: OVMF_VARS not found; continuing without persistent vars.
    set "OVMF_VARS_RW="
)

REM -----------------------------
REM Boot UEFI ISO in QEMU
REM -----------------------------
echo. > com1.log
echo. > com2.log
timeout /t 1 /nobreak >nul

set "ISO=dist\AMD64\kernel_uefi.iso"
if not exist "%ISO%" (
    echo ERROR: Missing %ISO%
    pause
    exit /b 1
)

REM Start QEMU (UEFI)
if not "%OVMF_VARS_RW%"=="" (
    start "cmdQEMU" qemu-system-x86_64 ^
        -machine q35 ^
        -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
        -drive if=pflash,format=raw,file="%OVMF_VARS_RW%" ^
        -smbios type=1,manufacturer="NTLLC",product="DevmanPC",version="1.0",serial="MDMDMDMDMDMD" ^
        -m 1024M ^
        -smp 2 ^
        -vga qxl ^
        -serial file:com1.log ^
        -serial file:com2.log ^
        -audiodev dsound,id=snd0 ^
        -device AC97,audiodev=snd0 ^
        -drive file=%ISO%,format=raw,media=cdrom,if=none,id=cdrom0 ^
        -drive file=.\disk.img,format=raw,media=disk,if=none,id=disk0 ^
        -drive file=.\ext2.img,format=raw,media=disk,if=none,id=disk1 ^
        -drive file=.\mdfs_disk.img,format=raw,media=disk,if=none,id=disk2 ^
        -device ahci,id=ahci0 ^
        -device ide-cd,drive=cdrom0,bus=ahci0.0 ^
        -device ide-hd,drive=disk0,bus=ahci0.1 ^
        -device ide-hd,drive=disk1,bus=ahci0.2 ^
        -device ide-hd,drive=disk2,bus=ahci0.3 ^
        -device ich9-usb-uhci1 -device usb-kbd -device usb-mouse ^
        -boot d
) else (
    start "cmdQEMU" qemu-system-x86_64 ^
        -machine q35 ^
        -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
        -smbios type=1,manufacturer="NTLLC",product="DevmanPC",version="1.0",serial="MDMDMDMDMDMD" ^
        -m 1024M ^
        -smp 2 ^
        -vga qxl ^
        -serial file:com1.log ^
        -serial file:com2.log ^
        -audiodev dsound,id=snd0 ^
        -device AC97,audiodev=snd0 ^
        -drive file=%ISO%,format=raw,media=cdrom,if=none,id=cdrom0 ^
        -drive file=.\disk.img,format=raw,media=disk,if=none,id=disk0 ^
        -drive file=.\ext2.img,format=raw,media=disk,if=none,id=disk1 ^
        -drive file=.\mdfs_disk.img,format=raw,media=disk,if=none,id=disk2 ^
        -device ahci,id=ahci0 ^
        -device ide-cd,drive=cdrom0,bus=ahci0.0 ^
        -device ide-hd,drive=disk0,bus=ahci0.1 ^
        -device ide-hd,drive=disk1,bus=ahci0.2 ^
        -device ide-hd,drive=disk2,bus=ahci0.3 ^
        -device ich9-usb-uhci1 -device usb-kbd -device usb-mouse ^
        -boot d
)

".\vendor\NTSoftware\Log Viewer.exe" com1.log com2.log

endlocal
