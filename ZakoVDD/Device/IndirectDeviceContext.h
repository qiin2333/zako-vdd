#pragma once

#include "..\Driver.h"
#include "..\Rendering\SwapChainProcessor.h"

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Microsoft
{
	namespace IndirectDisp
	{
		class IndirectDeviceContext
		{
		public:
			IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
			virtual ~IndirectDeviceContext();

			void InitAdapter();
			void FinishInit();

			void CreateMonitor(unsigned int index, const GUID *pClientGuid = nullptr, float maxNits = 1000.0f, float minNits = 0.0001f, float maxFALL = 0.0f, float widthCm = 0.0f, float heightCm = 0.0f);
			void DestroyMonitor(unsigned int index);

			void AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
			void UnassignSwapChain(IDDCX_MONITOR Monitor);
			void CommitModes(const IDARG_IN_COMMITMODES *pInArgs);
			void CommitModes2(const IDARG_IN_COMMITMODES2 *pInArgs);
			void UpdateMonitorHdrLuminanceMetadata(IDDCX_MONITOR Monitor, float maxNits, float minNits, float maxFALL);
			NTSTATUS OpenFrameChannel(const VDD_FRAME_CHANNEL_OPEN_REQUEST& request,
			                          HANDLE targetProcess,
			                          VDD_FRAME_CHANNEL_OPEN_RESPONSE& response);

			bool HasActiveSwapChain() const { std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex); return !m_ProcessingThreads.empty(); }
			bool HasActiveMonitor() const { std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex); return !m_Monitors.empty(); }
			bool HasMonitor(unsigned int index) const { std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex); return m_Monitors.count(index) > 0; }
			void UnassignAllSwapChains();
			void DestroyAllMonitors();

			int RefreshMonitorModes(bool refreshMonitorDescription);

		protected:
			struct MonitorCreationParams
			{
				bool hasClientGuid = false;
				GUID clientGuid{};
				float maxNits = 1000.0f;
				float minNits = 0.0001f;
				float maxFALL = 0.0f;
				float widthCm = 0.0f;
				float heightCm = 0.0f;
			};

			bool RecreateMonitor(unsigned int index);

			WDFDEVICE m_WdfDevice;
			IDDCX_ADAPTER m_Adapter;
			mutable std::recursive_mutex m_monitorsMutex;
			std::map<unsigned int, IDDCX_MONITOR> m_Monitors;
			std::map<unsigned int, GUID> m_MonitorGuids;
			std::map<unsigned int, MonitorCreationParams> m_MonitorCreationParams;

			std::map<IDDCX_MONITOR, DISPLAYCONFIG_VIDEO_SIGNAL_INFO> m_CommittedTargetModes;
			// Presence in this map means CommitModes2 supplied an authoritative
			// target color space. The bool is true for HDR10 and false for SDR/WCG.
			std::map<IDDCX_MONITOR, bool> m_CommittedTargetHdrStates;
			std::map<IDDCX_MONITOR, std::unique_ptr<SwapChainProcessor>> m_ProcessingThreads;
			std::map<IDDCX_MONITOR, HANDLE> m_MouseEvents;

		public:
			static const DISPLAYCONFIG_VIDEO_SIGNAL_INFO s_KnownMonitorModes[];
			static std::vector<BYTE> s_KnownMonitorEdid;
		};
	}
}
