@echo off
REM Set up WDK build environment (includes WDK include/lib paths)
set WDKCONTENTROOT=C:\Program Files (x86)\Windows Kits\10\
set WindowsSDKVersion=10.0.22621.0
set WindowsSDKLibVersion=10.0.22621.0\
set WindowsSdkDir=C:\Program Files (x86)\Windows Kits\10\

call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

cd /d E:\jmdev\AES67\driver
msbuild AES67Driver.vcxproj /p:Configuration=Win10 /p:Platform=x64 /t:Build
if %errorlevel% equ 0 (echo BUILD SUCCESS) else (echo BUILD FAILED)
pause
