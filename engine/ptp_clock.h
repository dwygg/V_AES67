#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

// PTP clock model — software slave clock using QPC as local timebase.
// EMA-filtered offset and frequency ratio estimation.
// Precision: ~100μs with software timestamps (QPC at recvfrom/sendto).

enum class PtpState {
    FREE_RUN,   // Waiting for first Sync (no master)
    TRACKING,   // Receiving Sync, computing offset
    LOCKED,     // Offset stable within threshold for N consecutive measurements
    HOLDOVER,   // Sync timeout, using last known drift
};

// Nanosecond-resolution PTP timestamp (IEEE 1588 Timestamp format)
struct PtpTimestamp {
    int64_t ns;  // nanoseconds since PTP epoch (1970-01-01T00:00:00 TAI)

    static PtpTimestamp FromSeconds(uint64_t sec, uint32_t nsec) {
        return { (int64_t)((uint64_t)sec * 1000000000ULL + nsec) };
    }

    uint64_t Seconds() const { return (uint64_t)(ns / 1000000000LL); }
    uint32_t Nanoseconds() const { return (uint32_t)(ns % 1000000000LL); }
};

class PtpClock {
public:
    PtpClock();

    // Call when a Sync/Follow_Up pair is received
    void UpdateSync(PtpTimestamp t1_master, int64_t t2_local_qpc);

    // Call when a Delay_Resp is received
    void UpdateDelay(PtpTimestamp t4_master, int64_t t3_local_qpc);

    // Convert QPC tick to estimated PTP time
    PtpTimestamp QpcToPtp(int64_t qpc) const;
    int64_t PtpToQpc(PtpTimestamp ptp) const;

    // Current estimates
    double   GetOffsetNs()    const { return m_offset; }
    double   GetDriftPpb()    const { return m_driftPpb; }  // parts per billion
    PtpState GetState()       const { return m_state.load(std::memory_order_acquire); }
    double   GetMeanPathDelay() const { return m_meanDelay; }

    // Master clock info
    uint64_t GetGrandmasterId() const { return m_gmIdentity; }
    uint16_t GetDomainNumber()  const { return m_domainNumber; }

    // RTP timestamp to PTP time (rate = 1/48kHz per RTP ts step)
    PtpTimestamp RtpToPtp(uint32_t rtpTs, uint32_t rtpTsAtSync, PtpTimestamp ptpAtSync) const;

private:
    void UpdateModel();

    // QPC frequency
    int64_t  m_qpcFreq;       // ticks per second
    int64_t  m_qpcBase;        // QPC at last update

    // EMA-filtered values
    double   m_offset   = 0.0;     // nanoseconds (PTP - local)
    double   m_driftPpb = 0.0;     // parts per billion (frequency offset)
    double   m_meanDelay = 0.0;    // one-way path delay (ns)

    // EMA coefficients
    static constexpr double kAlphaOffset = 0.1;    // offset smoothing
    static constexpr double kAlphaDelay  = 0.1;    // delay smoothing
    static constexpr double kBetaDrift    = 0.01;  // drift smoothing (slower)
    static constexpr double kLockThresholdNs = 500.0;  // ±500ns for lock
    static constexpr int    kLockCountNeeded  = 10;     // consecutive locked to declare LOCKED

    // Raw measurements
    double   m_rawOffset = 0.0;
    double   m_rawDelay  = 0.0;
    int64_t  m_lastSyncQpc = 0;
    int64_t  m_lastSyncPtp = 0;  // nanoseconds

    // State tracking
    std::atomic<PtpState> m_state{PtpState::FREE_RUN};
    int     m_lockCount = 0;
    int64_t m_syncCount  = 0;

    // Master info
    uint64_t m_gmIdentity = 0;
    uint16_t m_domainNumber = 0;
    uint16_t m_seqSync = 0;
    uint16_t m_seqDelay = 0;
};
