@echo off
call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /EHsc /std:c++17 /O2 /nologo /W3 sine_test.cpp /Fe:sine_test.exe ole32.lib avrt.lib 2>&1 | head -30
