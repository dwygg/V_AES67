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



//=============================================================================
// FIX (P1): The custom IOCTL dispatch below is a RUNTIME callback — it is
// registered into DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] and gets
// invoked long after DriverEntry returns. It therefore MUST NOT live in the
// "INIT" code segment: INIT is marked discardable (/SECTION:"INIT,d") and the
// system frees it once DriverEntry completes. Executing a handler out of freed
// INIT memory triggers ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY (bugcheck 0xFC)
// on the first IOCTL. Keep it in the pageable "PAGE" segment instead — IOCTL
// dispatch runs at PASSIVE_LEVEL and this handler touches nothing non-pageable.
#pragma code_seg("PAGE")
// ---- 自定义 IOCTL 分发 ----
//
// TODO(P9): 这是"内核↔用户态主动脉"的骨架，目前还是空壳：
//   - IOCTL_AES67_GET_BUFFER 能返回 g_SharedBuffer 的物理地址，但那块缓冲当前
//     没有任何音频写入（见 §共享内存 的 TODO(P9)），映射后读到的是全零。
//   - IOCTL_AES67_GET_POSITION / IOCTL_AES67_SET_FORMAT 目前直接返回成功，
//     没有真正的读写指针推进和格式协商。
//   P9 阶段：把 CSaveData::WriteData() 的 PCM 帧写入 g_SharedBuffer 环形缓冲，
//   在此维护 ReadOffset/WriteOffset，并让 SET_FORMAT 真正生效。
static PDRIVER_DISPATCH g_PortClsDeviceControl;  // 保存 PortCls 原 handler

// 共享内存描述结构（通过 IOCTL GET_BUFFER 返回给用户态）
typedef struct _AES67_BUFFER_INFO {
    ULONG64 PhysicalAddress;    // 物理地址（用户态可映射）
    ULONG   BufferSize;         // 缓冲区大小（字节）
    ULONG   ReadOffset;         // 当前读指针偏移
    ULONG   WriteOffset;        // 当前写指针偏移
    ULONG   Channels;           // 声道数
    ULONG   SampleRate;         // 采样率
} AES67_BUFFER_INFO;

// TODO(P9): this hooks the GLOBAL IRP_MJ_DEVICE_CONTROL, so it also intercepts
//   PortCls/audio-stack IOCTLs before forwarding to g_PortClsDeviceControl.
//   Most audio IOCTLs come in at PASSIVE_LEVEL, but if any arrives at
//   DISPATCH_LEVEL a pageable handler could fault at high IRQL (bugcheck 0xA).
//   If a non-0xFC bugcheck shows up after this fix, drop PAGED_CODE() and move
//   this routine to a non-paged segment. The proper P9 fix is to stop hooking
//   the global dispatch entry and expose the IOCTL via a dedicated interface.
static NTSTATUS AES67DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PAGED_CODE();   // IOCTL dispatch runs at PASSIVE_LEVEL; handler is pageable.

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;

    if (code == IOCTL_AES67_GET_BUFFER) {
        if (!g_SharedBuffer || stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(AES67_BUFFER_INFO)) {
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            AES67_BUFFER_INFO info = {0};
            // TODO(P9): 真正实现共享内存通路后，把 g_SharedBuffer 的物理地址填进去
            //   (MmGetPhysicalAddress(g_SharedBuffer).QuadPart)。当前 g_SharedBuffer
            //   恒为 NULL(见 §共享内存 TODO(P9))，上面的 if 已保证走不到这里，
            //   故此处留 0，避免为一个 P9 才生效的桩去引 ntddk.h 造成头文件冲突。
            info.PhysicalAddress = 0;
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

    return g_PortClsDeviceControl(DeviceObject, Irp);
}

// DriverEntry runs exactly once and may stay in the discardable INIT segment.
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

    // FIX (P2): 在 PcInitializeAdapterDriver 之后保存 PortCls handler.
    // 之前在 PcInitializeAdapterDriver 之前保存 → g_PortClsDeviceControl
    // 拿到的是空默认 handler → audiodg 发送的 IOCTL 被转错 → KS filter
    // 属性查询失败 → MMDevice 端点不创建 → 系统声音设置里没有虚拟声卡.
    g_PortClsDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AES67DeviceControl;

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
    // TODO(P9): 目前只是分配+清零的"死缓冲"，内核侧没有写入者。P9 打通主动脉后，
    //           CSaveData::WriteData() 的 PCM 帧写入这里，用户态引擎映射读取。
    if (NT_SUCCESS(ntStatus) && !g_SharedBuffer) {
        g_SharedBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED,
            g_SharedBufferSize, AES67_POOLTAG);
        if (g_SharedBuffer) {
            RtlZeroMemory(g_SharedBuffer, g_SharedBufferSize);
            DPF(D_TERSE, ("[StartDevice] Shared buffer allocated: %p, %lu bytes",
                g_SharedBuffer, g_SharedBufferSize));
        }
    }

    return ntStatus;
} // StartDevice
#pragma code_seg()
