#include "pipe_server.h"
#include "logger.h"
#include <cstring>

static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\AES67Engine";
static constexpr DWORD kTimeout = 1000;  // 1s pipe wait timeout

PipeServer::PipeServer() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeServer::~PipeServer() {
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool PipeServer::Start() {
    if (m_running.load(std::memory_order_acquire)) return false;
    if (!m_handler) return false;  // must set handler first

    ResetEvent(m_stopEvent);
    m_running.store(true, std::memory_order_release);

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        m_running.store(false, std::memory_order_release);
        Logger::Instance().Error("PipeServer CreateThread failed: %lu", GetLastError());
        return false;
    }
    Logger::Instance().Info("Pipe server listening on %ls", kPipeName);
    return true;
}

void PipeServer::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    SetEvent(m_stopEvent);

    // Connect a dummy client to unblock ConnectNamedPipe
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    if (m_thread) {
        WaitForSingleObject(m_thread, 3000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

DWORD WINAPI PipeServer::ThreadProc(LPVOID param) {
    auto* self = static_cast<PipeServer*>(param);
    self->RunLoop();
    return 0;
}

void PipeServer::RunLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        HANDLE hPipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, kTimeout, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Logger::Instance().Error("CreateNamedPipe failed: %lu", GetLastError());
            break;
        }

        // Wait for client or stop
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(hPipe, &ov);

        if (!connected && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ov.hEvent, m_stopEvent };
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0) {
                CloseHandle(ov.hEvent);
                CloseHandle(hPipe);
                break;
            }
        }
        CloseHandle(ov.hEvent);

        HandleClient(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

void PipeServer::HandleClient(HANDLE hPipe) {
    char buf[4096] = {};
    DWORD bytesRead = 0;

    if (!ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) || bytesRead == 0) return;
    buf[bytesRead] = '\0';

    // Parse command and optional argument
    char cmd[64] = {}, arg[256] = {};
    sscanf_s(buf, "%63s %255[^\r\n]", cmd, (unsigned)_countof(cmd), arg, (unsigned)_countof(arg));

    std::string response = m_handler(cmd, arg);
    response += '\n';

    DWORD written = 0;
    WriteFile(hPipe, response.c_str(), (DWORD)response.size(), &written, nullptr);
}
