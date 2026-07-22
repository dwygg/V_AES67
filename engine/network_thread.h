#pragma once
#include <winsock2.h>   // MUST precede windows.h (included by audio_config.h)
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include "audio_config.h"

class RingBuffer;
class MixingBus;

// RTP packetizer + high-precision UDP multicast sender.
// Reads 288 bytes (48 frames × 2ch × 3 bytes L24) from the ring buffer,
// wraps in RTP header (12 bytes), and sends via sendto().
//
// P5: when a MixingBus is provided, Process() is called on m_ringBuffer first,
// then packets are read from the MixingBus's per-stream output buffers.
class NetworkThread {
public:
    NetworkThread();
    ~NetworkThread();

    NetworkThread(const NetworkThread&) = delete;
    NetworkThread& operator=(const NetworkThread&) = delete;

    // Start RTP transmission. ringBuffer / config must outlive the thread.
    // mixingBus (optional, P5): if provided, data flows through MixingBus::Process()
    // before being read from per-stream output buffers.
    bool Start(RingBuffer* ringBuffer, const AudioConfig& config,
               const char* destAddr, uint16_t destPort,
               MixingBus* mixingBus = nullptr, int streamIndex = 0);
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    uint64_t GetPacketsSent() const { return m_packetsSent.load(std::memory_order_relaxed); }
    uint64_t GetBytesDropped() const { return m_bytesDropped.load(std::memory_order_relaxed); }
    uint32_t GetSSRC() const { return m_ssrc; }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunLoop();
    bool InitSocket(const char* destAddr, uint16_t destPort);

    // Build 12-byte RTP header (big-endian) in m_packetBuf.
    void BuildRtpHeader();

    RingBuffer*     m_ringBuffer  = nullptr;
    MixingBus*      m_mixingBus   = nullptr;  // P5: optional mixing bus
    int             m_streamIndex = 0;        // P5: which output stream to send
    AudioConfig     m_config;

    HANDLE          m_thread      = nullptr;
    HANDLE          m_stopEvent   = nullptr;
    HANDLE          m_timer       = nullptr;   // 1ms high-res waitable timer
    SOCKET          m_socket      = INVALID_SOCKET;
    sockaddr_in     m_destAddr    = {};

    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_packetsSent{0};
    std::atomic<uint64_t> m_bytesDropped{0};

    // RTP session state
    uint16_t m_seqNum    = 0;
    uint32_t m_timestamp = 0;
    uint32_t m_ssrc      = 0;
    UINT16   m_periodFrames = 0;  // 48 frames @ 48kHz / 1ms
    size_t   m_payloadSize  = 0;  // frames × blockAlign

    // Pre-allocated: no heap in send path
    BYTE m_payloadBuf[1152];   // max: 192 frames × 8ch × 3 bytes = 4608, safe min for 48×2×3=288
    BYTE m_packetBuf[1500];    // 12 RTP header + max payload (fits Ethernet MTU)
};
