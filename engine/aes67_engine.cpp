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

    if (!Start()) {
        Logger::Instance().Error("Engine start failed");
        return;
    }

    DWORD tickStart = GetTickCount();
    DWORD tickLastLog = tickStart;
    DWORD lastTotalFrames = 0;

    Logger::Instance().Info("Running... (duration=%us, %s, press Ctrl+C to stop)",
        config.durationSec,
        netConfig.enableTx ? netConfig.destAddr.c_str() : "capture only");

    while (m_state == EngineState::Running) {
        Sleep(kEngineStatusIntervalMs);

        if (m_stopRequested.load(std::memory_order_acquire)) {
            Logger::Instance().Info("Stop requested");
            break;
        }

        DWORD now = GetTickCount();
        DWORD elapsed = (now > tickStart) ? (now - tickStart) / 1000 : 0;

        // Check duration
        if (config.durationSec > 0 && elapsed >= config.durationSec) {
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
