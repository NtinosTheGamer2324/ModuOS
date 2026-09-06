@echo off
setlocal

:: 1. Check for Docker Image
echo Have you already created the Docker image 'modu-os'?
choice /c YN /m "Press Y for Yes, N for No"

if errorlevel 2 (
    echo.
    echo Building the Docker image...
    docker build -t modu-os .\buildenv
    if %errorlevel% neq 0 (
        echo [ERROR] Docker build failed.
        pause
        exit /b 1
    )
)

:: 2. Check for Log Viewer
echo.
echo Do you have 'Log Viewer' installed?
choice /c YN /m "Press Y for Yes, N for No"

if errorlevel 2 (
    echo.
    echo Downloading Log Viewer...
    
    if not exist "vendor\ntsoftware" mkdir "vendor\ntsoftware"
    
    powershell -Command "Invoke-WebRequest -UserAgent 'Wget' -Uri 'https://sourceforge.net/projects/log-viewer-v1/files/Log%%20Viewer.exe/download' -OutFile 'vendor\ntsoftware\Log Viewer.exe'"
    
    if not exist "vendor\ntsoftware\Log Viewer.exe" (
        echo [ERROR] Download failed.
        pause
    ) else (
        echo.
        echo Download complete.
    )
)

:: 3. Run the project
echo.
echo Executing run.bat...
if exist "run.bat" (
    call run.bat
) else (
    echo [ERROR] run.bat not found in current directory.
    pause
)
exit /b