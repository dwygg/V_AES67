// Quick named pipe test — connects to AES67 engine, sends STATUS, prints response
#include <windows.h>
#include <stdio.h>

int main() {
    HANDLE h = CreateFileW(L"\\\\.\\pipe\\AES67Engine",
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("Cannot connect to pipe. Is aes67_engine.exe running?\n");
        printf("Error: %lu\n", GetLastError());
        return 1;
    }
    printf("Connected. Sending STATUS...\n");

    const char* cmd = "STATUS\n";
    DWORD written = 0;
    WriteFile(h, cmd, (DWORD)strlen(cmd), &written, nullptr);

    char buf[512] = {};
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    buf[read] = '\0';
    printf("Response: %s", buf);

    CloseHandle(h);
    return 0;
}
