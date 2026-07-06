#include "DriverSettings.h"

#include "..\Core\DriverState.h"
#include "..\Edid\Edid.h"
#include "..\Logging\Logger.h"
#include "..\Util\RefreshRate.h"

#include <atlbase.h>
#include <exception>
#include <fstream>
#include <mutex>
#include <set>
#include <shlwapi.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <xmllite.h>

using namespace std;

namespace
{
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
}

bool initpath()
{
	// Prefer an explicit config path provided by the environment.
	wchar_t envPath[MAX_PATH] = {0};
	DWORD envLen = GetEnvironmentVariableW(L"ZAKOVDDPATH", envPath, MAX_PATH);
	if (envLen > 0 && envLen < MAX_PATH)
	{
		confpath = envPath;
		return true;
	}

	// Fall back to the installer-written registry path.
	HKEY hKey;
	wchar_t szPath[MAX_PATH] = {0};
	DWORD dwBufferSize = sizeof(szPath);
	LONG lResult;
	lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);
	if (lResult != ERROR_SUCCESS)
	{
		// Registry path is unavailable.
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

// Keep the in-memory runtime knobs in sync with registry/XML settings.
// DriverEntry calls this once at process startup, while RELOAD_DRIVER and
// toggle commands call it before recreating monitors so changes such as
// HARDWARECURSOR true affect the next swapchain without requiring an adapter
// disable/enable cycle.
void LoadDriverSettings()
{
	initpath();

	customEdid = EnabledQuery(L"CustomEdidEnabled");
	preventManufacturerSpoof = EnabledQuery(L"PreventMonitorSpoof");
	edidCeaOverride = EnabledQuery(L"EdidCeaOverride");

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
	legacyNamedFrameChannel = EnabledQuery(L"LegacyNamedFrameChannel");

	// Cursor
	hardwareCursor = EnabledQuery(L"HardwareCursorEnabled");
	alphaCursorSupport = EnabledQuery(L"AlphaCursorSupport");
	CursorMaxX = GetIntegerSetting(L"CursorMaxX");
	CursorMaxY = GetIntegerSetting(L"CursorMaxY");

	int xorCursorSupportLevelInt = GetIntegerSetting(L"XorCursorSupportLevel");
	string xorCursorSupportLevelName;

	if (xorCursorSupportLevelInt < 0 || xorCursorSupportLevelInt > 3)
	{
		VDD_LOG_WARNING("Selected Xor Level unsupported, defaulting to IDDCX_XOR_CURSOR_SUPPORT_FULL");
		XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;
	}
	else
	{
		XorCursorSupportLevel = static_cast<IDDCX_XOR_CURSOR_SUPPORT>(xorCursorSupportLevelInt);
	}

	xorCursorSupportLevelName = XorCursorSupportLevelToString(XorCursorSupportLevel);

	VDD_LOG_INFO(("Selected Xor Cursor Support Level: " + xorCursorSupportLevelName).c_str());
	VDD_LOG_INFO((string("Hardware cursor runtime setting: ") + (hardwareCursor ? "enabled" : "disabled")).c_str());
	VDD_LOG_INFO((string("Legacy named frame-channel export: ") + (legacyNamedFrameChannel ? "enabled" : "disabled")).c_str());
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
			VDD_LOG_ERROR("Loading Settings: Failed to create file stream.");
			return;
		}
		hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&pReader, NULL);
		if (FAILED(hr))
		{
			VDD_LOG_ERROR("Loading Settings: Failed to create XmlReader.");
			return;
		}
		hr = pReader->SetInput(pStream);
		if (FAILED(hr))
		{
			VDD_LOG_ERROR("Loading Settings: Failed to set input stream.");
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
					} catch (const exception &) {
						monitorcount = 1;
						VDD_LOG_WARNING("Failed to parse monitor count, defaulting to 1");
					}
					if (monitorcount == 0)
					{
						monitorcount = 1;
						VDD_LOG_INFO("Loading singular monitor (Monitor Count is not valid)");
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
					} catch (const exception &) {
						VDD_LOG_WARNING("Failed to parse resolution width/height, skipping");
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
						VDD_LOG_DEBUG_STREAM("Added: " << w << "x" << h << " @ " << vsync_num << "/" << vsync_den << "Hz");
					} catch (const exception &) {
						VDD_LOG_WARNING("Failed to parse refresh rate or resolution, skipping entry");
					}
				}
				else if (currentElement == L"g_refresh_rate")
				{
					try {
						globalRefreshRates.push_back(stof(wstring(pwszValue, cwchValue)));
					} catch (const exception &) {
						VDD_LOG_WARNING("Failed to parse global refresh rate, skipping");
					}
				}
				break;
			}
		}

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

		{
			lock_guard<mutex> dataLock(g_DataMutex);
			numVirtualDisplays = monitorcount;
			gpuname = gpuFriendlyName;
			monitorModes = res;
		}
		VDD_LOG_INFO("Using vdd_settings.xml");
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
			} catch (const exception &) {
				numVirtualDisplays = 1;
				VDD_LOG_WARNING("Failed to parse display count from option.txt, defaulting to 1");
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
					} catch (const exception &) {
						VDD_LOG_WARNING("Failed to parse option.txt line, skipping");
					}
				}
			}

			VDD_LOG_INFO("Using option.txt");
			{
				lock_guard<mutex> dataLock(g_DataMutex);
				monitorModes = res;
			}
			for (const auto &mode : res)
			{
				int width, height, vsync_num, vsync_den;
				tie(width, height, vsync_num, vsync_den) = mode;
				VDD_LOG_DEBUG_STREAM("Resolution: " << width << "x" << height << " @ " << vsync_num << "/" << vsync_den << "Hz");
			}
			return;
		}
		else
		{
			VDD_LOG_WARNING("option.txt is empty or the first line is invalid. Enabling Fallback");
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

	VDD_LOG_INFO("Loading Fallback - no settings found");

	for (const auto &mode : fallbackRes)
	{
		int width, height;
		float refreshRate;
		tie(width, height, refreshRate) = mode;

		int vsync_num, vsync_den;
		float_to_vsync(refreshRate, vsync_num, vsync_den);

		res.push_back(make_tuple(width, height, vsync_num, vsync_den));

		VDD_LOG_DEBUG_STREAM("Resolution: " << width << "x" << height << " @ " << vsync_num << "/" << vsync_den << "Hz");
	}

	{
		lock_guard<mutex> dataLock(g_DataMutex);
		monitorModes = res;
	}
	return;
}
