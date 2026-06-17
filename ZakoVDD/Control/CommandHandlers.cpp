#include "CommandDispatcher.h"
#include "CommandHandlers.h"

#include "..\Adapter\GpuAdapterSelection.h"
#include "..\Adapter\GpuStatus.h"
#include "..\Config\DriverSettings.h"
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
		vddlog("c", enableMsg);
		ReloadDriver(hPipe);
	}
	else if (wcsncmp(param, L"false", 5) == 0)
	{
		UpdateXmlToggleSetting(false, settingName);
		vddlog("c", disableMsg);
		ReloadDriver(hPipe);
	}
}
}

void ReloadDriver(HANDLE hPipe)
{
	UNREFERENCED_PARAMETER(hPipe);

	vddlog("i", "Starting driver reload process");

	if (g_GlobalDevice == nullptr)
	{
		vddlog("e", "Global device not available for reload");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		vddlog("e", "Invalid device context for driver reload");
		return;
	}

	try
	{
		LoadDriverSettings();

		UINT oldNumVirtualDisplays = numVirtualDisplays;
		vddlog("d", ("Saving current monitor count for cleanup: " + to_string(oldNumVirtualDisplays)).c_str());

		vddlog("d", "Cleaning up existing monitors before reload");
		if (pContext->pContext->HasActiveSwapChain())
		{
			vddlog("d", "Stopping active SwapChain processing before reload");
			pContext->pContext->UnassignAllSwapChains();
			Sleep(100);
		}

		if (pContext->pContext->HasActiveMonitor())
		{
			vddlog("d", "Destroying all existing monitors before reload");
			pContext->pContext->DestroyAllMonitors();
		}

		vddlog("d", "Waiting for system stabilization after cleanup");
		Sleep(200);

		vddlog("d", "Reinitializing adapter with new configuration");
		pContext->pContext->InitAdapter();

		vddlog("d", "Waiting for adapter initialization to stabilize");
		Sleep(100);

		vddlog("i", ("Driver reload completed successfully. Monitor count changed from " + to_string(oldNumVirtualDisplays) + " to " + to_string(numVirtualDisplays)).c_str());
	}
	catch (const exception &e)
	{
		stringstream errorStream;
		errorStream << "Exception during driver reload: " << e.what();
		vddlog("e", errorStream.str().c_str());

		try
		{
			LoadDriverSettings();
			pContext->pContext->InitAdapter();
			vddlog("w", "Adapter reinitialized after exception");
		}
		catch (...)
		{
			vddlog("e", "Failed to reinitialize adapter after exception");
		}
	}
	catch (...)
	{
		vddlog("e", "Unknown exception during driver reload");

		try
		{
			LoadDriverSettings();
			pContext->pContext->InitAdapter();
			vddlog("w", "Adapter reinitialized after unknown exception");
		}
		catch (...)
		{
			vddlog("e", "Failed to reinitialize adapter after unknown exception");
		}
	}
}

void HandleReloadDriverCommand(HANDLE hPipe, wchar_t *)
{
	vddlog("c", "Reloading the driver");
	ReloadDriver(hPipe);
}

void HandleLogDebugCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"debuglogging", "Pipe debugging enabled", "Debugging disabled");
}

void HandleLoggingCommand(HANDLE hPipe, wchar_t *param)
{
	ToggleSetting(hPipe, param, L"logging", "Logging Enabled", "Logging disabled");
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
		vddlog("e", "EDIDPROFILE requires a value: auto | legacy | modern");
		return;
	}

	wstring requested(param);
	if (!IsKnownEdidProfileSetting(requested))
	{
		vddlog("e", "EDIDPROFILE: unknown value (expected auto | legacy | modern)");
		return;
	}

	ApplyEdidProfileSetting(requested);
	vddlog("c", "EDID profile updated; recreate monitors to take effect");
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
	vddlog("c", "Retrieving D3D GPU (This information may be inaccurate without reloading the driver first)");
	InitializeD3DDeviceAndLogGPU();
	vddlog("c", "Retrieved D3D GPU");
}

void HandleIddCxVersionCommand(HANDLE, wchar_t *)
{
	vddlog("c", "Logging iddcx version");
	LogIddCxVersion();
}

void HandleGetAssignedGpuCommand(HANDLE, wchar_t *)
{
	vddlog("c", "Retrieving Assigned GPU");
	GetGpuInfo();
	vddlog("c", "Retrieved Assigned GPU");
}

void HandleGetAllGpusCommand(HANDLE, wchar_t *)
{
	vddlog("c", "Logging all GPUs");
	vddlog("i", "Any GPUs which show twice but you only have one, will most likely be the GPU the driver is attached to");
	LogAvailableGPUs();
	vddlog("c", "Logged all GPUs");
}

void HandleSetGpuCommand(HANDLE hPipe, wchar_t *param)
{
	wstring gpuName = param;
	gpuName = gpuName.substr(1, gpuName.size() - 2);

	string gpuNameNarrow = WStringToString(gpuName);

	vddlog("c", ("Setting GPU to: " + gpuNameNarrow).c_str());
	if (UpdateXmlGpuSetting(gpuName.c_str()))
	{
		vddlog("c", "Gpu Changed, Restarting Driver");
	}
	else
	{
		vddlog("e", "Failed to update GPU setting in XML. Restarting anyway");
	}
	ReloadDriver(hPipe);
}

void HandleSetDisplayCountCommand(HANDLE hPipe, wchar_t *param)
{
	vddlog("i", "Setting Display Count");

	int newDisplayCount = 1;
	swscanf_s(param, L"%d", &newDisplayCount);

	wstring displayLog = L"Setting display count  to " + to_wstring(newDisplayCount);
	vddlog("c", WStringToString(displayLog).c_str());

	if (UpdateXmlDisplayCountSetting(newDisplayCount))
	{
		vddlog("c", "Display Count Changed, Restarting Driver");
	}
	else
	{
		vddlog("e", "Failed to update display count setting in XML. Restarting anyway");
	}
	ReloadDriver(hPipe);
}

void HandleGetSettingsCommand(HANDLE hPipe, wchar_t *)
{
	bool debugEnabled = EnabledQuery(L"DebugLoggingEnabled");
	bool loggingEnabled = EnabledQuery(L"LoggingEnabled");

	wstring settingsResponse = L"SETTINGS ";
	settingsResponse += debugEnabled ? L"DEBUG=true " : L"DEBUG=false ";
	settingsResponse += loggingEnabled ? L"LOG=true" : L"LOG=false";

	if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL)
	{
		DWORD bytesWritten;
		DWORD bytesToWrite = static_cast<DWORD>((settingsResponse.length() + 1) * sizeof(wchar_t));
		WriteFile(hPipe, settingsResponse.c_str(), bytesToWrite, &bytesWritten, NULL);
	}
}

void HandlePingCommand(HANDLE, wchar_t *)
{
	SendToPipe("PONG");
	vddlog("p", "Heartbeat Ping");
}

void HandleCreateMonitorCommand(HANDLE, wchar_t *param)
{
	if (g_GlobalDevice == nullptr)
	{
		vddlog("e", "Global device not initialized");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		vddlog("e", "Failed to get device context for monitor creation");
		return;
	}

	if (numVirtualDisplays == 0)
	{
		vddlog("e", "Invalid display count: 0");
		return;
	}

	vddlog("i", "Starting monitor creation");

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

							stringstream ss;
							ss << "Parsed luminance - MaxNits: " << mp.maxNits
							   << ", MinNits: " << mp.minNits << ", MaxFALL: " << mp.maxFALL;
							vddlog("d", ss.str().c_str());
						}
					}
					else if (bracketIndex == 1)
					{
						if (values.size() >= 2)
						{
							mp.widthCm = values[0];
							mp.heightCm = values[1];

							stringstream ss;
							ss << "Parsed dimensions - Width: " << mp.widthCm
							   << " cm, Height: " << mp.heightCm << " cm";
							vddlog("d", ss.str().c_str());
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
					vddlog("d", ("Parsed client GUID: " + WStringToString(guidWithBraces)).c_str());
				}
				else
				{
					vddlog("w", ("Failed to parse GUID: " + WStringToString(guidStr)).c_str());
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
		vddlog("e", "Global device not initialized for monitor destruction");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		vddlog("e", "Failed to get device context for monitor destruction");
		return;
	}

	vddlog("i", "Starting monitor destruction process");
	vddlog("d", "Preparing system for monitor destruction");
	Sleep(50);

	try
	{
		pContext->pContext->DestroyAllMonitors();
		vddlog("d", "Allowing system to stabilize after monitor destruction");
		Sleep(100);
		vddlog("i", "All monitors destroyed successfully");
	}
	catch (const exception &e)
	{
		stringstream errorStream;
		errorStream << "Exception during monitor destruction: " << e.what();
		vddlog("e", errorStream.str().c_str());
		Sleep(200);
	}
	catch (...)
	{
		vddlog("e", "Unknown exception during monitor destruction");
		Sleep(200);
	}
}

void HandleRefreshModesCommand(HANDLE, wchar_t *)
{
	if (g_GlobalDevice == nullptr)
	{
		vddlog("e", "REFRESHMODES: global device not initialized");
		return;
	}

	lock_guard<mutex> lock(g_Mutex);
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
	if (!pContext || !pContext->pContext)
	{
		vddlog("e", "REFRESHMODES: invalid device context");
		return;
	}

	int n = pContext->pContext->RefreshMonitorModes();
	stringstream ss;
	ss << "REFRESHMODES: refreshed " << n << " monitor(s) without departure";
	vddlog("i", ss.str().c_str());
}

void HandleSetModesCommand(HANDLE, wchar_t *param)
{
	if (param == nullptr || *param == L'\0')
	{
		vddlog("e", "SETMODES: empty parameter");
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
			stringstream ss;
			ss << "SETMODES: skipping malformed token '" << WStringToString(token) << "'";
			vddlog("w", ss.str().c_str());
		}
	}

	if (parsed.empty())
	{
		vddlog("e", "SETMODES: no valid modes parsed; aborting");
		return;
	}

	{
		lock_guard<mutex> dataLock(g_DataMutex);
		monitorModes = parsed;
	}
	stringstream ss;
	ss << "SETMODES: applied " << parsed.size() << " modes (in-memory only)";
	vddlog("i", ss.str().c_str());

	if (g_GlobalDevice != nullptr)
	{
		lock_guard<mutex> lock(g_Mutex);
		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
		if (pContext && pContext->pContext)
		{
			int n = pContext->pContext->RefreshMonitorModes();
			stringstream s2;
			s2 << "SETMODES: pushed to " << n << " live monitor(s)";
			vddlog("i", s2.str().c_str());
		}
	}
}

void HandleUnknownCommand(HANDLE, wchar_t *buffer)
{
	vddlog("e", "Unknown command");
	vddlog("e", WStringToString(buffer).c_str());
}
