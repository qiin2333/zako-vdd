#include "../../ZakoVDD/Edid/DefaultEdid.h"
#include "../../ZakoVDD/Edid/EdidLuminance.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using Bytes = std::vector<BYTE>;

std::vector<Bytes> ParseCta(const Bytes& edid) {
    Require(edid.size() == 256 && edid[126] == 1, "Expected one CTA extension");
    Require(edid[128] == 2 && edid[129] == 3, "CTA revision changed");
    const size_t end = 128 + edid[130];
    Require(end >= 132 && end <= 255, "Invalid CTA data-block boundary");
    std::vector<Bytes> blocks;
    for (size_t pos = 132; pos < end;) {
        const size_t next = pos + 1 + (edid[pos] & 0x1f);
        Require(next <= end, "CTA data block overruns collection");
        blocks.emplace_back(edid.begin() + pos, edid.begin() + next);
        pos = next;
    }
    // The append operation relies on built-in CTA extensions having no DTDs.
    Require((edid[131] & 0x0f) == 0, "Unexpected CTA native DTD count");
    for (size_t pos = end; pos < 255; ++pos) {
        Require(edid[pos] == 0, "CTA append would overwrite a DTD or other nonzero data");
    }
    for (size_t block = 0; block < 2; ++block) {
        unsigned int sum = 0;
        for (size_t i = block * 128; i < (block + 1) * 128; ++i) sum += edid[i];
        Require((sum & 0xff) == 0, "EDID checksum invalid");
    }
    return blocks;
}

void CheckProfile(VddEdid::Profile profile, const BYTE* original, const char* name,
                  const std::string& outputDirectory) {
    auto disabled = VddEdid::GetDefaultEdid(profile, false);
    auto enabled = VddEdid::GetDefaultEdid(profile, true);
    Require(disabled == Bytes(original, original + 256), "Disabled switch changed original EDID");
    Require(VddEdid::GetDefaultEdid(profile) == disabled, "Old call site no longer defaults OFF");
    const auto oldBlocks = ParseCta(disabled);
    const auto newBlocks = ParseCta(enabled);
    Require(newBlocks.size() == oldBlocks.size() + 1, "Expected exactly one added CTA block");
    Require(std::equal(oldBlocks.begin(), oldBlocks.end(), newBlocks.begin()),
            "An existing HDR/colorimetry/audio/HDMI/FreeSync block changed");
    // Golden payload from the Windows false -> true experiment; 0x87 includes
    // the critical Low-Latency-HDMI bit missing from the original 0x86 payload.
    const Bytes expectedDolby = {0xeb, 0x01, 0x46, 0xd0, 0x00, 0x49,
                                0x03, 0x87, 0x58, 0x99, 0xaa, 0x5c};
    Require(newBlocks.back() == expectedDolby, "Dolby declaration differs from verified payload");
    Require(std::equal(disabled.begin(), disabled.begin() + 128, enabled.begin()),
            "Base block, timing, identity or extension count changed");
    Require(enabled[131] == disabled[131], "CTA display feature flags changed");
    Require(enabled[130] == disabled[130] + expectedDolby.size(), "CTA boundary not advanced");

    // Every monitor/reload must start from a fresh template. An enabled call
    // or per-monitor edits must not leak Dolby Vision into a later OFF call.
    enabled[21] = 1;
    enabled[136] = 0;
    Require(VddEdid::GetDefaultEdid(profile, false) == disabled, "EDID template mutated");
    Require(VddEdid::GetDefaultEdid(profile, true).at(136) == disabled[136],
            "Per-monitor HDR edits leaked into the next monitor");

    if (!outputDirectory.empty()) {
        for (bool on : {false, true}) {
            const auto data = VddEdid::GetDefaultEdid(profile, on);
            std::ofstream file(outputDirectory + "/" + name + (on ? "-dv.bin" : "-original.bin"),
                               std::ios::binary);
            file.write(reinterpret_cast<const char*>(data.data()), data.size());
            Require(file.good(), "Cannot write EDID fixture");
        }
    }
    std::cout << name << ": original preserved; Dolby payload, CTA boundaries, checksums and isolation PASS\n";
}

void CheckLuminance(VddEdid::Profile profile) {
    for (bool dolby : {false, true}) {
        auto edid = VddEdid::GetDefaultEdid(profile, dolby);
        const auto original = edid;
        // Independent V2 code fixtures: 400/1000/2000 nits -> 9/16/21,
        // and 0.001/0.01/0.1 nits -> 1/4/13 (ST 2084 + V2 quantization).
        struct Target { float max, min, fall; BYTE dvMax, dvMin; };
        for (const Target target : {Target{400, 0.001f, 300, 9, 1},
                                    Target{1000, 0.01f, 600, 16, 4},
                                    Target{2000, 0.1f, 1200, 21, 13},
                                    Target{10000, 0.1f, 4000, 31, 13},
                                    Target{400, 0.001f, 300, 9, 1}}) {
            Require(VddEdid::UpdateLuminance(edid, target.max, target.min, target.fall), "Luminance update failed");
            const auto blocks = ParseCta(edid);
            const auto hdr = blocks.front();
            const double peak = 50.0 * std::pow(2.0, hdr[4] / 32.0);
            const double fall = 50.0 * std::pow(2.0, hdr[5] / 32.0);
            const double minimum = peak / 100.0 * std::pow(hdr[6] / 255.0, 2.0);
            Require(peak >= target.max && peak < target.max * 1.023, "HDR peak did not follow client");
            Require(fall >= target.fall && fall < target.fall * 1.023, "HDR full-frame peak did not follow client");
            Require(std::abs(minimum - target.min) < target.min * 0.5, "HDR black level used the wrong encoding");
            if (dolby) {
                const auto dv = blocks.back();
                Require((dv[6] >> 3) == target.dvMin && (dv[7] >> 3) == target.dvMax,
                        "Dolby Vision target did not follow client");
                Require((dv[6] & 7) == 3 && (dv[7] & 7) == 7,
                        "Dolby Vision feature or LL-HDMI bits changed");
            }
            for (size_t pos = 0; pos < edid.size(); ++pos) {
                if (pos == 136 || pos == 137 || pos == 138 || pos == 255 ||
                    (dolby && (pos == 191 || pos == 192))) continue;
                Require(edid[pos] == original[pos], "Luminance update changed unrelated capabilities");
            }
            const auto once = edid;
            Require(VddEdid::UpdateLuminance(edid, target.max, target.min, target.fall) && edid == once,
                    "An identical target changed the EDID again");
        }
        Require(VddEdid::GetDefaultEdid(profile, dolby) == original, "Per-client update mutated template");
        const auto beforeInvalid = edid;
        Require(!VddEdid::UpdateLuminance(edid, std::numeric_limits<float>::quiet_NaN(), 0, 0), "Accepted NaN");
        Require(!VddEdid::UpdateLuminance(edid, 0, 0, 0), "Accepted zero peak");
        Require(!VddEdid::UpdateLuminance(edid, 1000, -1, 0), "Accepted negative black level");
        Require(!VddEdid::UpdateLuminance(edid, 1000, 0, std::numeric_limits<float>::infinity()), "Accepted infinite MaxFALL");
        Require(edid == beforeInvalid, "Invalid request modified EDID");
        Require(VddEdid::UpdateLuminance(edid, 1000, 0, 0), "Default MaxFALL update failed");
        const auto hdr = ParseCta(edid).front();
        Require(hdr[6] == 0 && hdr[5] > 0 && hdr[5] < hdr[4], "Zero black level/default MaxFALL failed");

        // Use the same normalization as CreateMonitor, including cached EDIDs
        // that previously advertised a different client's capabilities.
        auto defaults = VddEdid::GetDefaultEdid(profile, dolby);
        Require(VddEdid::UpdateLuminance(defaults, 1000, 0.0001f, 1000), "Default fixture update failed");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        struct Luminance { float max, min, fall; };
        for (auto invalid : {Luminance{nan, 0.1f, 1200}, Luminance{inf, 0.1f, 1200},
                             Luminance{0, 0, 0}, Luminance{-1, 0, 0},
                             Luminance{2000, nan, 1200}, Luminance{2000, inf, 1200},
                             Luminance{2000, -1, 1200}, Luminance{400, 500, 300},
                             Luminance{2000, 0.1f, nan}, Luminance{2000, 0.1f, inf},
                             Luminance{2000, 0.1f, -1}}) {
            Require(VddEdid::UpdateLuminance(edid, 2000, 0.1f, 1200), "Cached fixture update failed");
            Require(VddEdid::UseDefaultLuminanceIfInvalid(invalid.max, invalid.min, invalid.fall),
                    "Invalid creation parameters did not fall back");
            Require(invalid.max == 1000 && invalid.min == 0.0001f && invalid.fall == 1000,
                    "Fallback did not replace the entire runtime luminance tuple");
            Require(VddEdid::UpdateLuminance(edid, invalid.max, invalid.min, invalid.fall) && edid == defaults,
                    "Fallback retained a previous client's EDID luminance");
            ParseCta(edid);
        }
        for (auto valid : {Luminance{400, 0.001f, 300}, Luminance{1000, 0, 0},
                           Luminance{1000, 1000, 2000}}) {
            const auto originalValid = valid;
            Require(!VddEdid::UseDefaultLuminanceIfInvalid(valid.max, valid.min, valid.fall),
                    "Valid creation parameters fell back");
            Require(valid.max == originalValid.max && valid.min == originalValid.min && valid.fall == originalValid.fall,
                    "Valid creation parameters changed");
        }
    }

    // A Dolby-only CTA extension after a malformed extension must still update.
    auto multi = VddEdid::GetDefaultEdid(profile, true);
    const auto cta = Bytes(multi.begin() + 128, multi.end());
    multi.insert(multi.end(), cta.begin(), cta.end());
    multi[126] = 2;
    multi[130] = 255;
    multi[258] = 16;
    std::copy(cta.begin() + 57, cta.begin() + 69, multi.begin() + 260);
    std::fill(multi.begin() + 272, multi.begin() + 383, BYTE{0});
    const auto malformed = Bytes(multi.begin() + 128, multi.begin() + 256);
    Require(VddEdid::UpdateLuminance(multi, 2000, 0.1f, 1200), "Did not recover at next extension");
    Require(std::equal(malformed.begin(), malformed.end(), multi.begin() + 128), "Touched malformed extension");
    Require((multi[267] >> 3) == 21 && (multi[266] >> 3) == 13, "Later Dolby block was not updated");
    unsigned int sum = 0;
    for (size_t pos = 256; pos < 384; ++pos) sum += multi[pos];
    Require((sum & 255) == 0, "Later extension checksum invalid");
    std::cout << "HDR/Dolby luminance sync, repeated clients, preservation, invalid input/default fallback and extension recovery PASS\n";
}
}

#include "native_timing_test.h"

int main(int argc, char** argv) {
    try {
        const std::string outputDirectory = argc > 1 ? argv[1] : "";
        CheckProfile(VddEdid::Profile::Legacy, VddEdid::DEFAULT_EDID_DATA_LEGACY, "legacy", outputDirectory);
        CheckProfile(VddEdid::Profile::Modern, VddEdid::DEFAULT_EDID_DATA_MODERN, "modern", outputDirectory);
        CheckLuminance(VddEdid::Profile::Legacy);
        CheckLuminance(VddEdid::Profile::Modern);
        CheckNativeTiming(VddEdid::Profile::Legacy);
        CheckNativeTiming(VddEdid::Profile::Modern);
        Require(GetDefaultEdid() == VddEdid::GetDefaultEdid(VddEdid::Profile::Legacy, false),
                "Legacy global API compatibility changed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
