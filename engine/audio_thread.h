#pragma once
#include <windows.h>
#include <atomic>

class WasapiClient;
class RingBuffer;

// Cross-thread statistics — audio thread writes, main thread reads.
// All fields atomic, no lock needed for single-writer (audio thread) / single-reader (main thread).
struct AudioThreadStats {
    std::atomic<DWORD> totalFrames       = 0;   // Total frames captured
    std::atomic<DWORD> nonSilentFrames   = 0;   // Frames with actual audio data
    std::atomic<DWORD> glitchCount       = 0;   // Buffer errors
    std::atomic<DWORD> periodCount       = 0;   // Number of capture periods processed
    std::atomic<bool>  wasGlitch         = false;
    std::atomic<DWORD> bufferOverflows   = 0;   // Ring buffer write shortfalls (M5)

    void Reset() {
        totalFrames.store(0, std::memory_order_relaxed);
        nonSilentFrames.store(0, std::memory_order_relaxed);
        glitchCount.store(0, std::memory_order_relaxed);
        periodCount.store(0, std::memory_order_relaxed);
        wasGlitch.store(false, std::memory_order_relaxed);
        bufferOverflows.store(0, std::memory_order_relaxed);
    }
};

// MMCSS audio capture thread — event-driven or polling.
// All heavy allocation happens before Start(). Inner loop is zero-allocation.
class AudioThread {
public:
    AudioThread();
    ~AudioThread();

    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    // Start the thread. client and stats must outlive the thread.
    // ringBuffer + blockAlign are optional (for M5 transmit); nullptr = capture-only.
    bool Start(WasapiClient* client, AudioThreadStats* stats,
               RingBuffer* ringBuffer = nullptr, UINT16 blockAlign = 0);

    // Signal stop and wait for thread exit (blocking).
    void Stop();

    void Pause();
    void Resume();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    bool IsPaused()  const { return m_paused.load(std::memory_order_acquire); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunLoop();

    HANDLE            m_thread    = nullptr;
    HANDLE            m_stopEvent = nullptr;
    WasapiClient*     m_client    = nullptr;  // non-owning
    AudioThreadStats* m_stats     = nullptr;  // non-owning
    RingBuffer*       m_ringBuffer = nullptr;  // non-owning (M5 transmit)
    UINT16            m_blockAlign = 0;        // bytes per frame (M5 transmit)

    std::atomic<bool> m_running   = false;
    std::atomic<bool> m_paused    = false;
};
