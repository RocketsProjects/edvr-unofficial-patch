#include "../../src/common/config.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/plugins/performance/texture_sampler_policy.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace {
using namespace edvr::plugins::performance;

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct MultiplierFixture {
    bool available = false;
    float multiplier = 0.0f;
    float ssaa = 0.0f;
    const char* file = "fixture.2.4.fxcfg";
    unsigned calls = 0;
};

struct ConfigureHostFixture {
    TextureSamplerHostContextV1 admission{
        sizeof(TextureSamplerHostContextV1),
        kTextureSamplerHostContextVersion1,
        1u};
    MultiplierFixture* multiplier = nullptr;
    bool callbackReceivedAdmissionPointer = false;
};

static_assert(std::is_standard_layout<ConfigureHostFixture>::value,
              "the typed host context must remain the first fixture member");

bool readMultiplier(void* opaque, float* multiplier, float* ssaa, char* file,
                    uint32_t fileCapacity) {
    auto& host = *static_cast<ConfigureHostFixture*>(opaque);
    host.callbackReceivedAdmissionPointer = opaque == &host.admission;
    if (!host.multiplier) return false;
    auto& fixture = *host.multiplier;
    ++fixture.calls;
    if (!fixture.available) return false;
    if (multiplier) *multiplier = fixture.multiplier;
    if (ssaa) *ssaa = fixture.ssaa;
    if (file && fileCapacity) std::snprintf(file, fileCapacity, "%s", fixture.file);
    return true;
}

TextureSamplerSettingsV1 configure(const char* anisotropy, const char* bias,
                                   const char* temporal,
                                   MultiplierFixture* multiplier = nullptr,
                                   bool ownerAllowed = true) {
    edvr::Config::get().set("advanced.texture_anisotropy", anisotropy);
    if (bias) edvr::Config::get().set("advanced.texture_lod_bias", bias);
    edvr::Config::get().set("fix.temporal_aa", temporal);
    TextureSamplerSettingsV1 settings{};
    const auto* ops = textureSamplerPolicyOpsV1();
    check(ops && ops->structSize == sizeof(*ops) &&
              ops->version == kTextureSamplerPolicyOpsVersion1 && ops->configure &&
              ops->rewrite,
          "v1 policy ops table is complete");
    ConfigureHostFixture host{};
    host.admission.temporalAAOwnerAllowed = ownerAllowed ? 1u : 0u;
    host.multiplier = multiplier;
    ops->configure(ops->context, &host.admission, &readMultiplier, &settings);
    if (multiplier && multiplier->calls > 0)
        check(host.callbackReceivedAdmissionPointer,
              "multiplier callback receives the typed host context unchanged");
    check(settings.structSize == sizeof(settings) &&
              settings.version == kTextureSamplerSettingsVersion1,
          "configure produces the v1 POD settings record");
    return settings;
}

TextureSamplerSettingsV1 configureWithoutValidHostContext(void* hostContext) {
    edvr::Config::get().set("advanced.texture_anisotropy", "0");
    edvr::Config::get().set("fix.temporal_aa", "dlss");
    TextureSamplerSettingsV1 settings{};
    const auto* ops = textureSamplerPolicyOpsV1();
    check(ops && ops->configure, "policy configure operation is available");
    ops->configure(ops->context, hostContext, nullptr, &settings);
    check(settings.structSize == sizeof(settings) &&
              settings.version == kTextureSamplerSettingsVersion1,
          "invalid host context still produces initialized settings");
    return settings;
}

TextureSamplerDescV1 descriptor(uint32_t filter) {
    TextureSamplerDescV1 value{};
    value.filter = filter;
    value.addressU = 1;
    value.addressV = 2;
    value.addressW = 3;
    value.mipLodBias = -0.375f;
    value.maxAnisotropy = 7;
    value.comparisonFunc = 4;
    value.borderColor[0] = 0.125f;
    value.borderColor[1] = 0.25f;
    value.borderColor[2] = 0.5f;
    value.borderColor[3] = 1.0f;
    value.minLod = 0.0625f;
    value.maxLod = 14.0f;
    return value;
}

bool sameDescriptor(const TextureSamplerDescV1& a, const TextureSamplerDescV1& b) {
    if (a.filter != b.filter || a.addressU != b.addressU || a.addressV != b.addressV ||
        a.addressW != b.addressW || a.mipLodBias != b.mipLodBias ||
        a.maxAnisotropy != b.maxAnisotropy || a.comparisonFunc != b.comparisonFunc ||
        a.minLod != b.minLod || a.maxLod != b.maxLod) return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (a.borderColor[i] != b.borderColor[i]) return false;
    }
    return true;
}

void testUnsetDefaultsAndAuto() {
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    edvr::Config::get().set("advanced.texture_anisotropy", "0");
    edvr::Config::get().set("fix.temporal_aa", "off");
    auto settings = configure("0", nullptr, "off");
    check(settings.anisotropy == 0 && settings.bias == 0.0f &&
              !settings.biasAutoRequested && !settings.rewriteEnabled,
          "unset VR sampler settings retain zero defaults");

    // An admitted owner retains mode semantics: mode off defaults to zero,
    // while mode on defaults to auto and reaches the same opaque callback
    // context that carries the typed admission snapshot.
    MultiplierFixture admittedOnMultiplier{true, 0.5f, 1.25f};
    settings = configure("0", nullptr, "off", &admittedOnMultiplier, true);
    check(settings.bias == 0.0f && !settings.biasAutoRequested &&
              admittedOnMultiplier.calls == 0,
          "admitted TAA mode off keeps the zero default without a multiplier read");
    settings = configure("0", nullptr, "dlss", &admittedOnMultiplier, true);
    check(settings.biasAutoRequested && settings.autoSourceAvailable &&
              std::fabs(settings.bias - -1.0f) < 0.00001f &&
              admittedOnMultiplier.calls == 1,
          "admitted TAA mode on keeps the auto default and multiplier behavior");

    // Denial must not consult the TAA mode even when it is enabled and the
    // explicit LOD key is absent. The zero default and untouched callback
    // counter make an accidental mode read/auto branch observable.
    MultiplierFixture deniedMultiplier{true, 0.5f, 1.25f};
    settings = configure("0", nullptr, "dlss", &deniedMultiplier, false);
    check(settings.bias == 0.0f && !settings.biasAutoRequested &&
              !settings.rewriteEnabled && deniedMultiplier.calls == 0,
          "denied owner skips the TAA mode default and auto callback");

    TextureSamplerHostContextV1 malformed{
        sizeof(TextureSamplerHostContextV1),
        kTextureSamplerHostContextVersion1,
        2u};
    settings = configureWithoutValidHostContext(nullptr);
    check(settings.bias == 0.0f && !settings.biasAutoRequested &&
              !settings.rewriteEnabled,
          "null host context keeps the cold TAA default at zero");
    settings = configureWithoutValidHostContext(&malformed);
    check(settings.bias == 0.0f && !settings.biasAutoRequested &&
              !settings.rewriteEnabled,
          "out-of-range owner bit keeps the cold TAA default at zero");
    malformed.temporalAAOwnerAllowed = 1u;
    malformed.structSize -= 4u;
    settings = configureWithoutValidHostContext(&malformed);
    check(settings.bias == 0.0f && !settings.biasAutoRequested &&
              !settings.rewriteEnabled,
          "wrong host-context size keeps the cold TAA default at zero");
    malformed.structSize = sizeof(TextureSamplerHostContextV1);
    malformed.version += 1u;
    settings = configureWithoutValidHostContext(&malformed);
    check(settings.bias == 0.0f && !settings.biasAutoRequested &&
              !settings.rewriteEnabled,
          "wrong host-context version keeps the cold TAA default at zero");

    edvr::Config::get().set("fix.temporal_aa", "dlss");
    MultiplierFixture multiplier{true, 0.5f, 1.25f};
    settings = configure("17", nullptr, "dlss", &multiplier);
    check(settings.anisotropy == 16, "anisotropy clamps to its upper endpoint");
    check(settings.biasAutoRequested && settings.biasAuto && settings.autoSourceAvailable,
          "temporal AA defaults the bias to auto and records a successful source");
    check(std::fabs(settings.bias - -1.0f) < 0.00001f &&
              std::fabs(settings.autoMultiplier - 0.5f) < 0.00001f,
          "auto bias uses log2 of Elite's multiplier");
    check(multiplier.calls == 1 && std::strstr(settings.biasWhy, "fixture.2.4.fxcfg"),
          "auto alone requests the host multiplier and retains its diagnostic source");
    // The callback receives the host context pointer (whose first member is
    // the typed admission POD) unchanged through the policy boundary.
    check(settings.biasAutoRequested,
          "admitted auto control reaches the multiplier callback path");
    MultiplierFixture tinyMultiplier{true, 0.01f, 1.0f};
    settings = configure("0", "auto", "off", &tinyMultiplier);
    check(settings.biasAuto && settings.bias == -4.0f,
          "auto-derived bias is clamped at negative four");

    MultiplierFixture deniedExplicitMultiplier{true, 0.5f, 1.25f};
    settings = configure("0", "1.25", "dlss", &deniedExplicitMultiplier, false);
    check(std::fabs(settings.bias - 1.25f) < 0.00001f &&
              !settings.biasAutoRequested && settings.rewriteEnabled &&
              deniedExplicitMultiplier.calls == 0,
          "denied TAA owner preserves Performance's explicit bias without auto lookup");
}

void testConfigurationAndAuto() {
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    auto configuredOff = configure("0", "0", "off");
    check(!configuredOff.rewriteEnabled,
          "explicit zero settings leave the cached rewrite-interest flag clear");

    MultiplierFixture unavailable{};
    auto settings = configure("0", "auto", "off", &unavailable);
    check(settings.biasAutoRequested && !settings.autoSourceAvailable &&
              !settings.biasAuto && settings.bias == 0.0f,
          "failed auto source stays zero and is not reported as derived");
    check(unavailable.calls == 1, "failed auto still makes one host-service call");

    MultiplierFixture unused{};
    settings = configure("-7", "2.25tail", "off", &unused);
    check(settings.anisotropy == 0 && settings.bias == 2.25f &&
              !settings.biasAutoRequested && unused.calls == 0,
          "lower anisotropy clamp and atof-prefix bias behavior are retained");
    settings = configure("4", "99", "off", &unused);
    check(settings.anisotropy == 4 && settings.bias == 4.0f,
          "explicit bias clamps at positive four");
    settings = configure("4", "-99", "off", &unused);
    check(settings.bias == -4.0f, "explicit bias clamps at negative four");
    settings = configure("4", "nan", "off", &unused);
    check(settings.bias == -4.0f, "NaN follows the historical ordered-clamp result");
    settings = configure("4", "inf", "off", &unused);
    check(settings.bias == 4.0f, "positive infinity follows the historical clamp");
}

void testProfileDefaults() {
    edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    edvr::Config::get().set("advanced.texture_anisotropy", "9");
    edvr::Config::get().set("advanced.texture_lod_bias", "-1.5");
    edvr::Config::get().set("fix.temporal_aa", "off");
    auto settings = configure("9", "-1.5", "off");
    check(settings.anisotropy == 9 && settings.bias == -1.5f,
          "legacy VR profile keeps explicit sampler overrides");

    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    MultiplierFixture flatMultiplier{};
    settings = configure("9", "-1.5", "dlss", &flatMultiplier);
    check(settings.anisotropy == 0 && settings.bias == 0.0f &&
              flatMultiplier.calls == 0,
          "flat profile keeps historical profile-filtered sampler defaults");

    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    MultiplierFixture invalidMultiplier{};
    settings = configure("9", "-1.5", "dlss", &invalidMultiplier);
    check(settings.anisotropy == 0 && settings.bias == 0.0f &&
              invalidMultiplier.calls == 0,
          "invalid profile keeps historical profile-filtered sampler defaults");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
}

void testFilterClassesAndDescriptorCopy() {
    const auto* ops = textureSamplerPolicyOpsV1();
    auto settings = configure("8", "1.25", "off");
    const uint32_t accepted[] = {0x15u, 0x55u};
    for (uint32_t filter : accepted) {
        const auto input = descriptor(filter);
        TextureSamplerDescV1 output{};
        check(ops->rewrite(ops->context, &settings, &input, &output),
              "standard linear and anisotropic filter classes are accepted");
        const auto expectedFilter = 0x55u;
        check(output.filter == expectedFilter && output.maxAnisotropy == 8 &&
                  output.mipLodBias == input.mipLodBias + 1.25f,
              "accepted class gets anisotropy promotion and additive bias");
        check(output.addressU == input.addressU && output.addressV == input.addressV &&
                  output.addressW == input.addressW &&
                  output.comparisonFunc == input.comparisonFunc &&
                  output.minLod == input.minLod && output.maxLod == input.maxLod &&
                  std::memcmp(output.borderColor, input.borderColor,
                              sizeof(input.borderColor)) == 0,
              "addressing, comparison, LOD bounds and border color pass through");
    }

    // The D3D11 standard filter lattice: all nonaccepted point/mixed classes,
    // plus each comparison/minimum/maximum reduction class.
    const uint32_t standard[] = {0x00u, 0x01u, 0x04u, 0x05u, 0x10u, 0x11u, 0x14u,
                                 0x15u, 0x55u};
    for (uint32_t reduction : {0x00u, 0x80u, 0x100u, 0x180u}) {
        for (uint32_t base : standard) {
            const uint32_t filter = base | reduction;
            const bool shouldAccept = reduction == 0 && (base == 0x15u || base == 0x55u);
            const auto input = descriptor(filter);
            TextureSamplerDescV1 output{};
            const bool rewritten = ops->rewrite(ops->context, &settings, &input, &output);
            check(rewritten == shouldAccept,
                  "only exact standard linear and anisotropic filters are eligible");
            if (!shouldAccept)
                check(sameDescriptor(output, input),
                      "rejected point, mixed, comparison and reduction descriptors are exact copies");
        }
    }

    const auto noOverride = configure("0", "0", "off");
    const auto input = descriptor(0x15u);
    TextureSamplerDescV1 output{};
    check(!ops->rewrite(ops->context, &noOverride, &input, &output) &&
              sameDescriptor(output, input),
          "zero settings preserve the full descriptor without reporting a rewrite");
    check(!ops->rewrite(ops->context, nullptr, &input, &output) &&
              sameDescriptor(output, input),
          "absent settings preserve the full descriptor");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("texture sampler fixture dry-run: no config, policy, or callback calls");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::fprintf(stderr, "usage: texture_sampler_policy_test --dry-run|--self-test\n");
        return 2;
    }
    testUnsetDefaultsAndAuto();
    testConfigurationAndAuto();
    testProfileDefaults();
    testFilterClassesAndDescriptorCopy();
    std::printf("texture sampler production policy: %d checks passed\n", checks);
    return 0;
}
