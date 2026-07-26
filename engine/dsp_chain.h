#pragma once
#include <windows.h>
#include <cstring>

// P7: Per-stream DSP processing chain.
// Operates in-place on interleaved PCM blocks after routing mix,
// before RingBuffer Write(). P7a implements gain trim + mute.
// P7b will add 3-band biquad EQ.

struct DspSettings {
    float gain = 1.0f;   // trim gain, 0.0 ~ 2.0, default 1.0 (pass-through)
    bool  mute = false;  // per-stream mute
};

// --- Per-sample gain helpers (L24 / L16 / L32, little-endian) ---

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void applyGain16(const BYTE* src, BYTE* dst, float gain) {
    int16_t s;
    memcpy(&s, src, 2);
    int32_t val = (int32_t)((float)s * gain);
    if (val > 32767) val = 32767; else if (val < -32768) val = -32768;
    int16_t out = (int16_t)val;
    memcpy(dst, &out, 2);
}

static void applyGain24(const BYTE* src, BYTE* dst, float gain) {
    // L24 packed: little-endian 3 bytes, MSB sign-extended
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
    float f = (float)s * gain;
    f = clampf(f, -2147483648.0f, 2147483647.0f);
    int32_t out = (int32_t)f;
    memcpy(dst, &out, 4);
}

// --- Main DSP entry point ---
// Process a block of interleaved PCM in-place.
// - data: interleaved PCM bytes (L16/L24/L32 little-endian)
// - frames: frame count (sample count per channel)
// - s: DSP settings for this stream
// - channels: channel count (2 = stereo)
// - bitsPerSample: 16, 24, or 32
static void dspProcessBlock(BYTE* data, UINT32 frames, const DspSettings& s,
                            UINT16 channels, UINT16 bitsPerSample) {
    if (frames == 0 || channels == 0) return;

    // 1. Mute: zero the entire block
    if (s.mute) {
        UINT32 bytesPerFrame = (UINT32)channels * (bitsPerSample / 8);
        memset(data, 0, (size_t)frames * bytesPerFrame);
        return;
    }

    // 2. Gain trim: pass-through if ~1.0 (tiny epsilon to skip float noise)
    if (s.gain < 0.0001f) {
        // gain ≈ 0 → silence (same as mute, handled above for exact 0)
        UINT32 bytesPerFrame = (UINT32)channels * (bitsPerSample / 8);
        memset(data, 0, (size_t)frames * bytesPerFrame);
        return;
    }
    if (s.gain > 0.9999f && s.gain < 1.0001f) return;  // unity gain, skip

    UINT32 bytesPerSample = bitsPerSample / 8;
    UINT32 bytesPerFrame  = (UINT32)channels * bytesPerSample;

    // Select gain apply function once (branch outside inner loops)
    void (*applyGain)(const BYTE*, BYTE*, float) = nullptr;
    switch (bytesPerSample) {
        case 2: applyGain = applyGain16; break;
        case 3: applyGain = applyGain24; break;
        case 4: applyGain = applyGain32; break;
        default: return;
    }

    // Process sample-by-sample in-place.
    // We process backwards or use temp copies since src==dst for single-sample ops.
    // For L24 (3 bytes), src and dst alias within the same sample but NOT across
    // samples, so sequential forward is safe as long as each 3-byte write completes
    // before reading the next 3-byte sample.
    BYTE* p = data;
    UINT32 totalFrames = frames * (UINT32)channels;  // total samples (not frame pairs)
    for (UINT32 i = 0; i < totalFrames; i++) {
        applyGain(p, p, s.gain);  // in-place: src == dst
        p += bytesPerSample;
    }
}
