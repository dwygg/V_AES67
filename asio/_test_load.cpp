// Quick ASIO DLL loader test — verifies LoadLibrary + exports
#include <windows.h>
#include <stdio.h>

typedef long (*ASIOInitFn)(void*);
typedef long (*ASIOGetChannelsFn)(long*, long*);

int main() {
    HMODULE h = LoadLibraryW(L"e:\\jmdev\\AES67\\asio\\AES67_ASIO.dll");
    if (!h) {
        DWORD err = GetLastError();
        printf("LoadLibrary FAILED: 0x%08X\n", err);

        // Common error codes:
        // 0xC0000135 = STATUS_DLL_NOT_FOUND (missing dep)
        // 0xC000007B = STATUS_INVALID_IMAGE_FORMAT (32/64 mismatch)
        // 0x0000007E = ERROR_MOD_NOT_FOUND (missing dep, alternate)
        // 0x0000007F = ERROR_PROC_NOT_FOUND
        // 0x000000C1 = ERROR_BAD_EXE_FORMAT (32/64 mismatch)

        wchar_t buf[2048];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, 0, err, 0, buf, 2048, 0);
        wprintf(L"  Reason: %s\n", buf);

        // Also try loading with LoadLibraryEx to get more info
        SetLastError(0);
        HMODULE h2 = LoadLibraryExW(L"e:\\jmdev\\AES67\\asio\\AES67_ASIO.dll", 0, DONT_RESOLVE_DLL_REFERENCES);
        if (h2) {
            printf("  DLL loads OK (dep resolution bypassed → missing dependency!)\n");
            FreeLibrary(h2);
        } else {
            printf("  DLL fails even without dep resolution → file is corrupt or inaccessible\n");
        }

        return 1;
    }
    printf("LoadLibrary OK\n");

    // Check exports
    auto* pInit = (ASIOInitFn)GetProcAddress(h, "ASIOInit");
    auto* pChannels = (ASIOGetChannelsFn)GetProcAddress(h, "ASIOGetChannels");
    auto* pExit = (ASIOInitFn)GetProcAddress(h, "ASIOExit");

    printf("ASIOInit:       %s\n", pInit ? "OK" : "MISSING!");
    printf("ASIOGetChannels: %s\n", pChannels ? "OK" : "MISSING!");
    printf("ASIOExit:       %s\n", pExit ? "OK" : "MISSING!");

    if (pInit) {
        struct { long v; long dv; char n[32]; char e[124]; void* sys; } info = {};
        long ret = pInit(&info);
        printf("ASIOInit returned: %ld\n", ret);
        printf("  Name: %s\n", info.n);
        if (ret != 0) printf("  Error: %s\n", info.e);
    }

    if (pChannels) {
        long in = 0, out = 0;
        pChannels(&in, &out);
        printf("Channels: %ld in / %ld out\n", in, out);
    }

    FreeLibrary(h);
    return 0;
}
