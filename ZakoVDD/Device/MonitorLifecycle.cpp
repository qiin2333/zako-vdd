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

	wstring logMessage = L"Creating Monitor: " + to_wstring(index + 1);
	string narrowLogMessage = WStringToString(logMessage);
	vddlog("i", narrowLogMessage.c_str());

	{
		stringstream ss;
		ss << "Monitor " << (index + 1) << " HDR settings - MaxNits: " << maxNits << ", MaxFALL: " << maxFALL << ", MinNits: " << minNits;
		if (widthCm > 0 && heightCm > 0)
		{
			ss << ", Physical size: " << widthCm << " x " << heightCm << " cm";
		}
		vddlog("d", ss.str().c_str());
	}

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
			stringstream ss;
			ss << "Using existing EDID for client GUID (monitor " << (index + 1) << ")";
			vddlog("d", ss.str().c_str());

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

			stringstream ss;
			ss << "Created new EDID with serial number based on client GUID for monitor " << (index + 1);
			vddlog("d", ss.str().c_str());
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

	if (pClientGuid != nullptr)
	{
		m_MonitorGuids[index] = *pClientGuid;
	}
	else
	{
		m_MonitorGuids.erase(index);
	}

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = index;
	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	if (pMonitorEdid->size() > UINT_MAX)
	{
		vddlog("e", "Edid size passes UINT_Max, escape to prevent loading borked display");
	}
	else
	{
		MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(pMonitorEdid->size());
	}
	MonitorInfo.MonitorDescription.pData = pMonitorEdid->data();

	if (pClientGuid != nullptr)
	{
		MonitorInfo.MonitorContainerId = *pClientGuid;
		stringstream ss;
		ss << "Using client-provided GUID as container ID for monitor " << (index + 1);
		vddlog("d", ss.str().c_str());
	}
	else
	{
		CoCreateGuid(&MonitorInfo.MonitorContainerId);
		vddlog("d", "Created container ID");
	}

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (NT_SUCCESS(Status))
	{
		vddlog("d", "Monitor created successfully.");
		IDDCX_MONITOR newMonitor = MonitorCreateOut.MonitorObject;

		if (newMonitor == nullptr)
		{
			vddlog("e", "Invalid monitor handle");
			return;
		}

		m_Monitors[index] = newMonitor;

		{
			lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
			MonitorHdrSettings hdrSettings;
			hdrSettings.isHdr = false;
			hdrSettings.maxNits = maxNits;
			hdrSettings.minNits = minNits;
			hdrSettings.maxFALL = maxFALL;
			s_MonitorHdrSettingsMap[newMonitor] = hdrSettings;

			stringstream ss;
			ss << "Stored HDR settings for monitor " << (index + 1)
			   << " - IsHdr: false, MaxNits: " << maxNits
			   << ", MinNits: " << minNits
			   << ", MaxFALL: " << maxFALL;
			vddlog("d", ss.str().c_str());
		}

		auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorCreateOut.MonitorObject);
		pContext->pContext = this;

		IDARG_OUT_MONITORARRIVAL ArrivalOut;
		Status = IddCxMonitorArrival(newMonitor, &ArrivalOut);
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

void IndirectDeviceContext::DestroyMonitor(unsigned int index)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	auto monIt = m_Monitors.find(index);
	if (monIt == m_Monitors.end() || monIt->second == nullptr)
	{
		stringstream ws;
		ws << "Monitor handle for index " << index << " is already null or not found";
		vddlog("w", ws.str().c_str());
		return;
	}

	IDDCX_MONITOR hMonitor = monIt->second;

	stringstream logStream;
	logStream << "Destroying monitor (Index: " << index << ")";
	vddlog("d", logStream.str().c_str());

	try
	{
		{
			lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
			auto it = s_MonitorHdrSettingsMap.find(hMonitor);
			if (it != s_MonitorHdrSettingsMap.end())
			{
				s_MonitorHdrSettingsMap.erase(it);
				vddlog("d", "Cleaned up HDR settings for monitor");
			}
		}

		m_CommittedTargetModes.erase(hMonitor);

		{
			auto guidIt = m_MonitorGuids.find(index);
			if (guidIt != m_MonitorGuids.end())
			{
				lock_guard<mutex> edidLock(s_EdidMapMutex);
				s_ClientGuidEdidMap.erase(guidIt->second);
				m_MonitorGuids.erase(guidIt);
				vddlog("d", "Cleaned up EDID cache for monitor client GUID");
			}
		}

		{
			auto scIt = m_ProcessingThreads.find(hMonitor);
			if (scIt != m_ProcessingThreads.end())
			{
				vddlog("d", "Stopping SwapChain processing thread before monitor destruction");
				scIt->second.reset();
				m_ProcessingThreads.erase(scIt);
				vddlog("d", "SwapChain processing thread stopped");
			}
		}

		{
			auto meIt = m_MouseEvents.find(hMonitor);
			if (meIt != m_MouseEvents.end())
			{
				if (meIt->second != nullptr)
				{
					vddlog("d", "Cleaning up hardware cursor event handle");
					CloseHandle(meIt->second);
				}
				m_MouseEvents.erase(meIt);
				vddlog("d", "Hardware cursor event handle cleaned up");
			}
		}

		Sleep(300);

		NTSTATUS Status = STATUS_UNSUCCESSFUL;
		{
			vddlog("d", "Reporting monitor departure to system");
			int retryCount = 0;
			const int maxRetries = 3;

			while (retryCount < maxRetries)
			{
				Status = IddCxMonitorDeparture(hMonitor);
				if (NT_SUCCESS(Status))
				{
					vddlog("d", "Successfully reported monitor departure");
					break;
				}

				retryCount++;
				stringstream errorStream;
				errorStream << "Failed to report monitor departure attempt " << retryCount
							<< "/" << maxRetries << ". Status: 0x" << hex << Status;
				vddlog("w", errorStream.str().c_str());

				if (retryCount < maxRetries)
				{
					Sleep(100);
				}
			}
		}

		if (!NT_SUCCESS(Status))
		{
			vddlog("e", "All monitor departure attempts failed, continuing with cleanup");
		}

		Sleep(500);

		vddlog("d", "Deleting monitor WDF object");
		WdfObjectDelete(hMonitor);
		m_Monitors.erase(monIt);
		vddlog("d", "Monitor WDF object deleted successfully");

		logStream.str("");
		logStream << "Monitor object destroyed successfully (Index: " << index << ")";
		vddlog("i", logStream.str().c_str());
	}
	catch (const exception &e)
	{
		stringstream errorStream;
		errorStream << "Exception during monitor destruction (Index: " << index << "): " << e.what();
		vddlog("e", errorStream.str().c_str());
		m_Monitors.erase(index);
	}
	catch (...)
	{
		vddlog("e", "Unknown exception during monitor destruction");
		m_Monitors.erase(index);
	}
}

void IndirectDeviceContext::DestroyAllMonitors()
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	vddlog("i", "Destroying all monitors.");
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
		vddlog("w", "RefreshMonitorModes: IddCxMonitorUpdateModes2 not available on this OS");
		return -1;
	}

	vector<tuple<int, int, int, int>> localModes;
	{
		lock_guard<mutex> dataLock(g_DataMutex);
		localModes = monitorModes;
		s_KnownMonitorModes2.clear();
		for (size_t i = 0; i < localModes.size(); ++i)
		{
			s_KnownMonitorModes2.push_back(dispinfo(
				get<0>(localModes[i]),
				get<1>(localModes[i]),
				get<2>(localModes[i]),
				get<3>(localModes[i])));
		}
	}

	if (localModes.empty())
	{
		vddlog("w", "RefreshMonitorModes: monitorModes is empty, refusing to push");
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
		stringstream ss;
		ss << "RefreshMonitorModes: monitor index=" << pair.first
		   << " status=0x" << hex << status << " modeCount=" << dec << targetModes.size();
		if (NT_SUCCESS(status))
		{
			++refreshed;
			vddlog("d", ss.str().c_str());
		}
		else
		{
			vddlog("w", ss.str().c_str());
		}
	}

	stringstream summary;
	summary << "RefreshMonitorModes: pushed " << refreshed << "/" << m_Monitors.size()
	        << " monitors with " << targetModes.size() << " modes (no departure)";
	vddlog("i", summary.str().c_str());
	return refreshed;
}
