#pragma once

#include <string>
#include <tuple>
#include <vector>
#include <windows.h>

bool IsKnownEdidProfileSetting(const std::wstring &settingValue);
void ApplyEdidProfileSetting(const std::wstring &settingValue);
// The template and its source must be read together under s_EdidMapMutex.
bool KnownMonitorEdidIsBuiltIn();
void ApplyConfiguredEdidResolution(std::vector<BYTE> &edid, const std::vector<std::tuple<int, int, int, int>> &modes, bool isBuiltIn);

void ModifyEdidSerialNumber(std::vector<BYTE> &edid, const GUID &clientGuid);
BYTE CalculateEdidChecksum(const std::vector<BYTE> &edid);
void UpdateEdidPhysicalSize(std::vector<BYTE> &edid, float widthCm, float heightCm);
void UpdateEdidHdrMetadata(std::vector<BYTE> &edid, float maxNits, float minNits, float maxFALL);
void UpdateEdidFreeSyncRange(std::vector<BYTE> &edid, BYTE minHz, BYTE maxHz);

int LoadKnownMonitorEdid();
