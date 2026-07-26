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

REM Launcher
copy AES67_Launcher.exe "%RELEASE%\" >nul

REM Engine
mkdir "%RELEASE%\engine" 2>nul
copy engine\aes67_engine.exe "%RELEASE%\engine\" >nul
copy engine\routing.json "%RELEASE%\engine\" >nul
copy engine\dsp.json "%RELEASE%\engine\" >nul

REM Panel + Qt DLLs
mkdir "%RELEASE%\panel" 2>nul
copy panel\build\Release\aes67_panel.exe "%RELEASE%\panel\" >nul
copy "%QT%\bin\Qt6Core.dll" "%RELEASE%\panel\" >nul
copy "%QT%\bin\Qt6Gui.dll" "%RELEASE%\panel\" >nul
copy "%QT%\bin\Qt6Widgets.dll" "%RELEASE%\panel\" >nul
mkdir "%RELEASE%\panel\platforms" 2>nul
copy "%QT%\plugins\platforms\qwindows.dll" "%RELEASE%\panel\platforms\" >nul
mkdir "%RELEASE%\panel\styles" 2>nul
copy "%QT%\plugins\styles\qmodernwindowsstyle.dll" "%RELEASE%\panel\styles\" >nul

REM Driver
mkdir "%RELEASE%\driver" 2>nul
copy driver\x64\Win10\AES67Driver.sys "%RELEASE%\driver\" >nul
copy driver\x64\Win10\AES67Driver.inf "%RELEASE%\driver\" >nul
copy driver\x64\Win10\AES67Driver.cer "%RELEASE%\driver\" >nul
copy "driver\x64\Win10\AES67Driver\aes67driver.cat" "%RELEASE%\driver\" >nul 2>nul

REM Devcon
copy "%WDK_TOOLS%\devcon.exe" "%RELEASE%\driver\" >nul 2>nul

REM Installer
copy install.bat "%RELEASE%\" >nul

echo        Done.

echo.
echo [5/6] Updating installer paths for release build...
cd /d "%RELEASE%"
REM Driver path in installer: change to .\driver
powershell -Command "(gc install.bat) -replace 'E:\\\\jmdev\\\\AES67\\\\driver\\\\x64\\\\Win10', '.\driver' | Out-File -Encoding ASCII install.bat"
REM devcon path: use .\driver\devcon.exe directly
powershell -Command "(gc install.bat) -replace '::program files.*devcon.*', '' | Out-File -Encoding ASCII install.bat"

echo        Done.

echo.
echo [6/6] Release package ready:
echo.
dir "%RELEASE%" /b
echo.
echo ================================================
echo   Release: %RELEASE%
echo   Size:
cd /d "%RELEASE%" && dir /s | findstr "File(s)"
echo ================================================
echo.
echo   Give the release\ folder to others.
echo   They just need to run install.bat (as Admin).
echo.
goto :done

:error
echo.
echo   BUILD FAILED. Check error messages above.
pause

:done
endlocal
