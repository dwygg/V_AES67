#pragma once
#include <windows.h>
#include <atomic>
#include <cstring>

// Jitter buffer for AES67 receive path.
// 256 slots (256ms @ 48kHz/1ms packets), indexed by RTP sequence number.
// Read() never blocks — returns silence on underflow to keep audio clock continuous.
class JitterBuffer {
public:
    static constexpr size_t kSlotCount     = 256;          // power of 2
    static constexpr size_t kSlotMask      = kSlotCount - 1;
    static constexpr size_t kPayloadSize   = 288;          // 48 frames × 2ch × 3 bytes L24
    static constexpr size_t kTargetDepth   = 200;          // warm-up: 200ms buffer tolerates clock drift

    JitterBuffer() {
        Reset();
    }

    void Reset() {
        for (auto& v : m_valid) v.store(false, std::memory_order_relaxed);
        memset(m_slots, 0, sizeof(m_slots));
        m_readSeq.store(0, std::memory_order_release);
        m_writeSeq.store(0, std::memory_order_release);
        m_packetCount.store(0, std::memory_order_release);
        m_droppedLate.store(0, std::memory_order_release);
        m_droppedDup.store(0, std::memory_order_release);
        m_hasSync.store(false, std::memory_order_release);
    }

    // Network receiver thread: insert a received RTP packet.
    // Returns true if accepted, false if dropped (too late, duplicate, or out of window).
    bool Insert(uint16_t seq, const BYTE* payload) {
        // On first packet, lock the read sequence to establish playback start
        if (!m_hasSync.load(std::memory_order_acquire)) {
            m_readSeq.store(seq, std::memory_order_release);
            m_writeSeq.store(seq, std::memory_order_release);
            m_hasSync.store(true, std::memory_order_release);
        }

        uint16_t readSeq = m_readSeq.load(std::memory_order_acquire);
        uint16_t writeSeq = m_writeSeq.load(std::memory_order_acquire);

        // Compute distance from current read position
        int16_t aheadBy = (int16_t)(seq - readSeq);

        // Too late: already played or about to be played
        if (aheadBy <= 0) {
            // Within one window behind: still accept if within reorder tolerance
            if (aheadBy > -(int16_t)(kSlotCount / 2)) {
                size_t idx = (size_t)seq & kSlotMask;
                if (m_valid[idx] && m_slotSeq[idx] == seq) {
                    m_droppedDup.fetch_add(1, std::memory_order_relaxed);
                    return false;  // duplicate
                }
                if (m_valid[idx].load(std::memory_order_relaxed) && m_slotSeq[idx] == seq) {
                    m_droppedDup.fetch_add(1, std::memory_order_relaxed);
                    return false;  // duplicate
                }
                memcpy(m_slots[idx], payload, kPayloadSize);
                m_slotSeq[idx] = seq;
                m_valid[idx].store(true, std::memory_order_release);
                return true;
            }
            m_droppedLate.fetch_add(1, std::memory_order_relaxed);
            return false;  // way too late, drop
        }

        // Within forward window
        if (aheadBy < (int16_t)kSlotCount) {
            size_t idx = (size_t)seq & kSlotMask;
            if (m_valid[idx].load(std::memory_order_relaxed) && m_slotSeq[idx] == seq) {
                m_droppedDup.fetch_add(1, std::memory_order_relaxed);
                return false;  // duplicate
            }
            memcpy(m_slots[idx], payload, kPayloadSize);
            m_slotSeq[idx] = seq;
            m_valid[idx].store(true, std::memory_order_release);

            // Track the highest contiguous sequence received
            if (aheadBy > (int16_t)(writeSeq - readSeq)) {
                m_writeSeq.store(seq, std::memory_order_release);
            }
            m_packetCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Way too far ahead → drop
        m_droppedLate.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Audio render thread: read one 288-byte payload.
    // Always returns kPayloadSize bytes — fills with silence if no data.
    // Advances m_readSeq by 1 every call.
    void Read(BYTE* dst) {
        uint16_t seq = m_readSeq.load(std::memory_order_relaxed);
        size_t idx = (size_t)seq & kSlotMask;

        if (m_valid[idx].load(std::memory_order_acquire) && m_slotSeq[idx] == seq) {
            memcpy(dst, m_slots[idx], kPayloadSize);
            m_valid[idx].store(false, std::memory_order_relaxed);
        } else {
            // Underflow: fill with silence
            memset(dst, 0, kPayloadSize);
        }

        m_readSeq.store(seq + 1, std::memory_order_release);
    }

    // Read multiple contiguous packets (e.g., for a 10ms WASAPI period = 10 packets)
    // Each packet = 288 bytes. Returns actual packets read (partial = silence-padded).
    size_t ReadMultiple(BYTE* dst, size_t maxPackets) {
        size_t read = 0;
        for (size_t i = 0; i < maxPackets; i++) {
            Read(dst);
            dst += kPayloadSize;
            read++;
        }
        return read;
    }

    // How many valid packets are buffered ahead of the read cursor.
    // Used for startup warm-up: wait until HasEnough() before starting playback.
    size_t AvailableRead() const {
        uint16_t readSeq = m_readSeq.load(std::memory_order_acquire);
        size_t count = 0;
        uint16_t seq = readSeq;
        for (size_t i = 0; i < kSlotCount; i++) {
            size_t idx = (size_t)seq & kSlotMask;
            if (m_valid[idx].load(std::memory_order_relaxed) && m_slotSeq[idx] == seq) {
                count++;
                seq++;
            } else {
                break;  // only count contiguous
            }
        }
        return count;
    }

    bool HasEnough() const { return AvailableRead() >= kTargetDepth; }

    // Stats
    size_t PacketCount() const { return m_packetCount.load(std::memory_order_relaxed); }
    size_t DroppedLate() const { return m_droppedLate.load(std::memory_order_relaxed); }
    size_t DroppedDup()  const { return m_droppedDup.load(std::memory_order_relaxed);  }

private:
    BYTE                m_slots[kSlotCount][kPayloadSize];
    std::atomic<bool>   m_valid[kSlotCount];
    uint16_t            m_slotSeq[kSlotCount];

    std::atomic<uint16_t> m_readSeq{0};
    std::atomic<uint16_t> m_writeSeq{0};

    std::atomic<size_t> m_packetCount{0};
    std::atomic<size_t> m_droppedLate{0};
    std::atomic<size_t> m_droppedDup{0};
    std::atomic<bool>   m_hasSync{false};
};
