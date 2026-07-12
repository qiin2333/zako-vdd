#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "vulkan_minimal.h"
#include "../../Common/Include/vulkan_hdr_capability_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr char kSurfaceExtension[] = "VK_KHR_surface";
constexpr char kWin32SurfaceExtension[] = "VK_KHR_win32_surface";
constexpr char kSwapchainColorspaceExtension[] = "VK_EXT_swapchain_colorspace";
constexpr char kSwapchainExtension[] = "VK_KHR_swapchain";
constexpr wchar_t kImplicitLayersKey[] = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";

struct DisplayTarget {
  std::wstring gdi_name;
  RECT bounds {};
  bool hdr_supported {};
  bool hdr_enabled {};
};

struct AdvancedColorState {
  bool supported {};
  bool enabled {};
};

struct VulkanFunctions {
  HMODULE module {};
  PFN_vkGetInstanceProcAddr get_instance_proc_addr {};
  PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions {};
  PFN_vkCreateInstance create_instance {};
  PFN_vkDestroyInstance destroy_instance {};
  PFN_vkEnumeratePhysicalDevices enumerate_physical_devices {};
  PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 {};
  PFN_vkCreateWin32SurfaceKHR create_win32_surface {};
  PFN_vkDestroySurfaceKHR destroy_surface {};
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_surface_support {};
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_surface_formats {};
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities {};
  PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_families {};
  PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions {};
  PFN_vkCreateDevice create_device {};
  PFN_vkGetDeviceProcAddr get_device_proc_addr {};
  PFN_vkDestroyDevice destroy_device {};
};

struct DeviceFingerprint {
  zako::vulkan_hdr::CapabilityCacheEntry cache_entry;
  bool luid_valid {};
};

struct PresentTestResult {
  VkResult swapchain_result {-3};
  VkResult present_result {-3};
  std::uint32_t frames_presented {};
  bool metadata_submitted {};
};

std::string narrow(const std::wstring& value) {
  if (value.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
  if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
  return result;
}

AdvancedColorState query_advanced_color(const std::wstring& gdi_name) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return {};
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) return {};

  for (UINT32 i = 0; i < path_count; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || gdi_name != source.viewGdiDeviceName) continue;

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
    info2.header.adapterId = paths[i].targetInfo.adapterId;
    info2.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
      return {info2.state.bits.highDynamicRangeSupported != 0,
              info2.state.bits.highDynamicRangeUserEnabled != 0};
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info1 {};
    info1.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info1.header.size = sizeof(info1);
    info1.header.adapterId = paths[i].targetInfo.adapterId;
    info1.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&info1.header) == ERROR_SUCCESS) {
      return {info1.advancedColorSupported != 0, info1.advancedColorEnabled != 0};
    }
  }
  return {};
}

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
  auto& displays = *reinterpret_cast<std::vector<DisplayTarget>*>(context);
  MONITORINFOEXW info {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return TRUE;
  const auto color = query_advanced_color(info.szDevice);
  displays.push_back({info.szDevice, info.rcMonitor, color.supported, color.enabled});
  return TRUE;
}

std::vector<DisplayTarget> displays() {
  std::vector<DisplayTarget> result;
  EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&result));
  return result;
}

LRESULT CALLBACK probe_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}

void pump_messages_for(DWORD duration_ms) {
  const ULONGLONG deadline = GetTickCount64() + duration_ms;
  do {
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (duration_ms == 0 || GetTickCount64() >= deadline) break;
    Sleep(1);
  } while (true);
}

HWND create_probe_window(const DisplayTarget& display) {
  constexpr wchar_t kClassName[] = L"ZakoVulkanHdrProbeWindow";
  WNDCLASSW wc {};
  wc.lpfnWndProc = probe_window_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                kClassName, L"Zako Vulkan HDR Probe", WS_POPUP,
                                display.bounds.left + 32, display.bounds.top + 32, 256, 256,
                                nullptr, nullptr, wc.hInstance, nullptr);
  if (window) SetLayeredWindowAttributes(window, 0, 254, LWA_ALPHA);
  return window;
}

template <typename T>
T instance_proc(const VulkanFunctions& vk, VkInstance instance, const char* name) {
  return reinterpret_cast<T>(vk.get_instance_proc_addr(instance, name));
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
  return std::any_of(extensions.begin(), extensions.end(), [name](const auto& entry) {
    return std::strcmp(entry.extensionName, name) == 0;
  });
}

std::vector<VkExtensionProperties> instance_extensions(const VulkanFunctions& vk) {
  std::uint32_t count = 0;
  if (vk.enumerate_instance_extensions(nullptr, &count, nullptr) != VK_SUCCESS) return {};
  std::vector<VkExtensionProperties> result(count);
  const VkResult status = vk.enumerate_instance_extensions(nullptr, &count, result.data());
  if (status != VK_SUCCESS && status != VK_INCOMPLETE) return {};
  result.resize(count);
  return result;
}

std::vector<VkExtensionProperties> device_extensions(const VulkanFunctions& vk, VkPhysicalDevice physical_device) {
  std::uint32_t count = 0;
  if (vk.enumerate_device_extensions(physical_device, nullptr, &count, nullptr) != VK_SUCCESS) return {};
  std::vector<VkExtensionProperties> result(count);
  const VkResult status = vk.enumerate_device_extensions(physical_device, nullptr, &count, result.data());
  if (status != VK_SUCCESS && status != VK_INCOMPLETE) return {};
  result.resize(count);
  return result;
}

std::vector<VkSurfaceFormatKHR> surface_formats(const VulkanFunctions& vk, VkPhysicalDevice device, VkSurfaceKHR surface) {
  std::uint32_t count = 0;
  if (vk.get_surface_formats(device, surface, &count, nullptr) != VK_SUCCESS) return {};
  std::vector<VkSurfaceFormatKHR> result(count);
  const VkResult status = vk.get_surface_formats(device, surface, &count, result.data());
  if (status != VK_SUCCESS && status != VK_INCOMPLETE) return {};
  result.resize(count);
  return result;
}

bool contains_pair(const std::vector<VkSurfaceFormatKHR>& formats, VkFormat format, VkColorSpaceKHR color_space) {
  return std::any_of(formats.begin(), formats.end(), [=](const auto& entry) {
    return entry.format == format && entry.colorSpace == color_space;
  });
}

bool write_capability_cache_atomic(const std::wstring& path,
                                   const std::vector<zako::vulkan_hdr::CapabilityCacheEntry>& entries) {
  std::error_code directory_error;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, directory_error);
  if (directory_error) return false;
  const std::wstring temporary = path + L".tmp";
  if (!zako::vulkan_hdr::write_capability_cache(temporary, entries)) return false;
  if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  DeleteFileW(temporary.c_str());
  return false;
}

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

bool register_user_implicit_layer(const std::wstring& manifest_path) {
  HKEY key = nullptr;
  DWORD disposition = 0;
  const LONG created = RegCreateKeyExW(HKEY_CURRENT_USER, kImplicitLayersKey, 0, nullptr, 0,
                                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition);
  if (created != ERROR_SUCCESS) return false;
  const DWORD enabled = 0;
  const LONG written = RegSetValueExW(key, manifest_path.c_str(), 0, REG_DWORD,
                                      reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

bool unregister_user_implicit_layer(const std::wstring& manifest_name) {
  HKEY key = nullptr;
  const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, kImplicitLayersKey, 0,
                                     KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
  if (opened == ERROR_FILE_NOT_FOUND) return true;
  if (opened != ERROR_SUCCESS) return false;

  const auto expected_name = lowercase(std::filesystem::path(manifest_name).filename().wstring());
  std::vector<std::wstring> matches;
  for (DWORD index = 0;; ++index) {
    std::wstring value_name(32768, L'\0');
    DWORD length = static_cast<DWORD>(value_name.size());
    const LONG status = RegEnumValueW(key, index, value_name.data(), &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status != ERROR_SUCCESS) {
      RegCloseKey(key);
      return false;
    }
    value_name.resize(length);
    if (lowercase(std::filesystem::path(value_name).filename().wstring()) == expected_name) {
      matches.emplace_back(std::move(value_name));
    }
  }

  bool success = true;
  for (const auto& value_name : matches) {
    const LONG removed = RegDeleteValueW(key, value_name.c_str());
    success = success && (removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND);
  }
  RegCloseKey(key);
  return success;
}

void show_capability_cache(const std::wstring& path) {
  const auto entries = zako::vulkan_hdr::read_capability_cache(path);
  std::cout << "capability_cache_path=" << narrow(path) << " entry_count=" << entries.size() << '\n';
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    std::cout << "cache[" << index << "].gpu_name=" << entry.device_name
              << " vendor_id=" << entry.vendor_id << " device_id=" << entry.device_id
              << " driver_version=" << entry.driver_version << " api_version=" << entry.api_version
              << " luid=" << std::hex << static_cast<std::uint32_t>(entry.adapter_luid_high) << ':'
              << entry.adapter_luid_low << std::dec << " windows_build=" << entry.windows_build
              << " hdr10_create=" << ((entry.flags & zako::vulkan_hdr::kCapabilityHdr10Swapchain) != 0)
              << " scrgb_create=" << ((entry.flags & zako::vulkan_hdr::kCapabilityScRgbSwapchain) != 0)
              << " hdr_active=" << ((entry.flags & zako::vulkan_hdr::kCapabilityValidatedWhileHdrActive) != 0)
              << " presented=" << ((entry.flags & zako::vulkan_hdr::kCapabilityPresented) != 0)
              << " pixels_verified=" << ((entry.flags & zako::vulkan_hdr::kCapabilityPixelsVerified) != 0) << '\n';
  }
}

std::optional<std::uint32_t> present_queue_family(const VulkanFunctions& vk, VkPhysicalDevice device, VkSurfaceKHR surface) {
  std::uint32_t count = 0;
  vk.get_queue_families(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vk.get_queue_families(device, &count, families.data());
  for (std::uint32_t i = 0; i < count; ++i) {
    VkBool32 supported = 0;
    if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
        vk.get_surface_support(device, i, surface, &supported) == VK_SUCCESS && supported) return i;
  }
  return std::nullopt;
}

VkCompositeAlphaFlagBitsKHR choose_composite_alpha(VkFlags supported) {
  for (VkFlags candidate : {1u, 2u, 4u, 8u}) if ((supported & candidate) != 0) return candidate;
  return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps) {
  if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
  return {std::clamp(64u, caps.minImageExtent.width, caps.maxImageExtent.width),
          std::clamp(64u, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

VkResult try_forced_swapchain(const VulkanFunctions& vk, VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                              std::uint32_t queue_family, VkFormat format, VkColorSpaceKHR color_space) {
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queue_family, 1, &priority};
  const char* extensions[] = {kSwapchainExtension};
  VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queue_info, 0, nullptr, 1, extensions, nullptr};
  VkDevice device = nullptr;
  VkResult status = vk.create_device(physical_device, &device_info, nullptr, &device);
  if (status != VK_SUCCESS) return status;

  auto create_swapchain = vk.get_device_proc_addr ? reinterpret_cast<PFN_vkCreateSwapchainKHR>(vk.get_device_proc_addr(device, "vkCreateSwapchainKHR")) : nullptr;
  auto destroy_swapchain = vk.get_device_proc_addr ? reinterpret_cast<PFN_vkDestroySwapchainKHR>(vk.get_device_proc_addr(device, "vkDestroySwapchainKHR")) : nullptr;
  if (!create_swapchain || !destroy_swapchain || !vk.destroy_device) {
    if (vk.destroy_device) vk.destroy_device(device, nullptr);
    return -3;
  }

  VkSurfaceCapabilitiesKHR caps {};
  status = vk.get_surface_capabilities(physical_device, surface, &caps);
  if (status == VK_SUCCESS) {
    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) image_count = std::min(image_count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR info {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = image_count;
    info.imageFormat = format;
    info.imageColorSpace = color_space;
    info.imageExtent = choose_extent(caps);
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = choose_composite_alpha(caps.supportedCompositeAlpha);
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = 1;
    VkSwapchainKHR swapchain = 0;
    status = create_swapchain(device, &info, nullptr, &swapchain);
    if (status == VK_SUCCESS) destroy_swapchain(device, swapchain, nullptr);
  }
  vk.destroy_device(device, nullptr);
  return status;
}

template <typename T>
T device_proc(const VulkanFunctions& vk, VkDevice device, const char* name) {
  return vk.get_device_proc_addr ? reinterpret_cast<T>(vk.get_device_proc_addr(device, name)) : nullptr;
}

PresentTestResult run_present_test(const VulkanFunctions& vk, VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                   std::uint32_t queue_family, VkFormat format, VkColorSpaceKHR color_space,
                                   bool enable_hdr_metadata, std::uint32_t frame_count, DWORD hold_ms,
                                   const float* clear_override) {
  PresentTestResult result {};
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queue_family, 1, &priority};
  std::vector<const char*> extensions {kSwapchainExtension};
  if (enable_hdr_metadata) extensions.push_back("VK_EXT_hdr_metadata");
  VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queue_info, 0, nullptr,
                                  static_cast<std::uint32_t>(extensions.size()), extensions.data(), nullptr};
  VkDevice device = nullptr;
  result.swapchain_result = vk.create_device(physical_device, &device_info, nullptr, &device);
  if (result.swapchain_result != VK_SUCCESS) return result;

  const auto destroy_device = vk.destroy_device;
  const auto get_queue = device_proc<PFN_vkGetDeviceQueue>(vk, device, "vkGetDeviceQueue");
  const auto create_swapchain = device_proc<PFN_vkCreateSwapchainKHR>(vk, device, "vkCreateSwapchainKHR");
  const auto destroy_swapchain = device_proc<PFN_vkDestroySwapchainKHR>(vk, device, "vkDestroySwapchainKHR");
  const auto get_images = device_proc<PFN_vkGetSwapchainImagesKHR>(vk, device, "vkGetSwapchainImagesKHR");
  const auto create_pool = device_proc<PFN_vkCreateCommandPool>(vk, device, "vkCreateCommandPool");
  const auto destroy_pool = device_proc<PFN_vkDestroyCommandPool>(vk, device, "vkDestroyCommandPool");
  const auto allocate_commands = device_proc<PFN_vkAllocateCommandBuffers>(vk, device, "vkAllocateCommandBuffers");
  const auto begin_command = device_proc<PFN_vkBeginCommandBuffer>(vk, device, "vkBeginCommandBuffer");
  const auto end_command = device_proc<PFN_vkEndCommandBuffer>(vk, device, "vkEndCommandBuffer");
  const auto pipeline_barrier = device_proc<PFN_vkCmdPipelineBarrier>(vk, device, "vkCmdPipelineBarrier");
  const auto clear_image = device_proc<PFN_vkCmdClearColorImage>(vk, device, "vkCmdClearColorImage");
  const auto create_semaphore = device_proc<PFN_vkCreateSemaphore>(vk, device, "vkCreateSemaphore");
  const auto destroy_semaphore = device_proc<PFN_vkDestroySemaphore>(vk, device, "vkDestroySemaphore");
  const auto acquire_image = device_proc<PFN_vkAcquireNextImageKHR>(vk, device, "vkAcquireNextImageKHR");
  const auto queue_submit = device_proc<PFN_vkQueueSubmit>(vk, device, "vkQueueSubmit");
  const auto queue_present = device_proc<PFN_vkQueuePresentKHR>(vk, device, "vkQueuePresentKHR");
  const auto queue_wait_idle = device_proc<PFN_vkQueueWaitIdle>(vk, device, "vkQueueWaitIdle");
  const auto set_hdr_metadata = device_proc<PFN_vkSetHdrMetadataEXT>(vk, device, "vkSetHdrMetadataEXT");
  if (!destroy_device || !get_queue || !create_swapchain || !destroy_swapchain || !get_images ||
      !create_pool || !destroy_pool || !allocate_commands || !begin_command || !end_command ||
      !pipeline_barrier || !clear_image || !create_semaphore || !destroy_semaphore || !acquire_image ||
      !queue_submit || !queue_present || !queue_wait_idle) {
    destroy_device(device, nullptr);
    return result;
  }

  VkSurfaceCapabilitiesKHR caps {};
  result.swapchain_result = vk.get_surface_capabilities(physical_device, surface, &caps);
  if (result.swapchain_result != VK_SUCCESS || (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
    destroy_device(device, nullptr);
    return result;
  }

  std::uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0) image_count = std::min(image_count, caps.maxImageCount);
  VkSwapchainCreateInfoKHR swapchain_info {};
  swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchain_info.surface = surface;
  swapchain_info.minImageCount = image_count;
  swapchain_info.imageFormat = format;
  swapchain_info.imageColorSpace = color_space;
  swapchain_info.imageExtent = choose_extent(caps);
  swapchain_info.imageArrayLayers = 1;
  swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swapchain_info.preTransform = caps.currentTransform;
  swapchain_info.compositeAlpha = choose_composite_alpha(caps.supportedCompositeAlpha);
  swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swapchain_info.clipped = 1;
  VkSwapchainKHR swapchain = 0;
  result.swapchain_result = create_swapchain(device, &swapchain_info, nullptr, &swapchain);
  if (result.swapchain_result != VK_SUCCESS) {
    destroy_device(device, nullptr);
    return result;
  }

  if (enable_hdr_metadata && set_hdr_metadata) {
    VkHdrMetadataEXT metadata {};
    metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
    metadata.displayPrimaryRed = {0.708f, 0.292f};
    metadata.displayPrimaryGreen = {0.170f, 0.797f};
    metadata.displayPrimaryBlue = {0.131f, 0.046f};
    metadata.whitePoint = {0.3127f, 0.3290f};
    metadata.maxLuminance = 1000.0f;
    metadata.minLuminance = 0.0001f;
    metadata.maxContentLightLevel = 1000.0f;
    metadata.maxFrameAverageLightLevel = 400.0f;
    set_hdr_metadata(device, 1, &swapchain, &metadata);
    result.metadata_submitted = true;
  }

  std::uint32_t actual_count = 0;
  get_images(device, swapchain, &actual_count, nullptr);
  std::vector<VkImage> images(actual_count);
  get_images(device, swapchain, &actual_count, images.data());
  VkCommandPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, queue_family};
  VkCommandPool pool = 0;
  if (create_pool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    destroy_swapchain(device, swapchain, nullptr);
    destroy_device(device, nullptr);
    return result;
  }
  std::vector<VkCommandBuffer> commands(actual_count);
  VkCommandBufferAllocateInfo allocate_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, 0, actual_count};
  if (allocate_commands(device, &allocate_info, commands.data()) != VK_SUCCESS) {
    destroy_pool(device, pool, nullptr);
    destroy_swapchain(device, swapchain, nullptr);
    destroy_device(device, nullptr);
    return result;
  }

  VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkClearColorValue clear {};
  const float encoded_white = color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT ? 0.751827f : 12.5f;
  clear.float32[0] = clear_override ? clear_override[0] : encoded_white;
  clear.float32[1] = clear_override ? clear_override[1] : encoded_white;
  clear.float32[2] = clear_override ? clear_override[2] : encoded_white;
  clear.float32[3] = 1.0f;
  for (std::uint32_t index = 0; index < actual_count; ++index) {
    VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                    VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, nullptr};
    if (begin_command(commands[index], &begin) != VK_SUCCESS) continue;
    VkImageMemoryBarrier to_transfer {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[index], range};
    pipeline_barrier(commands[index], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     0, nullptr, 0, nullptr, 1, &to_transfer);
    clear_image(commands[index], images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    VkImageMemoryBarrier to_present {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[index], range};
    pipeline_barrier(commands[index], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                     0, nullptr, 0, nullptr, 1, &to_present);
    end_command(commands[index]);
  }

  VkSemaphoreCreateInfo semaphore_info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
  VkSemaphore acquired = 0;
  VkSemaphore rendered = 0;
  if (create_semaphore(device, &semaphore_info, nullptr, &acquired) != VK_SUCCESS ||
      create_semaphore(device, &semaphore_info, nullptr, &rendered) != VK_SUCCESS) {
    if (acquired) destroy_semaphore(device, acquired, nullptr);
    destroy_pool(device, pool, nullptr);
    destroy_swapchain(device, swapchain, nullptr);
    destroy_device(device, nullptr);
    return result;
  }

  VkQueue queue = nullptr;
  get_queue(device, queue_family, 0, &queue);
  result.present_result = VK_SUCCESS;
  for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
    std::uint32_t image_index = 0;
    VkResult status = acquire_image(device, swapchain, UINT64_MAX, acquired, 0, &image_index);
    if (status != VK_SUCCESS && status != 1000001003) { result.present_result = status; break; }
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &acquired, &wait_stage,
                         1, &commands[image_index], 1, &rendered};
    status = queue_submit(queue, 1, &submit, 0);
    if (status != VK_SUCCESS) { result.present_result = status; break; }
    VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &rendered,
                              1, &swapchain, &image_index, nullptr};
    status = queue_present(queue, &present);
    if (status != VK_SUCCESS && status != 1000001003) { result.present_result = status; break; }
    queue_wait_idle(queue);
    ++result.frames_presented;
    pump_messages_for(16);
  }

  queue_wait_idle(queue);
  if (hold_ms > 0) {
    std::cout << "holding_presented_swapchain_ms=" << hold_ms << '\n';
    pump_messages_for(hold_ms);
  }
  destroy_semaphore(device, rendered, nullptr);
  destroy_semaphore(device, acquired, nullptr);
  destroy_pool(device, pool, nullptr);
  destroy_swapchain(device, swapchain, nullptr);
  destroy_device(device, nullptr);
  return result;
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

DeviceFingerprint device_fingerprint(const VulkanFunctions& vk, VkPhysicalDevice device) {
  DeviceFingerprint result {};
  if (!vk.get_physical_device_properties2) return result;
  VkPhysicalDeviceIDProperties id {};
  id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2Probe properties {};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &id;
  vk.get_physical_device_properties2(device, &properties);

  auto& entry = result.cache_entry;
  entry.vendor_id = properties.properties.vendorID;
  entry.device_id = properties.properties.deviceID;
  entry.driver_version = properties.properties.driverVersion;
  entry.api_version = properties.properties.apiVersion;
  entry.windows_build = windows_build_number();
  strncpy_s(entry.device_name, properties.properties.deviceName, _TRUNCATE);
  result.luid_valid = id.deviceLUIDValid != 0;
  if (result.luid_valid) {
    std::memcpy(&entry.adapter_luid_low, id.deviceLUID, sizeof(entry.adapter_luid_low));
    std::memcpy(&entry.adapter_luid_high, id.deviceLUID + sizeof(entry.adapter_luid_low), sizeof(entry.adapter_luid_high));
  }
  return result;
}

std::optional<VulkanFunctions> load_vulkan() {
  VulkanFunctions vk {};
  vk.module = LoadLibraryW(L"vulkan-1.dll");
  if (!vk.module) return std::nullopt;
  vk.get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk.module, "vkGetInstanceProcAddr"));
  if (!vk.get_instance_proc_addr) return std::nullopt;
  vk.enumerate_instance_extensions = instance_proc<PFN_vkEnumerateInstanceExtensionProperties>(vk, nullptr, "vkEnumerateInstanceExtensionProperties");
  vk.create_instance = instance_proc<PFN_vkCreateInstance>(vk, nullptr, "vkCreateInstance");
  return vk;
}

void print_usage() {
  std::cout << "Usage: vulkan_hdr_probe.exe [--list] [--display \\\\.\\DISPLAYn] [--gpu N] [--force-create]"
               " [--present hdr10|scrgb] [--frames N] [--hold-ms N] [--clear R G B]"
               " [--cache PATH] [--show-cache PATH] [--mark-pixels-verified PATH]"
               " [--register-implicit-layer PATH] [--unregister-implicit-layer NAME]\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Monitor bounds returned by GetMonitorInfo are physical desktop pixels.
  // Opt out of DPI coordinate virtualization before positioning the probe
  // window, otherwise mixed-DPI hosts can place it outside the selected VDD.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  std::wstring requested_display;
  std::uint32_t requested_gpu = 0;
  bool list_only = false;
  bool force_create = false;
  std::wstring cache_path;
  std::wstring show_cache_path;
  std::wstring mark_pixels_path;
  std::wstring register_layer_path;
  std::wstring unregister_layer_name;
  std::wstring present_mode;
  std::uint32_t present_frames = 60;
  DWORD hold_ms = 0;
  float clear_override[3] {};
  bool has_clear_override = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--list") list_only = true;
    else if (arg == L"--force-create") force_create = true;
    else if (arg == L"--display" && i + 1 < argc) requested_display = argv[++i];
    else if (arg == L"--gpu" && i + 1 < argc) requested_gpu = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
    else if (arg == L"--cache" && i + 1 < argc) cache_path = argv[++i];
    else if (arg == L"--show-cache" && i + 1 < argc) show_cache_path = argv[++i];
    else if (arg == L"--mark-pixels-verified" && i + 1 < argc) mark_pixels_path = argv[++i];
    else if (arg == L"--register-implicit-layer" && i + 1 < argc) register_layer_path = argv[++i];
    else if (arg == L"--unregister-implicit-layer" && i + 1 < argc) unregister_layer_name = argv[++i];
    else if (arg == L"--present" && i + 1 < argc) { present_mode = argv[++i]; force_create = true; }
    else if (arg == L"--frames" && i + 1 < argc) present_frames = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
    else if (arg == L"--hold-ms" && i + 1 < argc) hold_ms = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
    else if (arg == L"--clear" && i + 3 < argc) {
      clear_override[0] = std::wcstof(argv[++i], nullptr);
      clear_override[1] = std::wcstof(argv[++i], nullptr);
      clear_override[2] = std::wcstof(argv[++i], nullptr);
      has_clear_override = true;
    }
    else { print_usage(); return 2; }
  }

  if (!register_layer_path.empty()) {
    const bool registered = register_user_implicit_layer(register_layer_path);
    std::cout << "implicit_layer_registered=" << registered << " hive=HKCU\n";
    return registered ? 0 : 17;
  }

  if (!unregister_layer_name.empty()) {
    const bool removed = unregister_user_implicit_layer(unregister_layer_name);
    std::cout << "implicit_layer_removed=" << removed << " hive=HKCU\n";
    return removed ? 0 : 18;
  }

  if (!show_cache_path.empty()) {
    show_capability_cache(show_cache_path);
    return 0;
  }

  if (!mark_pixels_path.empty()) {
    auto cache = zako::vulkan_hdr::read_capability_cache(mark_pixels_path);
    std::size_t marked = 0;
    for (auto& entry : cache) {
      const auto required = zako::vulkan_hdr::kCapabilityValidatedWhileHdrActive |
                            zako::vulkan_hdr::kCapabilityPresented;
      if ((entry.flags & required) == required) {
        entry.flags |= zako::vulkan_hdr::kCapabilityPixelsVerified;
        ++marked;
      }
    }
    const bool written = marked > 0 && write_capability_cache_atomic(mark_pixels_path, cache);
    std::cout << "pixels_verified_marked=" << marked << " cache_written=" << written
              << " path=" << narrow(mark_pixels_path) << '\n';
    return written ? 0 : 16;
  }

  const auto available_displays = displays();
  std::cout << "display_count=" << available_displays.size() << '\n';
  for (std::size_t i = 0; i < available_displays.size(); ++i) {
    const auto& display = available_displays[i];
    std::cout << "display[" << i << "].name=" << narrow(display.gdi_name)
              << " hdr_supported=" << display.hdr_supported
              << " hdr_enabled=" << display.hdr_enabled
              << " bounds=" << display.bounds.left << ',' << display.bounds.top << ','
              << display.bounds.right << ',' << display.bounds.bottom << '\n';
  }
  if (list_only) return 0;
  if (available_displays.empty()) return 3;

  auto selected = available_displays.begin();
  if (!requested_display.empty()) {
    selected = std::find_if(available_displays.begin(), available_displays.end(), [&](const auto& entry) {
      return _wcsicmp(entry.gdi_name.c_str(), requested_display.c_str()) == 0;
    });
    if (selected == available_displays.end()) {
      std::cerr << "error=display_not_found\n";
      return 4;
    }
  }
  std::cout << "selected_display=" << narrow(selected->gdi_name) << " hdr_supported=" << selected->hdr_supported
            << " hdr_enabled=" << selected->hdr_enabled << '\n';

  HWND window = create_probe_window(*selected);
  if (!window) {
    std::cerr << "error=create_window native_error=" << GetLastError() << '\n';
    return 5;
  }
  ShowWindow(window, SW_SHOWNOACTIVATE);
  SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  UpdateWindow(window);
  pump_messages_for(50);
  RECT actual_window {};
  GetWindowRect(window, &actual_window);
  MONITORINFOEXW actual_monitor {};
  actual_monitor.cbSize = sizeof(actual_monitor);
  GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &actual_monitor);
  std::cout << "window_visible=" << IsWindowVisible(window)
            << " window_rect=" << actual_window.left << ',' << actual_window.top << ','
            << actual_window.right << ',' << actual_window.bottom
            << " window_monitor=" << narrow(actual_monitor.szDevice) << '\n';

  auto loaded = load_vulkan();
  if (!loaded || !loaded->enumerate_instance_extensions || !loaded->create_instance) {
    std::cerr << "error=vulkan_loader_unavailable\n";
    DestroyWindow(window);
    return 6;
  }
  auto& vk = *loaded;
  const auto extensions = instance_extensions(vk);
  for (const char* required : {kSurfaceExtension, kWin32SurfaceExtension, kSwapchainColorspaceExtension}) {
    std::cout << "instance_extension." << required << '=' << has_extension(extensions, required) << '\n';
  }
  if (!has_extension(extensions, kSurfaceExtension) || !has_extension(extensions, kWin32SurfaceExtension)) {
    std::cerr << "error=required_surface_extension_missing\n";
    DestroyWindow(window);
    return 7;
  }

  std::vector<const char*> enabled {kSurfaceExtension, kWin32SurfaceExtension};
  if (has_extension(extensions, kSwapchainColorspaceExtension)) enabled.push_back(kSwapchainColorspaceExtension);
  VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Zako Vulkan HDR Probe", 1, "ZakoVDD", 1,
                         (1u << 22) | (1u << 12)};
  VkInstanceCreateInfo create_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr,
                                    static_cast<std::uint32_t>(enabled.size()), enabled.data()};
  VkInstance instance = nullptr;
  VkResult status = vk.create_instance(&create_info, nullptr, &instance);
  if (status != VK_SUCCESS) {
    std::cerr << "error=vkCreateInstance result=" << status << '\n';
    DestroyWindow(window);
    return 8;
  }

  vk.destroy_instance = instance_proc<PFN_vkDestroyInstance>(vk, instance, "vkDestroyInstance");
  vk.enumerate_physical_devices = instance_proc<PFN_vkEnumeratePhysicalDevices>(vk, instance, "vkEnumeratePhysicalDevices");
  vk.get_physical_device_properties2 = instance_proc<PFN_vkGetPhysicalDeviceProperties2>(vk, instance, "vkGetPhysicalDeviceProperties2");
  vk.create_win32_surface = instance_proc<PFN_vkCreateWin32SurfaceKHR>(vk, instance, "vkCreateWin32SurfaceKHR");
  vk.destroy_surface = instance_proc<PFN_vkDestroySurfaceKHR>(vk, instance, "vkDestroySurfaceKHR");
  vk.get_surface_support = instance_proc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(vk, instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
  vk.get_surface_formats = instance_proc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(vk, instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  vk.get_surface_capabilities = instance_proc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(vk, instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  vk.get_queue_families = instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(vk, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  vk.enumerate_device_extensions = instance_proc<PFN_vkEnumerateDeviceExtensionProperties>(vk, instance, "vkEnumerateDeviceExtensionProperties");
  vk.create_device = instance_proc<PFN_vkCreateDevice>(vk, instance, "vkCreateDevice");
  vk.get_device_proc_addr = instance_proc<PFN_vkGetDeviceProcAddr>(vk, instance, "vkGetDeviceProcAddr");
  vk.destroy_device = instance_proc<PFN_vkDestroyDevice>(vk, instance, "vkDestroyDevice");

  VkWin32SurfaceCreateInfoKHR surface_info {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0,
                                            GetModuleHandleW(nullptr), window};
  VkSurfaceKHR surface = 0;
  status = vk.create_win32_surface(instance, &surface_info, nullptr, &surface);
  if (status != VK_SUCCESS) {
    std::cerr << "error=vkCreateWin32SurfaceKHR result=" << status << '\n';
    vk.destroy_instance(instance, nullptr);
    DestroyWindow(window);
    return 9;
  }

  std::uint32_t gpu_count = 0;
  vk.enumerate_physical_devices(instance, &gpu_count, nullptr);
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  vk.enumerate_physical_devices(instance, &gpu_count, gpus.data());
  std::cout << "gpu_count=" << gpu_count << '\n';
  if (requested_gpu >= gpu_count) {
    std::cerr << "error=gpu_index_out_of_range\n";
    vk.destroy_surface(instance, surface, nullptr);
    vk.destroy_instance(instance, nullptr);
    DestroyWindow(window);
    return 10;
  }

  const auto gpu = gpus[requested_gpu];
  const auto fingerprint = device_fingerprint(vk, gpu);
  const auto& identity = fingerprint.cache_entry;
  std::cout << "gpu_name=" << identity.device_name << '\n';
  std::cout << "gpu_vendor_id=" << identity.vendor_id << " gpu_device_id=" << identity.device_id
            << " driver_version=" << identity.driver_version << " api_version=" << identity.api_version << '\n';
  std::cout << "gpu_luid_valid=" << fingerprint.luid_valid << " gpu_luid=" << std::hex
            << static_cast<std::uint32_t>(identity.adapter_luid_high) << ':' << identity.adapter_luid_low << std::dec
            << " windows_build=" << identity.windows_build << '\n';
  const auto formats = surface_formats(vk, gpu, surface);
  std::cout << "surface_format_count=" << formats.size() << '\n';
  for (std::size_t i = 0; i < formats.size(); ++i) {
    std::cout << "surface_format[" << i << "].format=" << formats[i].format
              << " colorspace=" << formats[i].colorSpace << '\n';
  }
  const bool advertises_hdr10 = contains_pair(formats, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT);
  const bool advertises_scrgb = contains_pair(formats, VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
  std::cout << "advertises_hdr10_pair=" << advertises_hdr10 << '\n';
  std::cout << "advertises_scrgb_pair=" << advertises_scrgb << '\n';

  int exit_code = 0;
  if (force_create) {
    const auto device_exts = device_extensions(vk, gpu);
    std::cout << "device_extension." << kSwapchainExtension << '=' << has_extension(device_exts, kSwapchainExtension) << '\n';
    const auto queue_family = present_queue_family(vk, gpu, surface);
    if (!has_extension(device_exts, kSwapchainExtension) || !queue_family) {
      std::cerr << "error=swapchain_or_present_queue_unavailable\n";
      exit_code = 11;
    } else {
      const VkResult hdr10 = try_forced_swapchain(vk, gpu, surface, *queue_family,
                                                  VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT);
      const VkResult scrgb = try_forced_swapchain(vk, gpu, surface, *queue_family,
                                                  VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
      PresentTestResult present {};
      bool ran_present = false;
      if (!present_mode.empty()) {
        const bool use_hdr10 = _wcsicmp(present_mode.c_str(), L"hdr10") == 0;
        const bool use_scrgb = _wcsicmp(present_mode.c_str(), L"scrgb") == 0;
        if (!use_hdr10 && !use_scrgb) {
          std::cerr << "error=invalid_present_mode\n";
          exit_code = 14;
        } else {
          const auto device_exts_for_present = device_extensions(vk, gpu);
          const bool metadata_available = has_extension(device_exts_for_present, "VK_EXT_hdr_metadata");
          present = run_present_test(vk, gpu, surface, *queue_family,
                                     use_hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT,
                                     use_hdr10 ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
                                     use_hdr10 && metadata_available, present_frames, hold_ms,
                                     has_clear_override ? clear_override : nullptr);
          ran_present = true;
          std::cout << "present_mode=" << (use_hdr10 ? "hdr10" : "scrgb")
                    << " swapchain_result=" << present.swapchain_result
                    << " present_result=" << present.present_result
                    << " frames_presented=" << present.frames_presented
                    << " hdr_metadata_submitted=" << present.metadata_submitted << '\n';
          if (present.swapchain_result != VK_SUCCESS || present.present_result != VK_SUCCESS || present.frames_presented == 0) exit_code = 15;
        }
      }
      std::cout << "forced_hdr10_swapchain_result=" << hdr10 << " success=" << (hdr10 == VK_SUCCESS) << '\n';
      std::cout << "forced_scrgb_swapchain_result=" << scrgb << " success=" << (scrgb == VK_SUCCESS) << '\n';
      std::cout << "capability_cache_eligible="
                << (selected->hdr_enabled && (hdr10 == VK_SUCCESS || scrgb == VK_SUCCESS)) << '\n';
      if (!selected->hdr_enabled) {
        std::cout << "warning=forced_swapchain_was_not_presented_on_an_active_hdr_display\n";
      }
      if (!cache_path.empty()) {
        auto entry = fingerprint.cache_entry;
        if (hdr10 == VK_SUCCESS) entry.flags |= zako::vulkan_hdr::kCapabilityHdr10Swapchain;
        if (scrgb == VK_SUCCESS) entry.flags |= zako::vulkan_hdr::kCapabilityScRgbSwapchain;
        if (selected->hdr_enabled) entry.flags |= zako::vulkan_hdr::kCapabilityValidatedWhileHdrActive;
        if (ran_present && present.present_result == VK_SUCCESS && present.frames_presented > 0) {
          entry.flags |= zako::vulkan_hdr::kCapabilityPresented;
        }
        auto cache = zako::vulkan_hdr::read_capability_cache(cache_path);
        zako::vulkan_hdr::upsert_capability(cache, entry);
        const bool written = write_capability_cache_atomic(cache_path, cache);
        std::cout << "capability_cache_written=" << written << " path=" << narrow(cache_path) << '\n';
        if (!written && exit_code == 0) exit_code = 13;
      }
      if (hdr10 != VK_SUCCESS && scrgb != VK_SUCCESS) exit_code = 12;
    }
  }

  vk.destroy_surface(instance, surface, nullptr);
  vk.destroy_instance(instance, nullptr);
  FreeLibrary(vk.module);
  DestroyWindow(window);
  return exit_code;
}
