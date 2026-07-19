#pragma once
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include "audio_config.h"

// ---- RAII: CoInitialize/CoUninitialize ----
class ComInitializer {
public:
    explicit ComInitializer(DWORD flags = COINIT_MULTITHREADED) {
        m_hr = CoInitializeEx(nullptr, flags);
    }
    ~ComInitializer() {
        if (SUCCEEDED(m_hr)) CoUninitialize();
    }
    bool Ok() const { return SUCCEEDED(m_hr); }
    HRESULT Hr() const { return m_hr; }
    ComInitializer(const ComInitializer&) = delete;
    ComInitializer& operator=(const ComInitializer&) = delete;
private:
    HRESULT m_hr;
};

// ---- RAII: COM interface pointer ----
template<typename T>
class ComPtr {
public:
    ComPtr() : m_ptr(nullptr) {}
    explicit ComPtr(T* ptr) : m_ptr(ptr) {}
    ~ComPtr() { if (m_ptr) m_ptr->Release(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { if (m_ptr) m_ptr->Release(); m_ptr = other.m_ptr; other.m_ptr = nullptr; }
        return *this;
    }

    T* Get() const { return m_ptr; }
    T** GetAddressOf() { return &m_ptr; }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    void Reset(T* ptr = nullptr) {
        if (m_ptr) m_ptr->Release();
        m_ptr = ptr;
    }
    T* Detach() { T* p = m_ptr; m_ptr = nullptr; return p; }

private:
    T* m_ptr;
};

// ---- Find device by substring match in friendly name ----
ComPtr<IMMDevice> FindAudioDevice(
    IMMDeviceEnumerator* enumerator,
    EDataFlow flow,
    const wchar_t* nameSubstring);

// ---- Diagnostic: log every ACTIVE render endpoint's friendly name ----
// Used when the AES67Driver endpoint cannot be found, so the user can see what
// endpoints actually exist / how the driver endpoint is named / whether it is
// enabled (a disabled endpoint won't appear in DEVICE_STATE_ACTIVE at all).
void LogRenderEndpoints(IMMDeviceEnumerator* enumerator);

// ---- Convert a UTF-16 device/path string to a log-safe narrow string ----
// The Logger uses the narrow CRT (vsnprintf); %ls truncates on the first
// non-ASCII char under the default C locale. Use this + %s for any wide string
// that may contain localized text (e.g. Chinese device names).
#include <string>
std::string WideToNarrow(const wchar_t* w);

// ---- Core WASAPI client wrapper ----
class WasapiClient {
public:
    WasapiClient() = default;
    ~WasapiClient();

    // Non-copyable, movable
    WasapiClient(const WasapiClient&) = delete;
    WasapiClient& operator=(const WasapiClient&) = delete;
    WasapiClient(WasapiClient&&) = default;
    WasapiClient& operator=(WasapiClient&&) = default;

    // Initialize for loopback capture from render endpoint
    HRESULT InitLoopback(IMMDevice* device, const AudioConfig& config);

    // Initialize for normal render to capture endpoint
    HRESULT InitRender(IMMDevice* device, const AudioConfig& config);

    HRESULT Start();
    HRESULT Stop();
    HRESULT Reset();

    // Set event handle for event-driven capture. hEvent must be manual-reset or auto-reset.
    // Call BEFORE Start(). Only valid after InitLoopback/InitRender.
    HRESULT SetEventHandle(HANDLE hEvent);

    // Accessors
    IAudioClient*        GetClient()  const { return m_client.Get(); }
    IAudioCaptureClient* GetCapture() const { return m_capture.Get(); }
    IAudioRenderClient*  GetRender()  const { return m_render.Get(); }
    UINT32 GetBufferFrames() const { return m_bufferFrames; }
    UINT32 GetPeriodFrames() const { return m_periodFrames; }
    bool   IsEventDriven() const { return m_eventDriven; }
    const WAVEFORMATEXTENSIBLE& GetWaveFormat() const { return m_wf; }

private:
    ComPtr<IAudioClient>        m_client;
    ComPtr<IAudioCaptureClient> m_capture;
    ComPtr<IAudioRenderClient>  m_render;
    WAVEFORMATEXTENSIBLE        m_wf = {};
    UINT32                      m_bufferFrames  = 0;
    UINT32                      m_periodFrames  = 0;
    bool                        m_eventDriven   = false;
};
