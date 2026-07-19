#ifndef _AES67_H_
#define _AES67_H_

#include <portcls.h>
#pragma warning(disable:4595)
#include <stdunk.h>
#include <ksdebug.h>
#include "kshelper.h"

//=============================================================================
// Defines
//=============================================================================

// Name Guid
// {49C58C39-D5EB-42C1-ABCD-EB75422ACF38}
#define STATIC_NAME_AES67_SIMPLE 0x49C58C39, 0xD5EB, 0x42C1, 0xab, 0xcd, 0xeb, 0x75, 0x42, 0x2a, 0xcf, 0x38
DEFINE_GUIDSTRUCT("49C58C39-D5EB-42C1-ABCD-EB75422ACF38", NAME_AES67_SIMPLE);
#define NAME_AES67_SIMPLE DEFINE_GUIDNAMED(NAME_AES67_SIMPLE)

// Version number. Revision numbers are specified for each sample.
#define AES67_VERSION               1

// Revision number.
#define AES67_REVISION              0

// Product Id
// {5B722BF8-F0AB-47ee-B9C8-8D61D31375A1}
#define STATIC_PID_AES67 0x5b722bf8, 0xf0ab, 0x47ee, 0xb9, 0xc8, 0x8d, 0x61, 0xd3, 0x13, 0x75, 0xa1
DEFINE_GUIDSTRUCT("5B722BF8-F0AB-47ee-B9C8-8D61D31375A1", PID_AES67);
#define PID_AES67 DEFINE_GUIDNAMED(PID_AES67)

// Pool tag used for AES67 allocations
#define AES67_POOLTAG               'DVSM'

// Debug module name
#define STR_MODULENAME              "AES67Driver: "

// Debug utility macros
#define D_FUNC                      4
#define D_BLAB                      DEBUGLVL_BLAB
#define D_VERBOSE                   DEBUGLVL_VERBOSE        
#define D_TERSE                     DEBUGLVL_TERSE          
#define D_ERROR                     DEBUGLVL_ERROR          
#define DPF                         _DbgPrintF
#define DPF_ENTER(x)                DPF(D_FUNC, x)

// Channel orientation
#define CHAN_LEFT                   0
#define CHAN_RIGHT                  1

// ---- IOCTL 自定义控制码 ----
// CTL_CODE(DeviceType, Function, Method, Access)
// FILE_DEVICE_UNKNOWN=0x0022, METHOD_NEITHER=3, FILE_ANY_ACCESS=0
#define IOCTL_AES67_GET_BUFFER      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_AES67_SET_FORMAT      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_AES67_GET_POSITION    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_NEITHER, FILE_ANY_ACCESS)

// 用户态与驱动通信的自定义设备接口 GUID
// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
#define STATIC_GUID_AES67_IOCTL_INTERFACE 0xa1b2c3d4, 0xe5f6, 0x7890, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90
DEFINE_GUIDSTRUCT("A1B2C3D4-E5F6-7890-ABCD-EF1234567890", GUID_AES67_IOCTL_INTERFACE);
#define GUID_AES67_IOCTL_INTERFACE DEFINE_GUIDNAMED(GUID_AES67_IOCTL_INTERFACE)
#define CHAN_MASTER                 (-1)

// Dma Settings.
#define DMA_BUFFER_SIZE             0x16000

#define KSPROPERTY_TYPE_ALL         KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET

// Pin properties.
#define MAX_OUTPUT_STREAMS          0       // Number of capture streams.
#define MAX_INPUT_STREAMS           1       // Number of render streams.
#define MAX_TOTAL_STREAMS           MAX_OUTPUT_STREAMS + MAX_INPUT_STREAMS

// PCM Info
#define MIN_CHANNELS                1        // Min Channels.
#define MAX_CHANNELS_PCM            8        // Max Channels.
#define MIN_BITS_PER_SAMPLE_PCM     16       // Min Bits Per Sample
#define MAX_BITS_PER_SAMPLE_PCM     32       // Max Bits Per Sample
#define MIN_SAMPLE_RATE             44100    // Min Sample Rate
#define MAX_SAMPLE_RATE             192000   // Max Sample Rate

#define DEV_SPECIFIC_VT_BOOL        9
#define DEV_SPECIFIC_VT_I4          10
#define DEV_SPECIFIC_VT_UI4         11

//=============================================================================
// Typedefs
//=============================================================================

// Connection table for registering topology/wave bridge connection
typedef struct _PHYSICALCONNECTIONTABLE {
    ULONG       ulTopologyIn;
    ULONG       ulTopologyOut;
    ULONG       ulWaveIn;
    ULONG       ulWaveOut;
} PHYSICALCONNECTIONTABLE, *PPHYSICALCONNECTIONTABLE;

//=============================================================================
// Enums
//=============================================================================

// Wave pins
enum {
    KSPIN_WAVE_RENDER_SINK = 0,
    KSPIN_WAVE_RENDER_SOURCE
};

// Wave Topology nodes.
enum {
    KSNODE_WAVE_DAC = 0
};

// topology pins.
enum {
    KSPIN_TOPO_WAVEOUT_SOURCE = 0,
    KSPIN_TOPO_LINEOUT_DEST
};

// topology nodes.
enum {
    KSNODE_TOPO_WAVEOUT_VOLUME = 0,
    KSNODE_TOPO_WAVEOUT_MUTE,
    KSNODE_TOPO_LINEOUT_MIX,
    KSNODE_TOPO_LINEOUT_VOLUME
};

//=============================================================================
// Externs
//=============================================================================

// Physical connection table. Defined in mintopo.cpp for each sample
extern PHYSICALCONNECTIONTABLE TopologyPhysicalConnections;

// Generic topology handler
extern NTSTATUS PropertyHandler_Topology(IN PPCPROPERTY_REQUEST PropertyRequest);

// Generic wave port handler
extern NTSTATUS PropertyHandler_Wave(IN PPCPROPERTY_REQUEST PropertyRequest);

// Default WaveFilter automation table.
// Handles the GeneralComponentId request.
extern NTSTATUS PropertyHandler_WaveFilter(IN PPCPROPERTY_REQUEST PropertyRequest);

extern PCHAR g_UnicastIPv4;
extern DWORD g_UnicastPort;
extern UINT8 g_UseIVSHMEM;
extern PCHAR g_UnicastSrcIPv4;
extern DWORD g_UnicastSrcPort;
extern DWORD g_DSCP;
extern DWORD g_TTL;
extern DWORD g_AES67DriverVersion;
extern DWORD g_silenceThreshold;

#endif
