#pragma once
#include <winsock2.h>   // MUST precede windows.h (included by audio_config.h)
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <atomic>
#include <cstdint>
#include "audio_config.h"

// RFC 2974 SAP (Session Announcement Protocol) periodic announcer.
// Sends SDP description to multicast every 30 seconds for AES67 device discovery.
class SapAnnouncer {
public:
    SapAnnouncer();
    ~SapAnnouncer();

    SapAnnouncer(const SapAnnouncer&) = delete;
    SapAnnouncer& operator=(const SapAnnouncer&) = delete;

    bool Start(uint32_t ssrc, uint16_t rtpPort, const AudioConfig& config,
               const char* mcastAddr = "239.255.255.255",
               uint16_t sapPort = 9875,
               const char* streamName = "AES67 Virtual Soundcard TX");
    void Stop();

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunLoop();
    bool InitSocket(uint16_t sapPort);
    std::string BuildSdp() const;
    void SendAnnouncement();

    HANDLE  m_thread    = nullptr;
    HANDLE  m_stopEvent = nullptr;
    SOCKET  m_socket    = INVALID_SOCKET;
    sockaddr_in m_sapDest = {};

    std::atomic<bool> m_running{false};

    uint32_t     m_ssrc       = 0;
    uint16_t     m_rtpPort    = 5004;
    AudioConfig  m_config;
    std::string  m_streamName;
    std::string  m_mcastAddr;  // RTP multicast group

    // Pre-built SDP body
    std::string m_sdp;
};
