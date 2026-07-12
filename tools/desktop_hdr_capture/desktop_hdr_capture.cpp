#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

float half_to_float(std::uint16_t value) {
  const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
  std::uint32_t exponent = (value >> 10) & 0x1fu;
  std::uint32_t mantissa = value & 0x3ffu;
  std::uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) bits = sign;
    else {
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) { mantissa <<= 1; --exponent; }
      bits = sign | (exponent << 23) | ((mantissa & 0x3ffu) << 13);
    }
  } else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
  else bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  float result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

const char* format_name(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    default: return "OTHER";
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::cout << "Usage: desktop_hdr_capture.exe \\\\.\\DISPLAYn [x y] [timeout_ms]\n";
    return 2;
  }
  const std::wstring display = argv[1];
  const UINT sample_x = argc > 2 ? static_cast<UINT>(std::wcstoul(argv[2], nullptr, 10)) : 100;
  const UINT sample_y = argc > 3 ? static_cast<UINT>(std::wcstoul(argv[3], nullptr, 10)) : 100;
  const UINT timeout = argc > 4 ? static_cast<UINT>(std::wcstoul(argv[4], nullptr, 10)) : 5000;

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return 3;

  ComPtr<IDXGIAdapter1> selected_adapter;
  ComPtr<IDXGIOutput> selected_output;
  DXGI_OUTPUT_DESC output_desc {};
  for (UINT adapter_index = 0; !selected_output; ++adapter_index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    for (UINT output_index = 0;; ++output_index) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC desc {};
      if (SUCCEEDED(output->GetDesc(&desc)) && _wcsicmp(desc.DeviceName, display.c_str()) == 0) {
        selected_adapter = adapter;
        selected_output = output;
        output_desc = desc;
        break;
      }
    }
  }
  if (!selected_output) {
    std::wcerr << L"error=output_not_found display=" << display << L'\n';
    return 4;
  }

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level {};
  hr = D3D11CreateDevice(selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                         &device, &level, &context);
  if (FAILED(hr)) {
    std::cerr << "error=D3D11CreateDevice hr=" << std::hex << hr << '\n';
    return 5;
  }

  ComPtr<IDXGIOutput5> output5;
  if (FAILED(selected_output.As(&output5))) {
    std::cerr << "error=IDXGIOutput5_unavailable\n";
    return 6;
  }
  const DXGI_FORMAT requested[] = {
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM,
  };
  ComPtr<IDXGIOutputDuplication> duplication;
  hr = output5->DuplicateOutput1(device.Get(), 0, static_cast<UINT>(std::size(requested)), requested, &duplication);
  if (FAILED(hr)) {
    std::cerr << "error=DuplicateOutput1 hr=0x" << std::hex << hr << '\n';
    return 7;
  }

  DXGI_OUTDUPL_FRAME_INFO frame_info {};
  ComPtr<IDXGIResource> resource;
  hr = duplication->AcquireNextFrame(timeout, &frame_info, &resource);
  if (FAILED(hr)) {
    std::cerr << "error=AcquireNextFrame hr=0x" << std::hex << hr << '\n';
    return 8;
  }
  ComPtr<ID3D11Texture2D> texture;
  resource.As(&texture);
  D3D11_TEXTURE2D_DESC desc {};
  texture->GetDesc(&desc);
  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.MiscFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  if (FAILED(hr)) {
    duplication->ReleaseFrame();
    return 9;
  }
  context->CopyResource(staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped {};
  hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    duplication->ReleaseFrame();
    return 10;
  }

  const UINT x = std::min(sample_x, desc.Width - 1);
  const UINT y = std::min(sample_y, desc.Height - 1);
  const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
  float r = 0, g = 0, b = 0, a = 0;
  if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
    const auto* pixel = reinterpret_cast<const std::uint16_t*>(row + x * 8);
    r = half_to_float(pixel[0]); g = half_to_float(pixel[1]); b = half_to_float(pixel[2]); a = half_to_float(pixel[3]);
  } else if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
    const auto pixel = *reinterpret_cast<const std::uint32_t*>(row + x * 4);
    r = static_cast<float>(pixel & 0x3ffu) / 1023.0f;
    g = static_cast<float>((pixel >> 10) & 0x3ffu) / 1023.0f;
    b = static_cast<float>((pixel >> 20) & 0x3ffu) / 1023.0f;
    a = static_cast<float>((pixel >> 30) & 0x3u) / 3.0f;
  } else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
    const auto* pixel = row + x * 4;
    b = pixel[0] / 255.0f; g = pixel[1] / 255.0f; r = pixel[2] / 255.0f; a = pixel[3] / 255.0f;
  }
  context->Unmap(staging.Get(), 0);
  duplication->ReleaseFrame();

  std::cout << "display=";
  std::wcout << display;
  std::cout << " desktop_bounds=" << output_desc.DesktopCoordinates.left << ',' << output_desc.DesktopCoordinates.top
            << ',' << output_desc.DesktopCoordinates.right << ',' << output_desc.DesktopCoordinates.bottom
            << " texture=" << desc.Width << 'x' << desc.Height << " format=" << format_name(desc.Format)
            << '(' << desc.Format << ") sample=" << x << ',' << y
            << " rgba=" << r << ',' << g << ',' << b << ',' << a << '\n';
  if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
    std::cout << "scrgb_1000nit_patch_match=" << (r > 10.0f && g > 10.0f && b > 10.0f) << '\n';
  }
  return 0;
}
