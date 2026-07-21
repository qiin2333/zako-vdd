#include "..\Driver.h"
#include "..\Core\DriverState.h"
#include "..\Edid\Edid.h"
#include "..\Logging\Logger.h"
#include "..\Util\StringConversion.h"
#include "DisplayModeHelpers.h"
#include "IndirectDeviceContext.h"
#include "IndirectDeviceContextWrapper.h"
#include "MonitorState.h"

#include <climits>
#include <exception>
#include <sstream>
#include <tuple>
#include <vector>

using namespace std;
using namespace Microsoft::IndirectDisp;

void CreateTargetMode2(IDDCX_TARGET_MODE2 &Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen);

void IndirectDeviceContext::CreateMonitor(unsigned int index, const GUID *pClientGuid, float maxNits, float minNits, float maxFALL, float widthCm, float heightCm)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);
	auto existingMonitor = m_Monitors.find(index);
	if (existingMonitor != m_Monitors.end() && existingMonitor->second != nullptr)
	{
		VDD_LOG_INFO_STREAM("Monitor " << (index + 1) << " already exists; treating duplicate create as success");
		return;
	}

	wstring logMessage = L"Creating Monitor: " + to_wstring(index + 1);
	string narrowLogMessage = WStringToString(logMessage);
	VDD_LOG_INFO(narrowLogMessage.c_str());

	VDD_LOG_DEBUG_LAZY([&]() -> string
	{
		stringstream ss;
		ss << "Monitor " << (index + 1) << " HDR settings - MaxNits: " << maxNits << ", MaxFALL: " << maxFALL << ", MinNits: " << minNits;
		if (widthCm > 0 && heightCm > 0)
		{
			ss << ", Physical size: " << widthCm << " x " << heightCm << " cm";
		}
		return ss.str();
	});

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	BYTE freeSyncMinHz = 48;
	BYTE freeSyncMaxHz = 60;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		float maxHz = 0.0f;
		for (const auto &m : monitorModes)
		{
			int num = get<2>(m);
			int den = get<3>(m);
			if (den > 0)
			{
				float hz = static_cast<float>(num) / static_cast<float>(den);
				if (hz > maxHz)
				{
					maxHz = hz;
				}
			}
		}
		int rounded = static_cast<int>(maxHz + 0.5f);
		if (rounded < freeSyncMinHz)
		{
			rounded = freeSyncMinHz;
		}
		if (rounded > 255)
		{
			rounded = 255;
		}
		freeSyncMaxHz = static_cast<BYTE>(rounded);
	}

	vector<BYTE> *pMonitorEdid = nullptr;
	if (pClientGuid != nullptr)
	{
		lock_guard<mutex> edidLock(s_EdidMapMutex);
		auto it = s_ClientGuidEdidMap.find(*pClientGuid);
		if (it != s_ClientGuidEdidMap.end())
		{
			pMonitorEdid = &it->second;
			VDD_LOG_DEBUG_STREAM("Using existing EDID for client GUID (monitor " << (index + 1) << ")");

			UpdateEdidHdrMetadata(*pMonitorEdid, maxNits, minNits, maxFALL);
			UpdateEdidFreeSyncRange(*pMonitorEdid, freeSyncMinHz, freeSyncMaxHz);
			if (widthCm > 0 || heightCm > 0)
			{
				UpdateEdidPhysicalSize(*pMonitorEdid, widthCm, heightCm);
			}

			BYTE checksum = CalculateEdidChecksum(*pMonitorEdid);
			(*pMonitorEdid)[127] = checksum;
		}
		else
		{
			s_ClientGuidEdidMap[*pClientGuid] = s_KnownMonitorEdid;
			pMonitorEdid = &s_ClientGuidEdidMap[*pClientGuid];
			ModifyEdidSerialNumber(*pMonitorEdid, *pClientGuid);

			UpdateEdidHdrMetadata(*pMonitorEdid, maxNits, minNits, maxFALL);
			UpdateEdidFreeSyncRange(*pMonitorEdid, freeSyncMinHz, freeSyncMaxHz);
			if (widthCm > 0 || heightCm > 0)
			{
				UpdateEdidPhysicalSize(*pMonitorEdid, widthCm, heightCm);
			}

			BYTE checksum = CalculateEdidChecksum(*pMonitorEdid);
			(*pMonitorEdid)[127] = checksum;

			VDD_LOG_DEBUG_STREAM("Created new EDID with serial number based on client GUID for monitor " << (index + 1));
		}
	}
	else
	{
		UpdateEdidHdrMetadata(s_KnownMonitorEdid, maxNits, minNits, maxFALL);
		UpdateEdidFreeSyncRange(s_KnownMonitorEdid, freeSyncMinHz, freeSyncMaxHz);
		if (widthCm > 0 || heightCm > 0)
		{
			UpdateEdidPhysicalSize(s_KnownMonitorEdid, widthCm, heightCm);
		}

		BYTE checksum = CalculateEdidChecksum(s_KnownMonitorEdid);
		s_KnownMonitorEdid[127] = checksum;

		pMonitorEdid = &s_KnownMonitorEdid;
	}

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = index;
	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	if (pMonitorEdid->size() > UINT_MAX)
	{
		VDD_LOG_ERROR("Edid size passes UINT_Max, escape to prevent loading borked display");
	}
	else
	{
		MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(pMonitorEdid->size());
	}
	MonitorInfo.MonitorDescription.pData = pMonitorEdid->data();

	if (pClientGuid != nullptr)
	{
		MonitorInfo.MonitorContainerId = *pClientGuid;
		VDD_LOG_DEBUG_STREAM("Using client-provided GUID as container ID for monitor " << (index + 1));
	}
	else
	{
		CoCreateGuid(&MonitorInfo.MonitorContainerId);
		VDD_LOG_DEBUG("Created container ID");
	}

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (NT_SUCCESS(Status))
	{
		VDD_LOG_DEBUG("Monitor created successfully.");
		IDDCX_MONITOR newMonitor = MonitorCreateOut.MonitorObject;

		if (newMonitor == nullptr)
		{
			VDD_LOG_ERROR("Invalid monitor handle");
			return;
		}

		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorCreateOut.MonitorObject);
		pContext->pContext = this;

		IDARG_OUT_MONITORARRIVAL ArrivalOut;
		Status = IddCxMonitorArrival(newMonitor, &ArrivalOut);
		if (NT_SUCCESS(Status))
		{
			m_Monitors[index] = newMonitor;
			if (pClientGuid != nullptr)
			{
				m_MonitorGuids[index] = *pClientGuid;
			}
			else
			{
				m_MonitorGuids.erase(index);
			}

			{
				lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
				MonitorHdrSettings hdrSettings;
				hdrSettings.maxNits = maxNits;
				hdrSettings.minNits = minNits;
				hdrSettings.maxFALL = maxFALL;
				s_MonitorHdrSettingsMap[newMonitor] = hdrSettings;
			}

			VDD_LOG_DEBUG("Monitor arrival successfully reported.");
		}
		else
		{
			VDD_LOG_ERROR_STREAM("Failed to report monitor arrival. Status: " << Status);
			WdfObjectDelete(newMonitor);
		}
	}
	else
	{
		VDD_LOG_ERROR_STREAM("Failed to create monitor. Status: " << Status);
	}
}

void IndirectDeviceContext::DestroyMonitor(unsigned int index)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	auto monIt = m_Monitors.find(index);
	if (monIt == m_Monitors.end() || monIt->second == nullptr)
	{
		VDD_LOG_WARNING_STREAM("Monitor handle for index " << index << " is already null or not found");
		return;
	}

	IDDCX_MONITOR hMonitor = monIt->second;

	VDD_LOG_DEBUG_STREAM("Destroying monitor (Index: " << index << ")");

	try
	{
		{
			lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
			auto it = s_MonitorHdrSettingsMap.find(hMonitor);
			if (it != s_MonitorHdrSettingsMap.end())
			{
				s_MonitorHdrSettingsMap.erase(it);
				VDD_LOG_DEBUG("Cleaned up HDR settings for monitor");
			}
		}

		m_CommittedTargetModes.erase(hMonitor);
		m_CommittedTargetHdrStates.erase(hMonitor);

		{
			auto guidIt = m_MonitorGuids.find(index);
			if (guidIt != m_MonitorGuids.end())
			{
				lock_guard<mutex> edidLock(s_EdidMapMutex);
				s_ClientGuidEdidMap.erase(guidIt->second);
				m_MonitorGuids.erase(guidIt);
				VDD_LOG_DEBUG("Cleaned up EDID cache for monitor client GUID");
			}
		}

		{
			auto scIt = m_ProcessingThreads.find(hMonitor);
			if (scIt != m_ProcessingThreads.end())
			{
				VDD_LOG_DEBUG("Stopping SwapChain processing thread before monitor destruction");
				scIt->second.reset();
				m_ProcessingThreads.erase(scIt);
				VDD_LOG_DEBUG("SwapChain processing thread stopped");
			}
		}

		{
			auto meIt = m_MouseEvents.find(hMonitor);
			if (meIt != m_MouseEvents.end())
			{
				if (meIt->second != nullptr)
				{
					VDD_LOG_DEBUG("Cleaning up hardware cursor event handle");
					CloseHandle(meIt->second);
				}
				m_MouseEvents.erase(meIt);
				VDD_LOG_DEBUG("Hardware cursor event handle cleaned up");
			}
		}

		Sleep(300);

		NTSTATUS Status = STATUS_UNSUCCESSFUL;
		{
			VDD_LOG_DEBUG("Reporting monitor departure to system");
			int retryCount = 0;
			const int maxRetries = 3;

			while (retryCount < maxRetries)
			{
				Status = IddCxMonitorDeparture(hMonitor);
				if (NT_SUCCESS(Status))
				{
					VDD_LOG_DEBUG("Successfully reported monitor departure");
					break;
				}

				retryCount++;
				VDD_LOG_WARNING_STREAM("Failed to report monitor departure attempt " << retryCount
				                       << "/" << maxRetries << ". Status: 0x" << hex << Status);

				if (retryCount < maxRetries)
				{
					Sleep(100);
				}
			}
		}

		if (!NT_SUCCESS(Status))
		{
			VDD_LOG_ERROR("All monitor departure attempts failed, continuing with cleanup");
		}

		Sleep(500);

		VDD_LOG_DEBUG("Deleting monitor WDF object");
		WdfObjectDelete(hMonitor);
		m_Monitors.erase(monIt);
		VDD_LOG_DEBUG("Monitor WDF object deleted successfully");

		VDD_LOG_INFO_STREAM("Monitor object destroyed successfully (Index: " << index << ")");
	}
	catch (const exception &e)
	{
		VDD_LOG_ERROR_STREAM("Exception during monitor destruction (Index: " << index << "): " << e.what());
		m_Monitors.erase(index);
	}
	catch (...)
	{
		VDD_LOG_ERROR("Unknown exception during monitor destruction");
		m_Monitors.erase(index);
	}
}

void IndirectDeviceContext::DestroyAllMonitors()
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	VDD_LOG_INFO("Destroying all monitors.");
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
	if (!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2))
	{
		VDD_LOG_WARNING("RefreshMonitorModes: IddCxMonitorUpdateModes2 not available on this OS");
		return -1;
	}

	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
	}

	if (localModes.empty())
	{
		VDD_LOG_WARNING("RefreshMonitorModes: monitorModes is empty, refusing to push");
		return 0;
	}

	vector<IDDCX_TARGET_MODE2> targetModes(localModes.size());
	for (size_t i = 0; i < localModes.size(); ++i)
	{
		CreateTargetMode2(targetModes[i],
			static_cast<UINT>(get<0>(localModes[i])),
			static_cast<UINT>(get<1>(localModes[i])),
			static_cast<UINT>(get<2>(localModes[i])),
			static_cast<UINT>(get<3>(localModes[i])));
	}

	int refreshed = 0;
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);
	for (const auto &pair : m_Monitors)
	{
		IDDCX_MONITOR hMonitor = pair.second;
		if (hMonitor == nullptr)
		{
			continue;
		}

		IDARG_IN_UPDATEMODES2 inArgs = {};
		inArgs.Reason = IDDCX_UPDATE_REASON_OTHER;
		inArgs.TargetModeCount = static_cast<UINT>(targetModes.size());
		inArgs.pTargetModes = targetModes.data();

		NTSTATUS status = IddCxMonitorUpdateModes2(hMonitor, &inArgs);
		if (NT_SUCCESS(status))
		{
			++refreshed;
			VDD_LOG_DEBUG_STREAM("RefreshMonitorModes: monitor index=" << pair.first
			                     << " status=0x" << hex << status << " modeCount=" << dec << targetModes.size());
		}
		else
		{
			VDD_LOG_WARNING_STREAM("RefreshMonitorModes: monitor index=" << pair.first
			                       << " status=0x" << hex << status << " modeCount=" << dec << targetModes.size());
		}
	}

	VDD_LOG_INFO_STREAM("RefreshMonitorModes: pushed " << refreshed << "/" << m_Monitors.size()
	                    << " monitors with " << targetModes.size() << " modes (no departure)");
	return refreshed;
}
