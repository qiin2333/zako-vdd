#pragma once
#include "../../ZakoVDD/Edid/EdidTiming.h"

namespace {
struct NativeTiming {
    int width;
    int height;
    double refresh;
};

NativeTiming DecodeNative(const Bytes& edid) {
    const int width = edid[56] | ((edid[58] >> 4) << 8);
    const int height = edid[59] | ((edid[61] >> 4) << 8);
    const int horizontalTotal = width + edid[57] + ((edid[58] & 15) << 8);
    const int verticalTotal = height + edid[60] + ((edid[61] & 15) << 8);
    const double clock = (edid[54] | (edid[55] << 8)) * 10000.0;
    return {width, height, clock / (horizontalTotal * static_cast<double>(verticalTotal))};
}

void CheckNativeTiming(VddEdid::Profile profile) {
    using Modes = std::vector<VddEdid::NativeMode>;
    for (bool dolby : {false, true}) {
        const auto original = VddEdid::GetDefaultEdid(profile, dolby);
        auto edid = original;
        Modes modes{{1280, 720, 60, 1}, {1920, 1080, 60, 1},
                    {2560, 1440, 60, 1}, {3840, 2160, 120, 1},
                    {3840, 2160, 60000, 1001}, {3840, 2160, 60, 1}};
        Require(VddEdid::UpdateNativeTiming(edid, modes, true), "Built-in native timing was not updated");
        const auto native = DecodeNative(edid);
        Require(native.width == 3840 && native.height == 2160 && std::abs(native.refresh - 60) < 0.002,
                "Native timing does not describe the largest supported size at 60 Hz");
        ParseCta(edid);
        for (size_t i = 0; i < edid.size(); ++i) {
            if ((i < 54 || i >= 72) && i != 127)
                Require(edid[i] == original[i], "Native timing changed identity, HDR, audio or another descriptor");
        }
        Require(std::equal(original.begin() + 66, original.begin() + 69, edid.begin() + 66),
                "Native timing changed physical dimensions");
        std::reverse(modes.begin(), modes.end());
        auto reversed = original;
        Require(VddEdid::UpdateNativeTiming(reversed, modes, true) && reversed == edid,
                "Native timing depends on input mode order");
        Require(VddEdid::GetDefaultEdid(profile, dolby) == original, "Shared template mutated");

        auto custom = original;
        Require(!VddEdid::UpdateNativeTiming(custom, modes, false) && custom == original,
                "User-supplied EDID timing was overwritten");
        Require(VddEdid::UpdateNativeTiming(edid, {{2560, 1440, 60, 1}}, true),
                "Reused client EDID did not accept a smaller native size");
        Require(DecodeNative(edid).width == 2560 && DecodeNative(edid).height == 1440,
                "Cached client retained the previous native resolution");
        Require(VddEdid::UpdateNativeTiming(edid, {{2160, 3840, 60000, 1001}}, true),
                "Portrait/fractional timing rejected");
        const auto portrait = DecodeNative(edid);
        Require(portrait.width == 2160 && portrait.height == 3840 &&
                std::abs(portrait.refresh - 60000.0 / 1001) < 0.002,
                "Portrait/fractional timing was truncated");
        ParseCta(edid);

        const Modes invalid{{0, 1080, 60, 1}, {-1, 1080, 60, 1}, {1920, 0, 60, 1},
                            {1920, 1080, 0, 1}, {1920, 1080, 60, 0},
                            {1920, 1080, -60, 1}, {1920, 1080, 60, -1},
                            {7680, 4320, 60, 1}, {4096, 2160, 60, 1},
                            {2160, 4096, 60, 1}, {3840, 2160, 120, 1},
                            {1920, 1080, 1, 2147483647},
                            {4095, 4095, 2147483647, 1}};
        for (const auto& invalidModes : {Modes{}, invalid}) {
            auto unchanged = original;
            Require(!VddEdid::UpdateNativeTiming(unchanged, invalidModes, true) && unchanged == original,
                    "Unencodable/invalid modes partially changed the EDID");
        }
        auto mixed = original;
        auto mixedModes = invalid;
        mixedModes.emplace_back(1920, 1080, 60, 1);
        Require(VddEdid::UpdateNativeTiming(mixed, mixedModes, true) && DecodeNative(mixed).width == 1920,
                "Invalid mode hid the supported fallback");
        auto boundary = original;
        Require(VddEdid::UpdateNativeTiming(boundary, {{4095, 4095, 1, 1}}, true) &&
                DecodeNative(boundary).width == 4095 && DecodeNative(boundary).height == 4095,
                "12-bit active-size boundary was truncated");
        ParseCta(boundary);
        auto shortEdid = Bytes(original.begin(), original.begin() + 127);
        const auto shortOriginal = shortEdid;
        Require(!VddEdid::UpdateNativeTiming(shortEdid, modes, true) && shortEdid == shortOriginal,
                "Short EDID was modified");
        auto noTiming = original;
        noTiming[54] = noTiming[55] = 0;
        const auto noTimingOriginal = noTiming;
        Require(!VddEdid::UpdateNativeTiming(noTiming, modes, true) && noTiming == noTimingOriginal,
                "Non-timing descriptor was overwritten");
    }
    std::cout << "Native EDID: 4K/portrait/fractional modes, custom preservation, repeated clients and bounds PASS\n";
}
}
