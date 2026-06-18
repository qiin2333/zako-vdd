#include "Direct3DDevice.h"

#include "..\Logging\Logger.h"

using namespace std;

namespace Microsoft
{
namespace IndirectDisp
{

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice() : AdapterLuid{}
{
}

HRESULT Direct3DDevice::Init()
{
	HRESULT hr;

	// Recreate the factory so newly attached render adapters can be discovered.
	VDD_LOG_DEBUG("Initializing Direct3DDevice...");

	hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		VDD_LOG_ERROR_STREAM("Failed to create DXGI factory. HRESULT: " << hr);
		return hr;
	}
	VDD_LOG_DEBUG("DXGI factory created successfully.");

	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		VDD_LOG_ERROR_STREAM("Failed to enumerate adapter by LUID. HRESULT: " << hr);
		return hr;
	}

	DXGI_ADAPTER_DESC desc;
	Adapter->GetDesc(&desc);
	VDD_LOG_INFO_STREAM("Adapter found: " << desc.Description << " (Vendor ID: " << desc.VendorId << ", Device ID: " << desc.DeviceId << ")");

	D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL featureLevel;

	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &Device, &featureLevel, &DeviceContext);
	if (FAILED(hr))
	{
		VDD_LOG_ERROR_STREAM("Failed to create Direct3D device. HRESULT: " << hr);
		VDD_LOG_ERROR_STREAM("If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the system is in a transient state. " << hr);
		return hr;
	}

	VDD_LOG_INFO_STREAM("Direct3D device created successfully. Feature Level: " << featureLevel);

	return S_OK;
}

}
}
