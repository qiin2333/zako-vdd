#pragma once

// Minimal Vulkan 1.x ABI declarations used by the standalone diagnostic probe.
// The probe loads vulkan-1.dll dynamically so end users do not need the Vulkan
// SDK. The production layer must use pinned Khronos Vulkan-Headers instead.

#include <cstdint>
#include <cstddef>

#define VKAPI_PTR __stdcall

using VkFlags = std::uint32_t;
using VkBool32 = std::uint32_t;
using VkDeviceSize = std::uint64_t;
using VkResult = std::int32_t;
using VkFormat = std::int32_t;
using VkColorSpaceKHR = std::int32_t;
using VkPresentModeKHR = std::int32_t;
using VkStructureType = std::int32_t;
using VkImageUsageFlags = VkFlags;
using VkSurfaceTransformFlagBitsKHR = VkFlags;
using VkCompositeAlphaFlagBitsKHR = VkFlags;
using VkSharingMode = std::int32_t;
using VkImageLayout = std::int32_t;
using VkCommandBufferLevel = std::int32_t;
using VkPipelineStageFlags = VkFlags;
using VkAccessFlags = VkFlags;
using VkInstance = struct VkInstance_T*;
using VkPhysicalDevice = struct VkPhysicalDevice_T*;
using VkDevice = struct VkDevice_T*;
using VkQueue = struct VkQueue_T*;
using VkSurfaceKHR = std::uint64_t;
using VkSwapchainKHR = std::uint64_t;
using VkImage = std::uint64_t;
using VkCommandPool = std::uint64_t;
using VkCommandBuffer = struct VkCommandBuffer_T*;
using VkSemaphore = std::uint64_t;
using VkFence = std::uint64_t;

constexpr VkResult VK_SUCCESS = 0;
constexpr VkResult VK_INCOMPLETE = 5;
constexpr VkStructureType VK_STRUCTURE_TYPE_APPLICATION_INFO = 0;
constexpr VkStructureType VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1;
constexpr VkStructureType VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2;
constexpr VkStructureType VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3;
constexpr VkStructureType VK_STRUCTURE_TYPE_SUBMIT_INFO = 4;
constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9;
constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39;
constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40;
constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42;
constexpr VkStructureType VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45;
constexpr VkStructureType VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000;
constexpr VkStructureType VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001;
constexpr VkStructureType VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000;
constexpr VkStructureType VK_STRUCTURE_TYPE_HDR_METADATA_EXT = 1000105000;
constexpr VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001;
constexpr VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES = 1000071004;
constexpr VkFormat VK_FORMAT_A2B10G10R10_UNORM_PACK32 = 64;
constexpr VkFormat VK_FORMAT_R16G16B16A16_SFLOAT = 97;
constexpr VkColorSpaceKHR VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0;
constexpr VkColorSpaceKHR VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT = 1000104002;
constexpr VkColorSpaceKHR VK_COLOR_SPACE_HDR10_ST2084_EXT = 1000104008;
constexpr VkPresentModeKHR VK_PRESENT_MODE_FIFO_KHR = 2;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2;
constexpr VkSharingMode VK_SHARING_MODE_EXCLUSIVE = 0;
constexpr VkFlags VK_QUEUE_GRAPHICS_BIT = 0x1;
constexpr VkCompositeAlphaFlagBitsKHR VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 0x1;
constexpr VkImageLayout VK_IMAGE_LAYOUT_UNDEFINED = 0;
constexpr VkImageLayout VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7;
constexpr VkImageLayout VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002;
constexpr VkPipelineStageFlags VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x1;
constexpr VkPipelineStageFlags VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000;
constexpr VkPipelineStageFlags VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x2000;
constexpr VkAccessFlags VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000;
constexpr VkFlags VK_IMAGE_ASPECT_COLOR_BIT = 0x1;
constexpr VkFlags VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT = 0x4;
constexpr std::uint32_t VK_QUEUE_FAMILY_IGNORED = 0xffffffffu;

struct VkExtent2D {
  std::uint32_t width;
  std::uint32_t height;
};

struct VkExtent3D {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t depth;
};

struct VkApplicationInfo {
  VkStructureType sType;
  const void* pNext;
  const char* pApplicationName;
  std::uint32_t applicationVersion;
  const char* pEngineName;
  std::uint32_t engineVersion;
  std::uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  const VkApplicationInfo* pApplicationInfo;
  std::uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  std::uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
};

struct VkDeviceQueueCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t queueFamilyIndex;
  std::uint32_t queueCount;
  const float* pQueuePriorities;
};

struct VkDeviceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t queueCreateInfoCount;
  const VkDeviceQueueCreateInfo* pQueueCreateInfos;
  std::uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  std::uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
  const void* pEnabledFeatures;
};

struct VkWin32SurfaceCreateInfoKHR {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* hinstance;
  void* hwnd;
};

struct VkSurfaceFormatKHR {
  VkFormat format;
  VkColorSpaceKHR colorSpace;
};

struct VkSurfaceFormat2KHR {
  VkStructureType sType;
  void* pNext;
  VkSurfaceFormatKHR surfaceFormat;
};

struct VkPhysicalDeviceSurfaceInfo2KHR {
  VkStructureType sType;
  const void* pNext;
  VkSurfaceKHR surface;
};

struct VkSurfaceCapabilitiesKHR {
  std::uint32_t minImageCount;
  std::uint32_t maxImageCount;
  VkExtent2D currentExtent;
  VkExtent2D minImageExtent;
  VkExtent2D maxImageExtent;
  std::uint32_t maxImageArrayLayers;
  VkFlags supportedTransforms;
  VkSurfaceTransformFlagBitsKHR currentTransform;
  VkFlags supportedCompositeAlpha;
  VkImageUsageFlags supportedUsageFlags;
};

struct VkSwapchainCreateInfoKHR {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkSurfaceKHR surface;
  std::uint32_t minImageCount;
  VkFormat imageFormat;
  VkColorSpaceKHR imageColorSpace;
  VkExtent2D imageExtent;
  std::uint32_t imageArrayLayers;
  VkImageUsageFlags imageUsage;
  VkSharingMode imageSharingMode;
  std::uint32_t queueFamilyIndexCount;
  const std::uint32_t* pQueueFamilyIndices;
  VkSurfaceTransformFlagBitsKHR preTransform;
  VkCompositeAlphaFlagBitsKHR compositeAlpha;
  VkPresentModeKHR presentMode;
  VkBool32 clipped;
  VkSwapchainKHR oldSwapchain;
};

struct VkSemaphoreCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
};

struct VkCommandPoolCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo {
  VkStructureType sType;
  const void* pNext;
  VkCommandPool commandPool;
  VkCommandBufferLevel level;
  std::uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  const void* pInheritanceInfo;
};

struct VkImageSubresourceRange {
  VkFlags aspectMask;
  std::uint32_t baseMipLevel;
  std::uint32_t levelCount;
  std::uint32_t baseArrayLayer;
  std::uint32_t layerCount;
};

struct VkImageMemoryBarrier {
  VkStructureType sType;
  const void* pNext;
  VkAccessFlags srcAccessMask;
  VkAccessFlags dstAccessMask;
  VkImageLayout oldLayout;
  VkImageLayout newLayout;
  std::uint32_t srcQueueFamilyIndex;
  std::uint32_t dstQueueFamilyIndex;
  VkImage image;
  VkImageSubresourceRange subresourceRange;
};

union VkClearColorValue {
  float float32[4];
  std::int32_t int32[4];
  std::uint32_t uint32[4];
};

struct VkSubmitInfo {
  VkStructureType sType;
  const void* pNext;
  std::uint32_t waitSemaphoreCount;
  const VkSemaphore* pWaitSemaphores;
  const VkPipelineStageFlags* pWaitDstStageMask;
  std::uint32_t commandBufferCount;
  const VkCommandBuffer* pCommandBuffers;
  std::uint32_t signalSemaphoreCount;
  const VkSemaphore* pSignalSemaphores;
};

struct VkPresentInfoKHR {
  VkStructureType sType;
  const void* pNext;
  std::uint32_t waitSemaphoreCount;
  const VkSemaphore* pWaitSemaphores;
  std::uint32_t swapchainCount;
  const VkSwapchainKHR* pSwapchains;
  const std::uint32_t* pImageIndices;
  VkResult* pResults;
};

struct VkXYColorEXT { float x; float y; };

struct VkHdrMetadataEXT {
  VkStructureType sType;
  const void* pNext;
  VkXYColorEXT displayPrimaryRed;
  VkXYColorEXT displayPrimaryGreen;
  VkXYColorEXT displayPrimaryBlue;
  VkXYColorEXT whitePoint;
  float maxLuminance;
  float minLuminance;
  float maxContentLightLevel;
  float maxFrameAverageLightLevel;
};

struct VkQueueFamilyProperties {
  VkFlags queueFlags;
  std::uint32_t queueCount;
  std::uint32_t timestampValidBits;
  VkExtent3D minImageTransferGranularity;
};

struct VkExtensionProperties {
  char extensionName[256];
  std::uint32_t specVersion;
};

// The properties prefix is stable Vulkan ABI. The opaque tail is deliberately
// oversized because the probe only reads the prefix while the loader writes
// the full VkPhysicalDeviceLimits/SparseProperties payload.
struct VkPhysicalDevicePropertiesPrefix {
  std::uint32_t apiVersion;
  std::uint32_t driverVersion;
  std::uint32_t vendorID;
  std::uint32_t deviceID;
  std::int32_t deviceType;
  char deviceName[256];
  std::uint8_t pipelineCacheUUID[16];
  std::byte opaqueTail[1024];
};

struct VkPhysicalDeviceProperties2Probe {
  VkStructureType sType;
  const void* pNext;
  VkPhysicalDevicePropertiesPrefix properties;
};

struct VkPhysicalDeviceIDProperties {
  VkStructureType sType;
  void* pNext;
  std::uint8_t deviceUUID[16];
  std::uint8_t driverUUID[16];
  std::uint8_t deviceLUID[8];
  std::uint32_t deviceNodeMask;
  VkBool32 deviceLUIDValid;
};

using PFN_vkVoidFunction = void(VKAPI_PTR*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(VkDevice, const char*);
using PFN_vkEnumerateInstanceExtensionProperties = VkResult(VKAPI_PTR*)(const char*, std::uint32_t*, VkExtensionProperties*);
using PFN_vkCreateInstance = VkResult(VKAPI_PTR*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(VKAPI_PTR*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult(VKAPI_PTR*)(VkInstance, std::uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties2 = void(VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDeviceProperties2Probe*);
using PFN_vkCreateWin32SurfaceKHR = VkResult(VKAPI_PTR*)(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const void*, VkSurfaceKHR*);
using PFN_vkDestroySurfaceKHR = void(VKAPI_PTR*)(VkInstance, VkSurfaceKHR, const void*);
using PFN_vkGetPhysicalDeviceSurfaceSupportKHR = VkResult(VKAPI_PTR*)(VkPhysicalDevice, std::uint32_t, VkSurfaceKHR, VkBool32*);
using PFN_vkGetPhysicalDeviceSurfaceFormatsKHR = VkResult(VKAPI_PTR*)(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t*, VkSurfaceFormatKHR*);
using PFN_vkGetPhysicalDeviceSurfaceFormats2KHR = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, std::uint32_t*, VkSurfaceFormat2KHR*);
using PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = VkResult(VKAPI_PTR*)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
using PFN_vkGetPhysicalDeviceQueueFamilyProperties = void(VKAPI_PTR*)(VkPhysicalDevice, std::uint32_t*, VkQueueFamilyProperties*);
using PFN_vkEnumerateDeviceExtensionProperties = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const char*, std::uint32_t*, VkExtensionProperties*);
using PFN_vkCreateDevice = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
using PFN_vkDestroyDevice = void(VKAPI_PTR*)(VkDevice, const void*);
using PFN_vkCreateSwapchainKHR = VkResult(VKAPI_PTR*)(VkDevice, const VkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*);
using PFN_vkDestroySwapchainKHR = void(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, const void*);
using PFN_vkGetDeviceQueue = void(VKAPI_PTR*)(VkDevice, std::uint32_t, std::uint32_t, VkQueue*);
using PFN_vkGetSwapchainImagesKHR = VkResult(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, std::uint32_t*, VkImage*);
using PFN_vkCreateCommandPool = VkResult(VKAPI_PTR*)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*);
using PFN_vkDestroyCommandPool = void(VKAPI_PTR*)(VkDevice, VkCommandPool, const void*);
using PFN_vkAllocateCommandBuffers = VkResult(VKAPI_PTR*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
using PFN_vkBeginCommandBuffer = VkResult(VKAPI_PTR*)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
using PFN_vkEndCommandBuffer = VkResult(VKAPI_PTR*)(VkCommandBuffer);
using PFN_vkCmdPipelineBarrier = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkFlags,
                                                  std::uint32_t, const void*, std::uint32_t, const void*,
                                                  std::uint32_t, const VkImageMemoryBarrier*);
using PFN_vkCmdClearColorImage = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*,
                                                  std::uint32_t, const VkImageSubresourceRange*);
using PFN_vkCreateSemaphore = VkResult(VKAPI_PTR*)(VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*);
using PFN_vkDestroySemaphore = void(VKAPI_PTR*)(VkDevice, VkSemaphore, const void*);
using PFN_vkAcquireNextImageKHR = VkResult(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, std::uint64_t, VkSemaphore, VkFence, std::uint32_t*);
using PFN_vkQueueSubmit = VkResult(VKAPI_PTR*)(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence);
using PFN_vkQueuePresentKHR = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
using PFN_vkQueueWaitIdle = VkResult(VKAPI_PTR*)(VkQueue);
using PFN_vkSetHdrMetadataEXT = void(VKAPI_PTR*)(VkDevice, std::uint32_t, const VkSwapchainKHR*, const VkHdrMetadataEXT*);
