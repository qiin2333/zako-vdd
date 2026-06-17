#include "..\Driver.h"
#include "..\Core\DriverState.h"
#include "..\Logging\Logger.h"
#include "IndirectDeviceContext.h"
#include "MonitorState.h"

#include <exception>
#include <memory>
#include <sstream>

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
		stringstream ss;
		ss << "Failed to initialize Direct3DDevice. HRESULT: " << hr << ". Deleting existing swap-chain.";
		vddlog("e", ss.str().c_str());
		WdfObjectDelete(SwapChain);
		return;
	}

	vddlog("d", "Creating a new swap-chain processing thread.");

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
			procIt->second->PublishModeMetadata(modeIt->second);
		}

		lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
		auto hdrIt = s_MonitorHdrSettingsMap.find(Monitor);
		if (hdrIt != s_MonitorHdrSettingsMap.end())
		{
			procIt->second->UpdateHdrMetadata(hdrIt->second.isHdr,
			                                hdrIt->second.maxNits,
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
			vddlog("d", "Cleaned up existing mouse event handle");
		}

		HANDLE hMouseEvent = CreateEventA(nullptr, false, false, nullptr);
		if (!hMouseEvent)
		{
			vddlog("e", "Failed to create mouse event. No hardware cursor supported!");
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
			vddlog("e", "Failed to setup hardware cursor");
			return;
		}

		vddlog("d", "Hardware cursor setup completed successfully.");
	}
	else
	{
		vddlog("d", "Hardware cursor is disabled, Skipped creation.");
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

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->PublishModeMetadata(path.TargetVideoSignalInfo);
			}
		}
		else
		{
			m_CommittedTargetModes.erase(path.MonitorObject);
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

			auto procIt = m_ProcessingThreads.find(path.MonitorObject);
			if (procIt != m_ProcessingThreads.end() && procIt->second)
			{
				procIt->second->PublishModeMetadata(path.TargetVideoSignalInfo);
			}
		}
		else
		{
			m_CommittedTargetModes.erase(path.MonitorObject);
		}
	}
}

void IndirectDeviceContext::UpdateMonitorHdrMetadata(IDDCX_MONITOR Monitor, bool isHdr, float maxNits, float minNits, float maxFALL)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	{
		lock_guard<mutex> hdrLock(s_HdrSettingsMutex);
		auto &hdrSettings = s_MonitorHdrSettingsMap[Monitor];
		hdrSettings.isHdr = isHdr;
		hdrSettings.maxNits = maxNits;
		hdrSettings.minNits = minNits;
		hdrSettings.maxFALL = maxFALL;
	}

	auto procIt = m_ProcessingThreads.find(Monitor);
	if (procIt != m_ProcessingThreads.end() && procIt->second)
	{
		procIt->second->UpdateHdrMetadata(isHdr, maxNits, minNits, maxFALL);
	}
}

void IndirectDeviceContext::UnassignSwapChain(IDDCX_MONITOR Monitor)
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	vddlog("i", "Unassigning Swapchain. Processing will be stopped.");

	auto scIt = m_ProcessingThreads.find(Monitor);
	if (scIt != m_ProcessingThreads.end())
	{
		try
		{
			vddlog("d", "Stopping SwapChain processing thread");
			auto processingThread = move(scIt->second);
			m_ProcessingThreads.erase(scIt);

			Sleep(50);
			processingThread.reset();
			vddlog("d", "SwapChain processing thread stopped successfully");
			Sleep(25);
		}
		catch (const exception &e)
		{
			stringstream errorStream;
			errorStream << "Exception while stopping SwapChain processing thread: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...)
		{
			vddlog("e", "Unknown exception while stopping SwapChain processing thread");
		}
	}
	else
	{
		vddlog("d", "No SwapChain processing thread to stop for this monitor");
	}
}

void IndirectDeviceContext::UnassignAllSwapChains()
{
	std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex);

	vddlog("i", "Unassigning all SwapChains.");
	for (auto it = m_ProcessingThreads.begin(); it != m_ProcessingThreads.end();)
	{
		try
		{
			it->second.reset();
			it = m_ProcessingThreads.erase(it);
		}
		catch (...)
		{
			vddlog("e", "Exception while stopping a SwapChain processing thread");
			it = m_ProcessingThreads.erase(it);
		}
	}
	Sleep(50);
}
