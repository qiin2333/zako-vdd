#include "SharedFrameExporter.h"

#include "..\Logging\Logger.h"

#include <sddl.h>
#include <sstream>

using namespace std;

namespace Microsoft
{
namespace IndirectDisp
{

struct SharedFrameMetadata
{
	UINT32 Magic;          // 'ZVDF' = 0x5A564446
	UINT32 Version;        // 1
	UINT32 Width;
	UINT32 Height;
	UINT32 DxgiFormat;     // DXGI_FORMAT
	UINT32 IsHdr;          // 0/1
	float  MaxNits;
	float  MinNits;
	float  MaxFALL;
	UINT64 FrameCounter;   // Incremented per pushed frame
	UINT64 LastPresentQpc; // IddCx present-display QPC if available, otherwise producer publish QPC
	// Optional fields appended for newer consumers. Version remains 1 so older
	// Sunshine builds can keep mapping only the original prefix above.
	UINT64 LastPublishQpc;             // QueryPerformanceCounter at producer-side publish
	UINT32 LastPresentationFrameNumber;
	UINT32 LastDirtyRectCount;
	UINT64 ReplacedUnreadFrames;       // Producer overwrote a published frame before it was consumed
	UINT64 DroppedConsumerHeldFrames;  // Consumer held the slot while producer wanted to publish
	UINT64 DroppedAcquireFailures;     // Non-timeout keyed mutex acquire failures
};

SharedFrameExporter::SharedFrameExporter(unsigned int monitorIndex, std::shared_ptr<Direct3DDevice> device)
	: m_MonitorIndex(monitorIndex), m_Device(device)
{
}

SharedFrameExporter::~SharedFrameExporter()
{
	Teardown();
}

void SharedFrameExporter::PushFrame(IDXGIResource* acquired,
                                    UINT64 presentDisplayQpc,
                                    UINT presentationFrameNumber,
                                    UINT dirtyRectCount)
{
	std::lock_guard<std::mutex> lock(m_ExportMutex);

	if (!acquired || !m_Device || !m_Device->Device)
	{
		return;
	}

	Microsoft::WRL::ComPtr<ID3D11Texture2D> srcTex;
	if (FAILED(acquired->QueryInterface(__uuidof(ID3D11Texture2D), &srcTex)) || !srcTex)
	{
		return;
	}

	D3D11_TEXTURE2D_DESC srcDesc = {};
	srcTex->GetDesc(&srcDesc);

	if (!EnsureSharedTexture(srcDesc))
	{
		return;
	}

	// Best-effort acquire with 0 timeout. First try the normal empty-slot
	// key (0). If the previous frame is still published but unread (key 1),
	// reclaim and overwrite it. That gives the export path WGC-like
	// "latest frame wins" mailbox semantics without stalling the IddCx loop.
	bool replacedUnreadFrame = false;
	HRESULT hr = m_KeyedMutex->AcquireSync(0, 0);
	if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
	{
		hr = m_KeyedMutex->AcquireSync(1, 0);
		if (SUCCEEDED(hr))
		{
			replacedUnreadFrame = true;
		}
	}
	if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
	{
		if (m_MetaView)
		{
			m_MetaView->DroppedConsumerHeldFrames++;
		}
		return;
	}
	if (FAILED(hr))
	{
		if (m_MetaView)
		{
			m_MetaView->DroppedAcquireFailures++;
		}
		return;
	}

	Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
	m_Device->Device->GetImmediateContext(&ctx);
	ctx->CopyResource(m_SharedTex.Get(), srcTex.Get());
	ctx->Flush();

	// Update metadata before releasing key 1 so the consumer sees metadata
	// that matches the texture contents it acquires.
	if (m_MetaView)
	{
		LARGE_INTEGER qpc{};
		QueryPerformanceCounter(&qpc);
		m_MetaView->FrameCounter++;
		m_MetaView->LastPresentQpc = presentDisplayQpc ? presentDisplayQpc : static_cast<UINT64>(qpc.QuadPart);
		m_MetaView->LastPublishQpc = static_cast<UINT64>(qpc.QuadPart);
		m_MetaView->LastPresentationFrameNumber = presentationFrameNumber;
		m_MetaView->LastDirtyRectCount = dirtyRectCount;
		if (replacedUnreadFrame)
		{
			m_MetaView->ReplacedUnreadFrames++;
		}
	}

	m_KeyedMutex->ReleaseSync(1);

	if (m_FrameReadyEvent)
	{
		SetEvent(m_FrameReadyEvent);
	}
}

void SharedFrameExporter::UpdateHdrMetadata(bool isHdr, float maxNits, float minNits, float maxFALL)
{
	std::lock_guard<std::mutex> lock(m_ExportMutex);

	m_PendingIsHdr = isHdr;
	m_PendingMaxNits = maxNits;
	m_PendingMinNits = minNits;
	m_PendingMaxFALL = maxFALL;
	if (m_MetaView)
	{
		m_MetaView->IsHdr = isHdr ? 1u : 0u;
		m_MetaView->MaxNits = maxNits;
		m_MetaView->MinNits = minNits;
		m_MetaView->MaxFALL = maxFALL;
		if (m_CachedFormat == DXGI_FORMAT_UNKNOWN)
		{
			m_MetaView->DxgiFormat = GuessMetadataFormat();
		}
	}
}

void SharedFrameExporter::PublishModeMetadata(UINT width, UINT height)
{
	if (width == 0 || height == 0)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(m_ExportMutex);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = GuessMetadataFormat();

	if (!EnsureEventAndMetadata(desc))
	{
		std::stringstream ss;
		ss << "[VddExport] Failed to publish mode metadata for monitor=" << m_MonitorIndex
		   << " " << width << "x" << height;
		VDD_LOG_ERROR(ss.str().c_str());
		return;
	}

	std::stringstream ss;
	ss << "[VddExport] Published mode metadata monitor=" << m_MonitorIndex
	   << " " << width << "x" << height
	   << " fmt=" << desc.Format
	   << " hdr=" << (m_PendingIsHdr ? 1 : 0);
	VDD_LOG_INFO(ss.str().c_str());
}

DXGI_FORMAT SharedFrameExporter::GuessMetadataFormat() const
{
	return m_PendingIsHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
}

bool SharedFrameExporter::EnsureSharedTexture(const D3D11_TEXTURE2D_DESC& srcDesc)
{
	if (m_SharedTex &&
	    m_CachedWidth == srcDesc.Width &&
	    m_CachedHeight == srcDesc.Height &&
	    m_CachedFormat == srcDesc.Format)
	{
		return true;
	}

	// Recreate everything since dimensions / format changed.
	TeardownTexture();

	// Keep the Global namespace for cross-session Sunshine compatibility, but
	// avoid granting WRITE_DAC/WRITE_OWNER/DELETE to consumers.
	SECURITY_ATTRIBUTES sa = {};
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr))
	{
		VDD_LOG_ERROR("[VddExport] Failed to build SDDL for shared texture");
		return false;
	}
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = FALSE;

	D3D11_TEXTURE2D_DESC desc = srcDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.MipLevels = 1;
	desc.ArraySize = 1;

	HRESULT hr = m_Device->Device->CreateTexture2D(&desc, nullptr, &m_SharedTex);
	if (FAILED(hr))
	{
		std::stringstream ss;
		ss << "[VddExport] CreateTexture2D failed: 0x" << std::hex << hr;
		VDD_LOG_ERROR(ss.str().c_str());
		LocalFree(sd);
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes;
	hr = m_SharedTex.As(&dxgiRes);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR("[VddExport] QueryInterface IDXGIResource1 failed");
		m_SharedTex.Reset();
		LocalFree(sd);
		return false;
	}

	std::wstring texName = L"Global\\ZakoVDD_Frame_" + std::to_wstring(m_MonitorIndex);
	HANDLE ntHandle = nullptr;
	hr = dxgiRes->CreateSharedHandle(&sa, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, texName.c_str(), &ntHandle);
	LocalFree(sd);
	if (FAILED(hr))
	{
		std::stringstream ss;
		ss << "[VddExport] CreateSharedHandle failed: 0x" << std::hex << hr;
		VDD_LOG_ERROR(ss.str().c_str());
		m_SharedTex.Reset();
		return false;
	}
	// The NT handle MUST stay open for the named lookup to remain valid;
	// closing it before consumers open will make OpenSharedResourceByName
	// return E_INVALIDARG even though the texture is still alive in our process.
	if (m_NtHandle)
	{
		CloseHandle(m_NtHandle);
	}
	m_NtHandle = ntHandle;

	hr = m_SharedTex.As(&m_KeyedMutex);
	if (FAILED(hr) || !m_KeyedMutex)
	{
		VDD_LOG_ERROR("[VddExport] QueryInterface IDXGIKeyedMutex failed");
		m_SharedTex.Reset();
		return false;
	}

	if (!EnsureEventAndMetadata(srcDesc))
	{
		m_KeyedMutex.Reset();
		m_SharedTex.Reset();
		return false;
	}

	m_CachedWidth = srcDesc.Width;
	m_CachedHeight = srcDesc.Height;
	m_CachedFormat = srcDesc.Format;

	std::stringstream ss;
	ss << "[VddExport] Shared texture ready monitor=" << m_MonitorIndex
	   << " " << srcDesc.Width << "x" << srcDesc.Height
	   << " fmt=" << srcDesc.Format;
	VDD_LOG_INFO(ss.str().c_str());
	return true;
}

bool SharedFrameExporter::EnsureEventAndMetadata(const D3D11_TEXTURE2D_DESC& srcDesc)
{
	SECURITY_ATTRIBUTES sa = {};
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr))
	{
		return false;
	}
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = FALSE;

	if (!m_FrameReadyEvent)
	{
		std::wstring evName = L"Global\\ZakoVDD_FrameReady_" + std::to_wstring(m_MonitorIndex);
		m_FrameReadyEvent = CreateEventW(&sa, FALSE, FALSE, evName.c_str());
		if (!m_FrameReadyEvent)
		{
			std::stringstream ss;
			ss << "[VddExport] CreateEventW failed: " << GetLastError();
			VDD_LOG_ERROR(ss.str().c_str());
			LocalFree(sd);
			return false;
		}
	}

	if (!m_MetaMapping)
	{
		std::wstring mapName = L"Global\\ZakoVDD_Meta_" + std::to_wstring(m_MonitorIndex);
		m_MetaMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
		                                   PAGE_READWRITE, 0, sizeof(SharedFrameMetadata),
		                                   mapName.c_str());
		if (!m_MetaMapping)
		{
			std::stringstream ss;
			ss << "[VddExport] CreateFileMappingW failed: " << GetLastError();
			VDD_LOG_ERROR(ss.str().c_str());
			LocalFree(sd);
			return false;
		}
		m_MetaView = static_cast<SharedFrameMetadata*>(
		    MapViewOfFile(m_MetaMapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedFrameMetadata)));
		if (!m_MetaView)
		{
			LocalFree(sd);
			return false;
		}
		ZeroMemory(m_MetaView, sizeof(SharedFrameMetadata));
		m_MetaView->Magic = 0x5A564446; // 'ZVDF'
		m_MetaView->Version = 1;
	}
	LocalFree(sd);

	m_MetaView->Width = srcDesc.Width;
	m_MetaView->Height = srcDesc.Height;
	m_MetaView->DxgiFormat = srcDesc.Format;
	m_MetaView->IsHdr = m_PendingIsHdr ? 1u : 0u;
	m_MetaView->MaxNits = m_PendingMaxNits;
	m_MetaView->MinNits = m_PendingMinNits;
	m_MetaView->MaxFALL = m_PendingMaxFALL;
	return true;
}

void SharedFrameExporter::TeardownTexture()
{
	m_KeyedMutex.Reset();
	m_SharedTex.Reset();
	if (m_NtHandle)
	{
		CloseHandle(m_NtHandle);
		m_NtHandle = nullptr;
	}
	m_CachedWidth = 0;
	m_CachedHeight = 0;
	m_CachedFormat = DXGI_FORMAT_UNKNOWN;
}

void SharedFrameExporter::Teardown()
{
	TeardownTexture();
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
	if (m_FrameReadyEvent)
	{
		CloseHandle(m_FrameReadyEvent);
		m_FrameReadyEvent = nullptr;
	}
}

}
}
