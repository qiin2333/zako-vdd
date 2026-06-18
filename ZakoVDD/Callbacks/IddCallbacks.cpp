#include "IddCallbacks.h"
#include "..\Device\DisplayModeHelpers.h"
#include "..\Device\IndirectDeviceContextWrapper.h"
#include "..\Device\MonitorState.h"
#include "..\Core\DriverState.h"
#include "..\Logging\Logger.h"
#include "..\Util\StringConversion.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace std;
using namespace Microsoft::IndirectDisp;

extern std::mutex g_DataMutex;
extern std::vector<DISPLAYCONFIG_VIDEO_SIGNAL_INFO> s_KnownMonitorModes2;
extern std::wstring ColourFormat;
extern IDDCX_BITS_PER_COMPONENT SDRCOLOUR;
extern IDDCX_BITS_PER_COMPONENT HDRCOLOUR;

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED *pInArgs)
{
	// This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
	// to report attached monitors.

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
	if (NT_SUCCESS(pInArgs->AdapterInitStatus))
	{
		pContext->pContext->FinishInit();
		VDD_LOG_DEBUG("Adapter initialization finished successfully.");
	}
	else
	{
		VDD_LOG_ERROR_STREAM("Adapter initialization failed. Status: " << pInArgs->AdapterInitStatus);
	}
	VDD_LOG_INFO("Finished Setting up adapter.");

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
VirtualDisplayDriverAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES *pInArgs)
{
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
	if (pContext && pContext->pContext)
	{
		pContext->pContext->CommitModes(pInArgs);
	}

	return STATUS_SUCCESS;
}
_Use_decl_annotations_
	NTSTATUS
VirtualDisplayDriverParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION *pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs)
{
	VDD_LOG_DEBUG_STREAM("Parsing monitor description. Input buffer count: " << pInArgs->MonitorModeBufferInputCount);

	// Take a local snapshot of monitorModes under lock to prevent data races
	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
	}

	// Clear previous monitor modes to prevent accumulation on reload
	s_KnownMonitorModes2.clear();

	for (int i = 0; i < localModes.size(); i++)
	{
		s_KnownMonitorModes2.push_back(dispinfo(std::get<0>(localModes[i]), std::get<1>(localModes[i]), std::get<2>(localModes[i]), std::get<3>(localModes[i])));
	}
	pOutArgs->MonitorModeBufferOutputCount = (UINT)localModes.size();

	VDD_LOG_DEBUG_STREAM("Number of monitor modes generated: " << localModes.size());

	if (pInArgs->MonitorModeBufferInputCount < localModes.size())
	{
		VDD_LOG_WARNING_STREAM("Buffer too small. Input count: " << pInArgs->MonitorModeBufferInputCount << ", Required: " << localModes.size());
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		auto *monitorModesOutput = pInArgs->pMonitorModes;
		if (monitorModesOutput == nullptr)
		{
			VDD_LOG_ERROR("Monitor mode output buffer is null.");
			return STATUS_INVALID_PARAMETER;
		}

		// Copy the known modes to the output buffer
		for (DWORD ModeIndex = 0; ModeIndex < localModes.size(); ModeIndex++)
		{
			monitorModesOutput[ModeIndex].Size = sizeof(IDDCX_MONITOR_MODE);
			monitorModesOutput[ModeIndex].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
			monitorModesOutput[ModeIndex].MonitorVideoSignalInfo = s_KnownMonitorModes2[ModeIndex];
		}

		// Set the preferred mode as represented in the EDID
		pOutArgs->PreferredMonitorModeIdx = 0;
		VDD_LOG_DEBUG("Monitor description parsed successfully.");
		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *pOutArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);
	UNREFERENCED_PARAMETER(pOutArgs);

	// The driver always exposes a monitor descriptor, so IddCx should not request fallback default modes.

	return STATUS_NOT_IMPLEMENTED;
}

/// <summary>
/// Creates a target mode from the fundamental mode attributes.
/// </summary>
void CreateTargetMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO &Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	VDD_LOG_DEBUG_STREAM("Creating target mode with Width: " << Width
	                     << ", Height: " << Height
	                     << ", VSyncNum: " << VSyncNum
	                     << ", VSyncDen: " << VSyncDen);

	Mode.totalSize.cx = Mode.activeSize.cx = Width;
	Mode.totalSize.cy = Mode.activeSize.cy = Height;
	Mode.AdditionalSignalInfo.vSyncFreqDivider = 1;
	Mode.AdditionalSignalInfo.videoStandard = 255;
	Mode.vSyncFreq.Numerator = VSyncNum;
	Mode.vSyncFreq.Denominator = VSyncDen;
	Mode.hSyncFreq.Numerator = VSyncNum * Height;
	Mode.hSyncFreq.Denominator = VSyncDen;
	Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
	Mode.pixelRate = static_cast<UINT64>(VSyncNum) * Width * Height / (VSyncDen > 0 ? VSyncDen : 1);

	VDD_LOG_DEBUG_STREAM("Target mode configured with:"
	                     << "\n  Total Size: (" << Mode.totalSize.cx << ", " << Mode.totalSize.cy << ")"
	                     << "\n  Active Size: (" << Mode.activeSize.cx << ", " << Mode.activeSize.cy << ")"
	                     << "\n  vSync Frequency: " << Mode.vSyncFreq.Numerator << "/" << Mode.vSyncFreq.Denominator
	                     << "\n  hSync Frequency: " << Mode.hSyncFreq.Numerator << "/" << Mode.hSyncFreq.Denominator
	                     << "\n  Pixel Rate: " << Mode.pixelRate
	                     << "\n  Scan Line Ordering: " << Mode.scanLineOrdering);
}

void CreateTargetMode(IDDCX_TARGET_MODE &Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	Mode.Size = sizeof(Mode);
	CreateTargetMode(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSyncNum, VSyncDen);
}

void CreateTargetMode2(IDDCX_TARGET_MODE2 &Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	VDD_LOG_DEBUG_STREAM("Creating IDDCX_TARGET_MODE2 with Width: " << Width
	                     << ", Height: " << Height
	                     << ", VSyncNum: " << VSyncNum
	                     << ", VSyncDen: " << VSyncDen);

	Mode.Size = sizeof(Mode);

	if (ColourFormat == L"RGB")
	{
		Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr444")
	{
		Mode.BitsPerComponent.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr422")
	{
		Mode.BitsPerComponent.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr420")
	{
		Mode.BitsPerComponent.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
	}
	else
	{
		Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR; // Default to RGB
	}

	VDD_LOG_DEBUG_STREAM("IDDCX_TARGET_MODE2 configured with Size: " << Mode.Size
	                     << " and colour format " << WStringToString(ColourFormat));

	CreateTargetMode(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSyncNum, VSyncDen);
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES *pInArgs, IDARG_OUT_QUERYTARGETMODES *pOutArgs) ////////////////////////////////////////////////////////////////////////////////
{
	UNREFERENCED_PARAMETER(MonitorObject);

	// Take a local snapshot of monitorModes under lock to prevent data races
	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
	}

	vector<IDDCX_TARGET_MODE> TargetModes(localModes.size());

	VDD_LOG_DEBUG_STREAM("Creating target modes. Number of monitor modes: " << localModes.size());

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	for (int i = 0; i < localModes.size(); i++)
	{
		CreateTargetMode(TargetModes[i], std::get<0>(localModes[i]), std::get<1>(localModes[i]), std::get<2>(localModes[i]), std::get<3>(localModes[i]));

		VDD_LOG_DEBUG_STREAM("Created target mode " << i << ": Width = " << std::get<0>(localModes[i])
		                     << ", Height = " << std::get<1>(localModes[i])
		                     << ", VSync = " << std::get<2>(localModes[i]));
	}

	pOutArgs->TargetModeBufferOutputCount = (UINT)TargetModes.size();

	VDD_LOG_DEBUG_STREAM("Number of target modes to output: " << pOutArgs->TargetModeBufferOutputCount);

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		VDD_LOG_DEBUG("Copying target modes to output buffer.");
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	}
	else
	{
		VDD_LOG_WARNING_STREAM("Input buffer too small. Required: " << TargetModes.size()
		                       << ", Provided: " << pInArgs->TargetModeBufferInputCount);
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN *pInArgs)
{
	VDD_LOG_DEBUG_STREAM("Assigning swap chain:"
	                     << "\n  hSwapChain: " << pInArgs->hSwapChain
	                     << "\n  RenderAdapterLuid: " << pInArgs->RenderAdapterLuid.LowPart << "-" << pInArgs->RenderAdapterLuid.HighPart
	                     << "\n  hNextSurfaceAvailable: " << pInArgs->hNextSurfaceAvailable);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	VDD_LOG_DEBUG("Swap chain assigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	VDD_LOG_DEBUG_STREAM("Unassigning swap chain for monitor object: " << MonitorObject);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->UnassignSwapChain(MonitorObject);
	VDD_LOG_DEBUG("Swap chain unassigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo(
		IDDCX_ADAPTER AdapterObject,
		IDARG_IN_QUERYTARGET_INFO *pInArgs,
		IDARG_OUT_QUERYTARGET_INFO *pOutArgs)
{
	VDD_LOG_DEBUG_STREAM("Querying target info for adapter object: " << AdapterObject);

	UNREFERENCED_PARAMETER(pInArgs);

	pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;

	if (ColourFormat == L"RGB")
	{
		pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr444")
	{
		pOutArgs->DitheringSupport.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr422")
	{
		pOutArgs->DitheringSupport.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr420")
	{
		pOutArgs->DitheringSupport.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
	}
	else
	{
		pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR; // Default to RGB
	}

	VDD_LOG_DEBUG_STREAM("Target capabilities set to: " << pOutArgs->TargetCaps
	                     << "\nDithering support colour format set to: " << WStringToString(ColourFormat));

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata(
		IDDCX_MONITOR MonitorObject,
		const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *pInArgs)
{
	VDD_LOG_DEBUG_STREAM("Setting default HDR metadata for monitor object: " << MonitorObject);

	// Get the HDR luminance settings for this monitor
	float maxNits = 1000.0f; // Default max luminance
	float minNits = 0.0001f; // Default min luminance
	float maxFALL = 0.0f;     // Default MaxFALL

	{
		lock_guard<mutex> lock(s_HdrSettingsMutex);
		auto it = s_MonitorHdrSettingsMap.find(MonitorObject);
		if (it != s_MonitorHdrSettingsMap.end())
		{
			maxNits = it->second.maxNits;
			minNits = it->second.minNits;
			maxFALL = it->second.maxFALL;
			VDD_LOG_DEBUG_STREAM("Retrieved HDR settings - MaxNits: " << maxNits
			                     << ", MinNits: " << minNits
			                     << ", MaxFALL: " << maxFALL);
		}
		else
		{
			VDD_LOG_DEBUG("Using default HDR luminance settings (monitor not found in settings map)");
		}
	}

	// Log the incoming metadata type
	if (pInArgs)
	{
		VDD_LOG_DEBUG_STREAM("HDR Metadata Type: " << static_cast<int>(pInArgs->Type));

		// Log current luminance settings being applied
		VDD_LOG_DEBUG_STREAM("Applying HDR10 metadata - MaxMasteringLuminance: " << maxNits
		                     << " nits, MinMasteringLuminance: " << (minNits * 10000.0f) << " (normalized)");
	}

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	if (pContext && pContext->pContext)
	{
		pContext->pContext->UpdateMonitorHdrMetadata(MonitorObject, true, maxNits, minNits, maxFALL);
	}

	VDD_LOG_DEBUG("Default HDR metadata set successfully.");

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2 *pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs)
{
	// Take a local snapshot of monitorModes under lock to prevent data races
	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
	}

	VDD_LOG_DEBUG_STREAM("Parsing monitor description:"
	                     << "\n  MonitorModeBufferInputCount: " << pInArgs->MonitorModeBufferInputCount
	                     << "\n  pMonitorModes: " << (pInArgs->pMonitorModes ? "Valid" : "Null"));

	VDD_LOG_DEBUG_LAZY([&]() -> string
	{
		stringstream logStream;
		logStream << "Monitor Modes:";
		for (const auto &mode : localModes)
		{
			logStream << "\n  Mode - Width: " << std::get<0>(mode)
			          << ", Height: " << std::get<1>(mode)
			          << ", RefreshRate: " << std::get<2>(mode);
		}
		return logStream.str();
	});

	// Clear previous monitor modes to prevent accumulation on reload
	s_KnownMonitorModes2.clear();

	for (int i = 0; i < localModes.size(); i++)
	{
		s_KnownMonitorModes2.push_back(dispinfo(std::get<0>(localModes[i]), std::get<1>(localModes[i]), std::get<2>(localModes[i]), std::get<3>(localModes[i])));
	}
	pOutArgs->MonitorModeBufferOutputCount = (UINT)localModes.size();

	if (pInArgs->MonitorModeBufferInputCount < localModes.size())
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		auto *monitorModesOutput = pInArgs->pMonitorModes;
		if (monitorModesOutput == nullptr)
		{
			VDD_LOG_ERROR("Monitor mode output buffer is null.");
			return STATUS_INVALID_PARAMETER;
		}

		// Copy the known modes to the output buffer
		for (DWORD ModeIndex = 0; ModeIndex < localModes.size(); ModeIndex++)
		{
			monitorModesOutput[ModeIndex].Size = sizeof(IDDCX_MONITOR_MODE2);
			monitorModesOutput[ModeIndex].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
			monitorModesOutput[ModeIndex].MonitorVideoSignalInfo = s_KnownMonitorModes2[ModeIndex];

			if (ColourFormat == L"RGB")
			{
				monitorModesOutput[ModeIndex].BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr444")
			{
				monitorModesOutput[ModeIndex].BitsPerComponent.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr422")
			{
				monitorModesOutput[ModeIndex].BitsPerComponent.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr420")
			{
				monitorModesOutput[ModeIndex].BitsPerComponent.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
			}
			else
			{
				monitorModesOutput[ModeIndex].BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR; // Default to RGB
			}

		}

		VDD_LOG_DEBUG_LAZY([&]() -> string
		{
			stringstream logStream;
			logStream << "Writing monitor modes to output buffer:";
			for (DWORD ModeIndex = 0; ModeIndex < localModes.size(); ModeIndex++)
			{
				logStream << "\n  ModeIndex: " << ModeIndex
				          << "\n    Size: " << monitorModesOutput[ModeIndex].Size
				          << "\n    Origin: " << monitorModesOutput[ModeIndex].Origin
				          << "\n    Colour Format: " << WStringToString(ColourFormat);
			}
			return logStream.str();
		});

		// Set the preferred mode as represented in the EDID
		pOutArgs->PreferredMonitorModeIdx = 0;

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2(
		IDDCX_MONITOR MonitorObject,
		const IDARG_IN_QUERYTARGETMODES2 *pInArgs,
		IDARG_OUT_QUERYTARGETMODES *pOutArgs)
{
	// UNREFERENCED_PARAMETER(MonitorObject);
	VDD_LOG_DEBUG_STREAM("Querying target modes:"
	                     << "\n  MonitorObject Handle: " << static_cast<void *>(MonitorObject)
	                     << "\n  TargetModeBufferInputCount: " << pInArgs->TargetModeBufferInputCount);

	// Take a local snapshot of monitorModes under lock to prevent data races
	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
	}

	vector<IDDCX_TARGET_MODE2> TargetModes(localModes.size());

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	for (int i = 0; i < localModes.size(); i++)
	{
		CreateTargetMode2(TargetModes[i], std::get<0>(localModes[i]), std::get<1>(localModes[i]), std::get<2>(localModes[i]), std::get<3>(localModes[i]));
	}

	VDD_LOG_DEBUG_LAZY([&]() -> string
	{
		stringstream logStream;
		logStream << "Creating target modes:";
		for (int i = 0; i < localModes.size(); i++)
		{
			logStream << "\n  TargetModeIndex: " << i
			          << "\n    Width: " << std::get<0>(localModes[i])
			          << "\n    Height: " << std::get<1>(localModes[i])
			          << "\n    RefreshRate: " << std::get<2>(localModes[i]);
		}
		return logStream.str();
	});

	pOutArgs->TargetModeBufferOutputCount = (UINT)TargetModes.size();

	VDD_LOG_DEBUG_STREAM("Output target modes count: " << pOutArgs->TargetModeBufferOutputCount);

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);

		VDD_LOG_DEBUG_LAZY([&]() -> string
		{
			stringstream logStream;
			logStream << "Target modes copied to output buffer:";
			for (int i = 0; i < TargetModes.size(); i++)
			{
				logStream << "\n  TargetModeIndex: " << i
				          << "\n    Size: " << TargetModes[i].Size
				          << "\n    ColourFormat: " << WStringToString(ColourFormat);
			}
			return logStream.str();
		});
	}
	else
	{
		VDD_LOG_WARNING("Input buffer is too small for target modes.");
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxAdapterCommitModes2(
		IDDCX_ADAPTER AdapterObject,
		const IDARG_IN_COMMITMODES2 *pInArgs)
{
		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
		if (pContext && pContext->pContext)
		{
			pContext->pContext->CommitModes2(pInArgs);
		}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp(
		IDDCX_MONITOR MonitorObject,
		const IDARG_IN_SET_GAMMARAMP *pInArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}
