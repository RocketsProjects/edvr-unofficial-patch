#include "fov_trim_settings_policy.h"

#include "../../common/config.h"
#include "../../common/openxr_resolution_entries.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace edvr::plugins::performance {
namespace {

constexpr size_t kTrimCount = kFovTrimSettingsKeyCount;
constexpr size_t kTrimNamedMax = 32;
const char* const kTrimKeys[kTrimCount] = {
    "experimental.fov_trim_vertical", "experimental.fov_trim_outer",
    "experimental.fov_trim_nasal"};
const char* const kTrimNames[kTrimCount] = {"vertical", "outer", "nasal"};

struct TrimState {
    bool resolved = false;
    std::string values[kTrimCount];
    std::string rt, sys;
    bool headset = false;
    uint32_t degrees[kTrimCount] = {0, 0, 0};
    bool headsetNoted = false;
    bool noHeadsetNoted = false;
    std::string notedKey;
    uint32_t notedDegrees[kTrimCount] = {0, 0, 0};
    std::vector<std::string> named;
};

void* create() {
    return new (std::nothrow) TrimState();
}

void destroy(void* policyContext) {
    delete static_cast<TrimState*>(policyContext);
}

bool resolve(void* policyContext, void* hostContext,
             NoteFovTrimSettingsV1 note,
             const FovTrimSettingsInputV1* input,
             FovTrimSettingsOutputV1* output) {
    if (!output) return false;
    *output = {};
    output->structSize = sizeof(*output);
    output->version = kFovTrimSettingsPolicyVersion1;
    if (!policyContext || !note || !input ||
        input->structSize != sizeof(*input) ||
        input->version != kFovTrimSettingsPolicyVersion1 ||
        input->haveHeadset > 1 ||
        !std::memchr(input->runtimeToken, '\0', sizeof(input->runtimeToken)) ||
        !std::memchr(input->systemToken, '\0', sizeof(input->systemToken))) {
        return false;
    }

    auto& state = *static_cast<TrimState*>(policyContext);
    const bool haveHeadset = input->haveHeadset != 0;
    const std::string rt = haveHeadset ? input->runtimeToken : "";
    const std::string sys = haveHeadset ? input->systemToken : "";
    // Read each literal key in the owning module. Config copies each value
    // under its own lock, so no borrowed text or host-side scratch is needed.
    const std::string values[kTrimCount] = {
        edvr::Config::get().getString("experimental.fov_trim_vertical", ""),
        edvr::Config::get().getString("experimental.fov_trim_outer", ""),
        edvr::Config::get().getString("experimental.fov_trim_nasal", "")};
    if (state.resolved && state.headset == haveHeadset && state.rt == rt &&
        state.sys == sys && state.values[0] == values[0] &&
        state.values[1] == values[1] && state.values[2] == values[2]) {
        for (size_t i = 0; i < kTrimCount; ++i) output->degrees[i] = state.degrees[i];
        return true;
    }

    using namespace edvr::native_render;
    HeadsetEntry entries[kTrimCount][kHeadsetEntryMax];
    size_t counts[kTrimCount] = {0, 0, 0};
    uint32_t degrees[kTrimCount] = {0, 0, 0};
    bool anyEntry = false;
    for (size_t i = 0; i < kTrimCount; ++i) {
        std::vector<std::string> skipped;
        counts[i] = parseHeadsetEntries(values[i].c_str(), entries[i], &skipped,
                                        0, kTrimDegreesMax);
        if (counts[i]) anyEntry = true;
        degrees[i] = haveHeadset
            ? resolveHeadsetValue(entries[i], counts[i], rt, sys, nullptr) : 0u;
        for (const std::string& token : skipped) {
            const std::string seen = std::string(kTrimKeys[i]) + "|" + token;
            if (state.named.size() >= kTrimNamedMax ||
                std::find(state.named.begin(), state.named.end(), seen) !=
                    state.named.end()) {
                continue;
            }
            state.named.push_back(seen);
            note(hostContext, FovTrimSettingsNoteV1::IgnoredEntry,
                 static_cast<uint32_t>(i), kTrimKeys[i], token.c_str(),
                 nullptr, 0, 0, 0, nullptr);
        }
    }

    const std::string key = haveHeadset ? headsetKey(rt, sys) : std::string();
    if (haveHeadset) {
        const bool changed = !state.headsetNoted || state.notedKey != key ||
                             state.notedDegrees[0] != degrees[0] ||
                             state.notedDegrees[1] != degrees[1] ||
                             state.notedDegrees[2] != degrees[2];
        if (changed) {
            std::string saved;
            for (size_t i = 0; i < kTrimCount; ++i) {
                if (i) saved += "; ";
                saved += kTrimNames[i];
                saved += " ";
                saved += counts[i] ? formatHeadsetEntries(entries[i], counts[i])
                                   : std::string("none");
            }
            note(hostContext, FovTrimSettingsNoteV1::ResolvedEntries, 0,
                 nullptr, nullptr, key.c_str(), degrees[0], degrees[1],
                 degrees[2], saved.c_str());
            state.headsetNoted = true;
            state.noHeadsetNoted = false;
            state.notedKey = key;
            for (size_t i = 0; i < kTrimCount; ++i)
                state.notedDegrees[i] = degrees[i];
        }
    } else if (anyEntry && !state.noHeadsetNoted) {
        note(hostContext, FovTrimSettingsNoteV1::NoHeadset, 0, nullptr,
             nullptr, nullptr, 0, 0, 0, nullptr);
        state.noHeadsetNoted = true;
        state.headsetNoted = false;
    }

    state.resolved = true;
    state.headset = haveHeadset;
    state.rt = rt;
    state.sys = sys;
    for (size_t i = 0; i < kTrimCount; ++i) {
        state.values[i] = values[i];
        state.degrees[i] = degrees[i];
        output->degrees[i] = degrees[i];
    }
    return true;
}

}  // namespace

const FovTrimSettingsPolicyOpsV1* fovTrimSettingsPolicyOpsV1() {
    static const FovTrimSettingsPolicyOpsV1 ops = {
        sizeof(FovTrimSettingsPolicyOpsV1), kFovTrimSettingsOpsVersion1,
        &create, &destroy, &resolve};
    return &ops;
}

}  // namespace edvr::plugins::performance
