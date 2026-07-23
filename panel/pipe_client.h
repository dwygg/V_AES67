#pragma once
#include <windows.h>
#include <string>
#include <map>

// Named pipe client for AES67 engine IPC.
// Sends text commands, receives text responses.
class PipeClient {
public:
    static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\AES67Engine";
    static constexpr DWORD kDefaultTimeout = 2000;

    PipeClient();
    ~PipeClient();

    // Send a command and get the response.
    // timeoutMs: max wait for the full round-trip (connect + write + read).
    //   0 = default (2000ms for commands, shorter for STATUS).
    // Returns empty string on timeout/failure, "ERR ..." on error.
    std::string SendCommand(const std::string& cmd, DWORD timeoutMs = 2000);

    // Quick fire-and-forget status check — short timeout, no retry storm.
    // Returns empty on any failure (caller handles "Disconnected" display).
    std::string PollStatus(DWORD timeoutMs = 300);

    // Parse STATUS response into key=value map
    static std::map<std::string, std::string> ParseStatus(const std::string& response);

    bool IsConnected() const;

private:
};
