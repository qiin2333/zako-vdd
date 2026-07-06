#include "SharedFrameExporter.h"

#include "..\Logging\Logger.h"

#include <sddl.h>
#include <sstream>

using namespace std;

namespace Microsoft
{
namespace IndirectDisp
{

static constexpr UINT32 FrameChannelFlags =
	VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW |
	VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES |
	VDD_FRAME_CHANNEL_FLAG_STRICT_DACL;

static std::wstring SharedTextureName(unsigned int monitorIndex, UINT slotIndex)
{
	std::wstring base = L"Global\\ZakoVDD_Frame_" + std::to_wstring(monitorIndex);
	if (slotIndex == 0)
	{
		return base;
	}
	return base + L"_Slot_" + std::to_wstring(slotIndex);
}

static bool IsZeroLuid(UINT32 lowPart, INT32 highPart)
{
	return lowPart == 0 && highPart == 0;
}

static bool LuidMatches(const LUID& lhs, UINT32 rhsLowPart, INT32 rhsHighPart)
{
	return lhs.LowPart == rhsLowPart && lhs.HighPart == rhsHighPart;
}

static void CloseDuplicatedTargetHandle(HANDLE targetProcess, HANDLE targetHandle)
{
	if (!targetProcess || !targetHandle)
	{
		return;
	}

	HANDLE localDuplicate = nullptr;
	if (DuplicateHandle(targetProcess,
	                    targetHandle,
	                    GetCurrentProcess(),
	                    &localDuplicate,
	                    0,
	                    FALSE,
	                    DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) &&
	    localDuplicate)
	{
		CloseHandle(localDuplicate);
	}
}

static bool DuplicateHandleToTarget(HANDLE targetProcess,
                                    HANDLE sourceHandle,
                                    DWORD desiredAccess,
                                    UINT64& targetHandleValue)
{
	targetHandleValue = 0;
	if (!targetProcess || !sourceHandle)
	{
		return false;
	}

	HANDLE duplicatedHandle = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(),
	                     sourceHandle,
	                     targetProcess,
	                     &duplicatedHandle,
	                     desiredAccess,
	                     FALSE,
	                     0))
	{
		return false;
	}

	targetHandleValue = static_cast<UINT64>(reinterpret_cast<UINT_PTR>(duplicatedHandle));
	return true;
}

static void CloseFrameChannelResponseHandles(HANDLE targetProcess, VDD_FRAME_CHANNEL_OPEN_RESPONSE& response)
{
	CloseDuplicatedTargetHandle(targetProcess, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(response.MetadataHandle)));
	response.MetadataHandle = 0;

	CloseDuplicatedTargetHandle(targetProcess, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(response.FrameReadyEventHandle)));
	response.FrameReadyEventHandle = 0;

	for (auto& slot : response.Slots)
	{
		CloseDuplicatedTargetHandle(targetProcess, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(slot.TextureHandle)));
		slot.TextureHandle = 0;
	}
}

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
	UINT32 MetadataSize;               // sizeof(SharedFrameMetadata) for optional-tail readers
	UINT32 SlotCount;                  // Number of shared mailbox slots exported by this producer
	UINT32 SlotIndex;                  // Slot containing the latest published frame
	UINT32 MetadataSequence;           // High 16 bits = channel generation; low 16 bits even/odd seqlock
	UINT32 AdapterLuidLowPart;         // Render adapter LUID used by this swap chain
	INT32  AdapterLuidHighPart;
	UINT64 ProducerQpcFrequency;
};

VDD_FRAME_CHANNEL_CAPS SharedFrameExporter::FrameChannelCaps()
{
	VDD_FRAME_CHANNEL_CAPS caps = {};
	caps.Size = sizeof(caps);
	caps.Version = VDD_FRAME_CHANNEL_CAPS_VERSION;
	caps.Flags = FrameChannelFlags;
	caps.MaxSharedSlots = SharedFrameSlotCount;
	caps.MetadataSize = sizeof(SharedFrameMetadata);
	return caps;
}

SharedFrameExporter::SharedFrameExporter(unsigned int monitorIndex, std::shared_ptr<Direct3DDevice> device)
	: m_MonitorIndex(monitorIndex), m_Device(device)
{
}

SharedFrameExporter::~SharedFrameExporter()
{
	Teardown();
}

void SharedFrameExporter::BeginMetadataWrite()
{
	if (m_MetaView)
	{
		if ((m_MetadataSequenceCounter & 1u) == 0)
		{
			++m_MetadataSequenceCounter;
		}
		m_MetaView->MetadataSequence = ComposeMetadataSequence(true);
	}
}

void SharedFrameExporter::EndMetadataWrite()
{
	if (m_MetaView)
	{
		if ((m_MetadataSequenceCounter & 1u) != 0)
		{
			++m_MetadataSequenceCounter;
		}
		m_MetaView->MetadataSequence = ComposeMetadataSequence(false);
	}
}

void SharedFrameExporter::BumpChannelGeneration()
{
	++m_ChannelGeneration;
	if (m_ChannelGeneration == 0)
	{
		m_ChannelGeneration = 1;
	}
	m_MetadataSequenceCounter = 0;
}

UINT32 SharedFrameExporter::ComposeMetadataSequence(bool writing) const
{
	UINT16 sequence = m_MetadataSequenceCounter;
	if (writing)
	{
		sequence = static_cast<UINT16>(sequence | 1u);
	}
	else
	{
		sequence = static_cast<UINT16>(sequence & ~1u);
	}
	return (static_cast<UINT32>(m_ChannelGeneration) << 16) | sequence;
}

SharedFrameExporter::MetadataWriteScope::MetadataWriteScope(SharedFrameExporter& exporter)
	: m_Exporter(exporter)
{
	m_Exporter.BeginMetadataWrite();
}

SharedFrameExporter::MetadataWriteScope::~MetadataWriteScope()
{
	m_Exporter.EndMetadataWrite();
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
	// key (0) across the ring. If all slots are already published but unread
	// (key 1), reclaim one and overwrite it. That keeps WGC-like "latest frame
	// wins" semantics without stalling the IddCx loop.
	bool replacedUnreadFrame = false;
	bool sawTimeout = false;
	bool sawFailure = false;
	UINT selectedSlot = SharedFrameSlotCount;
	HRESULT hr = E_FAIL;

	// IDXGIKeyedMutex::AcquireSync returns WAIT_TIMEOUT as a DWORD-style
	// success code, so only S_OK means the mutex was actually acquired.
	for (UINT offset = 0; offset < SharedFrameSlotCount; ++offset)
	{
		UINT slot = (m_NextPublishSlot + offset) % SharedFrameSlotCount;
		if (!m_KeyedMutex[slot])
		{
			continue;
		}

		hr = m_KeyedMutex[slot]->AcquireSync(0, 0);
		if (hr == S_OK)
		{
			selectedSlot = slot;
			break;
		}
		if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
		{
			sawTimeout = true;
		}
		else
		{
			sawFailure = true;
		}
	}

	if (selectedSlot == SharedFrameSlotCount)
	{
		for (UINT offset = 0; offset < SharedFrameSlotCount; ++offset)
		{
			UINT slot = (m_NextPublishSlot + offset) % SharedFrameSlotCount;
			if (!m_KeyedMutex[slot])
			{
				continue;
			}

			hr = m_KeyedMutex[slot]->AcquireSync(1, 0);
			if (hr == S_OK)
			{
				selectedSlot = slot;
				replacedUnreadFrame = true;
				break;
			}
			if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
			{
				sawTimeout = true;
			}
			else
			{
				sawFailure = true;
			}
		}
	}

	if (selectedSlot == SharedFrameSlotCount)
	{
		if (m_MetaView)
		{
			MetadataWriteScope metadataWrite(*this);
			if (sawTimeout)
			{
				m_MetaView->DroppedConsumerHeldFrames++;
			}
			else if (sawFailure)
			{
				m_MetaView->DroppedAcquireFailures++;
			}
		}
		return;
	}

	Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
	m_Device->Device->GetImmediateContext(&ctx);
	ctx->CopyResource(m_SharedTex[selectedSlot].Get(), srcTex.Get());
	ctx->Flush();

	// Update metadata before releasing key 1 so the consumer sees metadata
	// that matches the texture contents it acquires.
	if (m_MetaView)
	{
		LARGE_INTEGER qpc{};
		QueryPerformanceCounter(&qpc);
		MetadataWriteScope metadataWrite(*this);
		m_MetaView->FrameCounter++;
		m_MetaView->LastPresentQpc = presentDisplayQpc ? presentDisplayQpc : static_cast<UINT64>(qpc.QuadPart);
		m_MetaView->LastPublishQpc = static_cast<UINT64>(qpc.QuadPart);
		m_MetaView->LastPresentationFrameNumber = presentationFrameNumber;
		m_MetaView->LastDirtyRectCount = dirtyRectCount;
		m_MetaView->SlotCount = SharedFrameSlotCount;
		m_MetaView->SlotIndex = selectedSlot;
		if (replacedUnreadFrame)
		{
			m_MetaView->ReplacedUnreadFrames++;
		}
	}

	hr = m_KeyedMutex[selectedSlot]->ReleaseSync(1);
	if (FAILED(hr))
	{
		std::stringstream ss;
		ss << "[VddExport] ReleaseSync(1) failed for slot=" << selectedSlot << ": 0x" << std::hex << hr;
		VDD_LOG_ERROR(ss.str().c_str());
		TeardownTexture();
		return;
	}
	m_NextPublishSlot = (selectedSlot + 1) % SharedFrameSlotCount;

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
		MetadataWriteScope metadataWrite(*this);
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

NTSTATUS SharedFrameExporter::OpenFrameChannel(const VDD_FRAME_CHANNEL_OPEN_REQUEST& request,
                                               HANDLE targetProcess,
                                               VDD_FRAME_CHANNEL_OPEN_RESPONSE& response)
{
	ZeroMemory(&response, sizeof(response));
	response.Size = sizeof(response);
	response.Version = VDD_FRAME_CHANNEL_OPEN_VERSION;
	response.Flags = FrameChannelFlags;
	response.SlotCount = SharedFrameSlotCount;
	response.MetadataSize = sizeof(SharedFrameMetadata);

	if (request.Size < sizeof(VDD_FRAME_CHANNEL_OPEN_REQUEST) ||
	    request.Version != VDD_FRAME_CHANNEL_OPEN_VERSION ||
	    request.TargetProcessId == 0 ||
	    !targetProcess ||
	    (request.DesiredSlots != 0 && request.DesiredSlots != SharedFrameSlotCount) ||
	    (request.RequiredFlags & ~FrameChannelFlags) != 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	if (m_Device && !IsZeroLuid(request.AdapterLuidLowPart, request.AdapterLuidHighPart) &&
	    !LuidMatches(m_Device->AdapterLuid, request.AdapterLuidLowPart, request.AdapterLuidHighPart))
	{
		return STATUS_NOT_SUPPORTED;
	}

	std::lock_guard<std::mutex> lock(m_ExportMutex);
	if (!m_MetaMapping || !m_MetaView || !m_FrameReadyEvent)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	for (const auto handle : m_SealedNtHandle)
	{
		if (!handle)
		{
			return STATUS_DEVICE_NOT_READY;
		}
	}

	NTSTATUS status = STATUS_SUCCESS;
	if (!DuplicateHandleToTarget(targetProcess, m_MetaMapping, FILE_MAP_READ, response.MetadataHandle) ||
	    !DuplicateHandleToTarget(targetProcess, m_FrameReadyEvent, SYNCHRONIZE, response.FrameReadyEventHandle))
	{
		status = STATUS_ACCESS_DENIED;
	}

	for (UINT slot = 0; NT_SUCCESS(status) && slot < SharedFrameSlotCount; ++slot)
	{
		if (!DuplicateHandleToTarget(targetProcess,
		                             m_SealedNtHandle[slot],
		                             DXGI_SHARED_RESOURCE_READ,
		                             response.Slots[slot].TextureHandle))
		{
			status = STATUS_ACCESS_DENIED;
			break;
		}
	}

	if (!NT_SUCCESS(status))
	{
		CloseFrameChannelResponseHandles(targetProcess, response);
	}
	return status;
}

DXGI_FORMAT SharedFrameExporter::GuessMetadataFormat() const
{
	return m_PendingIsHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
}

bool SharedFrameExporter::EnsureSharedTexture(const D3D11_TEXTURE2D_DESC& srcDesc)
{
	bool allSlotsReady = true;
	for (const auto& tex : m_SharedTex)
	{
		if (!tex)
		{
			allSlotsReady = false;
			break;
		}
	}

	if (allSlotsReady &&
	    m_CachedWidth == srcDesc.Width &&
	    m_CachedHeight == srcDesc.Height &&
	    m_CachedFormat == srcDesc.Format)
	{
		return true;
	}

	// Recreate everything since dimensions / format changed.
	TeardownTexture();
	BumpChannelGeneration();

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

	for (UINT slot = 0; slot < SharedFrameSlotCount; ++slot)
	{
		HRESULT hr = m_Device->Device->CreateTexture2D(&desc, nullptr, &m_SharedTex[slot]);
		if (FAILED(hr))
		{
			std::stringstream ss;
			ss << "[VddExport] CreateTexture2D failed for slot=" << slot << ": 0x" << std::hex << hr;
			VDD_LOG_ERROR(ss.str().c_str());
			LocalFree(sd);
			TeardownTexture();
			return false;
		}

		Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes;
		hr = m_SharedTex[slot].As(&dxgiRes);
		if (FAILED(hr))
		{
			VDD_LOG_ERROR("[VddExport] QueryInterface IDXGIResource1 failed");
			LocalFree(sd);
			TeardownTexture();
			return false;
		}

		HANDLE namedNtHandle = nullptr;
		auto texName = SharedTextureName(m_MonitorIndex, slot);
		hr = dxgiRes->CreateSharedHandle(&sa, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, texName.c_str(), &namedNtHandle);
		if (FAILED(hr))
		{
			std::stringstream ss;
			ss << "[VddExport] CreateSharedHandle(named) failed for slot=" << slot << ": 0x" << std::hex << hr;
			VDD_LOG_ERROR(ss.str().c_str());
			LocalFree(sd);
			TeardownTexture();
			return false;
		}
		// The NT handle MUST stay open for the named lookup to remain valid;
		// closing it before consumers open will make OpenSharedResourceByName
		// return E_INVALIDARG even though the texture is still alive in our process.
		m_NamedNtHandle[slot] = namedNtHandle;

		HANDLE sealedNtHandle = nullptr;
		hr = dxgiRes->CreateSharedHandle(&sa, DXGI_SHARED_RESOURCE_READ, nullptr, &sealedNtHandle);
		if (FAILED(hr))
		{
			std::stringstream ss;
			ss << "[VddExport] CreateSharedHandle(sealed) failed for slot=" << slot << ": 0x" << std::hex << hr;
			VDD_LOG_ERROR(ss.str().c_str());
			LocalFree(sd);
			TeardownTexture();
			return false;
		}
		m_SealedNtHandle[slot] = sealedNtHandle;

		hr = m_SharedTex[slot].As(&m_KeyedMutex[slot]);
		if (FAILED(hr) || !m_KeyedMutex[slot])
		{
			VDD_LOG_ERROR("[VddExport] QueryInterface IDXGIKeyedMutex failed");
			LocalFree(sd);
			TeardownTexture();
			return false;
		}
	}
	LocalFree(sd);

	if (!EnsureEventAndMetadata(srcDesc))
	{
		TeardownTexture();
		return false;
	}

	m_CachedWidth = srcDesc.Width;
	m_CachedHeight = srcDesc.Height;
	m_CachedFormat = srcDesc.Format;

	std::stringstream ss;
	ss << "[VddExport] Shared texture ready monitor=" << m_MonitorIndex
	   << " " << srcDesc.Width << "x" << srcDesc.Height
	   << " fmt=" << srcDesc.Format
	   << " slots=" << SharedFrameSlotCount;
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
			std::stringstream ss;
			ss << "[VddExport] MapViewOfFile failed: " << GetLastError();
			VDD_LOG_ERROR(ss.str().c_str());
			CloseHandle(m_MetaMapping);
			m_MetaMapping = nullptr;
			LocalFree(sd);
			return false;
		}
		ZeroMemory(m_MetaView, sizeof(SharedFrameMetadata));
		MetadataWriteScope metadataWrite(*this);
		m_MetaView->Magic = 0x5A564446; // 'ZVDF'
		m_MetaView->Version = 1;
		m_MetaView->MetadataSize = sizeof(SharedFrameMetadata);
		m_MetaView->SlotCount = SharedFrameSlotCount;
		m_MetaView->SlotIndex = 0;
		LARGE_INTEGER frequency{};
		if (QueryPerformanceFrequency(&frequency))
		{
			m_MetaView->ProducerQpcFrequency = static_cast<UINT64>(frequency.QuadPart);
		}
	}
	LocalFree(sd);

	MetadataWriteScope metadataWrite(*this);
	m_MetaView->MetadataSize = sizeof(SharedFrameMetadata);
	m_MetaView->Width = srcDesc.Width;
	m_MetaView->Height = srcDesc.Height;
	m_MetaView->DxgiFormat = srcDesc.Format;
	m_MetaView->IsHdr = m_PendingIsHdr ? 1u : 0u;
	m_MetaView->MaxNits = m_PendingMaxNits;
	m_MetaView->MinNits = m_PendingMinNits;
	m_MetaView->MaxFALL = m_PendingMaxFALL;
	m_MetaView->SlotCount = SharedFrameSlotCount;
	m_MetaView->SlotIndex = 0;
	if (m_Device)
	{
		m_MetaView->AdapterLuidLowPart = m_Device->AdapterLuid.LowPart;
		m_MetaView->AdapterLuidHighPart = m_Device->AdapterLuid.HighPart;
	}
	return true;
}

void SharedFrameExporter::TeardownTexture()
{
	for (auto& keyedMutex : m_KeyedMutex)
	{
		keyedMutex.Reset();
	}
	for (auto& tex : m_SharedTex)
	{
		tex.Reset();
	}
	for (auto& handle : m_NamedNtHandle)
	{
		if (handle)
		{
			CloseHandle(handle);
			handle = nullptr;
		}
	}
	for (auto& handle : m_SealedNtHandle)
	{
		if (handle)
		{
			CloseHandle(handle);
			handle = nullptr;
		}
	}
	m_NextPublishSlot = 0;
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
