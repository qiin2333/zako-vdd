#pragma once

#include "..\Driver.h"
#include "vdd_cursor_shared.h"

#include <vector>

namespace Microsoft
{
namespace IndirectDisp
{

// Publishes the monitor hardware cursor through shared memory.
//
// IddCx DDIs (including IddCxMonitorQueryHardwareCursor*) are not documented
// as thread-safe with respect to other IddCx calls issued for the same
// monitor, so all cursor queries run on the swap-chain processing thread.
// SwapChainProcessor's frame loop wakes this exporter through the
// IddCx-provided cursor data-available event and calls Poll(); there is no
// dedicated worker thread and therefore no cursor work can outlive an
// unassigned swap chain (see issue #8).
class CursorExporter
{
public:
	CursorExporter(unsigned int monitorIndex, IDDCX_MONITOR monitor, HANDLE hNewCursorDataAvailable);
	~CursorExporter();
	CursorExporter(const CursorExporter&) = delete;
	CursorExporter& operator=(const CursorExporter&) = delete;
	CursorExporter(CursorExporter&&) = delete;
	CursorExporter& operator=(CursorExporter&&) = delete;

	// Prepares the shared-memory objects. Must be called while the swap chain
	// is still being assigned (i.e. from AssignSwapChain), never later.
	bool Start();

	// Queries the hardware cursor once and publishes any change through the
	// shared mapping. Must be called from the swap-chain processing thread
	// while the swap chain is still assigned. Returns S_OK on success (also
	// when nothing changed) and the last query status on failure, so the
	// caller can report it.
	HRESULT Poll();

	// Event that IddCx signals whenever new hardware-cursor data is available.
	// The handle itself is owned by IndirectDeviceContext.
	HANDLE GetCursorDataAvailableEvent() const { return m_hCursorDataAvailable; }

private:
	bool EnsureSharedObjects();
	void Teardown();

	struct CachedShape
	{
		UINT32 ShapeId = 0;
		UINT32 Type = 0;
		UINT32 Width = 0;
		UINT32 Height = 0;
		UINT32 Pitch = 0;
		INT32 XHot = 0;
		INT32 YHot = 0;
		UINT32 BufferSize = 0;
		std::vector<BYTE> Buffer;
	};

	unsigned int m_MonitorIndex = 0;
	IDDCX_MONITOR m_Monitor = nullptr;
	HANDLE m_hCursorDataAvailable = nullptr; // Owned by IndirectDeviceContext.
	HANDLE m_MetaMapping = nullptr;
	VDD_CURSOR_SHARED_METADATA* m_MetaView = nullptr;
	HANDLE m_hCursorReadyEvent = nullptr;

	std::vector<BYTE> m_ShapeBuffer;
	UINT32 m_LastShapeId = 0;
	UINT32 m_LastPositionId = 0;
	UINT32 m_LastSdrWhiteLevelX1000 = 0;
	INT32 m_LastX = 0;
	INT32 m_LastY = 0;
	UINT32 m_LastVisibility = 0xFFFFFFFFu;
	CachedShape m_CachedShape;
};

}
}
