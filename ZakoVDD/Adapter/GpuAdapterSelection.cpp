#include "GpuAdapterSelection.h"

#include "..\Driver.h"
#include "..\Logging\Logger.h"

#include <AdapterOption.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

using namespace std;
using namespace Microsoft::WRL;

static string WideToUtf8(const wstring &value)
{
	if (value.empty())
	{
		return "";
	}

	int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (sizeNeeded <= 0)
	{
		return "";
	}

	string converted(sizeNeeded, 0);
	int convertedSize = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &converted[0], sizeNeeded, nullptr, nullptr);
	if (convertedSize <= 0)
	{
		return "";
	}

	return converted;
}

static bool SameAdapterLuid(const LUID &left, const LUID &right)
{
	return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

bool SameGpuName(const wstring &left, const wstring &right)
{
	return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

static bool HasExplicitGpuPreference(const wstring &gpuName)
{
	return !gpuName.empty() && !SameGpuName(gpuName, L"default");
}

wstring ReadAdapterPreferenceFile(const wstring &path)
{
	ifstream ifs{path.c_str()};
	if (!ifs.is_open())
	{
		return L"";
	}

	string line;
	getline(ifs, line);
	return wstring(line.begin(), line.end());
}

static string FormatAdapterLogName(const wstring &adapterName, const LUID &adapterLuid)
{
	return WideToUtf8(adapterName) + " (LUID: " +
	       to_string(adapterLuid.LowPart) + "-" + to_string(adapterLuid.HighPart) + ")";
}

static bool IsSoftwareAdapter(const ComPtr<IDXGIAdapter> &adapter)
{
	ComPtr<IDXGIAdapter1> adapter1;
	if (FAILED(adapter.As(&adapter1)))
	{
		return false;
	}

	DXGI_ADAPTER_DESC1 desc1 = {};
	if (FAILED(adapter1->GetDesc1(&desc1)))
	{
		return false;
	}

	return (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
}

static bool CanCreateD3DDeviceOnAdapter(IDXGIAdapter *adapter, HRESULT *failureHr = nullptr)
{
	if (adapter == nullptr)
	{
		if (failureHr != nullptr)
		{
			*failureHr = E_POINTER;
		}
		return false;
	}

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL featureLevel;
	D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

	HRESULT hr = D3D11CreateDevice(
		adapter,
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		featureLevels,
		ARRAYSIZE(featureLevels),
		D3D11_SDK_VERSION,
		&device,
		&featureLevel,
		&context);

	if (failureHr != nullptr)
	{
		*failureHr = hr;
	}

	return SUCCEEDED(hr);
}

static bool CanCreateD3DDeviceOnAdapterLuid(const LUID &adapterLuid, HRESULT *failureHr = nullptr)
{
	ComPtr<IDXGIFactory5> factory;
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	if (FAILED(hr))
	{
		if (failureHr != nullptr)
		{
			*failureHr = hr;
		}
		return false;
	}

	ComPtr<IDXGIAdapter> adapter;
	hr = factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter));
	if (FAILED(hr))
	{
		if (failureHr != nullptr)
		{
			*failureHr = hr;
		}
		return false;
	}

	return CanCreateD3DDeviceOnAdapter(adapter.Get(), failureHr);
}

static bool SelectBestUsableRenderAdapter(AdapterOption &adapterOption, const LUID *rejectedLuid = nullptr)
{
	vector<GPUInfo> gpus = getAvailableGPUs();
	sort(gpus.begin(), gpus.end(), CompareGPUs);

	for (int pass = 0; pass < 2; ++pass)
	{
		const bool allowSoftwareAdapter = (pass == 1);

		for (const auto &gpu : gpus)
		{
			if (rejectedLuid != nullptr && SameAdapterLuid(gpu.desc.AdapterLuid, *rejectedLuid))
			{
				continue;
			}

			if (!allowSoftwareAdapter && IsSoftwareAdapter(gpu.adapter))
			{
				continue;
			}

			HRESULT hr = S_OK;
			if (!CanCreateD3DDeviceOnAdapter(gpu.adapter.Get(), &hr))
			{
				string logText = "Skipping GPU fallback candidate because D3D11 device creation failed: " +
				                 FormatAdapterLogName(gpu.name, gpu.desc.AdapterLuid) +
				                 ", HRESULT: " + to_string(hr);
				VDD_LOG_WARNING(logText.c_str());
				continue;
			}

			adapterOption.target_name = gpu.name;
			adapterOption.adapterLuid = gpu.desc.AdapterLuid;
			adapterOption.hasTargetAdapter = true;

			string logText = string(allowSoftwareAdapter ? "Selected software GPU fallback: " : "Selected hardware GPU fallback: ") +
			                 FormatAdapterLogName(adapterOption.target_name, adapterOption.adapterLuid);
			VDD_LOG(allowSoftwareAdapter ? VddLogLevel::Warning : VddLogLevel::Info, logText.c_str());
			return true;
		}
	}

	adapterOption.target_name = L"";
	adapterOption.adapterLuid = {};
	adapterOption.hasTargetAdapter = false;
	VDD_LOG_ERROR("No usable render adapter fallback was found.");
	return false;
}

void EnsureUsableRenderAdapter(AdapterOption &adapterOption, const wstring &requestedGpuName)
{
	const bool hasExplicitPreference = HasExplicitGpuPreference(requestedGpuName);

	if (adapterOption.hasTargetAdapter)
	{
		HRESULT hr = S_OK;
		if (CanCreateD3DDeviceOnAdapterLuid(adapterOption.adapterLuid, &hr))
		{
			if (hasExplicitPreference && !SameGpuName(requestedGpuName, adapterOption.target_name))
			{
				string logText = "Configured GPU is unavailable, using runtime fallback instead. Requested: " +
				                 WideToUtf8(requestedGpuName) + ", selected: " +
				                 FormatAdapterLogName(adapterOption.target_name, adapterOption.adapterLuid);
				VDD_LOG_WARNING(logText.c_str());
			}
			return;
		}

		string logText = "Selected GPU cannot create a D3D11 device, attempting runtime fallback. Selected: " +
		                 FormatAdapterLogName(adapterOption.target_name, adapterOption.adapterLuid) +
		                 ", HRESULT: " + to_string(hr);
		VDD_LOG_WARNING(logText.c_str());

		LUID rejectedLuid = adapterOption.adapterLuid;
		SelectBestUsableRenderAdapter(adapterOption, &rejectedLuid);
		return;
	}

	if (hasExplicitPreference)
	{
		string logText = "Configured GPU is unavailable, attempting runtime fallback. Requested: " +
		                 WideToUtf8(requestedGpuName);
		VDD_LOG_WARNING(logText.c_str());
	}

	SelectBestUsableRenderAdapter(adapterOption);
}

void LogAvailableGPUs()
{
	vector<GPUInfo> gpus = getAvailableGPUs();

	for (const auto &gpu : gpus)
	{
		wstring logMessage = L"GPU Name: ";
		logMessage += gpu.desc.Description;
		wstring memorySize = L" Memory: ";
		memorySize += to_wstring(gpu.desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB";
		string logText = WideToUtf8(logMessage + memorySize);
		if (!logText.empty())
		{
			VDD_LOG_INFO(logText.c_str());
		}
	}
}
