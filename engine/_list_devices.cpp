#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <initguid.h>
int main() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    for (int d = 0; d < 2; d++) {
        EDataFlow flow = d == 0 ? eCapture : eRender;
        const char* dir = d == 0 ? "CAPTURE" : "RENDER";
        IMMDeviceCollection* coll;
        en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll);
        UINT count; coll->GetCount(&count);
        printf("=== %s (%u devices) ===\n", dir, count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev; coll->Item(i, &dev);
            IPropertyStore* props;
            dev->OpenPropertyStore(STGM_READ, &props);
            PROPVARIANT var; PropVariantInit(&var);
            props->GetValue(PKEY_Device_FriendlyName, &var);
            printf("  [%u] %ls\n", i, var.pwszVal ? var.pwszVal : L"(null)");
            PropVariantClear(&var); props->Release(); dev->Release();
        }
        coll->Release();
    }
    en->Release(); CoUninitialize();
}
