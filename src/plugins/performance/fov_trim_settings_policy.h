#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace edvr::plugins::performance {

constexpr uint32_t kFovTrimSettingsPolicyVersion1 = 1;
constexpr uint32_t kFovTrimSettingsOpsVersion1 = 1;
constexpr uint32_t kFovTrimSettingsKeyCount = 3;
constexpr uint32_t kFovTrimSettingsTokenCapacity = 31;

enum class FovTrimSettingsNoteV1 : uint32_t {
    IgnoredEntry = 1,
    ResolvedEntries = 2,
    NoHeadset = 3,
};

// Fixed-width, private POD boundary. The host supplies already-sanitized labels;
// the policy reads Config itself and owns parsing, cache and diagnostic cadence.
struct FovTrimSettingsInputV1 {
    uint32_t structSize;
    uint32_t version;
    uint32_t haveHeadset;
    char runtimeToken[kFovTrimSettingsTokenCapacity];
    char systemToken[kFovTrimSettingsTokenCapacity];
};

struct FovTrimSettingsOutputV1 {
    uint32_t structSize;
    uint32_t version;
    uint32_t degrees[kFovTrimSettingsKeyCount];
};

using NoteFovTrimSettingsV1 = void (*)(
    void* hostContext, FovTrimSettingsNoteV1 kind, uint32_t keyIndex,
    const char* keyName, const char* token, const char* headsetKey,
    uint32_t vertical, uint32_t outer, uint32_t nasal, const char* entries);

using CreateFovTrimSettingsV1 = void* (*)();
using DestroyFovTrimSettingsV1 = void (*)(void* policyContext);
using ResolveFovTrimSettingsV1 = bool (*)(
    void* policyContext, void* hostContext,
    NoteFovTrimSettingsV1 note,
    const FovTrimSettingsInputV1* input,
    FovTrimSettingsOutputV1* output);

struct FovTrimSettingsPolicyOpsV1 {
    uint32_t structSize;
    uint32_t version;
    CreateFovTrimSettingsV1 create;
    DestroyFovTrimSettingsV1 destroy;
    ResolveFovTrimSettingsV1 resolve;
};

static_assert(sizeof(void*) == 8, "FOV trim settings policy is x64-only");
static_assert(std::is_standard_layout<FovTrimSettingsInputV1>::value &&
                  std::is_trivially_copyable<FovTrimSettingsInputV1>::value &&
                  sizeof(FovTrimSettingsInputV1) == 76 &&
                  offsetof(FovTrimSettingsInputV1, runtimeToken) == 12 &&
                  offsetof(FovTrimSettingsInputV1, systemToken) == 43,
              "FOV trim settings input v1 layout drifted");
static_assert(std::is_standard_layout<FovTrimSettingsOutputV1>::value &&
                  std::is_trivially_copyable<FovTrimSettingsOutputV1>::value &&
                  sizeof(FovTrimSettingsOutputV1) == 20 &&
                  offsetof(FovTrimSettingsOutputV1, degrees) == 8,
              "FOV trim settings output v1 layout drifted");
static_assert(std::is_standard_layout<FovTrimSettingsPolicyOpsV1>::value &&
                  std::is_trivially_copyable<FovTrimSettingsPolicyOpsV1>::value &&
                  sizeof(FovTrimSettingsPolicyOpsV1) == 32 &&
                  offsetof(FovTrimSettingsPolicyOpsV1, create) == 8 &&
                  offsetof(FovTrimSettingsPolicyOpsV1, destroy) == 16 &&
                  offsetof(FovTrimSettingsPolicyOpsV1, resolve) == 24,
              "FOV trim settings policy ops v1 layout drifted");

const FovTrimSettingsPolicyOpsV1* fovTrimSettingsPolicyOpsV1();

}  // namespace edvr::plugins::performance
