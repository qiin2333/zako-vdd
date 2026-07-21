#include "CursorExporter.h"

#include "..\Logging\Logger.h"

#include <cstring>
#include <sddl.h>
#include <sstream>
#include <string>

namespace Microsoft
{
namespace IndirectDisp
{

#pragma pack(push, 4)
struct CursorSharedMetadata
{
	UINT32 Magic;
	UINT32 Version;
	UINT32 IsVisible;
	INT32 PositionX;
	INT32 PositionY;
	UINT32 PositionId;
	UINT32 ShapeId;
	UINT32 ShapeType;
	UINT32 Width;
	UINT32 Height;
	UINT32 Pitch;
	INT32 XHot;
	INT32 YHot;
	UINT32 SdrWhiteLevelX1000;
	UINT32 ShapeBufferSize;
	UINT32 Reserved0;
	UINT64 LastUpdateQpc;
};
#pragma pack(pop)

namespace
{

static constexpr UINT32 ZAKO_CURSOR_MAGIC = 0x5A564355u; // 'ZVCU'
static constexpr UINT32 ZAKO_CURSOR_VERSION = 1u;
static constexpr UINT32 ZAKO_CURSOR_MAX_WIDTH = 256u;
static constexpr UINT32 ZAKO_CURSOR_MAX_HEIGHT = 256u;
static constexpr UINT32 ZAKO_CURSOR_MAX_BYTES = ZAKO_CURSOR_MAX_WIDTH * ZAKO_CURSOR_MAX_HEIGHT * 4u;

static_assert(sizeof(CursorSharedMetadata) % 4 == 0, "CursorSharedMetadata must be 4-byte aligned");

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
	std::vector<BYTE> shapeBuffer(ZAKO_CURSOR_MAX_BYTES);
	UINT lastShapeId = 0;
	UINT lastPositionId = 0;
	UINT publishedPositionId = 0;

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

		IDARG_OUT_QUERY_HWCURSOR3 outArgs = {};
		const NTSTATUS status = IddCxMonitorQueryHardwareCursor3(m_Monitor, &inArgs, &outArgs);
		if (!NT_SUCCESS(status))
		{
			continue;
		}

		const bool shapeUpdated = outArgs.IsCursorShapeUpdated != FALSE;
		if (shapeUpdated)
		{
			lastShapeId = outArgs.CursorShapeInfo.ShapeId;
			m_CachedShape.ShapeId = outArgs.CursorShapeInfo.ShapeId;
			m_CachedShape.Type = static_cast<UINT32>(outArgs.CursorShapeInfo.CursorType);
			m_CachedShape.Width = outArgs.CursorShapeInfo.Width;
			m_CachedShape.Height = outArgs.CursorShapeInfo.Height;
			m_CachedShape.Pitch = outArgs.CursorShapeInfo.Pitch;
			m_CachedShape.XHot = static_cast<INT32>(outArgs.CursorShapeInfo.XHot);
			m_CachedShape.YHot = static_cast<INT32>(outArgs.CursorShapeInfo.YHot);

			const UINT32 needed = static_cast<UINT32>(outArgs.CursorShapeInfo.Pitch) * outArgs.CursorShapeInfo.Height;
			m_CachedShape.BufferSize = needed <= ZAKO_CURSOR_MAX_BYTES ? needed : 0;
			if (m_CachedShape.BufferSize > 0)
			{
				m_CachedShape.Buffer.assign(shapeBuffer.begin(), shapeBuffer.begin() + m_CachedShape.BufferSize);
			}
			else
			{
				m_CachedShape.Buffer.clear();
			}
		}

		// IddCx reports the desktop-relative top-left of the cursor image.
		// The hot spot has already been applied; do not subtract it again.
		bool positionUpdated = false;
		if (outArgs.PositionValid)
		{
			if (outArgs.PositionId != lastPositionId)
			{
				lastPositionId = outArgs.PositionId;
				positionUpdated = true;
			}
			m_LastX = outArgs.X;
			m_LastY = outArgs.Y;
		}

		const UINT32 visibility = outArgs.IsCursorVisible ? 1u : 0u;
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

		CursorSharedMetadata snap = {};
		snap.Magic = ZAKO_CURSOR_MAGIC;
		snap.Version = ZAKO_CURSOR_VERSION;
		snap.IsVisible = visibility;
		snap.SdrWhiteLevelX1000 = static_cast<UINT32>(outArgs.SdrWhiteLevel);
		snap.ShapeId = m_CachedShape.ShapeId;
		snap.ShapeType = m_CachedShape.Type;
		snap.Width = m_CachedShape.Width;
		snap.Height = m_CachedShape.Height;
		snap.Pitch = m_CachedShape.Pitch;
		snap.XHot = m_CachedShape.XHot;
		snap.YHot = m_CachedShape.YHot;
		snap.ShapeBufferSize = m_CachedShape.BufferSize;
		snap.PositionX = m_LastX;
		snap.PositionY = m_LastY;
		snap.PositionId = ++publishedPositionId;

		LARGE_INTEGER qpc = {};
		QueryPerformanceCounter(&qpc);
		snap.LastUpdateQpc = static_cast<UINT64>(qpc.QuadPart);

		CursorSharedMetadata* dst = m_MetaView;
		if (shapeUpdated && m_CachedShape.BufferSize > 0)
		{
			BYTE* shapeDst = reinterpret_cast<BYTE*>(dst + 1);
			std::memcpy(shapeDst, m_CachedShape.Buffer.data(), m_CachedShape.BufferSize);
		}

		// Publish the payload before Magic so consumers can use Magic as a
		// lightweight publication marker after waking on the ready event.
		dst->Version = snap.Version;
		dst->IsVisible = snap.IsVisible;
		dst->PositionX = snap.PositionX;
		dst->PositionY = snap.PositionY;
		dst->PositionId = snap.PositionId;
		dst->ShapeId = snap.ShapeId;
		dst->ShapeType = snap.ShapeType;
		dst->Width = snap.Width;
		dst->Height = snap.Height;
		dst->Pitch = snap.Pitch;
		dst->XHot = snap.XHot;
		dst->YHot = snap.YHot;
		dst->SdrWhiteLevelX1000 = snap.SdrWhiteLevelX1000;
		dst->ShapeBufferSize = snap.ShapeBufferSize;
		dst->Reserved0 = 0;
		dst->LastUpdateQpc = snap.LastUpdateQpc;
		dst->Magic = snap.Magic;

		if (m_hCursorReadyEvent)
		{
			SetEvent(m_hCursorReadyEvent);
		}
	}

	VDD_LOG_DEBUG_STREAM("[VddCursor] Worker exiting monitor=" << m_MonitorIndex);
}

bool CursorExporter::EnsureSharedObjects()
{
	SECURITY_ATTRIBUTES sa = {};
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        L"D:(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1, &sd, nullptr))
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] Failed to build SDDL: " << GetLastError());
		return false;
	}

	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = FALSE;

	const SIZE_T mapSize = sizeof(CursorSharedMetadata) + ZAKO_CURSOR_MAX_BYTES;
	m_MetaMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
	                                   static_cast<DWORD>(mapSize), CursorMapName(m_MonitorIndex));
	if (!m_MetaMapping)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] CreateFileMappingW failed: " << GetLastError());
		LocalFree(sd);
		return false;
	}

	m_MetaView = static_cast<CursorSharedMetadata*>(MapViewOfFile(m_MetaMapping, FILE_MAP_WRITE, 0, 0, mapSize));
	if (!m_MetaView)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] MapViewOfFile failed: " << GetLastError());
		LocalFree(sd);
		return false;
	}
	ZeroMemory(m_MetaView, mapSize);
	m_MetaView->Magic = ZAKO_CURSOR_MAGIC;
	m_MetaView->Version = ZAKO_CURSOR_VERSION;

	m_hCursorReadyEvent = CreateEventW(&sa, FALSE, FALSE, CursorReadyEventName(m_MonitorIndex));
	if (!m_hCursorReadyEvent)
	{
		VDD_LOG_ERROR_STREAM("[VddCursor] CreateEventW (cursor ready) failed: " << GetLastError());
		LocalFree(sd);
		return false;
	}

	LocalFree(sd);
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
