#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdint>

class JitterBuffer;

// UDP/RTP receiver thread. Binds to a multicast group, receives RTP packets,
// parses headers, and inserts payload into JitterBuffer.
class NetworkReceiver {
public:
    NetworkReceiver();
    ~NetworkReceiver();

    NetworkReceiver(const NetworkReceiver&) = delete;
    NetworkReceiver& operator=(const NetworkReceiver&) = delete;

    bool Start(JitterBuffer* jitter, const char* mcastAddr, uint16_t port);
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    uint64_t GetPacketsRcvd() const { return m_packetsRcvd.load(std::memory_order_relaxed); }
    uint64_t GetParseErrors() const { return m_parseErrors.load(std::memory_order_relaxed); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunLoop();
    bool InitSocket(const char* mcastAddr, uint16_t port);

    JitterBuffer* m_jitter = nullptr;

    HANDLE  m_thread    = nullptr;
    HANDLE  m_stopEvent = nullptr;
    SOCKET  m_socket    = INVALID_SOCKET;

    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_packetsRcvd{0};
    std::atomic<uint64_t> m_parseErrors{0};
};
