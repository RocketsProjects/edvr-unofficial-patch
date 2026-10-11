#include <windows.h>
#include <d3d11.h>

#include "../../src/plugins/performance/texture_sampler_policy.h"

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace edvr {
class Log {
public:
    static Log& get() { static Log log; return log; }
    void note(const char*, ...) {}
};

namespace {
using plugins::performance::TextureSamplerDescV1;
using plugins::performance::TextureSamplerPolicyOpsV1;
using plugins::performance::TextureSamplerSettingsV1;

using CreateSamplerFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
    const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);

struct State {
    struct SamplerShape { uint32_t filter = 0, aniso = 0; float bias = 0; uint32_t count = 0; };
    ID3D11Device* device = nullptr;
    CreateSamplerFn realCreateSamplerState = nullptr;
    SamplerShape samplerShapes[12]{};
    uint32_t samplerShapeCount = 0, samplerCreates = 0, samplerOther = 0;
    uint64_t samplerFirstMs = 0;
    uint32_t samplerCensusPrints = 0, samplerPrintedShapes = 0;
    const TextureSamplerPolicyOpsV1* samplerPolicy = nullptr;
    TextureSamplerSettingsV1 samplerSettings{};
    bool samplerForceNoted = false;
    uint32_t censusCalls = 0;
};

State* g_state = nullptr;
int g_createBudget = 0;

template<class F>
bool guardedBudget(int&, F&& body) { std::forward<F>(body)(); return true; }

void samplerCensusNote(State& state) {
    ++state.censusCalls;
    state.samplerPrintedShapes = state.samplerShapeCount;
    ++state.samplerCensusPrints;
}

#include "../../src/d3d11/sampler_create_hook.inl"

int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct OriginalCall {
    unsigned calls = 0;
    const D3D11_SAMPLER_DESC* descAddress = nullptr;
    D3D11_SAMPLER_DESC desc{};
    ID3D11SamplerState** outAddress = nullptr;
    HRESULT result = E_INVALIDARG;
    ID3D11SamplerState* returned = reinterpret_cast<ID3D11SamplerState*>(uintptr_t(0x1234));
} g_original;

HRESULT STDMETHODCALLTYPE originalCreate(ID3D11Device*, const D3D11_SAMPLER_DESC* desc,
                                          ID3D11SamplerState** out) {
    ++g_original.calls;
    g_original.descAddress = desc;
    if (desc) g_original.desc = *desc;
    g_original.outAddress = out;
    if (out) *out = g_original.returned;
    return g_original.result;
}

struct CallbackCounts { unsigned rewrite = 0; } g_callbacks;

void configure(void*, void*, plugins::performance::ReadEliteHmdMultiplierV1,
               TextureSamplerSettingsV1*) {}
bool rewrite(void*, const TextureSamplerSettingsV1*, const TextureSamplerDescV1* input,
             TextureSamplerDescV1* output) {
    ++g_callbacks.rewrite;
    *output = *input;
    output->filter = 0x55u;
    output->maxAnisotropy = 8;
    output->mipLodBias = input->mipLodBias + 0.5f;
    return true;
}

TextureSamplerPolicyOpsV1 g_ops = {
    sizeof(TextureSamplerPolicyOpsV1),
    plugins::performance::kTextureSamplerPolicyOpsVersion1,
    nullptr, &configure, &rewrite};

D3D11_SAMPLER_DESC sampleDesc() {
    D3D11_SAMPLER_DESC d{};
    d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    d.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MipLODBias = 0.25f;
    d.MaxAnisotropy = 2;
    d.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    d.BorderColor[0] = 0.125f;
    d.BorderColor[1] = 0.25f;
    d.BorderColor[2] = 0.5f;
    d.BorderColor[3] = 1.0f;
    d.MinLOD = -2.0f;
    d.MaxLOD = 9.0f;
    return d;
}

bool sameDescriptor(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b) {
    if (a.Filter != b.Filter || a.AddressU != b.AddressU || a.AddressV != b.AddressV ||
        a.AddressW != b.AddressW || a.MipLODBias != b.MipLODBias ||
        a.MaxAnisotropy != b.MaxAnisotropy || a.ComparisonFunc != b.ComparisonFunc ||
        a.MinLOD != b.MinLOD || a.MaxLOD != b.MaxLOD) return false;
    for (size_t i = 0; i < 4; ++i)
        if (a.BorderColor[i] != b.BorderColor[i]) return false;
    return true;
}

void resetOriginal() { g_original = {}; }

void checkCase(bool condition, const char* scenario, const char* expectation) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s: %s\n", scenario, expectation);
        std::exit(1);
    }
}

void testIndependentNoRewriteGates() {
    TextureSamplerPolicyOpsV1 badSize = g_ops;
    badSize.structSize -= 1;
    TextureSamplerPolicyOpsV1 badVersion = g_ops;
    badVersion.version += 1;
    TextureSamplerPolicyOpsV1 noRewrite = g_ops;
    noRewrite.rewrite = nullptr;
    struct Scenario {
        const char* name;
        const TextureSamplerPolicyOpsV1* ops;
        uint32_t enabled;
    };
    const Scenario scenarios[] = {
        {"absent policy with stale enabled settings", nullptr, 1},
        {"selected policy with disabled settings", &g_ops, 0},
        {"bad Ops size with enabled settings", &badSize, 1},
        {"bad Ops version with enabled settings", &badVersion, 1},
        {"null rewrite callback with enabled settings", &noRewrite, 1},
    };
    for (const Scenario& scenario : scenarios) {
        State state{};
        state.device = reinterpret_cast<ID3D11Device*>(uintptr_t(0x20));
        state.realCreateSamplerState = &originalCreate;
        state.samplerPolicy = scenario.ops;
        state.samplerSettings.rewriteEnabled = scenario.enabled;
        state.samplerFirstMs = GetTickCount64() - 10001;
        g_state = &state;
        g_callbacks = {};
        resetOriginal();
        D3D11_SAMPLER_DESC desc = sampleDesc();
        ID3D11SamplerState* out = nullptr;

        const HRESULT got = hookedCreateSamplerState(state.device, &desc, &out);

        checkCase(g_callbacks.rewrite == 0, scenario.name,
                  "does not invoke the policy rewrite callback");
        checkCase(state.samplerCreates == 1 && state.samplerShapeCount == 1 &&
                      state.samplerShapes[0].count == 1, scenario.name,
                  "mandatory core census still counts the create and shape");
        checkCase(state.censusCalls == 1 && state.samplerCensusPrints == 1,
                  scenario.name, "settled core census report remains reachable");
        checkCase(g_original.calls == 1 && g_original.outAddress == &out,
                  scenario.name, "forwards once with the same output address");
        checkCase(g_original.descAddress != &desc && sameDescriptor(g_original.desc, desc),
                  scenario.name, "forwards an unchanged descriptor copy");
        checkCase(got == g_original.result && out == g_original.returned,
                  scenario.name, "preserves original HRESULT and output value");
    }
}

void testEnabledPolicyRewritesOnlyOwnedFieldsAndForwardsOnce() {
    State state{};
    state.device = reinterpret_cast<ID3D11Device*>(uintptr_t(0x20));
    state.realCreateSamplerState = &originalCreate;
    state.samplerPolicy = &g_ops;
    state.samplerSettings.structSize = sizeof(TextureSamplerSettingsV1);
    state.samplerSettings.version = plugins::performance::kTextureSamplerSettingsVersion1;
    state.samplerSettings.rewriteEnabled = 1;
    state.samplerSettings.anisotropy = 8;
    state.samplerSettings.bias = 0.5f;
    state.samplerFirstMs = GetTickCount64();
    g_state = &state;
    g_callbacks = {};
    resetOriginal();
    D3D11_SAMPLER_DESC desc = sampleDesc();
    ID3D11SamplerState* out = nullptr;

    const HRESULT got = hookedCreateSamplerState(state.device, &desc, &out);

    check(g_callbacks.rewrite == 1, "enabled policy invokes exactly one rewrite callback");
    check(g_original.calls == 1 && g_original.outAddress == &out,
          "enabled policy forwards exactly one original create and same output address");
    check(g_original.desc.Filter == D3D11_FILTER_ANISOTROPIC &&
          g_original.desc.MaxAnisotropy == 8 && g_original.desc.MipLODBias == 0.75f,
          "enabled policy changes only the three policy-owned D3D fields");
    D3D11_SAMPLER_DESC expected = desc;
    expected.Filter = D3D11_FILTER_ANISOTROPIC;
    expected.MaxAnisotropy = 8;
    expected.MipLODBias = 0.75f;
    check(sameDescriptor(g_original.desc, expected),
          "addressing, comparison, border and LOD descriptor fields pass through");
    check(got == g_original.result && out == g_original.returned,
          "enabled rewrite preserves original HRESULT and output value");
}

void testEarlyOriginalForwarding() {
    State state{};
    state.device = reinterpret_cast<ID3D11Device*>(uintptr_t(0x20));
    state.realCreateSamplerState = &originalCreate;
    state.samplerPolicy = &g_ops;
    state.samplerSettings.rewriteEnabled = 1;
    g_state = &state;
    D3D11_SAMPLER_DESC desc = sampleDesc();
    ID3D11SamplerState* out = nullptr;

    g_callbacks = {};
    resetOriginal();
    g_original.result = S_FALSE;
    const HRESULT nullResult = hookedCreateSamplerState(state.device, nullptr, &out);
    check(nullResult == S_FALSE && g_original.calls == 1 &&
          g_original.descAddress == nullptr && g_original.outAddress == &out &&
          out == g_original.returned && g_callbacks.rewrite == 0 &&
          state.samplerCreates == 0,
          "null descriptor forwards the original arguments/result without policy or census");

    g_callbacks = {};
    resetOriginal();
    g_original.result = S_FALSE;
    out = nullptr;
    ID3D11Device* foreign = reinterpret_cast<ID3D11Device*>(uintptr_t(0x40));
    const HRESULT foreignResult = hookedCreateSamplerState(foreign, &desc, &out);
    check(foreignResult == S_FALSE && g_original.calls == 1 &&
          g_original.descAddress == &desc && g_original.outAddress == &out &&
          out == g_original.returned && g_callbacks.rewrite == 0 &&
          state.samplerCreates == 0,
          "foreign device forwards original descriptor pointer/result without policy or census");

    g_state = nullptr;
    g_callbacks = {};
    resetOriginal();
    out = reinterpret_cast<ID3D11SamplerState*>(uintptr_t(0x5678));
    check(hookedCreateSamplerState(state.device, &desc, &out) == E_FAIL &&
          g_original.calls == 0 && g_callbacks.rewrite == 0 &&
          out == reinterpret_cast<ID3D11SamplerState*>(uintptr_t(0x5678)),
          "missing host state fails without original call or output write");

    state.realCreateSamplerState = nullptr;
    g_state = &state;
    resetOriginal();
    check(hookedCreateSamplerState(state.device, &desc, &out) == E_FAIL &&
          g_original.calls == 0 && g_callbacks.rewrite == 0 &&
          out == reinterpret_cast<ID3D11SamplerState*>(uintptr_t(0x5678)),
          "missing original function fails without output write");
}

int selfTest() {
    testIndependentNoRewriteGates();
    testEnabledPolicyRewritesOnlyOwnedFieldsAndForwardsOnce();
    testEarlyOriginalForwarding();
    std::printf("performance sampler hook fixture: %d checks passed\n", checks);
    return 0;
}
}  // namespace
}  // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) return 0;
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::fprintf(stderr, "usage: sampler_hook_test --dry-run|--self-test\n");
        return 2;
    }
    return edvr::selfTest();
}
