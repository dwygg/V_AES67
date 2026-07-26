@echo off
setlocal enabledelayedexpansion
echo ================================================
echo   AES67 Virtual Soundcard - Installer
echo ================================================
echo.
echo This script needs ADMIN privileges.
echo The following steps require a RESTART:
echo   1. Enable Windows Test Signing Mode
echo   2. (if Secure Boot is ON: disable in BIOS manually)
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Please run this script as Administrator.
    echo Right-click install.bat ^> Run as administrator.
    pause
    exit /b 1
)

set DRIVER=E:\jmdev\AES67\driver\x64\Win10
set TOOLS=C:\Program Files (x86)\Windows Kits\10\Tools

REM ---- Find devcon.exe ----
set DEVCON=
for /d %%v in ("%TOOLS%\*") do (
    if exist "%%v\x64\devcon.exe" set DEVCON=%%v\x64\devcon.exe
)
if "%DEVCON%"=="" (
    echo [WARN] devcon.exe not found. Driver device node will need manual install.
    echo        Install WDK or copy devcon.exe to a PATH directory.
)

REM ============================================
REM  Step 1: Enable Test Signing Mode
REM ============================================
echo.
echo [1/5] Enabling Test Signing Mode...
bcdedit /enum | findstr "testsigning" | findstr "Yes" >nul
if %errorlevel% equ 0 (
    echo        Test Signing Mode is already ON.
    set REBOOT=0
) else (
    bcdedit /set testsigning on
    echo        Test Signing Mode ENABLED. A restart is required before drivers will load.
    set REBOOT=1
)

REM ============================================
REM  Step 2: Import Test Certificate
REM ============================================
echo.
echo [2/5] Importing test certificate...
certutil -addstore Root "%DRIVER%\AES67Driver.cer" >nul 2>&1
certutil -addstore TrustedPublisher "%DRIVER%\AES67Driver.cer" >nul 2>&1
echo        Certificate imported.

REM ============================================
REM  Step 3: Install Driver
REM ============================================
echo.
echo [3/5] Installing driver...

REM Copy latest catalog
if exist "%DRIVER%\AES67Driver\aes67driver.cat" (
    copy /y "%DRIVER%\AES67Driver\aes67driver.cat" "%DRIVER%\aes67driver.cat" >nul
)

REM Remove old driver if present
pnputil /enum-drivers | findstr "AES67Driver" >nul
if %errorlevel% equ 0 (
    echo        Removing old driver...
    if "%DEVCON%" neq "" "%DEVCON%" remove *AES67Driver >nul 2>&1
    for /f "tokens=*" %%i in ('pnputil /enum-drivers ^| findstr /i "AES67Driver"') do (
        for /f "tokens=2 delims=:" %%j in ("%%i") do (
            set OEM=%%j
            set OEM=!OEM: =!
            pnputil /delete-driver !OEM! >nul 2>&1
        )
    )
)

pnputil /add-driver "%DRIVER%\AES67Driver.inf" /install
echo        Driver package added.

REM Create device node
if "%DEVCON%" neq "" (
    "%DEVCON%" install "%DRIVER%\AES67Driver.inf" "*AES67Driver" >nul 2>&1
    echo        Device node created.
)

REM ============================================
REM  Step 4: Restart Audio Service
REM ============================================
echo.
echo [4/5] Restarting Windows Audio service...
net stop audiosrv >nul 2>&1
net stop AudioEndpointBuilder >nul 2>&1
net start AudioEndpointBuilder >nul 2>&1
net start audiosrv >nul 2>&1
echo        Audio service restarted.

REM ============================================
REM  Step 5: Verify
REM ============================================
echo.
echo [5/5] Verification...
pnputil /enum-devices /class MEDIA | findstr "AES67" >nul
if %errorlevel% equ 0 (
    echo        [OK] AES67Driver device found.
) else (
    echo        [WARN] AES67Driver device not found in device list.
)

REM ============================================
REM  Done
REM ============================================
echo.
echo ================================================
echo   Installation complete!
echo ================================================
echo.
echo   Launch:  AES67_Launcher.exe  (double-click)
echo   or manually:
echo            engine\aes67_engine.exe --duration 0 --managed --rx
echo            panel\build\Release\aes67_panel.exe
echo.
if %REBOOT% equ 1 (
    echo   [!] Test Signing Mode was just enabled.
    echo   [!] You MUST restart Windows before the driver will work.
    echo   [!] If Secure Boot is ON, disable it in BIOS first, then restart.
    echo.
    choice /c YN /m "Restart now?"
    if !errorlevel! equ 1 shutdown /r /t 0
) else (
    echo   Driver is ready. No restart needed.
)
echo.
pause
