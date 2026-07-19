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
    // A single reusable event for the async ConnectNamedPipe.
    HANDLE hConnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Logger::Instance().Info("Pipe RunLoop started, waiting for clients on %ls", kPipeName);

    while (m_running.load(std::memory_order_acquire)) {
        // CRITICAL: the pipe MUST be created with FILE_FLAG_OVERLAPPED, otherwise
        // ConnectNamedPipe below ignores the OVERLAPPED struct and blocks
        // synchronously — which (a) makes m_stopEvent useless and (b) leaves a
        // window between instances where a connecting client gets ACCESS_DENIED(5)
        // or PIPE_BUSY. With overlapped I/O the instance is in a proper PENDING
        // accept state the moment it exists, so clients never race.
        HANDLE hPipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, kTimeout, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            // Do NOT break the loop on a transient failure — that would leave the
            // engine alive but permanently deaf (panel stuck "Disconnected").
            // Back off briefly and retry so the server self-heals.
            Logger::Instance().Error("CreateNamedPipe failed: %lu — retrying in 200ms", GetLastError());
            if (WaitForSingleObject(m_stopEvent, 200) == WAIT_OBJECT_0) break;
            continue;
        }

        ResetEvent(hConnectEvent);
        OVERLAPPED ov = {};
        ov.hEvent = hConnectEvent;

        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        DWORD err = connected ? ERROR_SUCCESS : GetLastError();
        // Note: err=997 (ERROR_IO_PENDING) is the normal async path — not logged
        // to avoid spamming the console once per STATUS poll.

        bool ready = false;
        if (connected) {
            // Rare: connected synchronously.
            ready = true;
        } else if (err == ERROR_PIPE_CONNECTED) {
            // Client connected in the tiny gap between CreateNamedPipe and
            // ConnectNamedPipe — this IS a successful connection.
            ready = true;
        } else if (err == ERROR_IO_PENDING) {
            // Normal path: wait for a client OR a stop request.
            HANDLE waits[2] = { hConnectEvent, m_stopEvent };
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) {
                ready = true;
            } else {
                // Stop requested: cancel the pending connect and exit.
                CancelIoEx(hPipe, &ov);
                CloseHandle(hPipe);
                break;
            }
        } else {
            Logger::Instance().Error("ConnectNamedPipe failed: %lu", err);
            CloseHandle(hPipe);
            continue;
        }

        if (ready) {
            HandleClient(hPipe);
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
        }
        CloseHandle(hPipe);
    }

    if (hConnectEvent) CloseHandle(hConnectEvent);
}

// Helper: synchronous-style read/write on an OVERLAPPED pipe handle.
// Because the pipe is created with FILE_FLAG_OVERLAPPED, plain ReadFile/WriteFile
// return FALSE + ERROR_IO_PENDING; we must wait on the OVERLAPPED event and then
// GetOverlappedResult. Returns true on success and fills *transferred.
static bool OverlappedIo(HANDLE hPipe, bool isWrite, void* buf, DWORD len,
                         DWORD* transferred) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = isWrite
        ? WriteFile(hPipe, buf, len, nullptr, &ov)
        : ReadFile(hPipe, buf, len, nullptr, &ov);

    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(hPipe, &ov, transferred, TRUE);
    } else if (ok) {
        // Completed synchronously — result is already available.
        GetOverlappedResult(hPipe, &ov, transferred, FALSE);
    }
    DWORD savedErr = GetLastError();
    if (ov.hEvent) CloseHandle(ov.hEvent);
    SetLastError(savedErr);
    return ok != FALSE;
}

void PipeServer::HandleClient(HANDLE hPipe) {
    char buf[4096] = {};
    DWORD bytesRead = 0;

    if (!OverlappedIo(hPipe, /*isWrite=*/false, buf, sizeof(buf) - 1, &bytesRead)
        || bytesRead == 0) {
        return;
    }
    buf[bytesRead] = '\0';

    // Parse command and optional argument
    char cmd[64] = {}, arg[256] = {};
    sscanf_s(buf, "%63s %255[^\r\n]", cmd, (unsigned)_countof(cmd), arg, (unsigned)_countof(arg));

    // Only log non-STATUS commands: STATUS is polled every second by the panel
    // and would otherwise flood the console.
    if (_stricmp(cmd, "STATUS") != 0) {
        Logger::Instance().Info("Pipe recv: cmd='%s' arg='%s'", cmd, arg);
    }

    std::string response = m_handler(cmd, arg);
    response += '\n';

    DWORD written = 0;
    OverlappedIo(hPipe, /*isWrite=*/true,
                 const_cast<char*>(response.c_str()),
                 (DWORD)response.size(), &written);
}
