#include "ptp_clock.h"
#include <cmath>

PtpClock::PtpClock() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = freq.QuadPart;
    QueryPerformanceCounter((LARGE_INTEGER*)&m_qpcBase);
}

void PtpClock::UpdateSync(PtpTimestamp t1_master, int64_t t2_local_qpc) {
    // t1: master send time (from Follow_Up)
    // t2: local receive time (QPC)
    m_lastSyncQpc = t2_local_qpc;
    m_lastSyncPtp = t1_master.ns;
    m_seqSync++;
    m_syncCount++;

    // Store raw values for when Delay_Resp arrives
    m_rawDelay = 0;  // will be computed in UpdateDelay
}

void PtpClock::UpdateDelay(PtpTimestamp t4_master, int64_t t3_local_qpc) {
    // t4: master receive time (from Delay_Resp)
    // t3: local send time (QPC)
    // t2: from last Sync
    // t1: from last Sync

    // Convert QPC times to nanoseconds
    double t1_ns = (double)m_lastSyncPtp;                    // master PTP time
    double t2_ns = (double)(m_lastSyncQpc - m_qpcBase) * 1e9 / (double)m_qpcFreq;
    double t3_ns = (double)(t3_local_qpc  - m_qpcBase) * 1e9 / (double)m_qpcFreq;
    double t4_ns = (double)t4_master.ns;                     // master PTP time

    // E2E delay mechanism:
    // offset = ((t2 - t1) - (t4 - t3)) / 2
    // delay  = ((t2 - t1) + (t4 - t3)) / 2
    double forward  = t2_ns - t1_ns;  // Sync propagation (master → slave)
    double backward = t4_ns - t3_ns;  // Delay_Req propagation (slave → master)

    m_rawOffset = (forward - backward) / 2.0;
    m_rawDelay  = (forward + backward) / 2.0;

    // Reject obviously invalid measurements (delay > 1 second or < 0)
    if (m_rawDelay < 0 || m_rawDelay > 1e9) {
        return;  // invalid, skip this cycle
    }

    UpdateModel();
}

void PtpClock::UpdateModel() {
    double prevOffset = m_offset;
    double prevDelay  = m_meanDelay;

    // EMA: offset (fast tracking)
    m_offset = kAlphaOffset * m_rawOffset + (1.0 - kAlphaOffset) * m_offset;

    // EMA: delay (fast tracking)
    m_meanDelay = kAlphaDelay * m_rawDelay + (1.0 - kAlphaDelay) * m_meanDelay;

    // Drift estimation: frequency offset between local and master clock
    // drift = d(offset)/dt normalized to frequency = offset_change_per_second / 1e9
    if (m_syncCount > 1 && m_lastSyncQpc > 0) {
        double dt_ns = (double)(m_lastSyncQpc - m_qpcBase) * 1e9 / (double)m_qpcFreq;
        // Use a simple first-order difference. More sophisticated Kalman would be better.
        double offsetDelta = m_offset - prevOffset;
        if (dt_ns > 1e8) {  // at least 100ms between measurements
            double rawDrift = (offsetDelta / dt_ns) * 1e9;  // ppb
            m_driftPpb = kBetaDrift * rawDrift + (1.0 - kBetaDrift) * m_driftPpb;
        }
    }

    // State transitions
    PtpState current = m_state.load(std::memory_order_acquire);

    if (current == PtpState::FREE_RUN || current == PtpState::HOLDOVER) {
        m_state.store(PtpState::TRACKING, std::memory_order_release);
        m_lockCount = 0;
    } else if (current == PtpState::TRACKING) {
        if (std::abs(m_offset) < kLockThresholdNs) {
            m_lockCount++;
            if (m_lockCount >= kLockCountNeeded) {
                m_state.store(PtpState::LOCKED, std::memory_order_release);
            }
        } else {
            m_lockCount = 0;
        }
    } else if (current == PtpState::LOCKED) {
        if (std::abs(m_offset) >= kLockThresholdNs * 2) {
            m_state.store(PtpState::TRACKING, std::memory_order_release);
            m_lockCount = 0;
        }
    }
}

PtpTimestamp PtpClock::QpcToPtp(int64_t qpc) const {
    double t_ns = (double)(qpc - m_qpcBase) * 1e9 / (double)m_qpcFreq;
    // Apply estimated offset (PTP = local + offset)
    t_ns += m_offset;
    return { (int64_t)t_ns };
}

int64_t PtpClock::PtpToQpc(PtpTimestamp ptp) const {
    double ptp_ns = (double)ptp.ns;
    double local_ns = ptp_ns - m_offset;
    return m_qpcBase + (int64_t)(local_ns * (double)m_qpcFreq / 1e9);
}

PtpTimestamp PtpClock::RtpToPtp(uint32_t rtpTs, uint32_t rtpTsAtSync,
                                 PtpTimestamp ptpAtSync) const {
    int32_t delta = (int32_t)(rtpTs - rtpTsAtSync);
    int64_t delta_ns = (int64_t)delta * 1000000000LL / 48000;  // 48kHz RTP clock
    return { ptpAtSync.ns + delta_ns };
}
