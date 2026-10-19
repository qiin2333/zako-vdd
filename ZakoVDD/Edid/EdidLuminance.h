#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace VddEdid {

inline double ClampLuminance(double value, double low, double high) {
    return value < low ? low : (value > high ? high : value);
}

inline double NitsToPq(double nits) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    const double y = std::pow(ClampLuminance(nits, 0.0, 10000.0) / 10000.0, m1);
    return 4095.0 * std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

// Update every advertised CTA luminance block, preserving capability/interface
// bits, chromaticities, timings and identities. DV V2 has no MaxFALL field.
inline bool UpdateLuminance(std::vector<std::uint8_t>& edid,
                            float maxNits, float minNits, float maxFALL) {
    if (edid.size() < 256 || edid.size() % 128 != 0 ||
        !std::isfinite(maxNits) || maxNits <= 0 ||
        !std::isfinite(minNits) || minNits < 0 || minNits > maxNits ||
        !std::isfinite(maxFALL) || maxFALL < 0) {
        return false;
    }
    const double fall = maxFALL == 0 ? maxNits * 0.8 : ClampLuminance(maxFALL, 0.0, maxNits);
    const auto encodeHdr = [](double nits) {
        return static_cast<std::uint8_t>(ClampLuminance(std::ceil(32.0 * std::log2(nits / 50.0)), 0.0, 255.0));
    };
    bool updated = false;
    for (std::size_t start = 128; start + 128 <= edid.size() && start / 128 <= edid[126]; start += 128) {
        if (edid[start] != 2 || edid[start + 2] < 4 || edid[start + 2] > 127) continue;
        const std::size_t end = start + edid[start + 2];
        bool updatedBlock = false;
        for (std::size_t pos = start + 4; pos < end;) {
            const auto length = edid[pos] & 0x1f;
            if (pos + 1 + length > end) break;
            if ((edid[pos] >> 5) == 7 && length >= 4 && edid[pos + 1] == 6) {
                edid[pos + 4] = encodeHdr(maxNits);
                if (length >= 5) edid[pos + 5] = encodeHdr(fall);
                if (length >= 6) {
                    // CTA minimum luminance is relative to the encoded maximum,
                    // with a squared code value (not a linear ratio).
                    const double peak = 50.0 * std::pow(2.0, edid[pos + 4] / 32.0);
                    edid[pos + 6] = static_cast<std::uint8_t>(ClampLuminance(
                        std::round(255.0 * std::sqrt(100.0 * minNits / peak)), 0.0, 255.0));
                }
                updatedBlock = true;
            }
            if ((edid[pos] >> 5) == 7 && length >= 11 && edid[pos + 1] == 1 &&
                edid[pos + 2] == 0x46 && edid[pos + 3] == 0xd0 && edid[pos + 4] == 0 &&
                (edid[pos + 5] >> 5) == 2) {
                // V2 fields as decoded by v4l-utils/edid-decode:
                // minPQ = 20 * code; maxPQ = 2055 + 65 * code.
                // The lower three bits include the required LL-HDMI interface.
                const auto minCode = static_cast<std::uint8_t>(ClampLuminance(
                    std::round(NitsToPq(minNits) / 20.0), 0.0, 31.0));
                const auto maxCode = static_cast<std::uint8_t>(ClampLuminance(
                    std::round((NitsToPq(maxNits) - 2055.0) / 65.0), 0.0, 31.0));
                edid[pos + 6] = static_cast<std::uint8_t>((edid[pos + 6] & 7) | (minCode << 3));
                edid[pos + 7] = static_cast<std::uint8_t>((edid[pos + 7] & 7) | (maxCode << 3));
                updatedBlock = true;
            }
            pos += 1 + length;
        }
        if (updatedBlock) {
            unsigned int sum = 0;
            for (std::size_t pos = start; pos < start + 127; ++pos) sum += edid[pos];
            edid[start + 127] = static_cast<std::uint8_t>((256 - (sum & 255)) & 255);
            updated = true;
        }
    }
    return updated;
}

} // namespace VddEdid
