#include "network_thread.h"
#include "ring_buffer.h"
#include "mixing_bus.h"
#include "logger.h"
#include <ctime>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

NetworkThread::NetworkThread() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

NetworkThread::~NetworkThread() {
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool NetworkThread::InitSocket(const char* destAddr, uint16_t destPort) {
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        Logger::Instance().Error("socket() failed: %d", WSAGetLastError());
        return false;
    }

    // Set DSCP Expedited Forwarding (46 << 2 = 0xB8)
    DWORD tos = 0xB8;
    setsockopt(m_socket, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof(tos));

    // Large send buffer to avoid blocking on NIC backpressure
    int bufsize = 256 * 1024;  // 256KB
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));

    // Multicast TTL
    DWORD ttl = 32;
    setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    m_destAddr.sin_family = AF_INET;
    m_destAddr.sin_port = htons(destPort);
    inet_pton(AF_INET, destAddr, &m_destAddr.sin_addr);

    Logger::Instance().Info("UDP socket ready: %s:%u DSCP=EF TTL=32", destAddr, destPort);
    return true;
}

bool NetworkThread::Start(RingBuffer* ringBuffer, const AudioConfig& config,
                          const char* destAddr, uint16_t destPort,
                          MixingBus* mixingBus, int streamIndex) {
    if (m_running.load(std::memory_order_acquire)) return false;
    if (!ringBuffer || !destAddr) return false;

    m_config = config;
    m_config.Init();
    m_ringBuffer = ringBuffer;
    m_mixingBus   = mixingBus;    // P5: optional mixing bus
    m_streamIndex = streamIndex;  // P5: which output stream

    // Compute RTP payload parameters
    m_periodFrames = m_config.sampleRate / 1000;   // 48 frames @ 48kHz for 1ms
    if (m_periodFrames < 1) m_periodFrames = 48;
    m_payloadSize = (size_t)m_periodFrames * m_config.blockAlign;

    Logger::Instance().Info("RTP: %zu samples/pkt, %zu bytes payload, %zu bytes total",
        (size_t)m_periodFrames, m_payloadSize, 12 + m_payloadSize);

    // Init Winsock for this thread
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!InitSocket(destAddr, destPort)) {
        WSACleanup();
        return false;
    }

    // Randomize session identifiers (RFC 3550)
    srand((unsigned)time(nullptr) ^ (unsigned)GetCurrentThreadId());
    m_seqNum    = (uint16_t)(rand() & 0xFFFF);
    m_timestamp = (uint32_t)(((uint64_t)rand() << 16) ^ rand());
    m_ssrc      = (uint32_t)(((uint64_t)rand() << 16) ^ rand());

    // High-precision timer: 1ms period
    timeBeginPeriod(1);
    m_timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!m_timer) {
        Logger::Instance().Warn("High-res timer unavailable, falling back");
        m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    ResetEvent(m_stopEvent);
    m_packetsSent.store(0, std::memory_order_relaxed);
    m_bytesDropped.store(0, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_release);

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        m_running.store(false, std::memory_order_release);
        Logger::Instance().Error("CreateThread(NetworkThread) failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void NetworkThread::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    SetEvent(m_stopEvent);

    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }

    if (m_timer) { CloseHandle(m_timer); m_timer = nullptr; }
    if (m_socket != INVALID_SOCKET) { closesocket(m_socket); m_socket = INVALID_SOCKET; }
    timeEndPeriod(1);
    WSACleanup();
}

void NetworkThread::BuildRtpHeader() {
    // RTP Fixed Header (RFC 3550, Section 5.1), 12 bytes, big-endian
    m_packetBuf[0]  = 0x80;  // V=2, P=0, X=0, CC=0
    m_packetBuf[1]  = 0x61;  // M=0, PT=97 (0x61 = 97 decimal)
    m_packetBuf[2]  = (m_seqNum >> 8) & 0xFF;
    m_packetBuf[3]  = m_seqNum & 0xFF;
    m_packetBuf[4]  = (m_timestamp >> 24) & 0xFF;
    m_packetBuf[5]  = (m_timestamp >> 16) & 0xFF;
    m_packetBuf[6]  = (m_timestamp >> 8) & 0xFF;
    m_packetBuf[7]  = m_timestamp & 0xFF;
    m_packetBuf[8]  = (m_ssrc >> 24) & 0xFF;
    m_packetBuf[9]  = (m_ssrc >> 16) & 0xFF;
    m_packetBuf[10] = (m_ssrc >> 8) & 0xFF;
    m_packetBuf[11] = m_ssrc & 0xFF;
}

DWORD WINAPI NetworkThread::ThreadProc(LPVOID param) {
    auto* self = static_cast<NetworkThread*>(param);
    self->RunLoop();
    return 0;
}

void NetworkThread::RunLoop() {
    // Batch-oriented: drain ring buffer and send all complete packets.
    // Wake every ~1ms, but send as many as data allows — handles timer jitter.
    size_t totalSize = 12 + m_payloadSize;

    while (m_running.load(std::memory_order_acquire)) {
        // P5: if MixingBus is active, process raw input through it first.
        // This distributes audio to per-stream ring buffers per the routing table.
        // We then read from our assigned stream's output buffer.
        RingBuffer* readBuf = m_ringBuffer;
        if (m_mixingBus && m_mixingBus->StreamCount() > 0) {
            // Drain all available input through the mixing bus in 1ms chunks
            while (m_ringBuffer->AvailableRead() >= m_payloadSize) {
                m_mixingBus->Process(*m_ringBuffer, m_periodFrames);
            }
            readBuf = m_mixingBus->GetStreamBuffer((size_t)m_streamIndex);
            if (!readBuf) readBuf = m_ringBuffer;  // fallback to direct
        }

        // Drain ring buffer: read all available data, send complete packets
        bool sentAny = false;
        for (;;) {
            size_t available = readBuf->AvailableRead();
            if (available < m_payloadSize) break;  // not enough for a full packet

            size_t read = readBuf->Read(m_payloadBuf, m_payloadSize);
            if (read < m_payloadSize) break;

            BuildRtpHeader();
            memcpy(m_packetBuf + 12, m_payloadBuf, m_payloadSize);

            int sent = sendto(m_socket, (const char*)m_packetBuf, (int)totalSize, 0,
                (const sockaddr*)&m_destAddr, sizeof(m_destAddr));
            if (sent == SOCKET_ERROR) {
                static DWORD lastErrLog = 0;
                DWORD now = GetTickCount();
                if (now - lastErrLog > 5000) {
                    Logger::Instance().Warn("sendto failed: %d", WSAGetLastError());
                    lastErrLog = now;
                }
                break;  // stop draining on error
            }

            m_packetsSent.fetch_add(1, std::memory_order_relaxed);
            m_seqNum++;
            m_timestamp += m_periodFrames;
            sentAny = true;
        }

        // Short sleep to avoid busy-spinning, with stop event check
        if (!sentAny) {
            DWORD result = WaitForSingleObject(m_stopEvent, 1);  // 1ms wait
            if (result == WAIT_OBJECT_0) break;
        }
    }

    Logger::Instance().Info("Network thread stopped. Sent %llu packets.",
        m_packetsSent.load(std::memory_order_relaxed));
}
