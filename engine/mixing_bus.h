#pragma once
#include "ring_buffer.h"
#include "routing.h"
#include "audio_config.h"
#include "logger.h"
#include <vector>
#include <cstring>

// P5: 混音总线 — 按路由表将输入通道分发到目标流，应用增益/静音。
// 每路目标流有独立 RingBuffer，网络线程从中读取发送。
//
// P6: CRITICAL_SECTION protects concurrent access between the network thread
// (Process) and the engine thread (Lock/LoadFromFile/Reinitialize/Unlock).
// Process() acquires the lock internally; the engine thread must hold the
// lock across the full LoadFromFile + Reinitialize sequence.

class MixingBus {
public:
    MixingBus(const RoutingTable& routing, const AudioConfig& config)
        : m_routing(routing), m_config(config) {
        InitializeCriticalSection(&m_lock);
    }

    ~MixingBus() {
        EnterCriticalSection(&m_lock);
        for (auto* rb : m_streamBuffers) delete rb;
        m_streamBuffers.clear();
        LeaveCriticalSection(&m_lock);
        DeleteCriticalSection(&m_lock);
    }

    // 初始化：为每路目标流创建独立 RingBuffer
    bool Initialize() {
        EnterCriticalSection(&m_lock);
        for (size_t i = 0; i < m_routing.destinations.size(); i++) {
            auto* rb = new RingBuffer(kPerStreamBufSize);
            m_streamBuffers.push_back(rb);
        }
        bool ok = !m_streamBuffers.empty();
        LeaveCriticalSection(&m_lock);
        if (ok) {
            Logger::Instance().Info("MixingBus: %zu streams ready", m_streamBuffers.size());
        }
        return ok;
    }

    // P6 fix: rebuild per-stream buffers after routing table changes.
    // Caller MUST hold m_lock (via Lock()/Unlock()) — this is NOT internally locked
    // because the engine thread does LoadFromFile + Reinitialize as one atomic step.
    //
    // CRITICAL: when the destination count hasn't changed, only Reset() the existing
    // buffers — do NOT delete+recreate them. The network thread holds raw pointers
    // (from GetStreamBuffer) OUTSIDE the lock, and deleting would cause use-after-free
    // on the next AvailableRead()/Read() call. Reset() is safe because head/tail are
    // atomic — the worst case is one iteration seeing an empty buffer.
    void Reinitialize(const RoutingTable& routing) {
        size_t newCount = routing.destinations.size();
        bool rebuilt = false;
        if (newCount == m_streamBuffers.size()) {
            // Count unchanged (common case: gain/mute changes) — safe in-place reset
            for (auto* rb : m_streamBuffers) rb->Reset();
        } else {
            // Count changed — must rebuild. Held lock means Process() is blocked,
            // and the network thread's next GetStreamBuffer() will see new buffers.
            for (auto* rb : m_streamBuffers) delete rb;
            m_streamBuffers.clear();
            for (size_t i = 0; i < newCount; i++) {
                m_streamBuffers.push_back(new RingBuffer(kPerStreamBufSize));
            }
            rebuilt = true;
        }
        if (!m_streamBuffers.empty()) {
            Logger::Instance().Info("MixingBus: %zu streams %s",
                m_streamBuffers.size(), rebuilt ? "rebuilt" : "reset in-place");
        }
    }

    // P6: public lock for engine-thread routing updates.
    // Usage: bus->Lock(); routing.LoadFromFile(...); bus->Reinitialize(routing); bus->Unlock();
    void Lock()   { EnterCriticalSection(&m_lock); }
    void Unlock() { LeaveCriticalSection(&m_lock); }

    // 处理一个音频周期：从输入 RingBuffer 读取 → 按路由分发到各流
    // frameCount: 本周期帧数
    void Process(RingBuffer& input, UINT32 frameCount) {
        if (frameCount == 0) return;

        // P6: guard against concurrent routing update from engine thread.
        // Contention is negligible — the engine thread touches this lock only
        // when the user clicks "Apply Routing", which is human-scale (seconds/minutes).
        EnterCriticalSection(&m_lock);

        UINT32 channels = m_config.channels;
        UINT32 bytesPerFrame = m_config.blockAlign;
        UINT32 totalBytes = frameCount * bytesPerFrame;

        // 从输入环形缓冲读取一整个周期
        BYTE* inputData = new BYTE[totalBytes];
        DWORD read = input.Read(inputData, totalBytes);
        if (read < totalBytes) {
            // 不够时静音填充
            memset(inputData + read, 0, totalBytes - read);
        }

        // 对每路目标流，按路由表混合
        for (size_t si = 0; si < m_streamBuffers.size(); si++) {
            std::vector<BYTE> streamBuf(totalBytes);
            memset(streamBuf.data(), 0, totalBytes);

            bool anyRoute = false;
            for (const auto& route : m_routing.routes) {
                if (route.destStream != (int)si) continue;
                if (route.mute) continue;
                anyRoute = true;

                // 从源通道复制样本，应用增益
                int srcCh = route.sourceChannel;
                float gain = route.gain;
                int bytesPerSample = m_config.bitsPerSample / 8;

                for (UINT32 f = 0; f < frameCount; f++) {
                    for (int c = 0; c < (int)channels; c++) {
                        int dstCh = c; // same channel layout
                        if (c != srcCh && srcCh >= 0) {
                            // 非目标通道填 0（静音）
                            continue;
                        }

                        BYTE* src = inputData + f * bytesPerFrame + c * bytesPerSample;
                        BYTE* dst = streamBuf.data() + f * bytesPerFrame + dstCh * bytesPerSample;

                        // 根据位深应用增益
                        switch (bytesPerSample) {
                            case 2: applyGain16(src, dst, gain); break;
                            case 3: applyGain24(src, dst, gain); break;
                            case 4: applyGain32(src, dst, gain); break;
                        }
                    }
                }
            }

            if (anyRoute) {
                m_streamBuffers[si]->Write(streamBuf.data(), totalBytes);
            }
        }

        delete[] inputData;
        LeaveCriticalSection(&m_lock);
    }

    // 获取第 i 路目标流的 RingBuffer（供网络线程读取）
    RingBuffer* GetStreamBuffer(size_t i) {
        return i < m_streamBuffers.size() ? m_streamBuffers[i] : nullptr;
    }

    size_t StreamCount() const { return m_streamBuffers.size(); }

    void Reset() {
        for (auto* rb : m_streamBuffers) rb->Reset();
    }

private:
    static constexpr size_t kPerStreamBufSize = 65536;  // 64KB per stream

    CRITICAL_SECTION    m_lock;
    const RoutingTable& m_routing;
    const AudioConfig&  m_config;
    std::vector<RingBuffer*> m_streamBuffers;

    static void applyGain16(const BYTE* src, BYTE* dst, float gain) {
        int16_t s;
        memcpy(&s, src, 2);
        int32_t val = (int32_t)((float)s * gain);
        if (val > 32767) val = 32767; else if (val < -32768) val = -32768;
        int16_t out = (int16_t)val;
        memcpy(dst, &out, 2);
    }

    static void applyGain24(const BYTE* src, BYTE* dst, float gain) {
        // L24 packed: little-endian 3 bytes
        int32_t s = (src[0] & 0xFF) | ((src[1] & 0xFF) << 8) | ((int8_t)src[2] << 16);
        int32_t val = (int32_t)((float)s * gain);
        if (val > 8388607) val = 8388607; else if (val < -8388608) val = -8388608;
        dst[0] = (BYTE)(val & 0xFF);
        dst[1] = (BYTE)((val >> 8) & 0xFF);
        dst[2] = (BYTE)((val >> 16) & 0xFF);
    }

    static void applyGain32(const BYTE* src, BYTE* dst, float gain) {
        int32_t s;
        memcpy(&s, src, 4);
        // float within int32 range, clamp
        float f = (float)s * gain;
        if (f > 2147483647.0f) f = 2147483647.0f;
        else if (f < -2147483648.0f) f = -2147483648.0f;
        int32_t out = (int32_t)f;
        memcpy(dst, &out, 4);
    }
};
