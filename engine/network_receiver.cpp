#include "network_receiver.h"
#include "jitter_buffer.h"
#include "logger.h"

#pragma comment(lib, "ws2_32.lib")

NetworkReceiver::NetworkReceiver() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

NetworkReceiver::~NetworkReceiver() {
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool NetworkReceiver::InitSocket(const char* mcastAddr, uint16_t port) {
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        Logger::Instance().Error("RX socket() failed: %d", WSAGetLastError());
        return false;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // Bind to RTP port
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_socket, (const sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        Logger::Instance().Error("RX bind(:%u) failed: %d", port, WSAGetLastError());
        return false;
    }

    // Join multicast group
    ip_mreq mreq = {};
    inet_pton(AF_INET, mcastAddr, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char*)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
        Logger::Instance().Error("RX IP_ADD_MEMBERSHIP(%s) failed: %d",
            mcastAddr, WSAGetLastError());
        return false;
    }

    Logger::Instance().Info("RX socket bound: %s:%u", mcastAddr, port);
    return true;
}

bool NetworkReceiver::Start(JitterBuffer* jitter, const char* mcastAddr, uint16_t port) {
    if (m_running.load(std::memory_order_acquire)) return false;
    if (!jitter || !mcastAddr) return false;

    m_jitter = jitter;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!InitSocket(mcastAddr, port)) {
        WSACleanup();
        return false;
    }

    m_packetsRcvd.store(0, std::memory_order_relaxed);
    m_parseErrors.store(0, std::memory_order_relaxed);
    ResetEvent(m_stopEvent);
    m_running.store(true, std::memory_order_release);

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        m_running.store(false, std::memory_order_release);
        Logger::Instance().Error("CreateThread(NetworkReceiver) failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void NetworkReceiver::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    SetEvent(m_stopEvent);

    // Close socket to unblock recvfrom
    if (m_socket != INVALID_SOCKET) { closesocket(m_socket); m_socket = INVALID_SOCKET; }

    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    WSACleanup();
}

DWORD WINAPI NetworkReceiver::ThreadProc(LPVOID param) {
    auto* self = static_cast<NetworkReceiver*>(param);
    self->RunLoop();
    return 0;
}

void NetworkReceiver::RunLoop() {
    BYTE buf[1500];  // max Ethernet MTU
    sockaddr_in from;
    int fromLen = sizeof(from);

    Logger::Instance().Info("RX thread started, waiting for RTP packets...");

    while (m_running.load(std::memory_order_acquire)) {
        int n = recvfrom(m_socket, (char*)buf, sizeof(buf), 0,
            (sockaddr*)&from, &fromLen);
        if (n == SOCKET_ERROR) {
            if (!m_running.load(std::memory_order_acquire)) break;  // stopped
            continue;
        }

        // Parse RTP header (minimum 12 bytes)
        if (n < 12) {
            m_parseErrors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        uint8_t  version = (buf[0] >> 6) & 0x03;
        uint8_t  pt      = buf[1] & 0x7F;
        uint16_t seq     = ((uint16_t)buf[2] << 8) | buf[3];
        // timestamp: buf[4..7], SSRC: buf[8..11] — not used for basic receive

        if (version != 2 || pt != 97) {
            m_parseErrors.fetch_add(1, std::memory_order_relaxed);
            continue;  // not our stream
        }

        // Payload starts at byte 12
        int payloadLen = n - 12;
        if (payloadLen < (int)JitterBuffer::kPayloadSize) {
            m_parseErrors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        m_jitter->Insert(seq, buf + 12);
        m_packetsRcvd.fetch_add(1, std::memory_order_relaxed);
    }

    Logger::Instance().Info("RX thread stopped. Received %llu packets, %llu parse errors.",
        m_packetsRcvd.load(std::memory_order_relaxed),
        m_parseErrors.load(std::memory_order_relaxed));
}
