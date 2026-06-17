#include "WdfDeviceLifecycle.h"

#include "..\Callbacks\IddCallbacks.h"
#include "..\Control\ControlTransport.h"
#include "..\Logging\Logger.h"
#include "IndirectDeviceContextWrapper.h"

#include <sstream>
#include <string>
#include <vdd_control_ioctl.h>

using namespace std;
using namespace Microsoft::IndirectDisp;

extern mutex g_Mutex;
extern WDFDEVICE g_GlobalDevice;

VOID EvtDriverUnload(
	_In_ WDFDRIVER Driver)
{
	UNREFERENCED_PARAMETER(Driver);

	vddlog("i", "Starting driver unload process");

	// Clean up global device resources before stopping services
	if (g_GlobalDevice != nullptr)
	{
		vddlog("d", "Cleaning up global device resources");

		try
		{
			lock_guard<mutex> lock(g_Mutex);
			auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
			if (pContext && pContext->pContext)
			{
				// Stop any active SwapChain processing
				if (pContext->pContext->HasActiveSwapChain())
				{
					vddlog("d", "Stopping active SwapChain processing during unload");
					pContext->pContext->UnassignAllSwapChains();
					Sleep(50);
				}

				// Destroy all active monitors
				if (pContext->pContext->HasActiveMonitor())
				{
					vddlog("d", "Destroying all monitors during unload");
					try
					{
						pContext->pContext->DestroyAllMonitors();
					}
					catch (...)
					{
						vddlog("w", "Failed to cleanly destroy monitors during unload");
					}
				}

				vddlog("d", "Global device resource cleanup completed");
			}
		}
		catch (const std::exception &e)
		{
			stringstream errorStream;
			errorStream << "Exception during device cleanup in unload: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...)
		{
			vddlog("e", "Unknown exception during device cleanup in unload");
		}

		// Wait for system stabilization
		Sleep(100);
	}

	// [LEGACY-PIPE] Stop the named pipe server
	StopNamedPipeServer();

	vddlog("i", "Driver unload completed");
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
	stringstream logStream;

	UNREFERENCED_PARAMETER(Driver);

	logStream << "Initializing device:"
			  << "\n  DeviceInit Pointer: " << static_cast<void *>(pDeviceInit);
	vddlog("d", logStream.str().c_str());

	// Register for power callbacks - D0Entry for power-on, D0Exit for power-off (IDDCX 1.10 power management)
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = VirtualDisplayDriverDeviceD0Entry;
	PnpPowerCallbacks.EvtDeviceD0Exit = VirtualDisplayDriverDeviceD0Exit;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	logStream.str("");
	logStream << "Configuring IDD_CX client:"
			  << "\n  EvtIddCxAdapterInitFinished: " << (IddConfig.EvtIddCxAdapterInitFinished ? "Set" : "Not Set")
			  << "\n  EvtIddCxMonitorGetDefaultDescriptionModes: " << (IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes ? "Set" : "Not Set")
			  << "\n  EvtIddCxMonitorAssignSwapChain: " << (IddConfig.EvtIddCxMonitorAssignSwapChain ? "Set" : "Not Set")
			  << "\n  EvtIddCxMonitorUnassignSwapChain: " << (IddConfig.EvtIddCxMonitorUnassignSwapChain ? "Set" : "Not Set");
	vddlog("d", logStream.str().c_str());

	// If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
	// redirects IoDeviceControl requests to an internal queue.
	IddConfig.EvtIddCxDeviceIoControl = VirtualDisplayDriverIoDeviceControl;

	IddConfig.EvtIddCxAdapterInitFinished = VirtualDisplayDriverAdapterInitFinished;

	IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = VirtualDisplayDriverMonitorGetDefaultModes;
	IddConfig.EvtIddCxMonitorAssignSwapChain = VirtualDisplayDriverMonitorAssignSwapChain;
	IddConfig.EvtIddCxMonitorUnassignSwapChain = VirtualDisplayDriverMonitorUnassignSwapChain;

	if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))
	{
		IddConfig.EvtIddCxAdapterQueryTargetInfo = VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo;
		IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata;
		IddConfig.EvtIddCxParseMonitorDescription2 = VirtualDisplayDriverEvtIddCxParseMonitorDescription2;
		IddConfig.EvtIddCxMonitorQueryTargetModes2 = VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2;
		IddConfig.EvtIddCxAdapterCommitModes2 = VirtualDisplayDriverEvtIddCxAdapterCommitModes2;
		IddConfig.EvtIddCxMonitorSetGammaRamp = VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp;
	}
	else
	{
		IddConfig.EvtIddCxParseMonitorDescription = VirtualDisplayDriverParseMonitorDescription;
		IddConfig.EvtIddCxMonitorQueryTargetModes = VirtualDisplayDriverMonitorQueryModes;
		IddConfig.EvtIddCxAdapterCommitModes = VirtualDisplayDriverAdapterCommitModes;
	}

	Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
	if (!NT_SUCCESS(Status))
	{
		logStream.str("");
		logStream << "IddCxDeviceInitConfig failed with status: " << Status;
		vddlog("e", logStream.str().c_str());
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		vddlog("d", "Device cleanup callback triggered");

		// Automatically cleanup the context when the WDF object is about to be deleted
		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
		if (pContext && pContext->pContext)
		{
			try
			{
				// Perform comprehensive cleanup before destroying the context
				vddlog("d", "Performing comprehensive device cleanup");

				// Stop any active SwapChain processing
				if (pContext->pContext->HasActiveSwapChain())
				{
					vddlog("d", "Stopping active SwapChain during device cleanup");
					pContext->pContext->UnassignAllSwapChains();
					Sleep(50);
				}

				// Destroy all active monitors
				if (pContext->pContext->HasActiveMonitor())
				{
					vddlog("d", "Destroying all monitors during device cleanup");
					try
					{
						pContext->pContext->DestroyAllMonitors();
					}
					catch (...)
					{
						vddlog("w", "Exception while destroying monitors during device cleanup");
					}
				}

				// Wait for stabilization
				Sleep(50);

				vddlog("d", "Device-specific cleanup completed, calling context cleanup");
			}
			catch (const std::exception &e)
			{
				stringstream errorStream;
				errorStream << "Exception during device cleanup: " << e.what();
				vddlog("e", errorStream.str().c_str());
			}
			catch (...)
			{
				vddlog("e", "Unknown exception during device cleanup");
			}

			// Always call the context cleanup
			pContext->Cleanup();
			vddlog("d", "Device cleanup callback completed");
		}
		else if (pContext)
		{
			vddlog("w", "Device context wrapper found but context is null during cleanup");
			pContext->Cleanup();
		}
		else
		{
			vddlog("w", "No device context wrapper found during cleanup");
		}
	};

	logStream.str("");
	logStream << "Creating device with WdfDeviceCreate:";
	vddlog("d", logStream.str().c_str());

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		logStream.str("");
		logStream << "WdfDeviceCreate failed with status: " << Status;
		vddlog("e", logStream.str().c_str());
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);
	if (!NT_SUCCESS(Status))
	{
		logStream.str("");
		logStream << "IddCxDeviceInitialize failed with status: " << Status;
		vddlog("e", logStream.str().c_str());
		return Status;
	}

	// Expose a custom device interface so external callers (Sunshine) can
	// reach us via DeviceIoControl over CreateFile(\\?\GUID...). This is the
	// transport that survives WUDFHost recycling: opening the interface
	// PnP-wakes the driver back into D0 transparently. The legacy named pipe
	// transport remains active in parallel for backwards compatibility but
	// is now only the fallback path.
	Status = WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL, NULL);
	if (!NT_SUCCESS(Status))
	{
		logStream.str("");
		logStream << "WdfDeviceCreateDeviceInterface failed with status: " << Status
		          << " - IOCTL transport will be unavailable, pipe transport still works";
		vddlog("e", logStream.str().c_str());
		// Non-fatal: pipe transport remains usable, so don't abort device add.
	}
	else
	{
		vddlog("d", "Registered Zako VDD control device interface");
	}

	// Create a new device context object and attach it to the WDF device object
	/*
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);
	*/
	// code to return uncase the device context wrapper isnt found (Most likely insufficient resources)

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext)
	{
		pContext->pContext = new IndirectDeviceContext(Device);
		logStream.str("");
		logStream << "Device context initialized and attached to WDF device.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to get device context wrapper.";
		vddlog("e", logStream.str().c_str());
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// Save global reference after successful device creation
	g_GlobalDevice = Device;

	return Status;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	stringstream logStream;

	// Log the entry into D0 state
	logStream << "Entering D0 power state:"
			  << "\n  Device Handle: " << static_cast<void *>(Device)
			  << "\n  Previous State: " << PreviousState;
	vddlog("d", logStream.str().c_str());

	// This function is called by WDF to start the device in the fully-on power state.
	// For IDDCX 1.10 power management: when recovering from low-power state (D3),
	// we need to ensure the adapter is initialized so the system can re-assign SwapChain.

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext && pContext->pContext)
	{
		// Check if we're recovering from a low-power state
		if (PreviousState == WdfPowerDeviceD3 || PreviousState == WdfPowerDeviceD3Final)
		{
			logStream.str("");
			logStream << "Recovering from low-power state (D3), reinitializing adapter...";
			vddlog("i", logStream.str().c_str());
		}
		else
		{
			logStream.str("");
			logStream << "Initializing adapter...";
			vddlog("d", logStream.str().c_str());
		}

		// Initialize adapter (safe to call multiple times)
		pContext->pContext->InitAdapter();

		logStream.str("");
		logStream << "InitAdapter called successfully.";
		vddlog("d", logStream.str().c_str());

		// Note: When recovering from D3, the system will automatically re-assign SwapChain
		// through the EvtIddCxMonitorAssignSwapChain callback, so we don't need to do it here.
	}
	else
	{
		logStream.str("");
		logStream << "Failed to get device context.";
		vddlog("e", logStream.str().c_str());
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverDeviceD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
	stringstream logStream;

	// Log the exit from D0 state
	logStream << "Exiting D0 power state:"
			  << "\n  Device Handle: " << static_cast<void *>(Device)
			  << "\n  Target State: " << TargetState;
	vddlog("d", logStream.str().c_str());

	// This function is called by WDF when the device is transitioning to a low-power state (D3).
	// For IDDCX 1.10 power management, we should pause SwapChain processing to save resources.

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext && pContext->pContext)
	{
		logStream.str("");
		logStream << "Preparing device for low-power state...";
		vddlog("d", logStream.str().c_str());

		// Stop SwapChain processing to save GPU/CPU resources during low-power state
		if (pContext->pContext->HasActiveSwapChain())
		{
			logStream.str("");
			logStream << "Pausing SwapChain processing for power management";
			vddlog("i", logStream.str().c_str());

			try
			{
				// Unassign all swap chains to stop processing
				pContext->pContext->UnassignAllSwapChains();
				Sleep(50);

				logStream.str("");
				logStream << "SwapChain processing paused successfully for power management";
				vddlog("d", logStream.str().c_str());
			}
			catch (const std::exception &e)
			{
				stringstream errorStream;
				errorStream << "Exception while pausing SwapChain for power management: " << e.what();
				vddlog("e", errorStream.str().c_str());
			}
			catch (...)
			{
				vddlog("e", "Unknown exception while pausing SwapChain for power management");
			}
		}
		else
		{
			logStream.str("");
			logStream << "No active SwapChain to pause";
			vddlog("d", logStream.str().c_str());
		}

		logStream.str("");
		logStream << "Device prepared for low-power state";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to get device context during D0Exit";
		vddlog("w", logStream.str().c_str());
		// Don't return error - allow power transition to continue
	}

	return STATUS_SUCCESS;
}
