#include "texture_sampler_policy.h"

#include "../../common/config.h"
#include "../../common/temporal_mode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace edvr::plugins::performance {
namespace {

constexpr uint32_t kFilterLinear = 0x15u;       // D3D11_FILTER_MIN_MAG_MIP_LINEAR
constexpr uint32_t kFilterAnisotropic = 0x55u;  // D3D11_FILTER_ANISOTROPIC

bool samplerFilterAccepted(uint32_t filter) {
    // Exact equality excludes point, mixed point/mip, and all comparison,
    // minimum and maximum reduction variants.
    return filter == kFilterLinear || filter == kFilterAnisotropic;
}

}  // namespace

void resolveTextureSamplerSettingsV1(TextureSamplerSettingsV1* settings,
                                     void* hostContext,
                                     ReadEliteHmdMultiplierV1 readMultiplier) {
    if (!settings) return;
    const int anisotropy = Config::get().getIntInRange(
        "advanced.texture_anisotropy", 0, 0, 16);
    const bool temporal = temporalModeEnabled(
        Config::get().getString("fix.temporal_aa", "off"));
    const std::string biasKey = Config::get().getString(
        "advanced.texture_lod_bias", temporal ? "auto" : "0");
    resolveTextureSamplerSettingsFromValuesV1(settings, anisotropy, temporal,
                                              biasKey.c_str(), hostContext,
                                              readMultiplier);
}

void resolveTextureSamplerSettingsFromValuesV1(
    TextureSamplerSettingsV1* settings, int32_t anisotropyValue,
    bool temporalAAEnabled, const char* biasKeyValue, void* hostContext,
    ReadEliteHmdMultiplierV1 readMultiplier) {
    if (!settings) return;
    *settings = {};
    settings->structSize = sizeof(*settings);
    settings->version = kTextureSamplerSettingsVersion1;

    const int anisotropy = anisotropyValue < 0 ? 0 :
                           anisotropyValue > 16 ? 16 : anisotropyValue;
    const char* const biasKey = biasKeyValue ? biasKeyValue :
                                (temporalAAEnabled ? "auto" : "0");
    float bias = 0.0f;
    if (_stricmp(biasKey, "auto") == 0) {
        settings->biasAutoRequested = 1;
        float multiplier = 0.0f;
        float ssaa = 0.0f;
        char file[80] = {};
        if (readMultiplier && readMultiplier(hostContext, &multiplier, &ssaa,
                                             file, sizeof(file))) {
            bias = log2f(multiplier);
            settings->biasAuto = 1;
            settings->autoSourceAvailable = 1;
            settings->autoMultiplier = multiplier;
            std::snprintf(settings->biasWhy, sizeof(settings->biasWhy),
                          "auto: Elite renders at %.3f of the size the runtime recommends "
                          "(HMDRenderTargetMultiplier in %s; its SSAAMultiplier is %.3f), and "
                          "log2 of that is the bias the mips want",
                          static_cast<double>(multiplier), file[0] ? file : "its graphics preset",
                          static_cast<double>(ssaa));
        }
    } else {
        bias = static_cast<float>(atof(biasKey));
        std::snprintf(settings->biasWhy, sizeof(settings->biasWhy),
                      "advanced.texture_lod_bias = %s", biasKey);
    }
    if (!(bias > -4.0f)) bias = -4.0f;
    if (!(bias < 4.0f)) bias = 4.0f;
    settings->anisotropy = anisotropy;
    settings->bias = bias;
    settings->rewriteEnabled = anisotropy > 0 || bias != 0.0f;
}

bool rewriteTextureSamplerV1(const TextureSamplerSettingsV1* settings,
                             const TextureSamplerDescV1* input,
                             TextureSamplerDescV1* output) {
    if (!input || !output) return false;
    *output = *input;
    if (!settings || settings->structSize != sizeof(*settings) ||
        settings->version != kTextureSamplerSettingsVersion1 ||
        (settings->anisotropy <= 0 && settings->bias == 0.0f) ||
        !samplerFilterAccepted(input->filter)) return false;

    if (settings->anisotropy > 0) {
        output->filter = kFilterAnisotropic;
        output->maxAnisotropy = static_cast<uint32_t>(settings->anisotropy);
    }
    output->mipLodBias = input->mipLodBias + settings->bias;
    return true;
}

namespace {
void configureOps(void*, void* hostContext, ReadEliteHmdMultiplierV1 readMultiplier,
                  TextureSamplerSettingsV1* settings) {
    resolveTextureSamplerSettingsV1(settings, hostContext, readMultiplier);
}

bool rewriteOps(void*, const TextureSamplerSettingsV1* settings,
                const TextureSamplerDescV1* input, TextureSamplerDescV1* output) {
    return rewriteTextureSamplerV1(settings, input, output);
}
}  // namespace

const TextureSamplerPolicyOpsV1* textureSamplerPolicyOpsV1() {
    static const TextureSamplerPolicyOpsV1 ops = {
        sizeof(TextureSamplerPolicyOpsV1), kTextureSamplerPolicyOpsVersion1,
        nullptr, &configureOps, &rewriteOps};
    return &ops;
}

}  // namespace edvr::plugins::performance
