#include "pipe_client.h"
#include <cstring>
#include <sstream>

PipeClient::PipeClient() = default;
PipeClient::~PipeClient() = default;

std::string PipeClient::SendCommand(const std::string& cmd) {
    // P2 fix: retry on ERROR_PIPE_BUSY. The server serves one instance at a time
    // and there is a brief window between CreateNamedPipe calls where no instance
    // is available; without a retry a click could "do nothing". Try up to ~1s.
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        h = CreateFileW(kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;

        lastErr = GetLastError();
        if (lastErr == ERROR_PIPE_BUSY) {
            // Wait for an instance to become available, then retry.
            WaitNamedPipeW(kPipeName, 100);
            continue;
        }
        if (lastErr == ERROR_ACCESS_DENIED) {
            // Transient race: the server instance exists but is mid-teardown /
            // not yet in a pending-accept state. A short backoff clears it.
            Sleep(50);
            continue;
        }
        if (lastErr == ERROR_FILE_NOT_FOUND) {
            // Server not up yet (or between instances) — brief backoff + retry.
            Sleep(50);
            continue;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "ERR CreateFile: %lu", lastErr);
        return buf;  // other errors: report for display
    }
    if (h == INVALID_HANDLE_VALUE) {
        // Gave up after all retries. Do NOT swallow the reason — surface the last
        // error code so we can see whether it's 2 (not found), 5 (access denied),
        // 231 (all instances busy), etc. instead of a blank "Command failed:".
        char buf[64];
        snprintf(buf, sizeof(buf), "ERR connect: %lu (retries exhausted)", lastErr);
        return buf;
    }

    // Ensure message-mode read so a single ReadFile returns the whole reply.
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    std::string fullCmd = cmd + "\n";
    DWORD written = 0;
    WriteFile(h, fullCmd.c_str(), (DWORD)fullCmd.size(), &written, nullptr);

    char buf[4096] = {};
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);

    if (read == 0) return "";
    buf[read] = '\0';

    std::string result(buf);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

bool PipeClient::IsConnected() const {
    HANDLE h = CreateFileW(kPipeName,
        GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return true;
    }
    return false;
}

std::map<std::string, std::string> PipeClient::ParseStatus(const std::string& response) {
    std::map<std::string, std::string> result;
    // Format: "STATUS key1=val1 key2=val2 ..."
    std::istringstream ss(response);
    std::string token;

    // Skip the leading "STATUS " prefix
    ss >> token;  // "STATUS"

    while (ss >> token) {
        auto eq = token.find('=');
        if (eq != std::string::npos) {
            result[token.substr(0, eq)] = token.substr(eq + 1);
        }
    }
    return result;
}
