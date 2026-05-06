/*++

Copyright (c) Microsoft Corporation

Abstract:

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"
// #include "Driver.tmh"
#include "DefaultEdid.h"
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <AdapterOption.h>
#include <vdd_control_ioctl.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <atlbase.h>
#include <iostream>
#include <cstdlib>
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <sddl.h>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <cerrno>
#include <locale>
#include <cwchar>
#include <map>
#include <set>
#include <atomic>

// =====================================================================
// Transitional named-pipe transport
// ---------------------------------------------------------------------
// All pipe-related code in this translation unit is marked with the
// tag [LEGACY-PIPE] in a leading comment so it can be removed in a
// single mechanical pass once every Sunshine release in the wild
// speaks IOCTL natively. To strip:
//   1. grep -nE '\[LEGACY-PIPE\]' Driver.cpp and delete each tagged
//      block (signature + body, plus the call site in DriverEntry /
//      EvtDriverUnload).
//   2. Drop PIPE_NAME, hPipeThread, g_Running (pipe-only), g_pipeHandle,
//      sendLogsThroughPipe, SendToPipe, the SendLogsThroughPipe registry
//      hook, HandleClient, StartNamedPipeServer, StopNamedPipeServer.
//   3. Drop the `hPipeForResponse` parameter on DispatchVddCommandBuffer
//      and remove the GETSETTINGS WriteFile guarded branch.
// The IOCTL transport (VirtualDisplayDriverIoDeviceControl +
// GUID_DEVINTERFACE_ZAKO_VDD_CONTROL) and DispatchVddCommandBuffer
// remain untouched.
// =====================================================================

// [LEGACY-PIPE]
#define PIPE_NAME L"\\\\.\\pipe\\ZakoVDDPipe"

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

// [LEGACY-PIPE]
HANDLE hPipeThread = NULL;
std::atomic<bool> g_Running{true};
mutex g_Mutex;
mutex g_DataMutex; // Protects monitorModes, s_KnownMonitorModes2, numVirtualDisplays, gpuname
// [LEGACY-PIPE]
HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
WDFDEVICE g_GlobalDevice = nullptr;

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

void vddlog(const char *type, const char *message);

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD VirtualDisplayDriverDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY VirtualDisplayDriverDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT VirtualDisplayDriverDeviceD0Exit;

EVT_IDD_CX_DEVICE_IO_CONTROL VirtualDisplayDriverIoDeviceControl;

EVT_IDD_CX_ADAPTER_INIT_FINISHED VirtualDisplayDriverAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES VirtualDisplayDriverAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION VirtualDisplayDriverParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES VirtualDisplayDriverMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES VirtualDisplayDriverMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 VirtualDisplayDriverEvtIddCxParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 VirtualDisplayDriverEvtIddCxAdapterCommitModes2;

EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp;

struct
{
	AdapterOption Adapter;
} Options;
vector<tuple<int, int, int, int>> monitorModes;
vector<DISPLAYCONFIG_VIDEO_SIGNAL_INFO> s_KnownMonitorModes2;
UINT numVirtualDisplays = 1;
wstring gpuname = L"";
wstring confpath = []()
{
	wchar_t sysDrive[MAX_PATH];
	DWORD len = GetEnvironmentVariableW(L"SystemDrive", sysDrive, MAX_PATH);
	if (len == 0 || len > MAX_PATH)
	{
		return wstring(L"C:\\VirtualDisplayDriver");
	}
	return wstring(sysDrive) + L"\\VirtualDisplayDriver";
}();
std::atomic<bool> logsEnabled{false};
std::atomic<bool> debugLogs{false};

// Cached log file handle for performance optimization
static FILE *s_cachedLogFile = nullptr;
static wstring s_cachedLogDate;
static mutex s_logFileMutex;

// Get fallback log directory when confpath is not writable
static wstring GetFallbackLogDir()
{
	// Try ProgramData first (usually writable for all users)
	wchar_t programDataPath[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, SHGFP_TYPE_CURRENT, programDataPath)))
	{
		return wstring(programDataPath) + L"\\VirtualDisplayDriver\\Logs";
	}
	
	// Fallback to TEMP if ProgramData fails
	wchar_t tempPath[MAX_PATH];
	DWORD len = GetTempPathW(MAX_PATH, tempPath);
	if (len > 0 && len < MAX_PATH)
	{
		return wstring(tempPath) + L"VirtualDisplayDriver\\Logs";
	}
	
	// Last resort: use C:\Windows\Temp
	return wstring(L"C:\\Windows\\Temp\\VirtualDisplayDriver\\Logs");
}
std::atomic<bool> HDRPlus{false};
std::atomic<bool> SDR10{false};
std::atomic<bool> customEdid{false};

// EDID profile resolved at DriverEntry (Auto -> Legacy/Modern based on host
// OS) and re-read whenever the IOCTL EDIDPROFILE command lands. New monitors
// pick up the latest value via GetHardcodedEdid(); existing monitors keep
// the bytes they were created with until they are recreated.
std::atomic<int> gEdidProfile{static_cast<int>(VddEdid::Profile::Modern)};

// Variable Refresh Rate (FreeSync / G-Sync compatible) toggle. When enabled,
// the adapter caps include IDDCX_ADAPTER_FLAGS_VARIABLE_REFRESH_RATE_SUPPORTED
// (added in IddCx 1.4); IddCx silently ignores unknown flag bits on older
// hosts so this is safe to declare unconditionally, but the user-facing
// toggle still defaults to OFF until we also publish the EDID FreeSync
// Range Block (see ROADMAP P1).
std::atomic<bool> vrrEnabled{false};
std::atomic<bool> hardwareCursor{false};
std::atomic<bool> preventManufacturerSpoof{false};
std::atomic<bool> edidCeaOverride{false};
// [LEGACY-PIPE]
std::atomic<bool> sendLogsThroughPipe{true};

// Mouse settings
std::atomic<bool> alphaCursorSupport{true};
int CursorMaxX = 128;
int CursorMaxY = 128;
IDDCX_XOR_CURSOR_SUPPORT XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;

// Rest
IDDCX_BITS_PER_COMPONENT SDRCOLOUR = IDDCX_BITS_PER_COMPONENT_8;
IDDCX_BITS_PER_COMPONENT HDRCOLOUR = IDDCX_BITS_PER_COMPONENT_10;

wstring ColourFormat = L"RGB";

std::map<std::wstring, std::pair<std::wstring, std::wstring>> SettingsQueryMap = {
	{L"LoggingEnabled", {L"LOGS", L"logging"}},
	{L"DebugLoggingEnabled", {L"DEBUGLOGS", L"debuglogging"}},
	{L"CustomEdidEnabled", {L"CUSTOMEDID", L"CustomEdid"}},

	{L"PreventMonitorSpoof", {L"PREVENTMONITORSPOOF", L"PreventSpoof"}},
	{L"EdidCeaOverride", {L"EDIDCEAOVERRIDE", L"EdidCeaOverride"}},
	{L"SendLogsThroughPipe", {L"SENDLOGSTHROUGHPIPE", L"SendLogsThroughPipe"}},
	// Cursor Begin
	{L"HardwareCursorEnabled", {L"HARDWARECURSOR", L"HardwareCursor"}},
	{L"AlphaCursorSupport", {L"ALPHACURSORSUPPORT", L"AlphaCursorSupport"}},
	{L"CursorMaxX", {L"CURSORMAXX", L"CursorMaxX"}},
	{L"CursorMaxY", {L"CURSORMAXY", L"CursorMaxY"}},
	{L"XorCursorSupportLevel", {L"XORCURSORSUPPORTLEVEL", L"XorCursorSupportLevel"}},
	/// Cursor End
	// Colour Begin
	{L"HDRPlusEnabled", {L"HDRPLUS", L"HDRPlus"}},
	{L"SDR10Enabled", {L"SDR10BIT", L"SDR10bit"}},
	{L"ColourFormat", {L"COLOURFORMAT", L"ColourFormat"}},
	{L"EdidProfile", {L"EDIDPROFILE", L"EdidProfile"}},
	{L"VrrEnabled", {L"VRR", L"Vrr"}},
	// Colour End
};

const char *XorCursorSupportLevelToString(IDDCX_XOR_CURSOR_SUPPORT level)
{
	switch (level)
	{
	case IDDCX_XOR_CURSOR_SUPPORT_UNINITIALIZED:
		return "IDDCX_XOR_CURSOR_SUPPORT_UNINITIALIZED";
	case IDDCX_XOR_CURSOR_SUPPORT_NONE:
		return "IDDCX_XOR_CURSOR_SUPPORT_NONE";
	case IDDCX_XOR_CURSOR_SUPPORT_FULL:
		return "IDDCX_XOR_CURSOR_SUPPORT_FULL";
	case IDDCX_XOR_CURSOR_SUPPORT_EMULATION:
		return "IDDCX_XOR_CURSOR_SUPPORT_EMULATION";
	default:
		return "Unknown";
	}
}

// Resolve Auto EDID profile by querying the host OS build number via
// ntdll!RtlGetVersion. We avoid GetVersionExW because it lies on Win10+
// without an explicit application manifest. Build < 22000 is treated as
// Win10 (or older) and routed to the Legacy profile to dodge issue #612.
// Anything else (including any future Windows release) gets the Modern
// profile so HDR / wide gamut declarations stay enabled.
static VddEdid::Profile DetectAutoEdidProfile()
{
	typedef LONG (NTAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
	{
		vddlog("w", "DetectAutoEdidProfile: ntdll handle missing, defaulting to Modern");
		return VddEdid::Profile::Modern;
	}
	auto pRtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
	if (!pRtlGetVersion)
	{
		vddlog("w", "DetectAutoEdidProfile: RtlGetVersion missing, defaulting to Modern");
		return VddEdid::Profile::Modern;
	}
	RTL_OSVERSIONINFOW info{};
	info.dwOSVersionInfoSize = sizeof(info);
	if (pRtlGetVersion(&info) != 0)
	{
		vddlog("w", "DetectAutoEdidProfile: RtlGetVersion failed, defaulting to Modern");
		return VddEdid::Profile::Modern;
	}
	// Win11 starts at build 22000.
	const bool isWin10OrOlder = (info.dwMajorVersion < 10) ||
		(info.dwMajorVersion == 10 && info.dwBuildNumber < 22000);
	stringstream ss;
	ss << "DetectAutoEdidProfile: build=" << info.dwBuildNumber
	   << " -> " << (isWin10OrOlder ? "Legacy" : "Modern");
	vddlog("i", ss.str().c_str());
	return isWin10OrOlder ? VddEdid::Profile::Legacy : VddEdid::Profile::Modern;
}

// Apply an EdidProfile setting value (Auto/Legacy/Modern, case-insensitive)
// to the global gEdidProfile. Auto is resolved here so callers further down
// can read gEdidProfile without having to repeat OS detection.
static void ApplyEdidProfileSetting(const std::wstring& settingValue)
{
	auto requested = VddEdid::ProfileFromString(settingValue);
	auto effective = (requested == VddEdid::Profile::Auto)
		? DetectAutoEdidProfile()
		: requested;
	gEdidProfile.store(static_cast<int>(effective));
	stringstream ss;
	ss << "EDID profile applied: requested=";
	ss << WStringToString(VddEdid::ProfileToString(requested));
	ss << " effective=";
	ss << WStringToString(VddEdid::ProfileToString(effective));
	vddlog("i", ss.str().c_str());
}

vector<unsigned char> Microsoft::IndirectDisp::IndirectDeviceContext::s_KnownMonitorEdid; // Changed to support static vector

// Custom comparator for GUID to use in map
struct GuidComparator
{
	bool operator()(const GUID &lhs, const GUID &rhs) const
	{
		if (lhs.Data1 != rhs.Data1)
			return lhs.Data1 < rhs.Data1;
		if (lhs.Data2 != rhs.Data2)
			return lhs.Data2 < rhs.Data2;
		if (lhs.Data3 != rhs.Data3)
			return lhs.Data3 < rhs.Data3;
		for (int i = 0; i < 8; i++)
		{
			if (lhs.Data4[i] != rhs.Data4[i])
				return lhs.Data4[i] < rhs.Data4[i];
		}
		return false; // Equal
	}
};

// Static map to store EDID copies for each client GUID to ensure data persistence
// Key: GUID, Value: EDID data
static map<GUID, vector<BYTE>, GuidComparator> s_ClientGuidEdidMap;
static mutex s_EdidMapMutex;

// Structure to store HDR luminance settings per monitor
struct MonitorHdrSettings
{
	bool isHdr;     // Whether the OS has enabled HDR for this monitor
	float maxNits; // Maximum luminance in nits (Max CLL - Content Light Level)
	float minNits; // Minimum luminance in nits
	float maxFALL; // Maximum Frame-Average Light Level in nits
};

// Static map to store HDR settings for each monitor (by IDDCX_MONITOR handle)
static map<IDDCX_MONITOR, MonitorHdrSettings> s_MonitorHdrSettingsMap;
static mutex s_HdrSettingsMutex;

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext *pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};
void LogQueries(const char *severity, const std::wstring &xmlName)
{
	if (xmlName.find(L"logging") == std::wstring::npos)
	{
		int size_needed = WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), (int)xmlName.size(), NULL, 0, NULL, NULL);
		if (size_needed > 0)
		{
			std::string strMessage(size_needed, 0);
			WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), (int)xmlName.size(), &strMessage[0], size_needed, NULL, NULL);
			vddlog(severity, strMessage.c_str());
		}
	}
}

string WStringToString(const wstring &wstr)
{ // basically just a function for converting strings since codecvt is depricated in c++ 17
	if (wstr.empty())
		return "";

	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
	string str(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
	return str;
}

bool EnabledQuery(const std::wstring &settingKey)
{
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end())
	{
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return false;
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwValue;
	DWORD dwBufferSize = sizeof(dwValue);
	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);

	if (lResult == ERROR_SUCCESS)
	{
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)&dwValue, &dwBufferSize);
		if (lResult == ERROR_SUCCESS)
		{
			RegCloseKey(hKey);
			if (dwValue == 1)
			{
				LogQueries("d", xmlName + L" - is enabled (value = 1).");
				return true;
			}
			else if (dwValue == 0)
			{
				goto check_xml;
			}
		}
		else
		{
			LogQueries("d", xmlName + L" - Failed to retrieve value from registry. Attempting to read as string.");
			wchar_t path[MAX_PATH];
			dwBufferSize = sizeof(path);
			lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)path, &dwBufferSize);
			if (lResult == ERROR_SUCCESS)
			{
				std::wstring logValue(path);
				RegCloseKey(hKey);
				if (logValue == L"true" || logValue == L"1")
				{
					LogQueries("d", xmlName + L" - is enabled (string value).");
					return true;
				}
				else if (logValue == L"false" || logValue == L"0")
				{
					goto check_xml;
				}
			}
			RegCloseKey(hKey);
			LogQueries("d", xmlName + L" - Failed to retrieve string value from registry.");
		}
	}

check_xml:
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, nullptr);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return false;
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t *pwszLocalName;
	bool xmlLoggingValue = false;

	while (S_OK == pReader->Read(&nodeType))
	{
		if (nodeType == XmlNodeType_Element)
		{
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, xmlName.c_str()) == 0)
			{
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text)
				{
					const wchar_t *pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					xmlLoggingValue = (wcscmp(pwszValue, L"true") == 0);
					LogQueries("i", xmlName + (xmlLoggingValue ? L" is enabled." : L" is disabled."));
					break;
				}
			}
		}
	}

	return xmlLoggingValue;
}

int GetIntegerSetting(const std::wstring &settingKey)
{
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end())
	{
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return -1;
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwValue;
	DWORD dwBufferSize = sizeof(dwValue);
	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);

	if (lResult == ERROR_SUCCESS)
	{
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)&dwValue, &dwBufferSize);
		if (lResult == ERROR_SUCCESS)
		{
			RegCloseKey(hKey);
			LogQueries("d", xmlName + L" - Retrieved integer value: " + std::to_wstring(dwValue));
			return static_cast<int>(dwValue);
		}
		else
		{
			LogQueries("d", xmlName + L" - Failed to retrieve integer value from registry. Attempting to read as string.");
			wchar_t path[MAX_PATH];
			dwBufferSize = sizeof(path);
			lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)path, &dwBufferSize);
			RegCloseKey(hKey);
			if (lResult == ERROR_SUCCESS)
			{
				try
				{
					int logValue = std::stoi(path);
					LogQueries("d", xmlName + L" - Retrieved string value: " + std::to_wstring(logValue));
					return logValue;
				}
				catch (const std::exception &)
				{
					LogQueries("d", xmlName + L" - Failed to convert registry string value to integer.");
				}
			}
		}
	}

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return -1;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, nullptr);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return -1;
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return -1;
	}

	XmlNodeType nodeType;
	const wchar_t *pwszLocalName;
	int xmlLoggingValue = -1;

	while (S_OK == pReader->Read(&nodeType))
	{
		if (nodeType == XmlNodeType_Element)
		{
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, xmlName.c_str()) == 0)
			{
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text)
				{
					const wchar_t *pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					try
					{
						xmlLoggingValue = std::stoi(pwszValue);
						LogQueries("i", xmlName + L" - Retrieved from XML: " + std::to_wstring(xmlLoggingValue));
					}
					catch (const std::exception &)
					{
						LogQueries("d", xmlName + L" - Failed to convert XML string value to integer.");
					}
					break;
				}
			}
		}
	}

	return xmlLoggingValue;
}

std::wstring GetStringSetting(const std::wstring &settingKey)
{
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end())
	{
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return L"";
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwBufferSize = MAX_PATH;
	wchar_t buffer[MAX_PATH];

	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS)
	{
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)buffer, &dwBufferSize);
		RegCloseKey(hKey);

		if (lResult == ERROR_SUCCESS)
		{
			LogQueries("d", xmlName + L" - Retrieved string value from registry: " + buffer);
			return std::wstring(buffer);
		}
		else
		{
			LogQueries("d", xmlName + L" - Failed to retrieve string value from registry. Attempting to read as XML.");
		}
	}

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return L"";
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, nullptr);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return L"";
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return L"";
	}

	XmlNodeType nodeType;
	const wchar_t *pwszLocalName;
	std::wstring xmlLoggingValue = L"";

	while (S_OK == pReader->Read(&nodeType))
	{
		if (nodeType == XmlNodeType_Element)
		{
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, xmlName.c_str()) == 0)
			{
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text)
				{
					const wchar_t *pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					xmlLoggingValue = pwszValue;
					LogQueries("i", xmlName + L" - Retrieved from XML: " + xmlLoggingValue);
					break;
				}
			}
		}
	}

	return xmlLoggingValue;
}

int gcd(int a, int b)
{
	while (b != 0)
	{
		int temp = b;
		b = a % b;
		a = temp;
	}
	return a;
}

void float_to_vsync(float refresh_rate, int &num, int &den)
{
	den = 10000;

	num = static_cast<int>(round(refresh_rate * den));

	int divisor = gcd(num, den);
	num /= divisor;
	den /= divisor;
}

// [LEGACY-PIPE] entire function
void SendToPipe(const std::string &logMessage)
{
	if (g_pipeHandle != INVALID_HANDLE_VALUE)
	{
		DWORD bytesWritten;
		DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
		WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
	}
}

void vddlog(const char *type, const char *message)
{
	// Early return if logging is disabled - check before any string operations
	if (!logsEnabled)
	{
		// Close cached file handle if it exists
		lock_guard<mutex> lock(s_logFileMutex);
		if (s_cachedLogFile != nullptr)
		{
			fclose(s_cachedLogFile);
			s_cachedLogFile = nullptr;
			s_cachedLogDate.clear();
		}
		return;
	}

	// Get current date string
	auto now = chrono::system_clock::now();
	auto in_time_t = chrono::system_clock::to_time_t(now);
	tm tm_buf;
	localtime_s(&tm_buf, &in_time_t);
	wchar_t date_str[11];
	wcsftime(date_str, sizeof(date_str) / sizeof(wchar_t), L"%Y-%m-%d", &tm_buf);
	wstring currentDate = date_str;

	// Determine log type early for early exit optimization
	string logType;
	switch (type[0])
	{
	case 'e':
		logType = "ERROR";
		break;
	case 'i':
		logType = "INFO";
		break;
	case 'p':
		logType = "PIPE";
		break;
	case 'd':
		logType = "DEBUG";
		break;
	case 'w':
		logType = "WARNING";
		break;
	case 't':
		logType = "TESTING";
		break;
	case 'c':
		logType = "COMPANION";
		break;
	default:
		logType = "UNKNOWN";
		break;
	}

	// Early exit if debug logging is disabled and this is a debug message
	if (logType == "DEBUG" && !debugLogs)
	{
		return;
	}

	lock_guard<mutex> lock(s_logFileMutex);

	// Check if we need to open a new file (date changed or file not open)
	if (s_cachedLogFile == nullptr || s_cachedLogDate != currentDate)
	{
		// Close old file if date changed
		if (s_cachedLogFile != nullptr)
		{
			fclose(s_cachedLogFile);
			s_cachedLogFile = nullptr;
		}

		// Try to create log directory in confpath first
		wstring logsDir = confpath + L"\\Logs";
		bool useFallback = false;
		
		// Ensure base config directory exists
		if (!CreateDirectoryW(confpath.c_str(), NULL))
		{
			DWORD err = GetLastError();
			useFallback = (err != ERROR_ALREADY_EXISTS);
		}
		
		if (!useFallback && !CreateDirectoryW(logsDir.c_str(), NULL))
		{
			DWORD err = GetLastError();
			useFallback = (err != ERROR_ALREADY_EXISTS);
		}

		// If config path isn't writable/creatable, switch to fallback directory immediately
		if (useFallback)
		{
			logsDir = GetFallbackLogDir();
			// Ensure fallback directory hierarchy exists
			size_t lastSlash = logsDir.find_last_of(L"\\");
			if (lastSlash != wstring::npos)
			{
				CreateDirectoryW(logsDir.substr(0, lastSlash).c_str(), NULL);
			}
			CreateDirectoryW(logsDir.c_str(), NULL);
		}

		// Build log file path
		wstring logPath = logsDir + L"\\log_" + currentDate + L".txt";
		string narrow_logPath = WStringToString(logPath);
		FILE *logFile = nullptr;
		errno_t fileErr = fopen_s(&logFile, narrow_logPath.c_str(), "a");
		
		// If failed and we weren't already using fallback, try fallback directory
		if ((fileErr != 0 || logFile == nullptr) && !useFallback)
		{
			logsDir = GetFallbackLogDir();
			// Ensure fallback directory hierarchy exists
			size_t lastSlash = logsDir.find_last_of(L"\\");
			if (lastSlash != wstring::npos)
			{
				CreateDirectoryW(logsDir.substr(0, lastSlash).c_str(), NULL);
			}
			CreateDirectoryW(logsDir.c_str(), NULL);
			
			logPath = logsDir + L"\\log_" + currentDate + L".txt";
			narrow_logPath = WStringToString(logPath);
			fileErr = fopen_s(&logFile, narrow_logPath.c_str(), "a");
		}
		
		if (fileErr != 0 || logFile == nullptr)
		{
			return; // Failed to open log file even with fallback
		}

		// Cache the file handle and date
		s_cachedLogFile = logFile;
		s_cachedLogDate = currentDate;
	}

	FILE* logFile = s_cachedLogFile;

	// Format timestamp
	stringstream ss;
	ss << put_time(&tm_buf, "%Y-%m-%d %X");

	// Write to log file
	fprintf(logFile, "[%s] [%s] %s\n", ss.str().c_str(), logType.c_str(), message);
	fflush(logFile); // Ensure data is written immediately

	// [LEGACY-PIPE] Send through pipe if enabled
	if (sendLogsThroughPipe && g_pipeHandle != INVALID_HANDLE_VALUE)
	{
		string logMessage = ss.str() + " [" + logType + "] " + message + "\n";
		DWORD bytesWritten;
		DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
		WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
	}
}

void LogIddCxVersion()
{
	IDARG_OUT_GETVERSION outArgs;
	NTSTATUS status = IddCxGetVersion(&outArgs);

	if (NT_SUCCESS(status))
	{
		char versionStr[16];
		sprintf_s(versionStr, "0x%lx", outArgs.IddCxVersion);
		string logMessage = "IDDCX Version: " + string(versionStr);
		vddlog("i", logMessage.c_str());
	}
	else
	{
		vddlog("i", "Failed to get IDDCX version");
	}
	vddlog("d", "Testing Debug Log");
}

void InitializeD3DDeviceAndLogGPU()
{
	ComPtr<ID3D11Device> d3dDevice;
	ComPtr<ID3D11DeviceContext> d3dContext;
	HRESULT hr = D3D11CreateDevice(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		0,
		nullptr,
		0,
		D3D11_SDK_VERSION,
		&d3dDevice,
		nullptr,
		&d3dContext);

	if (FAILED(hr))
	{
		vddlog("e", "Retrieving D3D Device GPU: Failed to create D3D11 device");
		return;
	}

	ComPtr<IDXGIDevice> dxgiDevice;
	hr = d3dDevice.As(&dxgiDevice);
	if (FAILED(hr))
	{
		vddlog("e", "Retrieving D3D Device GPU: Failed to get DXGI device");
		return;
	}

	ComPtr<IDXGIAdapter> dxgiAdapter;
	hr = dxgiDevice->GetAdapter(&dxgiAdapter);
	if (FAILED(hr))
	{
		vddlog("e", "Retrieving D3D Device GPU: Failed to get DXGI adapter");
		return;
	}

	DXGI_ADAPTER_DESC desc;
	hr = dxgiAdapter->GetDesc(&desc);
	if (FAILED(hr))
	{
		vddlog("e", "Retrieving D3D Device GPU: Failed to get GPU description");
		return;
	}

	d3dDevice.Reset();
	d3dContext.Reset();

	wstring wdesc(desc.Description);
	string utf8_desc;
	try
	{
		utf8_desc = WStringToString(wdesc);
	}
	catch (const exception &e)
	{
		vddlog("e", ("Retrieving D3D Device GPU: Conversion error: " + string(e.what())).c_str());
		return;
	}

	string logtext = "Retrieving D3D Device GPU: " + utf8_desc;
	vddlog("i", logtext.c_str());
}

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);
	UNREFERENCED_PARAMETER(dwReason);

	return TRUE;
}

static mutex s_xmlWriteMutex;

bool UpdateXmlToggleSetting(bool toggle, const wchar_t *variable)
{
	std::lock_guard<std::mutex> xmlLock(s_xmlWriteMutex);

	const wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, nullptr);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void **)&pWriter, nullptr);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t *pwszLocalName;
	const wchar_t *pwszValue;
	bool variableElementFound = false;

	while (S_OK == pReader->Read(&nodeType))
	{
		switch (nodeType)
		{
		case XmlNodeType_Element:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteStartElement(nullptr, pwszLocalName, nullptr);
			// Check if this element matches the target variable
			if (wcscmp(pwszLocalName, variable) == 0)
			{
				variableElementFound = true;
			}
			else
			{
				// A different child element appeared before Text, so the
				// previous match was a container element, not the target leaf
				variableElementFound = false;
			}
			break;

		case XmlNodeType_EndElement:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteEndElement();
			variableElementFound = false; // EndElement without Text means container
			break;

		case XmlNodeType_Text:
			pReader->GetValue(&pwszValue, nullptr);
			if (variableElementFound)
			{
				pWriter->WriteString(toggle ? L"true" : L"false");
				variableElementFound = false;
			}
			else
			{
				pWriter->WriteString(pwszValue);
			}
			break;

		case XmlNodeType_Whitespace:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteWhitespace(pwszValue);
			break;

		case XmlNodeType_Comment:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteComment(pwszValue);
			break;
		}
	}

	if (variableElementFound)
	{
		pWriter->WriteStartElement(nullptr, variable, nullptr);
		pWriter->WriteString(toggle ? L"true" : L"false");
		pWriter->WriteEndElement();
	}

	hr = pWriter->WriteEndDocument();
	if (FAILED(hr))
	{
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		return false;
	}
	return true;
}

bool UpdateXmlGpuSetting(const wchar_t *gpuName)
{
	const std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, nullptr);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	std::wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void **)&pWriter, nullptr);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t *pwszLocalName;
	const wchar_t *pwszValue;
	bool gpuElementFound = false;

	while (S_OK == pReader->Read(&nodeType))
	{
		switch (nodeType)
		{
		case XmlNodeType_Element:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteStartElement(nullptr, pwszLocalName, nullptr);
			break;

		case XmlNodeType_EndElement:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteEndElement();
			break;

		case XmlNodeType_Text:
			pReader->GetValue(&pwszValue, nullptr);
			if (gpuElementFound)
			{
				pWriter->WriteString(gpuName);
				gpuElementFound = false;
			}
			else
			{
				pWriter->WriteString(pwszValue);
			}
			break;

		case XmlNodeType_Whitespace:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteWhitespace(pwszValue);
			break;

		case XmlNodeType_Comment:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteComment(pwszValue);
			break;
		}

		if (nodeType == XmlNodeType_Element)
		{
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, L"gpu") == 0)
			{
				gpuElementFound = true;
			}
		}
	}
	hr = pWriter->WriteEndDocument();
	if (FAILED(hr))
	{
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		return false;
	}
	return true;
}

bool UpdateXmlDisplayCountSetting(int displayCount)
{
	const std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, nullptr);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	std::wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void **)&pWriter, nullptr);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr))
	{
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t *pwszLocalName;
	const wchar_t *pwszValue;
	bool displayCountElementFound = false;

	while (S_OK == pReader->Read(&nodeType))
	{
		switch (nodeType)
		{
		case XmlNodeType_Element:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteStartElement(nullptr, pwszLocalName, nullptr);
			break;

		case XmlNodeType_EndElement:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteEndElement();
			break;

		case XmlNodeType_Text:
			pReader->GetValue(&pwszValue, nullptr);
			if (displayCountElementFound)
			{
				pWriter->WriteString(std::to_wstring(displayCount).c_str());
				displayCountElementFound = false;
			}
			else
			{
				pWriter->WriteString(pwszValue);
			}
			break;

		case XmlNodeType_Whitespace:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteWhitespace(pwszValue);
			break;

		case XmlNodeType_Comment:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteComment(pwszValue);
			break;
		}

		if (nodeType == XmlNodeType_Element)
		{
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, L"count") == 0)
			{
				displayCountElementFound = true;
			}
		}
	}

	hr = pWriter->WriteEndDocument();
	if (FAILED(hr))
	{
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		return false;
	}
	return true;
}

LUID getSetAdapterLuid()
{
	AdapterOption &adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter)
	{
		vddlog("e", "No Gpu Found/Selected");
	}

	return adapterOption.adapterLuid;
}

void GetGpuInfo()
{
	AdapterOption &adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter)
	{
		vddlog("e", "No GPU found or set.");
		return;
	}

	try
	{
		string utf8_desc = WStringToString(adapterOption.target_name);
		LUID luid = getSetAdapterLuid();
		string logtext = "ASSIGNED GPU: " + utf8_desc +
						 " (LUID: " + std::to_string(luid.LowPart) + "-" + std::to_string(luid.HighPart) + ")";
		vddlog("i", logtext.c_str());
	}
	catch (const exception &e)
	{
		vddlog("e", ("Error: " + string(e.what())).c_str());
	}
}

void logAvailableGPUs()
{
	vector<GPUInfo> gpus;
	ComPtr<IDXGIFactory1> factory;
	if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
	{
		return;
	}
	for (UINT i = 0;; i++)
	{
		ComPtr<IDXGIAdapter> adapter;
		if (!SUCCEEDED(factory->EnumAdapters(i, &adapter)))
		{
			break;
		}
		DXGI_ADAPTER_DESC desc;
		if (!SUCCEEDED(adapter->GetDesc(&desc)))
		{
			continue;
		}
		GPUInfo info{desc.Description, adapter, desc};
		gpus.push_back(info);
	}
	for (const auto &gpu : gpus)
	{
		wstring logMessage = L"GPU Name: ";
		logMessage += gpu.desc.Description;
		wstring memorySize = L" Memory: ";
		memorySize += std::to_wstring(gpu.desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB";
		wstring logText = logMessage + memorySize;
		int bufferSize = WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (bufferSize > 0)
		{
			std::string logTextA(bufferSize - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, &logTextA[0], bufferSize, nullptr, nullptr);
			vddlog("c", logTextA.c_str());
		}
	}
}

void ReloadDriver(HANDLE hPipe)
{
	UNREFERENCED_PARAMETER(hPipe);

	vddlog("i", "Starting driver reload process");

	if (g_GlobalDevice != nullptr)
	{
		lock_guard<mutex> lock(g_Mutex);
		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
		if (pContext && pContext->pContext)
		{
			try
			{
				// Step 1: Save current numVirtualDisplays before configuration reload
				UINT oldNumVirtualDisplays = numVirtualDisplays;
				vddlog("d", ("Saving current monitor count for cleanup: " + std::to_string(oldNumVirtualDisplays)).c_str());

				// Step 2: Clean up existing monitors first
				vddlog("d", "Cleaning up existing monitors before reload");

				// Stop any active SwapChain processing
				if (pContext->pContext->HasActiveSwapChain())
				{
					vddlog("d", "Stopping active SwapChain processing before reload");
					pContext->pContext->UnassignAllSwapChains();
					Sleep(100);
				}

				// Destroy all existing monitors
				if (pContext->pContext->HasActiveMonitor())
				{
					vddlog("d", "Destroying all existing monitors before reload");
					pContext->pContext->DestroyAllMonitors();
				}

				// Step 3: Wait for system stabilization after cleanup
				vddlog("d", "Waiting for system stabilization after cleanup");
				Sleep(200);

				// Step 4: Reinitialize the adapter (this will reload XML configuration)
				vddlog("d", "Reinitializing adapter with new configuration");
				pContext->pContext->InitAdapter();

				// Step 5: Post-initialization stabilization
				vddlog("d", "Waiting for adapter initialization to stabilize");
				Sleep(100);

				vddlog("i", ("Driver reload completed successfully. Monitor count changed from " + std::to_string(oldNumVirtualDisplays) + " to " + std::to_string(numVirtualDisplays)).c_str());
			}
			catch (const std::exception &e)
			{
				stringstream errorStream;
				errorStream << "Exception during driver reload: " << e.what();
				vddlog("e", errorStream.str().c_str());

				// Try to continue with initialization anyway
				try
				{
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

				// Try to continue with initialization anyway
				try
				{
					pContext->pContext->InitAdapter();
					vddlog("w", "Adapter reinitialized after unknown exception");
				}
				catch (...)
				{
					vddlog("e", "Failed to reinitialize adapter after unknown exception");
				}
			}
		}
		else
		{
			vddlog("e", "Invalid device context for driver reload");
		}
	}
	else
	{
		vddlog("e", "Global device not available for reload");
	}
}

void toggleSettingImpl(HANDLE hPipe, wchar_t *param, const wchar_t *settingName, const char *enableMsg, const char *disableMsg)
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

// Centralised command-buffer dispatch shared by both the legacy named-pipe
// transport (HandleClient) and the new IOCTL transport
// (VirtualDisplayDriverIoDeviceControl).
//
// `buffer` MUST be a writable, null-terminated UTF-16 string of at most
// 2048 wchar_t. `hPipeForResponse` is the response sink for the few
// commands that write back via WriteFile/SendToPipe (GETSETTINGS / PING);
// pass INVALID_HANDLE_VALUE for IOCTL callers and those response handlers
// will silently no-op (Sunshine never relies on the response payload of
// any command it sends, so this is intentional and safe).
void DispatchVddCommandBuffer(HANDLE hPipeForResponse, wchar_t *buffer)
{
	struct Command
	{
		const wchar_t *name;
		size_t length;
		void (*action)(HANDLE, wchar_t *);
	};

	auto handleReloadDriver = [](HANDLE hPipe, wchar_t *)
	{
		vddlog("c", "Reloading the driver");
		ReloadDriver(hPipe);
	};

	auto handleLogDebug = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"debuglogging", "Pipe debugging enabled", "Debugging disabled");
	};

	auto handleLogging = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"logging", "Logging Enabled", "Logging disabled");
	};

	auto handleHDRPlus = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"HDRPlus", "HDR+ Enabled", "HDR+ Disabled");
	};

	auto handleSDR10 = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"SDR10bit", "SDR 10 Bit Enabled", "SDR 10 Bit Disabled");
	};

	auto handleCustomEdid = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"CustomEdid", "Custom Edid Enabled", "Custom Edid Disabled");
	};

	auto handlePreventSpoof = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"PreventSpoof", "Prevent Spoof Enabled", "Prevent Spoof Disabled");
	};

	auto handleCeaOverride = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"EdidCeaOverride", "Cea override Enabled", "Cea override Disabled");
	};

	// VRR adapter flag toggle. Persists via the existing toggleSettingImpl
	// path (writes to vdd_settings.xml) and triggers a driver reload so the
	// new adapter caps take effect.
	auto handleVrr = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"Vrr", "VRR Enabled", "VRR Disabled");
	};

	// Hot-switch the EDID profile (Auto / Legacy / Modern). Updates the
	// vdd_settings.xml on disk so the choice survives driver reloads, then
	// re-applies the new value (resolving Auto via host OS detection). Newly
	// created monitors will pick up the new EDID bytes via GetHardcodedEdid;
	// existing monitors keep their current bytes until recreated.
	auto handleEdidProfile = [](HANDLE /*hPipe*/, wchar_t *param)
	{
		if (!param || *param == 0)
		{
			vddlog("e", "EDIDPROFILE requires a value: auto | legacy | modern");
			return;
		}
		std::wstring requested(param);
		// Validate by parse; reject unknown spellings to surface typos.
		auto parsed = VddEdid::ProfileFromString(requested);
		if (parsed == VddEdid::Profile::Auto && requested.find(L"auto") == std::wstring::npos &&
		    requested.find(L"AUTO") == std::wstring::npos && requested.find(L"Auto") == std::wstring::npos)
		{
			vddlog("e", "EDIDPROFILE: unknown value (expected auto | legacy | modern)");
			return;
		}
		// In-memory only: persisting requires a string-valued XML writer
		// which the codebase does not yet expose (UpdateXmlToggleSetting
		// is bool-only). Set vdd_settings.xml manually if you want the
		// choice to survive a driver reload.
		ApplyEdidProfileSetting(requested);
		vddlog("c", "EDID profile updated; recreate monitors to take effect");
	};

	auto handleHardwareCursor = [](HANDLE hPipe, wchar_t *param)
	{
		toggleSettingImpl(hPipe, param, L"HardwareCursor", "Hardware Cursor Enabled", "Hardware Cursor Disabled");
	};

	auto handleD3DDeviceGPU = [](HANDLE, wchar_t *)
	{
		vddlog("c", "Retrieving D3D GPU (This information may be inaccurate without reloading the driver first)");
		InitializeD3DDeviceAndLogGPU();
		vddlog("c", "Retrieved D3D GPU");
	};

	auto handleIddCxVersion = [](HANDLE, wchar_t *)
	{
		vddlog("c", "Logging iddcx version");
		LogIddCxVersion();
	};

	auto handleGetAssignedGPU = [](HANDLE, wchar_t *)
	{
		vddlog("c", "Retrieving Assigned GPU");
		GetGpuInfo();
		vddlog("c", "Retrieved Assigned GPU");
	};

	auto handleGetAllGPUs = [](HANDLE, wchar_t *)
	{
		vddlog("c", "Logging all GPUs");
		vddlog("i", "Any GPUs which show twice but you only have one, will most likely be the GPU the driver is attached to");
		logAvailableGPUs();
		vddlog("c", "Logged all GPUs");
	};

	auto handleSetGPU = [](HANDLE hPipe, wchar_t *param)
	{
		std::wstring gpuName = param;
		gpuName = gpuName.substr(1, gpuName.size() - 2);

		std::string gpuNameNarrow = WStringToString(gpuName);

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
	};

	auto handleSetDisplayCount = [](HANDLE hPipe, wchar_t *param)
	{
		vddlog("i", "Setting Display Count");

		int newDisplayCount = 1;
		swscanf_s(param, L"%d", &newDisplayCount);

		std::wstring displayLog = L"Setting display count  to " + std::to_wstring(newDisplayCount);
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
	};

	auto handleGetSettings = [](HANDLE hPipe, wchar_t *)
	{
		bool debugEnabled = EnabledQuery(L"DebugLoggingEnabled");
		bool loggingEnabled = EnabledQuery(L"LoggingEnabled");

		wstring settingsResponse = L"SETTINGS ";
		settingsResponse += debugEnabled ? L"DEBUG=true " : L"DEBUG=false ";
		settingsResponse += loggingEnabled ? L"LOG=true" : L"LOG=false";

		// IOCTL callers pass INVALID_HANDLE_VALUE; WriteFile would fail with
		// ERROR_INVALID_HANDLE which we explicitly tolerate here. The IOCTL
		// path returns no payload because Sunshine never queries settings.
		if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL)
		{
			DWORD bytesWritten;
			DWORD bytesToWrite = static_cast<DWORD>((settingsResponse.length() + 1) * sizeof(wchar_t));
			WriteFile(hPipe, settingsResponse.c_str(), bytesToWrite, &bytesWritten, NULL);
		}
	};

	auto handlePing = [](HANDLE, wchar_t *)
	{
		// SendToPipe checks g_pipeHandle internally; for IOCTL callers
		// g_pipeHandle is INVALID_HANDLE_VALUE so this is a logged no-op.
		SendToPipe("PONG");
		vddlog("p", "Heartbeat Ping");
	};

	auto handleCreateMonitor = [](HANDLE, wchar_t *param)
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

		// Parse GUID and HDR luminance settings from parameter
		// Format: "{GUID}:[maxNits,minNits,maxFALL][widthCm,heightCm]" or multiple space-separated entries
		struct MonitorParams
		{
			GUID guid;
			bool hasGuid = false;
			float maxNits = 1000.0f;
			float minNits = 0.0001f;
			float maxFALL = 1000.0f;
			float widthCm = 0.0f;  // 0 means use EDID default
			float heightCm = 0.0f; // 0 means use EDID default
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

				// Parse settings after colon: [maxNits,minNits,maxFALL][widthCm,heightCm]
				if (colonPos != wstring::npos)
				{
					wstring settingsStr = token.substr(colonPos + 1);

					// Find all bracket pairs
					size_t pos = 0;
					int bracketIndex = 0;

					while (pos < settingsStr.length())
					{
						size_t openBracket = settingsStr.find(L'[', pos);
						if (openBracket == wstring::npos)
							break;

						size_t closeBracket = settingsStr.find(L']', openBracket);
						if (closeBracket == wstring::npos)
							break;

						wstring innerStr = settingsStr.substr(openBracket + 1, closeBracket - openBracket - 1);
						wstringstream valueStream(innerStr);
						wstring val;
						vector<float> values;

						while (getline(valueStream, val, L','))
						{
							try
							{
								values.push_back(std::stof(val));
							}
							catch (...)
							{
								break;
							}
						}

						if (bracketIndex == 0)
						{
							// First bracket: [maxNits,minNits,maxFALL]
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
							// Second bracket: [widthCm,heightCm]
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

				// Parse GUID
				if (!guidStr.empty())
				{
					wstring guidWithBraces = guidStr;
					if (guidWithBraces.front() != L'{')
						guidWithBraces = L"{" + guidWithBraces;
					if (guidWithBraces.back() != L'}')
						guidWithBraces += L"}";

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
					pGuid = &mp.guid;
				maxNits = mp.maxNits;
				minNits = mp.minNits;
				maxFALL = mp.maxFALL;
				widthCm = mp.widthCm;
				heightCm = mp.heightCm;
			}
			pContext->pContext->CreateMonitor(i, pGuid, maxNits, minNits, maxFALL, widthCm, heightCm);
		}
	};

	auto handleDestroyMonitor = [](HANDLE, wchar_t *)
	{
		if (g_GlobalDevice != nullptr)
		{
			lock_guard<mutex> lock(g_Mutex);
			auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
			if (pContext && pContext->pContext)
			{
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
				catch (const std::exception &e)
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
			else
			{
				vddlog("e", "Failed to get device context for monitor destruction");
			}
		}
		else
		{
			vddlog("e", "Global device not initialized for monitor destruction");
		}
	};

	auto handleUnknownCommand = [](HANDLE, wchar_t *buffer)
	{
		vddlog("e", "Unknown command");

		std::string narrowString = WStringToString(buffer);
		vddlog("e", narrowString.c_str());
	};

	auto handleRefreshModes = [](HANDLE, wchar_t *)
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
	};

	Command commands[] = {
		{L"RELOAD_DRIVER", 13, handleReloadDriver},
		{L"LOG_DEBUG", 9, handleLogDebug},
		{L"LOGGING", 7, handleLogging},
		{L"HDRPLUS", 7, handleHDRPlus},
		{L"SDR10", 5, handleSDR10},
		{L"CUSTOMEDID", 10, handleCustomEdid},
		{L"PREVENTSPOOF", 12, handlePreventSpoof},
		{L"CEAOVERRIDE", 11, handleCeaOverride},
		{L"EDIDPROFILE", 11, handleEdidProfile},
		{L"VRR", 3, handleVrr},
		{L"HARDWARECURSOR", 14, handleHardwareCursor},
		{L"D3DDEVICEGPU", 12, handleD3DDeviceGPU},
		{L"IDDCXVERSION", 12, handleIddCxVersion},
		{L"GETASSIGNEDGPU", 14, handleGetAssignedGPU},
		{L"GETALLGPUS", 10, handleGetAllGPUs},
		{L"SETGPU", 6, handleSetGPU},
		{L"SETDISPLAYCOUNT", 15, handleSetDisplayCount},
		{L"GETSETTINGS", 11, handleGetSettings},
		{L"PING", 4, handlePing},
		{L"REFRESHMODES", 12, handleRefreshModes},
		{L"CREATEMONITOR", 13, handleCreateMonitor},
		{L"DESTROYMONITOR", 14, handleDestroyMonitor},
		{nullptr, 0, handleUnknownCommand}};

	for (const auto &cmd : commands)
	{
		if (cmd.name && wcsncmp(buffer, cmd.name, cmd.length) == 0)
		{
			// Parse parameter: skip command name and optional space
			wchar_t *param = buffer + cmd.length;
			// Skip space if present
			if (*param == L' ')
			{
				param++;
			}
			// If param points to null terminator, it means no parameter was provided
			cmd.action(hPipeForResponse, param);
			break;
		}
	}
}

// [LEGACY-PIPE] entire function -- pipe-side wrapper around DispatchVddCommandBuffer
void HandleClient(HANDLE hPipe)
{
	g_pipeHandle = hPipe;
	vddlog("p", "Client Handling Enabled");
	wchar_t buffer[2048];
	DWORD bytesRead;
	BOOL result = ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, NULL);
	if (result && bytesRead != 0)
	{
		buffer[bytesRead / sizeof(wchar_t)] = L'\0';
		wstring bufferwstr(buffer);
		string bufferstr = WStringToString(bufferwstr);
		vddlog("p", bufferstr.c_str());

		DispatchVddCommandBuffer(hPipe, buffer);
	}
	DisconnectNamedPipe(hPipe);
	CloseHandle(hPipe);
	g_pipeHandle = INVALID_HANDLE_VALUE;
}

// IddCx redirects every IRP_MJ_DEVICE_CONTROL into its own internal queue
// before any default WDF queue ever sees it. The only way to receive a
// custom IOCTL in an IddCx driver is through this callback registered via
// IDD_CX_CLIENT_CONFIG.EvtIddCxDeviceIoControl. IddCx invokes this hook
// for IOCTLs it does not own; we recognise IOCTL_VDD_PING and
// IOCTL_VDD_COMMAND, and fall through with STATUS_NOT_SUPPORTED for
// everything else so unknown control codes don't hang the request queue.
_Use_decl_annotations_
VOID VirtualDisplayDriverIoDeviceControl(
	WDFDEVICE Device,
	WDFREQUEST Request,
	size_t OutputBufferLength,
	size_t InputBufferLength,
	ULONG IoControlCode)
{
	UNREFERENCED_PARAMETER(Device);
	UNREFERENCED_PARAMETER(OutputBufferLength);

	switch (IoControlCode)
	{
	case IOCTL_VDD_PING:
	{
		// Cheap "is the driver alive" probe used by Sunshine to decide
		// whether to short-circuit to disable_enable instead of waiting on
		// a slow command IOCTL.
		WdfRequestComplete(Request, STATUS_SUCCESS);
		return;
	}

	case IOCTL_VDD_COMMAND:
	{
		if (InputBufferLength == 0 || (InputBufferLength % sizeof(wchar_t)) != 0)
		{
			WdfRequestComplete(Request, STATUS_INVALID_BUFFER_SIZE);
			return;
		}

		// Mirror the legacy named-pipe HandleClient buffer (2048 wchar_t).
		// Anything larger is almost certainly malformed input.
		if (InputBufferLength > 2048 * sizeof(wchar_t))
		{
			WdfRequestComplete(Request, STATUS_BUFFER_OVERFLOW);
			return;
		}

		PVOID pInBuffer = nullptr;
		size_t inBufferLen = 0;
		NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(wchar_t), &pInBuffer, &inBufferLen);
		if (!NT_SUCCESS(status))
		{
			WdfRequestComplete(Request, status);
			return;
		}

		// Copy into a writable, NUL-terminated local buffer. METHOD_BUFFERED
		// already gives us a kernel-owned copy but the dispatch helpers
		// expect a wchar_t array they can scribble on (e.g. swscanf_s).
		wchar_t buffer[2048] = { 0 };
		size_t copyLen = inBufferLen;
		if (copyLen > sizeof(buffer) - sizeof(wchar_t))
		{
			copyLen = sizeof(buffer) - sizeof(wchar_t);
		}
		RtlCopyMemory(buffer, pInBuffer, copyLen);
		buffer[copyLen / sizeof(wchar_t)] = L'\0';

		try
		{
			wstring bufferwstr(buffer);
			string bufferstr = WStringToString(bufferwstr);
			vddlog("p", ("[IOCTL] " + bufferstr).c_str());

			// Pass INVALID_HANDLE_VALUE so response-emitting handlers
			// (GETSETTINGS / PING) silently skip their WriteFile path.
			// Sunshine never observes those responses anyway.
			DispatchVddCommandBuffer(INVALID_HANDLE_VALUE, buffer);
		}
		catch (const std::exception &e)
		{
			stringstream errorStream;
			errorStream << "Exception during IOCTL command dispatch: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...)
		{
			vddlog("e", "Unknown exception during IOCTL command dispatch");
		}

		WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
		return;
	}

	default:
		WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
		return;
	}
}


// [LEGACY-PIPE] entire function -- accept-loop thread for the named pipe
DWORD WINAPI NamedPipeServer(LPVOID lpParam)
{
	UNREFERENCED_PARAMETER(lpParam);

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = FALSE;
	const wchar_t *sddl = L"D:(A;;GA;;;WD)";
	vddlog("d", "Starting pipe with parameters: D:(A;;GA;;;WD)");
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
	{
		DWORD ErrorCode = GetLastError();
		string errorMsg = to_string(ErrorCode);
		vddlog("e", errorMsg.c_str());
		return 1;
	}
	HANDLE hPipe;
	while (g_Running)
	{
		hPipe = CreateNamedPipeW(
			PIPE_NAME,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES,
			512, 512,
			0,
			&sa);

		if (hPipe == INVALID_HANDLE_VALUE)
		{
			DWORD ErrorCode = GetLastError();
			string errorMsg = to_string(ErrorCode);
			vddlog("e", errorMsg.c_str());
			LocalFree(sa.lpSecurityDescriptor);
			return 1;
		}

		BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (connected)
		{
			vddlog("p", "Client Connected");
			HandleClient(hPipe);
		}
		else
		{
			CloseHandle(hPipe);
		}
	}
	LocalFree(sa.lpSecurityDescriptor);
	return 0;
}

// [LEGACY-PIPE] entire function
void StartNamedPipeServer()
{
	vddlog("p", "Starting Pipe");
	hPipeThread = CreateThread(NULL, 0, NamedPipeServer, NULL, 0, NULL);
	if (hPipeThread == NULL)
	{
		DWORD ErrorCode = GetLastError();
		string errorMsg = to_string(ErrorCode);
		vddlog("e", errorMsg.c_str());
	}
	else
	{
		vddlog("p", "Pipe created");
	}
}

// [LEGACY-PIPE] entire function
void StopNamedPipeServer()
{
	vddlog("p", "Stopping Pipe");
	{
		lock_guard<mutex> lock(g_Mutex);
		g_Running = false;
	}
	if (hPipeThread)
	{
		HANDLE hPipe = CreateFileW(
			PIPE_NAME,
			GENERIC_READ | GENERIC_WRITE,
			0,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);

		if (hPipe != INVALID_HANDLE_VALUE)
		{
			DisconnectNamedPipe(hPipe);
			CloseHandle(hPipe);
		}

		WaitForSingleObject(hPipeThread, INFINITE);
		CloseHandle(hPipeThread);
		hPipeThread = NULL;
		vddlog("p", "Stopped Pipe");
	}
}

// 优先从环境变量读取配置路径，如果没有则回退到注册表读取
bool initpath()
{
	// 先尝试从环境变量读取
	wchar_t envPath[MAX_PATH] = {0};
	DWORD envLen = GetEnvironmentVariableW(L"ZAKOVDDPATH", envPath, MAX_PATH);
	if (envLen > 0 && envLen < MAX_PATH)
	{
		confpath = envPath;
		return true;
	}

	// 环境变量未设置，尝试从注册表读取
	HKEY hKey;
	wchar_t szPath[MAX_PATH] = {0};
	DWORD dwBufferSize = sizeof(szPath);
	LONG lResult;
	lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);
	if (lResult != ERROR_SUCCESS)
	{
		// 注册表不存在，返回false
		return false;
	}

	lResult = RegQueryValueExW(hKey, L"VDDPATH", NULL, NULL, (LPBYTE)szPath, &dwBufferSize);
	if (lResult != ERROR_SUCCESS)
	{
		RegCloseKey(hKey);
		return false;
	}

	confpath = szPath;
	RegCloseKey(hKey);

	return true;
}

extern "C" EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;

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
	initpath();
	logsEnabled = EnabledQuery(L"LoggingEnabled");
	debugLogs = EnabledQuery(L"DebugLoggingEnabled");

	customEdid = EnabledQuery(L"CustomEdidEnabled");
	preventManufacturerSpoof = EnabledQuery(L"PreventMonitorSpoof");
	edidCeaOverride = EnabledQuery(L"EdidCeaOverride");
	// [LEGACY-PIPE]
	sendLogsThroughPipe = EnabledQuery(L"SendLogsThroughPipe");

	// colour
	HDRPlus = EnabledQuery(L"HDRPlusEnabled");
	SDR10 = EnabledQuery(L"SDR10Enabled");
	HDRCOLOUR = HDRPlus ? IDDCX_BITS_PER_COMPONENT_12 : IDDCX_BITS_PER_COMPONENT_10;
	SDRCOLOUR = SDR10 ? IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8;
	ColourFormat = GetStringSetting(L"ColourFormat");

	// EDID profile: Auto -> resolved via host OS build number (issue #612).
	ApplyEdidProfileSetting(GetStringSetting(L"EdidProfile"));

	// VRR / FreeSync: behavioural change, default OFF until EDID FreeSync
	// Range Block also lands (see ROADMAP P1).
	vrrEnabled = EnabledQuery(L"VrrEnabled");

	// Cursor
	hardwareCursor = EnabledQuery(L"HardwareCursorEnabled");
	alphaCursorSupport = EnabledQuery(L"AlphaCursorSupport");
	CursorMaxX = GetIntegerSetting(L"CursorMaxX");
	CursorMaxY = GetIntegerSetting(L"CursorMaxY");

	int xorCursorSupportLevelInt = GetIntegerSetting(L"XorCursorSupportLevel");
	std::string xorCursorSupportLevelName;

	if (xorCursorSupportLevelInt < 0 || xorCursorSupportLevelInt > 3)
	{
		vddlog("w", "Selected Xor Level unsupported, defaulting to IDDCX_XOR_CURSOR_SUPPORT_FULL");
		XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;
	}
	else
	{
		XorCursorSupportLevel = static_cast<IDDCX_XOR_CURSOR_SUPPORT>(xorCursorSupportLevelInt);
	}

	xorCursorSupportLevelName = XorCursorSupportLevelToString(XorCursorSupportLevel);

	vddlog("i", ("Selected Xor Cursor Support Level: " + xorCursorSupportLevelName).c_str());

	vddlog("i", "Driver Starting");
	string utf8_confpath = WStringToString(confpath);
	string logtext = "VDD Path: " + utf8_confpath;
	vddlog("i", logtext.c_str());
	LogIddCxVersion();

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	// [LEGACY-PIPE]
	StartNamedPipeServer();

	return Status;
}

vector<string> split(string &input, char delimiter)
{
	istringstream stream(input);
	string field;
	vector<string> result;
	while (getline(stream, field, delimiter))
	{
		result.push_back(field);
	}
	return result;
}

void loadSettings()
{
	const wstring settingsname = confpath + L"\\vdd_settings.xml";
	const wstring &filename = settingsname;
	if (PathFileExistsW(filename.c_str()))
	{
		CComPtr<IStream> pStream;
		CComPtr<IXmlReader> pReader;
		HRESULT hr = SHCreateStreamOnFileW(filename.c_str(), STGM_READ, &pStream);
		if (FAILED(hr))
		{
			vddlog("e", "Loading Settings: Failed to create file stream.");
			return;
		}
		hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, NULL);
		if (FAILED(hr))
		{
			vddlog("e", "Loading Settings: Failed to create XmlReader.");
			return;
		}
		hr = pReader->SetInput(pStream);
		if (FAILED(hr))
		{
			vddlog("e", "Loading Settings: Failed to set input stream.");
			return;
		}

		XmlNodeType nodeType;
		const WCHAR *pwszLocalName;
		const WCHAR *pwszValue;
		UINT cwchLocalName;
		UINT cwchValue;
		wstring currentElement;
		wstring width, height, refreshRate;
		vector<tuple<int, int, int, int>> res;
		wstring gpuFriendlyName;
		UINT monitorcount = 1;
		set<tuple<int, int>> resolutions;
		vector<float> globalRefreshRates;

		while (S_OK == (hr = pReader->Read(&nodeType)))
		{
			switch (nodeType)
			{
			case XmlNodeType_Element:
				hr = pReader->GetLocalName(&pwszLocalName, &cwchLocalName);
				if (FAILED(hr))
				{
					return;
				}
				currentElement = wstring(pwszLocalName, cwchLocalName);
				break;
			case XmlNodeType_Text:
				hr = pReader->GetValue(&pwszValue, &cwchValue);
				if (FAILED(hr))
				{
					return;
				}
				if (currentElement == L"count")
				{
					try {
						monitorcount = stoi(wstring(pwszValue, cwchValue));
					} catch (const std::exception &) {
						monitorcount = 1;
						vddlog("w", "Failed to parse monitor count, defaulting to 1");
					}
					if (monitorcount == 0)
					{
						monitorcount = 1;
						vddlog("i", "Loading singular monitor (Monitor Count is not valid)");
					}
				}
				else if (currentElement == L"friendlyname")
				{
					gpuFriendlyName = wstring(pwszValue, cwchValue);
				}
				else if (currentElement == L"width")
				{
					width = wstring(pwszValue, cwchValue);
					if (width.empty())
					{
						width = L"800";
					}
				}
				else if (currentElement == L"height")
				{
					height = wstring(pwszValue, cwchValue);
					if (height.empty())
					{
						height = L"600";
					}
					try {
						resolutions.insert(make_tuple(stoi(width), stoi(height)));
					} catch (const std::exception &) {
						vddlog("w", "Failed to parse resolution width/height, skipping");
					}
				}
				else if (currentElement == L"refresh_rate")
				{
					refreshRate = wstring(pwszValue, cwchValue);
					if (refreshRate.empty())
					{
						refreshRate = L"30";
					}
					try {
						int vsync_num, vsync_den;
						float_to_vsync(stof(refreshRate), vsync_num, vsync_den);
						int w = stoi(width), h = stoi(height);
						res.push_back(make_tuple(w, h, vsync_num, vsync_den));
						stringstream ss;
						ss << "Added: " << w << "x" << h << " @ " << vsync_num << "/" << vsync_den << "Hz";
						vddlog("d", ss.str().c_str());
					} catch (const std::exception &) {
						vddlog("w", "Failed to parse refresh rate or resolution, skipping entry");
					}
				}
				else if (currentElement == L"g_refresh_rate")
				{
					try {
						globalRefreshRates.push_back(stof(wstring(pwszValue, cwchValue)));
					} catch (const std::exception &) {
						vddlog("w", "Failed to parse global refresh rate, skipping");
					}
				}
				break;
			}
		}

		/*
		* This is for res testing, stores each resolution then iterates through each global adding a res for each one
		*

		for (const auto& resTuple : resolutions) {
			stringstream ss;
			ss << get<0>(resTuple) << "x" << get<1>(resTuple);
			vddlog("t", ss.str().c_str());
		}

		for (const auto& globalRate : globalRefreshRates) {
			stringstream ss;
			ss << globalRate << " Hz";
			vddlog("t", ss.str().c_str());
		}
		*/

		for (float globalRate : globalRefreshRates)
		{
			for (const auto &resTuple : resolutions)
			{
				int global_width = get<0>(resTuple);
				int global_height = get<1>(resTuple);

				int vsync_num, vsync_den;
				float_to_vsync(globalRate, vsync_num, vsync_den);
				res.push_back(make_tuple(global_width, global_height, vsync_num, vsync_den));
			}
		}

		/*
		* logging all resolutions after added global
		*
		for (const auto& tup : res) {
			stringstream ss;
			ss << "("
				<< get<0>(tup) << ", "
				<< get<1>(tup) << ", "
				<< get<2>(tup) << ", "
				<< get<3>(tup) << ")";
			vddlog("t", ss.str().c_str());
		}

		*/

		{
			lock_guard<mutex> dataLock(g_DataMutex);
			numVirtualDisplays = monitorcount;
			gpuname = gpuFriendlyName;
			monitorModes = res;
		}
		vddlog("i", "Using vdd_settings.xml");
		return;
	}
	const wstring optionsname = confpath + L"\\option.txt";
	ifstream ifs(optionsname);
	if (ifs.is_open())
	{
		string line;
		if (getline(ifs, line) && !line.empty())
		{
			try {
				numVirtualDisplays = stoi(line);
			} catch (const std::exception &) {
				numVirtualDisplays = 1;
				vddlog("w", "Failed to parse display count from option.txt, defaulting to 1");
			}
			vector<tuple<int, int, int, int>> res;

			while (getline(ifs, line))
			{
				vector<string> strvec = split(line, ',');
				if (strvec.size() == 3 && strvec[0].substr(0, 1) != "#")
				{
					try {
						int vsync_num, vsync_den;
						float_to_vsync(stof(strvec[2]), vsync_num, vsync_den);
						res.push_back({stoi(strvec[0]), stoi(strvec[1]), vsync_num, vsync_den});
					} catch (const std::exception &) {
						vddlog("w", "Failed to parse option.txt line, skipping");
					}
				}
			}

			vddlog("i", "Using option.txt");
			{
				lock_guard<mutex> dataLock(g_DataMutex);
				monitorModes = res;
			}
			for (const auto &mode : res)
			{
				int width, height, vsync_num, vsync_den;
				tie(width, height, vsync_num, vsync_den) = mode;
				stringstream ss;
				ss << "Resolution: " << width << "x" << height << " @ " << vsync_num << "/" << vsync_den << "Hz";
				vddlog("d", ss.str().c_str());
			}
			return;
		}
		else
		{
			vddlog("w", "option.txt is empty or the first line is invalid. Enabling Fallback");
		}
	}

	numVirtualDisplays = 1;
	vector<tuple<int, int, int, int>> res;
	vector<tuple<int, int, float>> fallbackRes = {
		{800, 600, 30.0f},
		{800, 600, 60.0f},
		{800, 600, 90.0f},
		{800, 600, 120.0f},
		{800, 600, 144.0f},
		{800, 600, 165.0f},
		{1280, 720, 30.0f},
		{1280, 720, 60.0f},
		{1280, 720, 90.0f},
		{1280, 720, 130.0f},
		{1280, 720, 144.0f},
		{1280, 720, 165.0f},
		{1366, 768, 30.0f},
		{1366, 768, 60.0f},
		{1366, 768, 90.0f},
		{1366, 768, 120.0f},
		{1366, 768, 144.0f},
		{1366, 768, 165.0f},
		{1920, 1080, 30.0f},
		{1920, 1080, 60.0f},
		{1920, 1080, 90.0f},
		{1920, 1080, 120.0f},
		{1920, 1080, 144.0f},
		{1920, 1080, 165.0f},
		{2560, 1440, 30.0f},
		{2560, 1440, 60.0f},
		{2560, 1440, 90.0f},
		{2560, 1440, 120.0f},
		{2560, 1440, 144.0f},
		{2560, 1440, 165.0f},
		{3840, 2160, 30.0f},
		{3840, 2160, 60.0f},
		{3840, 2160, 90.0f},
		{3840, 2160, 120.0f},
		{3840, 2160, 144.0f},
		{3840, 2160, 165.0f}};

	vddlog("i", "Loading Fallback - no settings found");

	for (const auto &mode : fallbackRes)
	{
		int width, height;
		float refreshRate;
		tie(width, height, refreshRate) = mode;

		int vsync_num, vsync_den;
		float_to_vsync(refreshRate, vsync_num, vsync_den);

		stringstream ss;
		res.push_back(make_tuple(width, height, vsync_num, vsync_den));

		ss << "Resolution: " << width << "x" << height << " @ " << vsync_num << "/" << vsync_den << "Hz";
		vddlog("d", ss.str().c_str());
	}

	{
		lock_guard<mutex> dataLock(g_DataMutex);
		monitorModes = res;
	}
	return;
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

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
}

HRESULT Direct3DDevice::Init()
{
	HRESULT hr;
	stringstream logStream;

	// The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
	// created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.

	logStream << "Initializing Direct3DDevice...";
	vddlog("d", logStream.str().c_str());

	hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		logStream.str("");
		logStream << "Failed to create DXGI factory. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return hr;
	}
	logStream.str("");
	logStream << "DXGI factory created successfully.";
	vddlog("d", logStream.str().c_str());

	// Find the specified render adapter
	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		logStream.str("");
		logStream << "Failed to enumerate adapter by LUID. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return hr;
	}

	DXGI_ADAPTER_DESC desc;
	Adapter->GetDesc(&desc);
	logStream.str("");
	logStream << "Adapter found: " << desc.Description << " (Vendor ID: " << desc.VendorId << ", Device ID: " << desc.DeviceId << ")";
	vddlog("i", logStream.str().c_str());

#if 0 // Test code
	{
		FILE* file;
		fopen_s(&file, WStringToString(confpath + L"\\desc_hdr.bin").c_str(), "wb");

		DXGI_ADAPTER_DESC desc;
		Adapter->GetDesc(&desc);

		fwrite(&desc, 1, sizeof(desc), file);
		fclose(file);
	}
#endif

	D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL featureLevel;

	// Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &Device, &featureLevel, &DeviceContext);
	if (FAILED(hr))
	{
		// If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
		// system is in a transient state.
		logStream.str("");
		logStream << "Failed to create Direct3D device. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		logStream.str("");
		logStream << "If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the system is in a transient state. " << hr;
		vddlog("e", logStream.str().c_str());
		return hr;
	}

	logStream.str("");
	logStream << "Direct3D device created successfully. Feature Level: " << featureLevel;
	vddlog("i", logStream.str().c_str());

	return S_OK;
}

#pragma endregion

#pragma region SharedFrameExporter

namespace Microsoft { namespace IndirectDisp {

// =============================================================================
// SharedFrameExporter
// -----------------------------------------------------------------------------
// Exports each acquired IddCx swap-chain frame to a named D3D11 shared texture
// (NT shared handle) plus a named Win32 event so that an external consumer
// (e.g. Sunshine running as SYSTEM service or in a user session) can pick up
// the frame WITHOUT going through DXGI Desktop Duplication / WGC.
//
// Per-monitor named objects:
//   Texture handle name : Global\ZakoVDD_Frame_<idx>
//   Frame-ready event   : Global\ZakoVDD_FrameReady_<idx>
//   Metadata mapping    : Global\ZakoVDD_Meta_<idx>
//
// Texture is keyed-mutex protected:
//   Producer (this driver): acquire key 0 with 0ms timeout, on success
//                            CopyResource then release key 1.
//   Consumer (Sunshine)   : acquire key 1, copy/encode, release key 0.
// If the producer can't acquire (consumer slow), the frame is skipped and the
// previous frame remains visible to the consumer (no stall on producer side).
// =============================================================================

struct SharedFrameMetadata
{
    UINT32 Magic;          // 'ZVDF' = 0x5A564446
    UINT32 Version;        // 1
    UINT32 Width;
    UINT32 Height;
    UINT32 DxgiFormat;     // DXGI_FORMAT
    UINT32 IsHdr;          // 0/1
    float  MaxNits;
    float  MinNits;
    float  MaxFALL;
    UINT64 FrameCounter;   // Incremented per pushed frame
    UINT64 LastPresentQpc; // QueryPerformanceCounter at producer-side push
};

class SharedFrameExporter
{
public:
    SharedFrameExporter(unsigned int monitorIndex, std::shared_ptr<Direct3DDevice> device)
        : m_MonitorIndex(monitorIndex), m_Device(device)
    {
    }

    ~SharedFrameExporter()
    {
        Teardown();
    }

    // Push one acquired surface to the consumer. Best-effort. Never throws.
    void PushFrame(IDXGIResource* acquired)
    {
		std::lock_guard<std::mutex> lock(m_ExportMutex);

        if (!acquired || !m_Device || !m_Device->Device)
        {
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> srcTex;
        if (FAILED(acquired->QueryInterface(__uuidof(ID3D11Texture2D), &srcTex)) || !srcTex)
        {
            return;
        }

        D3D11_TEXTURE2D_DESC srcDesc = {};
        srcTex->GetDesc(&srcDesc);

        if (!EnsureSharedTexture(srcDesc))
        {
            return;
        }

        // Best-effort acquire with 0 timeout. If consumer holds key 0 the
        // producer will simply skip this frame (do not stall the IddCx loop).
        HRESULT hr = m_KeyedMutex->AcquireSync(0, 0);
        if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
        {
            return;
        }
        if (FAILED(hr))
        {
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        m_Device->Device->GetImmediateContext(&ctx);
        ctx->CopyResource(m_SharedTex.Get(), srcTex.Get());
        ctx->Flush();

        m_KeyedMutex->ReleaseSync(1);

        // Update metadata block (frame counter / present timestamp).
        if (m_MetaView)
        {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            m_MetaView->FrameCounter++;
            m_MetaView->LastPresentQpc = static_cast<UINT64>(qpc.QuadPart);
        }

        if (m_FrameReadyEvent)
        {
            SetEvent(m_FrameReadyEvent);
        }
    }

    void UpdateHdrMetadata(bool isHdr, float maxNits, float minNits, float maxFALL)
    {
		std::lock_guard<std::mutex> lock(m_ExportMutex);

        m_PendingIsHdr = isHdr;
        m_PendingMaxNits = maxNits;
        m_PendingMinNits = minNits;
        m_PendingMaxFALL = maxFALL;
        if (m_MetaView)
        {
            m_MetaView->IsHdr = isHdr ? 1u : 0u;
            m_MetaView->MaxNits = maxNits;
            m_MetaView->MinNits = minNits;
            m_MetaView->MaxFALL = maxFALL;
			if (m_CachedFormat == DXGI_FORMAT_UNKNOWN)
			{
				m_MetaView->DxgiFormat = GuessMetadataFormat();
			}
        }
    }

	void PublishModeMetadata(UINT width, UINT height)
	{
		if (width == 0 || height == 0)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(m_ExportMutex);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.Format = GuessMetadataFormat();

		if (!EnsureEventAndMetadata(desc))
		{
			std::stringstream ss;
			ss << "[VddExport] Failed to publish mode metadata for monitor=" << m_MonitorIndex
			   << " " << width << "x" << height;
			vddlog("e", ss.str().c_str());
			return;
		}

		std::stringstream ss;
		ss << "[VddExport] Published mode metadata monitor=" << m_MonitorIndex
		   << " " << width << "x" << height
		   << " fmt=" << desc.Format
		   << " hdr=" << (m_PendingIsHdr ? 1 : 0);
		vddlog("i", ss.str().c_str());
	}

private:
	DXGI_FORMAT GuessMetadataFormat() const
	{
		return m_PendingIsHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
	}

    bool EnsureSharedTexture(const D3D11_TEXTURE2D_DESC& srcDesc)
    {
        if (m_SharedTex &&
            m_CachedWidth == srcDesc.Width &&
            m_CachedHeight == srcDesc.Height &&
            m_CachedFormat == srcDesc.Format)
        {
            return true;
        }

        // Recreate everything since dimensions / format changed.
        TeardownTexture();

        // Build SDDL: Built-in Admin (BA) + Interactive Users (IU), full access.
        SECURITY_ATTRIBUTES sa = {};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1, &sd, nullptr))
        {
            vddlog("e", "[VddExport] Failed to build SDDL for shared texture");
            return false;
        }
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;

        D3D11_TEXTURE2D_DESC desc = srcDesc;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;

        HRESULT hr = m_Device->Device->CreateTexture2D(&desc, nullptr, &m_SharedTex);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "[VddExport] CreateTexture2D failed: 0x" << std::hex << hr;
            vddlog("e", ss.str().c_str());
            LocalFree(sd);
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes;
        hr = m_SharedTex.As(&dxgiRes);
        if (FAILED(hr))
        {
            vddlog("e", "[VddExport] QueryInterface IDXGIResource1 failed");
            m_SharedTex.Reset();
            LocalFree(sd);
            return false;
        }

        std::wstring texName = L"Global\\ZakoVDD_Frame_" + std::to_wstring(m_MonitorIndex);
        HANDLE ntHandle = nullptr;
        hr = dxgiRes->CreateSharedHandle(&sa, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, texName.c_str(), &ntHandle);
        LocalFree(sd);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "[VddExport] CreateSharedHandle failed: 0x" << std::hex << hr;
            vddlog("e", ss.str().c_str());
            m_SharedTex.Reset();
            return false;
        }
        // The NT handle MUST stay open for the named lookup to remain valid;
        // closing it before consumers open will make OpenSharedResourceByName
        // return E_INVALIDARG even though the texture is still alive in our process.
        if (m_NtHandle) { CloseHandle(m_NtHandle); }
        m_NtHandle = ntHandle;

        hr = m_SharedTex.As(&m_KeyedMutex);
        if (FAILED(hr) || !m_KeyedMutex)
        {
            vddlog("e", "[VddExport] QueryInterface IDXGIKeyedMutex failed");
            m_SharedTex.Reset();
            return false;
        }

        if (!EnsureEventAndMetadata(srcDesc))
        {
            m_KeyedMutex.Reset();
            m_SharedTex.Reset();
            return false;
        }

        m_CachedWidth = srcDesc.Width;
        m_CachedHeight = srcDesc.Height;
        m_CachedFormat = srcDesc.Format;

        std::stringstream ss;
        ss << "[VddExport] Shared texture ready monitor=" << m_MonitorIndex
           << " " << srcDesc.Width << "x" << srcDesc.Height
           << " fmt=" << srcDesc.Format;
        vddlog("i", ss.str().c_str());
        return true;
    }

    bool EnsureEventAndMetadata(const D3D11_TEXTURE2D_DESC& srcDesc)
    {
        SECURITY_ATTRIBUTES sa = {};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1, &sd, nullptr))
        {
            return false;
        }
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;

        if (!m_FrameReadyEvent)
        {
            std::wstring evName = L"Global\\ZakoVDD_FrameReady_" + std::to_wstring(m_MonitorIndex);
            m_FrameReadyEvent = CreateEventW(&sa, FALSE, FALSE, evName.c_str());
            if (!m_FrameReadyEvent)
            {
                std::stringstream ss;
                ss << "[VddExport] CreateEventW failed: " << GetLastError();
                vddlog("e", ss.str().c_str());
                LocalFree(sd);
                return false;
            }
        }

        if (!m_MetaMapping)
        {
            std::wstring mapName = L"Global\\ZakoVDD_Meta_" + std::to_wstring(m_MonitorIndex);
            m_MetaMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
                                               PAGE_READWRITE, 0, sizeof(SharedFrameMetadata),
                                               mapName.c_str());
            if (!m_MetaMapping)
            {
                std::stringstream ss;
                ss << "[VddExport] CreateFileMappingW failed: " << GetLastError();
                vddlog("e", ss.str().c_str());
                LocalFree(sd);
                return false;
            }
            m_MetaView = static_cast<SharedFrameMetadata*>(
                MapViewOfFile(m_MetaMapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedFrameMetadata)));
            if (!m_MetaView)
            {
                LocalFree(sd);
                return false;
            }
            ZeroMemory(m_MetaView, sizeof(SharedFrameMetadata));
            m_MetaView->Magic = 0x5A564446; // 'ZVDF'
            m_MetaView->Version = 1;
        }
        LocalFree(sd);

        m_MetaView->Width = srcDesc.Width;
        m_MetaView->Height = srcDesc.Height;
        m_MetaView->DxgiFormat = srcDesc.Format;
        m_MetaView->IsHdr = m_PendingIsHdr ? 1u : 0u;
        m_MetaView->MaxNits = m_PendingMaxNits;
        m_MetaView->MinNits = m_PendingMinNits;
        m_MetaView->MaxFALL = m_PendingMaxFALL;
        return true;
    }

    void TeardownTexture()
    {
        m_KeyedMutex.Reset();
        m_SharedTex.Reset();
        if (m_NtHandle)
        {
            CloseHandle(m_NtHandle);
            m_NtHandle = nullptr;
        }
        m_CachedWidth = 0;
        m_CachedHeight = 0;
        m_CachedFormat = DXGI_FORMAT_UNKNOWN;
    }

    void Teardown()
    {
        TeardownTexture();
        if (m_MetaView)
        {
            UnmapViewOfFile(m_MetaView);
            m_MetaView = nullptr;
        }
        if (m_MetaMapping)
        {
            CloseHandle(m_MetaMapping);
            m_MetaMapping = nullptr;
        }
        if (m_FrameReadyEvent)
        {
            CloseHandle(m_FrameReadyEvent);
            m_FrameReadyEvent = nullptr;
        }
    }

    unsigned int m_MonitorIndex = 0;
    std::shared_ptr<Direct3DDevice> m_Device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_SharedTex;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_KeyedMutex;
	std::mutex m_ExportMutex;
    HANDLE m_NtHandle = nullptr;
    HANDLE m_FrameReadyEvent = nullptr;
    HANDLE m_MetaMapping = nullptr;
    SharedFrameMetadata* m_MetaView = nullptr;

    UINT m_CachedWidth = 0;
    UINT m_CachedHeight = 0;
    DXGI_FORMAT m_CachedFormat = DXGI_FORMAT_UNKNOWN;

    bool  m_PendingIsHdr = false;
    float m_PendingMaxNits = 0.0f;
    float m_PendingMinNits = 0.0f;
    float m_PendingMaxFALL = 0.0f;
};

}} // namespace Microsoft::IndirectDisp

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent, unsigned int MonitorIndex)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent), m_MonitorIndex(MonitorIndex)
{
	stringstream logStream;

	logStream << "Constructing SwapChainProcessor:"
			  << "\n  SwapChain Handle: " << static_cast<void *>(hSwapChain)
			  << "\n  Device Pointer: " << static_cast<void *>(Device.get())
			  << "\n  NewFrameEvent Handle: " << NewFrameEvent
			  << "\n  Monitor Index: " << MonitorIndex;
	vddlog("d", logStream.str().c_str());

	// Initialize the VDD->external consumer frame exporter (best effort).
	try
	{
		m_Exporter = std::make_unique<SharedFrameExporter>(MonitorIndex, Device);
	}
	catch (...)
	{
		vddlog("e", "[VddExport] Failed to construct SharedFrameExporter (non-fatal)");
		m_Exporter.reset();
	}

	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
	if (!m_hTerminateEvent.Get())
	{
		logStream.str("");
		logStream << "Failed to create terminate event. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Terminate event created successfully.";
		vddlog("d", logStream.str().c_str());
	}

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
	if (!m_hThread.Get())
	{
		logStream.str("");
		logStream << "Failed to create swap-chain processing thread. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Swap-chain processing thread created and started successfully.";
		vddlog("d", logStream.str().c_str());
	}
}

SwapChainProcessor::~SwapChainProcessor()
{
	stringstream logStream;

	logStream << "Destructing SwapChainProcessor:";

	vddlog("d", logStream.str().c_str());
	// Alert the swap-chain processing thread to terminate
	// SetEvent(m_hTerminateEvent.Get()); changed for error handling + log purposes

	if (SetEvent(m_hTerminateEvent.Get()))
	{
		logStream.str("");
		logStream << "Terminate event signaled successfully.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to signal terminate event. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}

	if (m_hThread.Get())
	{
		// Wait for the thread to terminate with a timeout to avoid hanging
		DWORD waitResult = WaitForSingleObject(m_hThread.Get(), 5000); // 5 second timeout
		switch (waitResult)
		{
		case WAIT_OBJECT_0:
			logStream.str("");
			logStream << "Thread terminated successfully.";
			vddlog("d", logStream.str().c_str());
			break;
		case WAIT_ABANDONED:
			logStream.str("");
			logStream << "Thread wait was abandoned. GetLastError: " << GetLastError();
			vddlog("e", logStream.str().c_str());
			break;
		case WAIT_TIMEOUT:
			logStream.str("");
			logStream << "Thread wait timed out after 5 seconds. Thread will be abandoned (unsafe to force terminate).";
			vddlog("w", logStream.str().c_str());
			// Note: TerminateThread is NOT used here because it can corrupt the heap,
			// leave locks held, and cause deadlocks. The thread handle will be closed
			// when m_hThread is destroyed, but the thread itself may still be running.
			break;
		default:
			logStream.str("");
			logStream << "Unexpected result from WaitForSingleObject: " << waitResult << ". GetLastError: " << GetLastError();
			vddlog("e", logStream.str().c_str());
			break;
		}
	}
	else
	{
		logStream.str("");
		logStream << "No valid thread handle to wait for.";
		vddlog("e", logStream.str().c_str());
	}
}

void SwapChainProcessor::PublishModeMetadata(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode)
{
	if (!m_Exporter)
	{
		return;
	}

	const UINT width = mode.activeSize.cx ? static_cast<UINT>(mode.activeSize.cx) : static_cast<UINT>(mode.totalSize.cx);
	const UINT height = mode.activeSize.cy ? static_cast<UINT>(mode.activeSize.cy) : static_cast<UINT>(mode.totalSize.cy);
	m_Exporter->PublishModeMetadata(width, height);
}

void SwapChainProcessor::UpdateHdrMetadata(bool isHdr, float maxNits, float minNits, float maxFALL)
{
	if (!m_Exporter)
	{
		return;
	}

	m_Exporter->UpdateHdrMetadata(isHdr, maxNits, minNits, maxFALL);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	stringstream logStream;

	logStream << "RunThread started. Argument: " << Argument;
	vddlog("d", logStream.str().c_str());

	reinterpret_cast<SwapChainProcessor *>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	stringstream logStream;

	logStream << "Run method started.";
	vddlog("d", logStream.str().c_str());

	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	// Use "Pro Audio" task for highest priority scheduling (lower latency than "Distribution" or "Playback").
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &AvTask);

	if (AvTaskHandle)
	{
		// Additionally boost thread priority within the MMCSS task
		if (!AvSetMmThreadPriority(AvTaskHandle, AVRT_PRIORITY_CRITICAL))
		{
			logStream.str("");
			logStream << "Failed to set MMCSS priority to CRITICAL. GetLastError: " << GetLastError();
			vddlog("w", logStream.str().c_str());
		}
		logStream.str("");
		logStream << "Multimedia thread characteristics set successfully (Pro Audio). AvTask: " << AvTask;
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to set multimedia thread characteristics. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}

	// Also set regular thread priority as high as possible (in case MMCSS doesn't fully apply)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	RunCore();

	logStream.str("");
	logStream << "Core processing function RunCore() completed.";
	vddlog("d", logStream.str().c_str());

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	if (m_hSwapChain)
	{
		try
		{
			// Attempt graceful cleanup first
			vddlog("d", "Attempting graceful swap-chain cleanup");

			// Give the system time to complete any pending operations
			Sleep(10);

			WdfObjectDelete((WDFOBJECT)m_hSwapChain);
			logStream.str("");
			logStream << "Swap-chain object deleted successfully.";
			vddlog("d", logStream.str().c_str());
			m_hSwapChain = nullptr;
		}
		catch (const std::exception &e)
		{
			stringstream errorStream;
			errorStream << "Exception while deleting swap-chain: " << e.what();
			vddlog("e", errorStream.str().c_str());
			m_hSwapChain = nullptr; // Mark as null to prevent further access
		}
		catch (...)
		{
			vddlog("e", "Unknown exception while deleting swap-chain");
			m_hSwapChain = nullptr; // Mark as null to prevent further access
		}
	}
	else
	{
		logStream.str("");
		logStream << "No valid swap-chain object to delete.";
		vddlog("d", logStream.str().c_str());
	}
	/*
	AvRevertMmThreadCharacteristics(AvTaskHandle);
	*/
	// error handling when reversing multimedia thread characteristics
	if (AvRevertMmThreadCharacteristics(AvTaskHandle))
	{
		logStream.str("");
		logStream << "Multimedia thread characteristics reverted successfully.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to revert multimedia thread characteristics. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
}

void SwapChainProcessor::RunCore()
{
	stringstream logStream;

	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		logStream << "Failed to get DXGI device interface. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	if (FAILED(hr))
	{
		// 0x887A0026 = DXGI_ERROR_ACCESS_LOST
		// This usually means the OS already unassigned/abandoned this swap-chain. Treat as a normal teardown path.
		if (hr != static_cast<HRESULT>(0x887A0026))
		{
			logStream.str("");
			logStream << "Failed to set device to swap chain. HRESULT: " << hr;
			vddlog("e", logStream.str().c_str());
		}
		return;
	}

	// Raise GPU priority to realtime for this device to avoid starvation under heavy GPU load (IddCx 1.9+)
	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority))
	{
		IDARG_IN_SETREALTIMEGPUPRIORITY PriorityArgs = {};
		PriorityArgs.pDevice = DxgiDevice.Get();
		hr = IddCxSetRealtimeGPUPriority(m_hSwapChain, &PriorityArgs);
		if (FAILED(hr))
		{
			logStream.str("");
			logStream << "IddCxSetRealtimeGPUPriority failed (non-fatal). HRESULT: 0x" << hex << hr;
			vddlog("w", logStream.str().c_str());
		}
		else
		{
			vddlog("d", "GPU priority raised to realtime for swap chain processing");
		}
	}

	// Cache function availability check outside the loop for better performance
	const bool useBuffer2 = IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2);

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		// Ask for the next buffer from the producer
		IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
		BufferInArgs.Size = sizeof(BufferInArgs);
		IDXGIResource *pSurface;

		if (useBuffer2)
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		else
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles[] =
				{
					m_hAvailableBufferEvent,
					m_hTerminateEvent.Get()};
			// Let the kernel wake us on the event.
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, INFINITE);

			if (WaitResult == WAIT_OBJECT_0)
			{
				// We have a new buffer, so try the AcquireBuffer again
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				hr = (WaitResult == WAIT_FAILED) ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(WaitResult);
				// Only build log message when actually needed (error case)
				logStream.str("");
				logStream << "Unexpected wait result. HRESULT: " << hr;
				vddlog("e", logStream.str().c_str());
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			AcquiredBuffer.Attach(pSurface);

			// ==============================
			// VDD->Sunshine direct-capture export. Best-effort, never throws,
			// failure here MUST NOT stall the IddCx swap-chain loop.
			// ==============================
			if (m_Exporter)
			{
				m_Exporter->PushFrame(AcquiredBuffer.Get());
			}

			AcquiredBuffer.Reset();
			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}

			// ==============================
			// TODO: Report frame statistics once the asynchronous encode/send work is completed
			//
			// Drivers should report information about sub-frame timings, like encode time, send time, etc.
			// ==============================
			// IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
		}
		else
		{
			// Only build log message when actually needed (error case)
			// 0x887A0026 = DXGI_ERROR_ACCESS_LOST: treat as normal swap-chain teardown.
			if (hr != static_cast<HRESULT>(0x887A0026))
			{
				logStream.str("");
				logStream << "Failed to acquire buffer. Exiting loop. HRESULT: " << hr;
				vddlog("e", logStream.str().c_str());
			}
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

const UINT64 MHZ = 1000000;
const UINT64 KHZ = 1000;

constexpr DISPLAYCONFIG_VIDEO_SIGNAL_INFO dispinfo(UINT32 h, UINT32 v, UINT32 rn, UINT32 rd)
{
	const UINT32 safe_rd = (rd > 0) ? rd : 1;
	const UINT32 h_total = h + 4;
	const UINT32 v_total = v + 4;
	const UINT32 clock_rate = static_cast<UINT32>(static_cast<UINT64>(rn) * h_total * v_total / safe_rd);
	return {
		clock_rate,												// pixel clock rate [Hz]
		{clock_rate, h_total},									// fractional horizontal refresh rate [Hz]
		{clock_rate, static_cast<UINT32>(static_cast<UINT64>(h_total) * v_total)},	// fractional vertical refresh rate [Hz]
		{h, v},													// (horizontal, vertical) active pixel resolution
		{h_total, v_total},										// (horizontal, vertical) total pixel resolution
		{{255, 0}},												// video standard and vsync divider
		DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE};
}

// Get default EDID from external header file.
// Returns a fresh copy keyed on the currently selected profile so a runtime
// EDIDPROFILE switch takes effect on the next monitor creation without
// requiring driver reload.
vector<BYTE> GetHardcodedEdid()
{
	auto profile = static_cast<VddEdid::Profile>(gEdidProfile.load());
	return VddEdid::GetDefaultEdid(profile);
}

void modifyEdid(vector<BYTE> &edid)
{
	if (edid.size() < 12)
	{
		return;
	}

	edid[8] = 0x36;
	edid[9] = 0x94;
	edid[10] = 0x37;
	edid[11] = 0x13;
}

// Modify EDID serial number based on client GUID to ensure consistency
void modifyEdidSerialNumber(vector<BYTE> &edid, const GUID &clientGuid)
{
	if (edid.size() < 16)
	{
		return;
	}

	// EDID serial number is at bytes 12-15 (32-bit value)
	// Use GUID's Data1 (first 32 bits) to generate consistent serial number
	// This ensures the same GUID always produces the same serial number
	edid[12] = (BYTE)(clientGuid.Data1 & 0xFF);
	edid[13] = (BYTE)((clientGuid.Data1 >> 8) & 0xFF);
	edid[14] = (BYTE)((clientGuid.Data1 >> 16) & 0xFF);
	edid[15] = (BYTE)((clientGuid.Data1 >> 24) & 0xFF);
}

BYTE calculateChecksum(const std::vector<BYTE> &edid)
{
	int sum = 0;
	for (int i = 0; i < 127; ++i)
	{
		sum += edid[i];
	}
	sum %= 256;
	if (sum != 0)
	{
		sum = 256 - sum;
	}
	return static_cast<BYTE>(sum);
	// check sum calculations. We dont need to include old checksum in calculation, so we only read up to the byte before.
	// Anything after the checksum bytes arent part of the checksum - a flaw with edid managment, not with us
}

// Update physical size in EDID
// EDID bytes 21-22 contain physical dimensions in centimeters
// Byte 21: Horizontal screen size (cm)
// Byte 22: Vertical screen size (cm)
// If widthCm or heightCm is 0, the corresponding dimension is not updated
void updateEdidPhysicalSize(vector<BYTE> &edid, float widthCm, float heightCm)
{
	if (edid.size() < 23)
	{
		vddlog("w", "EDID too small to update physical size");
		return;
	}

	if (widthCm > 0)
	{
		// Clamp to valid range (1-255 cm)
		int width = static_cast<int>(widthCm);
		if (width < 1)
			width = 1;
		if (width > 255)
			width = 255;
		edid[21] = static_cast<BYTE>(width);

		stringstream ss;
		ss << "Set EDID horizontal size: " << width << " cm";
		vddlog("d", ss.str().c_str());
	}

	if (heightCm > 0)
	{
		// Clamp to valid range (1-255 cm)
		int height = static_cast<int>(heightCm);
		if (height < 1)
			height = 1;
		if (height > 255)
			height = 255;
		edid[22] = static_cast<BYTE>(height);

		stringstream ss;
		ss << "Set EDID vertical size: " << height << " cm";
		vddlog("d", ss.str().c_str());
	}
}

void updateCeaExtensionCount(vector<BYTE> &edid, int count)
{
	edid[126] = static_cast<BYTE>(count);
}

// Calculate checksum for CEA extension block (bytes 128-255)
BYTE calculateCeaChecksum(const vector<BYTE> &edid)
{
	if (edid.size() < 256)
		return 0;
	int sum = 0;
	for (int i = 128; i < 255; ++i)
	{
		sum += edid[i];
	}
	sum %= 256;
	if (sum != 0)
	{
		sum = 256 - sum;
	}
	return static_cast<BYTE>(sum);
}

// Update HDR Static Metadata Data Block in EDID CEA extension
// The HDR Static Metadata Data Block format (CEA-861.3):
// - Tag code: 0x07 (Extended Tag) with Extended Tag Code 0x06 (HDR Static Metadata)
// - Byte 0: (Tag << 5) | Length = (0x07 << 5) | 0x06 = 0xE6
// - Byte 1: Extended Tag Code = 0x06
// - Byte 2: Electro-Optical Transfer Functions (EOTF) supported
// - Byte 3: Static Metadata Descriptors supported
// - Byte 4: Desired Content Max Luminance (cv = 50 * 2^(L/32)) - MaxCLL
// - Byte 5: Desired Content Max Frame-average Luminance (cv = 50 * 2^(L/32)) - MaxFALL
// - Byte 6: Desired Content Min Luminance (cv = MinLum / MaxLum * 255 * 255 / 100)
//
// Parameters:
// - maxNits: Maximum Content Light Level (MaxCLL) in nits
// - minNits: Minimum luminance in nits
// - maxFALL: Maximum Frame-Average Light Level in nits (if 0, will be calculated as maxNits * 0.8)
void updateEdidHdrMetadata(vector<BYTE> &edid, float maxNits, float minNits, float maxFALL)
{
	if (edid.size() < 256)
	{
		vddlog("w", "EDID too small to update HDR metadata");
		return;
	}

	// If maxFALL is not specified (0), calculate it as ~80% of maxNits
	if (maxFALL <= 0)
	{
		maxFALL = maxNits * 0.8f;
	}

	// CEA extension starts at byte 128, data blocks start at byte 132
	const int ceaStart = 128;
	int dtdOffset = edid[ceaStart + 2];
	if (dtdOffset == 0)
		dtdOffset = 4;

	const int endPos = ceaStart + dtdOffset;

	for (int pos = ceaStart + 4; pos < endPos && pos < 255;)
	{
		const BYTE header = edid[pos];
		const int tag = (header >> 5) & 0x07;
		const int length = header & 0x1F;

		// Check for Extended Tag (0x07) with HDR Static Metadata (0x06)
		if (tag == 0x07 && length >= 1 && (pos + 1) < 256 && edid[pos + 1] == 0x06)
		{
			vddlog("d", ("Found HDR Static Metadata block at position " + to_string(pos)).c_str());

			// Helper lambda to calculate luminance code value: cv = 32 * log2(nits / 50)
			// Use ceiling to ensure we meet or exceed the requested luminance
			auto calcLumCv = [](float nits) -> BYTE
			{
				float cv = 32.0f * log2f(nits / 50.0f);
				return static_cast<BYTE>(ceilf(max(0.0f, min(255.0f, cv))));
			};

			// Byte 4: Max Luminance (MaxCLL)
			if (length >= 4 && (pos + 4) < 256)
			{
				edid[pos + 4] = calcLumCv(maxNits);
				float actualNits = 50.0f * powf(2.0f, edid[pos + 4] / 32.0f);
				vddlog("d", ("Set MaxCLL cv=" + to_string((int)edid[pos + 4]) + " (req " + to_string(maxNits) + ", actual " + to_string(actualNits) + " nits)").c_str());
			}

			// Byte 5: Max Frame-Average Luminance (MaxFALL)
			if (length >= 5 && (pos + 5) < 256)
			{
				edid[pos + 5] = calcLumCv(maxFALL);
				float actualNits = 50.0f * powf(2.0f, edid[pos + 5] / 32.0f);
				vddlog("d", ("Set MaxFALL cv=" + to_string((int)edid[pos + 5]) + " (req " + to_string(maxFALL) + ", actual " + to_string(actualNits) + " nits)").c_str());
			}

			// Byte 6: Min Luminance cv = (MinLum * 255 * 255) / (MaxLum * 100)
			if (length >= 6 && (pos + 6) < 256)
			{
				float minCv = (minNits * 255.0f * 255.0f) / (maxNits * 100.0f);
				edid[pos + 6] = static_cast<BYTE>(roundf(max(0.0f, min(255.0f, minCv))));
				float actualNits = (edid[pos + 6] * maxNits * 100.0f) / (255.0f * 255.0f);
				vddlog("d", ("Set MinLum cv=" + to_string((int)edid[pos + 6]) + " (req " + to_string(minNits) + ", actual " + to_string(actualNits) + " nits)").c_str());
			}

			edid[255] = calculateCeaChecksum(edid);
			vddlog("d", "Updated CEA extension checksum");
			return;
		}

		pos += length + 1;
	}

	vddlog("w", "HDR Static Metadata block not found in EDID");
}

// Update AMD FreeSync VSDB rate range in EDID CEA extension.
// AMD FreeSync VSDB layout (after CEA tag/length header byte):
//   OUI bytes 0..2 = 0x1A, 0x00, 0x00 (little-endian 0x00001A = AMD)
//   payload[0] = version (e.g. 0x01)
//   payload[1] = caps (bit0 = FreeSync supported)
//   payload[2] = min refresh rate (Hz)
//   payload[3] = max refresh rate (Hz)
//   payload[4] = flags
// Both min and max are clamped to [1, 255]; min<=max enforced.
// Updates CEA extension checksum on success.
void updateEdidFreeSyncRange(vector<BYTE> &edid, BYTE minHz, BYTE maxHz)
{
	if (edid.size() < 256)
	{
		vddlog("w", "EDID too small to update FreeSync range");
		return;
	}
	if (minHz < 1) minHz = 1;
	if (maxHz < minHz) maxHz = minHz;

	const int ceaStart = 128;
	int dtdOffset = edid[ceaStart + 2];
	if (dtdOffset == 0)
		dtdOffset = 4;
	const int endPos = ceaStart + dtdOffset;

	for (int pos = ceaStart + 4; pos < endPos && pos < 256;)
	{
		const BYTE header = edid[pos];
		const int tag = (header >> 5) & 0x07;
		const int length = header & 0x1F;

		// Vendor-Specific Data Block (tag 0x03), need at least 3 OUI bytes
		if (tag == 0x03 && length >= 3 && (pos + 3) < 256)
		{
			// OUI is little-endian in EDID stream: bytes 1..3 = LSB..MSB
			if (edid[pos + 1] == 0x1A && edid[pos + 2] == 0x00 && edid[pos + 3] == 0x00)
			{
				// AMD FreeSync VSDB: need payload bytes 0..3 (min/max at +6/+7)
				if (length >= 7 && (pos + 7) < 256)
				{
					BYTE oldMin = edid[pos + 6];
					BYTE oldMax = edid[pos + 7];
					edid[pos + 6] = minHz;
					edid[pos + 7] = maxHz;
					edid[255] = calculateCeaChecksum(edid);
					stringstream ss;
					ss << "FreeSync VSDB rate range " << (int)oldMin << "-" << (int)oldMax
					   << " Hz -> " << (int)minHz << "-" << (int)maxHz << " Hz";
					vddlog("d", ss.str().c_str());
					return;
				}
				vddlog("w", "AMD FreeSync VSDB found but length too small for rate range");
				return;
			}
		}
		pos += length + 1;
	}
	vddlog("w", "AMD FreeSync VSDB not found in EDID");
}

vector<BYTE> loadEdid(const string &filePath)
{
	vector<BYTE> hardcodedEdid = GetHardcodedEdid();

	if (customEdid)
	{
		vddlog("i", "Attempting to use user Edid");
	}
	else
	{
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}

	ifstream file(filePath, ios::binary | ios::ate);
	if (!file)
	{
		vddlog("i", "No custom edid found");
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}

	streamsize size = file.tellg();
	file.seekg(0, ios::beg);

	if (size <= 0 || size > 1024 * 1024)
	{
		vddlog("e", "Custom edid file size invalid (must be >0 and <=1MB)");
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}

	vector<BYTE> buffer(static_cast<size_t>(size));
	if (file.read((char *)buffer.data(), size))
	{
		// calculate checksum and compare it to 127 byte, if false then return hardcoded if true then return buffer to prevent loading borked edid.
		BYTE calculatedChecksum = calculateChecksum(buffer);
		if (calculatedChecksum != buffer[127])
		{
			vddlog("e", "Custom edid failed due to invalid checksum");
			vddlog("i", "Using hardcoded edid");
			return hardcodedEdid;
		}

		if (edidCeaOverride)
		{
			if (buffer.size() == 256)
			{
				for (int i = 128; i < 256; ++i)
				{
					buffer[i] = hardcodedEdid[i];
				}
				updateCeaExtensionCount(buffer, 1);
			}
			else if (buffer.size() == 128)
			{
				buffer.insert(buffer.end(), hardcodedEdid.begin() + 128, hardcodedEdid.end());
				updateCeaExtensionCount(buffer, 1);
			}
		}

		vddlog("i", "Using custom edid");
		return buffer;
	}
	else
	{
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}
}

int maincalc()
{
	vector<BYTE> edid = loadEdid(WStringToString(confpath) + "\\user_edid.bin");

	// Validate EDID data
	if (edid.empty())
	{
		vddlog("e", "EDID data is empty, adapter initialization may fail");
		return -1;
	}

	if (edid.size() < 128)
	{
		stringstream ss;
		ss << "EDID data too small (" << edid.size() << " bytes), expected at least 128 bytes";
		vddlog("e", ss.str().c_str());
		return -1;
	}

	if (!preventManufacturerSpoof)
		modifyEdid(edid);
	BYTE checksum = calculateChecksum(edid);
	edid[127] = checksum;

	// Setting this variable is depricated, hardcoded edid is either returned or custom in loading edid function
	IndirectDeviceContext::s_KnownMonitorEdid = edid;

	stringstream ss;
	ss << "EDID data loaded successfully (" << edid.size() << " bytes)";
	vddlog("d", ss.str().c_str());

	return 0;
}

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) : m_WdfDevice(WdfDevice)
{
	m_Adapter = {};
}

// Helper function to wait with system state validation
bool IndirectDeviceContext::WaitForSystemStabilization(int timeoutMs, const char *operation)
{
	stringstream logStream;
	logStream << "Waiting for system stabilization during: " << operation;
	vddlog("d", logStream.str().c_str());

	const int checkInterval = 25; // Check every 25ms
	int elapsed = 0;

	while (elapsed < timeoutMs)
	{
		Sleep(checkInterval);
		elapsed += checkInterval;

		// Basic validation - check if we still have any monitors
		if (m_Monitors.empty())
		{
			vddlog("d", "All monitor handles became null during wait - system may have cleaned up");
			return true; // This might be expected in some cases
		}

		// Check if we're in the middle of multiple operations
		if (elapsed % 100 == 0)
		{ // Log every 100ms
			logStream.str("");
			logStream << "Still waiting for stabilization... (" << elapsed << "ms/" << timeoutMs << "ms)";
			vddlog("d", logStream.str().c_str());
		}
	}

	logStream.str("");
	logStream << "System stabilization wait completed for: " << operation;
	vddlog("d", logStream.str().c_str());
	return true;
}

// Helper function to validate monitor state before critical operations
bool IndirectDeviceContext::ValidateMonitorState(const char *operation)
{
	stringstream logStream;
	logStream << "Validating monitor state for: " << operation;
	vddlog("d", logStream.str().c_str());

	if (m_Monitors.empty())
	{
		logStream.str("");
		logStream << "No monitors exist during: " << operation;
		vddlog("w", logStream.str().c_str());
		return false;
	}

	// Additional state checks could be added here

	logStream.str("");
	logStream << "Monitor state validation passed for: " << operation;
	vddlog("d", logStream.str().c_str());
	return true;
}

IndirectDeviceContext::~IndirectDeviceContext()
{
	stringstream logStream;

	logStream << "Destroying IndirectDeviceContext. Starting cleanup process.";
	vddlog("d", logStream.str().c_str());

	// First stop all SwapChain processing threads
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
	catch (const std::exception &e)
	{
		stringstream errorStream;
		errorStream << "Exception while stopping SwapChains in destructor: " << e.what();
		vddlog("e", errorStream.str().c_str());
	}
	catch (...)
	{
		vddlog("e", "Unknown exception while stopping SwapChains in destructor");
	}

	// Clean up all hardware cursor event handles
	for (auto &pair : m_MouseEvents)
	{
		if (pair.second != nullptr)
		{
			CloseHandle(pair.second);
		}
	}
	m_MouseEvents.clear();
	vddlog("d", "Hardware cursor event handles cleaned up in destructor");

	// Clean up all monitors
	for (auto &pair : m_Monitors)
	{
		try
		{
			if (pair.second != nullptr)
			{
				vddlog("d", ("Cleaning up monitor " + std::to_string(pair.first) + " in destructor").c_str());
				// Do not call IddCxMonitorDeparture, as this may not be safe in the destructor
				WdfObjectDelete(pair.second);
			}
		}
		catch (const std::exception &e)
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

#define NUM_VIRTUAL_DISPLAYS 1 // What is this even used for ?? Its never referenced

void IndirectDeviceContext::InitAdapter()
{
	stringstream logStream;

	// Load settings and GPU configuration first
	loadSettings();
	logStream.str("");
	if (gpuname.empty() || gpuname == L"default")
	{
		const wstring adaptername = confpath + L"\\adapter.txt";
		Options.Adapter.load(adaptername.c_str());
		logStream << "Attempting to Load GPU from adapter.txt";
	}
	else
	{
		Options.Adapter.xmlprovide(gpuname);
		logStream << "Loading GPU from vdd_settings.xml";
	}
	vddlog("i", logStream.str().c_str());
	GetGpuInfo();

	// Validate EDID data before proceeding
	int edidResult = maincalc();
	if (edidResult != 0)
	{
		vddlog("e", "EDID validation failed, adapter initialization will likely fail");
		// Continue anyway, but log the warning
	}

	// Validate monitor modes
	if (monitorModes.empty())
	{
		vddlog("w", "No monitor modes loaded, adding fallback 1920x1080@60Hz mode");
		int vsync_num, vsync_den;
		float_to_vsync(60.0f, vsync_num, vsync_den);
		monitorModes.push_back(std::make_tuple(1920, 1080, vsync_num, vsync_den));
	}

	stringstream modeLog;
	modeLog << "Loaded " << monitorModes.size() << " monitor modes for adapter initialization";
	vddlog("d", modeLog.str().c_str());

	// ==============================
	// TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
	// numbers are used for telemetry and may be displayed to the user in some situations.
	//
	// This is also where static per-adapter capabilities are determined.
	// ==============================

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

	// VRR / FreeSync support flag (IddCx >= 1.4). The flag value 0x4 is
	// stable across SDK versions; older WDKs that don't ship the macro fall
	// back to the literal so the build stays portable. IddCx hosts that
	// don't understand the bit just ignore it, so this is safe.
	if (vrrEnabled.load())
	{
#ifdef IDDCX_ADAPTER_FLAGS_VARIABLE_REFRESH_RATE_SUPPORTED
		AdapterCaps.Flags |= IDDCX_ADAPTER_FLAGS_VARIABLE_REFRESH_RATE_SUPPORTED;
#else
		AdapterCaps.Flags |= 0x4; // VARIABLE_REFRESH_RATE_SUPPORTED
#endif
		logStream << " VRR adapter flag enabled.";
	}

	// Validate and set monitor count with bounds checking
	if (numVirtualDisplays == 0 || numVirtualDisplays > 16)
	{
		logStream << "Invalid numVirtualDisplays value: " << numVirtualDisplays << ". Setting to 1.";
		vddlog("w", logStream.str().c_str());
		numVirtualDisplays = 1;
		logStream.str("");
	}

	// Declare basic feature support for the adapter (required)
	AdapterCaps.MaxMonitorsSupported = numVirtualDisplays;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	// Declare your device strings for telemetry (required)
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"ZakoVdd Device";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"ZakoTech";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"ZakoVdd Model";

	// Declare your hardware and firmware versions (required)
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

	// Initialize a WDF context that can store a pointer to the device context object
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	// Validate required parameters before initialization
	if (AdapterInit.WdfDevice == nullptr)
	{
		vddlog("e", "WdfDevice is null, cannot initialize adapter");
		return;
	}

	// Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	logStream << "Adapter Initialization Status: 0x" << std::hex << Status;
	vddlog("d", logStream.str().c_str());
	logStream.str("");

	if (NT_SUCCESS(Status))
	{
		// Validate the output handle
		if (AdapterInitOut.AdapterObject == nullptr)
		{
			vddlog("e", "Adapter initialization returned null handle despite success status");
			return;
		}

		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;
		logStream << "Adapter handle stored successfully.";
		vddlog("d", logStream.str().c_str());

		// Store the device context object into the WDF object context
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
		logStream << "Failed to initialize adapter. Status: 0x" << std::hex << Status;
		vddlog("e", logStream.str().c_str());

		// Log additional diagnostic information
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

void IndirectDeviceContext::CreateMonitor(unsigned int index, const GUID *pClientGuid, float maxNits, float minNits, float maxFALL, float widthCm, float heightCm)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	wstring logMessage = L"Creating Monitor: " + to_wstring(index + 1);
	string narrowLogMessage = WStringToString(logMessage);
	vddlog("i", narrowLogMessage.c_str());

	// Log HDR luminance settings and physical size
	{
		stringstream ss;
		ss << "Monitor " << (index + 1) << " HDR settings - MaxNits: " << maxNits << ", MaxFALL: " << maxFALL << ", MinNits: " << minNits;
		if (widthCm > 0 && heightCm > 0)
		{
			ss << ", Physical size: " << widthCm << " x " << heightCm << " cm";
		}
		vddlog("d", ss.str().c_str());
	}

	// ==============================
	// TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDID
	// provided here is purely for demonstration, as it describes only 640x480 @ 60 Hz and 800x600 @ 60 Hz. Monitor
	// manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS to optimize
	// settings like viewing distance and scale factor. Manufacturers should also use a unique serial number every
	// single device to ensure the OS can tell the monitors apart.
	// ==============================

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	// Compute current max refresh rate from monitorModes for FreeSync VSDB rate range.
	// Min stays at 48 Hz (typical FreeSync floor; OS LFC handles below).
	BYTE freeSyncMinHz = 48;
	BYTE freeSyncMaxHz = 60;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		float maxHz = 0.0f;
		for (const auto &m : monitorModes)
		{
			int num = std::get<2>(m);
			int den = std::get<3>(m);
			if (den > 0)
			{
				float hz = static_cast<float>(num) / static_cast<float>(den);
				if (hz > maxHz) maxHz = hz;
			}
		}
		int rounded = static_cast<int>(maxHz + 0.5f);
		if (rounded < freeSyncMinHz) rounded = freeSyncMinHz;
		if (rounded > 255) rounded = 255;
		freeSyncMaxHz = static_cast<BYTE>(rounded);
	}

	// Get or create EDID for this client GUID
	// Use static storage to ensure EDID data persists for the lifetime of the monitor
	vector<BYTE> *pMonitorEdid = nullptr;
	if (pClientGuid != nullptr)
	{
		lock_guard<mutex> edidLock(s_EdidMapMutex);
		auto it = s_ClientGuidEdidMap.find(*pClientGuid);
		if (it != s_ClientGuidEdidMap.end())
		{
			// Use existing EDID for this GUID
			pMonitorEdid = &it->second;
			stringstream ss;
			ss << "Using existing EDID for client GUID (monitor " << (index + 1) << ")";
			vddlog("d", ss.str().c_str());

			// Update HDR metadata in EDID with new luminance values
			updateEdidHdrMetadata(*pMonitorEdid, maxNits, minNits, maxFALL);

			// Update FreeSync VSDB rate range to match current mode list
			updateEdidFreeSyncRange(*pMonitorEdid, freeSyncMinHz, freeSyncMaxHz);

			// Update physical size in EDID if provided
			if (widthCm > 0 || heightCm > 0)
			{
				updateEdidPhysicalSize(*pMonitorEdid, widthCm, heightCm);
			}

			// Recalculate checksum after modifying EDID
			BYTE checksum = calculateChecksum(*pMonitorEdid);
			(*pMonitorEdid)[127] = checksum;
		}
		else
		{
			// Create new EDID copy and modify serial number based on client GUID
			// Same GUID will always produce the same serial number
			s_ClientGuidEdidMap[*pClientGuid] = s_KnownMonitorEdid;
			pMonitorEdid = &s_ClientGuidEdidMap[*pClientGuid];
			modifyEdidSerialNumber(*pMonitorEdid, *pClientGuid);

			// Update HDR metadata in EDID with luminance values
			updateEdidHdrMetadata(*pMonitorEdid, maxNits, minNits, maxFALL);

			// Update FreeSync VSDB rate range to match current mode list
			updateEdidFreeSyncRange(*pMonitorEdid, freeSyncMinHz, freeSyncMaxHz);

			// Update physical size in EDID if provided
			if (widthCm > 0 || heightCm > 0)
			{
				updateEdidPhysicalSize(*pMonitorEdid, widthCm, heightCm);
			}

			// Recalculate checksum after modifying EDID
			BYTE checksum = calculateChecksum(*pMonitorEdid);
			(*pMonitorEdid)[127] = checksum;

			stringstream ss;
			ss << "Created new EDID with serial number based on client GUID for monitor " << (index + 1);
			vddlog("d", ss.str().c_str());
		}
	}
	else
	{
		// No client GUID, create a temporary copy to update HDR metadata
		// Note: For monitors without GUID, we update the shared EDID
		// This might affect other monitors using the same EDID
		updateEdidHdrMetadata(s_KnownMonitorEdid, maxNits, minNits, maxFALL);

		// Update FreeSync VSDB rate range to match current mode list
		updateEdidFreeSyncRange(s_KnownMonitorEdid, freeSyncMinHz, freeSyncMaxHz);

		// Update physical size in EDID if provided
		if (widthCm > 0 || heightCm > 0)
		{
			updateEdidPhysicalSize(s_KnownMonitorEdid, widthCm, heightCm);
		}

		// Recalculate checksum after modifying EDID
		BYTE checksum = calculateChecksum(s_KnownMonitorEdid);
		s_KnownMonitorEdid[127] = checksum;

		pMonitorEdid = &s_KnownMonitorEdid;
	}

	// Track GUID for cleanup in DestroyMonitor
	if (pClientGuid != nullptr)
	{
		m_MonitorGuids[index] = *pClientGuid;
	}
	else
	{
		m_MonitorGuids.erase(index);
	}

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = index;
	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	// MonitorInfo.MonitorDescription.DataSize = sizeof(s_KnownMonitorEdid);        can no longer use size of as converted to vector
	if (pMonitorEdid->size() > UINT_MAX)
	{
		vddlog("e", "Edid size passes UINT_Max, escape to prevent loading borked display");
	}
	else
	{
		MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(pMonitorEdid->size());
	}
	// MonitorInfo.MonitorDescription.pData = const_cast<BYTE*>(s_KnownMonitorEdid);
	//  Changed from using const_cast to data() to safely access the EDID data.
	//  This improves type safety and code readability, as it eliminates the need for casting
	//  and ensures we are directly working with the underlying container of known monitor EDID data.
	//  Use the monitor-specific EDID (either shared or client-specific)
	MonitorInfo.MonitorDescription.pData = pMonitorEdid->data();

	// ==============================
	// TODO: The monitor's container ID should be distinct from "this" device's container ID if the monitor is not
	// permanently attached to the display adapter device object. The container ID is typically made unique for each
	// monitor and can be used to associate the monitor with other devices, like audio or input devices. In this
	// sample we generate a random container ID GUID, but it's best practice to choose a stable container ID for a
	// unique monitor or to use "this" device's container ID for a permanent/integrated monitor.
	// ==============================

	// Use client-provided GUID as container ID if available, otherwise generate a new one
	if (pClientGuid != nullptr)
	{
		MonitorInfo.MonitorContainerId = *pClientGuid;
		stringstream ss;
		ss << "Using client-provided GUID as container ID for monitor " << (index + 1);
		vddlog("d", ss.str().c_str());
	}
	else
	{
		CoCreateGuid(&MonitorInfo.MonitorContainerId);
		vddlog("d", "Created container ID");
	}

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	// Create a monitor object with the specified monitor descriptor
	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (NT_SUCCESS(Status))
	{
		vddlog("d", "Monitor created successfully.");
		IDDCX_MONITOR newMonitor = MonitorCreateOut.MonitorObject;

		if (newMonitor == nullptr)
		{
			vddlog("e", "Invalid monitor handle");
			return;
		}

		// Store in monitors map
		m_Monitors[index] = newMonitor;

		// Store HDR luminance settings for this monitor
		{
			lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
			MonitorHdrSettings hdrSettings;
			hdrSettings.isHdr = false;
			hdrSettings.maxNits = maxNits;
			hdrSettings.minNits = minNits;
			hdrSettings.maxFALL = maxFALL;
			s_MonitorHdrSettingsMap[newMonitor] = hdrSettings;

			stringstream ss;
			ss << "Stored HDR settings for monitor " << (index + 1)
			   << " - IsHdr: false, MaxNits: " << maxNits
			   << ", MinNits: " << minNits
			   << ", MaxFALL: " << maxFALL;
			vddlog("d", ss.str().c_str());
		}

		// Associate the monitor with this device context
		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorCreateOut.MonitorObject);
		pContext->pContext = this;

		// Tell the OS that the monitor has been plugged in
		IDARG_OUT_MONITORARRIVAL ArrivalOut;
		Status = IddCxMonitorArrival(newMonitor, &ArrivalOut);
		if (NT_SUCCESS(Status))
		{
			vddlog("d", "Monitor arrival successfully reported.");
		}
		else
		{
			stringstream ss;
			ss << "Failed to report monitor arrival. Status: " << Status;
			vddlog("e", ss.str().c_str());
		}
	}
	else
	{
		stringstream ss;
		ss << "Failed to create monitor. Status: " << Status;
		vddlog("e", ss.str().c_str());
	}
}

void IndirectDeviceContext::DestroyMonitor(unsigned int index)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	auto monIt = m_Monitors.find(index);
	if (monIt == m_Monitors.end() || monIt->second == nullptr)
	{
		stringstream ws;
		ws << "Monitor handle for index " << index << " is already null or not found";
		vddlog("w", ws.str().c_str());
		return;
	}

	IDDCX_MONITOR hMonitor = monIt->second;

	stringstream logStream;
	logStream << "Destroying monitor (Index: " << index << ")";
	vddlog("d", logStream.str().c_str());

	try
	{
		// Clean up HDR settings for this monitor
		{
			lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
			auto it = s_MonitorHdrSettingsMap.find(hMonitor);
			if (it != s_MonitorHdrSettingsMap.end())
			{
				s_MonitorHdrSettingsMap.erase(it);
				vddlog("d", "Cleaned up HDR settings for monitor");
			}
		}

		m_CommittedTargetModes.erase(hMonitor);

		// Clean up EDID cache for this monitor's client GUID
		{
			auto guidIt = m_MonitorGuids.find(index);
			if (guidIt != m_MonitorGuids.end())
			{
				lock_guard<mutex> edidLock(s_EdidMapMutex);
				s_ClientGuidEdidMap.erase(guidIt->second);
				m_MonitorGuids.erase(guidIt);
				vddlog("d", "Cleaned up EDID cache for monitor client GUID");
			}
		}

		// Step 1: Stop SwapChain processing for this monitor
		{
			auto scIt = m_ProcessingThreads.find(hMonitor);
			if (scIt != m_ProcessingThreads.end())
			{
				vddlog("d", "Stopping SwapChain processing thread before monitor destruction");
				scIt->second.reset();
				m_ProcessingThreads.erase(scIt);
				vddlog("d", "SwapChain processing thread stopped");
			}
		}

		// Step 1.5: Clean up hardware cursor event handle for this monitor
		{
			auto meIt = m_MouseEvents.find(hMonitor);
			if (meIt != m_MouseEvents.end())
			{
				if (meIt->second != nullptr)
				{
					vddlog("d", "Cleaning up hardware cursor event handle");
					CloseHandle(meIt->second);
				}
				m_MouseEvents.erase(meIt);
				vddlog("d", "Hardware cursor event handle cleaned up");
			}
		}

		// Step 2: Wait for all resources to stabilize
		Sleep(300);

		// Step 3: Report monitor departure to the system with retry mechanism
		NTSTATUS Status = STATUS_UNSUCCESSFUL;
		{
			vddlog("d", "Reporting monitor departure to system");
			int retryCount = 0;
			const int maxRetries = 3;

			while (retryCount < maxRetries)
			{
				Status = IddCxMonitorDeparture(hMonitor);
				if (NT_SUCCESS(Status))
				{
					vddlog("d", "Successfully reported monitor departure");
					break;
				}
				else
				{
					retryCount++;
					stringstream errorStream;
					errorStream << "Failed to report monitor departure attempt " << retryCount
								<< "/" << maxRetries << ". Status: 0x" << hex << Status;
					vddlog("w", errorStream.str().c_str());

					if (retryCount < maxRetries)
					{
						Sleep(100); // Wait before retry
					}
				}
			}
		}

		if (!NT_SUCCESS(Status))
		{
			vddlog("e", "All monitor departure attempts failed, continuing with cleanup");
		}

		// Step 4: Wait for system to process the departure
		Sleep(500);

		// Step 5: Safely delete the monitor object
		vddlog("d", "Deleting monitor WDF object");
		WdfObjectDelete(hMonitor);
		m_Monitors.erase(monIt);
		vddlog("d", "Monitor WDF object deleted successfully");

		logStream.str("");
		logStream << "Monitor object destroyed successfully (Index: " << index << ")";
		vddlog("i", logStream.str().c_str());
	}
	catch (const std::exception &e)
	{
		stringstream errorStream;
		errorStream << "Exception during monitor destruction (Index: " << index << "): " << e.what();
		vddlog("e", errorStream.str().c_str());

		// Force cleanup even after exception
		m_Monitors.erase(index);
	}
	catch (...)
	{
		vddlog("e", "Unknown exception during monitor destruction");
		m_Monitors.erase(index);
	}
}

void IndirectDeviceContext::AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	// Stop any existing swap chain processing for this monitor
	{
		auto scIt = m_ProcessingThreads.find(Monitor);
		if (scIt != m_ProcessingThreads.end())
		{
			scIt->second.reset();
			m_ProcessingThreads.erase(scIt);
		}
	}

	auto Device = make_shared<Direct3DDevice>(RenderAdapter);
	HRESULT hr = Device->Init();
	if (FAILED(hr))
	{
		stringstream ss;
		ss << "Failed to initialize Direct3DDevice. HRESULT: " << hr << ". Deleting existing swap-chain.";
		vddlog("e", ss.str().c_str());
		WdfObjectDelete(SwapChain);
		return;
	}
	else
	{
		vddlog("d", "Creating a new swap-chain processing thread.");

		// Reverse-look up monitor index for the VDD->Sunshine frame exporter.
		unsigned int monitorIndex = 0xFFFFFFFFu;
		for (const auto &kv : m_Monitors)
		{
			if (kv.second == Monitor)
			{
				monitorIndex = kv.first;
				break;
			}
		}

		m_ProcessingThreads[Monitor] = std::unique_ptr<SwapChainProcessor>(new SwapChainProcessor(SwapChain, Device, NewFrameEvent, monitorIndex));

		auto procIt = m_ProcessingThreads.find(Monitor);
		if (procIt != m_ProcessingThreads.end() && procIt->second)
		{
			auto modeIt = m_CommittedTargetModes.find(Monitor);
			if (modeIt != m_CommittedTargetModes.end())
			{
				procIt->second->PublishModeMetadata(modeIt->second);
			}

			lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
			auto hdrIt = s_MonitorHdrSettingsMap.find(Monitor);
			if (hdrIt != s_MonitorHdrSettingsMap.end())
			{
				procIt->second->UpdateHdrMetadata(hdrIt->second.isHdr,
				                                hdrIt->second.maxNits,
				                                hdrIt->second.minNits,
				                                hdrIt->second.maxFALL);
			}
		}

		if (hardwareCursor)
		{
			// Clean up any existing mouse event handle for this monitor
			auto meIt = m_MouseEvents.find(Monitor);
			if (meIt != m_MouseEvents.end())
			{
				if (meIt->second != nullptr)
				{
					CloseHandle(meIt->second);
				}
				m_MouseEvents.erase(meIt);
				vddlog("d", "Cleaned up existing mouse event handle");
			}

			HANDLE hMouseEvent = CreateEventA(
				nullptr,
				false,
				false,
				nullptr);  // Use null name to allow multiple monitors to each have their own event

			if (!hMouseEvent)
			{
				vddlog("e", "Failed to create mouse event. No hardware cursor supported!");
				return;
			}

			m_MouseEvents[Monitor] = hMouseEvent;

			IDDCX_CURSOR_CAPS cursorInfo = {};
			cursorInfo.Size = sizeof(cursorInfo);
			cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
			cursorInfo.AlphaCursorSupport = alphaCursorSupport;

			cursorInfo.MaxX = CursorMaxX;
			cursorInfo.MaxY = CursorMaxY;

			IDARG_IN_SETUP_HWCURSOR hwCursor = {};
			hwCursor.CursorInfo = cursorInfo;
			hwCursor.hNewCursorDataAvailable = hMouseEvent;

			NTSTATUS Status = IddCxMonitorSetupHardwareCursor(
				Monitor,
				&hwCursor);

			if (FAILED(Status))
			{
				CloseHandle(hMouseEvent);
				m_MouseEvents.erase(Monitor);
				vddlog("e", "Failed to setup hardware cursor");
				return;
			}

			vddlog("d", "Hardware cursor setup completed successfully.");
		}
		else
		{
			vddlog("d", "Hardware cursor is disabled, Skipped creation.");
		}
	}
}

void IndirectDeviceContext::CommitModes(const IDARG_IN_COMMITMODES* pInArgs)
{
	if (!pInArgs || !pInArgs->pPaths)
	{
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);
	for (UINT i = 0; i < pInArgs->PathCount; ++i)
	{
		const auto& path = pInArgs->pPaths[i];
		if (!path.MonitorObject)
		{
			continue;
		}

		if ((path.Flags & IDDCX_PATH_FLAGS_ACTIVE) != 0)
		{
			m_CommittedTargetModes[path.MonitorObject] = path.TargetVideoSignalInfo;

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->PublishModeMetadata(path.TargetVideoSignalInfo);
			}
		}
		else
		{
			m_CommittedTargetModes.erase(path.MonitorObject);
		}
	}
}

void IndirectDeviceContext::CommitModes2(const IDARG_IN_COMMITMODES2* pInArgs)
{
	if (!pInArgs || !pInArgs->pPaths)
	{
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);
	for (UINT i = 0; i < pInArgs->PathCount; ++i)
	{
		const auto& path = pInArgs->pPaths[i];
		if (!path.MonitorObject)
		{
			continue;
		}

		if ((path.Flags & IDDCX_PATH_FLAGS_ACTIVE) != 0)
		{
			m_CommittedTargetModes[path.MonitorObject] = path.TargetVideoSignalInfo;

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->PublishModeMetadata(path.TargetVideoSignalInfo);
			}
		}
		else
		{
			m_CommittedTargetModes.erase(path.MonitorObject);
		}
	}
}

void IndirectDeviceContext::UpdateMonitorHdrMetadata(IDDCX_MONITOR Monitor, bool isHdr, float maxNits, float minNits, float maxFALL)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	{
		lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
		auto& hdrSettings = s_MonitorHdrSettingsMap[Monitor];
		hdrSettings.isHdr = isHdr;
		hdrSettings.maxNits = maxNits;
		hdrSettings.minNits = minNits;
		hdrSettings.maxFALL = maxFALL;
	}

	auto procIt = m_ProcessingThreads.find(Monitor);
	if (procIt != m_ProcessingThreads.end() && procIt->second)
	{
		procIt->second->UpdateHdrMetadata(isHdr, maxNits, minNits, maxFALL);
	}
}

void IndirectDeviceContext::UnassignSwapChain(IDDCX_MONITOR Monitor)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	vddlog("i", "Unassigning Swapchain. Processing will be stopped.");

	auto scIt = m_ProcessingThreads.find(Monitor);
	if (scIt != m_ProcessingThreads.end())
	{
		try
		{
			vddlog("d", "Stopping SwapChain processing thread");
			auto processingThread = std::move(scIt->second);
			m_ProcessingThreads.erase(scIt);

			Sleep(50);
			processingThread.reset();
			vddlog("d", "SwapChain processing thread stopped successfully");
			Sleep(25);
		}
		catch (const std::exception &e)
		{
			stringstream errorStream;
			errorStream << "Exception while stopping SwapChain processing thread: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...)
		{
			vddlog("e", "Unknown exception while stopping SwapChain processing thread");
		}
	}
	else
	{
		vddlog("d", "No SwapChain processing thread to stop for this monitor");
	}
}

void IndirectDeviceContext::UnassignAllSwapChains()
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	vddlog("i", "Unassigning all SwapChains.");
	for (auto it = m_ProcessingThreads.begin(); it != m_ProcessingThreads.end();)
	{
		try
		{
			it->second.reset();
			it = m_ProcessingThreads.erase(it);
		}
		catch (...)
		{
			vddlog("e", "Exception while stopping a SwapChain processing thread");
			it = m_ProcessingThreads.erase(it);
		}
	}
	Sleep(50);
}

void IndirectDeviceContext::DestroyAllMonitors()
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	vddlog("i", "Destroying all monitors.");
	// Get a copy of the keys to iterate safely
	vector<unsigned int> indices;
	for (const auto &pair : m_Monitors)
	{
		indices.push_back(pair.first);
	}
	for (unsigned int idx : indices)
	{
		DestroyMonitor(idx);
		if (!m_Monitors.empty())
		{
			Sleep(50);
		}
	}
}

int IndirectDeviceContext::RefreshMonitorModes()
{
	// Push the current monitorModes snapshot to all live IDDCX_MONITOR objects
	// via IddCxMonitorUpdateModes2. This avoids the DWM window rearrangement
	// triggered by full monitor departure + arrival when only the mode list
	// (resolution / refresh rate set) changes.
	//
	// Returns: number of monitors successfully refreshed, or -1 if the IddCx
	// runtime does not export IddCxMonitorUpdateModes2 (older OS / SDK).
	if (!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2))
	{
		vddlog("w", "RefreshMonitorModes: IddCxMonitorUpdateModes2 not available on this OS");
		return -1;
	}

	// Snapshot mode list under data lock and rebuild s_KnownMonitorModes2
	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
		s_KnownMonitorModes2.clear();
		for (size_t i = 0; i < localModes.size(); ++i)
		{
			s_KnownMonitorModes2.push_back(dispinfo(
				std::get<0>(localModes[i]),
				std::get<1>(localModes[i]),
				std::get<2>(localModes[i]),
				std::get<3>(localModes[i])));
		}
	}

	if (localModes.empty())
	{
		vddlog("w", "RefreshMonitorModes: monitorModes is empty, refusing to push");
		return 0;
	}

	// Build IDDCX_TARGET_MODE2 array once - same payload for every monitor.
	vector<IDDCX_TARGET_MODE2> targetModes(localModes.size());
	for (size_t i = 0; i < localModes.size(); ++i)
	{
		CreateTargetMode2(targetModes[i],
			static_cast<UINT>(std::get<0>(localModes[i])),
			static_cast<UINT>(std::get<1>(localModes[i])),
			static_cast<UINT>(std::get<2>(localModes[i])),
			static_cast<UINT>(std::get<3>(localModes[i])));
	}

	// Iterate live monitors under monitor lock and push the new mode list.
	int refreshed = 0;
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);
	for (const auto &pair : m_Monitors)
	{
		IDDCX_MONITOR hMonitor = pair.second;
		if (hMonitor == nullptr)
			continue;

		IDARG_IN_UPDATEMODES2 inArgs = {};
		inArgs.Reason = IDDCX_UPDATE_REASON_OTHER;
		inArgs.TargetModeCount = static_cast<UINT>(targetModes.size());
		inArgs.pTargetModes = targetModes.data();

		NTSTATUS status = IddCxMonitorUpdateModes2(hMonitor, &inArgs);
		stringstream ss;
		ss << "RefreshMonitorModes: monitor index=" << pair.first
		   << " status=0x" << std::hex << status << " modeCount=" << std::dec << targetModes.size();
		if (NT_SUCCESS(status))
		{
			++refreshed;
			vddlog("d", ss.str().c_str());
		}
		else
		{
			vddlog("w", ss.str().c_str());
		}
	}

	stringstream summary;
	summary << "RefreshMonitorModes: pushed " << refreshed << "/" << m_Monitors.size()
	        << " monitors with " << targetModes.size() << " modes (no departure)";
	vddlog("i", summary.str().c_str());
	return refreshed;
}

#pragma endregion

#pragma region DDI Callbacks

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
		vddlog("d", "Adapter initialization finished successfully.");
	}
	else
	{
		stringstream ss;
		ss << "Adapter initialization failed. Status: " << pInArgs->AdapterInitStatus;
		vddlog("e", ss.str().c_str());
	}
	vddlog("i", "Finished Setting up adapter.");

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

	// For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

	// ==============================
	// TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
	// through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
	// should be turned off).
	// ==============================

	return STATUS_SUCCESS;
}
_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION *pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	stringstream logStream;
	logStream << "Parsing monitor description. Input buffer count: " << pInArgs->MonitorModeBufferInputCount;
	vddlog("d", logStream.str().c_str());

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

	logStream.str("");
	logStream << "Number of monitor modes generated: " << localModes.size();
	vddlog("d", logStream.str().c_str());

	if (pInArgs->MonitorModeBufferInputCount < localModes.size())
	{
		logStream.str("");
		logStream << "Buffer too small. Input count: " << pInArgs->MonitorModeBufferInputCount << ", Required: " << localModes.size();
		vddlog("w", logStream.str().c_str());
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		// Copy the known modes to the output buffer
		for (DWORD ModeIndex = 0; ModeIndex < localModes.size(); ModeIndex++)
		{
			pInArgs->pMonitorModes[ModeIndex].Size = sizeof(IDDCX_MONITOR_MODE);
			pInArgs->pMonitorModes[ModeIndex].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
			pInArgs->pMonitorModes[ModeIndex].MonitorVideoSignalInfo = s_KnownMonitorModes2[ModeIndex];
		}

		// Set the preferred mode as represented in the EDID
		pOutArgs->PreferredMonitorModeIdx = 0;
		vddlog("d", "Monitor description parsed successfully.");
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

	// Should never be called since we create a single monitor with a known EDID in this sample driver.

	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for a monitor with no EDID.
	// Drivers should report modes that are guaranteed to be supported by the transport protocol and by nearly all
	// monitors (such 640x480, 800x600, or 1024x768). If the driver has access to monitor modes from a descriptor other
	// than an EDID, those modes would also be reported here.
	// ==============================

	return STATUS_NOT_IMPLEMENTED;
}

/// <summary>
/// Creates a target mode from the fundamental mode attributes.
/// </summary>
void CreateTargetMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO &Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	stringstream logStream;
	logStream << "Creating target mode with Width: " << Width
			  << ", Height: " << Height
			  << ", VSyncNum: " << VSyncNum
			  << ", VSyncDen: " << VSyncDen;
	vddlog("d", logStream.str().c_str());

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

	logStream.str("");
	logStream << "Target mode configured with:"
			  << "\n  Total Size: (" << Mode.totalSize.cx << ", " << Mode.totalSize.cy << ")"
			  << "\n  Active Size: (" << Mode.activeSize.cx << ", " << Mode.activeSize.cy << ")"
			  << "\n  vSync Frequency: " << Mode.vSyncFreq.Numerator << "/" << Mode.vSyncFreq.Denominator
			  << "\n  hSync Frequency: " << Mode.hSyncFreq.Numerator << "/" << Mode.hSyncFreq.Denominator
			  << "\n  Pixel Rate: " << Mode.pixelRate
			  << "\n  Scan Line Ordering: " << Mode.scanLineOrdering;
	vddlog("d", logStream.str().c_str());
}

void CreateTargetMode(IDDCX_TARGET_MODE &Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	Mode.Size = sizeof(Mode);
	CreateTargetMode(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSyncNum, VSyncDen);
}

void CreateTargetMode2(IDDCX_TARGET_MODE2 &Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	stringstream logStream;
	logStream << "Creating IDDCX_TARGET_MODE2 with Width: " << Width
			  << ", Height: " << Height
			  << ", VSyncNum: " << VSyncNum
			  << ", VSyncDen: " << VSyncDen;
	vddlog("d", logStream.str().c_str());

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

	logStream.str("");
	logStream << "IDDCX_TARGET_MODE2 configured with Size: " << Mode.Size
			  << " and colour format " << WStringToString(ColourFormat);
	vddlog("d", logStream.str().c_str());

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

	stringstream logStream;
	logStream << "Creating target modes. Number of monitor modes: " << localModes.size();
	vddlog("d", logStream.str().c_str());

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	for (int i = 0; i < localModes.size(); i++)
	{
		CreateTargetMode(TargetModes[i], std::get<0>(localModes[i]), std::get<1>(localModes[i]), std::get<2>(localModes[i]), std::get<3>(localModes[i]));

		logStream.str("");
		logStream << "Created target mode " << i << ": Width = " << std::get<0>(localModes[i])
				  << ", Height = " << std::get<1>(localModes[i])
				  << ", VSync = " << std::get<2>(localModes[i]);
		vddlog("d", logStream.str().c_str());
	}

	pOutArgs->TargetModeBufferOutputCount = (UINT)TargetModes.size();

	logStream.str("");
	logStream << "Number of target modes to output: " << pOutArgs->TargetModeBufferOutputCount;
	vddlog("d", logStream.str().c_str());

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		logStream.str("");
		logStream << "Copying target modes to output buffer.";
		vddlog("d", logStream.str().c_str());
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	}
	else
	{
		logStream.str("");
		logStream << "Input buffer too small. Required: " << TargetModes.size()
				  << ", Provided: " << pInArgs->TargetModeBufferInputCount;
		vddlog("w", logStream.str().c_str());
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN *pInArgs)
{
	stringstream logStream;
	logStream << "Assigning swap chain:"
			  << "\n  hSwapChain: " << pInArgs->hSwapChain
			  << "\n  RenderAdapterLuid: " << pInArgs->RenderAdapterLuid.LowPart << "-" << pInArgs->RenderAdapterLuid.HighPart
			  << "\n  hNextSurfaceAvailable: " << pInArgs->hNextSurfaceAvailable;
	vddlog("d", logStream.str().c_str());
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	vddlog("d", "Swap chain assigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	stringstream logStream;
	logStream << "Unassigning swap chain for monitor object: " << MonitorObject;
	vddlog("d", logStream.str().c_str());
	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->UnassignSwapChain(MonitorObject);
	vddlog("d", "Swap chain unassigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo(
		IDDCX_ADAPTER AdapterObject,
		IDARG_IN_QUERYTARGET_INFO *pInArgs,
		IDARG_OUT_QUERYTARGET_INFO *pOutArgs)
{
	stringstream logStream;
	logStream << "Querying target info for adapter object: " << AdapterObject;
	vddlog("d", logStream.str().c_str());

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

	logStream.str("");
	logStream << "Target capabilities set to: " << pOutArgs->TargetCaps
			  << "\nDithering support colour format set to: " << WStringToString(ColourFormat);
	vddlog("d", logStream.str().c_str());

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata(
		IDDCX_MONITOR MonitorObject,
		const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *pInArgs)
{
	stringstream logStream;
	logStream << "Setting default HDR metadata for monitor object: " << MonitorObject;
	vddlog("d", logStream.str().c_str());

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
			logStream.str("");
			logStream << "Retrieved HDR settings - MaxNits: " << maxNits
			          << ", MinNits: " << minNits
			          << ", MaxFALL: " << maxFALL;
			vddlog("d", logStream.str().c_str());
		}
		else
		{
			vddlog("d", "Using default HDR luminance settings (monitor not found in settings map)");
		}
	}

	// Log the incoming metadata type
	if (pInArgs)
	{
		logStream.str("");
		logStream << "HDR Metadata Type: " << static_cast<int>(pInArgs->Type);
		vddlog("d", logStream.str().c_str());

		// Log current luminance settings being applied
		logStream.str("");
		logStream << "Applying HDR10 metadata - MaxMasteringLuminance: " << maxNits
				  << " nits, MinMasteringLuminance: " << (minNits * 10000.0f) << " (normalized)";
		vddlog("d", logStream.str().c_str());
	}

	auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	if (pContext && pContext->pContext)
	{
		pContext->pContext->UpdateMonitorHdrMetadata(MonitorObject, true, maxNits, minNits, maxFALL);
	}

	vddlog("d", "Default HDR metadata set successfully.");

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
	NTSTATUS
	VirtualDisplayDriverEvtIddCxParseMonitorDescription2(
		const IDARG_IN_PARSEMONITORDESCRIPTION2 *pInArgs,
		IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	// Take a local snapshot of monitorModes under lock to prevent data races
	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
	}

	stringstream logStream;
	logStream << "Parsing monitor description:"
			  << "\n  MonitorModeBufferInputCount: " << pInArgs->MonitorModeBufferInputCount
			  << "\n  pMonitorModes: " << (pInArgs->pMonitorModes ? "Valid" : "Null");
	vddlog("d", logStream.str().c_str());

	logStream.str("");
	logStream << "Monitor Modes:";
	for (const auto &mode : localModes)
	{
		logStream << "\n  Mode - Width: " << std::get<0>(mode)
				  << ", Height: " << std::get<1>(mode)
				  << ", RefreshRate: " << std::get<2>(mode);
	}
	vddlog("d", logStream.str().c_str());

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
		// Copy the known modes to the output buffer
		logStream.str(""); // Clear the stream
		logStream << "Writing monitor modes to output buffer:";
		for (DWORD ModeIndex = 0; ModeIndex < localModes.size(); ModeIndex++)
		{
			pInArgs->pMonitorModes[ModeIndex].Size = sizeof(IDDCX_MONITOR_MODE2);
			pInArgs->pMonitorModes[ModeIndex].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
			pInArgs->pMonitorModes[ModeIndex].MonitorVideoSignalInfo = s_KnownMonitorModes2[ModeIndex];

			if (ColourFormat == L"RGB")
			{
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr444")
			{
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr422")
			{
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr420")
			{
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
			}
			else
			{
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR; // Default to RGB
			}

			logStream << "\n  ModeIndex: " << ModeIndex
					  << "\n    Size: " << pInArgs->pMonitorModes[ModeIndex].Size
					  << "\n    Origin: " << pInArgs->pMonitorModes[ModeIndex].Origin
					  << "\n    Colour Format: " << WStringToString(ColourFormat);
		}

		vddlog("d", logStream.str().c_str());

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
	stringstream logStream;

	logStream << "Querying target modes:"
			  << "\n  MonitorObject Handle: " << static_cast<void *>(MonitorObject)
			  << "\n  TargetModeBufferInputCount: " << pInArgs->TargetModeBufferInputCount;
	vddlog("d", logStream.str().c_str());

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

	logStream.str(""); // Clear the stream
	logStream << "Creating target modes:";

	for (int i = 0; i < localModes.size(); i++)
	{
		CreateTargetMode2(TargetModes[i], std::get<0>(localModes[i]), std::get<1>(localModes[i]), std::get<2>(localModes[i]), std::get<3>(localModes[i]));
		logStream << "\n  TargetModeIndex: " << i
				  << "\n    Width: " << std::get<0>(localModes[i])
				  << "\n    Height: " << std::get<1>(localModes[i])
				  << "\n    RefreshRate: " << std::get<2>(localModes[i]);
	}
	vddlog("d", logStream.str().c_str());

	pOutArgs->TargetModeBufferOutputCount = (UINT)TargetModes.size();

	logStream.str("");
	logStream << "Output target modes count: " << pOutArgs->TargetModeBufferOutputCount;
	vddlog("d", logStream.str().c_str());

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);

		logStream.str("");
		logStream << "Target modes copied to output buffer:";
		for (int i = 0; i < TargetModes.size(); i++)
		{
			logStream << "\n  TargetModeIndex: " << i
					  << "\n    Size: " << TargetModes[i].Size
					  << "\n    ColourFormat: " << WStringToString(ColourFormat);
		}
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		vddlog("w", "Input buffer is too small for target modes.");
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

#pragma endregion
