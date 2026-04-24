// =============================================================================
// vdd_capture_test
// -----------------------------------------------------------------------------
// Standalone consumer that opens the named D3D11 shared texture exported by
// ZakoVDD's SharedFrameExporter and dumps a few PNG/PPM frames to disk so we
// can verify that VDD direct-capture works end-to-end without going through
// DXGI Desktop Duplication / WGC.
//
// Build (x64 Native Tools Cmd Prompt for VS):
//   cl /std:c++17 /EHsc /W3 /O2 vdd_capture_test.cpp ^
//      d3d11.lib dxgi.lib dxguid.lib /Fe:vdd_capture_test.exe
//
// Usage:
//   vdd_capture_test.exe [--monitor N] [--frames N] [--out DIR] [--timeout MS]
//
// Default: --monitor 0 --frames 5 --out . --timeout 2000
// =============================================================================

#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

struct SharedFrameMetadata
{
    UINT32 Magic;
    UINT32 Version;
    UINT32 Width;
    UINT32 Height;
    UINT32 DxgiFormat;
    UINT32 IsHdr;
    float  MaxNits;
    float  MinNits;
    float  MaxFALL;
    UINT64 FrameCounter;
    UINT64 LastPresentQpc;
};

struct Args
{
    unsigned int monitor = 0;
    int frames = 5;
    std::string out = ".";
    DWORD timeoutMs = 2000;
};

static Args ParseArgs(int argc, char** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        std::string s = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", name); exit(2); }
            return argv[++i];
        };
        if (s == "--monitor") a.monitor = static_cast<unsigned int>(std::stoul(next("--monitor")));
        else if (s == "--frames") a.frames = std::stoi(next("--frames"));
        else if (s == "--out") a.out = next("--out");
        else if (s == "--timeout") a.timeoutMs = static_cast<DWORD>(std::stoul(next("--timeout")));
        else if (s == "-h" || s == "--help")
        {
            printf("vdd_capture_test [--monitor N] [--frames N] [--out DIR] [--timeout MS]\n");
            exit(0);
        }
        else { fprintf(stderr, "Unknown arg: %s\n", s.c_str()); exit(2); }
    }
    return a;
}

static const char* DxgiFormatName(DXGI_FORMAT f)
{
    switch (f)
    {
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        default: return "OTHER";
    }
}

// Dump as PPM (P6) for quick visual sanity check (only handles BGRA8/RGBA8).
static bool DumpPpm(const std::string& path, const uint8_t* bgra,
                    UINT width, UINT height, UINT rowPitch, bool isBgr)
{
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    std::vector<uint8_t> row(width * 3);
    for (UINT y = 0; y < height; ++y)
    {
        const uint8_t* src = bgra + y * rowPitch;
        for (UINT x = 0; x < width; ++x)
        {
            uint8_t b = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t r = src[x * 4 + 2];
            if (isBgr)
            {
                row[x * 3 + 0] = r;
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = b;
            }
            else
            {
                row[x * 3 + 0] = b; // already RGBA, but b/r naming above
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = r;
            }
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    Args args = ParseArgs(argc, argv);

    std::wstring metaName  = L"Global\\ZakoVDD_Meta_"       + std::to_wstring(args.monitor);
    std::wstring evName    = L"Global\\ZakoVDD_FrameReady_" + std::to_wstring(args.monitor);
    std::wstring texName   = L"Global\\ZakoVDD_Frame_"      + std::to_wstring(args.monitor);

    printf("[vdd_capture_test] monitor=%u frames=%d out=%s timeout=%ums\n",
           args.monitor, args.frames, args.out.c_str(), args.timeoutMs);

    // Open metadata mapping
    HANDLE hMeta = OpenFileMappingW(FILE_MAP_READ, FALSE, metaName.c_str());
    if (!hMeta)
    {
        fprintf(stderr, "OpenFileMappingW(%ls) failed: %lu (driver running? monitor active?)\n",
                metaName.c_str(), GetLastError());
        return 1;
    }
    auto* meta = static_cast<SharedFrameMetadata*>(
        MapViewOfFile(hMeta, FILE_MAP_READ, 0, 0, sizeof(SharedFrameMetadata)));
    if (!meta)
    {
        fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(hMeta);
        return 1;
    }

    if (meta->Magic != 0x5A564446)
    {
        fprintf(stderr, "Bad metadata magic: 0x%08X (expected 0x5A564446)\n", meta->Magic);
        return 1;
    }
    printf("[meta] %ux%u fmt=%s(%u) hdr=%u maxNits=%.1f frameCounter=%llu\n",
           meta->Width, meta->Height, DxgiFormatName((DXGI_FORMAT)meta->DxgiFormat),
           meta->DxgiFormat, meta->IsHdr, meta->MaxNits,
           static_cast<unsigned long long>(meta->FrameCounter));

    // Open frame-ready event
    HANDLE hEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, evName.c_str());
    if (!hEvent)
    {
        fprintf(stderr, "OpenEventW(%ls) failed: %lu\n", evName.c_str(), GetLastError());
        return 1;
    }

    // Enumerate all DXGI hardware adapters and try opening the shared texture
    // on each. The producer-side D3D device is on a specific RenderAdapter
    // assigned by IddCx; we have no way to know its LUID without extending the
    // metadata, so brute-force across adapters until one succeeds.
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        fprintf(stderr, "CreateDXGIFactory1 failed: 0x%08X\n", hr);
        return 1;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> sharedTex;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;

    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        wprintf(L"[adapter %u] %ls LUID=%lx:%lx\n", i, ad.Description, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);

        device.Reset(); ctx.Reset(); device1.Reset(); sharedTex.Reset();
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION,
                               &device, &fl, &ctx);
        if (FAILED(hr)) { fprintf(stderr, "  D3D11CreateDevice failed: 0x%08X\n", hr); continue; }
        if (FAILED(device.As(&device1)) || !device1) continue;

        hr = device1->OpenSharedResourceByName(texName.c_str(),
                                               DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                               IID_PPV_ARGS(&sharedTex));
        if (SUCCEEDED(hr) && sharedTex)
        {
            wprintf(L"  OpenSharedResourceByName OK on adapter %u\n", i);
            break;
        }
        fprintf(stderr, "  OpenSharedResourceByName failed: 0x%08X\n", hr);
        sharedTex.Reset();
    }

    if (!sharedTex)
    {
        fprintf(stderr, "FATAL: no adapter could open %ls\n", texName.c_str());
        return 1;
    }

    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(sharedTex.As(&mutex)) || !mutex)
    {
        fprintf(stderr, "QueryInterface IDXGIKeyedMutex failed\n");
        return 1;
    }

    D3D11_TEXTURE2D_DESC desc{};
    sharedTex->GetDesc(&desc);
    printf("[shared] tex %ux%u fmt=%s(%u) bind=0x%X misc=0x%X\n",
           desc.Width, desc.Height, DxgiFormatName(desc.Format),
           desc.Format, desc.BindFlags, desc.MiscFlags);

    // Create CPU-readable staging texture for download
    D3D11_TEXTURE2D_DESC stageDesc = desc;
    stageDesc.Usage = D3D11_USAGE_STAGING;
    stageDesc.BindFlags = 0;
    stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stageDesc.MiscFlags = 0;
    stageDesc.SampleDesc = { 1, 0 };
    ComPtr<ID3D11Texture2D> stage;
    hr = device->CreateTexture2D(&stageDesc, nullptr, &stage);
    if (FAILED(hr))
    {
        fprintf(stderr, "CreateTexture2D(staging) failed: 0x%08X\n", hr);
        return 1;
    }

    int captured = 0;
    auto t0 = std::chrono::steady_clock::now();
    UINT64 lastCounter = meta->FrameCounter;

    while (captured < args.frames)
    {
        DWORD wr = WaitForSingleObject(hEvent, args.timeoutMs);
        if (wr == WAIT_TIMEOUT)
        {
            fprintf(stderr, "[wait] timeout (%ums) after %d frames; lastCounter=%llu\n",
                    args.timeoutMs, captured,
                    static_cast<unsigned long long>(meta->FrameCounter));
            break;
        }
        if (wr != WAIT_OBJECT_0)
        {
            fprintf(stderr, "WaitForSingleObject failed: %lu\n", GetLastError());
            break;
        }

        // Acquire key 1 (producer released as 1)
        hr = mutex->AcquireSync(1, args.timeoutMs);
        if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
        {
            fprintf(stderr, "[acquire] keyed mutex timeout; producer stuck?\n");
            continue;
        }
        if (FAILED(hr))
        {
            fprintf(stderr, "AcquireSync failed: 0x%08X\n", hr);
            break;
        }

        ctx->CopyResource(stage.Get(), sharedTex.Get());

        // Release back to producer ASAP
        mutex->ReleaseSync(0);

        D3D11_MAPPED_SUBRESOURCE m{};
        hr = ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr))
        {
            fprintf(stderr, "Map(staging) failed: 0x%08X\n", hr);
            break;
        }

        UINT64 cnt = meta->FrameCounter;
        char path[512];
        snprintf(path, sizeof(path), "%s/vdd_frame_%03d.ppm", args.out.c_str(), captured);

        bool isBgr = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
        bool dumped = false;
        if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            dumped = DumpPpm(path, static_cast<const uint8_t*>(m.pData),
                             desc.Width, desc.Height, m.RowPitch, isBgr);
        }
        else
        {
            // 10/16 bpc HDR: dump raw bytes for now
            snprintf(path, sizeof(path), "%s/vdd_frame_%03d.raw", args.out.c_str(), captured);
            std::ofstream raw(path, std::ios::binary);
            for (UINT y = 0; y < desc.Height; ++y)
            {
                raw.write(reinterpret_cast<const char*>(m.pData) + y * m.RowPitch,
                          (desc.Width * 4 * (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2 : 1)));
            }
            dumped = true;
        }

        ctx->Unmap(stage.Get(), 0);

        UINT64 dCnt = cnt - lastCounter;
        lastCounter = cnt;
        printf("[frame %3d] dumped=%s path=%s frameCounter=%llu (+%llu)\n",
               captured, dumped ? "OK" : "FAIL", path,
               static_cast<unsigned long long>(cnt),
               static_cast<unsigned long long>(dCnt));

        ++captured;
    }

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("[done] captured=%d in %.3fs (%.1f fps)\n", captured, sec, captured / (sec > 0 ? sec : 1.0));

    UnmapViewOfFile(meta);
    CloseHandle(hMeta);
    CloseHandle(hEvent);
    return captured > 0 ? 0 : 1;
}
