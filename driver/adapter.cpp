/*++
Copyright (c) 1997-2000  Microsoft Corporation All Rights Reserved

Module Name:
    adapter.cpp

Abstract:
    Setup and miniport installation.  No resources are used by msvad.
--*/

#pragma warning (disable : 4127)

//
// All the GUIDS for all the miniports end up in this object.
//
#define PUT_GUIDS_HERE

#include "aes67driver.h"
#include "common.h"

//-----------------------------------------------------------------------------
// Defines                                                                    
//-----------------------------------------------------------------------------
// BUGBUG set this to number of miniports
#define MAX_MINIPORTS 3     // Number of maximum miniports.

//-----------------------------------------------------------------------------
// Externals
//-----------------------------------------------------------------------------
NTSTATUS CreateMiniportWaveCyclicAES67(OUT PUNKNOWN *, IN  REFCLSID, IN  PUNKNOWN, IN  POOL_TYPE);
NTSTATUS CreateMiniportTopologyAES67(OUT PUNKNOWN *, IN  REFCLSID, IN  PUNKNOWN, IN  POOL_TYPE);

// ---- 共享内存 ----
// TODO(P9): 这块非分页缓冲目前是"死缓冲"——已分配、清零、可通过 IOCTL 返回物理
//           地址，但内核侧尚无任何代码往里写音频。P9 打通 IOCTL + 共享内存主动脉
//           时，CSaveData::WriteData() 的 PCM 帧会落到这里，供用户态引擎映射读取。
static PVOID g_SharedBuffer = NULL;       // 非分页共享内存
static ULONG g_SharedBufferSize = 0x10000; // 64KB (4x 10ms @48kHz 2ch L24)

// TODO(P9): 预留给"用户态发现内核 IOCTL 接口"的符号链接。目前只声明未使用——
//           P9 打通主动脉时，用 IoCreateSymbolicLink / IoRegisterDeviceInterface
//           把 GUID_AES67_IOCTL_INTERFACE 暴露出去，用户态引擎才能打开设备发 IOCTL。
UNICODE_STRING g_AES67SymbolicLink = {0};

DWORD g_silenceThreshold;
DWORD g_AES67DriverVersion;

//-----------------------------------------------------------------------------
// Referenced forward.
//-----------------------------------------------------------------------------
DRIVER_ADD_DEVICE AddDevice;

NTSTATUS StartDevice(IN PDEVICE_OBJECT, IN PIRP, IN PRESOURCELIST);

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

#pragma code_seg("INIT")
__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS
GetRegistrySettings(
    _In_ PUNICODE_STRING RegistryPath
)
/*++

Routine Description:

    Initialize Driver Framework settings from the driver
    specific registry settings under

    \REGISTRY\MACHINE\SYSTEM\ControlSetxxx\Services\<driver>\Options

Arguments:

    RegistryPath - Registry path passed to DriverEntry

Returns:

    NTSTATUS - SUCCESS if able to configure the framework

--*/

{
    NTSTATUS            ntStatus;
    UNICODE_STRING      parametersPath;

    // NOTE (P1): all the network-related settings (UnicastIPv4/Port, source
    // address, DSCP, TTL, UseIVSHMEM) belonged to the removed kernel WSK sender.
    // The kernel driver no longer does any network I/O, so they are gone. Any
    // AES67 transport configuration lives in the user-space engine now.
    DWORD               AES67DriverVersion = 0;
    DWORD               silenceThreshold = 0;

    RTL_QUERY_REGISTRY_TABLE paramTable[] = {
        { NULL,   RTL_QUERY_REGISTRY_DIRECT, L"Version", &AES67DriverVersion, REG_NONE,  NULL, 0 },
        { NULL,   RTL_QUERY_REGISTRY_DIRECT, L"SilenceThreshold", &silenceThreshold, REG_NONE,  NULL, 0 },
        { NULL,   0,                         NULL,           NULL,         0,         NULL, 0 }
    };


    DPF(D_TERSE, ("[GetRegistrySettings]"));

    PAGED_CODE();

    RtlInitUnicodeString(&parametersPath, NULL);

    parametersPath.MaximumLength =
        RegistryPath->Length + sizeof(L"\\Options") + sizeof(WCHAR);

    parametersPath.Buffer = (PWCH)ExAllocatePoolWithTag(NonPagedPool, parametersPath.MaximumLength, AES67_POOLTAG);
    if (parametersPath.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(parametersPath.Buffer, parametersPath.MaximumLength);

    RtlAppendUnicodeToString(&parametersPath, RegistryPath->Buffer);
    RtlAppendUnicodeToString(&parametersPath, L"\\Options");

    ntStatus = RtlQueryRegistryValues(
        RTL_REGISTRY_ABSOLUTE | RTL_REGISTRY_OPTIONAL,
        parametersPath.Buffer,
        &paramTable[0],
        NULL,
        NULL
    );

    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_VERBOSE, ("RtlQueryRegistryValues failed, using default values, 0x%x", ntStatus));
        // Don't return error because we will operate with default values.
    }

    if (silenceThreshold > 0) {
        g_silenceThreshold = silenceThreshold;
    }
    else {
        g_silenceThreshold = 0;
    }

    if (AES67DriverVersion > 0) {
        g_AES67DriverVersion = AES67DriverVersion;
    }
    else {
        g_AES67DriverVersion = 0;
    }

    ExFreePool(parametersPath.Buffer);

    return STATUS_SUCCESS;
}



// ---- 独立 IOCTL 设备 (P2 重构) ----
//
// P2 前：DriverEntry 里全局 hook MajorFunction[IRP_MJ_DEVICE_CONTROL],
// g_PortClsDeviceControl 保存时机依赖与 PcInitializeAdapterDriver 的顺序
// → 顺序错 → 丢失 PortCls handler → audiodg 静默失败。
//
// P2 后：在 dispatch handler 里按设备对象分流 — 请求发到 \\.\AES67IOCTL
// 就处理自定义 IOCTL，否则转发给 PortCls。DeviceObject 检查保证即使
// PortCls handler 保存对了，也不会误拦截其他设备的 IOCTL。
//
// ╔══ WARNING: DriverEntry 保存顺序依赖 ═══════════════════════════╗
// ║  g_PortClsDeviceControl 必须在 PcInitializeAdapterDriver()     ║
// ║  **之后** 保存（见 DriverEntry 末尾）。顺序反了 → 拿到的不是  ║
// ║  PortCls handler → audiodg IOCTL 全被转错 → 端点消失。        ║
// ║  排查：检查调试日志 [DriverEntry] PortCls handler= 的地址。    ║
// ╚═════════════════════════════════════════════════════════════════╝
#pragma code_seg("PAGE")

// 共享内存描述结构（通过 IOCTL GET_BUFFER 返回给用户态）
typedef struct _AES67_BUFFER_INFO {
    ULONG64 PhysicalAddress;    // 物理地址（用户态可映射）
    ULONG   BufferSize;         // 缓冲区大小（字节）
    ULONG   ReadOffset;         // 当前读指针偏移
    ULONG   WriteOffset;        // 当前写指针偏移
    ULONG   Channels;           // 声道数
    ULONG   SampleRate;         // 采样率
} AES67_BUFFER_INFO;

// PortCls 原 handler（运行时分流用）和独立 IOCTL 设备
static PDRIVER_DISPATCH g_PortClsDeviceControl = NULL;
static PDEVICE_OBJECT     g_IoctlDevice = NULL;

static NTSTATUS AES67DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PAGED_CODE();

    // 不是发到 \\.\AES67IOCTL 的请求 → 原样交给 PortCls
    if (DeviceObject != g_IoctlDevice) {
        return g_PortClsDeviceControl(DeviceObject, Irp);
    }
    // 确保 g_IoctlDevice 不为 NULL（StartDevice 里已创建）。如果走到这里且为
    // NULL 说明 StartDevice 未调用或失败了 → DeviceObject 检查会被跳过，
    // 所有 IOCTL 都会走到下面自定义处理。这个条件永远不应触发。
    if (!g_IoctlDevice) {
        return g_PortClsDeviceControl(DeviceObject, Irp);
    }

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;

    if (code == IOCTL_AES67_GET_BUFFER) {
        if (!g_SharedBuffer || stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(AES67_BUFFER_INFO)) {
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            AES67_BUFFER_INFO info = {0};
            info.PhysicalAddress = 0;  // TODO(P9): MmGetPhysicalAddress(g_SharedBuffer).QuadPart
            info.BufferSize = g_SharedBufferSize;
            info.Channels = 2;
            info.SampleRate = 48000;
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &info, sizeof(info));
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = sizeof(AES67_BUFFER_INFO);
        }
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Irp->IoStatus.Status;
    }

    if (code == IOCTL_AES67_GET_POSITION || code == IOCTL_AES67_SET_FORMAT) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    // 不认识的 IOCTL → PortCls
    return g_PortClsDeviceControl(DeviceObject, Irp);
}
#pragma code_seg()

// DriverEntry runs exactly once and may stay in the discardable INIT segment.
// P2 重构：在 PcInitializeAdapterDriver *之后* 保存 PortCls handler 并 hook。
// handler 内部按 DeviceObject 分流，发到 \\.\AES67IOCTL 的才处理自定义 IOCTL。
#pragma code_seg("INIT")
extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(
    IN  PDRIVER_OBJECT          DriverObject,
    IN  PUNICODE_STRING         RegistryPathName
)
{
    NTSTATUS ntStatus;
    DPF(D_TERSE, ("[DriverEntry]"));
    GetRegistrySettings(RegistryPathName);

    ntStatus = PcInitializeAdapterDriver(DriverObject, RegistryPathName, (PDRIVER_ADD_DEVICE)AddDevice);

    // ╔══ WARNING ═══════════════════════════════════════════════════╗
    // ║  下面两行的顺序不可改。g_PortClsDeviceControl 必须在         ║
    // ║  PcInitializeAdapterDriver **之后** 保存，否则拿不到        ║
    // ║  PortCls 的真实 handler → 端点不会出现在系统声音设置。     ║
    // ║  详见 AES67DeviceControl 上方同名 WARNING 框。              ║
    // ╚══════════════════════════════════════════════════════════════╝
    g_PortClsDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AES67DeviceControl;
    DPF(D_TERSE, ("[DriverEntry] PortCls handler=%p, our hook=%p",
        g_PortClsDeviceControl, AES67DeviceControl));

    return ntStatus;
} // DriverEntry
#pragma code_seg()

#pragma code_seg("PAGE")
//=============================================================================
NTSTATUS AddDevice ( 
    IN  PDRIVER_OBJECT          DriverObject,
    IN  PDEVICE_OBJECT          PhysicalDeviceObject 
)
/*++
Routine Description:
  The Plug & Play subsystem is handing us a brand new PDO, for which we
  (by means of INF registration) have been asked to provide a driver.

  We need to determine if we need to be in the driver stack for the device.
  Create a function device object to attach to the stack
  Initialize that device object
  Return status success.

  All audio adapter drivers can use this code without change.
  Set MAX_MINIPORTS depending on the number of miniports that the driver
  uses.

Arguments:
  DriverObject - pointer to a driver object
  PhysicalDeviceObject -  pointer to a device object created by the
                            underlying bus driver.

Return Value:
  NT status code.
--*/
{
    PAGED_CODE();

    NTSTATUS ntStatus;

    DPF(D_TERSE, ("[AddDevice]"));

    // disable prefast warning 28152 because 
    // DO_DEVICE_INITIALIZING is cleared in PcAddAdapterDevice
#pragma warning(disable:28152)

    // Tell the class driver to add the device.
    ntStatus = PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, PCPFNSTARTDEVICE(StartDevice), MAX_MINIPORTS, 0);

    return ntStatus;
} // AddDevice

//=============================================================================
NTSTATUS InstallSubdevice( 
    __in        PDEVICE_OBJECT    DeviceObject,
    __in        PIRP              Irp,
    __in        PWSTR             Name,
    __in        REFGUID           PortClassId,
    __in        REFGUID           MiniportClassId,
    __in_opt    PFNCREATEINSTANCE MiniportCreate,
    __in_opt    PUNKNOWN          UnknownAdapter,
    __in_opt    PRESOURCELIST     ResourceList,
    __in        REFGUID           PortInterfaceId,
    __out_opt   PUNKNOWN *        OutPortInterface,
    __out_opt   PUNKNOWN *        OutPortUnknown
)
/*++
Routine Description:
    This function creates and registers a subdevice consisting of a port       
    driver, a minport driver and a set of resources bound together.  It will   
    also optionally place a pointer to an interface on the port driver in a    
    specified location before initializing the port driver.  This is done so   
    that a common ISR can have access to the port driver during 
    initialization, when the ISR might fire.                                   

Arguments:
    DeviceObject - pointer to the driver object
    Irp - pointer to the irp object.
    Name - name of the miniport. Passes to PcRegisterSubDevice
    PortClassId - port class id. Passed to PcNewPort.
    MiniportClassId - miniport class id. Passed to PcNewMiniport.
    MiniportCreate - pointer to a miniport creation function. If NULL, 
                     PcNewMiniport is used.
    UnknownAdapter - pointer to the adapter object. 
                     Used for initializing the port.
    ResourceList - pointer to the resource list.
    PortInterfaceId - GUID that represents the port interface.
    OutPortInterface - pointer to store the port interface
    OutPortUnknown - pointer to store the unknown port interface.

Return Value:
    NT status code.
--*/
{
    PAGED_CODE();

    ASSERT(DeviceObject);
    ASSERT(Irp);
    ASSERT(Name);

    NTSTATUS ntStatus;
    PPORT    port = NULL;
    PUNKNOWN miniport = NULL;
     
    DPF_ENTER(("[InstallSubDevice %S]", Name));

    // Create the port driver object
    ntStatus = PcNewPort(&port, PortClassId);

    // Create the miniport object
    if (NT_SUCCESS(ntStatus)) {
        if (MiniportCreate) {
            ntStatus = MiniportCreate(&miniport, MiniportClassId, NULL, NonPagedPool  );
        } else {
            ntStatus = PcNewMiniport((PMINIPORT *) &miniport, MiniportClassId);
        }
    }

    // Init the port driver and miniport in one go.
    if (NT_SUCCESS(ntStatus)) {
        ntStatus = port->Init(DeviceObject, Irp, miniport, UnknownAdapter, ResourceList);

        if (NT_SUCCESS(ntStatus)) {
            // Register the subdevice (port/miniport combination).
            ntStatus = PcRegisterSubdevice(DeviceObject, Name, port);
        }

        // We don't need the miniport any more.  Either the port has it,
        // or we've failed, and it should be deleted.
        miniport->Release();
    }

    // Deposit the port interfaces if it's needed.
    if (NT_SUCCESS(ntStatus)) {
        if (OutPortUnknown) {
            ntStatus = port->QueryInterface(IID_IUnknown, (PVOID *)OutPortUnknown);
        }

        if (OutPortInterface) {
            ntStatus = port->QueryInterface(PortInterfaceId, (PVOID *) OutPortInterface);
        }
    }

    if (port) {
        port->Release();
    }

    return ntStatus;
} // InstallSubDevice

//=============================================================================
NTSTATUS StartDevice(
    IN  PDEVICE_OBJECT          DeviceObject,     
    IN  PIRP                    Irp,              
    IN  PRESOURCELIST           ResourceList      
)  
/*++
Routine Description:
  This function is called by the operating system when the device is 
  started.
  It is responsible for starting the miniports.  This code is specific to    
  the adapter because it calls out miniports for functions that are specific 
  to the adapter.                                                            

Arguments:
  DeviceObject - pointer to the driver object
  Irp - pointer to the irp 
  ResourceList - pointer to the resource list assigned by PnP manager

Return Value:
  NT status code.
--*/
{
    UNREFERENCED_PARAMETER(ResourceList);

    PAGED_CODE();
    
    ASSERT(DeviceObject);
    ASSERT(Irp);
    ASSERT(ResourceList);

    NTSTATUS       ntStatus        = STATUS_SUCCESS;
    PUNKNOWN       unknownTopology = NULL;
    PUNKNOWN       unknownWave     = NULL;
    PADAPTERCOMMON pAdapterCommon  = NULL;
    PUNKNOWN       pUnknownCommon  = NULL;

    DPF_ENTER(("[StartDevice]"));

    // create a new adapter common object
    ntStatus = NewAdapterCommon(&pUnknownCommon, IID_IAdapterCommon, NULL, NonPagedPool);
    if (NT_SUCCESS(ntStatus)) {
        ntStatus = pUnknownCommon->QueryInterface(IID_IAdapterCommon, (PVOID *) &pAdapterCommon);
        if (NT_SUCCESS(ntStatus)) {
            ntStatus = pAdapterCommon->Init(DeviceObject);
            if (NT_SUCCESS(ntStatus)) {
                // register with PortCls for power-management services
                ntStatus = PcRegisterAdapterPowerManagement(PUNKNOWN(pAdapterCommon), DeviceObject);
            }
        }
    }

    // install AES67 topology miniport.
    if (NT_SUCCESS(ntStatus)) {
        ntStatus = InstallSubdevice(DeviceObject, Irp, L"Topology", CLSID_PortTopology, CLSID_PortTopology, CreateMiniportTopologyAES67, pAdapterCommon, NULL, IID_IPortTopology, NULL, &unknownTopology);
    }

    // install AES67 wavecyclic miniport.
    if (NT_SUCCESS(ntStatus)) {
        ntStatus = InstallSubdevice(DeviceObject, Irp, L"Wave", CLSID_PortWaveCyclic, CLSID_PortWaveCyclic, CreateMiniportWaveCyclicAES67, pAdapterCommon, NULL, IID_IPortWaveCyclic, pAdapterCommon->WavePortDriverDest(), &unknownWave);
    }

    if (unknownTopology && unknownWave) {
        // register wave <=> topology connections
        // This will connect bridge pins of wavecyclic and topology
        // miniports.
        if ((TopologyPhysicalConnections.ulTopologyOut != (ULONG)-1) && (TopologyPhysicalConnections.ulWaveIn != (ULONG)-1)) {
            ntStatus = PcRegisterPhysicalConnection(DeviceObject, unknownTopology, TopologyPhysicalConnections.ulTopologyOut, unknownWave, TopologyPhysicalConnections.ulWaveIn);
        }

        if (NT_SUCCESS(ntStatus)) {
            if ((TopologyPhysicalConnections.ulWaveOut != (ULONG)-1) && (TopologyPhysicalConnections.ulTopologyIn != (ULONG)-1)) {
                ntStatus = PcRegisterPhysicalConnection(DeviceObject, unknownWave, TopologyPhysicalConnections.ulWaveOut, unknownTopology, TopologyPhysicalConnections.ulTopologyIn);
            }
        }
    }

    // Release the adapter common object.  It either has other references,
    // or we need to delete it anyway.
    if (pAdapterCommon) {
        pAdapterCommon->Release();
    }

    if (pUnknownCommon) {
        pUnknownCommon->Release();
    }
    
    if (unknownTopology) {
        unknownTopology->Release();
    }

    if (unknownWave) {
        unknownWave->Release();
    }

    // 分配共享内存（非分页，用户态可通过 IOCTL 映射访问）
    if (NT_SUCCESS(ntStatus) && !g_SharedBuffer) {
        g_SharedBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED,
            g_SharedBufferSize, AES67_POOLTAG);
        if (g_SharedBuffer) {
            RtlZeroMemory(g_SharedBuffer, g_SharedBufferSize);
            DPF(D_TERSE, ("[StartDevice] Shared buffer allocated: %p, %lu bytes",
                g_SharedBuffer, g_SharedBufferSize));
        }
    }

    // P2 重构：创建独立 IOCTL 设备 \\.\AES67IOCTL。
    // 驱动只有一个 DriverObject，MajorFunction 表全局共享；在 handler 里用
    // DeviceObject == g_IoctlDevice 做运行时判断，自定义 IOCTL 才处理，
    // 其余全部转发 PortCls。
    if (NT_SUCCESS(ntStatus) && !g_IoctlDevice) {
        UNICODE_STRING devName, symLink;
        RtlInitUnicodeString(&devName, L"\\Device\\AES67IOCTL");
        RtlInitUnicodeString(&symLink, L"\\DosDevices\\AES67IOCTL");
        ntStatus = IoCreateDevice(DeviceObject->DriverObject, 0, &devName,
            FILE_DEVICE_UNKNOWN, 0, FALSE, &g_IoctlDevice);
        if (NT_SUCCESS(ntStatus)) {
            ntStatus = IoCreateSymbolicLink(&symLink, &devName);
            if (NT_SUCCESS(ntStatus)) {
                DPF(D_TERSE, ("[StartDevice] IOCTL device ready: \\\\.\\AES67IOCTL"));
            } else {
                DPF(D_TERSE, ("[StartDevice] IoCreateSymbolicLink failed: 0x%08X", ntStatus));
            }
        } else {
            DPF(D_TERSE, ("[StartDevice] IoCreateDevice for IOCTL failed: 0x%08X", ntStatus));
        }
    }

    return ntStatus;
} // StartDevice
#pragma code_seg()
