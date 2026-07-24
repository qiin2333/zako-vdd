#include "CursorExporter.h"

#include "..\Logging\Logger.h"

#include <cstring>
#include <limits>
#include <sddl.h>
#include <sstream>
#include <string>

namespace Microsoft
{
namespace IndirectDisp
{

namespace
{

static constexpr UINT32 DEFAULT_SDR_WHITE_LEVEL_NITS = 80u;

enum class CursorQueryApi
{
	Query1,
	Query2,
	Query3
};

struct CursorQueryResult
{
	BOOL IsCursorVisible = FALSE;
	INT X = 0;
	INT Y = 0;
	BOOL IsCursorShapeUpdated = FALSE;
	IDDCX_CURSOR_SHAPE_INFO CursorShapeInfo = {};
	BOOL PositionValid = FALSE;
	UINT PositionId = 0;
	UINT SdrWhiteLevelNits = DEFAULT_SDR_WHITE_LEVEL_NITS;
};

template <typename T>
void CopyCommonCursorResult(const T& source, CursorQueryResult& destination)
{
	destination.IsCursorVisible = source.IsCursorVisible;
	destination.X = source.X;
	destination.Y = source.Y;
	destination.IsCursorShapeUpdated = source.IsCursorShapeUpdated;
	destination.CursorShapeInfo = source.CursorShapeInfo;
}

CursorQueryApi SelectCursorQueryApi()
{
	if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3))
	{
		return CursorQueryApi::Query3;
	}
	if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor2))
	{
		return CursorQueryApi::Query2;
	}
	return CursorQueryApi::Query1;
}

const char* CursorQueryApiName(CursorQueryApi api)
{
	switch (api)
	{
	case CursorQueryApi::Query3:
		return "IddCxMonitorQueryHardwareCursor3";
	case CursorQueryApi::Query2:
		return "IddCxMonitorQueryHardwareCursor2";
	default:
		return "IddCxMonitorQueryHardwareCursor";
	}
}

bool QueryHardwareCursor(CursorQueryApi api,
	                     IDDCX_MONITOR monitor,
	                     const IDARG_IN_QUERY_HWCURSOR& inArgs,
	                     INT lastX,
	                     INT lastY,
	                     UINT lastPositionId,
	                     CursorQueryResult& result)
{
	switch (api)
	{
	case CursorQueryApi::Query3:
	{
		IDARG_OUT_QUERY_HWCURSOR3 outArgs = {};
		const HRESULT status = IddCxMonitorQueryHardwareCursor3(monitor, &inArgs, &outArgs);
		if (FAILED(status))
		{
			return false;
		}
		CopyCommonCursorResult(outArgs, result);
		result.PositionValid = outArgs.PositionValid;
		result.PositionId = outArgs.PositionId;
		result.SdrWhiteLevelNits = outArgs.SdrWhiteLevel;
		return true;
	}
	case CursorQueryApi::Query2:
	{
		IDARG_OUT_QUERY_HWCURSOR2 outArgs = {};
		const NTSTATUS status = IddCxMonitorQueryHardwareCursor2(monitor, &inArgs, &outArgs);
		if (!NT_SUCCESS(status))
		{
			return false;
		}
		CopyCommonCursorResult(outArgs, result);
		result.PositionValid = outArgs.PositionValid;
		result.PositionId = outArgs.PositionId;
		return true;
	}
	default:
	{
		IDARG_OUT_QUERY_HWCURSOR outArgs = {};
		const NTSTATUS status = IddCxMonitorQueryHardwareCursor(monitor, &inArgs, &outArgs);
		if (!NT_SUCCESS(status))
		{
			return false;
		}
		CopyCommonCursorResult(outArgs, result);
		result.PositionValid = outArgs.IsCursorVisible;
		result.PositionId = lastPositionId;
		if (result.PositionValid && (outArgs.X != lastX || outArgs.Y != lastY))
		{
			result.PositionId++;
		}
		return true;
	}
	}
}

UINT32 ScaleSdrWhiteLevel(UINT sdrWhiteLevelNits)
{
	constexpr UINT32 scale = 1000u;
	if (sdrWhiteLevelNits > std::numeric_limits<UINT32>::max() / scale)
	{
		return std::numeric_limits<UINT32>::max();
	}
	return static_cast<UINT32>(sdrWhiteLevelNits) * scale;
}

const wchar_t* CursorMapName(unsigned int monitorIndex)
{
	static thread_local std::wstring name;
	name = L"Global\\ZakoVDD_CursorMeta_" + std::to_wstring(monitorIndex);
	return name.c_str();
}

const wchar_t* CursorReadyEventName(unsigned int monitorIndex)
{
	static thread_local std::wstring name;
	name = L"Global\\ZakoVDD_CursorReady_" + std::to_wstring(monitorIndex);
	return name.c_str();
}

}

CursorExporter::CursorExporter(unsigned int monitorIndex, IDDCX_MONITOR monitor, HANDLE hNewCursorDataAvailable)
	: m_MonitorIndex(monitorIndex), m_Monitor(monitor), m_hCursorDataAvailable(hNewCursorDataAvailable)
{
}

CursorExporter::~CursorExporter()
{
	Stop();
	Teardown();
}

bool CursorExporter::Start()
{
	if (!m_Monitor || !m_hCursorDataAvailable)
	{
		VDD_LOG_WARNING("[VddCursor] Start skipped: monitor or cursor event not provided");
		return false;
	}

	if (!EnsureSharedObjects())
	{
		VDD_LOG_ERROR("[VddCursor] Failed to create cursor shared-memory objects");
		return false;
	}

	m_hTerminateEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!m_hTerminateEvent)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] CreateEvent (terminate) failed: " << GetLastError());
		return false;
	}

	m_hThread = CreateThread(nullptr, 0, &CursorExporter::ThreadProc, this, 0, nullptr);
	if (!m_hThread)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] CreateThread failed: " << GetLastError());
		CloseHandle(m_hTerminateEvent);
		m_hTerminateEvent = nullptr;
		return false;
	}

	VDD_LOG_INFO_STREAM("[VddCursor] Started monitor=" << m_MonitorIndex);
	return true;
}

void CursorExporter::Stop()
{
	if (m_hTerminateEvent)
	{
		SetEvent(m_hTerminateEvent);
	}

	if (m_hThread)
	{
		// The worker polls with a 33 ms fallback, so it must stop promptly. Wait
		// for it before releasing any state it can still access.
		const DWORD waitResult = WaitForSingleObject(m_hThread, INFINITE);
		if (waitResult != WAIT_OBJECT_0)
		{
			VDD_LOG_ERROR_STREAM("[VddCursor] Worker wait failed: " << waitResult);
		}
		CloseHandle(m_hThread);
		m_hThread = nullptr;
	}

	if (m_hTerminateEvent)
	{
		CloseHandle(m_hTerminateEvent);
		m_hTerminateEvent = nullptr;
	}
}

DWORD WINAPI CursorExporter::ThreadProc(LPVOID arg)
{
	static_cast<CursorExporter*>(arg)->Run();
	return 0;
}

void CursorExporter::Run()
{
	std::vector<BYTE> shapeBuffer(VDD_CURSOR_MAX_BYTES);
	UINT lastShapeId = 0;
	UINT lastPositionId = 0;
	const CursorQueryApi queryApi = SelectCursorQueryApi();

	VDD_LOG_INFO_STREAM("[VddCursor] Cursor query API: " << CursorQueryApiName(queryApi));

	HANDLE waits[2] = {m_hCursorDataAvailable, m_hTerminateEvent};

	for (;;)
	{
		const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, 33);
		if (waitResult == WAIT_OBJECT_0 + 1)
		{
			break;
		}
		if (waitResult == WAIT_FAILED)
		{
			VDD_LOG_ERROR_STREAM("[VddCursor] WaitForMultipleObjects failed: " << GetLastError());
			break;
		}

		IDARG_IN_QUERY_HWCURSOR inArgs = {};
		inArgs.LastShapeId = lastShapeId;
		inArgs.ShapeBufferSizeInBytes = static_cast<UINT>(shapeBuffer.size());
		inArgs.pShapeBuffer = shapeBuffer.data();

		CursorQueryResult queryResult = {};
		if (!QueryHardwareCursor(queryApi,
		                         m_Monitor,
		                         inArgs,
		                         m_LastX,
		                         m_LastY,
		                         lastPositionId,
		                         queryResult))
		{
			continue;
		}

		const bool shapeUpdated = queryResult.IsCursorShapeUpdated != FALSE;
		if (shapeUpdated)
		{
			lastShapeId = queryResult.CursorShapeInfo.ShapeId;
			m_CachedShape.ShapeId = queryResult.CursorShapeInfo.ShapeId;
			m_CachedShape.Type = static_cast<UINT32>(queryResult.CursorShapeInfo.CursorType);
			m_CachedShape.Width = queryResult.CursorShapeInfo.Width;
			m_CachedShape.Height = queryResult.CursorShapeInfo.Height;
			m_CachedShape.Pitch = queryResult.CursorShapeInfo.Pitch;
			m_CachedShape.XHot = static_cast<INT32>(queryResult.CursorShapeInfo.XHot);
			m_CachedShape.YHot = static_cast<INT32>(queryResult.CursorShapeInfo.YHot);

			const UINT64 needed = static_cast<UINT64>(queryResult.CursorShapeInfo.Pitch) *
			                      static_cast<UINT64>(queryResult.CursorShapeInfo.Height);
			m_CachedShape.BufferSize = needed <= VDD_CURSOR_MAX_BYTES ? static_cast<UINT32>(needed) : 0;
			if (m_CachedShape.BufferSize > 0)
			{
				m_CachedShape.Buffer.assign(shapeBuffer.begin(), shapeBuffer.begin() + m_CachedShape.BufferSize);
			}
			else
			{
				m_CachedShape.Buffer.clear();
				if (needed > VDD_CURSOR_MAX_BYTES)
				{
					VDD_LOG_WARNING_STREAM("[VddCursor] Cursor shape exceeds shared buffer: " << needed
					                       << " > " << VDD_CURSOR_MAX_BYTES);
				}
			}
		}

		// IddCx reports the desktop-relative top-left of the cursor image.
		// The hot spot has already been applied; do not subtract it again.
		bool positionUpdated = false;
		if (queryResult.PositionValid)
		{
			if (queryResult.PositionId != lastPositionId)
			{
				lastPositionId = queryResult.PositionId;
				positionUpdated = true;
			}
			m_LastX = queryResult.X;
			m_LastY = queryResult.Y;
		}

		const UINT32 visibility = queryResult.IsCursorVisible ? 1u : 0u;
		const bool visibilityChanged = m_LastVisibility != visibility;
		if (!shapeUpdated && !positionUpdated && !visibilityChanged)
		{
			continue;
		}
		m_LastVisibility = visibility;

		if (!m_MetaView)
		{
			continue;
		}

		LARGE_INTEGER qpc = {};
		QueryPerformanceCounter(&qpc);

		VDD_CURSOR_SHARED_METADATA* dst = m_MetaView;
		volatile LONG* sequence = reinterpret_cast<volatile LONG*>(&dst->PublicationSequence);
		// There is one producer per monitor. Setting the low bit also recovers an
		// interrupted publication that left an existing mapping at an odd value.
		const ULONG previousSequence = static_cast<ULONG>(InterlockedOr(sequence, 1));
		const LONG stableSequence = static_cast<LONG>((previousSequence | 1u) + 1u);
		MemoryBarrier();

		if (shapeUpdated && m_CachedShape.BufferSize > 0)
		{
			BYTE* shapeDst = reinterpret_cast<BYTE*>(dst + 1);
			std::memcpy(shapeDst, m_CachedShape.Buffer.data(), m_CachedShape.BufferSize);
		}

		dst->Version = VDD_CURSOR_VERSION;
		dst->IsVisible = visibility;
		if (queryResult.PositionValid)
		{
			dst->PositionX = m_LastX;
			dst->PositionY = m_LastY;
		}
		dst->SdrWhiteLevelX1000 = ScaleSdrWhiteLevel(queryResult.SdrWhiteLevelNits);
		dst->LastUpdateQpc = static_cast<UINT64>(qpc.QuadPart);

		if (shapeUpdated)
		{
			dst->ShapeType = m_CachedShape.Type;
			dst->Width = m_CachedShape.Width;
			dst->Height = m_CachedShape.Height;
			dst->Pitch = m_CachedShape.Pitch;
			dst->XHot = m_CachedShape.XHot;
			dst->YHot = m_CachedShape.YHot;
			dst->ShapeBufferSize = m_CachedShape.BufferSize;
		}

		MemoryBarrier();
		if (shapeUpdated)
		{
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&dst->ShapeId),
			                    static_cast<LONG>(m_CachedShape.ShapeId));
		}
		if (positionUpdated)
		{
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&dst->PositionId),
			                    static_cast<LONG>(lastPositionId));
		}

		InterlockedExchange(reinterpret_cast<volatile LONG*>(&dst->Magic),
		                    static_cast<LONG>(VDD_CURSOR_MAGIC));
		MemoryBarrier();
		InterlockedExchange(sequence, stableSequence); // Even: the snapshot is stable.

		if (m_hCursorReadyEvent)
		{
			SetEvent(m_hCursorReadyEvent);
		}
	}

	VDD_LOG_DEBUG_STREAM("[VddCursor] Worker exiting monitor=" << m_MonitorIndex);
}

bool CursorExporter::EnsureSharedObjects()
{
	SECURITY_ATTRIBUTES mappingSa = {};
	PSECURITY_DESCRIPTOR mappingSd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00000004;;;IU)",
	        SDDL_REVISION_1,
	        &mappingSd,
	        nullptr))
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] Failed to build mapping SDDL: " << GetLastError());
		return false;
	}

	mappingSa.nLength = sizeof(mappingSa);
	mappingSa.lpSecurityDescriptor = mappingSd;
	mappingSa.bInheritHandle = FALSE;

	const SIZE_T mapSize = sizeof(VDD_CURSOR_SHARED_METADATA) + VDD_CURSOR_MAX_BYTES;
	m_MetaMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &mappingSa, PAGE_READWRITE, 0,
	                                   static_cast<DWORD>(mapSize), CursorMapName(m_MonitorIndex));
	const DWORD mappingError = GetLastError();
	LocalFree(mappingSd);
	if (!m_MetaMapping)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] CreateFileMappingW failed: " << mappingError);
		return false;
	}
	const bool mappingAlreadyExists = mappingError == ERROR_ALREADY_EXISTS;

	m_MetaView = static_cast<VDD_CURSOR_SHARED_METADATA*>(MapViewOfFile(m_MetaMapping, FILE_MAP_WRITE, 0, 0, mapSize));
	if (!m_MetaView)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] MapViewOfFile failed: " << GetLastError());
		return false;
	}
	if (!mappingAlreadyExists)
	{
		ZeroMemory(m_MetaView, mapSize);
		m_MetaView->Version = VDD_CURSOR_VERSION;
	}

	SECURITY_ATTRIBUTES eventSa = {};
	PSECURITY_DESCRIPTOR eventSd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;IU)",
	        SDDL_REVISION_1,
	        &eventSd,
	        nullptr))
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] Failed to build event SDDL: " << GetLastError());
		return false;
	}

	eventSa.nLength = sizeof(eventSa);
	eventSa.lpSecurityDescriptor = eventSd;
	eventSa.bInheritHandle = FALSE;

	m_hCursorReadyEvent = CreateEventW(&eventSa, FALSE, FALSE, CursorReadyEventName(m_MonitorIndex));
	const DWORD eventError = GetLastError();
	LocalFree(eventSd);
	if (!m_hCursorReadyEvent)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] CreateEventW (cursor ready) failed: " << eventError);
		return false;
	}

	VDD_LOG_INFO_STREAM("[VddCursor] Shared objects ready monitor=" << m_MonitorIndex << " bytes=" << mapSize);
	return true;
}

void CursorExporter::Teardown()
{
	if (m_MetaView)
	{
		UnmapViewOfFile(m_MetaView);
		m_MetaView = nullptr;
	}
	if (m_MetaMapping)
	{
		CloseHandle(m_MetaMapping);
		m_MetaMapping = nullptr;
	}
	if (m_hCursorReadyEvent)
	{
		CloseHandle(m_hCursorReadyEvent);
		m_hCursorReadyEvent = nullptr;
	}
	// m_hCursorDataAvailable belongs to IndirectDeviceContext.
}

}
}
