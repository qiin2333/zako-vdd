#pragma once

#include "..\Driver.h"

extern "C" EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;

EVT_WDF_DRIVER_DEVICE_ADD VirtualDisplayDriverDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY VirtualDisplayDriverDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT VirtualDisplayDriverDeviceD0Exit;
