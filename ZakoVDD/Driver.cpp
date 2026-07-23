/*++

Copyright (c) Microsoft Corporation

Abstract:

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"
#include "Config\DriverSettings.h"
#include "Core\DriverState.h"
#include "Device\WdfDeviceLifecycle.h"
#include "Diagnostics\DriverDiagnostics.h"
#include "Logging\Logger.h"
#include "Util\StringConversion.h"

#include <string>

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace std;

extern "C" DRIVER_INITIALIZE DriverEntry;

_Use_decl_annotations_ extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT pDriverObject,
	PUNICODE_STRING pRegistryPath)
{
	WDF_DRIVER_CONFIG Config;
	NTSTATUS Status;

	WDF_OBJECT_ATTRIBUTES Attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

	WDF_DRIVER_CONFIG_INIT(&Config, VirtualDisplayDriverDeviceAdd);

	Config.EvtDriverUnload = EvtDriverUnload;
	LoadDriverSettings();

	VDD_LOG_INFO("Driver Starting");
	string utf8_confpath = WStringToString(confpath);
	string logtext = "VDD Path: " + utf8_confpath;
	VDD_LOG_INFO(logtext.c_str());
	LogIddCxVersion();

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	return Status;
}
