#pragma once
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

// ---- Defaults (all overridable via command-line) ----
constexpr UINT32 kDefaultSampleRate    = 48000;
constexpr UINT16 kDefaultBitsPerSample = 24;
constexpr UINT16 kDefaultChannels      = 2;
constexpr UINT32 kDefaultDurationSec   = 10;           // 0 = indefinite
constexpr UINT32 kDefaultPeriodUs      = 100000;       // 10ms in microseconds → 100000 hns
constexpr UINT32 kDefaultPeriodHns     = kDefaultPeriodUs * 10; // 1,000,000 hns = 100ms

constexpr UINT32 kEngineStatusIntervalMs = 1000;       // stats print interval

// Device search string — matches driver's friendly name
constexpr wchar_t kTargetDeviceName[] = L"AES67Driver";

struct AudioConfig {
    UINT32 sampleRate    = kDefaultSampleRate;
    UINT16 bitsPerSample = kDefaultBitsPerSample;
    UINT16 channels      = kDefaultChannels;
    UINT32 durationSec   = kDefaultDurationSec;
    UINT32 periodUs      = kDefaultPeriodUs;           // microseconds

    // Derived fields
    UINT16 blockAlign     = 0;
    UINT32 avgBytesPerSec = 0;
    UINT32 periodFrames   = 0;     // frames per hns period
    UINT32 periodHns      = 0;     // period in hundred-nanosecond units

    void Init() {
        blockAlign     = channels * (bitsPerSample / 8);
        avgBytesPerSec = sampleRate * blockAlign;
        periodHns      = periodUs * 10;  // us → hns (100ns units)
        // Cast to 64-bit to avoid overflow: 48000 * 1000000 = 48B > 2^32
        periodFrames   = (UINT32)(((UINT64)sampleRate * (UINT64)periodHns) / 10000000ULL);
        if (periodFrames == 0) periodFrames = 480;  // safeguard
    }

    // Build WAVEFORMATEXTENSIBLE — required for 24-bit PCM on Windows
    WAVEFORMATEXTENSIBLE ToWaveFormatExtensible() const {
        WAVEFORMATEXTENSIBLE wf = {};
        wf.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels       = channels;
        wf.Format.nSamplesPerSec  = sampleRate;
        wf.Format.wBitsPerSample  = bitsPerSample;
        wf.Format.nBlockAlign     = blockAlign;
        wf.Format.nAvgBytesPerSec = avgBytesPerSec;
        wf.Format.cbSize          = 22;  // sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
        wf.Samples.wValidBitsPerSample = bitsPerSample;
        wf.dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat              = KSDATAFORMAT_SUBTYPE_PCM;
        return wf;
    }

    // Convenience: cast to WAVEFORMATEX* for WASAPI calls
    WAVEFORMATEX* AsWaveFormatEx(WAVEFORMATEXTENSIBLE& storage) const {
        storage = ToWaveFormatExtensible();
        return reinterpret_cast<WAVEFORMATEX*>(&storage);
    }
};
