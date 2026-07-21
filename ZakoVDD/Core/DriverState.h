#pragma once

#include "..\Driver.h"
#include "DriverOptions.h"

#include <atomic>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

extern std::mutex g_Mutex;
extern std::mutex g_DataMutex;
extern WDFDEVICE g_GlobalDevice;

extern DriverOptions Options;
extern std::vector<std::tuple<int, int, int, int>> monitorModes;
extern UINT numVirtualDisplays;
extern std::wstring gpuname;
extern std::wstring confpath;

extern std::atomic<bool> HDRPlus;
extern std::atomic<bool> SDR10;
extern std::atomic<bool> customEdid;
extern std::atomic<bool> vrrEnabled;
extern std::atomic<bool> hardwareCursor;
extern std::atomic<bool> preventManufacturerSpoof;
extern std::atomic<bool> edidCeaOverride;
extern std::atomic<bool> legacyNamedFrameChannel;

extern std::atomic<bool> alphaCursorSupport;
extern int CursorMaxX;
extern int CursorMaxY;
extern IDDCX_XOR_CURSOR_SUPPORT XorCursorSupportLevel;

extern IDDCX_BITS_PER_COMPONENT SDRCOLOUR;
extern IDDCX_BITS_PER_COMPONENT HDRCOLOUR;
extern std::wstring ColourFormat;
