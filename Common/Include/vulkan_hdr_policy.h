#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace zako::vulkan_hdr {

struct FormatPair {
  std::int32_t format {};
  std::int32_t color_space {};

  friend constexpr bool operator==(const FormatPair& left, const FormatPair& right) {
    return left.format == right.format && left.color_space == right.color_space;
  }
};

inline constexpr FormatPair kHdr10Pair {64, 1000104008};
inline constexpr FormatPair kScRgbPair {97, 1000104002};

inline bool contains_pair(const std::vector<FormatPair>& formats, FormatPair pair) {
  return std::find(formats.begin(), formats.end(), pair) != formats.end();
}

inline void append_missing_verified_pairs(std::vector<FormatPair>& formats, bool allow_hdr10, bool allow_scrgb) {
  if (allow_scrgb && !contains_pair(formats, kScRgbPair)) formats.push_back(kScRgbPair);
  if (allow_hdr10 && !contains_pair(formats, kHdr10Pair)) formats.push_back(kHdr10Pair);
}

}  // namespace zako::vulkan_hdr
