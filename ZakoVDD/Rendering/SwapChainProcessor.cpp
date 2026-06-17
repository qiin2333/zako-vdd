#include "SwapChainProcessor.h"

#include <exception>
#include <sstream>

using namespace std;
using namespace Microsoft::WRL;

void vddlog(const char* type, const char* message);

namespace Microsoft
{
namespace IndirectDisp
{

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent, unsigned int MonitorIndex)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent), m_MonitorIndex(MonitorIndex)
{
	stringstream logStream;

	logStream << "Constructing SwapChainProcessor:"
	          << "\n  SwapChain Handle: " << static_cast<void*>(hSwapChain)
	          << "\n  Device Pointer: " << static_cast<void*>(Device.get())
	          << "\n  NewFrameEvent Handle: " << NewFrameEvent
	          << "\n  Monitor Index: " << MonitorIndex;
	vddlog("d", logStream.str().c_str());

	// Initialize the VDD->external consumer frame exporter (best effort).
	try
	{
		m_Exporter = std::make_unique<SharedFrameExporter>(MonitorIndex, Device);
	}
	catch (...)
	{
		vddlog("e", "[VddExport] Failed to construct SharedFrameExporter (non-fatal)");
		m_Exporter.reset();
	}

	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
	if (!m_hTerminateEvent.Get())
	{
		logStream.str("");
		logStream << "Failed to create terminate event. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Terminate event created successfully.";
		vddlog("d", logStream.str().c_str());
	}

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
	if (!m_hThread.Get())
	{
		logStream.str("");
		logStream << "Failed to create swap-chain processing thread. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Swap-chain processing thread created and started successfully.";
		vddlog("d", logStream.str().c_str());
	}
}

SwapChainProcessor::~SwapChainProcessor()
{
	stringstream logStream;

	logStream << "Destructing SwapChainProcessor:";

	vddlog("d", logStream.str().c_str());
	// Alert the swap-chain processing thread to terminate
	// SetEvent(m_hTerminateEvent.Get()); changed for error handling + log purposes

	if (SetEvent(m_hTerminateEvent.Get()))
	{
		logStream.str("");
		logStream << "Terminate event signaled successfully.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to signal terminate event. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}

	if (m_hThread.Get())
	{
		// Wait for the thread to terminate with a timeout to avoid hanging
		DWORD waitResult = WaitForSingleObject(m_hThread.Get(), 5000); // 5 second timeout
		switch (waitResult)
		{
		case WAIT_OBJECT_0:
			logStream.str("");
			logStream << "Thread terminated successfully.";
			vddlog("d", logStream.str().c_str());
			break;
		case WAIT_ABANDONED:
			logStream.str("");
			logStream << "Thread wait was abandoned. GetLastError: " << GetLastError();
			vddlog("e", logStream.str().c_str());
			break;
		case WAIT_TIMEOUT:
			logStream.str("");
			logStream << "Thread wait timed out after 5 seconds. Thread will be abandoned (unsafe to force terminate).";
			vddlog("w", logStream.str().c_str());
			// Note: TerminateThread is not used here because it can corrupt the heap,
			// leave locks held, and cause deadlocks. The thread handle will be closed
			// when m_hThread is destroyed, but the thread itself may still be running.
			break;
		default:
			logStream.str("");
			logStream << "Unexpected result from WaitForSingleObject: " << waitResult << ". GetLastError: " << GetLastError();
			vddlog("e", logStream.str().c_str());
			break;
		}
	}
	else
	{
		logStream.str("");
		logStream << "No valid thread handle to wait for.";
		vddlog("e", logStream.str().c_str());
	}
}

void SwapChainProcessor::PublishModeMetadata(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode)
{
	if (!m_Exporter)
	{
		return;
	}

	const UINT width = mode.activeSize.cx ? static_cast<UINT>(mode.activeSize.cx) : static_cast<UINT>(mode.totalSize.cx);
	const UINT height = mode.activeSize.cy ? static_cast<UINT>(mode.activeSize.cy) : static_cast<UINT>(mode.totalSize.cy);
	m_Exporter->PublishModeMetadata(width, height);
}

void SwapChainProcessor::UpdateHdrMetadata(bool isHdr, float maxNits, float minNits, float maxFALL)
{
	if (!m_Exporter)
	{
		return;
	}

	m_Exporter->UpdateHdrMetadata(isHdr, maxNits, minNits, maxFALL);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	stringstream logStream;

	logStream << "RunThread started. Argument: " << Argument;
	vddlog("d", logStream.str().c_str());

	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	stringstream logStream;

	logStream << "Run method started.";
	vddlog("d", logStream.str().c_str());

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
			logStream.str("");
			logStream << "Failed to set MMCSS priority to CRITICAL. GetLastError: " << GetLastError();
			vddlog("w", logStream.str().c_str());
		}
		logStream.str("");
		logStream << "Multimedia thread characteristics set successfully (Pro Audio). AvTask: " << AvTask;
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to set multimedia thread characteristics. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}

	// Also set regular thread priority as high as possible (in case MMCSS doesn't fully apply)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	RunCore();

	logStream.str("");
	logStream << "Core processing function RunCore() completed.";
	vddlog("d", logStream.str().c_str());

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	if (m_hSwapChain)
	{
		try
		{
			// Attempt graceful cleanup first
			vddlog("d", "Attempting graceful swap-chain cleanup");

			// Give the system time to complete any pending operations
			Sleep(10);

			WdfObjectDelete((WDFOBJECT)m_hSwapChain);
			logStream.str("");
			logStream << "Swap-chain object deleted successfully.";
			vddlog("d", logStream.str().c_str());
			m_hSwapChain = nullptr;
		}
		catch (const std::exception& e)
		{
			stringstream errorStream;
			errorStream << "Exception while deleting swap-chain: " << e.what();
			vddlog("e", errorStream.str().c_str());
			m_hSwapChain = nullptr; // Mark as null to prevent further access
		}
		catch (...)
		{
			vddlog("e", "Unknown exception while deleting swap-chain");
			m_hSwapChain = nullptr; // Mark as null to prevent further access
		}
	}
	else
	{
		logStream.str("");
		logStream << "No valid swap-chain object to delete.";
		vddlog("d", logStream.str().c_str());
	}
	/*
	AvRevertMmThreadCharacteristics(AvTaskHandle);
	*/
	// error handling when reversing multimedia thread characteristics
	if (AvRevertMmThreadCharacteristics(AvTaskHandle))
	{
		logStream.str("");
		logStream << "Multimedia thread characteristics reverted successfully.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Failed to revert multimedia thread characteristics. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
}

void SwapChainProcessor::RunCore()
{
	stringstream logStream;

	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		logStream << "Failed to get DXGI device interface. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
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
			logStream.str("");
			logStream << "Failed to set device to swap chain. HRESULT: " << hr;
			vddlog("e", logStream.str().c_str());
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
			logStream.str("");
			logStream << "IddCxSetRealtimeGPUPriority failed (non-fatal). HRESULT: 0x" << hex << hr;
			vddlog("w", logStream.str().c_str());
		}
		else
		{
			vddlog("d", "GPU priority raised to realtime for swap chain processing");
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

		if (useBuffer2)
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
			presentDisplayQpc = Buffer.MetaData.PresentDisplayQPCTime;
			presentationFrameNumber = Buffer.MetaData.PresentationFrameNumber;
			dirtyRectCount = Buffer.MetaData.DirtyRectCount;
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
				// Only build log message when actually needed (error case)
				logStream.str("");
				logStream << "Unexpected wait result. HRESULT: " << hr;
				vddlog("e", logStream.str().c_str());
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
				logStream.str("");
				logStream << "Failed to acquire buffer. Exiting loop. HRESULT: " << hr;
				vddlog("e", logStream.str().c_str());
			}
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

}
}
