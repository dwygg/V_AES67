/**
 * AES67 音频引擎 — WASAPI 环回测试 v1
 *
 * 从 AES67Driver 播放设备采集音频，计数帧。验证驱动数据通路。
 * 编译: python build.py engine
 */
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <stdio.h>
#include <initguid.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

#define TARGET_DEVICE L"AES67Driver"

// ---- Running... ----
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
            wprintf(L"Found: %s\n", var.pwszVal);
            PropVariantClear(&var); props->Release(); coll->Release();
            return dev;
        }
        PropVariantClear(&var); props->Release(); dev->Release();
    }
    coll->Release(); return nullptr;
}

int main() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr;

    // 1. Running... 播放设备
    IMMDeviceEnumerator* en;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&en);
    IMMDevice* dev = FindDevice(en, eRender);  // Render direction
    if (!dev) { printf("AES67Driver not found\n"); fflush(stdout); return 1; }

    // 2. Open audio client in loopback mode
    IAudioClient* client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
    if (FAILED(hr)) { printf("Activate failed: 0x%08X\n", hr); return 1; }

    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 24;
    wf.nBlockAlign = 6;
    wf.nAvgBytesPerSec = 48000 * 6;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 5000000, 0, &wf, NULL);  // 500ms buffer
    if (FAILED(hr)) { printf("Init failed: 0x%08X\n", hr); return 1; }

    IAudioCaptureClient* capture;
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    hr = client->Start();

    printf("Loopback started\n"); fflush(stdout);

    DWORD totalFrames = 0;
    DWORD ticks = GetTickCount();
    while (totalFrames < 48000 * 3) {  // run 3s
        Sleep(5);
        UINT32 packets;
        capture->GetNextPacketSize(&packets);
        for (UINT32 p = 0; p < packets; p++) {
            BYTE* data; UINT32 frames; DWORD flags;
            hr = capture->GetBuffer(&data, &frames, &flags, NULL, NULL);
            if (SUCCEEDED(hr)) {
                totalFrames += frames;  // 计数所有帧（包括静音）
                capture->ReleaseBuffer(frames);
            }
        }
        if (GetTickCount() - ticks > 1000) {
            printf("  Frames: %lu\n", totalFrames);
            ticks = GetTickCount();
        }
    }

    client->Stop();
    printf("Done. Total non-silent frames: %lu\n", totalFrames);
    dev->Release(); en->Release();
    CoUninitialize();
    return 0;
}
