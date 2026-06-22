#include "WdfDeviceLifecycle.h"

#include "..\Callbacks\IddCallbacks.h"
#include "..\Control\ControlTransport.h"
#include "..\Logging\Logger.h"
#include "IndirectDeviceContextWrapper.h"

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

	VDD_LOG_INFO("Starting driver unload process");

	// Clean up global device resources before stopping services
	if (g_GlobalDevice != nullptr)
	{
		VDD_LOG_DEBUG("Cleaning up global device resources");

		try
		{
			lock_guard<mutex> lock(g_Mutex);
			auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
			if (pContext && pContext->pContext)
			{
				// Stop any active SwapChain processing
				if (pContext->pContext->HasActiveSwapChain())
				{
					VDD_LOG_DEBUG("Stopping active SwapChain processing during unload");
					pContext->pContext->UnassignAllSwapChains();
					Sleep(50);
				}

				// Destroy all active monitors
				if (pContext->pContext->HasActiveMonitor())
				{
					VDD_LOG_DEBUG("Destroying all monitors during unload");
					try
					{
						pContext->pContext->DestroyAllMonitors();
					}
					catch (...)
					{
						VDD_LOG_WARNING("Failed to cleanly destroy monitors during unload");
					}
				}

				VDD_LOG_DEBUG("Global device resource cleanup completed");
			}
		}
		catch (const std::exception &e)
		{
			VDD_LOG_ERROR_STREAM("Exception during device cleanup in unload: " << e.what());
		}
		catch (...)
		{
			VDD_LOG_ERROR("Unknown exception during device cleanup in unload");
		}

		// Wait for system stabilization
		Sleep(100);
	}

	// [LEGACY-PIPE] Stop the named pipe server
	StopNamedPipeServer();

	VDD_LOG_INFO("Driver unload completed");
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

	UNREFERENCED_PARAMETER(Driver);

	VDD_LOG_DEBUG_STREAM("Initializing device:"
	                     << "\n  DeviceInit Pointer: " << static_cast<void *>(pDeviceInit));

	// Register for power callbacks - D0Entry for power-on, D0Exit for power-off (IDDCX 1.10 power management)
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = VirtualDisplayDriverDeviceD0Entry;
	PnpPowerCallbacks.EvtDeviceD0Exit = VirtualDisplayDriverDeviceD0Exit;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	VDD_LOG_DEBUG_STREAM("Configuring IDD_CX client:"
	                     << "\n  EvtIddCxAdapterInitFinished: " << (IddConfig.EvtIddCxAdapterInitFinished ? "Set" : "Not Set")
	                     << "\n  EvtIddCxMonitorGetDefaultDescriptionModes: " << (IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes ? "Set" : "Not Set")
	                     << "\n  EvtIddCxMonitorAssignSwapChain: " << (IddConfig.EvtIddCxMonitorAssignSwapChain ? "Set" : "Not Set")
	                     << "\n  EvtIddCxMonitorUnassignSwapChain: " << (IddConfig.EvtIddCxMonitorUnassignSwapChain ? "Set" : "Not Set"));

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
		VDD_LOG_ERROR_STREAM("IddCxDeviceInitConfig failed with status: " << Status);
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		VDD_LOG_DEBUG("Device cleanup callback triggered");

		// Automatically cleanup the context when the WDF object is about to be deleted
		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
		if (pContext && pContext->pContext)
		{
			try
			{
				// Perform comprehensive cleanup before destroying the context
				VDD_LOG_DEBUG("Performing comprehensive device cleanup");

				// Stop any active SwapChain processing
				if (pContext->pContext->HasActiveSwapChain())
				{
					VDD_LOG_DEBUG("Stopping active SwapChain during device cleanup");
					pContext->pContext->UnassignAllSwapChains();
					Sleep(50);
				}

				// Destroy all active monitors
				if (pContext->pContext->HasActiveMonitor())
				{
					VDD_LOG_DEBUG("Destroying all monitors during device cleanup");
					try
					{
						pContext->pContext->DestroyAllMonitors();
					}
					catch (...)
					{
						VDD_LOG_WARNING("Exception while destroying monitors during device cleanup");
					}
				}

				// Wait for stabilization
				Sleep(50);

				VDD_LOG_DEBUG("Device-specific cleanup completed, calling context cleanup");
			}
			catch (const std::exception &e)
			{
				VDD_LOG_ERROR_STREAM("Exception during device cleanup: " << e.what());
			}
			catch (...)
			{
				VDD_LOG_ERROR("Unknown exception during device cleanup");
			}

			// Always call the context cleanup
			pContext->Cleanup();
			VDD_LOG_DEBUG("Device cleanup callback completed");
		}
		else if (pContext)
		{
			VDD_LOG_WARNING("Device context wrapper found but context is null during cleanup");
			pContext->Cleanup();
		}
		else
		{
			VDD_LOG_WARNING("No device context wrapper found during cleanup");
		}
	};

	VDD_LOG_DEBUG("Creating device with WdfDeviceCreate:");

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		VDD_LOG_ERROR_STREAM("WdfDeviceCreate failed with status: " << Status);
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);
	if (!NT_SUCCESS(Status))
	{
		VDD_LOG_ERROR_STREAM("IddCxDeviceInitialize failed with status: " << Status);
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
		VDD_LOG_ERROR_STREAM("WdfDeviceCreateDeviceInterface failed with status: " << Status
		                     << " - IOCTL transport will be unavailable, pipe transport still works");
		// Non-fatal: pipe transport remains usable, so don't abort device add.
	}
	else
	{
		VDD_LOG_DEBUG("Registered Zako VDD control device interface");
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
		VDD_LOG_DEBUG("Device context initialized and attached to WDF device.");
	}
	else
	{
		VDD_LOG_ERROR("Failed to get device context wrapper.");
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
	// Log the entry into D0 state
	VDD_LOG_DEBUG_STREAM("Entering D0 power state:"
	                     << "\n  Device Handle: " << static_cast<void *>(Device)
	                     << "\n  Previous State: " << PreviousState);

	// This function is called by WDF to start the device in the fully-on power state.
	// For IDDCX 1.10 power management: when recovering from low-power state (D3),
	// we need to ensure the adapter is initialized so the system can re-assign SwapChain.

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext && pContext->pContext)
	{
		// Check if we're recovering from a low-power state
		if (PreviousState == WdfPowerDeviceD3 || PreviousState == WdfPowerDeviceD3Final)
		{
			VDD_LOG_INFO("Recovering from low-power state (D3), reinitializing adapter...");
		}
		else
		{
			VDD_LOG_DEBUG("Initializing adapter...");
		}

		// Initialize adapter (safe to call multiple times)
		pContext->pContext->InitAdapter();

		VDD_LOG_DEBUG("InitAdapter called successfully.");

		// Note: When recovering from D3, the system will automatically re-assign SwapChain
		// through the EvtIddCxMonitorAssignSwapChain callback, so we don't need to do it here.
	}
	else
	{
		VDD_LOG_ERROR("Failed to get device context.");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverDeviceD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
	// Log the exit from D0 state
	VDD_LOG_DEBUG_STREAM("Exiting D0 power state:"
	                     << "\n  Device Handle: " << static_cast<void *>(Device)
	                     << "\n  Target State: " << TargetState);

	// This function is called by WDF when the device is transitioning to a low-power state (D3).
	// For IDDCX 1.10 power management, we should pause SwapChain processing to save resources.

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext && pContext->pContext)
	{
		VDD_LOG_DEBUG("Preparing device for low-power state...");

		// Stop SwapChain processing to save GPU/CPU resources during low-power state
		if (pContext->pContext->HasActiveSwapChain())
		{
			VDD_LOG_INFO("Pausing SwapChain processing for power management");

			try
			{
				// Unassign all swap chains to stop processing
				pContext->pContext->UnassignAllSwapChains();
				Sleep(50);

				VDD_LOG_DEBUG("SwapChain processing paused successfully for power management");
			}
			catch (const std::exception &e)
			{
				VDD_LOG_ERROR_STREAM("Exception while pausing SwapChain for power management: " << e.what());
			}
			catch (...)
			{
				VDD_LOG_ERROR("Unknown exception while pausing SwapChain for power management");
			}
		}
		else
		{
			VDD_LOG_DEBUG("No active SwapChain to pause");
		}

		VDD_LOG_DEBUG("Device prepared for low-power state");
	}
	else
	{
		VDD_LOG_WARNING("Failed to get device context during D0Exit");
		// Don't return error - allow power transition to continue
	}

	return STATUS_SUCCESS;
}
