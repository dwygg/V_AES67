#include "pipe_client.h"
#include <cstring>
#include <sstream>

PipeClient::PipeClient() = default;
PipeClient::~PipeClient() = default;

std::string PipeClient::SendCommand(const std::string& cmd) {
    HANDLE h = CreateFileW(kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        char buf[64];
        snprintf(buf, sizeof(buf), "ERR CreateFile: %lu", err);
        return buf;  // return error for display
    }

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
