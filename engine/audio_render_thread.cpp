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
    // 1. MMCSS disabled on render thread: two MMCSS Pro Audio threads
    //    (capture + render) cause an access violation on some systems.
    //    The capture thread already runs under MMCSS Pro Audio for low-latency;
    //    the render thread is driven by WASAPI events and doesn't need it.
    HANDLE hMmcss = nullptr;

    // 2. Event-driven setup
    HANDLE hAudioEvent = nullptr;
    if (m_client.IsEventDriven()) {
        hAudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(m_client.SetEventHandle(hAudioEvent))) {
            Logger::Instance().Error("RX SetEventHandle failed");
        }
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
        if (hAudioEvent) CloseHandle(hAudioEvent);
        return;
    }

    IAudioRenderClient* render = m_client.GetRender();
    // P5 fix: use actual WASAPI buffer size (e.g. 1056 frames) instead of config
    // period (48 frames).  Using the config value filled only 48/1056 = 4.5% of
    // the buffer per period, causing crackle / silence.
    UINT32 bufferFrames = m_client.GetBufferFrames();
    // Mix format blockAlign (float32 = 8) vs our L24 config blockAlign (6)
    UINT16 mixBlockAlign = m_client.GetWaveFormat().Format.nBlockAlign;
    UINT16 configBlockAlign = m_config.blockAlign;
    bool renderFloat = (m_client.GetWaveFormat().SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    static constexpr UINT32 kFramesPerJitterPacket = 48;  // 48 frames @ 1ms AES67 ptime

    // Allocate int24→float32 conversion buffer (worst case: full buffer)
    BYTE* convBuf = nullptr;
    size_t convBufSize = 0;
    if (renderFloat) {
        convBufSize = (size_t)bufferFrames * m_config.channels * sizeof(float);
        convBuf = new BYTE[convBufSize];
    }

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

        // Get render buffer — use actual WASAPI buffer size, not config period
        UINT32 padding;
        m_client.GetClient()->GetCurrentPadding(&padding);
        UINT32 availFrames = bufferFrames - padding;
        if (availFrames == 0) continue;

        BYTE* pData;
        hr = render->GetBuffer(availFrames, &pData);
        if (FAILED(hr)) {
            m_stats->glitchCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Fill from jitter buffer: compute EXACT packet count from frame count.
        // Old code used bytesNeeded from float32 blockAlign (8) → over-read
        // (480×8÷288=13 instead of 480÷48=10).
        UINT32 fullPackets = availFrames / kFramesPerJitterPacket;
        UINT32 remainderFrames = availFrames % kFramesPerJitterPacket;

        for (UINT32 i = 0; i < fullPackets; i++) {
            m_jitter->Read(pData + (size_t)i * JitterBuffer::kPayloadSize);
        }
        size_t bytesWritten = (size_t)fullPackets * JitterBuffer::kPayloadSize;

        // Handle remainder frames: read one more packet, use partial data
        if (remainderFrames > 0) {
            BYTE temp[JitterBuffer::kPayloadSize];
            m_jitter->Read(temp);
            size_t remainderBytes = (size_t)remainderFrames * configBlockAlign;
            memcpy(pData + bytesWritten, temp, remainderBytes);
            bytesWritten += remainderBytes;
        }

        // Pad any remaining space (float32 render buffer > L24 jitter data)
        size_t floatBufSize = (size_t)availFrames * mixBlockAlign;
        if (bytesWritten < floatBufSize) {
            memset(pData + bytesWritten, 0, floatBufSize - bytesWritten);
        }

        // Shared-mode WASAPI expects float32 → convert int24→float from pData
        if (renderFloat && availFrames > 0) {
            size_t samples = (size_t)availFrames * m_config.channels;
            for (size_t s = 0; s < samples; s++) {
                int val = (pData[s * 3] & 0xFF) | ((pData[s * 3 + 1] & 0xFF) << 8)
                        | ((int8_t)pData[s * 3 + 2] << 16);
                float f = (float)val / 8388608.0f;
                memcpy(convBuf + s * sizeof(float), &f, sizeof(float));
            }
            memcpy(pData, convBuf, samples * sizeof(float));
        }

        hr = render->ReleaseBuffer(availFrames, 0);
        if (SUCCEEDED(hr)) {
            m_stats->totalFrames.fetch_add(availFrames, std::memory_order_relaxed);
        } else {
            m_stats->glitchCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 6. Cleanup
    m_client.Stop();
    m_client.Reset();
    if (hAudioEvent) CloseHandle(hAudioEvent);
    if (convBuf) delete[] convBuf;

    Logger::Instance().Info("RX render thread stopped. Total frames: %lu",
        m_stats->totalFrames.load(std::memory_order_relaxed));
}
