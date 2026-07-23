#include "pipe_client.h"
#include <cstring>
#include <sstream>

PipeClient::PipeClient() = default;
PipeClient::~PipeClient() = default;

// Connect to the named pipe with a bounded retry loop.
// Returns INVALID_HANDLE_VALUE on failure; *outErr gets the last OS error.
static HANDLE ConnectWithRetry(DWORD timeoutMs, DWORD* outErr) {
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD deadline = GetTickCount() + timeoutMs;

    for (;;) {
        h = CreateFileW(PipeClient::kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;

        DWORD err = GetLastError();
        *outErr = err;

        // Only retry on transient states; give up on hard errors.
        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(PipeClient::kPipeName, 100)) continue;
        } else if (err == ERROR_ACCESS_DENIED || err == ERROR_FILE_NOT_FOUND) {
            Sleep(50);
        } else {
            return INVALID_HANDLE_VALUE;  // hard error
        }

        if (GetTickCount() >= deadline) return INVALID_HANDLE_VALUE;
    }
}

// Overlapped read with timeout. Returns true on success, false on timeout/error.
// On success *bytesRead holds the count; caller must ensure buf has room.
static bool ReadWithTimeout(HANDLE h, void* buf, DWORD len, DWORD* bytesRead,
                            DWORD timeoutMs) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    BOOL ok = ReadFile(h, buf, len, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return false;
    }
    if (ok) {
        // Completed synchronously (unlikely on a pipe, but handle it).
        GetOverlappedResult(h, &ov, bytesRead, FALSE);
        CloseHandle(ov.hEvent);
        return true;
    }

    // Wait for completion or timeout.
    DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
    if (wait == WAIT_OBJECT_0) {
        ok = GetOverlappedResult(h, &ov, bytesRead, FALSE);
        CloseHandle(ov.hEvent);
        return ok != FALSE;
    }

    // Timeout — cancel the pending I/O.
    CancelIoEx(h, &ov);
    WaitForSingleObject(ov.hEvent, INFINITE);  // drain the cancelled completion
    CloseHandle(ov.hEvent);
    return false;
}

std::string PipeClient::SendCommand(const std::string& cmd, DWORD timeoutMs) {
    if (timeoutMs == 0) timeoutMs = kDefaultTimeout;

    DWORD err = 0;
    HANDLE h = ConnectWithRetry(timeoutMs, &err);
    if (h == INVALID_HANDLE_VALUE) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ERR connect: %lu", err);
        return buf;
    }

    // Message-mode read so a single ReadFile returns the whole reply.
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    std::string fullCmd = cmd + "\n";
    DWORD written = 0;
    if (!WriteFile(h, fullCmd.c_str(), (DWORD)fullCmd.size(), &written, nullptr)) {
        DWORD we = GetLastError();
        CloseHandle(h);
        char buf[64];
        snprintf(buf, sizeof(buf), "ERR write: %lu", we);
        return buf;
    }

    DWORD remaining = timeoutMs;
    char buf[8192] = {};  // match server-side arg buffer size
    DWORD read = 0;
    if (!ReadWithTimeout(h, buf, sizeof(buf) - 1, &read, remaining)) {
        CloseHandle(h);
        return "";  // timeout → empty, caller shows "Disconnected"
    }
    CloseHandle(h);

    if (read == 0) return "";
    buf[read] = '\0';

    std::string result(buf);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

std::string PipeClient::PollStatus(DWORD timeoutMs) {
    // Fast path: check connectivity first so we don't waste time in the retry loop.
    if (!IsConnected()) return "";
    return SendCommand("STATUS", timeoutMs);
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
