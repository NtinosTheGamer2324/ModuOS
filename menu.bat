@echo off
setlocal enabledelayedexpansion
:menu
cls
echo  made in the hellenic republic :)
echo ==============================================
echo             Modu-OS Launch Menu
echo ==============================================
echo.
echo  [1] Build and Run (Full Pipeline)
echo  [2] Just Run (Skip Docker Build)
echo  [3] Exit
echo.
echo ==============================================
set /p choice="Select an option (1-3): "

if "%choice%"=="1" goto option1
if "%choice%"=="2" goto option2
if "%choice%"=="3" goto option3

:: If invalid input, loop back
echo.
echo Invalid selection, please try again.
timeout /t 2 >nul
goto menu

:option1
echo.
echo Launching full build and run pipeline...
timeout /t 1 >nul
call run.bat
goto end

:option2
echo.
echo Launching QEMU in run-only mode...
timeout /t 1 >nul
call run.bat --run-only
goto end

:option3
echo.
echo Exiting...
timeout /t 1 >nul

:end
endlocal