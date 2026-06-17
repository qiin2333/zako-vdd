#include "Edid.h"

#include "DefaultEdid.h"
#include "..\Device\IndirectDeviceContext.h"
#include "..\Logging\Logger.h"
#include "..\Util\StringConversion.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace std;
using namespace Microsoft::IndirectDisp;

extern std::atomic<bool> customEdid;
extern std::atomic<bool> preventManufacturerSpoof;
extern std::atomic<bool> edidCeaOverride;
extern wstring confpath;

static std::atomic<int> gEdidProfile{static_cast<int>(VddEdid::Profile::Legacy)};

static VddEdid::Profile DetectAutoEdidProfile()
{
	typedef LONG (NTAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
	{
		vddlog("w", "DetectAutoEdidProfile: ntdll handle missing, defaulting to Legacy");
		return VddEdid::Profile::Legacy;
	}
	auto pRtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
	if (!pRtlGetVersion)
	{
		vddlog("w", "DetectAutoEdidProfile: RtlGetVersion missing, defaulting to Legacy");
		return VddEdid::Profile::Legacy;
	}
	RTL_OSVERSIONINFOW info{};
	info.dwOSVersionInfoSize = sizeof(info);
	if (pRtlGetVersion(&info) != 0)
	{
		vddlog("w", "DetectAutoEdidProfile: RtlGetVersion failed, defaulting to Legacy");
		return VddEdid::Profile::Legacy;
	}

	const bool isWin10OrOlder = (info.dwMajorVersion < 10) ||
		(info.dwMajorVersion == 10 && info.dwBuildNumber < 22000);
	stringstream ss;
	ss << "DetectAutoEdidProfile: build=" << info.dwBuildNumber
	   << " -> " << (isWin10OrOlder ? "Legacy" : "Modern");
	vddlog("i", ss.str().c_str());
	return isWin10OrOlder ? VddEdid::Profile::Legacy : VddEdid::Profile::Modern;
}

bool IsKnownEdidProfileSetting(const wstring &settingValue)
{
	auto parsed = VddEdid::ProfileFromString(settingValue);
	if (parsed != VddEdid::Profile::Auto)
	{
		return true;
	}

	wstring lower;
	lower.reserve(settingValue.size());
	for (wchar_t c : settingValue)
	{
		lower.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c);
	}
	return lower == L"auto";
}

void ApplyEdidProfileSetting(const wstring &settingValue)
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

static vector<BYTE> GetHardcodedEdid()
{
	auto profile = static_cast<VddEdid::Profile>(gEdidProfile.load());
	return VddEdid::GetDefaultEdid(profile);
}

static void ModifyEdidManufacturer(vector<BYTE> &edid)
{
	if (edid.size() < 12)
	{
		return;
	}

	// EDID manufacturer/product ID exposed by Windows as DISPLAY\ZAK2333.
	// Manufacturer "ZAK" is encoded as 0x682b; product 0x2333 is little-endian.
	edid[8] = 0x68;
	edid[9] = 0x2b;
	edid[10] = 0x33;
	edid[11] = 0x23;
}

void ModifyEdidSerialNumber(vector<BYTE> &edid, const GUID &clientGuid)
{
	if (edid.size() < 16)
	{
		return;
	}

	edid[12] = static_cast<BYTE>(clientGuid.Data1 & 0xFF);
	edid[13] = static_cast<BYTE>((clientGuid.Data1 >> 8) & 0xFF);
	edid[14] = static_cast<BYTE>((clientGuid.Data1 >> 16) & 0xFF);
	edid[15] = static_cast<BYTE>((clientGuid.Data1 >> 24) & 0xFF);
}

BYTE CalculateEdidChecksum(const vector<BYTE> &edid)
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
}

void UpdateEdidPhysicalSize(vector<BYTE> &edid, float widthCm, float heightCm)
{
	if (edid.size() < 23)
	{
		vddlog("w", "EDID too small to update physical size");
		return;
	}

	if (widthCm > 0)
	{
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

static void UpdateCeaExtensionCount(vector<BYTE> &edid, int count)
{
	edid[126] = static_cast<BYTE>(count);
}

static BYTE CalculateCeaChecksum(const vector<BYTE> &edid)
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

void UpdateEdidHdrMetadata(vector<BYTE> &edid, float maxNits, float minNits, float maxFALL)
{
	if (edid.size() < 256)
	{
		vddlog("w", "EDID too small to update HDR metadata");
		return;
	}

	if (maxFALL <= 0)
	{
		maxFALL = maxNits * 0.8f;
	}

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

		if (tag == 0x07 && length >= 1 && (pos + 1) < 256 && edid[pos + 1] == 0x06)
		{
			vddlog("d", ("Found HDR Static Metadata block at position " + to_string(pos)).c_str());

			auto calcLumCv = [](float nits) -> BYTE
			{
				float cv = 32.0f * log2f(nits / 50.0f);
				return static_cast<BYTE>(ceilf(max(0.0f, min(255.0f, cv))));
			};

			if (length >= 4 && (pos + 4) < 256)
			{
				edid[pos + 4] = calcLumCv(maxNits);
				float actualNits = 50.0f * powf(2.0f, edid[pos + 4] / 32.0f);
				vddlog("d", ("Set MaxCLL cv=" + to_string((int)edid[pos + 4]) + " (req " + to_string(maxNits) + ", actual " + to_string(actualNits) + " nits)").c_str());
			}

			if (length >= 5 && (pos + 5) < 256)
			{
				edid[pos + 5] = calcLumCv(maxFALL);
				float actualNits = 50.0f * powf(2.0f, edid[pos + 5] / 32.0f);
				vddlog("d", ("Set MaxFALL cv=" + to_string((int)edid[pos + 5]) + " (req " + to_string(maxFALL) + ", actual " + to_string(actualNits) + " nits)").c_str());
			}

			if (length >= 6 && (pos + 6) < 256)
			{
				float minCv = (minNits * 255.0f * 255.0f) / (maxNits * 100.0f);
				edid[pos + 6] = static_cast<BYTE>(roundf(max(0.0f, min(255.0f, minCv))));
				float actualNits = (edid[pos + 6] * maxNits * 100.0f) / (255.0f * 255.0f);
				vddlog("d", ("Set MinLum cv=" + to_string((int)edid[pos + 6]) + " (req " + to_string(minNits) + ", actual " + to_string(actualNits) + " nits)").c_str());
			}

			edid[255] = CalculateCeaChecksum(edid);
			vddlog("d", "Updated CEA extension checksum");
			return;
		}

		pos += length + 1;
	}

	vddlog("w", "HDR Static Metadata block not found in EDID");
}

void UpdateEdidFreeSyncRange(vector<BYTE> &edid, BYTE minHz, BYTE maxHz)
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

		if (tag == 0x03 && length >= 3 && (pos + 3) < 256)
		{
			if (edid[pos + 1] == 0x1A && edid[pos + 2] == 0x00 && edid[pos + 3] == 0x00)
			{
				if (length >= 7 && (pos + 7) < 256)
				{
					BYTE oldMin = edid[pos + 6];
					BYTE oldMax = edid[pos + 7];
					edid[pos + 6] = minHz;
					edid[pos + 7] = maxHz;
					edid[255] = CalculateCeaChecksum(edid);
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

static vector<BYTE> LoadEdid(const string &filePath)
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
	if (file.read(reinterpret_cast<char *>(buffer.data()), size))
	{
		BYTE calculatedChecksum = CalculateEdidChecksum(buffer);
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
				UpdateCeaExtensionCount(buffer, 1);
			}
			else if (buffer.size() == 128)
			{
				buffer.insert(buffer.end(), hardcodedEdid.begin() + 128, hardcodedEdid.end());
				UpdateCeaExtensionCount(buffer, 1);
			}
		}

		vddlog("i", "Using custom edid");
		return buffer;
	}

	vddlog("i", "Using hardcoded edid");
	return hardcodedEdid;
}

int LoadKnownMonitorEdid()
{
	vector<BYTE> edid = LoadEdid(WStringToString(confpath) + "\\user_edid.bin");

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
	{
		ModifyEdidManufacturer(edid);
	}
	BYTE checksum = CalculateEdidChecksum(edid);
	edid[127] = checksum;

	IndirectDeviceContext::s_KnownMonitorEdid = edid;

	stringstream ss;
	ss << "EDID data loaded successfully (" << edid.size() << " bytes)";
	vddlog("d", ss.str().c_str());

	return 0;
}
