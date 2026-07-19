#include "audio_thread.h"
#include "wasapi_device.h"
#include "ring_buffer.h"
#include "logger.h"
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

AudioThread::AudioThread() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
}

AudioThread::~AudioThread() {
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool AudioThread::Start(WasapiClient* client, AudioThreadStats* stats,
                        RingBuffer* ringBuffer, UINT16 blockAlign) {
    if (m_running.load(std::memory_order_acquire)) return false;
    if (!client || !stats) return false;

    m_client      = client;
    m_stats       = stats;
    m_ringBuffer  = ringBuffer;
    m_blockAlign  = blockAlign;
    m_running.store(true, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
    ResetEvent(m_stopEvent);

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        m_running.store(false, std::memory_order_release);
        Logger::Instance().Error("CreateThread failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void AudioThread::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    SetEvent(m_stopEvent);  // Wake thread from WaitForMultipleObjects

    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);  // 5s timeout
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

void AudioThread::Pause() {
    m_paused.store(true, std::memory_order_release);
}

void AudioThread::Resume() {
    m_paused.store(false, std::memory_order_release);
}

DWORD WINAPI AudioThread::ThreadProc(LPVOID param) {
    auto* self = static_cast<AudioThread*>(param);
    self->RunLoop();
    return 0;
}

void AudioThread::RunLoop() {
    // 1. Register MMCSS Pro Audio
    DWORD taskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!hMmcss) {
        Logger::Instance().Warn("AvSetMmThreadCharacteristics failed: %lu — continuing without MMCSS",
            GetLastError());
    } else {
        Logger::Instance().Info("MMCSS Pro Audio registered (task index %lu)", taskIndex);
    }

    // 2. Set up event handle (if event-driven)
    HANDLE hAudioEvent = nullptr;
    if (m_client->IsEventDriven()) {
        hAudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
        if (FAILED(m_client->SetEventHandle(hAudioEvent))) {
            Logger::Instance().Warn("SetEventHandle failed — falling back to polling");
            CloseHandle(hAudioEvent);
            hAudioEvent = nullptr;
        }
    }

    // 3. Start audio client
    HRESULT hr = m_client->Start();
    if (FAILED(hr)) {
        Logger::Instance().Error("Audio client Start() failed: 0x%08X", hr);
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (hAudioEvent) CloseHandle(hAudioEvent);
        return;
    }

    // 4. Main capture loop
    IAudioCaptureClient* capture = m_client->GetCapture();
    HANDLE waitHandles[2] = { hAudioEvent, m_stopEvent };
    DWORD waitCount = hAudioEvent ? 2 : 1;

    while (m_running.load(std::memory_order_acquire)) {
        // Wait for audio event or stop signal
        if (hAudioEvent) {
            DWORD result = WaitForMultipleObjects(waitCount, waitHandles, FALSE, 5);  // 5ms timeout safety
            if (result == WAIT_OBJECT_0 + 1) break;        // stop event
            if (result == WAIT_TIMEOUT) continue;           // spur, re-check
            if (result != WAIT_OBJECT_0) continue;          // error
        } else {
            // Polling fallback
            DWORD result = WaitForSingleObject(m_stopEvent, 5);
            if (result == WAIT_OBJECT_0) break;             // stop event
        }

        // Skip processing while paused
        if (m_paused.load(std::memory_order_acquire)) {
            // Drain pending packets silently
            UINT32 pkts = 0;
            if (SUCCEEDED(capture->GetNextPacketSize(&pkts))) {
                for (UINT32 p = 0; p < pkts; p++) {
                    BYTE* data; UINT32 frames; DWORD flags;
                    if (SUCCEEDED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                        capture->ReleaseBuffer(frames);
                    }
                }
            }
            continue;
        }

        // Drain all available packets
        UINT32 packets = 0;
        hr = capture->GetNextPacketSize(&packets);
        if (FAILED(hr)) {
            m_stats->glitchCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        for (UINT32 p = 0; p < packets; p++) {
            BYTE* data;
            UINT32 frames;
            DWORD flags;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                m_stats->totalFrames.fetch_add(frames, std::memory_order_relaxed);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    m_stats->nonSilentFrames.fetch_add(frames, std::memory_order_relaxed);
                }
                // M5: Forward to AES67 transmit ring buffer (before ReleaseBuffer!)
                if (m_ringBuffer && frames > 0) {
                    size_t bytes = (size_t)frames * m_blockAlign;
                    size_t written = m_ringBuffer->Write(data, bytes);
                    if (written < bytes) {
                        m_stats->bufferOverflows.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                capture->ReleaseBuffer(frames);
            } else {
                m_stats->glitchCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_stats->periodCount.fetch_add(1, std::memory_order_relaxed);
    }

    // 5. Cleanup
    m_client->Stop();
    m_client->Reset();
    if (hAudioEvent) CloseHandle(hAudioEvent);
    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
}
