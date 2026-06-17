#include "ControlTransport.h"

#include "CommandDispatcher.h"
#include "..\Logging\Logger.h"
#include "..\Util\StringConversion.h"

#include <atomic>
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

// [LEGACY-PIPE] entire function -- pipe-side wrapper around DispatchVddCommandBuffer
void HandleClient(HANDLE hPipe)
{
	g_pipeHandle = hPipe;
	vddlog("p", "Client Handling Enabled");
	wchar_t buffer[2048];
	DWORD bytesRead;
	BOOL result = ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, NULL);
	if (result && bytesRead != 0)
	{
		buffer[bytesRead / sizeof(wchar_t)] = L'\0';
		wstring bufferwstr(buffer);
		string bufferstr = WStringToString(bufferwstr);
		vddlog("p", bufferstr.c_str());

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
	UNREFERENCED_PARAMETER(Device);
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
			vddlog("p", ("[IOCTL] " + bufferstr).c_str());

			// Pass INVALID_HANDLE_VALUE so response-emitting handlers
			// (GETSETTINGS / PING) silently skip their WriteFile path.
			// Sunshine never observes those responses anyway.
			DispatchVddCommandBuffer(INVALID_HANDLE_VALUE, buffer);
		}
		catch (const std::exception &e)
		{
			stringstream errorStream;
			errorStream << "Exception during IOCTL command dispatch: " << e.what();
			vddlog("e", errorStream.str().c_str());
		}
		catch (...)
		{
			vddlog("e", "Unknown exception during IOCTL command dispatch");
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
	vddlog("d", "Starting pipe with parameters: D:(A;;GA;;;SY)(A;;GA;;;BA)");
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
	{
		DWORD ErrorCode = GetLastError();
		string errorMsg = to_string(ErrorCode);
		vddlog("e", errorMsg.c_str());
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
			vddlog("e", errorMsg.c_str());
			LocalFree(sa.lpSecurityDescriptor);
			return 1;
		}

		BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (connected)
		{
			vddlog("p", "Client Connected");
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
	vddlog("p", "Starting Pipe");
	hPipeThread = CreateThread(NULL, 0, NamedPipeServer, NULL, 0, NULL);
	if (hPipeThread == NULL)
	{
		DWORD ErrorCode = GetLastError();
		string errorMsg = to_string(ErrorCode);
		vddlog("e", errorMsg.c_str());
	}
	else
	{
		vddlog("p", "Pipe created");
	}
}

// [LEGACY-PIPE] entire function
void StopNamedPipeServer()
{
	vddlog("p", "Stopping Pipe");
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
		vddlog("p", "Stopped Pipe");
	}
}

// 优先从环境变量读取配置路径，如果没有则回退到注册表读取
