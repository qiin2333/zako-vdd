#include "../../Common/Include/vulkan_hdr_policy.h"
#include "../../Common/Include/vulkan_hdr_capability_cache.h"

#include <iostream>
#include <vector>
#include <cstdio>

int main() {
  using namespace zako::vulkan_hdr;
  std::vector<FormatPair> formats {{64, 0}, {44, 0}, {12, 1000104008}};
  append_missing_verified_pairs(formats, true, true);
  if (!contains_pair(formats, kHdr10Pair) || !contains_pair(formats, kScRgbPair) || formats.size() != 5) return 1;
  append_missing_verified_pairs(formats, true, true);
  if (formats.size() != 5) return 2;

  std::vector<FormatPair> hdr10_only;
  append_missing_verified_pairs(hdr10_only, true, false);
  if (!contains_pair(hdr10_only, kHdr10Pair) || contains_pair(hdr10_only, kScRgbPair)) return 3;

  CapabilityCacheEntry entry {};
  entry.vendor_id = 0x10de;
  entry.device_id = 0x2684;
  entry.driver_version = 123;
  entry.api_version = 456;
  entry.adapter_luid_low = 789;
  entry.windows_build = 26100;
  entry.flags = kCapabilityHdr10Swapchain | kCapabilityValidatedWhileHdrActive;
  std::vector<CapabilityCacheEntry> cache;
  upsert_capability(cache, entry);
  const std::wstring path = L"build\\vulkan_hdr_capability_test.bin";
  if (!write_capability_cache(path, cache)) return 4;
  const auto loaded = read_capability_cache(path);
  std::remove("build\\vulkan_hdr_capability_test.bin");
  if (loaded.size() != 1 || !same_capability_key(loaded.front(), entry) || loaded.front().flags != entry.flags) return 5;
  std::cout << "vulkan_hdr_policy_test=PASS\n";
  return 0;
}
