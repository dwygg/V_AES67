#include "ptp_thread.h"
#include "logger.h"
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

// PTP message types
static constexpr uint8_t kMsgSync       = 0x0;
static constexpr uint8_t kMsgDelayReq   = 0x1;
static constexpr uint8_t kMsgFollowUp   = 0x8;
static constexpr uint8_t kMsgDelayResp  = 0x9;

// PTP multicast
static constexpr char    kPtpMcastAddr[]  = "224.0.1.129";
static constexpr uint16_t kPtpEventPort   = 319;
static constexpr uint16_t kPtpGeneralPort = 320;

// PTP common header size
static constexpr int kPtpHeaderSize = 34;

PtpThread::PtpThread() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Cache QPC frequency
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = freq.QuadPart;
    m_qpcToNs = 1000000000.0 / (double)m_qpcFreq;
}

PtpThread::~PtpThread() {
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool PtpThread::InitSockets() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // ---- Event socket (port 319) ----
    m_sockEvent = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sockEvent == INVALID_SOCKET) {
        Logger::Instance().Error("PTP event socket failed: %d", WSAGetLastError());
        return false;
    }
    int reuse = 1;
    setsockopt(m_sockEvent, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPtpEventPort);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sockEvent, (const sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        Logger::Instance().Error("PTP bind(319) failed: %d", WSAGetLastError());
        return false;
    }

    ip_mreq mreq = {};
    inet_pton(AF_INET, kPtpMcastAddr, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(m_sockEvent, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    // ---- General socket (port 320) ----
    m_sockGeneral = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    local.sin_port = htons(kPtpGeneralPort);
    bind(m_sockGeneral, (const sockaddr*)&local, sizeof(local));
    setsockopt(m_sockGeneral, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    Logger::Instance().Info("PTP sockets bound: %s (319/320)", kPtpMcastAddr);
    return true;
}

bool PtpThread::Start(PtpClock* clock) {
    if (m_running.load(std::memory_order_acquire)) return false;
    if (!clock) return false;

    m_clock = clock;

    if (!InitSockets()) {
        return false;
    }

    ResetEvent(m_stopEvent);
    m_running.store(true, std::memory_order_release);
    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        m_running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void PtpThread::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    SetEvent(m_stopEvent);

    if (m_sockEvent != INVALID_SOCKET) { closesocket(m_sockEvent); m_sockEvent = INVALID_SOCKET; }
    if (m_sockGeneral != INVALID_SOCKET) { closesocket(m_sockGeneral); m_sockGeneral = INVALID_SOCKET; }

    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    WSACleanup();
}

// Parse a 10-byte PTP timestamp (6B seconds + 4B nanoseconds) from buffer
static PtpTimestamp ParseTimestamp(const BYTE* buf) {
    uint64_t sec = PtpThread::ReadU48(buf);
    uint32_t nsec = PtpThread::ReadU32(buf + 6);
    return PtpTimestamp::FromSeconds(sec, nsec);
}

// Write a 10-byte PTP timestamp to buffer
static void WriteTimestamp(BYTE* buf, const PtpTimestamp& ts) {
    PtpThread::WriteU48(buf, ts.Seconds());
    PtpThread::WriteU32(buf + 6, ts.Nanoseconds());
}

// Extract message type from PTP header byte 0
static uint8_t GetMsgType(const BYTE* buf) {
    return buf[0] & 0x0F;
}

// Extract sequence ID from PTP header bytes 30-31
static uint16_t GetSequenceId(const BYTE* buf) {
    return PtpThread::ReadU16(buf + 30);
}

// Extract twoStep flag from PTP header bytes 6-7
// twoStep = flag bit 9 (counting from bit 0), i.e., (flags >> 8) & 1
// Actually: flag bit 8 in the 16-bit flag field. (flags & 0x0200)
static bool IsTwoStep(const BYTE* buf) {
    uint16_t flags = PtpThread::ReadU16(buf + 6);
    return (flags & 0x0200) != 0;
}

bool PtpThread::RecvSync(PtpTimestamp& t1, int64_t& t2_qpc, uint16_t& syncSeq) {
    BYTE buf[1500];
    sockaddr_in from;
    int fromLen = sizeof(from);
    int n = recvfrom(m_sockEvent, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n == SOCKET_ERROR) return false;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    t2_qpc = qpc.QuadPart;

    if (n < kPtpHeaderSize || GetMsgType(buf) != kMsgSync) return false;

    syncSeq = GetSequenceId(buf);

    if (IsTwoStep(buf)) {
        t1.ns = 0;  // t1 is in Follow_Up
    } else {
        if (n >= kPtpHeaderSize + 10) {
            t1 = ParseTimestamp(buf + kPtpHeaderSize);
        }
    }
    return true;
}

bool PtpThread::RecvFollowUp(PtpTimestamp& t1, uint16_t syncSeq) {
    BYTE buf[1500];
    sockaddr_in from;
    int fromLen = sizeof(from);
    int n = recvfrom(m_sockGeneral, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n == SOCKET_ERROR) return false;

    if (n < kPtpHeaderSize + 10) return false;
    if (GetMsgType(buf) != kMsgFollowUp) return false;
    if (GetSequenceId(buf) != syncSeq) return false;

    t1 = ParseTimestamp(buf + kPtpHeaderSize);
    return true;
}

bool PtpThread::SendDelayReq(uint16_t seq, int64_t& t3_qpc) {
    BYTE buf[64] = {};
    int totalLen = kPtpHeaderSize + 10;

    // Build PTP common header
    buf[0] = (0 << 4) | kMsgDelayReq;  // transportSpecific=0, messageType=Delay_Req(1)
    buf[1] = 0x02;                      // versionPTP=2, minorVersion=0 (reserved=0, version=2)
    PtpThread::WriteU16(buf + 2, (uint16_t)totalLen);
    buf[4] = 0;    // domainNumber
    // buf[5] = minorSdoId = 0
    // buf[6-7] flags = 0 (no twoStep, no unicast)
    // buf[8-15] correctionField = 0
    // buf[20-27] clockIdentity = 0 (we don't claim an identity)
    PtpThread::WriteU16(buf + 30, seq);
    buf[32] = kMsgDelayReq;  // controlField (deprecated, matches messageType)
    buf[33] = 0x7F;          // logMessageInterval = 127 (0x7F)

    // Use current clock estimate for t3
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    t3_qpc = qpc.QuadPart;
    PtpTimestamp t3 = m_clock->QpcToPtp(t3_qpc);
    WriteTimestamp(buf + kPtpHeaderSize, t3);

    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kPtpEventPort);
    inet_pton(AF_INET, kPtpMcastAddr, &dest.sin_addr);

    int sent = sendto(m_sockEvent, (const char*)buf, totalLen, 0,
        (const sockaddr*)&dest, sizeof(dest));
    return sent != SOCKET_ERROR;
}

bool PtpThread::RecvDelayResp(PtpTimestamp& t4, uint16_t delaySeq) {
    BYTE buf[1500];
    sockaddr_in from;
    int fromLen = sizeof(from);
    int n = recvfrom(m_sockGeneral, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n == SOCKET_ERROR) return false;

    if (n < kPtpHeaderSize + 20) return false;
    if (GetMsgType(buf) != kMsgDelayResp) return false;
    if (GetSequenceId(buf) != delaySeq) return false;

    t4 = ParseTimestamp(buf + kPtpHeaderSize);
    return true;
}

DWORD WINAPI PtpThread::ThreadProc(LPVOID param) {
    auto* self = static_cast<PtpThread*>(param);
    self->RunLoop();
    return 0;
}

void PtpThread::RunLoop() {
    uint16_t delaySeq = 0;
    Logger::Instance().Info("PTP thread started, listening on %s:319/320", kPtpMcastAddr);

    while (m_running.load(std::memory_order_acquire)) {
        // Step 1: Wait for Sync (with 2s timeout)
        PtpTimestamp t1;
        int64_t t2_qpc = 0;
        uint16_t syncSeq = 0;
        if (!RecvSync(t1, t2_qpc, syncSeq)) {
            if (WaitForSingleObject(m_stopEvent, 2000) == WAIT_OBJECT_0) break;
            continue;
        }

        // Step 2: If two-step, wait for Follow_Up
        if (t1.ns == 0) {
            if (!RecvFollowUp(t1, syncSeq)) continue;
        }

        // Step 3: Send Delay_Req
        int64_t t3_qpc = 0;
        if (!SendDelayReq(++delaySeq, t3_qpc)) continue;

        // Step 4: Wait for Delay_Resp
        PtpTimestamp t4;
        if (!RecvDelayResp(t4, delaySeq)) continue;

        // Step 5: Update clock model
        m_clock->UpdateSync(t1, t2_qpc);
        m_clock->UpdateDelay(t4, t3_qpc);

        // Log periodically (every 16 syncs ≈ 16s)
        static int logCounter = 0;
        if (++logCounter % 16 == 0) {
            const char* stateStr = "FREE_RUN";
            switch (m_clock->GetState()) {
                case PtpState::TRACKING: stateStr = "TRACKING"; break;
                case PtpState::LOCKED:   stateStr = "LOCKED"; break;
                case PtpState::HOLDOVER: stateStr = "HOLDOVER"; break;
                default: break;
            }
            Logger::Instance().Info("PTP state=%s offset=%.0fns delay=%.0fus drift=%.1fppb",
                stateStr, m_clock->GetOffsetNs(),
                m_clock->GetMeanPathDelay() / 1000.0,
                m_clock->GetDriftPpb());
        }
    }

    Logger::Instance().Info("PTP thread stopped.");
}
