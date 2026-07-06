#include "ControlTransport.h"

#include "CommandDispatcher.h"
#include "..\Device\IndirectDeviceContextWrapper.h"
#include "..\Logging\Logger.h"
#include "..\Util\StringConversion.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <sddl.h>
#include <sstream>
#include <string>
#include <vdd_control_ioctl.h>

#define PIPE_NAME L"\\\\.\\pipe\\ZakoVDDPipe"

using namespace std;

HANDLE hPipeThread = NULL;
std::atomic<bool> g_Running{true};
HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;

extern std::mutex g_Mutex;

struct TargetProcessOpenContext
{
	DWORD pid = 0;
	HANDLE handle = nullptr;
	DWORD error = 0;
};

static void OpenTargetProcessAsRequestor(WDFREQUEST Request, PVOID Context)
{
	UNREFERENCED_PARAMETER(Request);

	auto* openContext = static_cast<TargetProcessOpenContext*>(Context);
	if (!openContext || openContext->pid == 0)
	{
		return;
	}

	openContext->handle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, openContext->pid);
	if (!openContext->handle)
	{
		openContext->error = GetLastError();
	}
}

static NTSTATUS HandleQueryFrameChannelCaps(WDFREQUEST Request)
{
	PVOID pOutBuffer = nullptr;
	size_t outBufferLen = 0;
	NTSTATUS status = WdfRequestRetrieveOutputBuffer(
	    Request, sizeof(VDD_FRAME_CHANNEL_CAPS), &pOutBuffer, &outBufferLen);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	if (outBufferLen < sizeof(VDD_FRAME_CHANNEL_CAPS))
	{
		return STATUS_BUFFER_TOO_SMALL;
	}

	auto caps = Microsoft::IndirectDisp::SharedFrameExporter::FrameChannelCaps();
	RtlCopyMemory(pOutBuffer, &caps, sizeof(caps));
	WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(caps));
	return STATUS_SUCCESS;
}

static NTSTATUS HandleOpenFrameChannel(WDFDEVICE Device,
                                       WDFREQUEST Request,
                                       size_t InputBufferLength)
{
	if (InputBufferLength < sizeof(VDD_FRAME_CHANNEL_OPEN_REQUEST))
	{
		return STATUS_INVALID_BUFFER_SIZE;
	}

	PVOID pInBuffer = nullptr;
	size_t inBufferLen = 0;
	NTSTATUS status = WdfRequestRetrieveInputBuffer(
	    Request, sizeof(VDD_FRAME_CHANNEL_OPEN_REQUEST), &pInBuffer, &inBufferLen);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	PVOID pOutBuffer = nullptr;
	size_t outBufferLen = 0;
	status = WdfRequestRetrieveOutputBuffer(
	    Request, sizeof(VDD_FRAME_CHANNEL_OPEN_RESPONSE), &pOutBuffer, &outBufferLen);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	if (outBufferLen < sizeof(VDD_FRAME_CHANNEL_OPEN_RESPONSE))
	{
		return STATUS_BUFFER_TOO_SMALL;
	}

	auto request = *static_cast<VDD_FRAME_CHANNEL_OPEN_REQUEST*>(pInBuffer);
	if (request.Size < sizeof(VDD_FRAME_CHANNEL_OPEN_REQUEST) ||
	    request.Version != VDD_FRAME_CHANNEL_OPEN_VERSION)
	{
		return STATUS_INVALID_PARAMETER;
	}

	if (request.TargetProcessId == 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (!pContext || !pContext->pContext)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	TargetProcessOpenContext openContext = {};
	openContext.pid = request.TargetProcessId;
	// Open the target process while impersonating the IOCTL caller. A caller can
	// only receive duplicated frame-channel handles for a process it can open.
	status = WdfRequestImpersonate(Request, SecurityImpersonation, OpenTargetProcessAsRequestor, &openContext);
	if (!NT_SUCCESS(status))
	{
		VDD_LOG_WARNING_STREAM("[IOCTL] WdfRequestImpersonate failed for frame channel open: 0x"
		                       << std::hex << status);
		return status;
	}
	if (!openContext.handle)
	{
		VDD_LOG_WARNING_STREAM("[IOCTL] OpenProcess(PROCESS_DUP_HANDLE) failed for frame channel pid="
		                       << request.TargetProcessId << " gle=" << openContext.error);
		return STATUS_ACCESS_DENIED;
	}

	VDD_FRAME_CHANNEL_OPEN_RESPONSE response = {};
	status = pContext->pContext->OpenFrameChannel(request, openContext.handle, response);
	CloseHandle(openContext.handle);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	RtlCopyMemory(pOutBuffer, &response, sizeof(response));
	WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(response));
	return STATUS_SUCCESS;
}

void SendLegacyPipeMessage(const char *message)
{
	if (message == nullptr || g_pipeHandle == INVALID_HANDLE_VALUE)
	{
		return;
	}

	DWORD bytesWritten = 0;
	DWORD bytesToWrite = static_cast<DWORD>(strlen(message));
	WriteFile(g_pipeHandle, message, bytesToWrite, &bytesWritten, NULL);
}

// [LEGACY-PIPE] entire function -- pipe-side wrapper around DispatchVddCommandBuffer
void HandleClient(HANDLE hPipe)
{
	g_pipeHandle = hPipe;
	VDD_LOG_VERBOSE("Client Handling Enabled");
	wchar_t buffer[2048];
	DWORD bytesRead;
	BOOL result = ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, NULL);
	if (result && bytesRead != 0)
	{
		buffer[bytesRead / sizeof(wchar_t)] = L'\0';
		wstring bufferwstr(buffer);
		string bufferstr = WStringToString(bufferwstr);
		VDD_LOG_VERBOSE(bufferstr.c_str());

		DispatchVddCommandBuffer(hPipe, buffer);
	}
	DisconnectNamedPipe(hPipe);
	CloseHandle(hPipe);
	g_pipeHandle = INVALID_HANDLE_VALUE;
}

// IddCx redirects every IRP_MJ_DEVICE_CONTROL into its own internal queue
// before any default WDF queue ever sees it. The only way to receive a
// custom IOCTL in an IddCx driver is through this callback registered via
// IDD_CX_CLIENT_CONFIG.EvtIddCxDeviceIoControl. IddCx invokes this hook
// for IOCTLs it does not own; we recognise IOCTL_VDD_PING and
// IOCTL_VDD_COMMAND, and fall through with STATUS_NOT_SUPPORTED for
// everything else so unknown control codes don't hang the request queue.
_Use_decl_annotations_
VOID VirtualDisplayDriverIoDeviceControl(
	WDFDEVICE Device,
	WDFREQUEST Request,
	size_t OutputBufferLength,
	size_t InputBufferLength,
	ULONG IoControlCode)
{
	UNREFERENCED_PARAMETER(OutputBufferLength);

	switch (IoControlCode)
	{
	case IOCTL_VDD_PING:
	{
		// Cheap "is the driver alive" probe used by Sunshine to decide
		// whether to short-circuit to disable_enable instead of waiting on
		// a slow command IOCTL.
		WdfRequestComplete(Request, STATUS_SUCCESS);
		return;
	}

	case IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS:
	{
		NTSTATUS status = HandleQueryFrameChannelCaps(Request);
		if (!NT_SUCCESS(status))
		{
			WdfRequestComplete(Request, status);
		}
		return;
	}

	case IOCTL_VDD_OPEN_FRAME_CHANNEL:
	{
		NTSTATUS status = HandleOpenFrameChannel(Device, Request, InputBufferLength);
		if (!NT_SUCCESS(status))
		{
			WdfRequestComplete(Request, status);
		}
		return;
	}

	case IOCTL_VDD_COMMAND:
	{
		if (InputBufferLength == 0 || (InputBufferLength % sizeof(wchar_t)) != 0)
		{
			WdfRequestComplete(Request, STATUS_INVALID_BUFFER_SIZE);
			return;
		}

		// Mirror the legacy named-pipe HandleClient buffer (2048 wchar_t).
		// Anything larger is almost certainly malformed input.
		if (InputBufferLength > 2048 * sizeof(wchar_t))
		{
			WdfRequestComplete(Request, STATUS_BUFFER_OVERFLOW);
			return;
		}

		PVOID pInBuffer = nullptr;
		size_t inBufferLen = 0;
		NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(wchar_t), &pInBuffer, &inBufferLen);
		if (!NT_SUCCESS(status))
		{
			WdfRequestComplete(Request, status);
			return;
		}

		// Copy into a writable, NUL-terminated local buffer. METHOD_BUFFERED
		// already gives us a kernel-owned copy but the dispatch helpers
		// expect a wchar_t array they can scribble on (e.g. swscanf_s).
		wchar_t buffer[2048] = { 0 };
		size_t copyLen = inBufferLen;
		if (copyLen > sizeof(buffer) - sizeof(wchar_t))
		{
			copyLen = sizeof(buffer) - sizeof(wchar_t);
		}
		RtlCopyMemory(buffer, pInBuffer, copyLen);
		buffer[copyLen / sizeof(wchar_t)] = L'\0';

		try
		{
			wstring bufferwstr(buffer);
			string bufferstr = WStringToString(bufferwstr);
			VDD_LOG_VERBOSE(("[IOCTL] " + bufferstr).c_str());

			// Pass INVALID_HANDLE_VALUE so response-emitting handlers
			// (GETSETTINGS / PING) silently skip their WriteFile path.
			// Sunshine never observes those responses anyway.
			DispatchVddCommandBuffer(INVALID_HANDLE_VALUE, buffer);
		}
		catch (const std::exception &e)
		{
			VDD_LOG_ERROR_STREAM("Exception during IOCTL command dispatch: " << e.what());
		}
		catch (...)
		{
			VDD_LOG_ERROR("Unknown exception during IOCTL command dispatch");
		}

		WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
		return;
	}

	default:
		WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
		return;
	}
}


// [LEGACY-PIPE] entire function -- accept-loop thread for the named pipe
DWORD WINAPI NamedPipeServer(LPVOID lpParam)
{
	UNREFERENCED_PARAMETER(lpParam);

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = FALSE;
	const wchar_t *sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
	VDD_LOG_DEBUG("Starting pipe with parameters: D:(A;;GA;;;SY)(A;;GA;;;BA)");
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
	{
		DWORD ErrorCode = GetLastError();
		string errorMsg = to_string(ErrorCode);
		VDD_LOG_ERROR(errorMsg.c_str());
		return 1;
	}
	HANDLE hPipe;
	while (g_Running)
	{
		hPipe = CreateNamedPipeW(
			PIPE_NAME,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES,
			512, 512,
			0,
			&sa);

		if (hPipe == INVALID_HANDLE_VALUE)
		{
			DWORD ErrorCode = GetLastError();
			string errorMsg = to_string(ErrorCode);
			VDD_LOG_ERROR(errorMsg.c_str());
			LocalFree(sa.lpSecurityDescriptor);
			return 1;
		}

		BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (connected)
		{
			VDD_LOG_VERBOSE("Client Connected");
			HandleClient(hPipe);
		}
		else
		{
			CloseHandle(hPipe);
		}
	}
	LocalFree(sa.lpSecurityDescriptor);
	return 0;
}

// [LEGACY-PIPE] entire function
void StartNamedPipeServer()
{
	VDD_LOG_VERBOSE("Starting Pipe");
	hPipeThread = CreateThread(NULL, 0, NamedPipeServer, NULL, 0, NULL);
	if (hPipeThread == NULL)
	{
		DWORD ErrorCode = GetLastError();
		string errorMsg = to_string(ErrorCode);
		VDD_LOG_ERROR(errorMsg.c_str());
	}
	else
	{
		VDD_LOG_VERBOSE("Pipe created");
	}
}

// [LEGACY-PIPE] entire function
void StopNamedPipeServer()
{
	VDD_LOG_VERBOSE("Stopping Pipe");
	{
		lock_guard<mutex> lock(g_Mutex);
		g_Running = false;
	}
	if (hPipeThread)
	{
		HANDLE hPipe = CreateFileW(
			PIPE_NAME,
			GENERIC_READ | GENERIC_WRITE,
			0,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);

		if (hPipe != INVALID_HANDLE_VALUE)
		{
			DisconnectNamedPipe(hPipe);
			CloseHandle(hPipe);
		}

		WaitForSingleObject(hPipeThread, INFINITE);
		CloseHandle(hPipeThread);
		hPipeThread = NULL;
		VDD_LOG_VERBOSE("Stopped Pipe");
	}
}

// 优先从环境变量读取配置路径，如果没有则回退到注册表读取
