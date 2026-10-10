#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace edvr::plugins::performance {

// Private fixed-width boundary between the device-hook host and the
// Performance sampler policy. The host owns the D3D descriptor and real call.
struct TextureSamplerDescV1 {
    uint32_t filter;
    uint32_t addressU;
    uint32_t addressV;
    uint32_t addressW;
    float mipLodBias;
    uint32_t maxAnisotropy;
    uint32_t comparisonFunc;
    float borderColor[4];
    float minLod;
    float maxLod;
};

struct TextureSamplerSettingsV1 {
    uint32_t structSize;
    uint32_t version;
    uint32_t rewriteEnabled;
    int32_t anisotropy;
    float bias;
    uint32_t biasAuto;
    uint32_t biasAutoRequested;
    uint32_t autoSourceAvailable;
    float autoMultiplier;
    char biasWhy[220];
};

using ReadEliteHmdMultiplierV1 = bool (*)(void* context, float* multiplier,
                                           float* ssaa, char* file,
                                           uint32_t fileCapacity);

using ConfigureTextureSamplerV1 = void (*)(void* policyContext,
                                           void* hostContext,
                                           ReadEliteHmdMultiplierV1 readMultiplier,
                                           TextureSamplerSettingsV1* settings);
using RewriteTextureSamplerV1 = bool (*)(void* policyContext,
                                        const TextureSamplerSettingsV1* settings,
                                        const TextureSamplerDescV1* input,
                                        TextureSamplerDescV1* output);

struct TextureSamplerPolicyOpsV1 {
    uint32_t structSize;
    uint32_t version;
    void* context;
    ConfigureTextureSamplerV1 configure;
    RewriteTextureSamplerV1 rewrite;
};

static_assert(sizeof(void*) == 8, "sampler policy ABI is x64-only");
static_assert(std::is_standard_layout<TextureSamplerDescV1>::value &&
              std::is_trivially_copyable<TextureSamplerDescV1>::value,
              "sampler descriptor boundary must remain plain POD");
static_assert(sizeof(TextureSamplerDescV1) == 52 &&
              offsetof(TextureSamplerDescV1, borderColor) == 28 &&
              offsetof(TextureSamplerDescV1, minLod) == 44 &&
              offsetof(TextureSamplerDescV1, maxLod) == 48,
              "sampler descriptor v1 layout drifted");
static_assert(std::is_standard_layout<TextureSamplerSettingsV1>::value &&
              std::is_trivially_copyable<TextureSamplerSettingsV1>::value,
              "sampler settings boundary must remain plain POD");
static_assert(sizeof(TextureSamplerSettingsV1) == 256 &&
              offsetof(TextureSamplerSettingsV1, anisotropy) == 12 &&
              offsetof(TextureSamplerSettingsV1, bias) == 16 &&
              offsetof(TextureSamplerSettingsV1, biasWhy) == 36,
              "sampler settings v1 layout drifted");
static_assert(std::is_standard_layout<TextureSamplerPolicyOpsV1>::value &&
              std::is_trivially_copyable<TextureSamplerPolicyOpsV1>::value &&
              sizeof(TextureSamplerPolicyOpsV1) == 32 &&
              offsetof(TextureSamplerPolicyOpsV1, context) == 8 &&
              offsetof(TextureSamplerPolicyOpsV1, configure) == 16 &&
              offsetof(TextureSamplerPolicyOpsV1, rewrite) == 24,
              "sampler policy ops v1 layout drifted");

constexpr uint32_t kTextureSamplerSettingsVersion1 = 1;
constexpr uint32_t kTextureSamplerPolicyOpsVersion1 = 1;

const TextureSamplerPolicyOpsV1* textureSamplerPolicyOpsV1();

// Called once while the first device's hooks are installed. Config keys are
// read here; the callback is the host-owned Elite settings-file service.
void resolveTextureSamplerSettingsV1(TextureSamplerSettingsV1* settings,
                                     void* hostContext,
                                     ReadEliteHmdMultiplierV1 readMultiplier);

// Same production resolver with its already-read config values supplied by a
// fixture. Production calls it only through resolveTextureSamplerSettingsV1.
void resolveTextureSamplerSettingsFromValuesV1(
    TextureSamplerSettingsV1* settings, int32_t anisotropy,
    bool temporalAAEnabled, const char* biasKey, void* hostContext,
    ReadEliteHmdMultiplierV1 readMultiplier);

// Pure policy operation. Returns true only when an accepted stock filter was
// changed; every non-policy descriptor field is copied through untouched.
bool rewriteTextureSamplerV1(const TextureSamplerSettingsV1* settings,
                             const TextureSamplerDescV1* input,
                             TextureSamplerDescV1* output);

}  // namespace edvr::plugins::performance
