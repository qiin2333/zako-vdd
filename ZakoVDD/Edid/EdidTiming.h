#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <vector>

namespace VddEdid {

using NativeMode = std::tuple<int, int, int, int>; // width, height, refresh numerator/denominator

// Keep the built-in EDID's native dimensions in step with the advertised modes.
// Applications can read the first EDID DTD instead of the IddCx mode list (e.g.
// Oblivion Remastered otherwise caps its resolution menu at the template's 1080p).
// Custom EDIDs belong to the user and must retain their timing descriptors.
inline bool UpdateNativeTiming(std::vector<std::uint8_t>& edid,
                               const std::vector<NativeMode>& modes,
                               bool builtInEdid) {
    if (!builtInEdid || edid.size() < 128 || edid[18] != 1 ||
        (edid[19] != 3 && edid[19] != 4) || (edid[54] == 0 && edid[55] == 0)) {
        return false;
    }

    std::array<std::uint8_t, 18> timing{};
    std::uint64_t bestArea = 0;
    int bestWidth = 0;
    double bestDistance = 0;
    double bestRefresh = 0;
    bool found = false;
    for (const auto& mode : modes) {
        const auto [width, height, numerator, denominator] = mode;
        // EDID 1.x DTDs have 12-bit active sizes and a 16-bit 10 kHz clock.
        // Do not truncate an 8K/ultrawide dimension or invent an unsupported mode.
        if (width <= 0 || width > 4095 || height <= 0 || height > 4095 ||
            numerator <= 0 || denominator <= 0) {
            continue;
        }
        // Same four-pixel blanking as DisplayModeHelpers::dispinfo. Round only
        // the EDID clock to its 10 kHz unit; preserve rational refresh selection.
        const std::uint64_t clockNumerator = static_cast<std::uint64_t>(numerator) *
            (static_cast<std::uint64_t>(width) + 4) * (static_cast<std::uint64_t>(height) + 4);
        const std::uint64_t clockDenominator = static_cast<std::uint64_t>(denominator) * 10000;
        const std::uint64_t clock = (clockNumerator + clockDenominator / 2) / clockDenominator;
        if (clock == 0 || clock > 65535) continue;

        const std::uint64_t area = static_cast<std::uint64_t>(width) * height;
        const double refresh = static_cast<double>(numerator) / denominator;
        const double distance = std::abs(refresh - 60.0);
        // Native size is a capability, not the current desktop size. At the
        // largest encodable size prefer a refresh near 60 Hz, deterministically
        // regardless of SETMODES ordering. The IddCx mode list/order is unchanged.
        if (found && (area < bestArea || (area == bestArea && width < bestWidth) ||
            (area == bestArea && width == bestWidth &&
             (distance > bestDistance || (distance == bestDistance && refresh >= bestRefresh))))) {
            continue;
        }
        found = true;
        bestArea = area;
        bestWidth = width;
        bestDistance = distance;
        bestRefresh = refresh;
        timing = {
            static_cast<std::uint8_t>(clock & 255), static_cast<std::uint8_t>(clock >> 8),
            static_cast<std::uint8_t>(width & 255), 4,
            static_cast<std::uint8_t>((width >> 8) << 4),
            static_cast<std::uint8_t>(height & 255), 4,
            static_cast<std::uint8_t>((height >> 8) << 4),
            1, 1, 0x11, 0, // one-pixel sync offsets/widths fit the four-pixel blanking
            edid[66], edid[67], edid[68], // retain physical size
            0, 0, 0x1e // progressive, separate positive H/V sync
        };
    }
    if (!found) return false;

    std::copy(timing.begin(), timing.end(), edid.begin() + 54);
    unsigned int sum = 0;
    for (std::size_t i = 0; i < 127; ++i) sum += edid[i];
    edid[127] = static_cast<std::uint8_t>((256 - (sum & 255)) & 255);
    return true;
}

} // namespace VddEdid
