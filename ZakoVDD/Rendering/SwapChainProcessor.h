#pragma once

#include "Direct3DDevice.h"
#include "SharedFrameExporter.h"

#include <memory>

namespace Microsoft
{
	namespace WRL
	{
		namespace Wrappers
		{
			typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
		}
	}

	namespace IndirectDisp
	{
		class SwapChainProcessor
		{
		public:
			SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent, unsigned int MonitorIndex);
			~SwapChainProcessor();

			void PublishModeMetadata(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO &mode,
			                         bool hasExpectedHdrState,
			                         bool expectedIsHdr);
			void ClearExpectedMode();
			void UpdateHdrLuminanceMetadata(float maxNits, float minNits, float maxFALL);
			NTSTATUS OpenFrameChannel(const VDD_FRAME_CHANNEL_OPEN_REQUEST& request,
			                          HANDLE targetProcess,
			                          VDD_FRAME_CHANNEL_OPEN_RESPONSE& response);

		private:
			static DWORD CALLBACK RunThread(LPVOID Argument);

			void Run();
			void RunCore();

		public:
			IDDCX_SWAPCHAIN m_hSwapChain;
			std::shared_ptr<Direct3DDevice> m_Device;
			HANDLE m_hAvailableBufferEvent;
			Microsoft::WRL::Wrappers::Thread m_hThread;
			Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
			unsigned int m_MonitorIndex;
			std::unique_ptr<SharedFrameExporter> m_Exporter;
		};
	}
}
