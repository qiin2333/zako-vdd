#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../vulkan_hdr_probe/vulkan_minimal.h"
#include "../../Common/Include/vulkan_hdr_capability_cache.h"
#include "../../Common/Include/vulkan_hdr_policy.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr VkResult VK_ERROR_INITIALIZATION_FAILED = -3;
constexpr VkStructureType VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR = 1000119002;
constexpr VkStructureType VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO = 47;
constexpr std::int32_t VK_LAYER_LINK_INFO = 0;

struct VkAllocationCallbacks;

struct VkLayerInstanceLink {
  VkLayerInstanceLink* pNext;
  PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
  PFN_vkVoidFunction pfnNextGetPhysicalDeviceProcAddr;
};

struct VkLayerInstanceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  std::int32_t function;
  union {
    VkLayerInstanceLink* pLayerInfo;
    void* other;
  } u;
};

using PFN_vkDestroyInstance = void(VKAPI_PTR*)(VkInstance, const VkAllocationCallbacks*);
using PFN_vkCreateInstance = VkResult(VKAPI_PTR*)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
using PFN_vkCreateWin32SurfaceKHR = VkResult(VKAPI_PTR*)(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
using PFN_vkDestroySurfaceKHR = void(VKAPI_PTR*)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);

struct InstanceDispatch {
  PFN_vkGetInstanceProcAddr next_gipa {};
  PFN_vkDestroyInstance destroy_instance {};
  PFN_vkCreateWin32SurfaceKHR create_surface {};
  PFN_vkDestroySurfaceKHR destroy_surface {};
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_formats {};
  PFN_vkGetPhysicalDeviceSurfaceFormats2KHR get_formats2 {};
  PFN_vkGetPhysicalDeviceProperties2 get_properties2 {};
};

struct AllowedPairs {
  bool hdr10 {};
  bool scrgb {};
};

std::mutex g_mutex;
std::unordered_map<void*, InstanceDispatch> g_instances;
std::unordered_map<VkSurfaceKHR, HWND> g_surface_windows;

void* dispatch_key(const void* dispatchable) {
  return dispatchable ? *reinterpret_cast<void* const*>(dispatchable) : nullptr;
}

bool get_dispatch(const void* dispatchable, InstanceDispatch& output) {
  const auto key = dispatch_key(dispatchable);
  if (!key) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto found = g_instances.find(key);
  if (found == g_instances.end()) return false;
  output = found->second;
  return true;
}

bool env_enabled(const char* name) {
  char value[8] {};
  return GetEnvironmentVariableA(name, value, static_cast<DWORD>(sizeof(value))) > 0 && value[0] == '1';
}

std::uint32_t windows_build_number() {
  struct VersionInfo {
    ULONG size;
    ULONG major;
    ULONG minor;
    ULONG build;
    ULONG platform;
    WCHAR service_pack[128];
  } version {sizeof(VersionInfo)};
  using RtlGetVersionFn = LONG(WINAPI*)(VersionInfo*);
  const auto ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto rtl_get_version = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  return rtl_get_version && rtl_get_version(&version) == 0 ? version.build : 0;
}

std::wstring capability_cache_path() {
  wchar_t override_path[32768] {};
  const DWORD override_length = GetEnvironmentVariableW(L"ZAKO_VHDR_CAPABILITY_CACHE", override_path,
                                                         static_cast<DWORD>(std::size(override_path)));
  if (override_length > 0 && override_length < std::size(override_path)) return override_path;

  wchar_t local_app_data[32768] {};
  const DWORD local_app_data_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
                                                               static_cast<DWORD>(std::size(local_app_data)));
  if (local_app_data_length > 0 && local_app_data_length < std::size(local_app_data)) {
    return (std::filesystem::path(local_app_data) / L"Sunshine" /
            L"zako_vulkan_hdr_capabilities.bin").wstring();
  }

  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&capability_cache_path), &module)) return {};
  wchar_t module_path[32768] {};
  const DWORD length = GetModuleFileNameW(module, module_path, static_cast<DWORD>(std::size(module_path)));
  if (length == 0 || length >= std::size(module_path)) return {};
  return (std::filesystem::path(module_path).parent_path() / L"zako_vulkan_hdr_capabilities.bin").wstring();
}

AllowedPairs verified_pairs(VkPhysicalDevice device, const InstanceDispatch& dispatch) {
  if (env_enabled("ZAKO_VHDR_FORCE")) return {true, true};
  if (!dispatch.get_properties2) return {};

  VkPhysicalDeviceIDProperties id {};
  id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2Probe properties {};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &id;
  dispatch.get_properties2(device, &properties);

  zako::vulkan_hdr::CapabilityCacheEntry key {};
  key.vendor_id = properties.properties.vendorID;
  key.device_id = properties.properties.deviceID;
  key.driver_version = properties.properties.driverVersion;
  key.api_version = properties.properties.apiVersion;
  key.windows_build = windows_build_number();
  if (id.deviceLUIDValid) {
    std::memcpy(&key.adapter_luid_low, id.deviceLUID, sizeof(key.adapter_luid_low));
    std::memcpy(&key.adapter_luid_high, id.deviceLUID + sizeof(key.adapter_luid_low), sizeof(key.adapter_luid_high));
  }

  const auto path = capability_cache_path();
  if (path.empty()) return {};
  const auto entries = zako::vulkan_hdr::read_capability_cache(path);
  const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return zako::vulkan_hdr::same_capability_key(entry, key);
  });
  const auto required = zako::vulkan_hdr::kCapabilityValidatedWhileHdrActive |
                        zako::vulkan_hdr::kCapabilityPresented;
  if (found == entries.end() || (found->flags & required) != required) return {};
  return {(found->flags & zako::vulkan_hdr::kCapabilityHdr10Swapchain) != 0,
          (found->flags & zako::vulkan_hdr::kCapabilityScRgbSwapchain) != 0};
}

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
}

bool looks_like_zako_target(const DISPLAYCONFIG_TARGET_DEVICE_NAME& target) {
  const auto friendly = lowercase(target.monitorFriendlyDeviceName);
  const auto path = lowercase(target.monitorDevicePath);
  return friendly.find(L"zako") != std::wstring::npos ||
         path.find(L"zakovdd") != std::wstring::npos ||
         path.find(L"#zak") != std::wstring::npos;
}

bool target_hdr_enabled(const DISPLAYCONFIG_PATH_INFO& path) {
  struct AdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
      struct {
        UINT32 advancedColorSupported : 1;
        UINT32 advancedColorActive : 1;
        UINT32 reserved1 : 1;
        UINT32 advancedColorLimitedByPolicy : 1;
        UINT32 highDynamicRangeSupported : 1;
        UINT32 highDynamicRangeUserEnabled : 1;
        UINT32 wideColorSupported : 1;
        UINT32 wideColorUserEnabled : 1;
        UINT32 reserved : 24;
      } bits;
      UINT32 value;
    } state;
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;
  } info2 {};
  info2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
  info2.header.size = sizeof(info2);
  info2.header.adapterId = path.targetInfo.adapterId;
  info2.header.id = path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
    return info2.state.bits.highDynamicRangeSupported && info2.state.bits.highDynamicRangeUserEnabled;
  }

  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info1 {};
  info1.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  info1.header.size = sizeof(info1);
  info1.header.adapterId = path.targetInfo.adapterId;
  info1.header.id = path.targetInfo.id;
  return DisplayConfigGetDeviceInfo(&info1.header) == ERROR_SUCCESS &&
         info1.advancedColorSupported && info1.advancedColorEnabled;
}

bool should_bridge(HWND window) {
  if (!window) return false;
  if (env_enabled("ZAKO_VHDR_FORCE") || env_enabled("ZAKO_VHDR_FORCE_SURFACE")) return true;

  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW monitor_info {};
  monitor_info.cbSize = sizeof(monitor_info);
  if (!monitor || !GetMonitorInfoW(monitor, &monitor_info)) return false;

  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) return false;

  for (UINT32 index = 0; index < path_count; ++index) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[index].sourceInfo.adapterId;
    source.header.id = paths[index].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        std::wcscmp(source.viewGdiDeviceName, monitor_info.szDevice) != 0) continue;

    DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = paths[index].targetInfo.adapterId;
    target.header.id = paths[index].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) return false;
    if (!env_enabled("ZAKO_VHDR_ALLOW_ANY_HDR") && !looks_like_zako_target(target)) return false;
    return target_hdr_enabled(paths[index]);
  }
  return false;
}

bool should_bridge_surface(VkSurfaceKHR surface) {
  HWND window = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto found = g_surface_windows.find(surface);
    if (found == g_surface_windows.end()) return false;
    window = found->second;
  }
  return should_bridge(window);
}

void append_verified_pairs(std::vector<VkSurfaceFormatKHR>& formats, AllowedPairs allowed) {
  std::vector<zako::vulkan_hdr::FormatPair> pairs;
  pairs.reserve(formats.size() + 2);
  for (const auto& entry : formats) pairs.push_back({entry.format, entry.colorSpace});
  zako::vulkan_hdr::append_missing_verified_pairs(pairs, allowed.hdr10, allowed.scrgb);
  formats.clear();
  formats.reserve(pairs.size());
  for (const auto& pair : pairs) formats.push_back({pair.format, pair.color_space});
}

VkResult VKAPI_PTR layer_create_surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info,
                                        const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
  InstanceDispatch dispatch;
  if (!info || !surface || !get_dispatch(instance, dispatch) || !dispatch.create_surface) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = dispatch.create_surface(instance, info, allocator, surface);
  if (result == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_surface_windows[*surface] = static_cast<HWND>(info->hwnd);
  }
  return result;
}

void VKAPI_PTR layer_destroy_surface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator) {
  InstanceDispatch dispatch;
  const bool have_dispatch = get_dispatch(instance, dispatch);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_surface_windows.erase(surface);
  }
  if (have_dispatch && dispatch.destroy_surface) dispatch.destroy_surface(instance, surface, allocator);
}

VkResult VKAPI_PTR layer_get_formats(VkPhysicalDevice device, VkSurfaceKHR surface, std::uint32_t* count,
                                     VkSurfaceFormatKHR* output) {
  InstanceDispatch dispatch;
  if (!count || !get_dispatch(device, dispatch) || !dispatch.get_formats) return VK_ERROR_INITIALIZATION_FAILED;
  if (!should_bridge_surface(surface)) return dispatch.get_formats(device, surface, count, output);
  const auto allowed = verified_pairs(device, dispatch);
  if (!allowed.hdr10 && !allowed.scrgb) return dispatch.get_formats(device, surface, count, output);

  std::uint32_t base_count = 0;
  VkResult result = dispatch.get_formats(device, surface, &base_count, nullptr);
  if (result != VK_SUCCESS) return dispatch.get_formats(device, surface, count, output);
  std::vector<VkSurfaceFormatKHR> formats(base_count);
  result = dispatch.get_formats(device, surface, &base_count, formats.data());
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) return dispatch.get_formats(device, surface, count, output);
  formats.resize(base_count);
  append_verified_pairs(formats, allowed);

  if (!output) {
    *count = static_cast<std::uint32_t>(formats.size());
    return VK_SUCCESS;
  }
  const auto total = static_cast<std::uint32_t>(formats.size());
  const auto copied = std::min(*count, total);
  std::memcpy(output, formats.data(), copied * sizeof(VkSurfaceFormatKHR));
  *count = copied;
  return copied < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_PTR layer_get_formats2(VkPhysicalDevice device, const VkPhysicalDeviceSurfaceInfo2KHR* surface_info,
                                      std::uint32_t* count, VkSurfaceFormat2KHR* output) {
  InstanceDispatch dispatch;
  if (!count || !get_dispatch(device, dispatch) || !dispatch.get_formats2) return VK_ERROR_INITIALIZATION_FAILED;
  if (!surface_info || !should_bridge_surface(surface_info->surface)) return dispatch.get_formats2(device, surface_info, count, output);
  const auto allowed = verified_pairs(device, dispatch);
  if (!allowed.hdr10 && !allowed.scrgb) return dispatch.get_formats2(device, surface_info, count, output);

  std::uint32_t base_count = 0;
  VkResult result = dispatch.get_formats2(device, surface_info, &base_count, nullptr);
  if (result != VK_SUCCESS) return dispatch.get_formats2(device, surface_info, count, output);
  std::vector<VkSurfaceFormat2KHR> base(base_count);
  for (auto& entry : base) {
    entry.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
    entry.pNext = nullptr;
  }
  result = dispatch.get_formats2(device, surface_info, &base_count, base.data());
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) return dispatch.get_formats2(device, surface_info, count, output);
  std::vector<VkSurfaceFormatKHR> formats;
  formats.reserve(base_count + 2);
  for (std::uint32_t index = 0; index < base_count; ++index) formats.push_back(base[index].surfaceFormat);
  append_verified_pairs(formats, allowed);

  if (!output) {
    *count = static_cast<std::uint32_t>(formats.size());
    return VK_SUCCESS;
  }
  const auto total = static_cast<std::uint32_t>(formats.size());
  const auto copied = std::min(*count, total);
  for (std::uint32_t index = 0; index < copied; ++index) output[index].surfaceFormat = formats[index];
  *count = copied;
  return copied < total ? VK_INCOMPLETE : VK_SUCCESS;
}

void VKAPI_PTR layer_destroy_instance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  PFN_vkDestroyInstance next = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto found = g_instances.find(dispatch_key(instance));
    if (found != g_instances.end()) {
      next = found->second.destroy_instance;
      g_instances.erase(found);
    }
  }
  if (next) next(instance, allocator);
}

VkResult VKAPI_PTR layer_create_instance(const VkInstanceCreateInfo* create_info,
                                         const VkAllocationCallbacks* allocator, VkInstance* instance) {
  if (!create_info || !instance) return VK_ERROR_INITIALIZATION_FAILED;
  auto* chain = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(create_info->pNext));
  while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) {
    chain = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
  }
  if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
  const auto next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
  const auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(nullptr, "vkCreateInstance"));
  if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = next_create(create_info, allocator, instance);
  if (result != VK_SUCCESS) return result;

  InstanceDispatch dispatch {};
  dispatch.next_gipa = next_gipa;
  dispatch.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*instance, "vkDestroyInstance"));
  dispatch.create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(next_gipa(*instance, "vkCreateWin32SurfaceKHR"));
  dispatch.destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(next_gipa(*instance, "vkDestroySurfaceKHR"));
  dispatch.get_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(next_gipa(*instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
  dispatch.get_formats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(next_gipa(*instance, "vkGetPhysicalDeviceSurfaceFormats2KHR"));
  dispatch.get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(next_gipa(*instance, "vkGetPhysicalDeviceProperties2"));
  if (!dispatch.get_properties2) {
    dispatch.get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(next_gipa(*instance, "vkGetPhysicalDeviceProperties2KHR"));
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances[dispatch_key(*instance)] = dispatch;
  }
  return VK_SUCCESS;
}

}  // namespace

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_PTR vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (!name) return nullptr;
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&layer_create_instance);
  if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&layer_destroy_instance);
  if (std::strcmp(name, "vkCreateWin32SurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&layer_create_surface);
  if (std::strcmp(name, "vkDestroySurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&layer_destroy_surface);
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&layer_get_formats);
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&layer_get_formats2);
  InstanceDispatch dispatch;
  return instance && get_dispatch(instance, dispatch) && dispatch.next_gipa ? dispatch.next_gipa(instance, name) : nullptr;
}
