#include "target_sharp_policy.h"

#include <windows.h>
#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../common/config.h"
#include "../../common/log.h"
#include "../../common/plugin_cost.h"
#include "../../d3d11/binding_shadow.h"
#include "../../d3d11/cockpit_cost_sites.h"
#include "../../d3d11/exposure_fix.h"
#include "fsr_hlsl_gen.h"
#include "../../d3d11/shader_swap.h"
#include "../../d3d11/vscreen.h"

namespace edvr {
namespace {
constexpr char kKind = 'X';
constexpr uint32_t kIndices = 6;
constexpr uint32_t kInstances = 1;
// Shared by both resamplers: the bindings, a Catmull-Rom fetch, and the
// game's own colour maths.
//
// The colour maths is ps 63ABD86359B57D01 transcribed from its disassembly:
//
//   mul r0.xyz, r0.xyzx, r0.xyzx        <- the square
//   mul r0.xyz, r0.xyzx, cb2[0].xxxx
//   mul r0.xyz, r0.xyzx, cb1[84].zzzz
//   mul r0.xyz, r0.xyzx, cb1[61].yyyy
//   mul r1.xyz, r0.yyyy, cb1[86].xyzx
//   mad r0.xyw, r0.xxxx, cb1[85].xyxz, r1.xyxz
//   mad r0.xyz, r0.zzzz, cb1[87].xyzx, r0.xywx
//   mul o0.xyz, r0.xyzx, cb1[90].yyyy
//
// The two mads are one 3x3 matrix whose COLUMNS are cb1[85], cb1[86] and
// cb1[87] -- written out as three dot products rather than left as the
// register shuffle, because a shuffle that is subtly wrong looks like a
// colour shift and nothing else. The emitted tail was diffed against the
// game's before this ever shipped.
const char kPsCommon[] =
    "cbuffer CB1 : register(b1) { float4 g_cb1[91]; };\n"
    "cbuffer CB2 : register(b2) { float4 g_cb2[1]; };\n"
    "Texture2D<float4> g_tex : register(t0);\n"
    "SamplerState g_smp : register(s0);\n"
    "\n"
    "// Catmull-Rom, a = -0.5, stated so it can be checked against a\n"
    "// reference rather than recognised:\n"
    "//   w0 = -0.5t +       t^2 - 0.5t^3\n"
    "//   w1 =  1    - 2.5 * t^2 + 1.5t^3\n"
    "//   w2 =  0.5t + 2.0 * t^2 - 1.5t^3\n"
    "//   w3 =       - 0.5 * t^2 + 0.5t^3\n"
    "float4 fetchCubic(float2 uv)\n"
    "{\n"
    "    float2 size;\n"
    "    g_tex.GetDimensions(size.x, size.y);\n"
    "    int2 lim = int2(size) - 1;\n"
    "    float2 pos = uv * size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 t = pos - base;\n"
    "    float2 t2 = t * t;\n"
    "    float2 t3 = t2 * t;\n"
    "    float2 w0 = -0.5 * t + t2 - 0.5 * t3;\n"
    "    float2 w1 = 1.0 - 2.5 * t2 + 1.5 * t3;\n"
    "    float2 w2 = 0.5 * t + 2.0 * t2 - 1.5 * t3;\n"
    "    float2 w3 = -0.5 * t2 + 0.5 * t3;\n"
    "    float wx[4] = { w0.x, w1.x, w2.x, w3.x };\n"
    "    float wy[4] = { w0.y, w1.y, w2.y, w3.y };\n"
    "    float4 acc = 0.0;\n"
    "    [unroll] for (int j = 0; j < 4; ++j) {\n"
    "        [unroll] for (int i = 0; i < 4; ++i) {\n"
    "            int2 p = clamp(int2(base) + int2(i - 1, j - 1),\n"
    "                           int2(0, 0), lim);\n"
    "            acc += g_tex.Load(int3(p, 0)) * (wx[i] * wy[j]);\n"
    "        }\n"
    "    }\n"
    "    return acc;\n"
    "}\n"
    "\n"
    "float4 shade(float3 rgb, float a)\n"
    "{\n"
    "    float3 c = rgb * rgb;\n"
    "    c *= g_cb2[0].x;\n"
    "    c *= g_cb1[84].z;\n"
    "    c *= g_cb1[61].y;\n"
    "    float3 o;\n"
    "    o.x = c.x * g_cb1[85].x + c.y * g_cb1[86].x + c.z * g_cb1[87].x;\n"
    "    o.y = c.x * g_cb1[85].y + c.y * g_cb1[86].y + c.z * g_cb1[87].y;\n"
    "    o.z = c.x * g_cb1[85].z + c.y * g_cb1[86].z + c.z * g_cb1[87].z;\n"
    "    return float4(o * g_cb1[90].y, a);\n"
    "}\n"
    "}\n";

// The fallback, for a rig where EASU will not compile. A cubic is a smaller
// win than an edge-adaptive kernel on line art and a much larger one than
// the single bilinear tap the game does.
const char kPsCubicMain[] =
    "float4 main(float2 uv : TEXCOORD0) : SV_TARGET\n"
    "{\n"
    "    float4 s = fetchCubic(uv);\n"
    "    // A cubic kernel overshoots. rgb is SQUARED in shade(), so a\n"
    "    // negative channel would come back as a bright one; alpha gates a\n"
    "    // discard.\n"
    "    s.rgb = max(s.rgb, 0.0);\n"
    "    s.a = saturate(s.a);\n"
    "    if (s.a - 0.00001 < 0.0) discard;\n"
    "    return shade(s.rgb, s.a);\n"
    "}\n";

// EASU, and RCAS over it. Everything the filtering itself does is AMD's;
// what is here is the callbacks their header asks the calling shader to
// provide, plus the constants.
//
// WHY THE CONSTANTS ARE BUILT PER PIXEL. FsrEasuCon bakes an output-pixel to
// source-pixel map into con0, which assumes the two grids are related by a
// fixed scale -- true for a fullscreen upscale, false here: this quad is 3D
// cockpit geometry, so its UV is perspective-interpolated and not affine in
// screen space. Zeroing con0.xy drops the integer output position out of
// FsrEasuF's first line and con0.zw carries the source position directly,
// which is the same arithmetic it would have done. con1..con3 are
// FsrEasuCon's own terms with inputSize = the surface, term for term.
//
// RCAS reads a five-tap neighbourhood of the UPSCALED image, which does not
// exist as a texture here -- so FsrRcasLoadF evaluates EASU again at the
// neighbouring output pixel, reached by stepping the UV along its own screen
// derivatives. Five EASU evaluations for one output pixel, on one small quad
// per eye.
const char kPsEasuMain[] =
    "static float2 g_size, g_rcp, g_uv, g_dx, g_dy;\n"
    "\n"
    "AF4 FsrEasuRF(AF2 p) { return g_tex.GatherRed(g_smp, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return g_tex.GatherGreen(g_smp, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return g_tex.GatherBlue(g_smp, p); }\n"
    "\n"
    "float3 easuAt(float2 uv)\n"
    "{\n"
    "    float2 pp = uv * g_size - 0.5;\n"
    "    AU4 con0 = AU4(0, 0, asuint(pp.x), asuint(pp.y));\n"
    "    AU4 con1 = AU4(asuint(g_rcp.x), asuint(g_rcp.y),\n"
    "                   asuint(g_rcp.x), asuint(-g_rcp.y));\n"
    "    AU4 con2 = AU4(asuint(-g_rcp.x), asuint(2.0 * g_rcp.y),\n"
    "                   asuint(g_rcp.x), asuint(2.0 * g_rcp.y));\n"
    "    AU4 con3 = AU4(0, asuint(4.0 * g_rcp.y), 0, 0);\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, AU2(0, 0), con0, con1, con2, con3);\n"
    "    return c;\n"
    "}\n"
    "\n"
    "AF4 FsrRcasLoadF(ASU2 p)\n"
    "{\n"
    "    return AF4(easuAt(g_uv + p.x * g_dx + p.y * g_dy), 1.0);\n"
    "}\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_TARGET\n"
    "{\n"
    "    g_tex.GetDimensions(g_size.x, g_size.y);\n"
    "    g_rcp = 1.0 / g_size;\n"
    "    g_uv = uv;\n"
    "    // One output pixel's step in UV, taken BEFORE the discard so the\n"
    "    // derivative is read while the quad is still whole.\n"
    "    g_dx = ddx(uv);\n"
    "    g_dy = ddy(uv);\n"
    "\n"
    "    // Alpha is a coverage mask and EASU has no alpha path, so it keeps\n"
    "    // the cubic. It gates the same discard the game's shader gates.\n"
    "    float a = saturate(fetchCubic(uv).a);\n"
    "    if (a - 0.00001 < 0.0) discard;\n"
    "\n"
    "    AF3 c;\n"
    "#if EDVR_RCAS\n"
    "    FsrRcasF(c.r, c.g, c.b, AU2(0, 0),\n"
    "             AU4(asuint(float(EDVR_RCAS_SHARP)), 0, 0, 0));\n"
    "#else\n"
    "    c = easuAt(uv);\n"
    "#endif\n"
    "    return shade(max(c, 0.0), a);\n"
    "}\n";

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

std::string joinChunks(const char* const* chunks) {
    std::string out;
    for (const char* const* c = chunks; *c; ++c) out += *c;
    return out;
}

enum class Mode { kOff, kCubic, kEasu };

constexpr float g_sharpen = 0.25f;   // RCAS stops (AMD's unit: stops of sharpness reduction, 0 the sharpest)
struct TargetSharpState {
    bool sharp = false;
    bool failed = false;
    Mode running = Mode::kOff;
    ID3D11PixelShader* ps = nullptr;
    float psSharpen = 0.0f;
    bool psHadRcas = false;
    bool engaged = false;
    bool costSample = false;
    ID3D11PixelShader* displaced = nullptr;
    uint64_t applied = 0;
};

TargetSharpState g_state{};

ID3D11PixelShader* replacement(TargetSharpState& state,
                               ID3D11DeviceContext* ctx) {
    const bool wantRcas = g_sharpen >= 0.0f;
    if (state.ps && state.psHadRcas == wantRcas && state.psSharpen == g_sharpen) {
        return state.ps;
    }
    if (state.failed) return nullptr;
    if (state.ps) {
        state.ps->Release();
        state.ps = nullptr;
    }

    // AMD's own unit is STOPS of sharpness reduction, so 0 is the sharpest
    // and the linear value the shader wants is exp2(-stops).
    char sharpBuf[32] = "1.0";
    if (wantRcas) {
        _snprintf_s(sharpBuf, sizeof(sharpBuf), _TRUNCATE, "%.8f",
                    static_cast<double>(powf(2.0f, -g_sharpen)));
    }
    const SwapMacro macros[] = {{"EDVR_RCAS", wantRcas ? "1" : "0"},
                                {"EDVR_RCAS_SHARP", sharpBuf},
                                {nullptr, nullptr}};

    const std::string easu = std::string(kGpuPrologue) +
                             joinChunks(kFfxAChunks) +
                             "#define FSR_EASU_F 1\n" +
                             "#define FSR_RCAS_F 1\n" +
                             joinChunks(kFfxFsr1Chunks) + kPsCommon +
                             kPsEasuMain;
    state.ps = shaderSwapCompilePs(ctx, easu.c_str(), easu.size(), "main",
                               "target_indicator_easu", macros,
                               "target indicator");
    state.running = state.ps ? Mode::kEasu : Mode::kOff;
    if (!state.ps) {
        Log::get().note("target indicator: EASU would not compile; falling "
                        "back to the bicubic, a smaller win than FSR on line "
                        "art and a much larger one than the bilinear tap the "
                        "game does.");
        const std::string cubic = std::string(kPsCommon) + kPsCubicMain;
        state.ps = shaderSwapCompilePs(ctx, cubic.c_str(), cubic.size(), "main",
                                   "target_indicator_cubic", macros,
                                   "target indicator");
        state.running = state.ps ? Mode::kCubic : Mode::kOff;
    }
    if (!state.ps) {
        // shaderSwapCompilePs has already said why. One stand-down for the
        // session: a compile that failed once fails every draw, and a line
        // per draw would be the log.
        state.failed = true;
        Log::get().note("target indicator: no resampler would compile, so the "
                        "indicator is drawn exactly as the game draws it for "
                        "the rest of this session.");
        return nullptr;
    }
    state.psHadRcas = wantRcas;
    state.psSharpen = g_sharpen;
    Log::get().note(
        "target indicator: running %s%s.",
        state.running == Mode::kEasu ? "EASU (AMD's own, vendored)"
                                 : "the bicubic (Catmull-Rom)",
        (state.running == Mode::kEasu && wantRcas) ? ", then RCAS-sharpened" : "");
    return state.ps;
}


template <bool Observe>
bool targetSharpOnEyeDrawImpl(TargetSharpState& state,
                              ID3D11DeviceContext* ctx, char kind,
                              uint32_t count, uint32_t instances,
                              TargetSharpObservation* observation) {
    const bool sharp = state.sharp;
    if constexpr (Observe)
        observation->helperSharp = {true, true, sharp};
    if (!sharp) return false;
    const bool failed = state.failed;
    if constexpr (Observe)
        observation->helperFailed = {true, true, failed};
    if (failed) return false;
    if (kind != kKind || count != kIndices || instances != kInstances) {
        return false;
    }
    // Slot 0 is the interface surface: a Texture2D that is NOT eye-sized.
    // Eye-sized by vScreen's answer rather than an equality, the lesson
    // holo_fix paid for on a rig with a render scale.
    ResourceInfo surf;
    void* const srv0 = bindingGet(BindSlot::PsSrv0);
    if constexpr (Observe)
        observation->srv0Present = {true, true, srv0 != nullptr};
    const bool resolved = bindingResolve(srv0, &surf);
    if constexpr (Observe)
        observation->srv0Resolved = {true, true, resolved};
    if (!resolved) return false;
    if constexpr (Observe)
        observation->srv0Texture2D = {true, true, surf.isTexture2D};
    if (!surf.isTexture2D) return false;
    if constexpr (Observe) {
        observation->srv0Width = {true, true, surf.a};
        observation->srv0Height = {true, true, surf.b};
    }
    bool eyeSized = false;
    if constexpr (Observe) {
        eyeSized = vScreenIsEyeSizedObserved(surf.a, surf.b,
                                             &observation->eyeSize);
    } else {
        eyeSized = vScreenIsEyeSized(surf.a, surf.b);
    }
    if (eyeSized) {
        return false;
    }
    // Slots 1-3 unbound. The composite reads one texture and nothing else,
    // which is most of what separates it from every other textured quad.
    void* const srv1 = bindingGet(BindSlot::PsSrv1);
    if constexpr (Observe)
        observation->aux1Present = {true, true, srv1 != nullptr};
    if (srv1) return false;
    void* const srv2 = bindingGet(BindSlot::PsSrv2);
    if constexpr (Observe)
        observation->aux2Present = {true, true, srv2 != nullptr};
    if (srv2) return false;
    void* const srv3 = bindingGet(BindSlot::PsSrv3);
    if constexpr (Observe)
        observation->aux3Present = {true, true, srv3 != nullptr};
    if (srv3) {
        return false;
    }
    // The clincher, and last because it costs a call: nothing hooks
    // VSSetShader, so the shader is read off the context the way the census
    // reads it.
    ID3D11VertexShader* vs = nullptr;
    if (edvrPluginCostApiSampleContext(ctx)) {
        edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::CockpitVisuals),
                                  cockpit_cost::id(cockpit_cost::Site::TargetVsGetShader),
                                  static_cast<uint8_t>(plugin_cost::ApiClass::ReadQuery));
    }
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if constexpr (Observe)
        observation->vsPresent = {true, true, vs != nullptr};
    if (!vs) return false;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    const uint64_t configuredHash = kTargetSharpVertexShaderHashV1;
    if constexpr (Observe) {
        observation->queriedShaderHash = {true, true, h};
        observation->configuredShaderHash = {true, true, configuredHash};
    }
    return h == configuredHash;
}


void publish(const TargetSharpState& state, TargetSharpFastStateV1* out) {
    if (!out) return;
    *out = {sizeof(TargetSharpFastStateV1), kTargetSharpPolicyVersionV1,
            state.sharp ? 1u : 0u, state.failed ? 1u : 0u,
            state.engaged ? 1u : 0u, 0u,
            kTargetSharpVertexShaderHashV1};
}

void builtinConfigure(void* opaque, Config* cfg,
                      TargetSharpFastStateV1* out) {
    auto& state = *static_cast<TargetSharpState*>(opaque);
    if (!cfg) { publish(state, out); return; }
    const bool was = state.sharp;
    const std::string mode = cfg->getString("experimental.target_indicator",
                                            "stock");
    if (mode == "stock") {
        state.sharp = false;
    } else if (mode == "sharp") {
        state.sharp = true;
    } else {
        state.sharp = false;
        Log::get().note("target_indicator \"%s\" is not stock or sharp; "
                        "running stock.", mode.c_str());
    }
    if (was != state.sharp) {
        Log::get().note(
            "target indicator: %s. The selected target's direction indicator "
            "is composited into the eye by one quad through a single bilinear "
            "sample; sharp replaces that pixel shader with the same colour "
            "maths over an edge-adaptive reconstruction (AMD's EASU), "
            "sharpening %s. It cannot add detail. Watching for vs %016llX.",
            state.sharp ? "sharp" : "stock",
            g_sharpen >= 0.0f ? "on" : "off",
            static_cast<unsigned long long>(kTargetSharpVertexShaderHashV1));
    }
    publish(state, out);
}

bool builtinMatches(void* opaque, ID3D11DeviceContext* ctx, char kind,
                    uint32_t count, uint32_t instances,
                    TargetSharpObservation* observation) {
    auto& state = *static_cast<TargetSharpState*>(opaque);
    if (observation)
        return targetSharpOnEyeDrawImpl<true>(state, ctx, kind, count,
                                               instances, observation);
    return targetSharpOnEyeDrawImpl<false>(state, ctx, kind, count,
                                            instances, nullptr);
}

void builtinBegin(void* opaque, ID3D11DeviceContext* ctx,
                  TargetSharpFastStateV1* out) {
    auto& state = *static_cast<TargetSharpState*>(opaque);
    state.costSample = false;
    state.engaged = false;
    ID3D11PixelShader* ps = replacement(state, ctx);
    if (!ps) { publish(state, out); return; }
    state.costSample = edvrPluginCostApiSampleContext(ctx) != 0;
    if (state.costSample) {
        edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::CockpitVisuals),
                                  cockpit_cost::id(cockpit_cost::Site::TargetPsGetShader),
                                  static_cast<uint8_t>(plugin_cost::ApiClass::ReadQuery));
    }
    ctx->PSGetShader(&state.displaced, nullptr, nullptr);
    if (state.costSample) {
        edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::CockpitVisuals),
                                  cockpit_cost::id(cockpit_cost::Site::TargetPsSetShaderApply),
                                  static_cast<uint8_t>(plugin_cost::ApiClass::State));
    }
    ctx->PSSetShader(ps, nullptr, 0);
    state.engaged = true;
    if (++state.applied == 1) {
        Log::get().note("target indicator: sharp engaged -- the composite is "
                        "reconstructed for exactly this draw, and the game's "
                        "own shader is restored after every one.");
    }
    publish(state, out);
}

void builtinEnd(void* opaque, ID3D11DeviceContext* ctx,
                TargetSharpFastStateV1* out) {
    auto& state = *static_cast<TargetSharpState*>(opaque);
    if (!state.engaged) {
        state.costSample = false;
        if (state.displaced) { state.displaced->Release(); state.displaced = nullptr; }
        publish(state, out);
        return;
    }
    const bool costSample = state.costSample;
    ID3D11PixelShader* orig = state.displaced;
    if (costSample) {
        edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::CockpitVisuals),
                                  cockpit_cost::id(cockpit_cost::Site::TargetPsSetShaderRestore),
                                  static_cast<uint8_t>(plugin_cost::ApiClass::State));
    }
    ctx->PSSetShader(orig, nullptr, 0);
    state.engaged = false;
    state.costSample = false;
    state.displaced = nullptr;
    if (orig) orig->Release();
    publish(state, out);
}

void builtinShutdown(void* opaque, TargetSharpFastStateV1* out) {
    auto& state = *static_cast<TargetSharpState*>(opaque);
    state.costSample = false;
    if (state.ps) { state.ps->Release(); state.ps = nullptr; }
    if (state.displaced) { state.displaced->Release(); state.displaced = nullptr; }
    publish(state, out);
}

const TargetSharpPolicyOpsV1 kBuiltinOps = {
    sizeof(TargetSharpPolicyOpsV1), kTargetSharpPolicyVersionV1, &g_state,
    &builtinConfigure, &builtinMatches, &builtinBegin, &builtinEnd,
    &builtinShutdown};

}  // namespace

const TargetSharpPolicyOpsV1* targetSharpPolicyOpsV1() noexcept {
    return &kBuiltinOps;
}

#if defined(EDVR_VSCREEN_PREDICATE_TEST)
void targetSharpPolicyPredicateSeedForTest(bool sharp, bool failed) noexcept {
    g_state.sharp = sharp;
    g_state.failed = failed;
}
#endif

}  // namespace edvr
