#include "SwapChainProcessor.h"

#include "..\Logging\Logger.h"

#include <exception>

using namespace std;
using namespace Microsoft::WRL;

namespace Microsoft
{
namespace IndirectDisp
{

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent, unsigned int MonitorIndex)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent), m_MonitorIndex(MonitorIndex)
{
	VDD_LOG_DEBUG_STREAM("Constructing SwapChainProcessor:"
	                     << "\n  SwapChain Handle: " << static_cast<void*>(hSwapChain)
	                     << "\n  Device Pointer: " << static_cast<void*>(Device.get())
	                     << "\n  NewFrameEvent Handle: " << NewFrameEvent
	                     << "\n  Monitor Index: " << MonitorIndex);

	// Initialize the VDD->external consumer frame exporter (best effort).
	try
	{
		m_Exporter = std::make_unique<SharedFrameExporter>(MonitorIndex, Device);
	}
	catch (...)
	{
		VDD_LOG_ERROR("[VddExport] Failed to construct SharedFrameExporter (non-fatal)");
		m_Exporter.reset();
	}

	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
	if (!m_hTerminateEvent.Get())
	{
		VDD_LOG_ERROR_STREAM("Failed to create terminate event. GetLastError: " << GetLastError());
	}
	else
	{
		VDD_LOG_DEBUG("Terminate event created successfully.");
	}

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
	if (!m_hThread.Get())
	{
		VDD_LOG_ERROR_STREAM("Failed to create swap-chain processing thread. GetLastError: " << GetLastError());
	}
	else
	{
		VDD_LOG_DEBUG("Swap-chain processing thread created and started successfully.");
	}
}

SwapChainProcessor::~SwapChainProcessor()
{
	VDD_LOG_DEBUG("Destructing SwapChainProcessor:");
	// Alert the swap-chain processing thread to terminate
	// SetEvent(m_hTerminateEvent.Get()); changed for error handling + log purposes

	if (SetEvent(m_hTerminateEvent.Get()))
	{
		VDD_LOG_DEBUG("Terminate event signaled successfully.");
	}
	else
	{
		VDD_LOG_ERROR_STREAM("Failed to signal terminate event. GetLastError: " << GetLastError());
	}

	if (m_hThread.Get())
	{
		// The worker owns a raw this pointer, so the destructor must not return
		// until the thread has stopped touching object state.
		DWORD waitResult = WaitForSingleObject(m_hThread.Get(), INFINITE);
		switch (waitResult)
		{
		case WAIT_OBJECT_0:
			VDD_LOG_DEBUG("Thread terminated successfully.");
			break;
		case WAIT_ABANDONED:
			VDD_LOG_ERROR_STREAM("Thread wait was abandoned. GetLastError: " << GetLastError());
			break;
		default:
			VDD_LOG_ERROR_STREAM("Unexpected result from WaitForSingleObject: " << waitResult << ". GetLastError: " << GetLastError());
			break;
		}
	}
	else
	{
		VDD_LOG_ERROR("No valid thread handle to wait for.");
	}
}

void SwapChainProcessor::PublishModeMetadata(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode,
                                             bool hasExpectedHdrState,
                                             bool expectedIsHdr)
{
	if (!m_Exporter)
	{
		return;
	}

	const UINT width = mode.activeSize.cx ? static_cast<UINT>(mode.activeSize.cx) : static_cast<UINT>(mode.totalSize.cx);
	const UINT height = mode.activeSize.cy ? static_cast<UINT>(mode.activeSize.cy) : static_cast<UINT>(mode.totalSize.cy);
	m_Exporter->PublishModeMetadata(width, height, hasExpectedHdrState, expectedIsHdr);
}

void SwapChainProcessor::ClearExpectedMode()
{
	if (m_Exporter)
	{
		m_Exporter->ClearExpectedMode();
	}
}

void SwapChainProcessor::UpdateHdrLuminanceMetadata(float maxNits, float minNits, float maxFALL)
{
	if (!m_Exporter)
	{
		return;
	}

	m_Exporter->UpdateHdrLuminanceMetadata(maxNits, minNits, maxFALL);
}

NTSTATUS SwapChainProcessor::OpenFrameChannel(const VDD_FRAME_CHANNEL_OPEN_REQUEST& request,
                                              HANDLE targetProcess,
                                              VDD_FRAME_CHANNEL_OPEN_RESPONSE& response)
{
	if (!m_Exporter)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	return m_Exporter->OpenFrameChannel(request, targetProcess, response);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	VDD_LOG_DEBUG_STREAM("RunThread started. Argument: " << Argument);

	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	VDD_LOG_DEBUG("Run method started.");

	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	// Use "Pro Audio" task for highest priority scheduling (lower latency than "Distribution" or "Playback").
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &AvTask);

	if (AvTaskHandle)
	{
		// Additionally boost thread priority within the MMCSS task
		if (!AvSetMmThreadPriority(AvTaskHandle, AVRT_PRIORITY_CRITICAL))
		{
			VDD_LOG_WARNING_STREAM("Failed to set MMCSS priority to CRITICAL. GetLastError: " << GetLastError());
		}
		VDD_LOG_DEBUG_STREAM("Multimedia thread characteristics set successfully (Pro Audio). AvTask: " << AvTask);
	}
	else
	{
		VDD_LOG_ERROR_STREAM("Failed to set multimedia thread characteristics. GetLastError: " << GetLastError());
	}

	// Also set regular thread priority as high as possible (in case MMCSS doesn't fully apply)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	RunCore();

	VDD_LOG_DEBUG("Core processing function RunCore() completed.");

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	if (m_hSwapChain)
	{
		try
		{
			// Attempt graceful cleanup first
			VDD_LOG_DEBUG("Attempting graceful swap-chain cleanup");

			// Give the system time to complete any pending operations
			Sleep(10);

			WdfObjectDelete((WDFOBJECT)m_hSwapChain);
			VDD_LOG_DEBUG("Swap-chain object deleted successfully.");
			m_hSwapChain = nullptr;
		}
		catch (const std::exception& e)
		{
			VDD_LOG_ERROR_STREAM("Exception while deleting swap-chain: " << e.what());
			m_hSwapChain = nullptr; // Mark as null to prevent further access
		}
		catch (...)
		{
			VDD_LOG_ERROR("Unknown exception while deleting swap-chain");
			m_hSwapChain = nullptr; // Mark as null to prevent further access
		}
	}
	else
	{
		VDD_LOG_DEBUG("No valid swap-chain object to delete.");
	}
	/*
	AvRevertMmThreadCharacteristics(AvTaskHandle);
	*/
	// error handling when reversing multimedia thread characteristics
	if (AvRevertMmThreadCharacteristics(AvTaskHandle))
	{
		VDD_LOG_DEBUG("Multimedia thread characteristics reverted successfully.");
	}
	else
	{
		VDD_LOG_ERROR_STREAM("Failed to revert multimedia thread characteristics. GetLastError: " << GetLastError());
	}
}

void SwapChainProcessor::RunCore()
{
	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR_STREAM("Failed to get DXGI device interface. HRESULT: " << hr);
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	if (FAILED(hr))
	{
		// 0x887A0026 = DXGI_ERROR_ACCESS_LOST
		// This usually means the OS already unassigned/abandoned this swap-chain. Treat as a normal teardown path.
		if (hr != static_cast<HRESULT>(0x887A0026))
		{
			VDD_LOG_ERROR_STREAM("Failed to set device to swap chain. HRESULT: " << hr);
		}
		return;
	}

	// Raise GPU priority to realtime for this device to avoid starvation under heavy GPU load (IddCx 1.9+)
	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority))
	{
		IDARG_IN_SETREALTIMEGPUPRIORITY PriorityArgs = {};
		PriorityArgs.pDevice = DxgiDevice.Get();
		hr = IddCxSetRealtimeGPUPriority(m_hSwapChain, &PriorityArgs);
		if (FAILED(hr))
		{
			VDD_LOG_WARNING_STREAM("IddCxSetRealtimeGPUPriority failed (non-fatal). HRESULT: 0x" << hex << hr);
		}
		else
		{
			VDD_LOG_DEBUG("GPU priority raised to realtime for swap chain processing");
		}
	}

	// Cache function availability check outside the loop for better performance
	const bool useBuffer2 = IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2);

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		// Ask for the next buffer from the producer
		IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
		BufferInArgs.Size = sizeof(BufferInArgs);
		IDXGIResource* pSurface = nullptr;
		UINT64 presentDisplayQpc = 0;
		UINT presentationFrameNumber = 0;
		UINT dirtyRectCount = 0;
		DXGI_COLOR_SPACE_TYPE surfaceColorSpace = DXGI_COLOR_SPACE_CUSTOM;
		bool hasSurfaceColorSpace = false;

		if (useBuffer2)
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
			presentDisplayQpc = Buffer.MetaData.PresentDisplayQPCTime;
			presentationFrameNumber = Buffer.MetaData.PresentationFrameNumber;
			dirtyRectCount = Buffer.MetaData.DirtyRectCount;
			surfaceColorSpace = Buffer.MetaData.SurfaceColorSpace;
			hasSurfaceColorSpace = surfaceColorSpace != DXGI_COLOR_SPACE_CUSTOM;
		}
		else
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
			presentDisplayQpc = Buffer.MetaData.PresentDisplayQPCTime;
			presentationFrameNumber = Buffer.MetaData.PresentationFrameNumber;
			dirtyRectCount = Buffer.MetaData.DirtyRectCount;
		}
		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles[] =
			    {
			        m_hAvailableBufferEvent,
			        m_hTerminateEvent.Get()};
			// Let the kernel wake us on the event.
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, INFINITE);

			if (WaitResult == WAIT_OBJECT_0)
			{
				// We have a new buffer, so try the AcquireBuffer again
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				hr = (WaitResult == WAIT_FAILED) ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(WaitResult);
				VDD_LOG_ERROR_STREAM("Unexpected wait result. HRESULT: " << hr);
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			AcquiredBuffer.Attach(pSurface);

			// VDD->Sunshine direct-capture export. Best-effort, never throws,
			// failure here MUST NOT stall the IddCx swap-chain loop.
			if (m_Exporter)
			{
				m_Exporter->PushFrame(AcquiredBuffer.Get(),
				                      surfaceColorSpace,
				                      hasSurfaceColorSpace,
				                      presentDisplayQpc,
				                      presentationFrameNumber,
				                      dirtyRectCount);
			}

			AcquiredBuffer.Reset();
			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}

			// Frame statistics can be reported here once encode/send timings are tracked.
			// IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
		}
		else
		{
			// Only build log message when actually needed (error case)
			// 0x887A0026 = DXGI_ERROR_ACCESS_LOST: treat as normal swap-chain teardown.
			if (hr != static_cast<HRESULT>(0x887A0026))
			{
				VDD_LOG_ERROR_STREAM("Failed to acquire buffer. Exiting loop. HRESULT: " << hr);
			}
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

}
}
