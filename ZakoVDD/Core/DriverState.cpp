#include "DriverState.h"

std::mutex g_Mutex;
std::mutex g_DataMutex; // Protects monitorModes, s_KnownMonitorModes2, numVirtualDisplays, gpuname
WDFDEVICE g_GlobalDevice = nullptr;

DriverOptions Options;
std::vector<std::tuple<int, int, int, int>> monitorModes;
std::vector<DISPLAYCONFIG_VIDEO_SIGNAL_INFO> s_KnownMonitorModes2;
UINT numVirtualDisplays = 1;
std::wstring gpuname = L"";
std::wstring confpath = []()
{
	wchar_t sysDrive[MAX_PATH];
	DWORD len = GetEnvironmentVariableW(L"SystemDrive", sysDrive, MAX_PATH);
	if (len == 0 || len > MAX_PATH)
	{
		return std::wstring(L"C:\\VirtualDisplayDriver");
	}
	return std::wstring(sysDrive) + L"\\VirtualDisplayDriver";
}();

std::atomic<bool> HDRPlus{false};
std::atomic<bool> SDR10{false};
std::atomic<bool> customEdid{false};

// Variable Refresh Rate (FreeSync / G-Sync compatible) toggle. Keep the
// setting for UI/IOCTL compatibility, but do not infer adapter capability
// flags from it on the current IddCx0102 compatibility path. Some hosts
// reject unknown or mismatched adapter flag bits during adapter init.
std::atomic<bool> vrrEnabled{false};
std::atomic<bool> hardwareCursor{false};
std::atomic<bool> preventManufacturerSpoof{false};
std::atomic<bool> edidCeaOverride{false};
// Mouse settings
std::atomic<bool> alphaCursorSupport{true};
int CursorMaxX = 128;
int CursorMaxY = 128;
IDDCX_XOR_CURSOR_SUPPORT XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;

// Rest
IDDCX_BITS_PER_COMPONENT SDRCOLOUR = IDDCX_BITS_PER_COMPONENT_8;
IDDCX_BITS_PER_COMPONENT HDRCOLOUR = IDDCX_BITS_PER_COMPONENT_10;

std::wstring ColourFormat = L"RGB";
