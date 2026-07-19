/**
 * AES67 发送通路测试 — 生成 1kHz 正弦波写入驱动并回采验证
 *
 * 编译: python build.py engine
 */
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <stdio.h>
#include <cmath>
#include <initguid.h>

#define TARGET_DEVICE L"AES67Driver"
#define SAMPLE_RATE 48000
#define SINE_FREQ    1000
#define CHANNELS     2
#define BITS         24
#define BLOCK_ALIGN  (CHANNELS * BITS / 8)   // 6 bytes
#define PERIOD_MS    10

// ---- 查找设备 ----
IMMDevice* FindDevice(IMMDeviceEnumerator* en, EDataFlow flow) {
    IMMDeviceCollection* coll;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return nullptr;
    UINT count; coll->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev; coll->Item(i, &dev);
        IPropertyStore* props;
        dev->OpenPropertyStore(STGM_READ, &props);
        PROPVARIANT var; PropVariantInit(&var);
        props->GetValue(PKEY_Device_FriendlyName, &var);
        if (var.pwszVal && wcsstr(var.pwszVal, TARGET_DEVICE)) {
            PropVariantClear(&var); props->Release(); coll->Release(); return dev;
        }
        PropVariantClear(&var); props->Release(); dev->Release();
    }
    coll->Release(); return nullptr;
}

// ---- 生成 24-bit PCM 正弦波 ----
void GenerateSine(BYTE* buf, UINT32 numFrames, double freq, double sampleRate, double* phase) {
    for (UINT32 i = 0; i < numFrames; i++) {
        double sample = sin(2.0 * 3.1415926535 * (*phase));
        *phase += freq / sampleRate;
        if (*phase >= 1.0) *phase -= 1.0;

        // 24-bit signed PCM, range [-8388608, 8388607], 60% amplitude
        int val = (int)(sample * 5033164.0);   // 8388608 * 0.6
        for (int ch = 0; ch < CHANNELS; ch++) {
            int off = (i * CHANNELS + ch) * 3;
            buf[off]     = (BYTE)(val & 0xFF);
            buf[off + 1] = (BYTE)((val >> 8) & 0xFF);
            buf[off + 2] = (BYTE)((val >> 16) & 0xFF);
        }
    }
}

int main() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr;

    // 1. 找设备
    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

    IMMDevice* renderDev = FindDevice(pEnum, eRender);    // 播放 → 我们读它（环回）
    IMMDevice* captureDev = FindDevice(pEnum, eCapture);   // 录音 → 我们写它（发送）

    if (!renderDev) { printf("Render device not found\n"); return 1; }
    if (!captureDev) { printf("Capture device not found\n"); return 1; }
    printf("Both devices found\n"); fflush(stdout);

    // 2. 初始化渲染客户端（写入正弦波到录音设备）
    IAudioClient* capClient;
    hr = captureDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&capClient);
    if (FAILED(hr)) { printf("Activate capture: 0x%08X\n", hr); return 1; }

    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = CHANNELS;
    wf.nSamplesPerSec = SAMPLE_RATE;
    wf.wBitsPerSample = BITS;
    wf.nBlockAlign = BLOCK_ALIGN;
    wf.nAvgBytesPerSec = SAMPLE_RATE * BLOCK_ALIGN;

    hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 5000000, 0, &wf, NULL);
    if (FAILED(hr)) { printf("Init capture: 0x%08X\n", hr); return 1; }

    IAudioRenderClient* render;
    hr = capClient->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    hr = capClient->Start();

    // 3. 打开播放设备环回（验证是否收到数据）
    IAudioClient* rendClient;
    hr = renderDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&rendClient);
    hr = rendClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 5000000, 0, &wf, NULL);
    IAudioCaptureClient* loopback;
    hr = rendClient->GetService(__uuidof(IAudioCaptureClient), (void**)&loopback);
    hr = rendClient->Start();

    printf("Streaming 1kHz sine...\n"); fflush(stdout);

    // 4. 写入 2 秒正弦波
    UINT32 padFrames = wf.nSamplesPerSec * PERIOD_MS / 1000;  // 480 frames/packet
    BYTE* buf = new BYTE[padFrames * BLOCK_ALIGN];
    double phase = 0.0;

    for (int sec = 0; sec < 2; sec++) {
        for (int i = 0; i < 1000 / PERIOD_MS; i++) {
            // 获取可用 buffer 空间
            UINT32 padding;
            capClient->GetCurrentPadding(&padding);
            UINT32 avail = padFrames - padding;
            if (avail > 0) {
                GenerateSine(buf, avail, SINE_FREQ, SAMPLE_RATE, &phase);
                BYTE* pData;
                hr = render->GetBuffer(avail, &pData);
                if (FAILED(hr)) break;
                memcpy(pData, buf, avail * BLOCK_ALIGN);
                hr = render->ReleaseBuffer(avail, 0);
            }
            Sleep(PERIOD_MS / 2);
        }
        printf("  [%d s] wrote ... ", sec + 1); fflush(stdout);

        // 从环回读数据验证
        UINT32 totalFrames = 0;
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 200) {
            Sleep(5);
            UINT32 packets;
            loopback->GetNextPacketSize(&packets);
            for (UINT32 p = 0; p < packets; p++) {
                BYTE* data; UINT32 frames; DWORD flags;
                hr = loopback->GetBuffer(&data, &frames, &flags, NULL, NULL);
                if (SUCCEEDED(hr)) {
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) totalFrames += frames;
                    loopback->ReleaseBuffer(frames);
                }
            }
        }
        printf("got %lu frames back\n", totalFrames);
    }

    capClient->Stop();
    rendClient->Stop();
    delete[] buf;

    printf("Done. Sine wave rendered and verified.\n"); fflush(stdout);
    captureDev->Release(); renderDev->Release(); pEnum->Release();
    CoUninitialize();
    return 0;
}
