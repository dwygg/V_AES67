#ifndef _AES67_SAVEDATA_H
#define _AES67_SAVEDATA_H

#pragma warning(push)
#pragma warning(disable:4201) // nameless struct/union
#pragma warning(disable:4214) // bit field types other than int
#pragma warning(pop)

#include <ntddk.h>

//-----------------------------------------------------------------------------
//  Forward declaration
//-----------------------------------------------------------------------------
class CSaveData;
typedef CSaveData *PCSaveData;

//-----------------------------------------------------------------------------
//  Structs
//-----------------------------------------------------------------------------

// Parameter to workitem.
#include <pshpack1.h>
typedef struct _SAVEWORKER_PARAM {
    PIO_WORKITEM     WorkItem;
    PCSaveData       pSaveData;
    KEVENT           EventDone;
} SAVEWORKER_PARAM;
typedef SAVEWORKER_PARAM *PSAVEWORKER_PARAM;
#include <poppack.h>

//-----------------------------------------------------------------------------
//  Classes
//-----------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////////
// CSaveData
//
//   AES67 rework note (P1):
//   Historically (Scream heritage) this class ran a WSK UDP multicast sender
//   directly from the kernel. That network path has been removed. The kernel
//   driver must stay thin: it only accepts PCM frames from the audio stack and
//   (in a later phase) hands them to user space via IOCTL + shared memory.
//
//   For now this is a "dummy sink": it accepts WriteData() calls and drops the
//   samples. The public interface is intentionally preserved so minstream.cpp /
//   common.cpp do not need to change when the real hand-off is wired up.
//
//   TODO(P9): replace the dummy sink with the ring-buffer -> shared-memory
//             hand-off that the user-space engine drains via IOCTL.
//
class CSaveData {
protected:
    static PDEVICE_OBJECT       m_pDeviceObject;
    static PSAVEWORKER_PARAM    m_pWorkItem;

    BOOL                        m_fWriteDisabled;

    BYTE                        m_bSamplingFreqMarker;
    BYTE                        m_bBitsPerSampleMarker;
    BYTE                        m_bChannels;
    WORD                        m_wChannelMask;

public:
    CSaveData();
    ~CSaveData();

    NTSTATUS                    Initialize(DWORD nSamplesPerSec, WORD wBitsPerSample, WORD nChannels, DWORD dwChannelMask);
    void                        Disable(BOOL fDisable);

    static void                 DestroyWorkItems(void);
    void                        WaitAllWorkItems(void);

    static NTSTATUS             SetDeviceObject(IN PDEVICE_OBJECT DeviceObject);
    static PDEVICE_OBJECT       GetDeviceObject(void);

    void                        WriteData(IN PBYTE pBuffer, IN ULONG ulByteCount);
};
typedef CSaveData *PCSaveData;

#endif
