#include "wasapi_device.h"
#include "logger.h"
#include <initguid.h>

// ---- FindAudioDevice ----
ComPtr<IMMDevice> FindAudioDevice(
    IMMDeviceEnumerator* enumerator,
    EDataFlow flow,
    const wchar_t* nameSubstring)
{
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)))
        return ComPtr<IMMDevice>();

    UINT count = 0;
    coll->GetCount(&count);
    ComPtr<IMMDevice> result;

    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;

        IPropertyStore* props = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))) {
                if (var.pwszVal && wcsstr(var.pwszVal, nameSubstring)) {
                    Logger::Instance().Info("Found device: %ls", var.pwszVal);
                    PropVariantClear(&var);
                    props->Release();
                    result.Reset(dev);
                    break;
                }
                PropVariantClear(&var);
            }
            props->Release();
        }
        dev->Release();
    }
    coll->Release();
    return result;
}

// ---- WasapiClient ----

WasapiClient::~WasapiClient() {
    // COM references auto-released by ComPtr destructors.
    // Stop/Reset handled by AudioThread::RunLoop() cleanup.
}

HRESULT WasapiClient::InitLoopback(IMMDevice* device, const AudioConfig& config) {
    // Activate IAudioClient
    HRESULT hr = device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        (void**)m_client.GetAddressOf());
    if (FAILED(hr)) {
        Logger::Instance().Error("IAudioClient::Activate failed: 0x%08X", hr);
        return hr;
    }

    WAVEFORMATEXTENSIBLE wf = config.ToWaveFormatExtensible();

    // Try event-driven loopback first (Win10+)
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = m_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        config.periodHns, 0,
        reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);

    if (FAILED(hr)) {
        // Fallback: polling mode (no event callback)
        Logger::Instance().Warn("Event-driven loopback failed (0x%08X), falling back to polling", hr);
        streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
        m_eventDriven = false;
        hr = m_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            config.periodHns, 0,
            reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
    } else {
        m_eventDriven = true;
    }

    if (FAILED(hr)) {
        Logger::Instance().Error("IAudioClient::Initialize failed: 0x%08X", hr);
        return hr;
    }

    m_wf = wf;

    // Get buffer size
    hr = m_client->GetBufferSize(&m_bufferFrames);
    if (FAILED(hr)) return hr;

    // Get capture service
    hr = m_client->GetService(
        __uuidof(IAudioCaptureClient),
        (void**)m_capture.GetAddressOf());
    if (FAILED(hr)) {
        Logger::Instance().Error("GetService(IAudioCaptureClient) failed: 0x%08X", hr);
        return hr;
    }

    m_periodFrames = config.periodFrames;
    if (m_periodFrames < 1) m_periodFrames = 480;

    Logger::Instance().Info("Loopback init: %u ch %u Hz %u-bit, buffer=%u frames, period=%u frames, %s",
        config.channels, config.sampleRate, config.bitsPerSample,
        m_bufferFrames, m_periodFrames,
        m_eventDriven ? "event-driven" : "polling");
    return S_OK;
}

HRESULT WasapiClient::InitRender(IMMDevice* device, const AudioConfig& config) {
    HRESULT hr = device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        (void**)m_client.GetAddressOf());
    if (FAILED(hr)) return hr;

    WAVEFORMATEXTENSIBLE wf = config.ToWaveFormatExtensible();

    // Try event-driven first
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = m_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        config.periodHns, 0,
        reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);

    if (FAILED(hr)) {
        // Fallback to polling
        Logger::Instance().Warn("Event-driven render failed (0x%08X), falling back to polling", hr);
        streamFlags = 0;
        m_eventDriven = false;
        hr = m_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            config.periodHns, 0,
            reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
    } else {
        m_eventDriven = true;
    }

    if (FAILED(hr)) return hr;

    m_wf = wf;
    hr = m_client->GetBufferSize(&m_bufferFrames);
    if (FAILED(hr)) return hr;

    hr = m_client->GetService(
        __uuidof(IAudioRenderClient),
        (void**)m_render.GetAddressOf());
    if (FAILED(hr)) return hr;

    m_periodFrames = config.periodFrames;
    if (m_periodFrames < 1) m_periodFrames = 480;

    Logger::Instance().Info("Render init: %u ch %u Hz %u-bit, buffer=%u frames",
        config.channels, config.sampleRate, config.bitsPerSample, m_bufferFrames);
    return S_OK;
}

HRESULT WasapiClient::SetEventHandle(HANDLE hEvent) {
    if (!m_eventDriven) return S_FALSE;  // Not event-driven, no-op
    if (!m_client.Get()) return E_POINTER;
    return m_client->SetEventHandle(hEvent);
}

HRESULT WasapiClient::Start() {
    if (!m_client.Get()) return E_POINTER;
    return m_client->Start();
}

HRESULT WasapiClient::Stop() {
    if (!m_client.Get()) return E_POINTER;
    return m_client->Stop();
}

HRESULT WasapiClient::Reset() {
    if (!m_client.Get()) return E_POINTER;
    return m_client->Reset();
}
