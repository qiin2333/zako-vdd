#pragma once

#include "Direct3DDevice.h"

#include <memory>
#include <mutex>

namespace Microsoft
{
namespace IndirectDisp
{

class SharedFrameExporter
{
public:
	SharedFrameExporter(unsigned int monitorIndex, std::shared_ptr<Direct3DDevice> device);
	~SharedFrameExporter();

	void PushFrame(IDXGIResource* acquired,
	               UINT64 presentDisplayQpc = 0,
	               UINT presentationFrameNumber = 0,
	               UINT dirtyRectCount = 0);
	void UpdateHdrMetadata(bool isHdr, float maxNits, float minNits, float maxFALL);
	void PublishModeMetadata(UINT width, UINT height);

private:
	DXGI_FORMAT GuessMetadataFormat() const;
	bool EnsureSharedTexture(const D3D11_TEXTURE2D_DESC& srcDesc);
	bool EnsureEventAndMetadata(const D3D11_TEXTURE2D_DESC& srcDesc);
	void TeardownTexture();
	void Teardown();

	unsigned int m_MonitorIndex = 0;
	std::shared_ptr<Direct3DDevice> m_Device;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_SharedTex;
	Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_KeyedMutex;
	std::mutex m_ExportMutex;
	HANDLE m_NtHandle = nullptr;
	HANDLE m_FrameReadyEvent = nullptr;
	HANDLE m_MetaMapping = nullptr;
	struct SharedFrameMetadata* m_MetaView = nullptr;

	UINT m_CachedWidth = 0;
	UINT m_CachedHeight = 0;
	DXGI_FORMAT m_CachedFormat = DXGI_FORMAT_UNKNOWN;

	bool m_PendingIsHdr = false;
	float m_PendingMaxNits = 0.0f;
	float m_PendingMinNits = 0.0f;
	float m_PendingMaxFALL = 0.0f;
};

}
}
