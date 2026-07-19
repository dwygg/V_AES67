#pragma once
#include <windows.h>
#include <string>
#include <map>

// Named pipe client for AES67 engine IPC.
// Sends text commands, receives text responses.
class PipeClient {
public:
    PipeClient();
    ~PipeClient();

    // Send a command and get the response.
    // Returns empty string on failure.
    std::string SendCommand(const std::string& cmd);

    // Parse STATUS response into key=value map
    static std::map<std::string, std::string> ParseStatus(const std::string& response);

    bool IsConnected() const;

private:
    static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\AES67Engine";
    static constexpr DWORD kTimeout = 2000;
};
