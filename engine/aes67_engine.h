#pragma once
#include <winsock2.h>   // MUST precede windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <functional>
#include "audio_config.h"
#include "wasapi_device.h"
#include "audio_thread.h"
#include "ring_buffer.h"
#include "network_thread.h"
#include "sap_announcer.h"
#include "jitter_buffer.h"
#include "network_receiver.h"
#include "audio_render_thread.h"
#include "ptp_clock.h"
#include "ptp_thread.h"
#include "pipe_server.h"

// ---- Network defaults ----
constexpr char     kDefaultMulticastAddr[] = "239.69.1.128";
constexpr uint16_t kDefaultRtpPort         = 5004;
constexpr uint16_t kDefaultSapPort         = 9875;
constexpr char     kDefaultSapMcastAddr[]  = "239.255.255.255";
constexpr size_t   kRingBufferCapacity     = 65536;  // 64KB (~227ms @ 48kHz/2ch/L24)

struct NetworkConfig {
    std::string destAddr   = kDefaultMulticastAddr;
    uint16_t    destPort   = kDefaultRtpPort;
    bool        enableTx   = true;   // M5: transmit RTP
    bool        enableRx   = false;  // M6: receive RTP
    bool        enablePtp  = true;   // M7: PTPv2 clock sync
    std::string sourceAddr = kDefaultMulticastAddr;  // RX multicast group
    uint16_t    sourcePort = kDefaultRtpPort;        // RX port
};

enum class EngineState {
    Uninitialized,
    Initialized,
    Running,
    Paused,
    Stopped
};

// AES67 Audio Engine — lifecycle state machine.
// M4: WASAPI loopback capture. M5: RTP/UDP transmit + SAP. M6: RTP receive + render.
class Aes67Engine {
public:
    Aes67Engine();
    ~Aes67Engine();

    Aes67Engine(const Aes67Engine&) = delete;
    Aes67Engine& operator=(const Aes67Engine&) = delete;

    bool Initialize(const AudioConfig& config,
                    const NetworkConfig& netConfig = NetworkConfig());
    bool Start();
    void Stop();
    void Pause();
    void Resume();
    void Shutdown();

    EngineState GetState() const { return m_state; }
    const AudioThreadStats& GetStats() const { return m_stats; }

    // M5 TX stats
    uint64_t GetPacketsSent() const { return m_networkThread.GetPacketsSent(); }
    uint64_t GetBytesDropped() const { return m_networkThread.GetBytesDropped(); }
    // M6 RX stats
    uint64_t GetPacketsRcvd() const { return m_networkReceiver.GetPacketsRcvd(); }
    uint64_t GetParseErrors() const { return m_networkReceiver.GetParseErrors(); }
    const RenderStats& GetRenderStats() const { return m_renderStats; }
    size_t JitterAvailable() const { return m_jitterBuffer.AvailableRead(); }
    // M7 PTP stats
    const PtpClock& GetPtpClock() const { return m_ptpClock; }

    void RunBlocking(const AudioConfig& config, AudioThreadStats& outStats,
                     const NetworkConfig& netConfig = NetworkConfig());
    void SignalStop() { m_stopRequested.store(true, std::memory_order_release); }

private:
    EngineState              m_state = EngineState::Uninitialized;
    AudioConfig              m_config;
    NetworkConfig            m_netConfig;
    ComInitializer           m_com;
    ComPtr<IMMDeviceEnumerator> m_enumerator;
    ComPtr<IMMDevice>           m_device;         // TX: render endpoint for loopback
    ComPtr<IMMDevice>           m_deviceRx;       // RX: capture endpoint for render
    WasapiClient             m_client;            // TX: loopback capture client
    WasapiClient             m_clientRx;          // RX: render client
    AudioThread              m_thread;
    AudioThreadStats         m_stats;
    std::atomic<bool>        m_stopRequested = false;

    // M5 transmit
    RingBuffer     m_ringBuffer{kRingBufferCapacity};
    NetworkThread  m_networkThread;
    SapAnnouncer   m_sapAnnouncer;

    // M6 receive
    JitterBuffer       m_jitterBuffer;
    NetworkReceiver    m_networkReceiver;
    AudioRenderThread  m_audioRenderThread;
    RenderStats        m_renderStats;

    // M7 PTP clock synchronization
    PtpClock           m_ptpClock;
    PtpThread          m_ptpThread;

    // M9 IPC
    PipeServer         m_pipeServer;
};
