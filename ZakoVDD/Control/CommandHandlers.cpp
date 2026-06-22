#include "CommandDispatcher.h"
#include "CommandHandlers.h"

#include "..\Adapter\GpuAdapterSelection.h"
#include "..\Adapter\GpuStatus.h"
#include "..\Config\DriverSettings.h"
#include "..\Control\ControlTransport.h"
#include "..\Device\IndirectDeviceContextWrapper.h"
#include "..\Diagnostics\DriverDiagnostics.h"
#include "..\Core\DriverState.h"
#include "..\Edid\Edid.h"
#include "..\Logging\Logger.h"
#include "..\Util\RefreshRate.h"
#include "..\Util\StringConversion.h"

#include <cwchar>
#include <exception>
#include <mutex>
#include <objbase.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace std;

namespace
{
void ToggleSetting(HANDLE hPipe, wchar_t *param, const wchar_t *settingName, const char *enableMsg, const char *disableMsg)
{
	if (wcsncmp(param, L"true", 4) == 0)
	{
		UpdateXmlToggleSetting(true, settingName);
		VDD_LOG_INFO(enableMsg);
		ReloadDriver(hPipe);
	}
	else if (wcsncmp(param, L"false", 5) == 0)
	{
		UpdateXmlToggleSetting(false, settingName);
		VDD_LOG_INFO(disableMsg);
		ReloadDriver(hPipe);
	}
}
}

void ReloadDriver(HANDLE hPipe)
{
	UNREFERENCED_PARAMETER(hPipe);

	VDD_LOG_INFO("Starting driver reload process");

	if (g_GlobalDevice == nullptr)
	{
		VDD_LOG_ERROR("Global device not available for reload");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		VDD_LOG_ERROR("Invalid device context for driver reload");
		return;
	}

	try
	{
		LoadDriverSettings();

		UINT oldNumVirtualDisplays = numVirtualDisplays;
		VDD_LOG_DEBUG_STREAM("Saving current monitor count for cleanup: " << oldNumVirtualDisplays);

		VDD_LOG_DEBUG("Cleaning up existing monitors before reload");
		if (pContext->pContext->HasActiveSwapChain())
		{
			VDD_LOG_DEBUG("Stopping active SwapChain processing before reload");
			pContext->pContext->UnassignAllSwapChains();
			Sleep(100);
		}

		if (pContext->pContext->HasActiveMonitor())
		{
			VDD_LOG_DEBUG("Destroying all existing monitors before reload");
			pContext->pContext->DestroyAllMonitors();
		}

		VDD_LOG_DEBUG("Waiting for system stabilization after cleanup");
		Sleep(200);

		VDD_LOG_DEBUG("Reinitializing adapter with new configuration");
		pContext->pContext->InitAdapter();

		VDD_LOG_DEBUG("Waiting for adapter initialization to stabilize");
		Sleep(100);

		VDD_LOG_INFO(("Driver reload completed successfully. Monitor count changed from " + to_string(oldNumVirtualDisplays) + " to " + to_string(numVirtualDisplays)).c_str());
	}
		catch (const exception &e)
		{
			VDD_LOG_ERROR_STREAM("Exception during driver reload: " << e.what());

		try
		{
			LoadDriverSettings();
			pContext->pContext->InitAdapter();
			VDD_LOG_WARNING("Adapter reinitialized after exception");
		}
		catch (...)
		{
			VDD_LOG_ERROR("Failed to reinitialize adapter after exception");
		}
	}
	catch (...)
	{
		VDD_LOG_ERROR("Unknown exception during driver reload");

		try
		{
			LoadDriverSettings();
			pContext->pContext->InitAdapter();
			VDD_LOG_WARNING("Adapter reinitialized after unknown exception");
		}
		catch (...)
		{
			VDD_LOG_ERROR("Failed to reinitialize adapter after unknown exception");
		}
	}
}

void HandleReloadDriverCommand(HANDLE hPipe, wchar_t *)
{
	VDD_LOG_INFO("Reloading the driver");
	ReloadDriver(hPipe);
}

void HandleHdrPlusCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"HDRPlus", "HDR+ Enabled", "HDR+ Disabled");
}

void HandleSdr10Command(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"SDR10bit", "SDR 10 Bit Enabled", "SDR 10 Bit Disabled");
}

void HandleCustomEdidCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"CustomEdid", "Custom Edid Enabled", "Custom Edid Disabled");
}

void HandlePreventSpoofCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"PreventSpoof", "Prevent Spoof Enabled", "Prevent Spoof Disabled");
}

void HandleCeaOverrideCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"EdidCeaOverride", "Cea override Enabled", "Cea override Disabled");
}

void HandleEdidProfileCommand(HANDLE, wchar_t *param)
{
	if (!param || *param == 0)
	{
		VDD_LOG_ERROR("EDIDPROFILE requires a value: auto | legacy | modern");
		return;
	}

	wstring requested(param);
	if (!IsKnownEdidProfileSetting(requested))
	{
		VDD_LOG_ERROR("EDIDPROFILE: unknown value (expected auto | legacy | modern)");
		return;
	}

	ApplyEdidProfileSetting(requested);
	VDD_LOG_INFO("EDID profile updated; recreate monitors to take effect");
}

void HandleVrrCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"Vrr", "VRR Enabled", "VRR Disabled");
}

void HandleHardwareCursorCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"HardwareCursor", "Hardware Cursor Enabled", "Hardware Cursor Disabled");
}

void HandleD3DDeviceGpuCommand(HANDLE, wchar_t *)
{
	VDD_LOG_INFO("Retrieving D3D GPU (This information may be inaccurate without reloading the driver first)");
	InitializeD3DDeviceAndLogGPU();
	VDD_LOG_INFO("Retrieved D3D GPU");
}

void HandleIddCxVersionCommand(HANDLE, wchar_t *)
{
	VDD_LOG_INFO("Logging iddcx version");
	LogIddCxVersion();
}

void HandleGetAssignedGpuCommand(HANDLE, wchar_t *)
{
	VDD_LOG_INFO("Retrieving Assigned GPU");
	GetGpuInfo();
	VDD_LOG_INFO("Retrieved Assigned GPU");
}

void HandleGetAllGpusCommand(HANDLE, wchar_t *)
{
	VDD_LOG_INFO("Logging all GPUs");
	VDD_LOG_INFO("Any GPUs which show twice but you only have one, will most likely be the GPU the driver is attached to");
	LogAvailableGPUs();
	VDD_LOG_INFO("Logged all GPUs");
}

void HandleSetGpuCommand(HANDLE hPipe, wchar_t *param)
{
	wstring gpuName = param;
	gpuName = gpuName.substr(1, gpuName.size() - 2);

	string gpuNameNarrow = WStringToString(gpuName);

	VDD_LOG_INFO(("Setting GPU to: " + gpuNameNarrow).c_str());
	if (UpdateXmlGpuSetting(gpuName.c_str()))
	{
		VDD_LOG_INFO("Gpu Changed, Restarting Driver");
	}
	else
	{
		VDD_LOG_ERROR("Failed to update GPU setting in XML. Restarting anyway");
	}
	ReloadDriver(hPipe);
}

void HandleSetDisplayCountCommand(HANDLE hPipe, wchar_t *param)
{
	VDD_LOG_INFO("Setting Display Count");

	int newDisplayCount = 1;
	swscanf_s(param, L"%d", &newDisplayCount);

	wstring displayLog = L"Setting display count  to " + to_wstring(newDisplayCount);
	VDD_LOG_INFO(WStringToString(displayLog).c_str());

	if (UpdateXmlDisplayCountSetting(newDisplayCount))
	{
		VDD_LOG_INFO("Display Count Changed, Restarting Driver");
	}
	else
	{
		VDD_LOG_ERROR("Failed to update display count setting in XML. Restarting anyway");
	}
	ReloadDriver(hPipe);
}

void HandleGetSettingsCommand(HANDLE hPipe, wchar_t *)
{
	wstring settingsResponse = L"SETTINGS LOG=ETW";

	if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL)
	{
		DWORD bytesWritten;
		DWORD bytesToWrite = static_cast<DWORD>((settingsResponse.length() + 1) * sizeof(wchar_t));
		WriteFile(hPipe, settingsResponse.c_str(), bytesToWrite, &bytesWritten, NULL);
	}
}

void HandlePingCommand(HANDLE, wchar_t *)
{
	SendLegacyPipeMessage("PONG");
	VDD_LOG_VERBOSE("Heartbeat Ping");
}

void HandleCreateMonitorCommand(HANDLE, wchar_t *param)
{
	if (g_GlobalDevice == nullptr)
	{
		VDD_LOG_ERROR("Global device not initialized");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		VDD_LOG_ERROR("Failed to get device context for monitor creation");
		return;
	}

	if (numVirtualDisplays == 0)
	{
		VDD_LOG_ERROR("Invalid display count: 0");
		return;
	}

	VDD_LOG_INFO("Starting monitor creation");

	struct MonitorParams
	{
		GUID guid{};
		bool hasGuid = false;
		float maxNits = 1000.0f;
		float minNits = 0.0001f;
		float maxFALL = 1000.0f;
		float widthCm = 0.0f;
		float heightCm = 0.0f;
	};
	vector<MonitorParams> monitorParams;

	if (param && wcslen(param) > 0)
	{
		wstringstream wss(param);
		wstring token;

		while (wss >> token)
		{
			MonitorParams mp;
			size_t colonPos = token.find(L':');
			wstring guidStr = (colonPos != wstring::npos) ? token.substr(0, colonPos) : token;

			if (colonPos != wstring::npos)
			{
				wstring settingsStr = token.substr(colonPos + 1);
				size_t pos = 0;
				int bracketIndex = 0;

				while (pos < settingsStr.length())
				{
					size_t openBracket = settingsStr.find(L'[', pos);
					if (openBracket == wstring::npos)
					{
						break;
					}

					size_t closeBracket = settingsStr.find(L']', openBracket);
					if (closeBracket == wstring::npos)
					{
						break;
					}

					wstring innerStr = settingsStr.substr(openBracket + 1, closeBracket - openBracket - 1);
					wstringstream valueStream(innerStr);
					wstring val;
					vector<float> values;

					while (getline(valueStream, val, L','))
					{
						try
						{
							values.push_back(stof(val));
						}
						catch (...)
						{
							break;
						}
					}

					if (bracketIndex == 0)
					{
						if (values.size() >= 2)
						{
							mp.maxNits = values[0];
							mp.minNits = values[1];
							mp.maxFALL = (values.size() >= 3) ? values[2] : values[0];

							VDD_LOG_DEBUG_STREAM("Parsed luminance - MaxNits: " << mp.maxNits
							                     << ", MinNits: " << mp.minNits << ", MaxFALL: " << mp.maxFALL);
						}
					}
					else if (bracketIndex == 1)
					{
						if (values.size() >= 2)
						{
							mp.widthCm = values[0];
							mp.heightCm = values[1];

							VDD_LOG_DEBUG_STREAM("Parsed dimensions - Width: " << mp.widthCm
							                     << " cm, Height: " << mp.heightCm << " cm");
						}
					}

					bracketIndex++;
					pos = closeBracket + 1;
				}
			}

			if (!guidStr.empty())
			{
				wstring guidWithBraces = guidStr;
				if (guidWithBraces.front() != L'{')
				{
					guidWithBraces = L"{" + guidWithBraces;
				}
				if (guidWithBraces.back() != L'}')
				{
					guidWithBraces += L"}";
				}

				if (SUCCEEDED(CLSIDFromString(guidWithBraces.c_str(), &mp.guid)))
				{
					mp.hasGuid = true;
					VDD_LOG_DEBUG_STREAM("Parsed client GUID: " << WStringToString(guidWithBraces));
				}
				else
				{
					VDD_LOG_WARNING(("Failed to parse GUID: " + WStringToString(guidStr)).c_str());
				}
			}

			monitorParams.push_back(mp);
		}
	}

	for (unsigned int i = 0; i < numVirtualDisplays; i++)
	{
		const GUID *pGuid = nullptr;
		float maxNits = 1000.0f, minNits = 0.0001f, maxFALL = 1000.0f;
		float widthCm = 0.0f, heightCm = 0.0f;

		if (i < monitorParams.size())
		{
			const auto &mp = monitorParams[i];
			if (mp.hasGuid)
			{
				pGuid = &mp.guid;
			}
			maxNits = mp.maxNits;
			minNits = mp.minNits;
			maxFALL = mp.maxFALL;
			widthCm = mp.widthCm;
			heightCm = mp.heightCm;
		}
		pContext->pContext->CreateMonitor(i, pGuid, maxNits, minNits, maxFALL, widthCm, heightCm);
	}
}

void HandleDestroyMonitorCommand(HANDLE, wchar_t *)
{
	if (g_GlobalDevice == nullptr)
	{
		VDD_LOG_ERROR("Global device not initialized for monitor destruction");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		VDD_LOG_ERROR("Failed to get device context for monitor destruction");
		return;
	}

	VDD_LOG_INFO("Starting monitor destruction process");
	VDD_LOG_DEBUG("Preparing system for monitor destruction");
	Sleep(50);

	try
	{
		pContext->pContext->DestroyAllMonitors();
		VDD_LOG_DEBUG("Allowing system to stabilize after monitor destruction");
		Sleep(100);
		VDD_LOG_INFO("All monitors destroyed successfully");
	}
	catch (const exception &e)
	{
		VDD_LOG_ERROR_STREAM("Exception during monitor destruction: " << e.what());
		Sleep(200);
	}
	catch (...)
	{
		VDD_LOG_ERROR("Unknown exception during monitor destruction");
		Sleep(200);
	}
}

void HandleRefreshModesCommand(HANDLE, wchar_t *)
{
	if (g_GlobalDevice == nullptr)
	{
		VDD_LOG_ERROR("REFRESHMODES: global device not initialized");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		VDD_LOG_ERROR("REFRESHMODES: invalid device context");
		return;
	}

	int n = pContext->pContext->RefreshMonitorModes();
	VDD_LOG_INFO_STREAM("REFRESHMODES: refreshed " << n << " monitor(s) without departure");
}

void HandleSetModesCommand(HANDLE, wchar_t *param)
{
	if (param == nullptr || *param == L'\0')
	{
		VDD_LOG_ERROR("SETMODES: empty parameter");
		return;
	}

	vector<tuple<int, int, int, int>> parsed;
	wstring input(param);
	size_t pos = 0;
	while (pos < input.size())
	{
		size_t comma = input.find(L',', pos);
		wstring token = input.substr(pos, comma == wstring::npos ? wstring::npos : comma - pos);
		pos = (comma == wstring::npos) ? input.size() : comma + 1;

		int w = 0, h = 0, r = 0;
		if (swscanf_s(token.c_str(), L"%dx%dx%d", &w, &h, &r) == 3 && w > 0 && h > 0 && r > 0)
		{
			int vnum = 0, vden = 0;
			float_to_vsync(static_cast<float>(r), vnum, vden);
			parsed.emplace_back(w, h, vnum, vden);
		}
		else
		{
			VDD_LOG_WARNING_STREAM("SETMODES: skipping malformed token '" << WStringToString(token) << "'");
		}
	}

	if (parsed.empty())
	{
		VDD_LOG_ERROR("SETMODES: no valid modes parsed; aborting");
		return;
	}

	{
		lock_guard<mutex> dataLock(g_DataMutex);
		monitorModes = parsed;
	}
	VDD_LOG_INFO_STREAM("SETMODES: applied " << parsed.size() << " modes (in-memory only)");

	if (g_GlobalDevice != nullptr)
	{
		lock_guard<mutex> lock(g_Mutex);
		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
		if (pContext && pContext->pContext)
		{
			int n = pContext->pContext->RefreshMonitorModes();
			VDD_LOG_INFO_STREAM("SETMODES: pushed to " << n << " live monitor(s)");
		}
	}
}

void HandleUnknownCommand(HANDLE, wchar_t *buffer)
{
	VDD_LOG_ERROR("Unknown command");
	VDD_LOG_ERROR(WStringToString(buffer).c_str());
}
