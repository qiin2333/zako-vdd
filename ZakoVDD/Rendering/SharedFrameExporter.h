#pragma once

#include "Direct3DDevice.h"

#include <array>
#include <memory>
#include <mutex>
#include <vdd_control_ioctl.h>

namespace Microsoft
{
namespace IndirectDisp
{

class SharedFrameExporter
{
public:
	static constexpr UINT SharedFrameSlotCount = 3;
	static VDD_FRAME_CHANNEL_CAPS FrameChannelCaps();

	SharedFrameExporter(unsigned int monitorIndex, std::shared_ptr<Direct3DDevice> device);
	~SharedFrameExporter();

	void PushFrame(IDXGIResource* acquired,
	               UINT64 presentDisplayQpc = 0,
	               UINT presentationFrameNumber = 0,
	               UINT dirtyRectCount = 0);
	void UpdateHdrMetadata(bool isHdr, float maxNits, float minNits, float maxFALL);
	void PublishModeMetadata(UINT width, UINT height);
	void ClearExpectedMode();
	NTSTATUS OpenFrameChannel(const VDD_FRAME_CHANNEL_OPEN_REQUEST& request,
	                          HANDLE targetProcess,
	                          VDD_FRAME_CHANNEL_OPEN_RESPONSE& response);

private:
	DXGI_FORMAT GuessMetadataFormat() const;
	bool EnsureSharedTexture(const D3D11_TEXTURE2D_DESC& srcDesc);
	bool EnsureEventAndMetadata(const D3D11_TEXTURE2D_DESC& srcDesc);
	bool IsReadyForExpectedMode() const;
	void BeginMetadataWrite();
	void EndMetadataWrite();
	void BumpChannelGeneration();
	UINT32 ComposeMetadataSequence(bool writing) const;
	void TeardownTexture();
	void Teardown();

	class MetadataWriteScope
	{
	public:
		explicit MetadataWriteScope(SharedFrameExporter& exporter);
		~MetadataWriteScope();

	private:
		SharedFrameExporter& m_Exporter;
	};

	unsigned int m_MonitorIndex = 0;
	std::shared_ptr<Direct3DDevice> m_Device;
	std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, SharedFrameSlotCount> m_SharedTex;
	std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, SharedFrameSlotCount> m_KeyedMutex;
	std::array<HANDLE, SharedFrameSlotCount> m_NamedNtHandle = {};
	std::array<HANDLE, SharedFrameSlotCount> m_SealedNtHandle = {};
	std::mutex m_ExportMutex;
	HANDLE m_FrameReadyEvent = nullptr;
	HANDLE m_MetaMapping = nullptr;
	struct SharedFrameMetadata* m_MetaView = nullptr;
	UINT m_NextPublishSlot = 0;

	UINT m_CachedWidth = 0;
	UINT m_CachedHeight = 0;
	DXGI_FORMAT m_CachedFormat = DXGI_FORMAT_UNKNOWN;
	// A committed display mode is only a request. The shared texture becomes
	// authoritative after the first frame from the replacement swap chain
	// arrives. Keep the expected mode and readiness state separate so
	// OpenFrameChannel() cannot expose a previous mode during that transition.
	UINT m_ExpectedWidth = 0;
	UINT m_ExpectedHeight = 0;
	DXGI_FORMAT m_ExpectedFormat = DXGI_FORMAT_UNKNOWN;
	bool m_HasExpectedMode = false;
	bool m_ModePending = false;
	UINT m_LastPendingLogWidth = 0;
	UINT m_LastPendingLogHeight = 0;
	DXGI_FORMAT m_LastPendingLogFormat = DXGI_FORMAT_UNKNOWN;
	// Metadata writes are serialized by m_ExportMutex and must be wrapped in
	// MetadataWriteScope so consumers never accept a partially updated snapshot.
	UINT16 m_ChannelGeneration = 1;
	UINT16 m_MetadataSequenceCounter = 0;

	bool m_PendingIsHdr = false;
	float m_PendingMaxNits = 0.0f;
	float m_PendingMinNits = 0.0f;
	float m_PendingMaxFALL = 0.0f;
};

}
}
