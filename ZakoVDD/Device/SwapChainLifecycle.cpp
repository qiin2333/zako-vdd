#include "..\Driver.h"
#include "..\Core\DriverState.h"
#include "..\Logging\Logger.h"
#include "IndirectDeviceContext.h"
#include "MonitorState.h"

#include <exception>
#include <memory>

using namespace std;
using namespace Microsoft::IndirectDisp;

void IndirectDeviceContext::AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	{
		auto scIt = m_ProcessingThreads.find(Monitor);
		if (scIt != m_ProcessingThreads.end())
		{
			scIt->second.reset();
			m_ProcessingThreads.erase(scIt);
		}
	}

	auto Device = make_shared<Direct3DDevice>(RenderAdapter);
	HRESULT hr = Device->Init();
	if (FAILED(hr))
	{
		VDD_LOG_ERROR_STREAM("Failed to initialize Direct3DDevice. HRESULT: " << hr << ". Deleting existing swap-chain.");
		WdfObjectDelete(SwapChain);
		return;
	}

	VDD_LOG_DEBUG("Creating a new swap-chain processing thread.");

	unsigned int monitorIndex = 0xFFFFFFFFu;
	for (const auto &kv : m_Monitors)
	{
		if (kv.second == Monitor)
		{
			monitorIndex = kv.first;
			break;
		}
	}

	m_ProcessingThreads[Monitor] = unique_ptr<SwapChainProcessor>(new SwapChainProcessor(SwapChain, Device, NewFrameEvent, monitorIndex));

	auto procIt = m_ProcessingThreads.find(Monitor);
	if (procIt != m_ProcessingThreads.end() && procIt->second)
	{
		auto modeIt = m_CommittedTargetModes.find(Monitor);
		if (modeIt != m_CommittedTargetModes.end())
		{
			auto hdrIt = m_CommittedTargetHdrStates.find(Monitor);
			const bool hasExpectedHdrState = hdrIt != m_CommittedTargetHdrStates.end();
			procIt->second->PublishModeMetadata(modeIt->second,
			                                    hasExpectedHdrState,
			                                    hasExpectedHdrState && hdrIt->second);
		}

		lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
		auto hdrIt = s_MonitorHdrSettingsMap.find(Monitor);
		if (hdrIt != s_MonitorHdrSettingsMap.end())
		{
			procIt->second->UpdateHdrLuminanceMetadata(hdrIt->second.maxNits,
			                                           hdrIt->second.minNits,
			                                           hdrIt->second.maxFALL);
		}
	}

	if (hardwareCursor)
	{
		auto meIt = m_MouseEvents.find(Monitor);
		if (meIt != m_MouseEvents.end())
		{
			if (meIt->second != nullptr)
			{
				CloseHandle(meIt->second);
			}
			m_MouseEvents.erase(meIt);
			VDD_LOG_DEBUG("Cleaned up existing mouse event handle");
		}

		HANDLE hMouseEvent = CreateEventA(nullptr, false, false, nullptr);
		if (!hMouseEvent)
		{
			VDD_LOG_ERROR("Failed to create mouse event. No hardware cursor supported!");
			return;
		}

		m_MouseEvents[Monitor] = hMouseEvent;

		IDDCX_CURSOR_CAPS cursorInfo = {};
		cursorInfo.Size = sizeof(cursorInfo);
		cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
		cursorInfo.AlphaCursorSupport = alphaCursorSupport;
		cursorInfo.MaxX = CursorMaxX;
		cursorInfo.MaxY = CursorMaxY;

		IDARG_IN_SETUP_HWCURSOR hwCursor = {};
		hwCursor.CursorInfo = cursorInfo;
		hwCursor.hNewCursorDataAvailable = hMouseEvent;

		NTSTATUS Status = IddCxMonitorSetupHardwareCursor(Monitor, &hwCursor);
		if (FAILED(Status))
		{
			CloseHandle(hMouseEvent);
			m_MouseEvents.erase(Monitor);
			VDD_LOG_ERROR("Failed to setup hardware cursor");
			return;
		}

		VDD_LOG_DEBUG("Hardware cursor setup completed successfully.");
	}
	else
	{
		VDD_LOG_DEBUG("Hardware cursor is disabled, Skipped creation.");
	}
}

void IndirectDeviceContext::CommitModes(const IDARG_IN_COMMITMODES *pInArgs)
{
	if (!pInArgs || !pInArgs->pPaths)
	{
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);
	for (UINT i = 0; i < pInArgs->PathCount; ++i)
	{
		const auto &path = pInArgs->pPaths[i];
		if (!path.MonitorObject)
		{
			continue;
		}

		if ((path.Flags & IDDCX_PATH_FLAGS_ACTIVE) != 0)
		{
			m_CommittedTargetModes[path.MonitorObject] = path.TargetVideoSignalInfo;
			m_CommittedTargetHdrStates.erase(path.MonitorObject);

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->PublishModeMetadata(path.TargetVideoSignalInfo, false, false);
			}
		}
		else
		{
			m_CommittedTargetModes.erase(path.MonitorObject);
			m_CommittedTargetHdrStates.erase(path.MonitorObject);

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->ClearExpectedMode();
			}
		}
	}
}

void IndirectDeviceContext::CommitModes2(const IDARG_IN_COMMITMODES2 *pInArgs)
{
	if (!pInArgs || !pInArgs->pPaths)
	{
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);
	for (UINT i = 0; i < pInArgs->PathCount; ++i)
	{
		const auto &path = pInArgs->pPaths[i];
		if (!path.MonitorObject)
		{
			continue;
		}

		if ((path.Flags & IDDCX_PATH_FLAGS_ACTIVE) != 0)
		{
			m_CommittedTargetModes[path.MonitorObject] = path.TargetVideoSignalInfo;
			const bool hasExpectedHdrState =
				path.WireFormatInfo.ColorSpace != IDDCX_COLOR_SPACE_UNINITIALIZED;
			const bool expectedIsHdr =
				path.WireFormatInfo.ColorSpace == IDDCX_COLOR_SPACE_G2084_P2020;
			if (hasExpectedHdrState)
			{
				m_CommittedTargetHdrStates[path.MonitorObject] = expectedIsHdr;
			}
			else
			{
				m_CommittedTargetHdrStates.erase(path.MonitorObject);
			}

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->PublishModeMetadata(path.TargetVideoSignalInfo,
				                                    hasExpectedHdrState,
				                                    expectedIsHdr);
			}
		}
		else
		{
			m_CommittedTargetModes.erase(path.MonitorObject);
			m_CommittedTargetHdrStates.erase(path.MonitorObject);

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->ClearExpectedMode();
			}
		}
	}
}

void IndirectDeviceContext::UpdateMonitorHdrLuminanceMetadata(IDDCX_MONITOR Monitor, float maxNits, float minNits, float maxFALL)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	{
		lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
		auto &hdrSettings = s_MonitorHdrSettingsMap[Monitor];
		hdrSettings.maxNits = maxNits;
		hdrSettings.minNits = minNits;
		hdrSettings.maxFALL = maxFALL;
	}

	auto procIt = m_ProcessingThreads.find(Monitor);
	if (procIt != m_ProcessingThreads.end() && procIt->second)
	{
		procIt->second->UpdateHdrLuminanceMetadata(maxNits, minNits, maxFALL);
	}
}

NTSTATUS IndirectDeviceContext::OpenFrameChannel(const VDD_FRAME_CHANNEL_OPEN_REQUEST& request,
                                                 HANDLE targetProcess,
                                                 VDD_FRAME_CHANNEL_OPEN_RESPONSE& response)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	for (auto& pair : m_ProcessingThreads)
	{
		if (pair.second && pair.second->m_MonitorIndex == request.MonitorIndex)
		{
			return pair.second->OpenFrameChannel(request, targetProcess, response);
		}
	}

	return STATUS_DEVICE_NOT_READY;
}

void IndirectDeviceContext::UnassignSwapChain(IDDCX_MONITOR Monitor)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	VDD_LOG_INFO("Unassigning Swapchain. Processing will be stopped.");

	auto scIt = m_ProcessingThreads.find(Monitor);
	if (scIt != m_ProcessingThreads.end())
	{
		try
		{
			VDD_LOG_DEBUG("Stopping SwapChain processing thread");
			auto processingThread = move(scIt->second);
			m_ProcessingThreads.erase(scIt);

			Sleep(50);
			processingThread.reset();
			VDD_LOG_DEBUG("SwapChain processing thread stopped successfully");
			Sleep(25);
		}
		catch (const exception &e)
		{
			VDD_LOG_ERROR_STREAM("Exception while stopping SwapChain processing thread: " << e.what());
		}
		catch (...)
		{
			VDD_LOG_ERROR("Unknown exception while stopping SwapChain processing thread");
		}
	}
	else
	{
		VDD_LOG_DEBUG("No SwapChain processing thread to stop for this monitor");
	}
}

void IndirectDeviceContext::UnassignAllSwapChains()
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	VDD_LOG_INFO("Unassigning all SwapChains.");
	for (auto it = m_ProcessingThreads.begin(); it != m_ProcessingThreads.end();)
	{
		try
		{
			it->second.reset();
			it = m_ProcessingThreads.erase(it);
		}
		catch (...)
		{
			VDD_LOG_ERROR("Exception while stopping a SwapChain processing thread");
			it = m_ProcessingThreads.erase(it);
		}
	}
	Sleep(50);
}
