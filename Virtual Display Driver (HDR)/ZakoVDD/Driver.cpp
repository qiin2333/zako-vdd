/*++

Copyright (c) Microsoft Corporation

Abstract:

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"
//#include "Driver.tmh"
#include<fstream>
#include<sstream>
#include<string>
#include<tuple>
#include<vector>
#include <AdapterOption.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <atlbase.h>
#include <iostream>
#include <cstdlib>
#include <windows.h>
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





#define PIPE_NAME L"\\\\.\\pipe\\ZakoVDDPipe"

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "shlwapi.lib")

HANDLE hPipeThread = NULL;
bool g_Running = true;
mutex g_Mutex;
HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
WDFDEVICE g_GlobalDevice = nullptr;

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

void vddlog(const char* type, const char* message);

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD VirtualDisplayDriverDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY VirtualDisplayDriverDeviceD0Entry;

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
vector< DISPLAYCONFIG_VIDEO_SIGNAL_INFO> s_KnownMonitorModes2;
UINT numVirtualDisplays = 1;
wstring gpuname = L"";
wstring confpath = []() {
    wchar_t sysDrive[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"SystemDrive", sysDrive, MAX_PATH);
    if (len == 0 || len > MAX_PATH) {
        return wstring(L"C:\\VirtualDisplayDriver");
    }
    return wstring(sysDrive) + L"\\VirtualDisplayDriver";
}();
bool logsEnabled = false;
bool debugLogs = false;
bool HDRPlus = false;
bool SDR10 = false;
bool customEdid = false;
bool hardwareCursor = false;
bool preventManufacturerSpoof = false;
bool edidCeaOverride = false;
bool sendLogsThroughPipe = true;

//Mouse settings
bool alphaCursorSupport = true;
int CursorMaxX = 128;
int CursorMaxY = 128;
IDDCX_XOR_CURSOR_SUPPORT XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;


//Rest
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
	//Cursor Begin
	{L"HardwareCursorEnabled", {L"HARDWARECURSOR", L"HardwareCursor"}},
	{L"AlphaCursorSupport", {L"ALPHACURSORSUPPORT", L"AlphaCursorSupport"}},
	{L"CursorMaxX", {L"CURSORMAXX", L"CursorMaxX"}},
	{L"CursorMaxY", {L"CURSORMAXY", L"CursorMaxY"}},
	{L"XorCursorSupportLevel", {L"XORCURSORSUPPORTLEVEL", L"XorCursorSupportLevel"}},
	///Cursor End
	//Colour Begin
	{L"HDRPlusEnabled", {L"HDRPLUS", L"HDRPlus"}},
	{L"SDR10Enabled", {L"SDR10BIT", L"SDR10bit"}},
	{L"ColourFormat", {L"COLOURFORMAT", L"ColourFormat"}},
	//Colour End
};

const char* XorCursorSupportLevelToString(IDDCX_XOR_CURSOR_SUPPORT level) {
	switch (level) {
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


vector<unsigned char> Microsoft::IndirectDisp::IndirectDeviceContext::s_KnownMonitorEdid; //Changed to support static vector

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};
void LogQueries(const char* severity, const std::wstring& xmlName) {
	if (xmlName.find(L"logging") == std::wstring::npos) { 
		int size_needed = WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), (int)xmlName.size(), NULL, 0, NULL, NULL);
		if (size_needed > 0) {
			std::string strMessage(size_needed, 0);
			WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), (int)xmlName.size(), &strMessage[0], size_needed, NULL, NULL);
			vddlog(severity, strMessage.c_str());
		}
	}
}

string WStringToString(const wstring& wstr) { //basically just a function for converting strings since codecvt is depricated in c++ 17
	if (wstr.empty()) return "";

	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
	string str(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
	return str;
}

bool EnabledQuery(const std::wstring& settingKey) {
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end()) {
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

	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)&dwValue, &dwBufferSize);
		if (lResult == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			if (dwValue == 1) {
				LogQueries("d", xmlName + L" - is enabled (value = 1).");
				return true;
			}
			else if (dwValue == 0) {
				goto check_xml;
			}
		}
		else {
			LogQueries("d", xmlName + L" - Failed to retrieve value from registry. Attempting to read as string.");
			wchar_t path[MAX_PATH];
			dwBufferSize = sizeof(path);
			lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)path, &dwBufferSize);
			if (lResult == ERROR_SUCCESS) {
				std::wstring logValue(path);
				RegCloseKey(hKey);
				if (logValue == L"true" || logValue == L"1") {
					LogQueries("d", xmlName + L" - is enabled (string value).");
					return true;
				}
				else if (logValue == L"false" || logValue == L"0") {
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
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return false;
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	bool xmlLoggingValue = false;

	while (S_OK == pReader->Read(&nodeType)) {
		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, xmlName.c_str()) == 0) {
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text) {
					const wchar_t* pwszValue;
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

int GetIntegerSetting(const std::wstring& settingKey) {
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end()) {
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return -1;
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwValue;
	DWORD dwBufferSize = sizeof(dwValue);
	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &hKey);

	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)&dwValue, &dwBufferSize);
		if (lResult == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			LogQueries("d", xmlName + L" - Retrieved integer value: " + std::to_wstring(dwValue));
			return static_cast<int>(dwValue);
		}
		else {
			LogQueries("d", xmlName + L" - Failed to retrieve integer value from registry. Attempting to read as string.");
			wchar_t path[MAX_PATH];
			dwBufferSize = sizeof(path);
			lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)path, &dwBufferSize);
			RegCloseKey(hKey);
			if (lResult == ERROR_SUCCESS) {
				try {
					int logValue = std::stoi(path);
					LogQueries("d", xmlName + L" - Retrieved string value: " + std::to_wstring(logValue));
					return logValue;
				}
				catch (const std::exception&) {
					LogQueries("d", xmlName + L" - Failed to convert registry string value to integer.");
				}
			}
		}
	}

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return -1;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return -1;
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return -1;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	int xmlLoggingValue = -1;

	while (S_OK == pReader->Read(&nodeType)) {
		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, xmlName.c_str()) == 0) {
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text) {
					const wchar_t* pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					try {
						xmlLoggingValue = std::stoi(pwszValue);
						LogQueries("i", xmlName + L" - Retrieved from XML: " + std::to_wstring(xmlLoggingValue));
					}
					catch (const std::exception&) {
						LogQueries("d", xmlName + L" - Failed to convert XML string value to integer.");
					}
					break;
				}
			}
		}
	}

	return xmlLoggingValue;
}

std::wstring GetStringSetting(const std::wstring& settingKey) {
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end()) {
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return L""; 
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwBufferSize = MAX_PATH;
	wchar_t buffer[MAX_PATH];

	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)buffer, &dwBufferSize);
		RegCloseKey(hKey);

		if (lResult == ERROR_SUCCESS) {
			LogQueries("d", xmlName + L" - Retrieved string value from registry: " + buffer);
			return std::wstring(buffer);  
		}
		else {
			LogQueries("d", xmlName + L" - Failed to retrieve string value from registry. Attempting to read as XML.");
		}
	}

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return L""; 
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return L""; 
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return L"";  
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	std::wstring xmlLoggingValue = L"";  

	while (S_OK == pReader->Read(&nodeType)) {
		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, xmlName.c_str()) == 0) {
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text) {
					const wchar_t* pwszValue;
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


int gcd(int a, int b) {
	while (b != 0) {
		int temp = b;
		b = a % b;
		a = temp;
	}
	return a;
}

void float_to_vsync(float refresh_rate, int& num, int& den) {
	den = 10000;

	num = static_cast<int>(round(refresh_rate * den));

	int divisor = gcd(num, den);
	num /= divisor;
	den /= divisor;
}

void SendToPipe(const std::string& logMessage) {
	if (g_pipeHandle != INVALID_HANDLE_VALUE) {
		DWORD bytesWritten;
		DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
		WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
	}
}

void vddlog(const char* type, const char* message) {
	FILE* logFile;
	wstring logsDir = confpath + L"\\Logs";

	auto now = chrono::system_clock::now();
	auto in_time_t = chrono::system_clock::to_time_t(now);
	tm tm_buf;
	localtime_s(&tm_buf, &in_time_t);
	wchar_t date_str[11]; 
	wcsftime(date_str, sizeof(date_str) / sizeof(wchar_t), L"%Y-%m-%d", &tm_buf);

	wstring logPath = logsDir + L"\\log_" + date_str + L".txt";

	if (logsEnabled) {
		if (!CreateDirectoryW(logsDir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
			//Just any errors here
		}
	}
	else {
		WIN32_FIND_DATAW findFileData;
		HANDLE hFind = FindFirstFileW((logsDir + L"\\*").c_str(), &findFileData);

		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				const wstring fileOrDir = findFileData.cFileName;
				if (fileOrDir != L"." && fileOrDir != L"..") {
					wstring filePath = logsDir + L"\\" + fileOrDir;
					if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
						DeleteFileW(filePath.c_str());
					}
				}
			} while (FindNextFileW(hFind, &findFileData) != 0);
			FindClose(hFind);
		}

		RemoveDirectoryW(logsDir.c_str());
		return;  
	}

	string narrow_logPath = WStringToString(logPath);
	const char* mode = "a";
	errno_t err = fopen_s(&logFile, narrow_logPath.c_str(), mode);
	if (err == 0 && logFile != nullptr) {
		stringstream ss;
		ss << put_time(&tm_buf, "%Y-%m-%d %X");

		string logType;
		switch (type[0]) {
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

		bool shouldLog = true;
		if (logType == "DEBUG" && !debugLogs) {
			shouldLog = false;
		}
		if (shouldLog) {
			fprintf(logFile, "[%s] [%s] %s\n", ss.str().c_str(), logType.c_str(), message);
		}

		fclose(logFile);

		if (sendLogsThroughPipe && g_pipeHandle != INVALID_HANDLE_VALUE) {
			string logMessage = ss.str() + " [" + logType + "] " + message + "\n";
			DWORD bytesWritten;
			DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
			WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
		}
	}
}



void LogIddCxVersion() {
	IDARG_OUT_GETVERSION outArgs;
	NTSTATUS status = IddCxGetVersion(&outArgs);

	if (NT_SUCCESS(status)) {
		char versionStr[16];
		sprintf_s(versionStr, "0x%lx", outArgs.IddCxVersion);
		string logMessage = "IDDCX Version: " + string(versionStr);
		vddlog("i", logMessage.c_str());
	}
	else {
		vddlog("i", "Failed to get IDDCX version");
	}
	vddlog("d", "Testing Debug Log");
}

void InitializeD3DDeviceAndLogGPU() {
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

	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to create D3D11 device");
		return;
	}

	ComPtr<IDXGIDevice> dxgiDevice;
	hr = d3dDevice.As(&dxgiDevice);
	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to get DXGI device");
		return;
	}

	ComPtr<IDXGIAdapter> dxgiAdapter;
	hr = dxgiDevice->GetAdapter(&dxgiAdapter);
	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to get DXGI adapter");
		return;
	}

	DXGI_ADAPTER_DESC desc;
	hr = dxgiAdapter->GetDesc(&desc);
	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to get GPU description");
		return;
	}

	d3dDevice.Reset();
	d3dContext.Reset();

	wstring wdesc(desc.Description);
	string utf8_desc;
	try {
		utf8_desc = WStringToString(wdesc);
	}
	catch (const exception& e) {
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


bool UpdateXmlToggleSetting(bool toggle, const wchar_t* variable) {
	const wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	const wchar_t* pwszValue;
	bool variableElementFound = false;

	while (S_OK == pReader->Read(&nodeType)) {
		switch (nodeType) {
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
			if (variableElementFound) {
				pWriter->WriteString(toggle ? L"true" : L"false");
				variableElementFound = false;
			}
			else {
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

		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, variable) == 0) {
				variableElementFound = true;
			}
		}
	}

	if (variableElementFound) {
		pWriter->WriteStartElement(nullptr, variable, nullptr);
		pWriter->WriteString(toggle ? L"true" : L"false");
		pWriter->WriteEndElement();
	}

	hr = pWriter->WriteEndDocument();
	if (FAILED(hr)) {
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		return false;
	}
	return true;
}


bool UpdateXmlGpuSetting(const wchar_t* gpuName) {
	const std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	std::wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	const wchar_t* pwszValue;
	bool gpuElementFound = false;

	while (S_OK == pReader->Read(&nodeType)) {
		switch (nodeType) {
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
			if (gpuElementFound) {
				pWriter->WriteString(gpuName); 
				gpuElementFound = false;
			}
			else {
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

		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, L"gpu") == 0) {
				gpuElementFound = true;
			}
		}
	}
	hr = pWriter->WriteEndDocument();
	if (FAILED(hr)) {
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		return false;
	}
	return true;
}

bool UpdateXmlDisplayCountSetting(int displayCount) {
	const std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	std::wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	const wchar_t* pwszValue;
	bool displayCountElementFound = false;

	while (S_OK == pReader->Read(&nodeType)) {
		switch (nodeType) {
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
			if (displayCountElementFound) {
				pWriter->WriteString(std::to_wstring(displayCount).c_str());
				displayCountElementFound = false; 
			}
			else {
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

		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, L"count") == 0) {
				displayCountElementFound = true; 
			}
		}
	}

	hr = pWriter->WriteEndDocument();
	if (FAILED(hr)) {
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		return false;
	}
	return true;
}


LUID getSetAdapterLuid() {
	AdapterOption& adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter) {
		vddlog("e","No Gpu Found/Selected");
	}

	return adapterOption.adapterLuid;
}


void GetGpuInfo()
{
	AdapterOption& adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter) {
		vddlog("e", "No GPU found or set.");
		return;
	}

	try {
		string utf8_desc = WStringToString(adapterOption.target_name);
		LUID luid = getSetAdapterLuid();
		string logtext = "ASSIGNED GPU: " + utf8_desc +
			" (LUID: " + std::to_string(luid.LowPart) + "-" + std::to_string(luid.HighPart) + ")";
		vddlog("i", logtext.c_str());
	}
	catch (const exception& e) {
		vddlog("e", ("Error: " + string(e.what())).c_str());
	}
}

void logAvailableGPUs() {
	vector<GPUInfo> gpus;
	ComPtr<IDXGIFactory1> factory;
	if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return;
	}
	for (UINT i = 0;; i++) {
		ComPtr<IDXGIAdapter> adapter;
		if (!SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
			break;
		}
		DXGI_ADAPTER_DESC desc;
		if (!SUCCEEDED(adapter->GetDesc(&desc))) {
			continue;
		}
		GPUInfo info{ desc.Description, adapter, desc };
		gpus.push_back(info);
	}
	for (const auto& gpu : gpus) {
		wstring logMessage = L"GPU Name: ";
		logMessage += gpu.desc.Description;
		wstring memorySize = L" Memory: ";
		memorySize += std::to_wstring(gpu.desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB";
		wstring logText = logMessage + memorySize;
		int bufferSize = WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (bufferSize > 0) {
			std::string logTextA(bufferSize - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, &logTextA[0], bufferSize, nullptr, nullptr);
			vddlog("c", logTextA.c_str());
		}
	}
}

void ReloadDriver(HANDLE hPipe) {
	UNREFERENCED_PARAMETER(hPipe);
	
	vddlog("i", "Starting driver reload process");
	
	if (g_GlobalDevice != nullptr) {
		lock_guard<mutex> lock(g_Mutex);
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
		if (pContext && pContext->pContext) {
			try {
				// Step 1: Save current numVirtualDisplays before configuration reload
				UINT oldNumVirtualDisplays = numVirtualDisplays;
				vddlog("d", ("Saving current monitor count for cleanup: " + std::to_string(oldNumVirtualDisplays)).c_str());
				
				// Step 2: Clean up existing monitors first
				vddlog("d", "Cleaning up existing monitors before reload");
				
				// Stop any active SwapChain processing
				if (pContext->pContext->HasActiveSwapChain()) {
					vddlog("d", "Stopping active SwapChain processing before reload");
					pContext->pContext->UnassignSwapChain();
					
					// Additional wait to ensure cleanup completes
					Sleep(100);
				}
				
				// Destroy existing monitors using the OLD monitor count
				for (unsigned int i = 0; i < oldNumVirtualDisplays; i++) {
					if (pContext->pContext->HasActiveMonitor()) {
						vddlog("d", ("Destroying existing monitor " + std::to_string(i) + " before reload").c_str());
						pContext->pContext->DestroyMonitor(i);
						
						// Wait between monitor destructions
						if (i < oldNumVirtualDisplays - 1) {
							Sleep(50);
						}
					}
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
				
				vddlog("i", ("Driver reload completed successfully. Monitor count changed from " 
					+ std::to_string(oldNumVirtualDisplays) + " to " + std::to_string(numVirtualDisplays)).c_str());
			}
			catch (const std::exception& e) {
				stringstream errorStream;
				errorStream << "Exception during driver reload: " << e.what();
				vddlog("e", errorStream.str().c_str());
				
				// Try to continue with initialization anyway
				try {
					pContext->pContext->InitAdapter();
					vddlog("w", "Adapter reinitialized after exception");
				}
				catch (...) {
					vddlog("e", "Failed to reinitialize adapter after exception");
				}
			}
			catch (...) {
				vddlog("e", "Unknown exception during driver reload");
				
				// Try to continue with initialization anyway
				try {
					pContext->pContext->InitAdapter();
					vddlog("w", "Adapter reinitialized after unknown exception");
				}
				catch (...) {
					vddlog("e", "Failed to reinitialize adapter after unknown exception");
				}
			}
		} else {
			vddlog("e", "Invalid device context for driver reload");
		}
	} else {
		vddlog("e", "Global device not available for reload");
	}
}

void toggleSettingImpl(HANDLE hPipe, wchar_t* param, const wchar_t* settingName, const char* enableMsg, const char* disableMsg) {
    if (wcsncmp(param, L"true", 4) == 0) {
        UpdateXmlToggleSetting(true, settingName);
        vddlog("c", enableMsg);
        ReloadDriver(hPipe);
    } else if (wcsncmp(param, L"false", 5) == 0) {
        UpdateXmlToggleSetting(false, settingName);
        vddlog("c", disableMsg);
        ReloadDriver(hPipe);
    }
}

void HandleClient(HANDLE hPipe) {
    g_pipeHandle = hPipe;
    vddlog("p", "Client Handling Enabled");
    wchar_t buffer[128];
    DWORD bytesRead;
    BOOL result = ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, NULL);
    if (result && bytesRead != 0) {
        buffer[bytesRead / sizeof(wchar_t)] = L'\0';
        wstring bufferwstr(buffer);
        string bufferstr = WStringToString(bufferwstr);
        vddlog("p", bufferstr.c_str());

        struct Command {
            const wchar_t* name;
            size_t length;
            void (*action)(HANDLE, wchar_t*);
        };

        auto handleReloadDriver = [](HANDLE hPipe, wchar_t*) {
            vddlog("c", "Reloading the driver");
            ReloadDriver(hPipe);
        };

        auto handleLogDebug = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"debuglogging", "Pipe debugging enabled", "Debugging disabled");
        };

        auto handleLogging = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"logging", "Logging Enabled", "Logging disabled");
        };

        auto handleHDRPlus = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"HDRPlus", "HDR+ Enabled", "HDR+ Disabled");
        };

        auto handleSDR10 = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"SDR10bit", "SDR 10 Bit Enabled", "SDR 10 Bit Disabled");
        };

        auto handleCustomEdid = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"CustomEdid", "Custom Edid Enabled", "Custom Edid Disabled");
        };

        auto handlePreventSpoof = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"PreventSpoof", "Prevent Spoof Enabled", "Prevent Spoof Disabled");
        };

        auto handleCeaOverride = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"EdidCeaOverride", "Cea override Enabled", "Cea override Disabled");
        };

        auto handleHardwareCursor = [](HANDLE hPipe, wchar_t* param) {
            toggleSettingImpl(hPipe, param, L"HardwareCursor", "Hardware Cursor Enabled", "Hardware Cursor Disabled");
        };

        auto handleD3DDeviceGPU = [](HANDLE, wchar_t*) {
            vddlog("c", "Retrieving D3D GPU (This information may be inaccurate without reloading the driver first)");
            InitializeD3DDeviceAndLogGPU();
            vddlog("c", "Retrieved D3D GPU");
        };

        auto handleIddCxVersion = [](HANDLE, wchar_t*) {
            vddlog("c", "Logging iddcx version");
            LogIddCxVersion();
        };

        auto handleGetAssignedGPU = [](HANDLE, wchar_t*) {
            vddlog("c", "Retrieving Assigned GPU");
            GetGpuInfo();
            vddlog("c", "Retrieved Assigned GPU");
        };

        auto handleGetAllGPUs = [](HANDLE, wchar_t*) {
            vddlog("c", "Logging all GPUs");
            vddlog("i", "Any GPUs which show twice but you only have one, will most likely be the GPU the driver is attached to");
            logAvailableGPUs();
            vddlog("c", "Logged all GPUs");
        };

        auto handleSetGPU = [](HANDLE hPipe, wchar_t* param) {
            std::wstring gpuName = param;
            gpuName = gpuName.substr(1, gpuName.size() - 2);

            std::string gpuNameNarrow = WStringToString(gpuName);

            vddlog("c", ("Setting GPU to: " + gpuNameNarrow).c_str());
            if (UpdateXmlGpuSetting(gpuName.c_str())) {
                vddlog("c", "Gpu Changed, Restarting Driver");
            } else {
                vddlog("e", "Failed to update GPU setting in XML. Restarting anyway");
            }
            ReloadDriver(hPipe);
        };

        auto handleSetDisplayCount = [](HANDLE hPipe, wchar_t* param) {
            vddlog("i", "Setting Display Count");

            int newDisplayCount = 1;
            swscanf_s(param, L"%d", &newDisplayCount);

            std::wstring displayLog = L"Setting display count  to " + std::to_wstring(newDisplayCount);
            vddlog("c", WStringToString(displayLog).c_str());

            if (UpdateXmlDisplayCountSetting(newDisplayCount)) {
                vddlog("c", "Display Count Changed, Restarting Driver");
            } else {
                vddlog("e", "Failed to update display count setting in XML. Restarting anyway");
            }
            ReloadDriver(hPipe);
        };

        auto handleGetSettings = [](HANDLE hPipe, wchar_t*) {
            bool debugEnabled = EnabledQuery(L"DebugLoggingEnabled");
            bool loggingEnabled = EnabledQuery(L"LoggingEnabled");

            wstring settingsResponse = L"SETTINGS ";
            settingsResponse += debugEnabled ? L"DEBUG=true " : L"DEBUG=false ";
            settingsResponse += loggingEnabled ? L"LOG=true" : L"LOG=false";

            DWORD bytesWritten;
            DWORD bytesToWrite = static_cast<DWORD>((settingsResponse.length() + 1) * sizeof(wchar_t));
            WriteFile(hPipe, settingsResponse.c_str(), bytesToWrite, &bytesWritten, NULL);
        };

        auto handlePing = [](HANDLE, wchar_t*) {
            SendToPipe("PONG");
            vddlog("p", "Heartbeat Ping");
        };

        auto handleCreateMonitor = [](HANDLE, wchar_t*) {
            if (g_GlobalDevice != nullptr) {
                lock_guard<mutex> lock(g_Mutex);
                auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
                if (pContext && pContext->pContext) {
                    vddlog("i", "Starting monitor creation");

                    if (numVirtualDisplays == 0) {
                        vddlog("e", "Invalid display count: 0");
                        return;
                    }

                    for (unsigned int i = 0; i < numVirtualDisplays; i++) {
                        pContext->pContext->CreateMonitor(i);
                    }
                } else {
                    vddlog("e", "Failed to get device context for monitor creation");
                    if (!pContext) {
                        vddlog("e", "Context wrapper is null");
                    } else if (!pContext->pContext) {
                        vddlog("e", "Device context is null");
                    }
                }
            } else {
                vddlog("e", "Global device not initialized");
            }
        };

        auto handleDestroyMonitor = [](HANDLE, wchar_t*) {
            if (g_GlobalDevice != nullptr) {
                lock_guard<mutex> lock(g_Mutex);
                auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
                if (pContext && pContext->pContext) {
                    vddlog("i", "Starting monitor destruction process");

                    vddlog("d", "Preparing system for monitor destruction");
                    Sleep(50);

                    try {
                        for (unsigned int i = 0; i < numVirtualDisplays; i++) {
                            vddlog("d", ("Destroying monitor " + std::to_string(i) + " of " + std::to_string(numVirtualDisplays)).c_str());
                            pContext->pContext->DestroyMonitor(i);

                            if (numVirtualDisplays > 1 && i < numVirtualDisplays - 1) {
                                vddlog("d", "Waiting between monitor destructions");
                                Sleep(100);
                            }
                        }

                        vddlog("d", "Allowing system to stabilize after monitor destruction");
                        Sleep(100);

                        vddlog("i", "All monitors destroyed successfully");
                    } catch (const std::exception& e) {
                        stringstream errorStream;
                        errorStream << "Exception during monitor destruction: " << e.what();
                        vddlog("e", errorStream.str().c_str());

                        Sleep(200);
                    } catch (...) {
                        vddlog("e", "Unknown exception during monitor destruction");

                        Sleep(200);
                    }
                } else {
                    vddlog("e", "Failed to get device context for monitor destruction");
                }
            } else {
                vddlog("e", "Global device not initialized for monitor destruction");
            }
        };

        auto handleUnknownCommand = [](HANDLE, wchar_t* buffer) {
            vddlog("e", "Unknown command");

            std::string narrowString = WStringToString(buffer);
            vddlog("e", narrowString.c_str());
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
            {L"HARDWARECURSOR", 14, handleHardwareCursor},
            {L"D3DDEVICEGPU", 12, handleD3DDeviceGPU},
            {L"IDDCXVERSION", 12, handleIddCxVersion},
            {L"GETASSIGNEDGPU", 14, handleGetAssignedGPU},
            {L"GETALLGPUS", 10, handleGetAllGPUs},
            {L"SETGPU", 6, handleSetGPU},
            {L"SETDISPLAYCOUNT", 15, handleSetDisplayCount},
            {L"GETSETTINGS", 11, handleGetSettings},
            {L"PING", 4, handlePing},
            {L"CREATEMONITOR", 13, handleCreateMonitor},
            {L"DESTROYMONITOR", 14, handleDestroyMonitor},
            {nullptr, 0, handleUnknownCommand}
        };

        for (const auto& cmd : commands) {
            if (cmd.name && wcsncmp(buffer, cmd.name, cmd.length) == 0) {
                cmd.action(hPipe, buffer + cmd.length + 1);
                break;
            }
        }
    }
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    g_pipeHandle = INVALID_HANDLE_VALUE;
}


DWORD WINAPI NamedPipeServer(LPVOID lpParam) {
	UNREFERENCED_PARAMETER(lpParam);

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = FALSE;
	const wchar_t* sddl = L"D:(A;;GA;;;WD)";
	vddlog("d", "Starting pipe with parameters: D:(A;;GA;;;WD)");
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
		sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
		DWORD ErrorCode = GetLastError();
		const char* errorMessage = to_string(ErrorCode).c_str();
		vddlog("e", errorMessage);
		return 1;
	}
	HANDLE hPipe;
	while (g_Running) {
		hPipe = CreateNamedPipeW(
			PIPE_NAME,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES,
			512, 512,
			0,
			&sa);

		if (hPipe == INVALID_HANDLE_VALUE) {
			DWORD ErrorCode = GetLastError();
			const char* errorMessage = to_string(ErrorCode).c_str();
			vddlog("e", errorMessage);
			LocalFree(sa.lpSecurityDescriptor);
			return 1;
		}

		BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (connected) {
			vddlog("p", "Client Connected");
			HandleClient(hPipe);
		}
		else {
			CloseHandle(hPipe);
		}
	}
	LocalFree(sa.lpSecurityDescriptor);
	return 0;
}

void StartNamedPipeServer() {
	vddlog("p", "Starting Pipe");
	hPipeThread = CreateThread(NULL, 0, NamedPipeServer, NULL, 0, NULL);
	if (hPipeThread == NULL) {
		DWORD ErrorCode = GetLastError();
		const char* errorMessage = to_string(ErrorCode).c_str();
		vddlog("e", errorMessage);
	}
	else {
		vddlog("p", "Pipe created");
	}
}

void StopNamedPipeServer() {
	vddlog("p", "Stopping Pipe");
	{
		lock_guard<mutex> lock(g_Mutex);
		g_Running = false;
	}
	if (hPipeThread) {
		HANDLE hPipe = CreateFileW(
			PIPE_NAME,
			GENERIC_READ | GENERIC_WRITE,
			0,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);

		if (hPipe != INVALID_HANDLE_VALUE) {
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
bool initpath() {
	// 先尝试从环境变量读取
	wchar_t envPath[MAX_PATH] = { 0 };
	DWORD envLen = GetEnvironmentVariableW(L"ZAKOVDDPATH", envPath, MAX_PATH);
	if (envLen > 0 && envLen < MAX_PATH) {
		confpath = envPath;
		return true;
	}

	// 环境变量未设置，尝试从注册表读取
	HKEY hKey;
	wchar_t szPath[MAX_PATH] = { 0 };
	DWORD dwBufferSize = sizeof(szPath);
	LONG lResult;
	lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter", 0, KEY_READ, &hKey);
	if (lResult != ERROR_SUCCESS) {
		// 注册表不存在，返回false
		return false;
	}

	lResult = RegQueryValueExW(hKey, L"VDDPATH", NULL, NULL, (LPBYTE)szPath, &dwBufferSize);
	if (lResult != ERROR_SUCCESS) {
		RegCloseKey(hKey);
		return false;
	}

	confpath = szPath;
	RegCloseKey(hKey);

	return true;
}


extern "C" EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;

VOID
EvtDriverUnload(
	_In_ WDFDRIVER Driver
)
{
	UNREFERENCED_PARAMETER(Driver);
	
	vddlog("i", "Starting driver unload process");
	
	// Clean up global device resources before stopping services
	if (g_GlobalDevice != nullptr) {
		vddlog("d", "Cleaning up global device resources");
		
		try {
			lock_guard<mutex> lock(g_Mutex);
			auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(g_GlobalDevice);
			if (pContext && pContext->pContext) {
				// Stop any active SwapChain processing
				if (pContext->pContext->HasActiveSwapChain()) {
					vddlog("d", "Stopping active SwapChain processing during unload");
					pContext->pContext->UnassignSwapChain();
					Sleep(50); // Brief wait for cleanup
				}
				
				// Destroy any active monitors
				for (unsigned int i = 0; i < numVirtualDisplays; i++) {
					if (pContext->pContext->HasActiveMonitor()) {
						vddlog("d", ("Destroying monitor " + std::to_string(i) + " during unload").c_str());
						try {
							pContext->pContext->DestroyMonitor(i);
						}
						catch (...) {
							vddlog("w", ("Failed to cleanly destroy monitor " + std::to_string(i) + " during unload").c_str());
						}
						
						if (i < numVirtualDisplays - 1) {
							Sleep(25); // Short wait between destructions
						}
					}
				}
				
				vddlog("d", "Global device resource cleanup completed");
			}
		}
		catch (const std::exception& e) {
			stringstream errorStream;
			errorStream << "Exception during device cleanup in unload: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...) {
			vddlog("e", "Unknown exception during device cleanup in unload");
		}
		
		// Wait for system stabilization
		Sleep(100);
	}
	
	// Stop the named pipe server
	StopNamedPipeServer();
	
	vddlog("i", "Driver unload completed");
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT  pDriverObject,
	PUNICODE_STRING pRegistryPath
)
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
	sendLogsThroughPipe = EnabledQuery(L"SendLogsThroughPipe");


	//colour
	HDRPlus = EnabledQuery(L"HDRPlusEnabled");
	SDR10 = EnabledQuery(L"SDR10Enabled");
	HDRCOLOUR = HDRPlus ? IDDCX_BITS_PER_COMPONENT_12 : IDDCX_BITS_PER_COMPONENT_10;
	SDRCOLOUR = SDR10 ? IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8;
	ColourFormat = GetStringSetting(L"ColourFormat");

	//Cursor
	hardwareCursor = EnabledQuery(L"HardwareCursorEnabled");
	alphaCursorSupport = EnabledQuery(L"AlphaCursorSupport");
	CursorMaxX = GetIntegerSetting(L"CursorMaxX");
	CursorMaxY = GetIntegerSetting(L"CursorMaxY");

	int xorCursorSupportLevelInt = GetIntegerSetting(L"XorCursorSupportLevel");
	std::string xorCursorSupportLevelName;

	if (xorCursorSupportLevelInt < 0 || xorCursorSupportLevelInt > 3) {
		vddlog("w", "Selected Xor Level unsupported, defaulting to IDDCX_XOR_CURSOR_SUPPORT_FULL");
		XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;
	}
	else {
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

	StartNamedPipeServer();

	return Status;
}

vector<string> split(string& input, char delimiter)
{
	istringstream stream(input);
	string field;
	vector<string> result;
	while (getline(stream, field, delimiter)) {
		result.push_back(field);
	}
	return result;
}


void loadSettings() {
	const wstring settingsname = confpath + L"\\vdd_settings.xml";
	const wstring& filename = settingsname;
	if (PathFileExistsW(filename.c_str())) {
		CComPtr<IStream> pStream;
		CComPtr<IXmlReader> pReader;
		HRESULT hr = SHCreateStreamOnFileW(filename.c_str(), STGM_READ, &pStream);
		if (FAILED(hr)) {
			vddlog("e", "Loading Settings: Failed to create file stream.");
			return; 
		}
		hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, NULL);
		if (FAILED(hr)) {
			vddlog("e", "Loading Settings: Failed to create XmlReader.");
			return;
		}
		hr = pReader->SetInput(pStream);
		if (FAILED(hr)) {
			vddlog("e", "Loading Settings: Failed to set input stream.");
			return;
		}

		XmlNodeType nodeType;
		const WCHAR* pwszLocalName;
		const WCHAR* pwszValue;
		UINT cwchLocalName;
		UINT cwchValue;
		wstring currentElement;
		wstring width, height, refreshRate;
		vector<tuple<int, int, int, int>> res;
		wstring gpuFriendlyName;
		UINT monitorcount = 1;
		set<tuple<int, int>> resolutions;
		vector<int> globalRefreshRates;

		while (S_OK == (hr = pReader->Read(&nodeType))) {
			switch (nodeType) {
			case XmlNodeType_Element:
				hr = pReader->GetLocalName(&pwszLocalName, &cwchLocalName);
				if (FAILED(hr)) {
					return;
				}
				currentElement = wstring(pwszLocalName, cwchLocalName);
				break;
			case XmlNodeType_Text:
				hr = pReader->GetValue(&pwszValue, &cwchValue);
				if (FAILED(hr)) {
					return;
				}
				if (currentElement == L"count") {
					monitorcount = stoi(wstring(pwszValue, cwchValue));
					if (monitorcount == 0) {
						monitorcount = 1;
						vddlog("i", "Loading singular monitor (Monitor Count is not valid)");
					}
				}
				else if (currentElement == L"friendlyname") {
					gpuFriendlyName = wstring(pwszValue, cwchValue);
				}
				else if (currentElement == L"width") {
					width = wstring(pwszValue, cwchValue);
					if (width.empty()) {
						width = L"800";
					}
				}
				else if (currentElement == L"height") {
					height = wstring(pwszValue, cwchValue);
					if (height.empty()) {
						height = L"600";
					}
					resolutions.insert(make_tuple(stoi(width), stoi(height)));
				}
				else if (currentElement == L"refresh_rate") {
					refreshRate = wstring(pwszValue, cwchValue);
					if (refreshRate.empty()) {
						refreshRate = L"30";
					}
					int vsync_num, vsync_den;
					float_to_vsync(stof(refreshRate), vsync_num, vsync_den);

					res.push_back(make_tuple(stoi(width), stoi(height), vsync_num, vsync_den));
					stringstream ss;
					ss << "Added: " << stoi(width) << "x" << stoi(height) << " @ " << vsync_num << "/" << vsync_den << "Hz";
					vddlog("d", ss.str().c_str());
				}
				else if (currentElement == L"g_refresh_rate") {
					globalRefreshRates.push_back(stoi(wstring(pwszValue, cwchValue)));
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

		for (int globalRate : globalRefreshRates) {
			for (const auto& resTuple : resolutions) {
				int global_width = get<0>(resTuple);
				int global_height = get<1>(resTuple);

				int vsync_num, vsync_den;
				float_to_vsync(static_cast<float>(globalRate), vsync_num, vsync_den);
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


		numVirtualDisplays = monitorcount;
		gpuname = gpuFriendlyName;
		monitorModes = res;
		vddlog("i","Using vdd_settings.xml");
		return;
	}
	const wstring optionsname = confpath + L"\\option.txt";
	ifstream ifs(optionsname);
	if (ifs.is_open()) {
    string line;
    if (getline(ifs, line) && !line.empty()) {
        numVirtualDisplays = stoi(line);
        vector<tuple<int, int, int, int>> res; 

        while (getline(ifs, line)) {
            vector<string> strvec = split(line, ',');
            if (strvec.size() == 3 && strvec[0].substr(0, 1) != "#") {
                int vsync_num, vsync_den;
                float_to_vsync(stof(strvec[2]), vsync_num, vsync_den); 
                res.push_back({ stoi(strvec[0]), stoi(strvec[1]), vsync_num, vsync_den });
            }
        }

        vddlog("i", "Using option.txt");
        monitorModes = res;
        for (const auto& mode : res) {
            int width, height, vsync_num, vsync_den;
            tie(width, height, vsync_num, vsync_den) = mode;
            stringstream ss;
            ss << "Resolution: " << width << "x" << height << " @ " << vsync_num << "/" << vsync_den << "Hz";
            vddlog("d", ss.str().c_str());
        }
		return;
    } else {
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
		{3840, 2160, 165.0f}
	};

	vddlog("i", "Loading Fallback - no settings found");

	for (const auto& mode : fallbackRes) {
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

	monitorModes = res;
	return;

}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
	stringstream logStream;

	UNREFERENCED_PARAMETER(Driver);

	logStream << "Initializing device:"
		<< "\n  DeviceInit Pointer: " << static_cast<void*>(pDeviceInit);
	vddlog("d", logStream.str().c_str());

	// Register for power callbacks - in this sample only power-on is needed
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = VirtualDisplayDriverDeviceD0Entry;
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
	// redirects IoDeviceControl requests to an internal queue. This sample does not need this.
	// IddConfig.EvtIddCxDeviceIoControl = VirtualDisplayDriverIoDeviceControl;

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
	else {
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
			auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
			if (pContext && pContext->pContext)
			{
				try {
					// Perform comprehensive cleanup before destroying the context
					vddlog("d", "Performing comprehensive device cleanup");
					
					// Stop any active SwapChain processing
					if (pContext->pContext->HasActiveSwapChain()) {
						vddlog("d", "Stopping active SwapChain during device cleanup");
						pContext->pContext->UnassignSwapChain();
						Sleep(50); // Allow cleanup to complete
					}
					
					// Destroy any active monitors
					if (pContext->pContext->HasActiveMonitor()) {
						vddlog("d", "Destroying active monitor during device cleanup");
						try {
							// Note: We only try to destroy monitor 0 since DestroyMonitor handles single monitor
							pContext->pContext->DestroyMonitor(0);
						}
						catch (...) {
							vddlog("w", "Exception while destroying monitor during device cleanup");
						}
					}
					
					// Wait for stabilization
					Sleep(50);
					
					vddlog("d", "Device-specific cleanup completed, calling context cleanup");
				}
				catch (const std::exception& e) {
					stringstream errorStream;
					errorStream << "Exception during device cleanup: " << e.what();
					vddlog("e", errorStream.str().c_str());
				}
				catch (...) {
					vddlog("e", "Unknown exception during device cleanup");
				}
				
				// Always call the context cleanup
				pContext->Cleanup();
				vddlog("d", "Device cleanup callback completed");
			}
			else if (pContext) {
				vddlog("w", "Device context wrapper found but context is null during cleanup");
				pContext->Cleanup();
			}
			else {
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

	// Create a new device context object and attach it to the WDF device object
	/*
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);
	*/ // code to return uncase the device context wrapper isnt found (Most likely insufficient resources)

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
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
NTSTATUS VirtualDisplayDriverDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	//UNREFERENCED_PARAMETER(PreviousState);

	stringstream logStream;

	// Log the entry into D0 state
	logStream << "Entering D0 power state:"
		<< "\n  Device Handle: " << static_cast<void*>(Device)
		<< "\n  Previous State: " << PreviousState;
	vddlog("d", logStream.str().c_str());

	// This function is called by WDF to start the device in the fully-on power state.

	/*
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext->InitAdapter();
	*/ //Added error handling incase fails to get device context

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext && pContext->pContext)
	{
		logStream.str("");
		logStream << "Initializing adapter...";
		vddlog("d", logStream.str().c_str());


		pContext->pContext->InitAdapter();
		logStream.str(""); 
		logStream << "InitAdapter called successfully.";
		vddlog("d", logStream.str().c_str());
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

	D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
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

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
	stringstream logStream;

	logStream << "Constructing SwapChainProcessor:"
		<< "\n  SwapChain Handle: " << static_cast<void*>(hSwapChain)
		<< "\n  Device Pointer: " << static_cast<void*>(Device.get())
		<< "\n  NewFrameEvent Handle: " << NewFrameEvent;
	vddlog("d", logStream.str().c_str());

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
	//SetEvent(m_hTerminateEvent.Get()); changed for error handling + log purposes 

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
			logStream << "Thread wait timed out after 5 seconds. Forcing termination.";
			vddlog("w", logStream.str().c_str());
			// Force terminate the thread if it doesn't respond
			if (!TerminateThread(m_hThread.Get(), 1)) {
				logStream.str("");
				logStream << "Failed to terminate thread. GetLastError: " << GetLastError();
				vddlog("e", logStream.str().c_str());
			} else {
				vddlog("w", "Thread forcefully terminated due to timeout.");
			}
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

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	stringstream logStream;

	logStream << "RunThread started. Argument: " << Argument;
	vddlog("d", logStream.str().c_str());

	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	stringstream logStream;

	logStream << "Run method started.";
	vddlog("d", logStream.str().c_str());

	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

	if (AvTaskHandle)
	{
		logStream.str("");
		logStream << "Multimedia thread characteristics set successfully. AvTask: " << AvTask;
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str(""); 
		logStream << "Failed to set multimedia thread characteristics. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}

	RunCore();

	logStream.str(""); 
	logStream << "Core processing function RunCore() completed.";
	vddlog("d", logStream.str().c_str());

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	if (m_hSwapChain)
	{
		try {
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
		catch (const std::exception& e) {
			stringstream errorStream;
			errorStream << "Exception while deleting swap-chain: " << e.what();
			vddlog("e", errorStream.str().c_str());
			m_hSwapChain = nullptr; // Mark as null to prevent further access
		}
		catch (...) {
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
	*/ //error handling when reversing multimedia thread characteristics 
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
	logStream << "DXGI device interface obtained successfully.";
	//vddlog("d", logStream.str().c_str());


	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	logStream.str("");
	if (FAILED(hr))
	{
		logStream << "Failed to set device to swap chain. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return;
	}
	logStream << "Device set to swap chain successfully.";
	//vddlog("d", logStream.str().c_str());

	logStream.str(""); 
	logStream << "Starting buffer acquisition and release loop.";
	//vddlog("d", logStream.str().c_str());

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		// Ask for the next buffer from the producer
		IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
		BufferInArgs.Size = sizeof(BufferInArgs);
		IDXGIResource* pSurface;

		if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
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
		logStream.str("");
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles[] =
			{
				m_hAvailableBufferEvent,
				m_hTerminateEvent.Get()
			};
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);

			logStream << "Buffer acquisition pending. WaitResult: " << WaitResult;

			if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
			{
				// We have a new buffer, so try the AcquireBuffer again
				//vddlog("d", "New buffer trying aquire new buffer");
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				logStream << "Terminate event signaled. Exiting loop.";
				//vddlog("d", logStream.str().c_str());
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				hr = HRESULT_FROM_WIN32(WaitResult);
				logStream << "Unexpected wait result. HRESULT: " << hr;
				vddlog("e", logStream.str().c_str());
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			AcquiredBuffer.Attach(pSurface);

			// ==============================
			// TODO: Process the frame here
			//
			// This is the most performance-critical section of code in an IddCx driver. It's important that whatever
			// is done with the acquired surface be finished as quickly as possible. This operation could be:
			//  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
			//  * a GPU encode operation
			//  * a GPU VPBlt to another surface
			//  * a GPU custom compute shader encode operation
			// ==============================

			AcquiredBuffer.Reset();
			//vddlog("d", "Reset buffer");
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
			logStream.str(""); // Clear the stream
			logStream << "Failed to acquire buffer. Exiting loop. The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST) - HRESULT: " << hr;
			vddlog("e", logStream.str().c_str());
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

const UINT64 MHZ = 1000000;
const UINT64 KHZ = 1000;

constexpr DISPLAYCONFIG_VIDEO_SIGNAL_INFO dispinfo(UINT32 h, UINT32 v, UINT32 rn, UINT32 rd) {
	const UINT32 clock_rate = rn * (v + 4) * (v + 4) / rd + 1000;
	return {
	  clock_rate,                                      // pixel clock rate [Hz]
	{ clock_rate, v + 4 },                         // fractional horizontal refresh rate [Hz]
	{ clock_rate, (v + 4) * (v + 4) },          // fractional vertical refresh rate [Hz]
	{ h, v },                                    // (horizontal, vertical) active pixel resolution
	{ h + 4, v + 4 },                         // (horizontal, vertical) total pixel resolution
	{ { 255, 0 }},                                   // video standard and vsync divider
	DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE
	};
}

vector<BYTE> hardcodedEdid =
{
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x6b, 0x34, 0x14, 0x02, 0xe7, 0x1e, 0xe7, 0x1e,
    0x1c, 0x22, 0x01, 0x03, 0x80, 0x32, 0x1f, 0x78, 0x06, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0xe0, 0x0e, 0x11, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x5a, 0x61, 0x6b,
    0x6f, 0x20, 0x48, 0x44, 0x52, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64,
    0x02, 0x03, 0x39, 0x70, 0xe6, 0x06, 0x0f, 0x01, 0xa2, 0xa2, 0x10, 0xe3, 0x05, 0xd8, 0x80, 0x68,
    0x03, 0x0c, 0x00, 0x77, 0x77, 0x78, 0x64, 0x0f, 0x29, 0x67, 0x04, 0x03, 0x5f, 0x7f, 0x00, 0x09,
    0x06, 0x07, 0x6d, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0xdf, 0x67, 0x3f, 0x30, 0xf0, 0xcf, 0x60, 0x00,
    0x68, 0x1a, 0x00, 0x00, 0x01, 0x01, 0x30, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
};


void modifyEdid(vector<BYTE>& edid) {
	if (edid.size() < 12) {
		return;
	}

	edid[8] = 0x36;
	edid[9] = 0x94;
	edid[10] = 0x37;
	edid[11] = 0x13;
}



BYTE calculateChecksum(const std::vector<BYTE>& edid) {
	int sum = 0;
	for (int i = 0; i < 127; ++i) {
		sum += edid[i];
	}
	sum %= 256;
	if (sum != 0) {
		sum = 256 - sum;
	}
	return static_cast<BYTE>(sum);
	// check sum calculations. We dont need to include old checksum in calculation, so we only read up to the byte before.
	// Anything after the checksum bytes arent part of the checksum - a flaw with edid managment, not with us
}

void updateCeaExtensionCount(vector<BYTE>& edid, int count) {
	edid[126] = static_cast<BYTE>(count);
}

vector<BYTE> loadEdid(const string& filePath) {
	if (customEdid) {
		vddlog("i", "Attempting to use user Edid");
	}
	else {
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}

	ifstream file(filePath, ios::binary | ios::ate);
	if (!file) {
		vddlog("i", "No custom edid found");
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}

	streamsize size = file.tellg();
	file.seekg(0, ios::beg);

	vector<BYTE> buffer(size);
	if (file.read((char*)buffer.data(), size)) {
		//calculate checksum and compare it to 127 byte, if false then return hardcoded if true then return buffer to prevent loading borked edid.
		BYTE calculatedChecksum = calculateChecksum(buffer);
		if (calculatedChecksum != buffer[127]) {
			vddlog("e", "Custom edid failed due to invalid checksum");
			vddlog("i", "Using hardcoded edid");
			return hardcodedEdid;
		}

		if (edidCeaOverride) {
			if (buffer.size() == 256) {
				for (int i = 128; i < 256; ++i) {
					buffer[i] = hardcodedEdid[i];
				}
				updateCeaExtensionCount(buffer, 1);
			}
			else if (buffer.size() == 128) {
				buffer.insert(buffer.end(), hardcodedEdid.begin() + 128, hardcodedEdid.end());
				updateCeaExtensionCount(buffer, 1);
			}
		}

		vddlog("i", "Using custom edid");
		return buffer;
	}
	else {
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}
}

int maincalc() {
	vector<BYTE> edid = loadEdid(WStringToString(confpath) + "\\user_edid.bin");

	// Validate EDID data
	if (edid.empty()) {
		vddlog("e", "EDID data is empty, adapter initialization may fail");
		return -1;
	}
	
	if (edid.size() < 128) {
		stringstream ss;
		ss << "EDID data too small (" << edid.size() << " bytes), expected at least 128 bytes";
		vddlog("e", ss.str().c_str());
		return -1;
	}

	if (!preventManufacturerSpoof) modifyEdid(edid);
	BYTE checksum = calculateChecksum(edid);
	edid[127] = checksum;
	
	// Setting this variable is depricated, hardcoded edid is either returned or custom in loading edid function
	IndirectDeviceContext::s_KnownMonitorEdid = edid;
	
	stringstream ss;
	ss << "EDID data loaded successfully (" << edid.size() << " bytes)";
	vddlog("d", ss.str().c_str());
	
	return 0;
}



IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
	m_WdfDevice(WdfDevice), m_hMouseEvent(nullptr)
{
	m_Adapter = {};
}

// Helper function to wait with system state validation
bool IndirectDeviceContext::WaitForSystemStabilization(int timeoutMs, const char* operation)
{
	stringstream logStream;
	logStream << "Waiting for system stabilization during: " << operation;
	vddlog("d", logStream.str().c_str());
	
	const int checkInterval = 25; // Check every 25ms
	int elapsed = 0;
	
	while (elapsed < timeoutMs) {
		Sleep(checkInterval);
		elapsed += checkInterval;
		
		// Basic validation - check if our monitor handle is still valid
		if (m_Monitor == nullptr) {
			vddlog("d", "Monitor handle became null during wait - system may have cleaned up");
			return true; // This might be expected in some cases
		}
		
		// Check if we're in the middle of multiple operations
		if (elapsed % 100 == 0) { // Log every 100ms
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
bool IndirectDeviceContext::ValidateMonitorState(const char* operation)
{
	stringstream logStream;
	logStream << "Validating monitor state for: " << operation;
	vddlog("d", logStream.str().c_str());
	
	if (m_Monitor == nullptr) {
		logStream.str("");
		logStream << "Monitor is null during: " << operation;
		vddlog("w", logStream.str().c_str());
		return false;
	}
	
	// Additional state checks could be added here
	// For now, we just verify the handle is not null
	
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

	// First stop the SwapChain processing thread
	if (m_ProcessingThread) {
		try {
			logStream.str("");
			logStream << "Stopping SwapChain processing thread in destructor";
			vddlog("d", logStream.str().c_str());
			
			m_ProcessingThread.reset();
			
			logStream.str("");
			logStream << "SwapChain processing thread stopped successfully in destructor";
			vddlog("d", logStream.str().c_str());
		}
		catch (const std::exception& e) {
			stringstream errorStream;
			errorStream << "Exception while stopping SwapChain in destructor: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...) {
			vddlog("e", "Unknown exception while stopping SwapChain in destructor");
		}
	}

	// Clean up hardware cursor event handle
	if (m_hMouseEvent != nullptr) {
		try {
			vddlog("d", "Cleaning up hardware cursor event handle in destructor");
			CloseHandle(m_hMouseEvent);
			m_hMouseEvent = nullptr;
			vddlog("d", "Hardware cursor event handle cleaned up successfully");
		}
		catch (const std::exception& e) {
			stringstream errorStream;
			errorStream << "Exception while cleaning mouse event in destructor: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...) {
			vddlog("e", "Unknown exception while cleaning mouse event in destructor");
		}
	}

	if (m_Monitor != nullptr) {
		try {
			vddlog("d", "Cleaning up monitor in destructor");
			// Do not call IddCxMonitorDeparture, as this may not be safe in the destructor
			WdfObjectDelete(m_Monitor);
			m_Monitor = nullptr;
			vddlog("d", "Monitor cleaned up successfully in destructor");
		}
		catch (const std::exception& e) {
			stringstream errorStream;
			errorStream << "Exception while cleaning monitor in destructor: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...) {
			vddlog("e", "Unknown exception while cleaning monitor in destructor");
		}
	}

	logStream.str("");
	logStream << "IndirectDeviceContext cleanup completed.";
	vddlog("d", logStream.str().c_str());
}

#define NUM_VIRTUAL_DISPLAYS 1   //What is this even used for ?? Its never referenced

void IndirectDeviceContext::InitAdapter()
{
	stringstream logStream;
	
	// Load settings and GPU configuration first
	loadSettings();
	logStream.str("");
	if (gpuname.empty() || gpuname == L"default") {
		const wstring adaptername = confpath + L"\\adapter.txt";
		Options.Adapter.load(adaptername.c_str());
		logStream << "Attempting to Load GPU from adapter.txt";
	}
	else {
		Options.Adapter.xmlprovide(gpuname);
		logStream << "Loading GPU from vdd_settings.xml";
	}
	vddlog("i", logStream.str().c_str());
	GetGpuInfo();

	// Validate EDID data before proceeding
	int edidResult = maincalc();
	if (edidResult != 0) {
		vddlog("e", "EDID validation failed, adapter initialization will likely fail");
		// Continue anyway, but log the warning
	}

	// Validate monitor modes
	if (monitorModes.empty()) {
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

	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
		logStream << "FP16 processing capability detected.";
	}

	// Validate and set monitor count with bounds checking
	if (numVirtualDisplays == 0 || numVirtualDisplays > 16) {
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
	if (AdapterInit.WdfDevice == nullptr) {
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
		if (AdapterInitOut.AdapterObject == nullptr) {
			vddlog("e", "Adapter initialization returned null handle despite success status");
			return;
		}

		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;
		logStream << "Adapter handle stored successfully.";
		vddlog("d", logStream.str().c_str());

		// Store the device context object into the WDF object context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		if (pContext != nullptr) {
			pContext->pContext = this;
			vddlog("d", "Device context successfully linked to adapter");
		} else {
			vddlog("e", "Failed to get adapter context wrapper");
		}
	}
	else {
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

void IndirectDeviceContext::CreateMonitor(unsigned int index) {
	wstring logMessage = L"Creating Monitor: " + to_wstring(index + 1);
	string narrowLogMessage = WStringToString(logMessage);
	vddlog("i", narrowLogMessage.c_str());

	// ==============================
	// TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDID
	// provided here is purely for demonstration, as it describes only 640x480 @ 60 Hz and 800x600 @ 60 Hz. Monitor
	// manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS to optimize
	// settings like viewing distance and scale factor. Manufacturers should also use a unique serial number every
	// single device to ensure the OS can tell the monitors apart.
	// ==============================

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = index;
	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	//MonitorInfo.MonitorDescription.DataSize = sizeof(s_KnownMonitorEdid);        can no longer use size of as converted to vector
	if (s_KnownMonitorEdid.size() > UINT_MAX)
	{
		vddlog("e", "Edid size passes UINT_Max, escape to prevent loading borked display");
	}
	else
	{
		MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(s_KnownMonitorEdid.size());
	}
	//MonitorInfo.MonitorDescription.pData = const_cast<BYTE*>(s_KnownMonitorEdid);
	// Changed from using const_cast to data() to safely access the EDID data.
	// This improves type safety and code readability, as it eliminates the need for casting 
	// and ensures we are directly working with the underlying container of known monitor EDID data.
	MonitorInfo.MonitorDescription.pData = IndirectDeviceContext::s_KnownMonitorEdid.data();






	// ==============================
	// TODO: The monitor's container ID should be distinct from "this" device's container ID if the monitor is not
	// permanently attached to the display adapter device object. The container ID is typically made unique for each
	// monitor and can be used to associate the monitor with other devices, like audio or input devices. In this
	// sample we generate a random container ID GUID, but it's best practice to choose a stable container ID for a
	// unique monitor or to use "this" device's container ID for a permanent/integrated monitor.
	// ==============================

	// Create a container ID
	CoCreateGuid(&MonitorInfo.MonitorContainerId);
	vddlog("d", "Created container ID");

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	// Create a monitor object with the specified monitor descriptor
	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (NT_SUCCESS(Status))
	{
		vddlog("d", "Monitor created successfully.");
		m_Monitor = MonitorCreateOut.MonitorObject;

		if (m_Monitor == nullptr) {
    		vddlog("e", "Invalid monitor handle");
    		return;
		}

		// Associate the monitor with this device context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorCreateOut.MonitorObject);
		pContext->pContext = this;

		// Tell the OS that the monitor has been plugged in
		IDARG_OUT_MONITORARRIVAL ArrivalOut;
		Status = IddCxMonitorArrival(m_Monitor, &ArrivalOut);
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

void IndirectDeviceContext::DestroyMonitor(unsigned int index) {
	if (m_Monitor == nullptr) {
		vddlog("w", "Monitor handle is already null");
		return;
	}

	stringstream logStream;
	logStream << "Destroying monitor (Index: " << index << ")";
	vddlog("d", logStream.str().c_str());

	try {
		// Step 1: Stop SwapChain processing immediately
		if (m_ProcessingThread) {
			vddlog("d", "Stopping SwapChain processing thread before monitor destruction");
			m_ProcessingThread.reset();
			vddlog("d", "SwapChain processing thread stopped");
		}

		// Step 1.5: Clean up hardware cursor event handle
		if (m_hMouseEvent != nullptr) {
			vddlog("d", "Cleaning up hardware cursor event handle");
			CloseHandle(m_hMouseEvent);
			m_hMouseEvent = nullptr;
			vddlog("d", "Hardware cursor event handle cleaned up");
		}

		// Step 2: Wait for all resources to stabilize with system validation
		if (!WaitForSystemStabilization(300, "resource stabilization before monitor departure")) {
			vddlog("w", "System stabilization timed out, continuing with departure");
		}

		// Step 3: Report monitor departure to the system with retry mechanism
		NTSTATUS Status = STATUS_UNSUCCESSFUL;
		if (!ValidateMonitorState("monitor departure reporting")) {
			vddlog("e", "Monitor state validation failed, skipping departure reporting");
		} else {
			vddlog("d", "Reporting monitor departure to system");
			int retryCount = 0;
			const int maxRetries = 3;
			
			while (retryCount < maxRetries) {
				// Validate state before each retry
				if (!ValidateMonitorState("monitor departure retry")) {
					vddlog("w", "Monitor state invalid during retry, aborting departure attempts");
					break;
				}
				
				Status = IddCxMonitorDeparture(m_Monitor);
				if (NT_SUCCESS(Status)) {
					vddlog("d", "Successfully reported monitor departure");
					break;
				} else {
					retryCount++;
					stringstream errorStream;
					errorStream << "Failed to report monitor departure attempt " << retryCount 
						<< "/" << maxRetries << ". Status: 0x" << hex << Status;
					vddlog("w", errorStream.str().c_str());
					
					if (retryCount < maxRetries) {
						vddlog("d", "Retrying monitor departure after short delay");
						Sleep(100); // Wait before retry
					}
				}
			}
		}
		
		if (!NT_SUCCESS(Status)) {
			vddlog("e", "All monitor departure attempts failed, continuing with cleanup");
			// Continue cleanup even if failed to avoid resource leaks
		}

		// Step 4: Wait for system to process the departure with validation
		if (!WaitForSystemStabilization(500, "system processing of monitor departure")) {
			vddlog("w", "System departure processing timed out, continuing with cleanup");
		}

		// Step 5: Safely delete the monitor object
		if (ValidateMonitorState("final monitor deletion")) {
			vddlog("d", "Deleting monitor WDF object");
			WdfObjectDelete(m_Monitor);
			m_Monitor = nullptr;
			vddlog("d", "Monitor WDF object deleted successfully");
		} else {
			vddlog("w", "Monitor already null, skipping WDF object deletion");
		}

		logStream.str("");
		logStream << "Monitor object destroyed successfully (Index: " << index << ")";
		vddlog("i", logStream.str().c_str());

	} catch (const std::exception& e) {
		stringstream errorStream;
		errorStream << "Exception during monitor destruction (Index: " << index << "): " << e.what();
		vddlog("e", errorStream.str().c_str());
		
		// Force cleanup even after exception
		if (m_Monitor != nullptr) {
			try {
				WdfObjectDelete(m_Monitor);
				m_Monitor = nullptr;
				vddlog("w", "Forced monitor cleanup after exception");
			} catch (...) {
				vddlog("e", "Failed to force cleanup monitor after exception");
				m_Monitor = nullptr; // Set to null anyway to prevent further access
			}
		}
	} catch (...) {
		vddlog("e", "Unknown exception during monitor destruction");
		
		// Force cleanup even after unknown exception
		if (m_Monitor != nullptr) {
			try {
				WdfObjectDelete(m_Monitor);
				m_Monitor = nullptr;
				vddlog("w", "Forced monitor cleanup after unknown exception");
			} catch (...) {
				vddlog("e", "Failed to force cleanup monitor after unknown exception");
				m_Monitor = nullptr; // Set to null anyway to prevent further access
			}
		}
	}
}

void IndirectDeviceContext::AssignSwapChain(IDDCX_MONITOR& Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
	m_ProcessingThread.reset();

	auto Device = make_shared<Direct3DDevice>(RenderAdapter);
	if (FAILED(Device->Init()))
	{
		vddlog("e", "D3D Initialization failed, deleting existing swap-chain.");
		WdfObjectDelete(SwapChain);
		return;
	}
	HRESULT hr = Device->Init(); 
	if (FAILED(hr))
	{
		vddlog("e", "Failed to initialize Direct3DDevice.");
	}
	else
	{
		vddlog("d", "Creating a new swap-chain processing thread.");
		m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));

		if (hardwareCursor){
			// Clean up any existing mouse event handle first
			if (m_hMouseEvent != nullptr) {
				CloseHandle(m_hMouseEvent);
				m_hMouseEvent = nullptr;
				vddlog("d", "Cleaned up existing mouse event handle");
			}

			m_hMouseEvent = CreateEventA(
				nullptr, 
				false,   
				false,   
				"VirtualDisplayDriverMouse"
			);

			if (!m_hMouseEvent)
			{
				vddlog("e", "Failed to create mouse event. No hardware cursor supported!");
				return;
			}

			IDDCX_CURSOR_CAPS cursorInfo = {};
			cursorInfo.Size = sizeof(cursorInfo);
			cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL; 
			cursorInfo.AlphaCursorSupport = alphaCursorSupport;

			cursorInfo.MaxX = CursorMaxX;       //Apparently in most cases 128 is fine but for safe guarding we will go 512, older intel cpus may be limited to 64x64
			cursorInfo.MaxY = CursorMaxY;

			//DirectXDevice->QueryMaxCursorSize(&cursorInfo.MaxX, &cursorInfo.MaxY);                 Experimental to get max cursor size - THIS IS NTO WORKING CODE

			IDARG_IN_SETUP_HWCURSOR hwCursor = {};
			hwCursor.CursorInfo = cursorInfo;
			hwCursor.hNewCursorDataAvailable = m_hMouseEvent;

			NTSTATUS Status = IddCxMonitorSetupHardwareCursor(
				Monitor,
				&hwCursor
			);

			if (FAILED(Status))
			{
				CloseHandle(m_hMouseEvent); 
				m_hMouseEvent = nullptr;
				vddlog("e", "Failed to setup hardware cursor");
				return;
			}

			vddlog("d", "Hardware cursor setup completed successfully.");
		}
		else {
			vddlog("d", "Hardware cursor is disabled, Skipped creation.");
		}
		// At this point, the swap-chain is set up and the hardware cursor is enabled
		// Further swap-chain and cursor processing will occur in the new processing thread.
	}
}


void IndirectDeviceContext::UnassignSwapChain()
{
	// Stop processing the last swap-chain
	vddlog("i", "Unassigning Swapchain. Processing will be stopped.");
	
	if (m_ProcessingThread) {
		try {
			vddlog("d", "Stopping SwapChain processing thread");
			
			// Store a reference to ensure thread cleanup completes
			auto processingThread = std::move(m_ProcessingThread);
			
			// Give the thread time to clean up gracefully
			vddlog("d", "Waiting for SwapChain thread to complete cleanup");
			Sleep(50);
			
			// Now reset the thread (this will trigger the destructor)
			processingThread.reset();
			
			vddlog("d", "SwapChain processing thread stopped successfully");
			
			// Additional wait to ensure all cleanup is complete
			Sleep(25);
			
		}
		catch (const std::exception& e) {
			stringstream errorStream;
			errorStream << "Exception while stopping SwapChain processing thread: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...) {
			vddlog("e", "Unknown exception while stopping SwapChain processing thread");
		}
	} else {
		vddlog("d", "No SwapChain processing thread to stop");
	}
	
	// Ensure m_ProcessingThread is definitely null
	if (m_ProcessingThread) {
		vddlog("w", "Forcing m_ProcessingThread to null after cleanup");
		m_ProcessingThread.reset();
	}
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
	// This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
	// to report attached monitors.

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
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
NTSTATUS VirtualDisplayDriverAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	// For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

	// ==============================
	// TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
	// through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
	// should be turned off).
	// ==============================

	return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	stringstream logStream;
	logStream << "Parsing monitor description. Input buffer count: " << pInArgs->MonitorModeBufferInputCount;
	vddlog("d", logStream.str().c_str());

	// Clear previous monitor modes to prevent accumulation on reload
	s_KnownMonitorModes2.clear();

	for (int i = 0; i < monitorModes.size(); i++) {
		s_KnownMonitorModes2.push_back(dispinfo(std::get<0>(monitorModes[i]), std::get<1>(monitorModes[i]), std::get<2>(monitorModes[i]), std::get<3>(monitorModes[i])));
	}
	pOutArgs->MonitorModeBufferOutputCount = (UINT)monitorModes.size();

	logStream.str("");
	logStream << "Number of monitor modes generated: " << monitorModes.size();
	vddlog("d", logStream.str().c_str());

	if (pInArgs->MonitorModeBufferInputCount < monitorModes.size())
	{
		logStream.str(""); 
		logStream << "Buffer too small. Input count: " << pInArgs->MonitorModeBufferInputCount << ", Required: " << monitorModes.size();
		vddlog("w", logStream.str().c_str());
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		// Copy the known modes to the output buffer
		for (DWORD ModeIndex = 0; ModeIndex < monitorModes.size(); ModeIndex++)
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
NTSTATUS VirtualDisplayDriverMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
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
void CreateTargetMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
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
	Mode.pixelRate = VSyncNum * Width * Height / VSyncDen;

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

void CreateTargetMode(IDDCX_TARGET_MODE& Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	Mode.Size = sizeof(Mode);
	CreateTargetMode(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSyncNum, VSyncDen);
}

void CreateTargetMode2(IDDCX_TARGET_MODE2& Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	stringstream logStream;
	logStream << "Creating IDDCX_TARGET_MODE2 with Width: " << Width
		<< ", Height: " << Height
		<< ", VSyncNum: " << VSyncNum
		<< ", VSyncDen: " << VSyncDen;
	vddlog("d", logStream.str().c_str());

	Mode.Size = sizeof(Mode);


	if (ColourFormat == L"RGB") {
		Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr444") {
		Mode.BitsPerComponent.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr422") {
		Mode.BitsPerComponent.YCbCr422 = SDRCOLOUR | HDRCOLOUR; 
	}
	else if (ColourFormat == L"YCbCr420") {
		Mode.BitsPerComponent.YCbCr420 = SDRCOLOUR | HDRCOLOUR; 
	}
	else {
		Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;  // Default to RGB
	}
	

	logStream.str(""); 
	logStream << "IDDCX_TARGET_MODE2 configured with Size: " << Mode.Size
		<< " and colour format " << WStringToString(ColourFormat);
	vddlog("d", logStream.str().c_str());


	CreateTargetMode(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSyncNum, VSyncDen);
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)////////////////////////////////////////////////////////////////////////////////
{
	UNREFERENCED_PARAMETER(MonitorObject);

	vector<IDDCX_TARGET_MODE> TargetModes(monitorModes.size());

	stringstream logStream;
	logStream << "Creating target modes. Number of monitor modes: " << monitorModes.size();
	vddlog("d", logStream.str().c_str());

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	for (int i = 0; i < monitorModes.size(); i++) {
		CreateTargetMode(TargetModes[i], std::get<0>(monitorModes[i]), std::get<1>(monitorModes[i]), std::get<2>(monitorModes[i]), std::get<3>(monitorModes[i]));

		logStream.str("");
		logStream << "Created target mode " << i << ": Width = " << std::get<0>(monitorModes[i])
			<< ", Height = " << std::get<1>(monitorModes[i])
			<< ", VSync = " << std::get<2>(monitorModes[i]);
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
	else {
		logStream.str("");
		logStream << "Input buffer too small. Required: " << TargetModes.size()
			<< ", Provided: " << pInArgs->TargetModeBufferInputCount;
		vddlog("w", logStream.str().c_str());
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
	stringstream logStream;
	logStream << "Assigning swap chain:"
		<< "\n  hSwapChain: " << pInArgs->hSwapChain
		<< "\n  RenderAdapterLuid: " << pInArgs->RenderAdapterLuid.LowPart << "-" << pInArgs->RenderAdapterLuid.HighPart
		<< "\n  hNextSurfaceAvailable: " << pInArgs->hNextSurfaceAvailable;
	vddlog("d", logStream.str().c_str());
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	vddlog("d", "Swap chain assigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	stringstream logStream;
	logStream << "Unassigning swap chain for monitor object: " << MonitorObject;
	vddlog("d", logStream.str().c_str());
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->UnassignSwapChain();
	vddlog("d", "Swap chain unassigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo(
	IDDCX_ADAPTER AdapterObject,
	IDARG_IN_QUERYTARGET_INFO* pInArgs,
	IDARG_OUT_QUERYTARGET_INFO* pOutArgs
)
{
	stringstream logStream;
	logStream << "Querying target info for adapter object: " << AdapterObject;
	vddlog("d", logStream.str().c_str());

	UNREFERENCED_PARAMETER(pInArgs);

	pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;

	if (ColourFormat == L"RGB") {
		pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr444") {
		pOutArgs->DitheringSupport.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr422") {
		pOutArgs->DitheringSupport.YCbCr422 = SDRCOLOUR | HDRCOLOUR; 
	}
	else if (ColourFormat == L"YCbCr420") {
		pOutArgs->DitheringSupport.YCbCr420 = SDRCOLOUR | HDRCOLOUR; 
	}
	else {
		pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR;  // Default to RGB
	}

	logStream.str("");
	logStream << "Target capabilities set to: " << pOutArgs->TargetCaps
		<< "\nDithering support colour format set to: " << WStringToString(ColourFormat);
	vddlog("d", logStream.str().c_str());

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs
)
{
	stringstream logStream;
	logStream << "Setting default HDR metadata for monitor object: " << MonitorObject;
	vddlog("d", logStream.str().c_str());

	UNREFERENCED_PARAMETER(pInArgs);

	vddlog("d", "Default HDR metadata set successfully.");

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs
)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	stringstream logStream;
	logStream << "Parsing monitor description:"
		<< "\n  MonitorModeBufferInputCount: " << pInArgs->MonitorModeBufferInputCount
		<< "\n  pMonitorModes: " << (pInArgs->pMonitorModes ? "Valid" : "Null");
	vddlog("d", logStream.str().c_str());

	logStream.str("");
	logStream << "Monitor Modes:";
	for (const auto& mode : monitorModes)
	{
		logStream << "\n  Mode - Width: " << std::get<0>(mode)
			<< ", Height: " << std::get<1>(mode)
			<< ", RefreshRate: " << std::get<2>(mode);
	}
	vddlog("d", logStream.str().c_str());

	// Clear previous monitor modes to prevent accumulation on reload
	s_KnownMonitorModes2.clear();

	for (int i = 0; i < monitorModes.size(); i++) {
		s_KnownMonitorModes2.push_back(dispinfo(std::get<0>(monitorModes[i]), std::get<1>(monitorModes[i]), std::get<2>(monitorModes[i]), std::get<3>(monitorModes[i])));
	}
	pOutArgs->MonitorModeBufferOutputCount = (UINT)monitorModes.size();

	if (pInArgs->MonitorModeBufferInputCount < monitorModes.size())
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		// Copy the known modes to the output buffer
		logStream.str(""); // Clear the stream
		logStream << "Writing monitor modes to output buffer:";
		for (DWORD ModeIndex = 0; ModeIndex < monitorModes.size(); ModeIndex++)
		{
			pInArgs->pMonitorModes[ModeIndex].Size = sizeof(IDDCX_MONITOR_MODE2);
			pInArgs->pMonitorModes[ModeIndex].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
			pInArgs->pMonitorModes[ModeIndex].MonitorVideoSignalInfo = s_KnownMonitorModes2[ModeIndex];


			if (ColourFormat == L"RGB") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;
				
			}
			else if (ColourFormat == L"YCbCr444") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr422") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
			}
			else if (ColourFormat == L"YCbCr420") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
			}
			else {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;  // Default to RGB
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
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_QUERYTARGETMODES2* pInArgs,
	IDARG_OUT_QUERYTARGETMODES* pOutArgs
)
{
	//UNREFERENCED_PARAMETER(MonitorObject);
	stringstream logStream;

	logStream << "Querying target modes:"
		<< "\n  MonitorObject Handle: " << static_cast<void*>(MonitorObject) 
		<< "\n  TargetModeBufferInputCount: " << pInArgs->TargetModeBufferInputCount;
	vddlog("d", logStream.str().c_str());

	vector<IDDCX_TARGET_MODE2> TargetModes(monitorModes.size());

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	logStream.str(""); // Clear the stream
	logStream << "Creating target modes:";

	for (int i = 0; i < monitorModes.size(); i++) {
		CreateTargetMode2(TargetModes[i], std::get<0>(monitorModes[i]), std::get<1>(monitorModes[i]), std::get<2>(monitorModes[i]), std::get<3>(monitorModes[i]));
		logStream << "\n  TargetModeIndex: " << i
			<< "\n    Width: " << std::get<0>(monitorModes[i])
			<< "\n    Height: " << std::get<1>(monitorModes[i])
			<< "\n    RefreshRate: " << std::get<2>(monitorModes[i]);
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
NTSTATUS VirtualDisplayDriverEvtIddCxAdapterCommitModes2(
	IDDCX_ADAPTER AdapterObject,
	const IDARG_IN_COMMITMODES2* pInArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_SET_GAMMARAMP* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

#pragma endregion
