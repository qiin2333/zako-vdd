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

constexpr UINT32 ToSharedCursorShapeType(IDDCX_CURSOR_SHAPE_TYPE type)
{
	switch (type)
	{
	case IDDCX_CURSOR_SHAPE_TYPE_ALPHA:
		return VDD_CURSOR_SHAPE_COLOR;
	case IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR:
		return VDD_CURSOR_SHAPE_MASKED_COLOR;
	default:
		return VDD_CURSOR_SHAPE_MONOCHROME;
	}
}

static_assert(ToSharedCursorShapeType(IDDCX_CURSOR_SHAPE_TYPE_ALPHA) == VDD_CURSOR_SHAPE_COLOR);
static_assert(ToSharedCursorShapeType(IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR) == VDD_CURSOR_SHAPE_MASKED_COLOR);

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

HRESULT QueryHardwareCursor(CursorQueryApi api,
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
			return status;
		}
		CopyCommonCursorResult(outArgs, result);
		result.PositionValid = outArgs.PositionValid;
		result.PositionId = outArgs.PositionId;
		result.SdrWhiteLevelNits = outArgs.SdrWhiteLevel;
		return S_OK;
	}
	case CursorQueryApi::Query2:
	{
		IDARG_OUT_QUERY_HWCURSOR2 outArgs = {};
		const NTSTATUS status = IddCxMonitorQueryHardwareCursor2(monitor, &inArgs, &outArgs);
		if (!NT_SUCCESS(status))
		{
			return status;
		}
		CopyCommonCursorResult(outArgs, result);
		result.PositionValid = outArgs.PositionValid;
		result.PositionId = outArgs.PositionId;
		return S_OK;
	}
	default:
	{
		IDARG_OUT_QUERY_HWCURSOR outArgs = {};
		const NTSTATUS status = IddCxMonitorQueryHardwareCursor(monitor, &inArgs, &outArgs);
		if (!NT_SUCCESS(status))
		{
			return status;
		}
		CopyCommonCursorResult(outArgs, result);
		result.PositionValid = outArgs.IsCursorVisible;
		result.PositionId = lastPositionId;
		if (result.PositionValid && (outArgs.X != lastX || outArgs.Y != lastY))
		{
			result.PositionId++;
		}
		return S_OK;
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

	// No worker thread: Poll() is invoked by the swap-chain processing thread
	// whenever IddCx signals the cursor data-available event. See issue #8 -
	// a dedicated cursor thread can outlive swap-chain unassignment and call
	// IddCx with a monitor whose swap chain is already gone.
	VDD_LOG_INFO_STREAM("[VddCursor] Ready monitor=" << m_MonitorIndex);
	return true;
}

HRESULT CursorExporter::Poll()
{
	if (!m_MetaView || !m_Monitor)
	{
		return E_FAIL;
	}

	if (m_ShapeBuffer.empty())
	{
		m_ShapeBuffer.resize(VDD_CURSOR_MAX_BYTES);
	}

	const CursorQueryApi queryApi = SelectCursorQueryApi();

	CursorQueryResult queryResult = {};

	// Bounded retry for transient query failures. Reconnect / teardown /
	// re-init transitions can briefly report the path as not in topology, and
	// the old 33 ms polling no longer hides that: if no further cursor data
	// arrives IddCx will not re-signal the event, so a single-shot failure
	// would leave the shared mapping stale forever. A handful of short
	// retries is safe here because the caller is the swap-chain processing
	// thread, which guarantees the swap chain is still assigned.
	constexpr UINT CursorQueryRetryCount = 3;
	constexpr DWORD CursorQueryRetryDelayMs = 20;

	IDARG_IN_QUERY_HWCURSOR queryInArgs = {};
	auto PrepareQueryArgs = [&]()
	{
		queryInArgs = {};
		queryInArgs.LastShapeId = m_LastShapeId;
		queryInArgs.ShapeBufferSizeInBytes = static_cast<UINT>(m_ShapeBuffer.size());
		queryInArgs.pShapeBuffer = m_ShapeBuffer.data();
	};

	PrepareQueryArgs();
	HRESULT queryStatus = QueryHardwareCursor(queryApi,
	                                          m_Monitor,
	                                          queryInArgs,
	                                          m_LastX,
	                                          m_LastY,
	                                          m_LastPositionId,
	                                          queryResult);
	for (UINT attempt = 1; FAILED(queryStatus) && attempt < CursorQueryRetryCount; ++attempt)
	{
		Sleep(CursorQueryRetryDelayMs);
		PrepareQueryArgs();
		queryStatus = QueryHardwareCursor(queryApi,
		                                  m_Monitor,
		                                  queryInArgs,
		                                  m_LastX,
		                                  m_LastY,
		                                  m_LastPositionId,
		                                  queryResult);
	}

	if (FAILED(queryStatus))
	{
		return queryStatus;
	}

	const bool shapeUpdated = queryResult.IsCursorShapeUpdated != FALSE;
	if (shapeUpdated)
	{
		m_LastShapeId = queryResult.CursorShapeInfo.ShapeId;
		m_CachedShape.ShapeId = queryResult.CursorShapeInfo.ShapeId;
		m_CachedShape.Type = ToSharedCursorShapeType(queryResult.CursorShapeInfo.CursorType);
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
			m_CachedShape.Buffer.assign(m_ShapeBuffer.begin(), m_ShapeBuffer.begin() + m_CachedShape.BufferSize);
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
		if (queryResult.PositionId != m_LastPositionId)
		{
			m_LastPositionId = queryResult.PositionId;
			positionUpdated = true;
		}
		m_LastX = queryResult.X;
		m_LastY = queryResult.Y;
	}

	const UINT32 visibility = queryResult.IsCursorVisible ? 1u : 0u;
	const UINT32 sdrWhiteLevelX1000 = ScaleSdrWhiteLevel(queryResult.SdrWhiteLevelNits);
	const bool whiteLevelChanged = m_LastSdrWhiteLevelX1000 != sdrWhiteLevelX1000;
	const bool visibilityChanged = m_LastVisibility != visibility;
	if (!shapeUpdated && !positionUpdated && !visibilityChanged && !whiteLevelChanged)
	{
		return S_OK;
	}
	m_LastVisibility = visibility;

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
	dst->SdrWhiteLevelX1000 = sdrWhiteLevelX1000;
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
		                    static_cast<LONG>(m_LastPositionId));
	}

	InterlockedExchange(reinterpret_cast<volatile LONG*>(&dst->Magic),
	                    static_cast<LONG>(VDD_CURSOR_MAGIC));
	MemoryBarrier();
	InterlockedExchange(sequence, stableSequence); // Even: the snapshot is stable.

	if (m_hCursorReadyEvent)
	{
		if (!SetEvent(m_hCursorReadyEvent))
		{
			const DWORD error = GetLastError();
			VDD_LOG_ERROR_STREAM("[VddCursor] Failed to signal cursor-ready event: " << error);
		}
	}

	m_LastSdrWhiteLevelX1000 = sdrWhiteLevelX1000;
	return S_OK;
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
