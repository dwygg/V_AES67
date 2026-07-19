#pragma warning (disable : 4127)

#include "aes67driver.h"
#include "savedata.h"

//=============================================================================
// CSaveData  (AES67 rework, P1)
//
//   The original Scream-derived implementation ran a WSK UDP multicast sender
//   from inside the kernel. That entire network path has been removed: the
//   kernel driver must stay thin and must NOT do network I/O.
//
//   This is now a "dummy sink". It keeps the exact public interface that
//   minstream.cpp / common.cpp depend on (Initialize / Disable / WriteData /
//   DestroyWorkItems / WaitAllWorkItems / SetDeviceObject / GetDeviceObject),
//   but WriteData() simply drops the PCM frames it receives.
//
//   TODO(P9): wire WriteData() into a lock-free ring buffer that is exposed to
//             the user-space engine via IOCTL + shared memory. That is where the
//             real RTP/AES67 packetisation will live (user space, not here).
//=============================================================================

#pragma code_seg("PAGE")

//=============================================================================
CSaveData::CSaveData()
    : m_fWriteDisabled(FALSE)
    , m_bSamplingFreqMarker(0)
    , m_bBitsPerSampleMarker(0)
    , m_bChannels(0)
    , m_wChannelMask(0) {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::CSaveData]"));

    // Allocate the (static) work item once. It is no longer used to drive a
    // network sender; we keep it only so WaitAllWorkItems() has a signalled
    // event to wait on and DestroyWorkItems() has something to free.
    // TODO(P9): repurpose or remove once the shared-memory hand-off lands.
    if (!m_pWorkItem) {
        m_pWorkItem = (PSAVEWORKER_PARAM)ExAllocatePoolWithTag(NonPagedPool, sizeof(SAVEWORKER_PARAM), AES67_POOLTAG);
        if (m_pWorkItem) {
            m_pWorkItem->WorkItem = NULL;
            m_pWorkItem->pSaveData = NULL;
            // Start signalled: there is never outstanding work in the dummy sink.
            KeInitializeEvent(&(m_pWorkItem->EventDone), NotificationEvent, TRUE);
        }
    }
} // CSaveData

//=============================================================================
CSaveData::~CSaveData() {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::~CSaveData]"));
    // Nothing to tear down: no socket, no ring buffer, no MDL.
} // CSaveData

//=============================================================================
void CSaveData::DestroyWorkItems(void) {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::DestroyWorkItems]"));

    if (m_pWorkItem) {
        ExFreePoolWithTag(m_pWorkItem, AES67_POOLTAG);
        m_pWorkItem = NULL;
    }
} // DestroyWorkItems

//=============================================================================
void CSaveData::Disable(BOOL fDisable) {
    PAGED_CODE();

    m_fWriteDisabled = fDisable;
} // Disable

//=============================================================================
NTSTATUS CSaveData::SetDeviceObject(IN PDEVICE_OBJECT DeviceObject) {
    PAGED_CODE();

    ASSERT(DeviceObject);

    m_pDeviceObject = DeviceObject;
    return STATUS_SUCCESS;
}

//=============================================================================
PDEVICE_OBJECT CSaveData::GetDeviceObject(void) {
    PAGED_CODE();

    return m_pDeviceObject;
}

//=============================================================================
NTSTATUS CSaveData::Initialize(DWORD nSamplesPerSec, WORD wBitsPerSample, WORD nChannels, DWORD dwChannelMask) {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::Initialize]"));

    // Retain the format markers. They cost nothing and will be handy once the
    // real hand-off to user space is implemented (TODO(P9)).
    // Only multiples of 44100 and 48000 are supported.
    m_bSamplingFreqMarker  = (BYTE)((nSamplesPerSec % 44100) ? (0 + (nSamplesPerSec / 48000)) : (128 + (nSamplesPerSec / 44100)));
    m_bBitsPerSampleMarker = (BYTE)(wBitsPerSample);
    m_bChannels            = (BYTE)nChannels;
    m_wChannelMask         = (WORD)dwChannelMask;

    return STATUS_SUCCESS;
} // Initialize

//=============================================================================
void CSaveData::WaitAllWorkItems(void) {
    PAGED_CODE();

    DPF_ENTER(("[CSaveData::WaitAllWorkItems]"));

    // The dummy sink never queues work, so EventDone stays signalled and this
    // returns immediately. Kept for interface compatibility with minstream.cpp.
    if (m_pWorkItem) {
        KeWaitForSingleObject(&(m_pWorkItem->EventDone), Executive, KernelMode, FALSE, NULL);
    }
} // WaitAllWorkItems

//=============================================================================
void CSaveData::WriteData(IN PBYTE pBuffer, IN ULONG ulByteCount) {
    UNREFERENCED_PARAMETER(pBuffer);
    UNREFERENCED_PARAMETER(ulByteCount);

    if (m_fWriteDisabled) {
        return;
    }

    // Dummy sink: drop the samples.
    // TODO(P9): push (pBuffer, ulByteCount) into a ring buffer that the
    //           user-space AES67 engine drains via IOCTL + shared memory.
} // WriteData
