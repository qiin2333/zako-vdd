#include "DriverSettings.h"

#include "..\Core\DriverState.h"
#include "..\Logging\Logger.h"

#include <atlbase.h>
#include <exception>
#include <map>
#include <shlwapi.h>
#include <string>
#include <utility>
#include <xmllite.h>

using namespace std;

namespace
{
const map<wstring, pair<wstring, wstring>> SettingsQueryMap = {
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

const pair<wstring, wstring> *FindSettingNames(const wstring &settingKey)
{
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end())
	{
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return nullptr;
	}

	return &it->second;
}

void LogQueries(const char *severity, const wstring &xmlName)
{
	if (xmlName.find(L"logging") != wstring::npos)
	{
		return;
	}

	int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), static_cast<int>(xmlName.size()), nullptr, 0, nullptr, nullptr);
	if (sizeNeeded <= 0)
	{
		return;
	}

	string strMessage(sizeNeeded, 0);
	WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), static_cast<int>(xmlName.size()), &strMessage[0], sizeNeeded, nullptr, nullptr);
	vddlog(severity, strMessage.c_str());
}

bool TryReadRegistryDword(const wstring &regName, DWORD &value)
{
	HKEY hKey = nullptr;
	DWORD bufferSize = sizeof(value);

	LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);
	if (result != ERROR_SUCCESS)
	{
		return false;
	}

	result = RegQueryValueExW(hKey, regName.c_str(), nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &bufferSize);
	RegCloseKey(hKey);
	return result == ERROR_SUCCESS;
}

bool TryReadRegistryString(const wstring &regName, wstring &value)
{
	HKEY hKey = nullptr;
	wchar_t buffer[MAX_PATH] = {};
	DWORD bufferSize = sizeof(buffer);

	LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);
	if (result != ERROR_SUCCESS)
	{
		return false;
	}

	result = RegQueryValueExW(hKey, regName.c_str(), nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &bufferSize);
	RegCloseKey(hKey);
	if (result != ERROR_SUCCESS)
	{
		return false;
	}

	value = buffer;
	return true;
}

bool TryReadXmlText(const wstring &xmlName, wstring &value)
{
	const wstring settingsname = confpath + L"\\vdd_settings.xml";

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr))
	{
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void **>(&pReader), nullptr);
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
	const wchar_t *pwszLocalName = nullptr;
	UINT cwchLocalName = 0;

	while (S_OK == pReader->Read(&nodeType))
	{
		if (nodeType != XmlNodeType_Element)
		{
			continue;
		}

		hr = pReader->GetLocalName(&pwszLocalName, &cwchLocalName);
		if (FAILED(hr) || pwszLocalName == nullptr)
		{
			return false;
		}

		if (wstring(pwszLocalName, cwchLocalName) != xmlName)
		{
			continue;
		}

		if (pReader->Read(&nodeType) != S_OK || nodeType != XmlNodeType_Text)
		{
			return false;
		}

		const wchar_t *pwszValue = nullptr;
		UINT cwchValue = 0;
		hr = pReader->GetValue(&pwszValue, &cwchValue);
		if (FAILED(hr) || pwszValue == nullptr)
		{
			return false;
		}

		value.assign(pwszValue, cwchValue);
		return true;
	}

	return false;
}
}

bool EnabledQuery(const wstring &settingKey)
{
	const auto *settingNames = FindSettingNames(settingKey);
	if (settingNames == nullptr)
	{
		return false;
	}

	const wstring &regName = settingNames->first;
	const wstring &xmlName = settingNames->second;

	DWORD dwValue = 0;
	if (TryReadRegistryDword(regName, dwValue))
	{
		if (dwValue == 1)
		{
			LogQueries("d", xmlName + L" - is enabled (value = 1).");
			return true;
		}
		if (dwValue == 0)
		{
			LogQueries("d", xmlName + L" - is disabled (value = 0).");
		}
	}
	else
	{
		LogQueries("d", xmlName + L" - Failed to retrieve value from registry. Attempting to read as string.");

		wstring registryValue;
		if (TryReadRegistryString(regName, registryValue))
		{
			if (registryValue == L"true" || registryValue == L"1")
			{
				LogQueries("d", xmlName + L" - is enabled (string value).");
				return true;
			}
			if (registryValue == L"false" || registryValue == L"0")
			{
				LogQueries("d", xmlName + L" - is disabled (string value).");
			}
		}
		else
		{
			LogQueries("d", xmlName + L" - Failed to retrieve string value from registry.");
		}
	}

	wstring xmlValue;
	if (!TryReadXmlText(xmlName, xmlValue))
	{
		return false;
	}

	bool xmlLoggingValue = (xmlValue == L"true");
	LogQueries("i", xmlName + (xmlLoggingValue ? L" is enabled." : L" is disabled."));
	return xmlLoggingValue;
}

int GetIntegerSetting(const wstring &settingKey)
{
	const auto *settingNames = FindSettingNames(settingKey);
	if (settingNames == nullptr)
	{
		return -1;
	}

	const wstring &regName = settingNames->first;
	const wstring &xmlName = settingNames->second;

	DWORD dwValue = 0;
	if (TryReadRegistryDword(regName, dwValue))
	{
		LogQueries("d", xmlName + L" - Retrieved integer value: " + to_wstring(dwValue));
		return static_cast<int>(dwValue);
	}

	LogQueries("d", xmlName + L" - Failed to retrieve integer value from registry. Attempting to read as string.");
	wstring registryValue;
	if (TryReadRegistryString(regName, registryValue))
	{
		try
		{
			int logValue = stoi(registryValue);
			LogQueries("d", xmlName + L" - Retrieved string value: " + to_wstring(logValue));
			return logValue;
		}
		catch (const exception &)
		{
			LogQueries("d", xmlName + L" - Failed to convert registry string value to integer.");
		}
	}

	wstring xmlValue;
	if (!TryReadXmlText(xmlName, xmlValue))
	{
		return -1;
	}

	try
	{
		int xmlLoggingValue = stoi(xmlValue);
		LogQueries("i", xmlName + L" - Retrieved from XML: " + to_wstring(xmlLoggingValue));
		return xmlLoggingValue;
	}
	catch (const exception &)
	{
		LogQueries("d", xmlName + L" - Failed to convert XML string value to integer.");
		return -1;
	}
}

wstring GetStringSetting(const wstring &settingKey)
{
	const auto *settingNames = FindSettingNames(settingKey);
	if (settingNames == nullptr)
	{
		return L"";
	}

	const wstring &regName = settingNames->first;
	const wstring &xmlName = settingNames->second;

	wstring registryValue;
	if (TryReadRegistryString(regName, registryValue))
	{
		LogQueries("d", xmlName + L" - Retrieved string value from registry: " + registryValue);
		return registryValue;
	}

	LogQueries("d", xmlName + L" - Failed to retrieve string value from registry. Attempting to read as XML.");

	wstring xmlValue;
	if (!TryReadXmlText(xmlName, xmlValue))
	{
		return L"";
	}

	LogQueries("i", xmlName + L" - Retrieved from XML: " + xmlValue);
	return xmlValue;
}
