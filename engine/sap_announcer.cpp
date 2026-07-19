#include "sap_announcer.h"
#include "logger.h"
#include <cstdio>
#include <ctime>

#pragma comment(lib, "ws2_32.lib")

SapAnnouncer::SapAnnouncer() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

SapAnnouncer::~SapAnnouncer() {
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool SapAnnouncer::InitSocket(const char* mcastAddr, uint16_t sapPort) {
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        Logger::Instance().Error("SAP socket() failed: %d", WSAGetLastError());
        return false;
    }

    DWORD ttl = 32;
    setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    m_sapDest.sin_family = AF_INET;
    m_sapDest.sin_port = htons(sapPort);
    inet_pton(AF_INET, mcastAddr, &m_sapDest.sin_addr);

    Logger::Instance().Info("SAP socket ready: %s:%u", mcastAddr, sapPort);
    return true;
}

bool SapAnnouncer::Start(uint32_t ssrc, uint16_t rtpPort, const AudioConfig& config,
                         const char* mcastAddr, uint16_t sapPort,
                         const char* streamName) {
    if (m_running.load(std::memory_order_acquire)) return false;

    m_ssrc       = ssrc;
    m_rtpPort    = rtpPort;
    m_config     = config;
    m_config.Init();
    m_streamName = streamName ? streamName : "AES67 Virtual Soundcard TX";
    m_mcastAddr  = mcastAddr ? mcastAddr : "239.69.1.128";

    // Init Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!InitSocket(mcastAddr, sapPort)) {
        WSACleanup();
        return false;
    }

    // Build the SDP template once (session_id/version update each send)
    m_sdp = BuildSdp();

    ResetEvent(m_stopEvent);
    m_running.store(true, std::memory_order_release);

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        m_running.store(false, std::memory_order_release);
        Logger::Instance().Error("CreateThread(SAP) failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void SapAnnouncer::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    SetEvent(m_stopEvent);

    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }

    if (m_socket != INVALID_SOCKET) { closesocket(m_socket); m_socket = INVALID_SOCKET; }
    WSACleanup();
}

std::string SapAnnouncer::BuildSdp() const {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "v=0\r\n"
        "o=- %u 1 IN IP4 0.0.0.0\r\n"
        "s=%s\r\n"
        "c=IN IP4 %s/32\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP 97\r\n"
        "a=rtpmap:97 L24/%u/%u\r\n"
        "a=ptime:1\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-00-00-ff-fe-00-00-00:0\r\n"
        "a=mediaclk:direct=0\r\n"
        "a=framecount:48\r\n",
        m_ssrc,
        m_streamName.c_str(),
        m_mcastAddr.c_str(),
        m_rtpPort,
        m_config.sampleRate,
        m_config.channels);
    if (n < 0) return "";
    return std::string(buf);
}

void SapAnnouncer::SendAnnouncement() {
    // SAP header (8 bytes, RFC 2974)
    BYTE sapHdr[8] = {};
    sapHdr[0] = 0x20;  // V=1, A=0 (IPv4), R=0, T=0 (announce), E=0, C=0
    sapHdr[1] = 0x00;  // auth_len = 0

    // msg_id_hash: simple rolling counter
    static uint16_t s_msgId = 0;
    s_msgId++;
    sapHdr[2] = (s_msgId >> 8) & 0xFF;
    sapHdr[3] = s_msgId & 0xFF;

    // Originating source: use INADDR_ANY (0.0.0.0) since we don't know our IP
    // In production, use getsockname() on a connected socket to get local IP
    sapHdr[4] = sapHdr[5] = sapHdr[6] = sapHdr[7] = 0x00;

    // Send SAP header + SDP payload
    std::string payload = std::string((const char*)sapHdr, 8) + m_sdp;

    int sent = sendto(m_socket, payload.c_str(), (int)payload.size(), 0,
        (const sockaddr*)&m_sapDest, sizeof(m_sapDest));
    if (sent == SOCKET_ERROR) {
        Logger::Instance().Warn("SAP sendto failed: %d", WSAGetLastError());
    }
}

DWORD WINAPI SapAnnouncer::ThreadProc(LPVOID param) {
    auto* self = static_cast<SapAnnouncer*>(param);
    self->RunLoop();
    return 0;
}

void SapAnnouncer::RunLoop() {
    // Exponential backoff on startup: 0s, 1s, 2s, 4s, 8s, 16s, then every 30s
    DWORD backoff[] = {0, 1000, 2000, 4000, 8000, 16000};
    int backoffIdx = 0;

    Logger::Instance().Info("SAP announcer started: %u ch %u Hz, SSRC=0x%08X",
        m_config.channels, m_config.sampleRate, m_ssrc);

    while (m_running.load(std::memory_order_acquire)) {
        SendAnnouncement();

        DWORD waitMs;
        if (backoffIdx < 6) {
            waitMs = backoff[backoffIdx++];
        } else {
            waitMs = 30000;  // 30 seconds steady state
        }

        // Wait with stop event
        DWORD result = WaitForSingleObject(m_stopEvent, waitMs);
        if (result == WAIT_OBJECT_0) break;
    }
}
