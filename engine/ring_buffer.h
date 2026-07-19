#pragma once
#include <windows.h>
#include <atomic>
#include <cstddef>

// Lock-free SPSC (Single Producer, Single Consumer) ring buffer.
// Power-of-2 capacity. 64-byte aligned head/tail prevent false sharing.
// Producer: audio thread calls Write(). Never blocks.
// Consumer: network thread calls Read(). Never blocks.

class RingBuffer {
public:
    explicit RingBuffer(size_t capacityBytes) {
        // Round up to next power of 2
        m_capacity = 1;
        while (m_capacity < capacityBytes)
            m_capacity <<= 1;
        m_mask = m_capacity - 1;
        m_buffer = new BYTE[m_capacity]();  // zero-init
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

    ~RingBuffer() {
        delete[] m_buffer;
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ---- Producer (audio thread) ----
    // Write up to 'bytes' into the ring buffer. Returns bytes actually written.
    // Partial write = network thread is falling behind (track bufferOverflows).
    size_t Write(const BYTE* src, size_t bytes) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t tail = m_tail.load(std::memory_order_acquire);

        size_t used = (head - tail) & m_mask;
        size_t free = m_capacity - used - 1;  // -1 to prevent wrap overlap

        size_t toWrite = (bytes < free) ? bytes : free;
        if (toWrite == 0) return 0;

        // Copy in chunks (may wrap around buffer end)
        size_t firstChunk = m_capacity - head;
        if (toWrite <= firstChunk) {
            memcpy(m_buffer + head, src, toWrite);
        } else {
            memcpy(m_buffer + head, src, firstChunk);
            memcpy(m_buffer, src + firstChunk, toWrite - firstChunk);
        }

        m_head.store((head + toWrite) & m_mask, std::memory_order_release);
        return toWrite;
    }

    // ---- Consumer (network thread) ----
    // Read up to 'bytes' from the ring buffer. Returns bytes actually read.
    size_t Read(BYTE* dst, size_t bytes) {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t head = m_head.load(std::memory_order_acquire);

        size_t available = (head - tail) & m_mask;
        if (available == 0) return 0;

        size_t toRead = (bytes < available) ? bytes : available;

        // Copy in chunks
        size_t firstChunk = m_capacity - tail;
        if (toRead <= firstChunk) {
            memcpy(dst, m_buffer + tail, toRead);
        } else {
            memcpy(dst, m_buffer + tail, firstChunk);
            memcpy(dst + firstChunk, m_buffer, toRead - firstChunk);
        }

        m_tail.store((tail + toRead) & m_mask, std::memory_order_release);
        return toRead;
    }

    size_t AvailableRead() const {
        size_t head = m_head.load(std::memory_order_acquire);
        size_t tail = m_tail.load(std::memory_order_relaxed);
        return (head - tail) & m_mask;
    }

    size_t AvailableWrite() const {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t tail = m_tail.load(std::memory_order_acquire);
        size_t used = (head - tail) & m_mask;
        return m_capacity - used - 1;
    }

    void Reset() {
        m_head.store(0, std::memory_order_release);
        m_tail.store(0, std::memory_order_release);
    }

    size_t Capacity() const { return m_capacity; }

private:
    BYTE*  m_buffer   = nullptr;
    size_t m_capacity = 0;
    size_t m_mask     = 0;

    // Cache-line alignment prevents false sharing between cores
    alignas(64) std::atomic<size_t> m_head;   // Producer writes
    alignas(64) std::atomic<size_t> m_tail;   // Consumer writes
};
