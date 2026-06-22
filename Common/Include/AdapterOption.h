#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include <algorithm>		// For sort
#include <dxgi.h>		// For IDXGIAdapter, IDXGIFactory1
#include <fstream>
#include <string>
#include <vector>
#include <wrl/client.h>	// For ComPtr

using namespace std;
using namespace Microsoft::WRL;

// Structure to vector gpus
struct GPUInfo {
    wstring name;                // GPU name
    ComPtr<IDXGIAdapter> adapter;// COM pointer to the adapter
    DXGI_ADAPTER_DESC desc;      // Adapter description
};

// Sort function for GPUs by dedicated video memory
inline bool CompareGPUs(const GPUInfo& a, const GPUInfo& b) {
    return a.desc.DedicatedVideoMemory > b.desc.DedicatedVideoMemory;
}

// Get a enumerate list of available GPUs
inline vector<GPUInfo> getAvailableGPUs() {
    vector<GPUInfo> gpus; // Vector to hold all GPU's information

    ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return gpus;
    }

    // Enumerate all adapters (GPUs)
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter> adapter;
        if (!SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
            break;
        }

        DXGI_ADAPTER_DESC desc;

        if (!SUCCEEDED(adapter->GetDesc(&desc))) {
            continue;
        }

        // Add the adapter information to the list
        GPUInfo info{ desc.Description, adapter, desc };
        gpus.push_back(info);
    }

    return gpus;
}

class AdapterOption {
public:
    bool hasTargetAdapter{}; // Indicates if a target adapter is selected
    LUID adapterLuid{};      // Adapter's unique identifier (LUID)
    wstring target_name{};   // Target adapter name

    // Select the best GPU based on dedicated video memory
    wstring selectBestGPU() {
        auto gpus = getAvailableGPUs();
        if (gpus.empty()) {
            return L""; // Error check for headless / vm
        }

        // Sort GPUs by dedicated video memory in descending order
        sort(gpus.begin(), gpus.end(), CompareGPUs);
        auto bestGPU = gpus.front(); // Get the GPU with the most memory

        return bestGPU.name;
    }

    // Load friendlyname from a file OR select the best GPU 
    void load(const wchar_t* path) {
        ifstream ifs{ path };

        if (!ifs.is_open()) {
            target_name = selectBestGPU();
        }
        else {
            string line;
            getline(ifs, line);
            target_name.assign(line.begin(), line.end());
        }

        // Find and set the adapter based on the target name
        if (!findAndSetAdapter(target_name)) {
            // If the adapter is not found, select the best GPU and retry
            target_name = selectBestGPU();
            findAndSetAdapter(target_name);
        }
    }

    // Set the target adapter from a given name and validate it
    void xmlprovide(const wstring& xtarg) {
        target_name = xtarg;
        if (!findAndSetAdapter(target_name)) {
            // If the adapter is not found, select the best GPU and retry
            target_name = selectBestGPU();
            findAndSetAdapter(target_name);
        }
    }

    // Apply the adapter settings to the specified adapter
    void apply(const IDDCX_ADAPTER& adapter) {
        if (hasTargetAdapter && IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) {
            IDARG_IN_ADAPTERSETRENDERADAPTER arg{};
            arg.PreferredRenderAdapter = adapterLuid;
            IddCxAdapterSetRenderAdapter(adapter, &arg);
        }
    }

private:
    // Find and set the adapter by its name
    bool findAndSetAdapter(const wstring& adapterName) {
        auto gpus = getAvailableGPUs();

        // Iterate through all available GPUs
        for (const auto& gpu : gpus) {
            if (_wcsicmp(gpu.name.c_str(), adapterName.c_str()) == 0) {
                adapterLuid = gpu.desc.AdapterLuid; // Set the adapter LUID
                hasTargetAdapter = true; // Indicate that a target adapter is selected
                return true;
            }
        }

        hasTargetAdapter = false; // Indicate that no target adapter is selected
        return false;
    }
};
