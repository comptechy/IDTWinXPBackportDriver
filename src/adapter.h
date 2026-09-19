/*****************************************************************************
 * adapter.h
 *****************************************************************************
 * DriverEntry / AddDevice / StartDevice for the HDA XPDM adapter driver.
 * Architecturally modeled on the WDK 7600 ac97\driver sample's
 * adapter.h/adapter.cpp (PcInitializeAdapterDriver / PcAddAdapterDevice /
 * PcNewPort pattern), but with the topology + wave subdevices always
 * created unconditionally (no CLSID-dispatch class table - this driver
 * only ever instantiates these two known miniports, unlike ac97's mixer/
 * wave split that dispatches by requested class).
 */

#ifndef _ADAPTER_H_
#define _ADAPTER_H_

#include "shared.h"

#define WAVE_SUBDEVICE_NAME     L"Wave"
#define TOPO_SUBDEVICE_NAME     L"Topology"

// Passed to PcAddAdapterDevice - see the WDK 7600 ac97\driver sample's
// adapter.h. Just the two subdevices this driver ever installs (topology +
// wave); no CLSID-dispatch class table.
const ULONG MAX_MINIPORTS = 2;

NTSTATUS ValidateResources
(
    IN  PRESOURCELIST  ResourceList
);

// PDRIVER_ADD_DEVICE-typed - passed to PcInitializeAdapterDriver from
// DriverEntry (adapter.cpp). Called once by the PnP manager per device
// instance; just hands off to PcAddAdapterDevice with StartDevice as the
// callback PortCls invokes once resources are assigned.
NTSTATUS AddDevice
(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  PhysicalDeviceObject
);

NTSTATUS StartDevice
(
    IN  PDEVICE_OBJECT     DeviceObject,
    IN  PIRP                Irp,
    IN  PRESOURCELIST       ResourceList
);

#endif // _ADAPTER_H_
