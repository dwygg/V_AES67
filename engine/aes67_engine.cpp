#include "aes67_engine.h"
#include "logger.h"

Aes67Engine::Aes67Engine() : m_com(COINIT_MULTITHREADED) {
    if (!m_com.Ok()) {
        Logger::Instance().Error("CoInitializeEx failed: 0x%08X", m_com.Hr());
    }
}

Aes67Engine::~Aes67Engine() {
    Shutdown();
}

bool Aes67Engine::Initialize(const AudioConfig& config, const NetworkConfig& netConfig) {
    if (!m_com.Ok()) {
        Logger::Instance().Error("COM not initialized");
        return false;
    }

    m_config = config;
    m_config.Init();
    m_netConfig = netConfig;
    m_stopRequested.store(false, std::memory_order_release);

    // Reset ring buffer for fresh session
    m_ringBuffer.Reset();

    // 1. Create MMDeviceEnumerator
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)m_enumerator.GetAddressOf());
    if (FAILED(hr)) {
        Logger::Instance().Error("CoCreateInstance(MMDeviceEnumerator) failed: 0x%08X", hr);
        return false;
    }

    // 2. Find + init devices per mode
    bool hasTx = m_netConfig.enableTx;
    bool hasRx = m_netConfig.enableRx;

    if (hasTx) {
        m_device = FindAudioDevice(m_enumerator.Get(), eRender, kTargetDeviceName);
        if (!m_device) {
            Logger::Instance().Error("AES67Driver render endpoint not found (TX).");
            return false;
        }
        hr = m_client.InitLoopback(m_device.Get(), m_config);
        if (FAILED(hr)) {
            Logger::Instance().Error("TX InitLoopback failed: 0x%08X", hr);
            return false;
        }
    }

    if (hasRx) {
        // RX renders to a NON-AES67Driver render endpoint (physical speakers/headphones).
        // We search all render devices and pick the first one that is NOT AES67Driver.
        IMMDeviceCollection* coll = nullptr;
        hr = m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
        if (SUCCEEDED(hr) && coll) {
            UINT count = 0; coll->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED(coll->Item(i, &dev))) {
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                        PROPVARIANT var; PropVariantInit(&var);
                        props->GetValue(PKEY_Device_FriendlyName, &var);
                        bool isAes67 = var.pwszVal && wcsstr(var.pwszVal, kTargetDeviceName);
                        if (!isAes67) {
                            Logger::Instance().Info("RX output: %ls",
                                var.pwszVal ? var.pwszVal : L"(unknown)");
                            PropVariantClear(&var); props->Release();
                            m_deviceRx.Reset(dev);
                            break;
                        }
                        PropVariantClear(&var); props->Release();
                        dev->Release();
                    } else {
                        dev->Release();
                    }
                }
            }
            coll->Release();
        }
        if (!m_deviceRx) {
            Logger::Instance().Error("No non-AES67Driver render device found for RX output.");
            return false;
        }
        if (!m_audioRenderThread.Initialize(m_deviceRx.Get(), m_config)) {
            Logger::Instance().Error("RX render init failed");
            return false;
        }
        m_jitterBuffer.Reset();
    }

    // M9: Set up IPC command handler
    m_pipeServer.SetHandler([this](const std::string& cmd, const std::string& arg) -> std::string {
        if (cmd == "STATUS") {
            char buf[512];
            const char* stateStr = "stopped";
            switch (m_state) {
                case EngineState::Running: stateStr = "running"; break;
                case EngineState::Paused:  stateStr = "paused"; break;
                default: break;
            }
            DWORD total = m_stats.totalFrames.load(std::memory_order_relaxed);
            snprintf(buf, sizeof(buf),
                "STATUS state=%s fps=%lu tx=%llu rx=%llu jb=%zu ptp=%d ptp_off=%.0fns",
                stateStr, total,
                (unsigned long long)m_networkThread.GetPacketsSent(),
                (unsigned long long)m_networkReceiver.GetPacketsRcvd(),
                m_jitterBuffer.AvailableRead(),
                (int)m_ptpClock.GetState(),
                m_ptpClock.GetOffsetNs());
            return buf;
        }
        // NOTE: this handler runs on the pipe thread.
        // START/PAUSE/RESUME don't join the pipe thread, so they're safe to call here.
        // STOP is special: Stop() used to join the pipe thread -> self-deadlock (M9-1).
        // We now defer STOP to the engine's own loop via an atomic flag.
        if (cmd == "START") { Start(); return "OK"; }
        if (cmd == "STOP")  { SignalStopAudio(); return "OK"; }
        if (cmd == "PAUSE") { Pause(); return "OK"; }
        if (cmd == "RESUME"){ Resume();return "OK"; }
        // M9-3/M9-4 (P2): SET updates m_netConfig, then flags a reconfig so the
        // engine thread rebuilds the sockets (changing the value alone never
        // re-bound the socket -> address changes silently didn't take effect).
        // Supported keys: dest / port (TX dest addr+port),
        //                 source / sourceport (RX source addr+port).
        if (cmd == "SET" && arg.find("dest ") == 0) {
            m_netConfig.destAddr = arg.substr(5);
            m_reconfigRequested.store(true, std::memory_order_release);
            return m_state == EngineState::Running ? "OK reconfiguring" : "OK";
        }
        if (cmd == "SET" && arg.find("sourceport ") == 0) {
            m_netConfig.sourcePort = (uint16_t)atoi(arg.substr(11).c_str());
            m_reconfigRequested.store(true, std::memory_order_release);
            return m_state == EngineState::Running ? "OK reconfiguring" : "OK";
        }
        if (cmd == "SET" && arg.find("source ") == 0) {
            m_netConfig.sourceAddr = arg.substr(7);
            m_reconfigRequested.store(true, std::memory_order_release);
            return m_state == EngineState::Running ? "OK reconfiguring" : "OK";
        }
        if (cmd == "SET" && arg.find("port ") == 0) {
            m_netConfig.destPort = (uint16_t)atoi(arg.substr(5).c_str());
            m_reconfigRequested.store(true, std::memory_order_release);
            return m_state == EngineState::Running ? "OK reconfiguring" : "OK";
        }
        if (cmd == "EXIT") { SignalStop(); return "OK"; }
        return "ERR unknown command";
    });

    // M9-2 (P2): pipe listens as soon as the engine is initialized, BEFORE audio
    // Start(). "Engine process alive == pipe listening" — so the panel can connect
    // and query STATUS / remotely START even when audio hasn't started yet.
    // Pipe is torn down only in Shutdown() (not in Stop()), avoiding the previous
    // egg-and-chicken problem (pipe bound to audio Start).
    m_pipeServer.Start();

    m_state = EngineState::Initialized;
    Logger::Instance().Info("Engine initialized: %u Hz, %u-bit, %u ch [%s%s]",
        m_config.sampleRate, m_config.bitsPerSample, m_config.channels,
        hasTx ? "TX" : "", hasRx ? " RX" : "");
    return true;
}

bool Aes67Engine::Start() {
    if (m_state != EngineState::Initialized && m_state != EngineState::Stopped) {
        Logger::Instance().Error("Cannot start: invalid state (%d)", (int)m_state);
        return false;
    }

    m_stats.Reset();

    // Start audio thread (with optional ring buffer for M5 transmit)
    RingBuffer* rb = m_netConfig.enableTx ? &m_ringBuffer : nullptr;
    if (!m_thread.Start(&m_client, &m_stats, rb, m_config.blockAlign)) {
        Logger::Instance().Error("Failed to start audio thread");
        return false;
    }

    // M9-2 (P2): pipe server is started in Initialize() and stopped in Shutdown(),
    // decoupled from audio Start/Stop. Do NOT start/stop it here.

    // M7: Start PTP clock synchronization
    if (m_netConfig.enablePtp) {
        if (!m_ptpThread.Start(&m_ptpClock)) {
            Logger::Instance().Warn("PTP thread start failed — continuing without clock sync");
        }
    }

    // M5: Start network transmit
    if (m_netConfig.enableTx) {
        if (!m_networkThread.Start(&m_ringBuffer, m_config,
                                   m_netConfig.destAddr.c_str(),
                                   m_netConfig.destPort)) {
            Logger::Instance().Error("Failed to start network thread");
            m_thread.Stop();
            return false;
        }

        // Start SAP announcer (non-fatal if fails)
        if (!m_sapAnnouncer.Start(m_networkThread.GetSSRC(),
                                  m_netConfig.destPort, m_config)) {
            Logger::Instance().Warn("SAP announcer start failed — stream works but won't be auto-discovered");
        }
    }

    // M6: Start receiver + render
    if (m_netConfig.enableRx) {
        if (!m_networkReceiver.Start(&m_jitterBuffer,
                                     m_netConfig.sourceAddr.c_str(),
                                     m_netConfig.sourcePort)) {
            Logger::Instance().Error("Failed to start network receiver");
            // Rollback TX
            if (m_netConfig.enableTx) {
                m_sapAnnouncer.Stop();
                m_networkThread.Stop();
            }
            m_thread.Stop();
            return false;
        }

        if (!m_audioRenderThread.Start(&m_jitterBuffer, &m_renderStats)) {
            Logger::Instance().Error("Failed to start audio render thread");
            m_networkReceiver.Stop();
            if (m_netConfig.enableTx) {
                m_sapAnnouncer.Stop();
                m_networkThread.Stop();
            }
            m_thread.Stop();
            return false;
        }
    }

    m_state = EngineState::Running;
    const char* mode = "capture only";
    if (m_netConfig.enableTx && m_netConfig.enableRx) mode = "TX + RX (full duplex)";
    else if (m_netConfig.enableTx) mode = "capturing + transmitting";
    else if (m_netConfig.enableRx) mode = "receiving + rendering";
    Logger::Instance().Info("Engine started — %s", mode);
    return true;
}

void Aes67Engine::Stop() {
    if (m_state != EngineState::Running && m_state != EngineState::Paused) return;
    // M9-1/M9-2 (P2): DO NOT stop the pipe server here.
    //  - Stop() may be called from the pipe thread (via the deferred STOP flag);
    //    m_pipeServer.Stop() joins that very thread -> self-deadlock.
    //  - Even off the pipe thread, we want the pipe to stay alive after STOP so the
    //    panel can re-START. Pipe is torn down only in Shutdown().
    // Reverse dependency order
    m_ptpThread.Stop();
    m_sapAnnouncer.Stop();
    m_networkThread.Stop();
    m_thread.Stop();
    m_ringBuffer.Reset();
    m_networkReceiver.Stop();
    m_audioRenderThread.Stop();
    m_jitterBuffer.Reset();
    m_state = EngineState::Stopped;
    Logger::Instance().Info("Engine stopped");
}

// M9-3 (P2): apply new dest/source addr+port by rebuilding the network threads.
// Runs on the engine thread only (called from RunBlocking loop). Changing
// m_netConfig values alone never re-bound the UDP sockets, so address changes
// used to silently not take effect; here we actually stop and restart the
// affected network threads with the current config.
void Aes67Engine::ApplyNetworkReconfig() {
    if (m_state != EngineState::Running) return;  // only meaningful while streaming

    if (m_netConfig.enableTx) {
        m_sapAnnouncer.Stop();
        m_networkThread.Stop();
        if (!m_networkThread.Start(&m_ringBuffer, m_config,
                                   m_netConfig.destAddr.c_str(),
                                   m_netConfig.destPort)) {
            Logger::Instance().Error("Reconfig: TX network restart failed");
        } else if (!m_sapAnnouncer.Start(m_networkThread.GetSSRC(),
                                         m_netConfig.destPort, m_config)) {
            Logger::Instance().Warn("Reconfig: SAP announcer restart failed");
        }
        Logger::Instance().Info("Reconfig: TX now -> %s:%u",
            m_netConfig.destAddr.c_str(), m_netConfig.destPort);
    }

    if (m_netConfig.enableRx) {
        m_networkReceiver.Stop();
        m_jitterBuffer.Reset();
        if (!m_networkReceiver.Start(&m_jitterBuffer,
                                     m_netConfig.sourceAddr.c_str(),
                                     m_netConfig.sourcePort)) {
            Logger::Instance().Error("Reconfig: RX receiver restart failed");
        }
        Logger::Instance().Info("Reconfig: RX now <- %s:%u",
            m_netConfig.sourceAddr.c_str(), m_netConfig.sourcePort);
    }
}

void Aes67Engine::Pause() {
    if (m_state != EngineState::Running) return;
    m_thread.Pause();
    m_state = EngineState::Paused;
    Logger::Instance().Info("Engine paused");
}

void Aes67Engine::Resume() {
    if (m_state != EngineState::Paused) return;
    m_thread.Resume();
    m_state = EngineState::Running;
    Logger::Instance().Info("Engine resumed");
}

void Aes67Engine::Shutdown() {
    if (m_state == EngineState::Uninitialized) return;
    if (m_state == EngineState::Running || m_state == EngineState::Paused) {
        Stop();
    }
    // M9-2 (P2): pipe server is torn down here (not in Stop()). Safe: Shutdown() is
    // called from the engine/main thread, never from the pipe thread.
    m_pipeServer.Stop();
    // WasapiClient dtor calls Stop/Reset
    // ComPtr dtor releases interfaces
    // ComInitializer dtor calls CoUninitialize
    m_device.Reset();
    m_deviceRx.Reset();
    m_enumerator.Reset();
    m_state = EngineState::Uninitialized;
    Logger::Instance().Info("Engine shutdown complete");
}

void Aes67Engine::RunBlocking(const AudioConfig& config, AudioThreadStats& outStats,
                             const NetworkConfig& netConfig) {
    if (!Initialize(config, netConfig)) {
        Logger::Instance().Error("Engine initialization failed");
        return;
    }

    // P2: panel-hosted mode. With autoStart=false the engine stays Initialized
    // (pipe already listening from Initialize) and waits for the panel to send
    // START. Legacy CLI (autoStart=true, the default) starts audio immediately.
    if (config.autoStart) {
        if (!Start()) {
            Logger::Instance().Error("Engine start failed");
            return;
        }
    } else {
        Logger::Instance().Info("Managed mode: engine initialized, waiting for START from panel (pipe listening)");
    }

    DWORD tickStart = GetTickCount();
    DWORD tickLastLog = tickStart;
    DWORD lastTotalFrames = 0;

    Logger::Instance().Info("Running... (duration=%us, %s, press Ctrl+C to stop)",
        config.durationSec,
        netConfig.enableTx ? netConfig.destAddr.c_str() : "capture only");

    // M9 (P2): loop while the engine process is alive (until EXIT / Ctrl+C).
    // Audio STOP no longer ends the process — it just stops streaming; the pipe
    // keeps listening so the panel can re-START. Loop condition is the process
    // lifetime flag, not the audio Running state.
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        Sleep(kEngineStatusIntervalMs);

        if (m_stopRequested.load(std::memory_order_acquire)) {
            Logger::Instance().Info("Exit requested");
            break;
        }

        // M9-1 (P2): deferred STOP from the pipe thread — executed here on the
        // engine thread, so Stop() (which joins nothing on this thread) is safe.
        if (m_stopAudioRequested.exchange(false, std::memory_order_acq_rel)) {
            Logger::Instance().Info("Audio stop requested (pipe) — stopping stream, keeping process/pipe alive");
            Stop();
        }

        // M9-3 (P2): apply SET address/port changes by rebuilding sockets here.
        if (m_reconfigRequested.exchange(false, std::memory_order_acq_rel)) {
            ApplyNetworkReconfig();
        }

        // While stopped/paused, just idle and keep serving the pipe.
        if (m_state != EngineState::Running) {
            continue;
        }

        DWORD now = GetTickCount();
        DWORD elapsed = (now > tickStart) ? (now - tickStart) / 1000 : 0;

        // Check duration (only meaningful while actively running, and only in
        // legacy auto-start CLI mode; in panel-hosted mode the engine is a
        // long-lived process controlled by START/STOP, so duration is ignored).
        if (config.autoStart && config.durationSec > 0 && elapsed >= config.durationSec) {
            Logger::Instance().Info("Duration reached (%us)", config.durationSec);
            break;
        }

        // Log stats
        DWORD total = m_stats.totalFrames.load(std::memory_order_relaxed);
        DWORD active = m_stats.nonSilentFrames.load(std::memory_order_relaxed);
        DWORD glitches = m_stats.glitchCount.load(std::memory_order_relaxed);
        DWORD periods = m_stats.periodCount.load(std::memory_order_relaxed);
        DWORD overflows = m_stats.bufferOverflows.load(std::memory_order_relaxed);

        DWORD dt = (now > tickLastLog) ? (now - tickLastLog) / 1000 : 1;
        DWORD fps = (total > lastTotalFrames) ? (total - lastTotalFrames) / dt : 0;

        auto pktsTx = m_networkThread.GetPacketsSent();
        auto pktsRx = m_networkReceiver.GetPacketsRcvd();
        auto jbAvail = m_jitterBuffer.AvailableRead();
        DWORD rendFrames = m_renderStats.totalFrames.load(std::memory_order_relaxed);

        const char* ptpState = "N/A";
        double ptpOff = 0;
        if (m_netConfig.enablePtp) {
            ptpOff = m_ptpClock.GetOffsetNs();
            switch (m_ptpClock.GetState()) {
                case PtpState::FREE_RUN:  ptpState = "FREE"; break;
                case PtpState::TRACKING:  ptpState = "TRACK"; break;
                case PtpState::LOCKED:    ptpState = "LOCK"; break;
                case PtpState::HOLDOVER:  ptpState = "HOLD"; break;
            }
        }
        Logger::Instance().Info("[%3us] cap=%lu fps=%lu gl=%lu | tx=%llu ovf=%lu | rx=%llu jb=%zu rend=%lu | ptp=%s %.0fns",
            elapsed, total, fps, glitches,
            pktsTx, overflows, pktsRx, jbAvail, rendFrames, ptpState, ptpOff);

        lastTotalFrames = total;
        tickLastLog = now;
    }

    Stop();
    // Copy atomics one by one
    outStats.totalFrames.store(m_stats.totalFrames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    outStats.nonSilentFrames.store(m_stats.nonSilentFrames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    outStats.glitchCount.store(m_stats.glitchCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    outStats.periodCount.store(m_stats.periodCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    outStats.wasGlitch.store(m_stats.wasGlitch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    outStats.bufferOverflows.store(m_stats.bufferOverflows.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
