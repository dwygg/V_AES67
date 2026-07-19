@echo off
REM ============================================================
REM  AES67 虚拟声卡驱动 - 命令行编译脚本
REM ============================================================
REM  使用前请按本机环境修改下面 3 个占位路径 / 版本号：
REM    1. WindowsSDKVersion   -> 本机已安装的 WDK/SDK 版本号
REM       (查 "C:\Program Files (x86)\Windows Kits\10\Include\" 下的目录名)
REM    2. vcvars64.bat 路径   -> 本机 Visual Studio 的安装路径
REM       (VS2022=\17\，VS2026=\18\；社区版=Community，专业版=Professional)
REM    3. cd /d 目标          -> 本机本驱动目录 (即本 build.bat 所在目录)
REM ------------------------------------------------------------

REM ---- 1. WDK / SDK 版本 (改成本机实际版本号) ----
set WDKCONTENTROOT=C:\Program Files (x86)\Windows Kits\10\
set WindowsSDKVersion=<在此填入本机 WDK 版本, 例如 10.0.22621.0>
set WindowsSDKLibVersion=%WindowsSDKVersion%\
set WindowsSdkDir=C:\Program Files (x86)\Windows Kits\10\

REM ---- 2. 初始化 VS x64 编译环境 (改成本机 VS 路径) ----
REM   VS2022 示例: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
REM   VS2026 示例: "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call "<在此填入本机 vcvars64.bat 完整路径>"

REM ---- 3. 切到本驱动目录 (改成本机路径, 或直接在本目录运行本脚本) ----
cd /d "%~dp0"

REM ---- 4. 编译 (配置名固定为 Win10, 不是 Debug/Release) ----
msbuild AES67Driver.vcxproj /p:Configuration=Win10 /p:Platform=x64 /t:Build
if %errorlevel% equ 0 (echo BUILD SUCCESS) else (echo BUILD FAILED)
pause
