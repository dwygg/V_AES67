#include "audio_render_thread.h"
#include "jitter_buffer.h"
#include "logger.h"
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

AudioRenderThread::AudioRenderThread() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AudioRenderThread::~AudioRenderThread() {
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool AudioRenderThread::Initialize(IMMDevice* captureDevice, const AudioConfig& config) {
    m_config = config;
    m_config.Init();

    HRESULT hr = m_client.InitRender(captureDevice, m_config);
    if (FAILED(hr)) {
        Logger::Instance().Error("RX InitRender failed: 0x%08X", hr);
        return false;
    }
    m_initialized = true;

    Logger::Instance().Info("RX render init: %u ch %u Hz %u-bit, period=%u frames, %s",
        m_config.channels, m_config.sampleRate, m_config.bitsPerSample,
        m_client.GetPeriodFrames(),
        m_client.IsEventDriven() ? "event-driven" : "polling");
    return true;
}

bool AudioRenderThread::Start(JitterBuffer* jitter, RenderStats* stats) {
    if (m_running.load(std::memory_order_acquire)) return false;
    if (!m_initialized || !jitter || !stats) return false;

    m_jitter = jitter;
    m_stats  = stats;
    m_stats->Reset();

    ResetEvent(m_stopEvent);
    m_running.store(true, std::memory_order_release);

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        m_running.store(false, std::memory_order_release);
        Logger::Instance().Error("CreateThread(AudioRender) failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void AudioRenderThread::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    SetEvent(m_stopEvent);

    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

DWORD WINAPI AudioRenderThread::ThreadProc(LPVOID param) {
    auto* self = static_cast<AudioRenderThread*>(param);
    self->RunLoop();
    return 0;
}

void AudioRenderThread::RunLoop() {
    // 1. MMCSS Pro Audio
    DWORD taskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!hMmcss) {
        Logger::Instance().Warn("RX AvSetMmThreadCharacteristics failed: %lu", GetLastError());
    }

    // 2. Event-driven setup
    HANDLE hAudioEvent = nullptr;
    if (m_client.IsEventDriven()) {
        hAudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_client.SetEventHandle(hAudioEvent);
    }

    // 3. Warm-up: wait for jitter buffer to fill
    Logger::Instance().Info("RX render waiting for jitter buffer warm-up (%zu packets)...",
        JitterBuffer::kTargetDepth);
    while (m_running.load(std::memory_order_acquire)) {
        if (m_jitter->HasEnough()) break;
        if (WaitForSingleObject(m_stopEvent, 10) == WAIT_OBJECT_0) break;
    }
    Logger::Instance().Info("RX render warm-up complete (%zu packets ready)",
        m_jitter->AvailableRead());

    // 4. Start WASAPI
    HRESULT hr = m_client.Start();
    if (FAILED(hr)) {
        Logger::Instance().Error("RX render Start() failed: 0x%08X", hr);
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (hAudioEvent) CloseHandle(hAudioEvent);
        return;
    }

    IAudioRenderClient* render = m_client.GetRender();
    UINT32 periodFrames = m_client.GetPeriodFrames();
    UINT16 blockAlign = m_config.blockAlign;
    size_t periodBytes = (size_t)periodFrames * blockAlign;
    size_t packetsPerPeriod = periodFrames / 48;  // 480 frames / 48 = 10 packets

    HANDLE waitHandles[2] = { hAudioEvent, m_stopEvent };
    DWORD waitCount = hAudioEvent ? 2 : 1;

    // 5. Render loop
    while (m_running.load(std::memory_order_acquire)) {
        if (hAudioEvent) {
            DWORD result = WaitForMultipleObjects(waitCount, waitHandles, FALSE, 5);
            if (result == WAIT_OBJECT_0 + 1) break;
            if (result != WAIT_OBJECT_0) continue;
        } else {
            if (WaitForSingleObject(m_stopEvent, 5) == WAIT_OBJECT_0) break;
        }

        // Get render buffer
        UINT32 padding;
        m_client.GetClient()->GetCurrentPadding(&padding);
        UINT32 avail = periodFrames - padding;
        if (avail == 0) continue;

        BYTE* pData;
        hr = render->GetBuffer(avail, &pData);
        if (FAILED(hr)) {
            m_stats->glitchCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Fill from jitter buffer
        size_t bytesNeeded = (size_t)avail * blockAlign;
        size_t bytesWritten = 0;

        // Read in 288-byte (1ms) chunks from jitter buffer
        while (bytesWritten + JitterBuffer::kPayloadSize <= bytesNeeded) {
            m_jitter->Read(pData + bytesWritten);
            bytesWritten += JitterBuffer::kPayloadSize;
        }

        // Pad any remainder with silence
        if (bytesWritten < bytesNeeded) {
            memset(pData + bytesWritten, 0, bytesNeeded - bytesWritten);
        }

        hr = render->ReleaseBuffer(avail, 0);
        if (SUCCEEDED(hr)) {
            m_stats->totalFrames.fetch_add(avail, std::memory_order_relaxed);
        } else {
            m_stats->glitchCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 6. Cleanup
    m_client.Stop();
    m_client.Reset();
    if (hAudioEvent) CloseHandle(hAudioEvent);
    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);

    Logger::Instance().Info("RX render thread stopped. Total frames: %lu",
        m_stats->totalFrames.load(std::memory_order_relaxed));
}
