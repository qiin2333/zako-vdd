#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>
#include <windows.h>

namespace VddEdid {

enum class NativeResolutionResult {
    Default, Applied, CustomEdid, InvalidEdid, InvalidResolution, UnsupportedMode, UnencodableTiming
};

inline bool ParseNativeResolution(const std::wstring& value, int& width, int& height) {
    const auto separator = value.find(L'x');
    if (separator == value.npos || separator == 0 || separator + 1 == value.size()) return false;
    auto dimension = [](const std::wstring& digits, int& result) {
        result = 0;
        for (wchar_t digit : digits) {
            if (digit < L'0' || digit > L'9' || result > 409) return false;
            result = result * 10 + (digit - L'0');
            if (result > 4095) return false;
        }
        return result > 0;
    };
    return dimension(value.substr(0, separator), width) && dimension(value.substr(separator + 1), height);
}

inline NativeResolutionResult ApplyNativeResolution(std::vector<BYTE>& edid, bool builtIn,
                                  const std::wstring& requested,
                                  const std::vector<std::tuple<int, int, int, int>>& modes) {
    if (!builtIn) return NativeResolutionResult::CustomEdid;
    if (edid.size() < 128 || edid[18] != 1 || (edid[19] != 3 && edid[19] != 4))
        return NativeResolutionResult::InvalidEdid;

    constexpr BYTE fullHd[] = {0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
                              0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00};
    constexpr BYTE ultraHd[] = {0x08, 0xe8, 0x00, 0x30, 0xf2, 0x70,
                               0x5a, 0x80, 0xb0, 0x58, 0x8a, 0x00};
    // Always restore the original timing on default/invalid selections, even
    // when this client previously had a different native resolution cached.
    std::array<BYTE, 12> timing;
    std::copy(std::begin(fullHd), std::end(fullHd), timing.begin());
    auto result = NativeResolutionResult::Default;
    if (!requested.empty() && requested != L"default") {
        int width = 0, height = 0;
        if (!ParseNativeResolution(requested, width, height)) {
            result = NativeResolutionResult::InvalidResolution;
        } else {
            result = NativeResolutionResult::UnsupportedMode;
            bool found = false;
            double bestDistance = 0, bestRefresh = 0;
            for (const auto& mode : modes) {
                const int w = std::get<0>(mode), h = std::get<1>(mode);
                const int numerator = std::get<2>(mode), denominator = std::get<3>(mode);
                if (w != width || h != height || numerator <= 0 || denominator <= 0) continue;
                if (!found) result = NativeResolutionResult::UnencodableTiming;
                const bool at60 = static_cast<std::uint64_t>(numerator) == 60ull * denominator;
                const bool fullHd60 = at60 && w == 1920 && h == 1080;
                const bool ultraHd60 = at60 && w == 3840 && h == 2160;
                // Other virtual modes use the same four-pixel blanking as
                // DisplayModeHelpers::dispinfo, rounded to EDID's 10 kHz unit.
                const std::uint64_t clockNumerator = static_cast<std::uint64_t>(numerator) * (w + 4) * (h + 4);
                const std::uint64_t clockDenominator = static_cast<std::uint64_t>(denominator) * 10000;
                const std::uint64_t clock = fullHd60 ? 14850 : ultraHd60 ? 59400 :
                    (clockNumerator + clockDenominator / 2) / clockDenominator;
                if (clock == 0 || clock > 65535) continue;
                const double refresh = static_cast<double>(numerator) / denominator;
                const double distance = std::abs(refresh - 60.0);
                if (found && (distance > bestDistance || (distance == bestDistance && refresh >= bestRefresh))) continue;
                found = true; bestDistance = distance; bestRefresh = refresh;
                result = NativeResolutionResult::Applied;
                if (fullHd60 || ultraHd60) {
                    const auto* preset = fullHd60 ? fullHd : ultraHd;
                    std::copy(preset, preset + timing.size(), timing.begin());
                } else {
                    timing = {static_cast<BYTE>(clock & 255), static_cast<BYTE>(clock >> 8),
                        static_cast<BYTE>(w & 255), 4, static_cast<BYTE>((w >> 8) << 4),
                        static_cast<BYTE>(h & 255), 4, static_cast<BYTE>((h >> 8) << 4),
                        1, 1, 0x11, 0};
                }
            }
        }
    }
    // Retain physical size, borders, positive H/V flags and every CTA byte.
    std::copy(timing.begin(), timing.end(), edid.begin() + 54);
    unsigned int sum = 0;
    for (size_t i = 0; i < 127; ++i) sum += edid[i];
    edid[127] = static_cast<BYTE>((256 - (sum & 255)) & 255);
    return result;
}

} // namespace VddEdid
