#include "../../ZakoVDD/Edid/DefaultEdid.h"

#include <algorithm>
#include <fstream>
#include <iostream>
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
}

int main(int argc, char** argv) {
    try {
        const std::string outputDirectory = argc > 1 ? argv[1] : "";
        CheckProfile(VddEdid::Profile::Legacy, VddEdid::DEFAULT_EDID_DATA_LEGACY, "legacy", outputDirectory);
        CheckProfile(VddEdid::Profile::Modern, VddEdid::DEFAULT_EDID_DATA_MODERN, "modern", outputDirectory);
        Require(GetDefaultEdid() == VddEdid::GetDefaultEdid(VddEdid::Profile::Legacy, false),
                "Legacy global API compatibility changed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
