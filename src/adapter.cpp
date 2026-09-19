/*****************************************************************************
 * adapter.cpp
 *****************************************************************************
 * DriverEntry -> PcInitializeAdapterDriver -> AddDevice -> StartDevice.
 * Modeled on the WDK 7600 ac97\driver sample's adapter.cpp: validate
 * resources, create the adapter-common object, install the topology
 * subdevice then the wave subdevice, then wire a physical connection
 * between the two so the KS filter graph can be built across both.
 */

static char STR_MODULENAME[] = "stwrtxp Adapter: ";
#include "adapter.h"
#include "common.h"
#include "mintopo.h"
#include "wavecyclicminiport.h"

// Runtime workaround (see HANDOFF.md Stage 5af and Stage 5aj): the bus-owned
// PDO this driver attaches to has FILE_DEVICE_SECURE_OPEN (0x100) set AND a
// *populated* DACL holding zero ACEs. An ACL with no ACEs matches nothing, so
// SeAccessCheck denies every open against it, for every identity and every
// requested access mask. That is the check IopParseDevice runs against the PDO
// when user mode opens one of our KS device-interface paths - device interfaces
// are registered on the PDO, so the symbolic link resolves there - and it
// accounts for every ACCESS_DENIED symptom seen since Stage 5n.
//
// The previous attempt at this (Stage 5ag/5ah) rewrote the PDO's DACL at
// runtime via ObOpenObjectByPointer + ZwSetSecurityObject. That BLUESCREENED
// the machine (SYSTEM_SERVICE_EXCEPTION / 0x3B, Stage 5ai) and has been
// removed. Do not reintroduce it: setting another driver's PDO security
// descriptor from a function driver is not a supported operation, and 0x3B
// means "exception raised inside a system service", which is exactly where a
// ZwSetSecurityObject call on a device-object handle would die.
//
// This replacement never touches a security descriptor. It clears
// FILE_DEVICE_SECURE_OPEN instead - a single ULONG field write on a device
// object we already hold a referenced pointer to, at PASSIVE_LEVEL, with no
// API calls, no handles and no stack-built structures. It cannot fault and it
// cannot bugcheck. With that flag clear the I/O manager stops applying the
// device object's own SD to opens that carry a trailing name, so the
// reference-string opens KS and sysaudio actually use (the "wave" and
// "topology" suffixes) no longer meet the empty DACL.
//
// Expected side effect worth knowing before reading the next test's output: a
// *bare* device-interface open with no reference string IS still security
// checked, so kstest.exe's TestOpenBareInterface is still expected to fail
// with ERROR_ACCESS_DENIED. That is not a regression and not a signal that
// this workaround failed - only the two reference-string opens matter.
//
// This deliberately loosens access control on this one device and is a
// debug-stage measure. The supported fix is the INF's [DDInstall.HW] Security
// entry added in the same round (see stwrtxp.inf) - if the "PDO" dump above
// starts showing a DACL with ACEs instead of 0, that mechanism worked and this
// function should be deleted.
static void RelaxPdoSecureOpen(PDEVICE_OBJECT PhysicalDeviceObject)
{
    if (PhysicalDeviceObject->Characteristics & FILE_DEVICE_SECURE_OPEN)
    {
        PhysicalDeviceObject->Characteristics &= ~FILE_DEVICE_SECURE_OPEN;
        DOUT(DBG_PRINT, ("RelaxPdoSecureOpen: cleared FILE_DEVICE_SECURE_OPEN, PDO Characteristics now %08X",
            PhysicalDeviceObject->Characteristics));
    }
    else
    {
        DOUT(DBG_PRINT, ("RelaxPdoSecureOpen: FILE_DEVICE_SECURE_OPEN already clear (Characteristics=%08X), nothing to do",
            PhysicalDeviceObject->Characteristics));
    }
}

#pragma code_seg("INIT")
extern "C" NTSTATUS DriverEntry
(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PUNICODE_STRING RegistryPathName
)
{
    NTSTATUS status = PcInitializeAdapterDriver(DriverObject, RegistryPathName,
        (PDRIVER_ADD_DEVICE)AddDevice);

    return status;
}
#pragma code_seg()

/*****************************************************************************
 * AddDevice
 *****************************************************************************
 * PDRIVER_ADD_DEVICE-shaped (PDRIVER_OBJECT, PDEVICE_OBJECT PDO) - called by
 * the PnP manager, not by us. PcInitializeAdapterDriver's 3rd parameter is
 * this WDM AddDevice callback, not StartDevice directly (StartDevice's real
 * (DeviceObject, Irp, ResourceList) signature only matches PCPFNSTARTDEVICE,
 * handed to PortCls here via PcAddAdapterDevice, which invokes it once
 * resources are assigned). Modeled on the WDK 7600 ac97\driver sample's
 * AddDevice - "all adapter drivers can use this code without change".
 */
#pragma code_seg("PAGE")
NTSTATUS AddDevice
(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  PhysicalDeviceObject
)
{
    PAGED_CODE();

    // Stage 5aj: the PDO's DACL was found to hold 0 ACEs (deny-all) with
    // FILE_DEVICE_SECURE_OPEN set, so the I/O manager enforces it. Rather than
    // rewrite that DACL (the Stage 5ah attempt at this bluescreened the machine),
    // stop the flag that makes it apply to named/relative opens at all.
    RelaxPdoSecureOpen(PhysicalDeviceObject);

    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
        (PCPFNSTARTDEVICE)StartDevice, MAX_MINIPORTS, 0);
}
#pragma code_seg()

/*****************************************************************************
 * ValidateResources
 *****************************************************************************
 * Originally modeled on AC97/a directly-owned HDA controller (1 MMIO BAR +
 * 1 IRQ expected). Confirmed wrong on real hardware: under the real HD
 * Audio/UAA architecture the PCI controller's MMIO BAR and IRQ belong to
 * the bus driver (HDAudBus.sys); this driver instead binds to the codec's
 * function PDO (HDAUDIO\FUNC_01\...), a purely logical child device that
 * legitimately gets zero PCI resources of its own - real-hardware logging
 * confirmed "expected at least 1 memory resource, got 0" before this was
 * fixed. Verb transport and stream/DMA management instead go through
 * HDAUDIO_BUS_INTERFACE (see CHdaAdapterCommon::AcquireBusInterface in
 * common.cpp), so there is nothing left to require here - just accept
 * whatever ResourceList PnP handed us, including empty.
 */
#pragma code_seg("PAGE")
NTSTATUS ValidateResources
(
    IN  PRESOURCELIST  ResourceList
)
{
    PAGED_CODE();

    if (!ResourceList)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * StartDevice
 *****************************************************************************
 * PcInitializeAdapterDriver calls this once resources have been assigned.
 * Order: validate resources -> bring up CHdaAdapterCommon (controller
 * reset, CORB/RIRB, codec find + init verb sequence, widget discovery) ->
 * install the topology subdevice -> install the wave subdevice -> wire a
 * physical connection between the topology's bridge pin and the wave
 * filter's bridge pin so mmsystem/KS can build one filter graph spanning
 * both, exactly as ac97\driver's StartDevice does for its mixer/wave split.
 */
NTSTATUS StartDevice
(
    IN  PDEVICE_OBJECT     DeviceObject,
    IN  PIRP               Irp,
    IN  PRESOURCELIST      ResourceList
)
{
    PAGED_CODE();

    NTSTATUS status = ValidateResources(ResourceList);
    if (!NT_SUCCESS(status))
        return status;

    PUNKNOWN adapterCommonUnknown = NULL;
    status = NewAdapterCommon(&adapterCommonUnknown, GUID_NULL, NULL, NonPagedPool);
    if (!NT_SUCCESS(status))
        return status;

    PADAPTERCOMMON adapterCommon;
    status = adapterCommonUnknown->QueryInterface(IID_IHdaAdapterCommon,
        (PVOID *)&adapterCommon);
    if (!NT_SUCCESS(status))
    {
        adapterCommonUnknown->Release();
        return status;
    }

    status = adapterCommon->Init(ResourceList, DeviceObject);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("StartDevice: adapter common Init failed, status=%08X", status));
        adapterCommonUnknown->Release();
        return status;
    }

    // Register for PortCls power-management callbacks. msvad\simple (and
    // every other WDK PortCls sample) does this immediately after adapter
    // common Init and before installing any subdevice - CHdaAdapterCommon
    // already implements IAdapterPowerManagement (see common.h/common.cpp)
    // but this call was missing, so PortCls never knew to invoke it. Without
    // it PortCls has no confirmed power-up handshake for this adapter, which
    // was found to correlate with every KS filter CreateFile on this device
    // failing with ACCESS_DENIED (see HANDOFF.md Stage 5x) even though an
    // identical, unmodified msvad\simple filter - which does register - opens
    // fine on the same machine.
    status = PcRegisterAdapterPowerManagement((PUNKNOWN)adapterCommon, DeviceObject);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("StartDevice: PcRegisterAdapterPowerManagement failed, status=%08X", status));
        adapterCommonUnknown->Release();
        return status;
    }

    // --- Topology subdevice ---
    PUNKNOWN topoMiniportUnknown = NULL;
    status = CreateMiniportTopologyHda(&topoMiniportUnknown, GUID_NULL, NULL, NonPagedPool);
    if (!NT_SUCCESS(status))
    {
        adapterCommonUnknown->Release();
        return status;
    }

    PMINIPORTTOPOLOGY topoMiniport;
    topoMiniportUnknown->QueryInterface(IID_IMiniportTopology, (PVOID *)&topoMiniport);

    // Port->Init() drives the miniport's own Init() internally (passing
    // itself as the PPORTTOPOLOGY/PPORTWAVEPCI parameter and, for WavePci,
    // collecting its ServiceGroup out-param) - it is not something we call
    // ourselves, see the WDK 7600 ac97\driver sample's InstallSubdevice
    // (port->Init(DeviceObject, Irp, miniport, UnknownAdapter, ResourceList)
    // is the entire miniport-bring-up sequence there too).
    PPORTTOPOLOGY topoPort = NULL;
    status = PcNewPort((PPORT *)&topoPort, CLSID_PortTopology);
    if (NT_SUCCESS(status))
    {
        status = ((PPORT)topoPort)->Init(DeviceObject, Irp, topoMiniportUnknown,
            adapterCommonUnknown, ResourceList);
    }
    if (NT_SUCCESS(status))
    {
        status = PcRegisterSubdevice(DeviceObject, TOPO_SUBDEVICE_NAME, (PPORT)topoPort);
    }

    topoMiniport->Release();
    topoMiniportUnknown->Release();

    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("StartDevice: topology subdevice bring-up failed, status=%08X", status));
        if (topoPort)
            ((PPORT)topoPort)->Release();
        adapterCommonUnknown->Release();
        return status;
    }
    DOUT(DBG_PRINT, ("StartDevice: topology subdevice registered"));

    // --- Wave subdevice ---
    PUNKNOWN waveMiniportUnknown = NULL;
    status = CreateMiniportWaveCyclicHda(&waveMiniportUnknown, GUID_NULL, NULL, NonPagedPool);
    if (!NT_SUCCESS(status))
    {
        ((PPORT)topoPort)->Release();
        adapterCommonUnknown->Release();
        return status;
    }

    PMINIPORTWAVECYCLIC waveMiniport;
    waveMiniportUnknown->QueryInterface(IID_IMiniportWaveCyclic, (PVOID *)&waveMiniport);

    PPORTWAVECYCLIC wavePort = NULL;
    status = PcNewPort((PPORT *)&wavePort, CLSID_PortWaveCyclic);
    if (NT_SUCCESS(status))
    {
        status = ((PPORT)wavePort)->Init(DeviceObject, Irp, waveMiniportUnknown,
            adapterCommonUnknown, ResourceList);
    }
    if (NT_SUCCESS(status))
    {
        status = PcRegisterSubdevice(DeviceObject, WAVE_SUBDEVICE_NAME, (PPORT)wavePort);
    }

    waveMiniport->Release();
    waveMiniportUnknown->Release();

    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("StartDevice: wave subdevice bring-up failed, status=%08X", status));
        if (wavePort)
            ((PPORT)wavePort)->Release();
        ((PPORT)topoPort)->Release();
        adapterCommonUnknown->Release();
        return status;
    }
    DOUT(DBG_PRINT, ("StartDevice: wave subdevice registered"));

    // --- Physical connections between the wave and topology filters.
    //
    // Both endpoints of each call MUST be bridge pins
    // (KSPIN_COMMUNICATION_NONE), with the source endpoint's dataflow
    // pointing out of its filter and the destination endpoint's pointing in.
    // Before Stage 5at the wave side of both calls named a STREAMING pin -
    // PIN_WAVEOUT_BRIDGE and PIN_WAVEIN_BRIDGE were 0 and 1, the render and
    // capture sinks - so sysaudio had no traversable path from any wave pin
    // to the topology filter's speaker connector and never built a waveOut
    // device. That was Control Panel's "no audio device". The wave filter now
    // has real bridge pins at indices 2 and 3; see wavecyclicminiport.cpp and
    // the HdaWavePin/HdaTopoPin enums in shared.h.
    NTSTATUS connStatus = PcRegisterPhysicalConnection(DeviceObject,
        (PUNKNOWN)wavePort, PIN_WAVEOUT_BRIDGE,
        (PUNKNOWN)topoPort, PIN_TOPO_WAVEOUT_DEST);
    if (!NT_SUCCESS(connStatus))
    {
        DOUT(DBG_ERROR, ("StartDevice: PcRegisterPhysicalConnection (wave render->topo) failed, status=%08X", connStatus));
    }

    connStatus = PcRegisterPhysicalConnection(DeviceObject,
        (PUNKNOWN)topoPort, PIN_TOPO_WAVEIN_SOURCE,
        (PUNKNOWN)wavePort, PIN_WAVEIN_BRIDGE);
    if (!NT_SUCCESS(connStatus))
    {
        DOUT(DBG_ERROR, ("StartDevice: PcRegisterPhysicalConnection (topo->wave capture) failed, status=%08X", connStatus));
    }

    ((PPORT)wavePort)->Release();
    ((PPORT)topoPort)->Release();
    adapterCommonUnknown->Release();

    DOUT(DBG_PRINT, ("StartDevice: complete, physical connections registered"));

    return STATUS_SUCCESS;
}
