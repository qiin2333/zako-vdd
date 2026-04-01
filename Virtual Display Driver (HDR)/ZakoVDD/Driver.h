#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>
#include <tuple>
#include <string>
#include <map>
#include <mutex>

#include "Trace.h"

// Forward declarations for global variables
extern UINT numVirtualDisplays;
extern std::wstring gpuname;
extern std::vector<std::tuple<int, int, int, int>> monitorModes;

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace IndirectDisp
    {
        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

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
        };

        /// <summary>
        /// Provides a sample implementation of an indirect display driver.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();

            void CreateMonitor(unsigned int index, const GUID* pClientGuid = nullptr, float maxNits = 1000.0f, float minNits = 0.0001f, float maxFALL = 0.0f, float widthCm = 0.0f, float heightCm = 0.0f);
            void DestroyMonitor(unsigned int index);

            void AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain(IDDCX_MONITOR Monitor);

            // Helper methods for driver reload
            bool HasActiveSwapChain() const { std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex); return !m_ProcessingThreads.empty(); }
            bool HasActiveMonitor() const { std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex); return !m_Monitors.empty(); }
            bool HasMonitor(unsigned int index) const { std::lock_guard<std::recursive_mutex> lock(m_monitorsMutex); return m_Monitors.count(index) > 0; }
            void UnassignAllSwapChains();
            void DestroyAllMonitors();

        private:
            bool WaitForSystemStabilization(int timeoutMs, const char *operation);
            bool ValidateMonitorState(const char *operation);

        protected:
            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
            mutable std::recursive_mutex m_monitorsMutex; // Protects m_Monitors, m_ProcessingThreads, m_MouseEvents
            std::map<unsigned int, IDDCX_MONITOR> m_Monitors;
            std::map<unsigned int, GUID> m_MonitorGuids; // Maps index to client GUID for EDID cleanup

            std::map<IDDCX_MONITOR, std::unique_ptr<SwapChainProcessor>> m_ProcessingThreads;
            std::map<IDDCX_MONITOR, HANDLE> m_MouseEvents;

        public:
            static const DISPLAYCONFIG_VIDEO_SIGNAL_INFO s_KnownMonitorModes[];
            static std::vector<BYTE> s_KnownMonitorEdid;
        };
    }
}