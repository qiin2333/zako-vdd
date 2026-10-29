#include "MonitorState.h"

std::map<GUID, ClientEdid, GuidComparator> s_ClientGuidEdidMap;
std::mutex s_EdidMapMutex;

std::map<IDDCX_MONITOR, MonitorHdrSettings> s_MonitorHdrSettingsMap;
std::mutex s_HdrSettingsMutex;
