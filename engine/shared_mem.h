#pragma once
#include <windows.h>
#include <cstdio>
#include "logger.h"

// P9: User-mode bridge to the kernel shared-memory ring buffer.
// Opens \\.\AES67IOCTL, sends captured audio via IOCTL_AES67_WRITE_CAPTURE.
//
// The IOCTL copies data into the driver's non-paged ring buffer (g_SharedBuffer).
// CopyFrom() in the miniport then reads it to fill the capture DMA buffer.
//
// Keep it simple: each Write() is one DeviceIoControl call. Latency overhead
// of the kernel transition is negligible at 10ms WASAPI period granularity.

#define IOCTL_AES67_GET_BUFFER     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AES67_WRITE_CAPTURE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

struct SharedMemBridge {
    HANDLE hDevice = INVALID_HANDLE_VALUE;

    bool Open() {
        hDevice = CreateFileW(L"\\\\.\\AES67IOCTL",
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (hDevice == INVALID_HANDLE_VALUE) {
            Logger::Instance().Error("SharedMemBridge: cannot open \\\\.\\AES67IOCTL (err=%lu)", GetLastError());
            return false;
        }
        Logger::Instance().Info("SharedMemBridge: IOCTL device opened");
        return true;
    }

    void Close() {
        if (hDevice != INVALID_HANDLE_VALUE) {
            CloseHandle(hDevice);
            hDevice = INVALID_HANDLE_VALUE;
        }
    }

    // Write captured audio data to the driver's ring buffer.
    // Returns bytes written, 0 on failure.
    DWORD Write(const BYTE* data, DWORD bytes) {
        if (hDevice == INVALID_HANDLE_VALUE || !data || bytes == 0) return 0;
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(hDevice, IOCTL_AES67_WRITE_CAPTURE,
            (LPVOID)data, bytes,
            nullptr, 0, &ret, nullptr);
        if (!ok) {
            static DWORD lastLog = 0;
            DWORD now = GetTickCount();
            if (now - lastLog > 5000) {
                Logger::Instance().Warn("SharedMemBridge: IOCTL_WRITE_CAPTURE failed (err=%lu)", GetLastError());
                lastLog = now;
            }
            return 0;
        }
        return ret;
    }

    bool IsOpen() const { return hDevice != INVALID_HANDLE_VALUE; }
};
