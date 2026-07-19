#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <atomic>

// Named pipe IPC server — text line protocol.
// Pipe: \\.\pipe\AES67Engine
// Commands end with \n, responses end with \n.

class PipeServer {
public:
    using CommandHandler = std::function<std::string(const std::string& cmd, const std::string& arg)>;

    PipeServer();
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Set the handler before Start(). Called from pipe thread — keep it fast.
    void SetHandler(CommandHandler handler) { m_handler = std::move(handler); }

    bool Start();
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunLoop();
    void HandleClient(HANDLE hPipe);

    CommandHandler m_handler;
    HANDLE  m_thread = nullptr;
    HANDLE  m_stopEvent = nullptr;
    std::atomic<bool> m_running{false};
};
