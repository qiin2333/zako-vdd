#include "ControlTransport.h"

#include "CommandDispatcher.h"
#include "..\Device\IndirectDeviceContextWrapper.h"
#include "..\Logging\Logger.h"
#include "..\Util\StringConversion.h"

#include <sstream>
#include <string>
#include <vdd_control_ioctl.h>

using namespace std;

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
	const ULONG requestorProcessId = WdfRequestGetRequestorProcessId(Request);
	if (requestorProcessId == 0 || request.TargetProcessId != requestorProcessId)
	{
		VDD_LOG_WARNING_STREAM("[IOCTL] Rejected frame channel target pid="
		                       << request.TargetProcessId << " for requestor pid=" << requestorProcessId);
		return STATUS_ACCESS_DENIED;
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

		// Commands are deliberately capped at 2048 wchar_t. Anything larger
		// is almost certainly malformed input.
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

			DispatchVddCommandBuffer(buffer);
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

// 优先从环境变量读取配置路径，如果没有则回退到注册表读取
