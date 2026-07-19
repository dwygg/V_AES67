/**
 * IOCTL 测试 — 通过 ROOT\MEDIA 找 AES67Driver 设备实例路径
 */
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "CfgMgr32.lib")

// GUID_AUDIO_RENDER = {E6327CAD-DCEC-4949-AE8A-991E976A79D2}
static const GUID GUID_AUDIO_RENDER =
    {0xE6327CAD, 0xDCEC, 0x4949, {0xAE,0x8A,0x99,0x1E,0x97,0x6A,0x79,0xD2}};

#define IOCTL_GET_BUFFER   0x22E0000
#define IOCTL_GET_POSITION 0x22E0008

int main() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(NULL, L"ROOT\\MEDIA", NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("No ROOT\\MEDIA devices\n"); return 1;
    }

    SP_DEVINFO_DATA d = { sizeof(SP_DEVINFO_DATA) };
    HANDLE hDevice = INVALID_HANDLE_VALUE;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &d); i++) {
        WCHAR desc[256], id[256];
        SetupDiGetDeviceRegistryPropertyW(devInfo, &d, SPDRP_DEVICEDESC,
            NULL, (PBYTE)desc, sizeof(desc), NULL);
        if (!wcsstr(desc, L"AES67Driver")) continue;
        wprintf(L"Found: %s\n", desc);

        SetupDiGetDeviceInstanceIdW(devInfo, &d, id, 256, NULL);

        // 通过 CM_Get_Device_Interface_List 获取音频端点路径
        ULONG len = 0;
        CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&len,
            (LPGUID)&GUID_AUDIO_RENDER, id,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS && len > 1) {
            WCHAR* list = new WCHAR[len + 1];
            cr = CM_Get_Device_Interface_ListW(
                (LPGUID)&GUID_AUDIO_RENDER,
                id, list, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
            if (cr == CR_SUCCESS && list[0]) {
                wprintf(L"Path: [%s]\n", list);
                // 尝试不同的打开方式
                hDevice = CreateFileW(list,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
                if (hDevice == INVALID_HANDLE_VALUE) {
                    hDevice = CreateFileW(list,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
                }
                if (hDevice != INVALID_HANDLE_VALUE) break;
                wprintf(L"  Open failed: %lu\n", GetLastError());
            }
            delete[] list;
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("Cannot open AES67Driver audio device\nErr: %lu\n", GetLastError());
        return 1;
    }
    printf("Device opened OK\n");

    DWORD ret; BYTE buf[256] = {0};
    printf("GET_BUFFER:   %s\n",
        DeviceIoControl(hDevice, IOCTL_GET_BUFFER, NULL,0, buf,sizeof(buf),&ret,NULL)?"OK":"FAIL");
    printf("GET_POSITION: %s\n",
        DeviceIoControl(hDevice, IOCTL_GET_POSITION, NULL,0, buf,sizeof(buf),&ret,NULL)?"OK":"FAIL");

    CloseHandle(hDevice);
    return 0;
}
