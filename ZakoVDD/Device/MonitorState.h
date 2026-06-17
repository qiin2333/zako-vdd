#pragma once

#include <map>
#include <mutex>
#include <vector>

#include "..\Driver.h"

struct GuidComparator
{
	bool operator()(const GUID& lhs, const GUID& rhs) const
	{
		if (lhs.Data1 != rhs.Data1)
		{
			return lhs.Data1 < rhs.Data1;
		}
		if (lhs.Data2 != rhs.Data2)
		{
			return lhs.Data2 < rhs.Data2;
		}
		if (lhs.Data3 != rhs.Data3)
		{
			return lhs.Data3 < rhs.Data3;
		}
		for (int i = 0; i < 8; i++)
		{
			if (lhs.Data4[i] != rhs.Data4[i])
			{
				return lhs.Data4[i] < rhs.Data4[i];
			}
		}
		return false;
	}
};

struct MonitorHdrSettings
{
	bool isHdr;
	float maxNits;
	float minNits;
	float maxFALL;
};

extern std::map<GUID, std::vector<BYTE>, GuidComparator> s_ClientGuidEdidMap;
extern std::mutex s_EdidMapMutex;

extern std::map<IDDCX_MONITOR, MonitorHdrSettings> s_MonitorHdrSettingsMap;
extern std::mutex s_HdrSettingsMutex;
