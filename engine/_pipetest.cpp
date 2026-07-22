// Pipe test client — send arbitrary commands to AES67 engine
// Usage: _pipetest.exe [command] [arg]
//   _pipetest.exe STATUS
//   _pipetest.exe GET_ROUTING
//   _pipetest.exe SET_ROUTING {"destinations":[...],"routes":[...]}
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    const char* cmd = "STATUS\n";
    if (argc >= 2) {
        static char buf[4096];
        if (argc >= 3) {
            snprintf(buf, sizeof(buf), "%s %s\n", argv[1], argv[2]);
        } else {
            snprintf(buf, sizeof(buf), "%s\n", argv[1]);
        }
        cmd = buf;
    }

    HANDLE h = CreateFileW(L"\\\\.\\pipe\\AES67Engine",
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("Cannot connect to pipe. Error: %lu\n", GetLastError());
        return 1;
    }

    printf(">>> %s", cmd);
    DWORD written = 0;
    WriteFile(h, cmd, (DWORD)strlen(cmd), &written, nullptr);

    // Read up to 4KB response (routing tables can be large)
    char buf[4096] = {};
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    buf[read] = '\0';
    printf("<<< %s", buf);

    CloseHandle(h);
    return 0;
}
