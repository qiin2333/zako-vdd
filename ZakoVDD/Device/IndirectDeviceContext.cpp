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
#include <sstream>
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
	stringstream logStream;

	logStream << "Destroying IndirectDeviceContext. Starting cleanup process.";
	vddlog("d", logStream.str().c_str());

	try
	{
		for (auto &pair : m_ProcessingThreads)
		{
			vddlog("d", "Stopping SwapChain processing thread in destructor");
			pair.second.reset();
		}
		m_ProcessingThreads.clear();
		vddlog("d", "All SwapChain processing threads stopped in destructor");
	}
	catch (const exception &e)
	{
		stringstream errorStream;
		errorStream << "Exception while stopping SwapChains in destructor: " << e.what();
		vddlog("e", errorStream.str().c_str());
	}
	catch (...)
	{
		vddlog("e", "Unknown exception while stopping SwapChains in destructor");
	}

	for (auto &pair : m_MouseEvents)
	{
		if (pair.second != nullptr)
		{
			CloseHandle(pair.second);
		}
	}
	m_MouseEvents.clear();
	vddlog("d", "Hardware cursor event handles cleaned up in destructor");

	for (auto &pair : m_Monitors)
	{
		try
		{
			if (pair.second != nullptr)
			{
				vddlog("d", ("Cleaning up monitor " + to_string(pair.first) + " in destructor").c_str());
				WdfObjectDelete(pair.second);
			}
		}
		catch (const exception &e)
		{
			stringstream errorStream;
			errorStream << "Exception while cleaning monitor in destructor: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...)
		{
			vddlog("e", "Unknown exception while cleaning monitor in destructor");
		}
	}
	m_Monitors.clear();

	logStream.str("");
	logStream << "IndirectDeviceContext cleanup completed.";
	vddlog("d", logStream.str().c_str());
}

void IndirectDeviceContext::InitAdapter()
{
	stringstream logStream;

	loadSettings();
	wstring requestedGpuName;
	if (gpuname.empty() || SameGpuName(gpuname, L"default"))
	{
		const wstring adaptername = confpath + L"\\adapter.txt";
		requestedGpuName = ReadAdapterPreferenceFile(adaptername);
		Options.Adapter.load(adaptername.c_str());
		logStream << "Attempting to Load GPU from adapter.txt";
	}
	else
	{
		requestedGpuName = gpuname;
		Options.Adapter.xmlprovide(gpuname);
		logStream << "Loading GPU from vdd_settings.xml";
	}
	vddlog("i", logStream.str().c_str());
	EnsureUsableRenderAdapter(Options.Adapter, requestedGpuName);
	GetGpuInfo();

	int edidResult = LoadKnownMonitorEdid();
	if (edidResult != 0)
	{
		vddlog("e", "EDID validation failed, adapter initialization will likely fail");
	}

	if (monitorModes.empty())
	{
		vddlog("w", "No monitor modes loaded, adding fallback 1920x1080@60Hz mode");
		int vsync_num, vsync_den;
		float_to_vsync(60.0f, vsync_num, vsync_den);
		monitorModes.push_back(make_tuple(1920, 1080, vsync_num, vsync_den));
	}

	stringstream modeLog;
	modeLog << "Loaded " << monitorModes.size() << " monitor modes for adapter initialization";
	vddlog("d", modeLog.str().c_str());

	logStream.str("");
	logStream << "Initializing adapter...";
	vddlog("d", logStream.str().c_str());
	logStream.str("");

	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);

	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2))
	{
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
		logStream << "FP16 processing capability detected.";
	}

	if (vrrEnabled.load())
	{
		if (!logStream.str().empty())
		{
			logStream << " ";
		}
		logStream << "VRR setting enabled; adapter flag not advertised.";
	}

	if (numVirtualDisplays == 0 || numVirtualDisplays > 16)
	{
		logStream << "Invalid numVirtualDisplays value: " << numVirtualDisplays << ". Setting to 1.";
		vddlog("w", logStream.str().c_str());
		numVirtualDisplays = 1;
		logStream.str("");
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

	logStream << "Adapter Caps Initialized:"
			  << "\n  Max Monitors Supported: " << AdapterCaps.MaxMonitorsSupported
			  << "\n  Gamma Support: " << AdapterCaps.EndPointDiagnostics.GammaSupport
			  << "\n  Transmission Type: " << AdapterCaps.EndPointDiagnostics.TransmissionType
			  << "\n  Friendly Name: " << AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName
			  << "\n  Manufacturer Name: " << AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName
			  << "\n  Model Name: " << AdapterCaps.EndPointDiagnostics.pEndPointModelName
			  << "\n  Firmware Version: " << Version.MajorVer
			  << "\n  Hardware Version: " << Version.MajorVer;

	vddlog("d", logStream.str().c_str());
	logStream.str("");

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	if (AdapterInit.WdfDevice == nullptr)
	{
		vddlog("e", "WdfDevice is null, cannot initialize adapter");
		return;
	}

	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	logStream << "Adapter Initialization Status: 0x" << hex << Status;
	vddlog("d", logStream.str().c_str());
	logStream.str("");

	if (NT_SUCCESS(Status))
	{
		if (AdapterInitOut.AdapterObject == nullptr)
		{
			vddlog("e", "Adapter initialization returned null handle despite success status");
			return;
		}

		m_Adapter = AdapterInitOut.AdapterObject;
		logStream << "Adapter handle stored successfully.";
		vddlog("d", logStream.str().c_str());

		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		if (pContext != nullptr)
		{
			pContext->pContext = this;
			vddlog("d", "Device context successfully linked to adapter");
		}
		else
		{
			vddlog("e", "Failed to get adapter context wrapper");
		}
	}
	else
	{
		logStream << "Failed to initialize adapter. Status: 0x" << hex << Status;
		vddlog("e", logStream.str().c_str());

		logStream.str("");
		logStream << "Adapter initialization failed with numVirtualDisplays=" << numVirtualDisplays;
		vddlog("e", logStream.str().c_str());
	}
}

void IndirectDeviceContext::FinishInit()
{
	Options.Adapter.apply(m_Adapter);
	SendToPipe("FinishInit");
	vddlog("i", "Applied Adapter configs.");
}
