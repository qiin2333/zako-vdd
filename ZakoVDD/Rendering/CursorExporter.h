#pragma once

#include "..\Driver.h"

#include <vector>

namespace Microsoft
{
namespace IndirectDisp
{

struct CursorSharedMetadata;

class CursorExporter
{
public:
	CursorExporter(unsigned int monitorIndex, IDDCX_MONITOR monitor, HANDLE hNewCursorDataAvailable);
	~CursorExporter();

	bool Start();
	void Stop();

private:
	static DWORD WINAPI ThreadProc(LPVOID arg);

	void Run();
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
	HANDLE m_hTerminateEvent = nullptr;
	HANDLE m_hThread = nullptr;
	HANDLE m_MetaMapping = nullptr;
	CursorSharedMetadata* m_MetaView = nullptr;
	HANDLE m_hCursorReadyEvent = nullptr;

	INT32 m_LastX = 0;
	INT32 m_LastY = 0;
	UINT32 m_LastVisibility = 0xFFFFFFFFu;
	CachedShape m_CachedShape;
};

}
}
