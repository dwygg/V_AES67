#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include "ptp_clock.h"

// PTPv2 slave thread (IEEE 1588-2008, E2E Delay Mechanism).
// Receives Sync/Follow_Up from master, sends Delay_Req, processes Delay_Resp.
// Software-timestamps using QueryPerformanceCounter at recvfrom/sendto.
class PtpThread {
public:
    PtpThread();
    ~PtpThread();

    PtpThread(const PtpThread&) = delete;
    PtpThread& operator=(const PtpThread&) = delete;

    bool Start(PtpClock* clock);
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    PtpState GetState() const { return m_clock ? m_clock->GetState() : PtpState::FREE_RUN; }
    double   GetOffsetNs() const { return m_clock ? m_clock->GetOffsetNs() : 0.0; }
    double   GetMeanPathDelay() const { return m_clock ? m_clock->GetMeanPathDelay() : 0.0; }

    // Big-endian byte helpers
    static uint16_t ReadU16(const BYTE* buf) { return ((uint16_t)buf[0] << 8) | buf[1]; }
    static uint32_t ReadU32(const BYTE* buf) { return ((uint32_t)ReadU16(buf) << 16) | ReadU16(buf + 2); }
    static uint64_t ReadU48(const BYTE* buf) {
        return ((uint64_t)ReadU16(buf) << 32) | ((uint64_t)ReadU32(buf + 2));
    }
    static void WriteU16(BYTE* buf, uint16_t v) { buf[0] = (v >> 8) & 0xFF; buf[1] = v & 0xFF; }
    static void WriteU32(BYTE* buf, uint32_t v) { WriteU16(buf, (uint16_t)(v >> 16)); WriteU16(buf + 2, (uint16_t)v); }
    static void WriteU48(BYTE* buf, uint64_t v) { WriteU16(buf, (uint16_t)(v >> 32)); WriteU32(buf + 2, (uint32_t)v); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunLoop();

    bool InitSockets();
    bool RecvSync(PtpTimestamp& t1, int64_t& t2_qpc, uint16_t& syncSeq);
    bool RecvFollowUp(PtpTimestamp& t1, uint16_t syncSeq);
    bool SendDelayReq(uint16_t seq, int64_t& t3_qpc);
    bool RecvDelayResp(PtpTimestamp& t4, uint16_t delaySeq);

    PtpClock* m_clock = nullptr;
    HANDLE    m_thread = nullptr;
    HANDLE    m_stopEvent = nullptr;

    SOCKET    m_sockEvent   = INVALID_SOCKET;   // port 319 (Sync, Delay_Req)
    SOCKET    m_sockGeneral = INVALID_SOCKET;   // port 320 (Follow_Up, Delay_Resp)

    std::atomic<bool> m_running{false};

    // QPC frequency
    int64_t   m_qpcFreq = 1;
    double    m_qpcToNs = 1.0;  // multiplier: QPC ticks → nanoseconds
};
