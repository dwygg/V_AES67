@echo off
setlocal
echo ================================================
echo   AES67 - Build Release Package
echo ================================================
set RELEASE=E:\jmdev\AES67\release
set QT=D:\Qt\6.8.3\msvc2022_64
set WDK_TOOLS=C:\Program Files (x86)\Windows Kits\10\Tools\10.0.28000.0\x64
set VS_VARS=D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat

rmdir /s /q "%RELEASE%" 2>nul
mkdir "%RELEASE%" 2>nul

echo.
echo [1/6] Building engine...
call "%VS_VARS%" >nul
cd /d "E:\jmdev\AES67\engine"
cl /EHsc /std:c++17 /O2 /nologo /W3 /utf-8 main.cpp aes67_engine.cpp wasapi_device.cpp audio_thread.cpp logger.cpp network_thread.cpp sap_announcer.cpp network_receiver.cpp audio_render_thread.cpp ptp_clock.cpp ptp_thread.cpp pipe_server.cpp /Fe:aes67_engine.exe ole32.lib avrt.lib ws2_32.lib winmm.lib >nul 2>&1
if exist aes67_engine.exe (echo        OK) else (echo        FAILED & goto :error)

echo.
echo [2/6] Building panel...
cd /d "E:\jmdev\AES67\panel\build"
cmake --build . --config Release -- /nologo /v:q >nul 2>&1
if exist Release\aes67_panel.exe (echo        OK) else (echo        FAILED & goto :error)

echo.
echo [3/6] Building launcher...
cd /d "E:\jmdev\AES67"
cl /EHsc /nologo /utf-8 launcher.cpp /Fe:AES67_Launcher.exe /link /SUBSYSTEM:WINDOWS >nul 2>&1
del launcher.obj 2>nul
if exist AES67_Launcher.exe (echo        OK) else (echo        FAILED & goto :error)

echo.
echo [4/6] Copying files to release\...
cd /d "E:\jmdev\AES67"

copy AES67_Launcher.exe "%RELEASE%\" >nul

mkdir "%RELEASE%\engine" 2>nul
copy engine\aes67_engine.exe "%RELEASE%\engine\" >nul
copy engine\routing.json "%RELEASE%\engine\" >nul
copy engine\dsp.json "%RELEASE%\engine\" >nul

mkdir "%RELEASE%\panel" 2>nul
copy panel\build\Release\aes67_panel.exe "%RELEASE%\panel\" >nul
copy "%QT%\bin\Qt6Core.dll" "%RELEASE%\panel\" >nul
copy "%QT%\bin\Qt6Gui.dll" "%RELEASE%\panel\" >nul
copy "%QT%\bin\Qt6Widgets.dll" "%RELEASE%\panel\" >nul
mkdir "%RELEASE%\panel\platforms" 2>nul
copy "%QT%\plugins\platforms\qwindows.dll" "%RELEASE%\panel\platforms\" >nul
mkdir "%RELEASE%\panel\styles" 2>nul
copy "%QT%\plugins\styles\qmodernwindowsstyle.dll" "%RELEASE%\panel\styles\" >nul

mkdir "%RELEASE%\driver" 2>nul
copy driver\x64\Win10\AES67Driver.sys "%RELEASE%\driver\" >nul
copy driver\x64\Win10\AES67Driver.inf "%RELEASE%\driver\" >nul
copy driver\x64\Win10\AES67Driver.cer "%RELEASE%\driver\" >nul
copy "driver\x64\Win10\AES67Driver\aes67driver.cat" "%RELEASE%\driver\" >nul 2>nul
copy "%WDK_TOOLS%\devcon.exe" "%RELEASE%\driver\" >nul 2>nul

echo        Done.

echo.
echo [5/6] Generating release install.bat (relative paths)...
> "%RELEASE%\install.bat" (
    echo @echo off
    echo setlocal enabledelayedexpansion
    echo echo ================================================
    echo echo   AES67 Virtual Soundcard - Installer
    echo echo ================================================
    echo echo.
    echo echo This script needs ADMIN privileges.
    echo echo.
    echo net session ^>nul 2^>^&1
    echo if %%errorlevel%% neq 0 (
    echo     echo [ERROR] Right-click ^> Run as administrator.
    echo     pause
    echo     exit /b 1
    echo ^)
    echo.
    echo set DRIVER=%%~dp0driver
    echo set DEVCON=%%~dp0driver\devcon.exe
    echo.
    echo echo [1/5] Enabling Test Signing Mode...
    echo bcdedit /enum ^| findstr "testsigning" ^| findstr "Yes" ^>nul
    echo if %%errorlevel%% equ 0 (
    echo     echo        Already ON.
    echo     set REBOOT=0
    echo ^) else (
    echo     bcdedit /set testsigning on
    echo     echo        Enabled. Restart required.
    echo     set REBOOT=1
    echo ^)
    echo.
    echo echo [2/5] Importing certificate...
    echo certutil -addstore Root "%%DRIVER%%\AES67Driver.cer" ^>nul 2^>^&1
    echo certutil -addstore TrustedPublisher "%%DRIVER%%\AES67Driver.cer" ^>nul 2^>^&1
    echo echo        OK.
    echo.
    echo echo [3/5] Installing driver...
    echo pnputil /enum-drivers ^| findstr "AES67Driver" ^>nul
    echo if %%errorlevel%% equ 0 (
    echo     echo        Removing old driver...
    echo     if exist "%%DEVCON%%" "%%DEVCON%%" remove *AES67Driver ^>nul 2^>^&1
    echo     for /f "tokens=*" %%%%i in ^('pnputil /enum-drivers ^| findstr /i "AES67Driver"'^) do (
    echo         for /f "tokens=2 delims=:" %%%%j in ^("%%%%i"^) do (
    echo             set OEM=%%%%j
    echo             set OEM=!OEM: =!
    echo             pnputil /delete-driver !OEM! ^>nul 2^>^&1
    echo         ^)
    echo     ^)
    echo ^)
    echo pnputil /add-driver "%%DRIVER%%\AES67Driver.inf" /install
    echo echo        Driver installed.
    echo if exist "%%DEVCON%%" (
    echo     "%%DEVCON%%" install "%%DRIVER%%\AES67Driver.inf" "*AES67Driver" ^>nul 2^>^&1
    echo     echo        Device node created.
    echo ^)
    echo.
    echo echo [4/5] Restarting Audio service...
    echo net stop audiosrv ^>nul 2^>^&1
    echo net stop AudioEndpointBuilder ^>nul 2^>^&1
    echo net start AudioEndpointBuilder ^>nul 2^>^&1
    echo net start audiosrv ^>nul 2^>^&1
    echo echo        Done.
    echo.
    echo echo [5/5] Verification...
    echo pnputil /enum-devices /class MEDIA ^| findstr "AES67" ^>nul
    echo if %%errorlevel%% equ 0 (echo        [OK] AES67Driver device found.^) else (echo        [WARN] Not found.^)
    echo.
    echo echo ================================================
    echo echo   Install complete!
    echo echo ================================================
    echo echo   Launch: AES67_Launcher.exe
    echo echo.
    echo if %%REBOOT%% equ 1 (
    echo     echo   [!] You MUST restart before the driver works.
    echo     echo   [!] If Secure Boot is ON, disable it in BIOS first.
    echo     choice /c YN /m "Restart now?"
    echo     if !errorlevel! equ 1 shutdown /r /t 0
    echo ^) else (
    echo     echo   No restart needed.
    echo ^)
    echo pause
)
echo        Done.

echo.
echo [6/6] Release package:
echo.
dir "%RELEASE%" /b
echo ================================================
echo   Release: %RELEASE%
echo   Give this folder to others.
echo   They run install.bat (Admin) then AES67_Launcher.exe
echo ================================================
goto :done

:error
echo.
echo   BUILD FAILED.
pause

:done
endlocal
