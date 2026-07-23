#include "..\Driver.h"
#include "..\Adapter\GpuAdapterSelection.h"
#include "..\Adapter\GpuStatus.h"
#include "..\Config\DriverSettings.h"
#include "..\Core\DriverState.h"
#include "..\Edid\Edid.h"
#include "..\Logging\Logger.h"
#include "..\Util\RefreshRate.h"
#include "IndirectDeviceContext.h"
#include "IndirectDeviceContextWrapper.h"

#include <exception>
#include <string>
#include <tuple>

using namespace std;
using namespace Microsoft::IndirectDisp;

std::vector<unsigned char> Microsoft::IndirectDisp::IndirectDeviceContext::s_KnownMonitorEdid;

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) : m_WdfDevice(WdfDevice)
{
	m_Adapter = {};
}

IndirectDeviceContext::~IndirectDeviceContext()
{
	VDD_LOG_DEBUG("Destroying IndirectDeviceContext. Starting cleanup process.");

	try
	{
		for (auto &pair : m_ProcessingThreads)
		{
			VDD_LOG_DEBUG("Stopping SwapChain processing thread in destructor");
			pair.second.reset();
		}
		m_ProcessingThreads.clear();
		VDD_LOG_DEBUG("All SwapChain processing threads stopped in destructor");
	}
	catch (const exception &e)
	{
		VDD_LOG_ERROR_STREAM("Exception while stopping SwapChains in destructor: " << e.what());
	}
	catch (...)
	{
		VDD_LOG_ERROR("Unknown exception while stopping SwapChains in destructor");
	}

	for (auto &pair : m_MouseEvents)
	{
		if (pair.second != nullptr)
		{
			CloseHandle(pair.second);
		}
	}
	m_MouseEvents.clear();
	VDD_LOG_DEBUG("Hardware cursor event handles cleaned up in destructor");

	for (auto &pair : m_Monitors)
	{
		try
		{
			if (pair.second != nullptr)
			{
				VDD_LOG_DEBUG_STREAM("Cleaning up monitor " << pair.first << " in destructor");
				WdfObjectDelete(pair.second);
			}
		}
		catch (const exception &e)
		{
			VDD_LOG_ERROR_STREAM("Exception while cleaning monitor in destructor: " << e.what());
		}
		catch (...)
		{
			VDD_LOG_ERROR("Unknown exception while cleaning monitor in destructor");
		}
	}
	m_Monitors.clear();

	VDD_LOG_DEBUG("IndirectDeviceContext cleanup completed.");
}

void IndirectDeviceContext::InitAdapter()
{
	loadSettings();
	if (gpuname.empty() || SameGpuName(gpuname, L"default"))
	{
		const wstring adaptername = confpath + L"\\adapter.txt";
		Options.Adapter.load(adaptername.c_str());
		VDD_LOG_INFO("Attempting to Load GPU from adapter.txt");
	}
	else
	{
		Options.Adapter.xmlprovide(gpuname);
		VDD_LOG_INFO("Loading GPU from vdd_settings.xml");
	}
	// Avoid probing D3D11 render-adapter usability during UMDF D0 startup.
	// Some Intel display stacks can hang WUDFHost while the virtual display
	// adapter is still initializing. Sunshine should validate explicit GPU
	// selections before writing them to the driver configuration instead.
	GetGpuInfo();

	int edidResult = LoadKnownMonitorEdid();
	if (edidResult != 0)
	{
		VDD_LOG_ERROR("EDID validation failed, adapter initialization will likely fail");
	}

	if (monitorModes.empty())
	{
		VDD_LOG_WARNING("No monitor modes loaded, adding fallback 1920x1080@60Hz mode");
		int vsync_num, vsync_den;
		float_to_vsync(60.0f, vsync_num, vsync_den);
		monitorModes.push_back(make_tuple(1920, 1080, vsync_num, vsync_den));
	}

	VDD_LOG_DEBUG_STREAM("Loaded " << monitorModes.size() << " monitor modes for adapter initialization");
	VDD_LOG_DEBUG("Initializing adapter...");

	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);
	string adapterCapabilityNotes;

	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2))
	{
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
		adapterCapabilityNotes = "FP16 processing capability detected.";
	}

	if (vrrEnabled.load())
	{
		if (!adapterCapabilityNotes.empty())
		{
			adapterCapabilityNotes += " ";
		}
		adapterCapabilityNotes += "VRR setting enabled; adapter flag not advertised.";
	}

	if (numVirtualDisplays == 0 || numVirtualDisplays > 16)
	{
		VDD_LOG_WARNING_STREAM((adapterCapabilityNotes.empty() ? "" : adapterCapabilityNotes + " ")
		                       << "Invalid numVirtualDisplays value: " << numVirtualDisplays << ". Setting to 1.");
		numVirtualDisplays = 1;
	}

	AdapterCaps.MaxMonitorsSupported = numVirtualDisplays;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"ZakoVdd Device";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"ZakoTech";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"ZakoVdd Model";

	IDDCX_ENDPOINT_VERSION Version = {};
	Version.Size = sizeof(Version);
	Version.MajorVer = 1;
	AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
	AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

	VDD_LOG_DEBUG_STREAM((adapterCapabilityNotes.empty() ? "" : adapterCapabilityNotes + "\n")
	                     << "Adapter Caps Initialized:"
	                     << "\n  Max Monitors Supported: " << AdapterCaps.MaxMonitorsSupported
	                     << "\n  Gamma Support: " << AdapterCaps.EndPointDiagnostics.GammaSupport
	                     << "\n  Transmission Type: " << AdapterCaps.EndPointDiagnostics.TransmissionType
	                     << "\n  Friendly Name: " << AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName
	                     << "\n  Manufacturer Name: " << AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName
	                     << "\n  Model Name: " << AdapterCaps.EndPointDiagnostics.pEndPointModelName
	                     << "\n  Firmware Version: " << Version.MajorVer
	                     << "\n  Hardware Version: " << Version.MajorVer);

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	if (AdapterInit.WdfDevice == nullptr)
	{
		VDD_LOG_ERROR("WdfDevice is null, cannot initialize adapter");
		return;
	}

	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	VDD_LOG_DEBUG_STREAM("Adapter Initialization Status: 0x" << hex << Status);

	if (NT_SUCCESS(Status))
	{
		if (AdapterInitOut.AdapterObject == nullptr)
		{
			VDD_LOG_ERROR("Adapter initialization returned null handle despite success status");
			return;
		}

		m_Adapter = AdapterInitOut.AdapterObject;
		VDD_LOG_DEBUG("Adapter handle stored successfully.");

		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		if (pContext != nullptr)
		{
			pContext->pContext = this;
			VDD_LOG_DEBUG("Device context successfully linked to adapter");
		}
		else
		{
			VDD_LOG_ERROR("Failed to get adapter context wrapper");
		}
	}
	else
	{
		VDD_LOG_ERROR_STREAM("Failed to initialize adapter. Status: 0x" << hex << Status);
		VDD_LOG_ERROR_STREAM("Adapter initialization failed with numVirtualDisplays=" << numVirtualDisplays);
	}
}

void IndirectDeviceContext::FinishInit()
{
	Options.Adapter.apply(m_Adapter);
	VDD_LOG_INFO("Applied Adapter configs.");
}
