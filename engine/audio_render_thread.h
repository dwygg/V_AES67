#pragma once
#include <windows.h>
#include <atomic>
#include "audio_config.h"
#include "wasapi_device.h"

class JitterBuffer;

// Cross-thread render statistics — render thread writes, main thread reads.
struct RenderStats {
    std::atomic<DWORD> totalFrames     = 0;  // Total frames rendered
    std::atomic<DWORD> underflowCount  = 0;  // Times jitter buffer had no data
    std::atomic<DWORD> glitchCount     = 0;  // WASAPI buffer errors

    void Reset() {
        totalFrames.store(0, std::memory_order_relaxed);
        underflowCount.store(0, std::memory_order_relaxed);
        glitchCount.store(0, std::memory_order_relaxed);
    }
};

// MMCSS audio render thread — reads from JitterBuffer, writes to WASAPI capture endpoint.
// Event-driven via SetEventHandle, same pattern as AudioThread but inverted (write vs read).
class AudioRenderThread {
public:
    AudioRenderThread();
    ~AudioRenderThread();

    AudioRenderThread(const AudioRenderThread&) = delete;
    AudioRenderThread& operator=(const AudioRenderThread&) = delete;

    // Initialize WASAPI render client on the given capture device.
    bool Initialize(IMMDevice* captureDevice, const AudioConfig& config);

    // Start the render thread. jitter and stats must outlive the thread.
    bool Start(JitterBuffer* jitter, RenderStats* stats);

    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunLoop();

    WasapiClient       m_client;
    AudioConfig        m_config;
    JitterBuffer*      m_jitter = nullptr;
    RenderStats*       m_stats  = nullptr;

    HANDLE             m_thread    = nullptr;
    HANDLE             m_stopEvent = nullptr;

    std::atomic<bool>  m_running{false};
    bool               m_initialized = false;
};
