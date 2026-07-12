#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace zako::vulkan_hdr {

inline constexpr std::uint32_t kCapabilityCacheMagic = 0x5A564843;  // 'ZVHC'
inline constexpr std::uint16_t kCapabilityCacheVersion = 1;
inline constexpr std::uint32_t kCapabilityHdr10Swapchain = 0x00000001u;
inline constexpr std::uint32_t kCapabilityScRgbSwapchain = 0x00000002u;
inline constexpr std::uint32_t kCapabilityValidatedWhileHdrActive = 0x00000004u;
inline constexpr std::uint32_t kCapabilityPresented = 0x00000008u;
inline constexpr std::uint32_t kCapabilityPixelsVerified = 0x00000010u;

struct CapabilityCacheHeader {
  std::uint32_t magic {kCapabilityCacheMagic};
  std::uint16_t version {kCapabilityCacheVersion};
  std::uint16_t header_size {sizeof(CapabilityCacheHeader)};
  std::uint32_t entry_size {};
  std::uint32_t entry_count {};
};

struct CapabilityCacheEntry {
  std::uint32_t vendor_id {};
  std::uint32_t device_id {};
  std::uint32_t driver_version {};
  std::uint32_t api_version {};
  std::uint32_t adapter_luid_low {};
  std::int32_t adapter_luid_high {};
  std::uint32_t windows_build {};
  std::uint32_t flags {};
  char device_name[128] {};
  std::uint32_t reserved[8] {};
};

static_assert(sizeof(CapabilityCacheHeader) == 16);
static_assert(sizeof(CapabilityCacheEntry) == 192);

inline bool same_capability_key(const CapabilityCacheEntry& left, const CapabilityCacheEntry& right) {
  return left.vendor_id == right.vendor_id && left.device_id == right.device_id &&
         left.driver_version == right.driver_version && left.api_version == right.api_version &&
         left.adapter_luid_low == right.adapter_luid_low && left.adapter_luid_high == right.adapter_luid_high &&
         left.windows_build == right.windows_build;
}

inline std::vector<CapabilityCacheEntry> read_capability_cache(const std::wstring& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  CapabilityCacheHeader header {};
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!input || header.magic != kCapabilityCacheMagic || header.version != kCapabilityCacheVersion ||
      header.header_size != sizeof(header) || header.entry_size != sizeof(CapabilityCacheEntry) ||
      header.entry_count > 64) return {};
  std::vector<CapabilityCacheEntry> entries(header.entry_count);
  input.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(CapabilityCacheEntry)));
  if (!input) return {};
  return entries;
}

inline bool write_capability_cache(const std::wstring& path, const std::vector<CapabilityCacheEntry>& entries) {
  CapabilityCacheHeader header {};
  header.entry_size = sizeof(CapabilityCacheEntry);
  header.entry_count = static_cast<std::uint32_t>(entries.size());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  output.write(reinterpret_cast<const char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(CapabilityCacheEntry)));
  return output.good();
}

inline void upsert_capability(std::vector<CapabilityCacheEntry>& entries, const CapabilityCacheEntry& value) {
  const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return same_capability_key(entry, value);
  });
  if (found == entries.end()) entries.push_back(value);
  else *found = value;
}

}  // namespace zako::vulkan_hdr
