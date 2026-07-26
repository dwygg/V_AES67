// AES67 一键启动器 — 双击运行，无窗口，后台启动引擎 + 面板
// 关闭面板后自动停止引擎
// 编译: cl /EHsc /nologo /utf-8 launcher.cpp /Fe:AES67_Launcher.exe /link /SUBSYSTEM:WINDOWS

#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 1. 启动引擎（隐藏窗口）
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION piEng = {};

    if (!CreateProcessW(L"engine\\aes67_engine.exe",
            (LPWSTR)L"engine\\aes67_engine.exe --duration 0 --managed --rx",
            nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, L"engine", &si, &piEng)) {
        return 1;
    }
    CloseHandle(piEng.hThread);

    // 2. 等引擎初始化完毕（管道就绪）
    Sleep(2000);

    // 3. 启动面板
    PROCESS_INFORMATION piPanel = {};
    if (!CreateProcessW(L"panel\\build\\Release\\aes67_panel.exe",
            nullptr, nullptr, nullptr, FALSE,
            0, nullptr, L"panel\\build\\Release", &si, &piPanel)) {
        if (!CreateProcessW(L"aes67_panel.exe", nullptr,
                nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &piPanel)) {
            TerminateProcess(piEng.hProcess, 0);
            CloseHandle(piEng.hProcess);
            return 2;
        }
    }

    // 4. 等面板关闭
    if (piPanel.hProcess) {
        WaitForSingleObject(piPanel.hProcess, INFINITE);
        CloseHandle(piPanel.hThread);
        CloseHandle(piPanel.hProcess);
    }

    // 5. 停止引擎
    TerminateProcess(piEng.hProcess, 0);
    CloseHandle(piEng.hProcess);

    return 0;
}
