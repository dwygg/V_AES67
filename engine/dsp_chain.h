#pragma once
#include <windows.h>
#include <cstring>
#include <cmath>
#include <malloc.h>

// P7: Per-stream DSP processing chain.
// P7a: gain trim + mute. P7b: 3-band biquad EQ (RBJ cookbook).

static constexpr float DSP_PI = 3.14159265358979323846f;
static constexpr UINT32 kMaxDspFrames = 2048;

// ===================================================================
//  Biquad Filter (Direct Form I)
// ===================================================================

struct BiquadCoeffs {
    float b0, b1, b2;
    float a1, a2;
    BiquadCoeffs() : b0(1.0f), b1(0.0f), b2(0.0f), a1(0.0f), a2(0.0f) {}
    bool IsIdentity() const {
        return b0 > 0.9999f && b0 < 1.0001f &&
               b1 > -0.0001f && b1 < 0.0001f &&
               b2 > -0.0001f && b2 < 0.0001f &&
               a1 > -0.0001f && a1 < 0.0001f &&
               a2 > -0.0001f && a2 < 0.0001f;
    }
};

struct BiquadState {
    float x1, x2, y1, y2;
    BiquadState() : x1(0), x2(0), y1(0), y2(0) {}
    void Reset() { x1 = 0; x2 = 0; y1 = 0; y2 = 0; }
};

inline float processBiquad(BiquadState& st, const BiquadCoeffs& c, float x) {
    float y = c.b0 * x + c.b1 * st.x1 + c.b2 * st.x2 - c.a1 * st.y1 - c.a2 * st.y2;
    st.x2 = st.x1; st.x1 = x;
    st.y2 = st.y1; st.y1 = y;
    return y;
}

// ===================================================================
//  RBJ Cookbook Design Functions
// ===================================================================

static BiquadCoeffs designPeak(float freqHz, float gainDb, float Q, float Fs) {
    BiquadCoeffs c;
    if (fabsf(gainDb) < 0.001f) return c;
    float A  = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * DSP_PI * freqHz / Fs;
    float cosW = cosf(w0), sinW = sinf(w0);
    float alpha = sinW / (2.0f * Q);
    float b0 =  1.0f + alpha * A;
    float b1 = -2.0f * cosW;
    float b2 =  1.0f - alpha * A;
    float a0 =  1.0f + alpha / A;
    float a1 = -2.0f * cosW;
    float a2 =  1.0f - alpha / A;
    float invA0 = 1.0f / a0;
    c.b0 = b0 * invA0; c.b1 = b1 * invA0; c.b2 = b2 * invA0;
    c.a1 = a1 * invA0; c.a2 = a2 * invA0;
    return c;
}

static BiquadCoeffs designLowShelf(float freqHz, float gainDb, float Q, float Fs) {
    BiquadCoeffs c;
    if (fabsf(gainDb) < 0.001f) return c;
    float A  = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * DSP_PI * freqHz / Fs;
    float cosW = cosf(w0), sinW = sinf(w0);
    float alpha = sinW / (2.0f * Q);
    float sqrtA = sqrtf(A);
    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW + 2.0f * sqrtA * alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cosW + 2.0f * sqrtA * alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW);
    float a2 = (A + 1.0f) + (A - 1.0f) * cosW - 2.0f * sqrtA * alpha;
    float invA0 = 1.0f / a0;
    c.b0 = b0 * invA0; c.b1 = b1 * invA0; c.b2 = b2 * invA0;
    c.a1 = a1 * invA0; c.a2 = a2 * invA0;
    return c;
}

static BiquadCoeffs designHighShelf(float freqHz, float gainDb, float Q, float Fs) {
    BiquadCoeffs c;
    if (fabsf(gainDb) < 0.001f) return c;
    float A  = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * DSP_PI * freqHz / Fs;
    float cosW = -cosf(w0);  // high shelf key difference
    float sinW = sinf(w0);
    float alpha = sinW / (2.0f * Q);
    float sqrtA = sqrtf(A);
    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW + 2.0f * sqrtA * alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cosW + 2.0f * sqrtA * alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW);
    float a2 = (A + 1.0f) + (A - 1.0f) * cosW - 2.0f * sqrtA * alpha;
    float invA0 = 1.0f / a0;
    c.b0 = b0 * invA0; c.b1 = b1 * invA0; c.b2 = b2 * invA0;
    c.a1 = a1 * invA0; c.a2 = a2 * invA0;
    return c;
}

// ===================================================================
//  DspBand — One EQ band with per-channel state
// ===================================================================

struct DspBand {
    BiquadCoeffs coeffs;
    BiquadState  state[2];
    float freqHz   = 1000.0f;
    float gainDb   = 0.0f;
    float Q        = 0.707f;
    bool  enabled  = false;

    void Redesign(int bandType, float sampleRate) {
        switch (bandType) {
            case 0: coeffs = designLowShelf (freqHz, gainDb, Q, sampleRate); break;
            case 1: coeffs = designPeak     (freqHz, gainDb, Q, sampleRate); break;
            case 2: coeffs = designHighShelf(freqHz, gainDb, Q, sampleRate); break;
        }
        state[0].Reset(); state[1].Reset();
    }
    void Reset() { state[0].Reset(); state[1].Reset(); }
};

// ===================================================================
//  DspSettings — Per-stream DSP configuration
// ===================================================================

struct DspSettings {
    float   gain = 1.0f;
    bool    mute = false;
    DspBand bands[3];

    DspSettings() { InitBands(); }
    DspSettings(float g, bool m) : gain(g), mute(m) { InitBands(); }

    void RedesignAll(float sampleRate) {
        for (int i = 0; i < 3; i++)
            if (bands[i].enabled) bands[i].Redesign(i, sampleRate);
    }
    bool HasActiveEQ() const {
        for (int i = 0; i < 3; i++)
            if (bands[i].enabled && fabsf(bands[i].gainDb) > 0.001f) return true;
        return false;
    }
private:
    void InitBands() {
        bands[0].freqHz = 200.0f;  bands[0].gainDb = 0.0f; bands[0].Q = 0.707f; bands[0].enabled = false;
        bands[1].freqHz = 1000.0f; bands[1].gainDb = 0.0f; bands[1].Q = 1.0f;   bands[1].enabled = false;
        bands[2].freqHz = 8000.0f; bands[2].gainDb = 0.0f; bands[2].Q = 0.707f; bands[2].enabled = false;
    }
};

// ===================================================================
//  Float utilities
// ===================================================================

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float softClip(float x) { return x / (1.0f + fabsf(x)); }

inline float unpackToFloat(const BYTE* src, UINT16 bps) {
    switch (bps) {
        case 2: { int16_t s; memcpy(&s, src, 2); return (float)s / 32768.0f; }
        case 3: { int32_t s = (src[0]&0xFF)|((src[1]&0xFF)<<8)|((int8_t)src[2]<<16); return (float)s / 8388608.0f; }
        case 4: { int32_t s; memcpy(&s, src, 4); return (float)s / 2147483648.0f; }
        default: return 0.0f;
    }
}

inline void packFromFloat(BYTE* dst, float sample, UINT16 bps) {
    sample = softClip(sample);
    switch (bps) {
        case 2: {
            float s = sample * 32768.0f; s = clampf(s, -32768.0f, 32767.0f);
            int16_t v = (int16_t)s; memcpy(dst, &v, 2);
        } break;
        case 3: {
            float s = sample * 8388608.0f; s = clampf(s, -8388608.0f, 8388607.0f);
            int32_t v = (int32_t)s;
            dst[0] = (BYTE)(v & 0xFF); dst[1] = (BYTE)((v>>8)&0xFF); dst[2] = (BYTE)((v>>16)&0xFF);
        } break;
        case 4: {
            float s = sample * 2147483648.0f; s = clampf(s, -2147483648.0f, 2147483647.0f);
            int32_t v = (int32_t)s; memcpy(dst, &v, 4);
        } break;
    }
}

// ===================================================================
//  Main DSP entry point — in-place interleaved PCM processing
// ===================================================================

static void dspProcessBlock(BYTE* data, UINT32 frames, DspSettings& s,
                            UINT16 channels, UINT16 bitsPerSample) {
    if (frames == 0 || channels == 0) return;
    UINT16 bps = bitsPerSample / 8;
    UINT32 stride = (UINT32)channels * bps;

    if (s.mute) { memset(data, 0, (size_t)frames * stride); return; }

    bool unityGain = (s.gain > 0.9999f && s.gain < 1.0001f);
    if (unityGain && !s.HasActiveEQ()) return;
    if (s.gain < 0.0001f) { memset(data, 0, (size_t)frames * stride); return; }

    UINT16 chCount = (channels < 2) ? channels : 2;
    float* temp = (frames <= kMaxDspFrames)
        ? (float*)_alloca((size_t)frames * sizeof(float))
        : new float[frames];

    for (UINT16 ch = 0; ch < chCount; ch++) {
        const BYTE* src = data + (size_t)ch * bps;
        for (UINT32 i = 0; i < frames; i++) { temp[i] = unpackToFloat(src, bps); src += stride; }
        if (!unityGain) { for (UINT32 i = 0; i < frames; i++) temp[i] *= s.gain; }
        if (s.HasActiveEQ()) {
            for (int band = 0; band < 3; band++) {
                if (!s.bands[band].enabled || s.bands[band].coeffs.IsIdentity()) continue;
                BiquadState& st = s.bands[band].state[ch];
                const BiquadCoeffs& c = s.bands[band].coeffs;
                for (UINT32 i = 0; i < frames; i++) temp[i] = processBiquad(st, c, temp[i]);
            }
        }
        BYTE* dst = data + (size_t)ch * bps;
        for (UINT32 i = 0; i < frames; i++) { packFromFloat(dst, temp[i], bps); dst += stride; }
    }
    if (unityGain) { if (frames > kMaxDspFrames) delete[] temp; return; }
    for (UINT16 ch = chCount; ch < channels; ch++) {
        BYTE* p = data + (size_t)ch * bps;
        for (UINT32 i = 0; i < frames; i++) {
            float f = unpackToFloat(p, bps) * s.gain;
            packFromFloat(p, f, bps); p += stride;
        }
    }
    if (frames > kMaxDspFrames) delete[] temp;
}
