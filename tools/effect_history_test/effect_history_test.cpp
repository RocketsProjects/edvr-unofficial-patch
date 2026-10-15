// The effect-history rule (src\d3d11\effect_history.cpp, effect_history_core.h, effect_history_pairs.h; docs/dlss-blur-elements-2026-10-14.md).
//
// PURE HALF (no device): the list (membership by PAIR, the deferred light volumes and the force field NOT on it, names unique), the threshold decision on
// its boundaries, the census line (worst case, both figures), and the wiring of the motion shader's text (the registers, the bit, the one HLSL
// definition spliced from the header).
//
// DEVICE HALF (a WARP device, the production module linked in as a source): the HLSL decision against the C++ one on a table of cases, then the replica:
//   E1  every piece of state the replica touches is put back (render targets, depth view, depth-stencil state and reference, blend state, rasterizer, scissor,
//       viewport, shaders, topology) -- and nothing else moved
//   E2  the replica writes nothing the game would see: the game's HDR target and its depth buffer are byte for byte what they were
//   E3  the private target holds the effect's light -- zero outside the quad, zero behind the game's depth, the effect's colour in front of it, summed over
//       two draws of one eye-frame, and cleared again for the next
//   E4  the data path to the motion shader: the two views come once per eye-frame, are consumed by the asking, are refused at a size that is not the eye's, and
//       the production HLSL decision over them hides the strong glow, spares the faint one over a bright scene, and honours the floor
//   E4b two eyes: each scene depth has its own private target and is handed over by its own depth texture (the eye's identity)
//   E5  what is left alone: an unlisted pair, a UAV on the draw, a missing depth view -- no replay, nothing touched
//   E6  the census: draws per frame, covered pixels per draw (exact on a pixel-aligned quad), the GPU timing, the hidden-pixel counter's growth over the
//       eye-frames it was counted for, and the lapse of the NVIDIA flag
//   E7  the D3D11 debug layer, when installed, reports no error for any of it
// --self-test runs both halves; --dry-run runs neither and writes nothing.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/effect_history.h"
#include "../../src/d3d11/effect_history_core.h"
#include "../../src/d3d11/effect_history_pairs.h"
#include "../../src/d3d11/stale_depth_core.h"
#include "../../src/d3d11/temporal_shader_source.h"

using Microsoft::WRL::ComPtr;

// ---- what the DLL provides around the module: the log ---------------------------------------------------------------------------
namespace edvr {
std::vector<std::string> g_lines;
bool g_verbose = false;
Log& Log::get() {
    static Log instance;
    return instance;
}
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lines.emplace_back(buf);
    if (g_verbose) std::printf("    log: %s\n", buf);
}
}  // namespace edvr

namespace {
unsigned g_checks = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) throw std::runtime_error(label);
}
void checkf(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (ok) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    throw std::runtime_error(buf);
}
bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

using namespace edvr;

// =============================================================================================================================
// PURE HALF
// =============================================================================================================================

void testList() {
    // The list is the particle billboard alone (the halo billboard was removed on build D, see effect_history_pairs.h); both hashes must match.
    check(kEffectHistoryPairCount == 1, "the list is one pair");
    check(effectPairIndex(0x68DDDEF04D9894AFull, 0x06332CA168B6DA63ull) == 0, "the particle billboard is pair 0");
    check(effectPairIndex(0xB75A6FF2CA9FA5D6ull, 0xD56F859BE4781431ull) == -1 && !effectVsListed(0xB75A6FF2CA9FA5D6ull), "the halo/anchor billboard B75A6FF2/D56F859B is NOT listed (it covers the whole sky: ruled out as an effect-only pair)");
    check(effectPairIndex(0x68DDDEF04D9894AFull, 0xD56F859BE4781431ull) == -1, "a listed vertex shader with another pair's pixel shader is not a pair");
    check(effectPairIndex(0xB75A6FF2CA9FA5D6ull, 0x06332CA168B6DA63ull) == -1, "...either way round");
    check(effectPairIndex(0x68DDDEF04D9894AFull, 0) == -1 && effectPairIndex(0, 0) == -1, "zero hashes are not on the list");
    check(effectVsListed(0x68DDDEF04D9894AFull), "the draw path's first test knows the vertex shader");
    check(!effectVsListed(0) && !effectVsListed(0x06332CA168B6DA63ull), "...and not a pixel shader's hash");
    // NOT on the list, on purpose: the deferred light volumes and the force field's G-buffer quad.
    const struct {
        uint64_t vs, ps;
        const char* what;
    } excluded[] = {
        {0x0357BBB2DEE43C1Full, 0x81812EF97FB4A361ull, "light volume 0357BBB2"},
        {0x8289669D93A18C1Dull, 0xC6E6E419DA9F6FADull, "light volume 8289669D"},
        {0xA1B7CFCD0BE7493Eull, 0x2DB678B6B558B604ull, "light volume A1B7CFCD"},
        {0x124D7F3F649138D4ull, 0x8085AE8DD1906CDCull, "the force field's G-buffer quad"},
    };
    for (const auto& e : excluded) {
        checkf(effectPairIndex(e.vs, e.ps) == -1, "%s is on the list", e.what);
        checkf(!effectVsListed(e.vs), "%s's vertex shader is listed", e.what);
    }
    for (uint32_t i = 0; i < kEffectHistoryPairCount; ++i) {
        check(kEffectHistoryPairs[i].vs != 0 && kEffectHistoryPairs[i].ps != 0, "a listed pair has real hashes");
        check(kEffectHistoryPairs[i].name && kEffectHistoryPairs[i].name[0], "a listed pair has a name");
        for (uint32_t j = i + 1; j < kEffectHistoryPairCount; ++j) {
            check(std::strcmp(kEffectHistoryPairs[i].name, kEffectHistoryPairs[j].name) != 0, "names are unique");
            check(kEffectHistoryPairs[i].vs != kEffectHistoryPairs[j].vs || kEffectHistoryPairs[i].ps != kEffectHistoryPairs[j].ps, "pairs are unique");
        }
    }
    check(kEffectHistoryPairCount <= kEffectPairSlots, "the census has a slot for every pair");
}

void testDecision() {
    check(kEffectHideShare == 0.35f && kEffectHideFloor == 0.002f, "the named constants are the ones the doc and the census say");
    // The luma weights.
    check(std::fabs(lumaOf(1, 1, 1) - 1.0f) < 1e-6f, "white is luma 1");
    check(std::fabs(lumaOf(0, 1, 0) - 0.7152f) < 1e-6f && std::fabs(lumaOf(1, 0, 0) - 0.2126f) < 1e-6f && std::fabs(lumaOf(0, 0, 1) - 0.0722f) < 1e-6f, "Rec.709 weights");
    check(lumaOf(-5, -5, -5) == 0.0f, "negative light is no light");
    // The boundary: the effect supplies at least the share of the pixel's whole light.
    check(effectHides(0.35f, 1.0f), "exactly the share hides");
    check(!effectHides(0.3499f, 1.0f), "just under the share does not");
    check(effectHides(0.36f, 1.0f), "over the share hides");
    check(effectHides(0.5f, 0.6f), "a strong glow over a dim scene (E 0.5 of F 0.6) hides");
    check(!effectHides(0.02f, 3.0f), "a faint glow over a bright scene (E 0.02 of F 3) does not");
    // The floor: a pixel with next to no glow is never hidden, whatever the ratio.
    check(!effectHides(0.001f, 0.001f), "below the floor nothing hides, though the share is 1");
    check(!effectHides(0.002f, 0.002f), "AT the floor nothing hides (strictly above)");
    check(effectHides(0.003f, 0.003f), "just above the floor, on a black scene, hides");
    check(effectHides(0.003f, 0.0f), "a glow on nothing hides");
    check(!effectHides(0.0f, 0.0f), "nothing on nothing does not");
    // Custom share and floor are honoured (the shader gets them as constants).
    check(effectHides(0.5f, 1.0f, 0.5f, 0.0f) && !effectHides(0.49f, 1.0f, 0.5f, 0.0f), "the share is a parameter");
    check(!effectHides(0.4f, 0.4f, 0.35f, 0.5f), "the floor is a parameter");
    // The HLSL splice is the one text.
    check(std::strstr(kEffectHideHlsl, "bool effectHideTest(float3 e, float3 f, float share, float floorValue)") != nullptr, "the HLSL defines effectHideTest");
    check(std::strstr(kEffectHideHlsl, "0.2126, 0.7152, 0.0722") != nullptr, "...with the same luma weights");
}

void testCensus() {
    EffectWindow w;
    w.frames = 2700;
    w.eyeFrames = 5380;
    w.draws[0] = 4000;
    w.samples[0] = 4000ull * 12345;
    w.sampled[0] = 4000;
    w.substituted[0] = 5400;
    w.gpuMs = 31.5;
    w.gpuSamples = 25000;
    w.gpuSkipped = 3;
    w.gpuInvalid = 1;
    w.refused = 12;
    w.queryGaps = 7;
    w.scratchMiB = 31.0;
    w.hiddenPixels = 123456789ull;
    w.hiddenEyeFrames = 5000;
    w.counterReads = 80;
    char line[1400];
    const int n = formatEffectCensus(line, sizeof(line), w, 30);
    check(n > 0 && n < 1200, "the census line fits the log's 1200 bytes");
    const std::string s(line);
    check(s.rfind("effect history: last 30 s, 2700 frames, 5380 eye-frames with a listed draw;", 0) == 0, "the census line's signature");
    check(has(s, "particle-billboard: 1.48 draws/frame, 12345 covered pixels per draw (4000 sampled), 2.00/frame left alone (a fix substitutes them);"), "per-pair draws per frame, coverage, and the draws a fix substituted");
    check(has(s, "pixels hidden: 24691 per eye-frame with a listed draw, 45725 per frame (both eyes; 5000 eye-frames counted);"), "the hidden-pixel figures");
    check(has(s, "replica GPU 0.001 ms per replicated draw (25000 timed, 3 skipped, 1 invalid)"), "the GPU cost");
    check(has(s, "12 refused, 7 unanswered queries; private targets 31.0 MiB; the game's HDR target was readable."), "refusals, gaps, memory, readability");
    EffectWindow none;
    none.targetUnreadable = 3;
    none.targetReadable = false;
    const int m = formatEffectCensus(line, sizeof(line), none, 30);
    check(m > 0 && has(line, "pixels hidden: not measured this window (0 reads of the GPU counter, 0 eye-frames);") && has(line, "COULD NOT BE READ"),
          "an unmeasured window says so, and an unreadable HDR target is said");
    check(formatEffectCensus(line, 100, w, 30) < 0, "a short buffer is refused, not truncated silently");
}

void testShaderText() {
    const std::string src(kTemporalCsHlsl);
    check(has(src, "Texture2D<float4> EFX : register(t5);") && has(src, "Texture2D<float4> EFH : register(t16);"), "the shader declares the two inputs at t5 and t9");
    check(src.find(kEffectHideHlsl) != std::string::npos, "the shader carries the header's definition, text for text (it is a literal copy: the tools that read the shader's raw literals cannot follow a macro)");
    check(has(src, "(uint(probe.w + 0.5) & 32768u) != 0u"), "the rule is gated on probe.w bit 32768");
    check(has(src, "effectHideTest(EFX.Load(effectAt).rgb, EFH.Load(effectAt).rgb, split.y, split.z)"), "the decision reads both inputs at the pixel with the cb's share and floor");
    check(has(src, "(effectHidden?32768u:0u)"), "the trace variant flags the pixel (bit 15)");
    check(has(src, "InterlockedAdd(Stats[40], gEffectHidden)"), "the running total is flushed to Stats[40]");
    check(kEffectProbeBit == 32768u && kEffectDecisionBit == 32768u, "the header's bits are the shader's");
    // t5 and t16 are not read anywhere else.
    size_t at = 0, t5 = 0, t16 = 0;
    while ((at = src.find("register(t5)", at)) != std::string::npos) { ++t5; ++at; }
    at = 0;
    while ((at = src.find("register(t16)", at)) != std::string::npos) { ++t16; ++at; }
    check(t5 == 1 && t16 == 1, "t5 and t16 are declared once");
}

void testKind4() {
    // The rule on the four previous-depth samples: raw depth, A + B / z, here A = 0 and an expectation of 0.001 (tolerance 1% = 1e-5).
    const float same[4] = {0.001f, 0.001f, 0.001f, 0.001f};
    check(staleDepthConfirms(same, 0.001f, 0.0f), "kind 4: a previous depth equal to the prediction confirms the camera term");
    const float near9[4] = {0.001009f, 0.001009f, 0.001009f, 0.001009f}, far9[4] = {0.000991f, 0.000991f, 0.000991f, 0.000991f};
    check(staleDepthConfirms(near9, 0.001f, 0.0f) && staleDepthConfirms(far9, 0.001f, 0.0f), "...within 1% either way (nearer and farther)");
    const float near2[4] = {0.00102f, 0.00102f, 0.00102f, 0.00102f}, far2[4] = {0.00098f, 0.00098f, 0.00098f, 0.00098f};
    check(!staleDepthConfirms(near2, 0.001f, 0.0f) && !staleDepthConfirms(far2, 0.001f, 0.0f), "kind 4: 2% off, nearer or farther, does not: the history is some other surface's");
    const float one[4] = {0.0002f, 0.0002f, 0.001f, 0.0002f};
    check(staleDepthConfirms(one, 0.001f, 0.0f), "one of the four samples confirming is enough (the 2x2 around the previous raster position)");
    const float sky[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    check(!staleDepthConfirms(sky, 0.001f, 0.0f), "a rail over bare sky last frame (previous depth 0): not confirmed, so history is refused");
    const float tiny[4] = {1.05e-5f, 1.05e-5f, 1.05e-5f, 1.05e-5f};
    check(staleDepthConfirms(tiny, 1.0e-5f, 0.0f), "the absolute floor (1e-6) keeps a very far surface's rounding from refusing it");
    const float off[4] = {0.500502f, 0.500502f, 0.500502f, 0.500502f}, off2[4] = {0.50051f, 0.50051f, 0.50051f, 0.50051f};
    check(staleDepthConfirms(off, 0.5005f, 0.5f) && !staleDepthConfirms(off2, 0.5005f, 0.5f), "the tolerance is 1% of the encoded depth B / z, not of A + B / z");
    check(kStaleDepthRel == 0.01f && kStaleDepthFloor == 1e-6f && kStaleDepthDecisionBit == 65536u, "the constants are flat's (1%, 1e-6) and the decision bit is the shader's");
    // The census line.
    StaleDepthWindow w;
    w.frames = 2700;
    w.hiddenPixels = 123456;
    w.passEyeFrames = 5400;
    w.counterReads = 80;
    char line[800];
    int n = formatStaleDepthCensus(line, sizeof(line), w, 30);
    check(n > 0 && n < 1100 && has(line, "kind-4 depth check: last 30 s, 2700 frames: 22.9 pixels hidden per eye-frame (5400 eye-frames counted)") && has(line, "within 1%"), "the kind-4 census line");
    n = formatStaleDepthCensus(line, sizeof(line), StaleDepthWindow(), 30);
    check(n > 0 && has(line, "not measured this window"), "an unmeasured window says so");
    // The shader's wiring, as text.
    const std::string src(kTemporalCsHlsl);
    check(src.find(kStaleDepthHlsl) != std::string::npos, "the shader carries stale_depth_core.h's HLSL text for text (a literal copy: the rig fails if it drifts)");
    check(has(src, "bool kind4Hidden = engineKind == 4u && !trackedForeground && !uiHere && staleMarkerDepthHidden(p, motion, zPred);"), "the rule applies to engine kind 4 only (kinds 1, 2, 3, 5, 6 untouched), off the foreground and UI paths");
    check(has(src, "bool hidden = engineMasked || kind4Hidden || (!trackedForeground"), "a kind-4 pixel the check refuses is hidden like a masked one");
    check(has(src, "(kind4Hidden?65536u:0u)") && has(src, "InterlockedAdd(Stats[41], gKind4Hidden)"), "the trace variant flags it and the census total is flushed to Stats[41]");
}

void runPure() {
    testKind4();
    testList();
    testDecision();
    testCensus();
    testShaderText();
}

// =============================================================================================================================
// DEVICE HALF
// =============================================================================================================================

constexpr UINT kSize = 64;

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    bool debugLayer = false;
};
void hr(HRESULT h) { checkf(SUCCEEDED(h), "D3D operation failed, hr=0x%08lX", static_cast<unsigned long>(h)); }

Gpu makeGpu() {
    PFN_D3D11_CREATE_DEVICE create = edvr::systemD3D11CreateDevice();
    check(create != nullptr, "System32 D3D11CreateDevice is available");
    Gpu g;
    D3D_FEATURE_LEVEL level{};
    HRESULT made = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, g.dev.ReleaseAndGetAddressOf(), &level,
                          g.ctx.ReleaseAndGetAddressOf());
    g.debugLayer = SUCCEEDED(made);
    if (!g.debugLayer)
        made = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, g.dev.ReleaseAndGetAddressOf(), &level, g.ctx.ReleaseAndGetAddressOf());
    hr(made);
    if (g.debugLayer) g.dev.As(&g.info);
    return g;
}

ComPtr<ID3DBlob> compile(const std::string& src, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    const HRESULT h = D3DCompile(src.c_str(), src.size(), nullptr, nullptr, nullptr, "main", target, 0, 0, blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(h)) throw std::runtime_error(std::string("shader: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "no message"));
    return blob;
}

const char* kVs = R"HLSL(
cbuffer C : register(b0) { float4 rect; float4 misc; float4 colour; };
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 c[4] = { float2(rect.x, rect.y), float2(rect.z, rect.y), float2(rect.x, rect.w), float2(rect.z, rect.w) };
    return float4(c[id], misc.x, 1);
}
)HLSL";
const char* kPs = R"HLSL(
cbuffer C : register(b0) { float4 rect; float4 misc; float4 colour; };
float4 main(float4 p : SV_Position) : SV_Target { return colour; }
)HLSL";

// The motion shader's decision, as a kernel over the two views the module hands the pass (the same text the shader splices in).
std::string decisionKernelSource() {
    return std::string(kEffectHideHlsl) + R"HLSL(
Texture2D<float4> EFX : register(t5);
Texture2D<float4> EFH : register(t16);
RWStructuredBuffer<float4> Out : register(u0);
cbuffer C : register(b0) { float share; float floorValue; uint w; uint h; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    float4 e = EFX.Load(int3(id.xy, 0));
    float4 f = EFH.Load(int3(id.xy, 0));
    Out[id.y * w + id.x] = float4(e.rgb, effectHideTest(e.rgb, f.rgb, share, floorValue) ? 1.0 : 0.0);
}
)HLSL";
}
// And over a table of cases, for the parity with the C++ rule.
std::string tableKernelSource() {
    return std::string(kEffectHideHlsl) + R"HLSL(
StructuredBuffer<float4> A : register(t0);   // e.rgb, share
StructuredBuffer<float4> B : register(t1);   // f.rgb, floor
RWStructuredBuffer<uint> Out : register(u0);
cbuffer C : register(b0) { uint n; uint3 pad; };
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= n) return;
    Out[id.x] = effectHideTest(A[id.x].xyz, B[id.x].xyz, A[id.x].w, B[id.x].w) ? 1u : 0u;
}
)HLSL";
}

struct Quad {
    float x0, y0, x1, y1;   // pixels of the 64x64 target
    float z;
    float colour[4];
};
struct DrawSpec {
    Quad q;
    int blend;   // 0 off, 1 additive (SRC_ALPHA, ONE)
    int depth;   // 0 test LESS + write, 1 test LESS_EQUAL no write
};

struct Rig {
    Gpu g;
    ComPtr<ID3D11Texture2D> hdr, ds, other, cb;
    ComPtr<ID3D11RenderTargetView> hdrRtv, otherRtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cbuf;
    ComPtr<ID3D11RasterizerState> rsGame;
    ComPtr<ID3D11BlendState> bsOff, bsAdd;
    ComPtr<ID3D11DepthStencilState> dsWrite, dsTest;
};

Rig* g_rig = nullptr;
const void* g_eyeTarget = nullptr;   // the scene depth texture: the eye's identity to the module
const void* g_otherEye = nullptr;    // some other texture: not an eye the module has seen

void makeRig(Rig& r) {
    r.g = makeGpu();
    ID3D11Device* d = r.g.dev.Get();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr(d->CreateTexture2D(&td, nullptr, r.hdr.GetAddressOf()));
    hr(d->CreateRenderTargetView(r.hdr.Get(), nullptr, r.hdrRtv.GetAddressOf()));
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr(d->CreateTexture2D(&td, nullptr, r.other.GetAddressOf()));
    hr(d->CreateRenderTargetView(r.other.Get(), nullptr, r.otherRtv.GetAddressOf()));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr(d->CreateTexture2D(&td, nullptr, r.ds.GetAddressOf()));
    hr(d->CreateDepthStencilView(r.ds.Get(), nullptr, r.dsv.GetAddressOf()));
    ComPtr<ID3DBlob> vb = compile(kVs, "vs_5_0"), pb = compile(kPs, "ps_5_0");
    hr(d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, r.vs.GetAddressOf()));
    hr(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, r.ps.GetAddressOf()));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr(d->CreateBuffer(&bd, nullptr, r.cbuf.GetAddressOf()));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr(d->CreateRasterizerState(&rd, r.rsGame.GetAddressOf()));
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    hr(d->CreateBlendState(&b, r.bsOff.GetAddressOf()));
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    hr(d->CreateBlendState(&b, r.bsAdd.GetAddressOf()));
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
    dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    dd.BackFace = dd.FrontFace;
    hr(d->CreateDepthStencilState(&dd, r.dsWrite.GetAddressOf()));
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr(d->CreateDepthStencilState(&dd, r.dsTest.GetAddressOf()));
    g_eyeTarget = r.ds.Get();
    g_otherEye = r.other.Get();
}

// The frame start: the pipeline as the game leaves it for an eye draw. `scene` is the HDR target's clear.
void beginFrame(Rig& r, float scene, bool withSecondTarget = false) {
    ID3D11DeviceContext* c = r.g.ctx.Get();
    const float clear[4] = {scene, scene, scene, 1.0f};
    c->ClearRenderTargetView(r.hdrRtv.Get(), clear);
    const float clear2[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    c->ClearRenderTargetView(r.otherRtv.Get(), clear2);
    c->ClearDepthStencilView(r.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* v[2] = {r.hdrRtv.Get(), withSecondTarget ? r.otherRtv.Get() : nullptr};
    c->OMSetRenderTargets(withSecondTarget ? 2u : 1u, v, r.dsv.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);
    c->RSSetState(r.rsGame.Get());
    const D3D11_RECT sc = {2, 3, 61, 62};
    c->RSSetScissorRects(1, &sc);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(r.vs.Get(), nullptr, 0);
    c->PSSetShader(r.ps.Get(), nullptr, 0);
    ID3D11Buffer* b = r.cbuf.Get();
    c->VSSetConstantBuffers(0, 1, &b);
    c->PSSetConstantBuffers(0, 1, &b);
}

void setSpec(Rig& r, const DrawSpec& s) {
    ID3D11DeviceContext* c = r.g.ctx.Get();
    float cbv[12];
    cbv[0] = s.q.x0 / kSize * 2.0f - 1.0f;
    cbv[1] = 1.0f - s.q.y0 / kSize * 2.0f;
    cbv[2] = s.q.x1 / kSize * 2.0f - 1.0f;
    cbv[3] = 1.0f - s.q.y1 / kSize * 2.0f;
    cbv[4] = s.q.z;
    cbv[5] = cbv[6] = cbv[7] = 0.0f;
    std::memcpy(cbv + 8, s.q.colour, 16);
    c->UpdateSubresource(r.cbuf.Get(), 0, nullptr, cbv, 0, 0);
    const float bf[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    c->OMSetBlendState(s.blend ? r.bsAdd.Get() : r.bsOff.Get(), bf, 0xFFFFFFF7u);
    c->OMSetDepthStencilState(s.depth ? r.dsTest.Get() : r.dsWrite.Get(), 5);
}

// The state E1 compares: everything the replica may touch and everything it must not.
struct Snapshot {
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4] = {};
    UINT bmask = 0;
    ID3D11DepthStencilState* ds = nullptr;
    UINT dref = 0;
    UINT nScissor = 0;
    D3D11_RECT scissor[16] = {};
    UINT nVp = 0;
    D3D11_VIEWPORT vp[16] = {};
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vcb = nullptr;
    ID3D11Buffer* pcb = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* layout = nullptr;
};
void takeSnapshot(ID3D11DeviceContext* c, Snapshot* s) {
    c->RSGetState(&s->rs);
    c->OMGetBlendState(&s->bs, s->bf, &s->bmask);
    c->OMGetDepthStencilState(&s->ds, &s->dref);
    s->nScissor = 16;
    c->RSGetScissorRects(&s->nScissor, s->scissor);
    s->nVp = 16;
    c->RSGetViewports(&s->nVp, s->vp);
    c->OMGetRenderTargets(8, s->rtv, &s->dsv);
    c->VSGetShader(&s->vs, nullptr, nullptr);
    c->PSGetShader(&s->ps, nullptr, nullptr);
    c->VSGetConstantBuffers(0, 1, &s->vcb);
    c->PSGetConstantBuffers(0, 1, &s->pcb);
    c->IAGetPrimitiveTopology(&s->topo);
    c->IAGetInputLayout(&s->layout);
}
void releaseSnapshot(Snapshot* s) {
    for (IUnknown* u : {static_cast<IUnknown*>(s->rs), static_cast<IUnknown*>(s->bs), static_cast<IUnknown*>(s->ds), static_cast<IUnknown*>(s->dsv),
                        static_cast<IUnknown*>(s->vs), static_cast<IUnknown*>(s->ps), static_cast<IUnknown*>(s->vcb), static_cast<IUnknown*>(s->pcb),
                        static_cast<IUnknown*>(s->layout)})
        if (u) u->Release();
    for (ID3D11RenderTargetView* v : s->rtv)
        if (v) v->Release();
}
std::string diffSnapshots(const Snapshot& a, const Snapshot& b) {
    std::string d;
    auto add = [&](const char* w) {
        if (!d.empty()) d += ", ";
        d += w;
    };
    if (a.rs != b.rs) add("rasterizer state");
    if (a.bs != b.bs) add("blend state");
    if (std::memcmp(a.bf, b.bf, sizeof(a.bf)) != 0) add("blend factor");
    if (a.bmask != b.bmask) add("sample mask");
    if (a.ds != b.ds) add("depth-stencil state");
    if (a.dref != b.dref) add("stencil reference");
    if (a.nScissor != b.nScissor || std::memcmp(a.scissor, b.scissor, sizeof(D3D11_RECT) * std::min(a.nScissor, 16u)) != 0) add("scissor rects");
    if (a.nVp != b.nVp || std::memcmp(a.vp, b.vp, sizeof(D3D11_VIEWPORT) * std::min(a.nVp, 16u)) != 0) add("viewports");
    for (int i = 0; i < 8; ++i)
        if (a.rtv[i] != b.rtv[i]) {
            add("render targets");
            break;
        }
    if (a.dsv != b.dsv) add("depth view");
    if (a.vs != b.vs) add("vertex shader");
    if (a.ps != b.ps) add("pixel shader");
    if (a.vcb != b.vcb || a.pcb != b.pcb) add("constant buffers");
    if (a.topo != b.topo) add("topology");
    if (a.layout != b.layout) add("input layout");
    return d;
}

// ---- the module's environment ----------------------------------------------------------------------------------------------------------
void envSet(ID3D11DeviceContext* c, uint32_t n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) { c->OMSetRenderTargets(n, rtvs, dsv); }
void envClear(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const float colour[4]) { c->ClearRenderTargetView(rtv, colour); }
bool envTimerBegin(ID3D11DeviceContext*) { return true; }
void envTimerEnd(ID3D11DeviceContext*) {}
void envTimerPoll(ID3D11DeviceContext*, double* ms, uint32_t* n, uint32_t* skipped, uint32_t* invalid) {
    *ms = 1.0;
    *n = 4;
    *skipped = 0;
    *invalid = 0;
}
ComPtr<ID3D11Buffer> g_counterBuf;
ID3D11Buffer* envCounter(uint32_t* off) {
    *off = 8;   // not at zero: the module must honour the offset
    return g_counterBuf.Get();
}
EffectHistoryEnv makeEnv() {
    EffectHistoryEnv e;
    e.setRenderTargets = &envSet;
    e.clearRenderTarget = &envClear;
    e.timerBegin = &envTimerBegin;
    e.timerEnd = &envTimerEnd;
    e.timerPoll = &envTimerPoll;
    e.hiddenCounter = &envCounter;
    return e;
}

struct ReplayArgs {
    ID3D11DeviceContext* c;
    unsigned draws = 0;
};
void replay(void* p) {
    auto* a = static_cast<ReplayArgs*>(p);
    a->c->Draw(4, 0);
    ++a->draws;
}

constexpr uint64_t kVs0 = 0x68DDDEF04D9894AFull, kPs0 = 0x06332CA168B6DA63ull;   // pair 0
constexpr uint64_t kVs1 = 0xB75A6FF2CA9FA5D6ull, kPs1 = 0xD56F859BE4781431ull;   // pair 1

// One effect draw the way the draw path makes it: the module is offered it first, then the game's own issue. Returns the replays made.
unsigned effectDraw(Rig& r, const DrawSpec& s, uint64_t vs = kVs0, uint64_t ps = kPs0, std::string* stateDiff = nullptr) {
    setSpec(r, s);
    Snapshot before, after;
    takeSnapshot(r.g.ctx.Get(), &before);
    ReplayArgs a{r.g.ctx.Get(), 0};
    effectHistoryDraw(r.g.ctx.Get(), vs, ps, &replay, &a);
    takeSnapshot(r.g.ctx.Get(), &after);
    if (stateDiff) *stateDiff += diffSnapshots(before, after);
    releaseSnapshot(&before);
    releaseSnapshot(&after);
    r.g.ctx->Draw(4, 0);
    return a.draws;
}
void plainDraw(Rig& r, const DrawSpec& s) {
    setSpec(r, s);
    r.g.ctx->Draw(4, 0);
}

ComPtr<ID3D11Buffer> makeStaging(Rig& r, UINT bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> b;
    hr(r.g.dev->CreateBuffer(&bd, nullptr, b.GetAddressOf()));
    return b;
}

std::vector<uint8_t> readTexture(Rig& r, ID3D11Texture2D* t) {
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    hr(r.g.dev->CreateTexture2D(&d, nullptr, st.GetAddressOf()));
    r.g.ctx->CopyResource(st.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(r.g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m));
    const UINT bpp = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    std::vector<uint8_t> out(static_cast<size_t>(d.Width) * d.Height * bpp);
    for (UINT y = 0; y < d.Height; ++y) std::memcpy(&out[static_cast<size_t>(y) * d.Width * bpp], static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, static_cast<size_t>(d.Width) * bpp);
    r.g.ctx->Unmap(st.Get(), 0);
    return out;
}

struct Decision {
    std::vector<float> e;     // e.rgb per pixel
    std::vector<uint8_t> hidden;
};
// Runs the production HLSL decision over the two views the module hands out for `eye` (consuming them); false when it handed none.
bool runDecision(Rig& r, uint32_t w, uint32_t h, float share, float floorValue, Decision* out, const void* eyeKey = nullptr) {
    ID3D11ShaderResourceView *light = nullptr, *frame = nullptr;
    if (!effectHistoryViews(eyeKey ? eyeKey : g_eyeTarget, w, h, &light, &frame)) return false;
    static ComPtr<ID3D11ComputeShader> cs;
    if (!cs) {
        ComPtr<ID3DBlob> b = compile(decisionKernelSource(), "cs_5_0");
        hr(r.g.dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, cs.GetAddressOf()));
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = w * h * 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16;
    ComPtr<ID3D11Buffer> buf;
    hr(r.g.dev->CreateBuffer(&bd, nullptr, buf.GetAddressOf()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = w * h;
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr(r.g.dev->CreateUnorderedAccessView(buf.Get(), &ud, uav.GetAddressOf()));
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const uint32_t cbv[4] = {*reinterpret_cast<uint32_t*>(&share), *reinterpret_cast<uint32_t*>(&floorValue), w, h};
    D3D11_SUBRESOURCE_DATA init{cbv, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    hr(r.g.dev->CreateBuffer(&cbd, &init, cb.GetAddressOf()));
    ID3D11DeviceContext* c = r.g.ctx.Get();
    c->OMSetRenderTargets(0, nullptr, nullptr);   // as the temporal pass does before it reads (it restores them after): a resource bound as a target reads as null
    ID3D11ShaderResourceView* srvs[17] = {};
    srvs[5] = light;
    srvs[16] = frame;
    c->CSSetShader(cs.Get(), nullptr, 0);
    c->CSSetShaderResources(0, 17, srvs);
    ID3D11UnorderedAccessView* u = uav.Get();
    c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    ID3D11Buffer* cbp = cb.Get();
    c->CSSetConstantBuffers(0, 1, &cbp);
    c->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView* nullSrvs[17] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    c->CSSetShaderResources(0, 17, nullSrvs);
    c->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    c->CSSetShader(nullptr, nullptr, 0);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> st;
    hr(r.g.dev->CreateBuffer(&bd, nullptr, st.GetAddressOf()));
    c->CopyResource(st.Get(), buf.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(c->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m));
    const float* f = static_cast<const float*>(m.pData);
    out->e.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    out->hidden.assign(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        out->e[i * 3 + 0] = f[i * 4 + 0];
        out->e[i * 3 + 1] = f[i * 4 + 1];
        out->e[i * 3 + 2] = f[i * 4 + 2];
        out->hidden[i] = f[i * 4 + 3] > 0.5f ? 1 : 0;
    }
    c->Unmap(st.Get(), 0);
    return true;
}
size_t px(uint32_t x, uint32_t y) { return static_cast<size_t>(y) * kSize + x; }

void testParity(Rig& r) {
    // The HLSL against the C++ rule on a table: the boundaries, then a deterministic spread.
    struct Case {
        float e[3], share, f[3], floorValue;
    };
    std::vector<Case> cases;
    auto add = [&](float er, float eg, float eb, float fr, float fg, float fb, float share, float floorValue) {
        cases.push_back({{er, eg, eb}, share, {fr, fg, fb}, floorValue});
    };
    add(0.35f, 0.35f, 0.35f, 1, 1, 1, 0.35f, 0.002f);
    add(0.3499f, 0.3499f, 0.3499f, 1, 1, 1, 0.35f, 0.002f);
    add(0.002f, 0.002f, 0.002f, 0.002f, 0.002f, 0.002f, 0.35f, 0.002f);
    add(0.003f, 0.003f, 0.003f, 0, 0, 0, 0.35f, 0.002f);
    add(0, 0, 0, 0, 0, 0, 0.35f, 0.002f);
    add(-1, 0.5f, -1, 1, 1, 1, 0.35f, 0.002f);
    add(0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.35f, 0.002f);
    uint32_t s = 12345;
    auto rnd = [&]() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < 4000; ++i) {
        const float scale = std::pow(10.0f, rnd() * 6.0f - 4.0f);   // 1e-4 .. 1e2
        const float fscale = std::pow(10.0f, rnd() * 6.0f - 4.0f);
        add(rnd() * scale, rnd() * scale, rnd() * scale, rnd() * fscale, rnd() * fscale, rnd() * fscale, 0.2f + 0.3f * rnd(), 0.001f * rnd());
    }
    const UINT n = static_cast<UINT>(cases.size());
    std::vector<float> a(n * 4), b(n * 4);
    for (UINT i = 0; i < n; ++i) {
        a[i * 4 + 0] = cases[i].e[0]; a[i * 4 + 1] = cases[i].e[1]; a[i * 4 + 2] = cases[i].e[2]; a[i * 4 + 3] = cases[i].share;
        b[i * 4 + 0] = cases[i].f[0]; b[i * 4 + 1] = cases[i].f[1]; b[i * 4 + 2] = cases[i].f[2]; b[i * 4 + 3] = cases[i].floorValue;
    }
    auto mk = [&](const std::vector<float>& v, ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = n * 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 16;
        D3D11_SUBRESOURCE_DATA init{v.data(), 0, 0};
        ComPtr<ID3D11Buffer> buf;
        hr(r.g.dev->CreateBuffer(&bd, &init, buf.GetAddressOf()));
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_UNKNOWN;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.NumElements = n;
        hr(r.g.dev->CreateShaderResourceView(buf.Get(), &vd, srv.GetAddressOf()));
    };
    ComPtr<ID3D11ShaderResourceView> sa, sb;
    mk(a, sa);
    mk(b, sb);
    D3D11_BUFFER_DESC od{};
    od.ByteWidth = n * 4;
    od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    od.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    od.StructureByteStride = 4;
    ComPtr<ID3D11Buffer> out;
    hr(r.g.dev->CreateBuffer(&od, nullptr, out.GetAddressOf()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = n;
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr(r.g.dev->CreateUnorderedAccessView(out.Get(), &ud, uav.GetAddressOf()));
    ComPtr<ID3DBlob> blob = compile(tableKernelSource(), "cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    hr(r.g.dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, cs.GetAddressOf()));
    const uint32_t cbv[4] = {n, 0, 0, 0};
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{cbv, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    hr(r.g.dev->CreateBuffer(&cbd, &init, cb.GetAddressOf()));
    ID3D11DeviceContext* c = r.g.ctx.Get();
    ID3D11ShaderResourceView* srvs[2] = {sa.Get(), sb.Get()};
    c->CSSetShader(cs.Get(), nullptr, 0);
    c->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* u = uav.Get();
    c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    ID3D11Buffer* cbp = cb.Get();
    c->CSSetConstantBuffers(0, 1, &cbp);
    c->Dispatch((n + 63) / 64, 1, 1);
    ID3D11ShaderResourceView* nullSrvs[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    c->CSSetShaderResources(0, 2, nullSrvs);
    c->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    c->CSSetShader(nullptr, nullptr, 0);
    od.Usage = D3D11_USAGE_STAGING;
    od.BindFlags = 0;
    od.MiscFlags = 0;
    od.StructureByteStride = 0;
    od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> st;
    hr(r.g.dev->CreateBuffer(&od, nullptr, st.GetAddressOf()));
    c->CopyResource(st.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(c->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m));
    const uint32_t* res = static_cast<const uint32_t*>(m.pData);
    unsigned mismatches = 0, hides = 0;
    std::string first;
    for (UINT i = 0; i < n; ++i) {
        const bool cpp = effectHides(lumaOf(cases[i].e[0], cases[i].e[1], cases[i].e[2]), lumaOf(cases[i].f[0], cases[i].f[1], cases[i].f[2]), cases[i].share, cases[i].floorValue);
        hides += res[i] ? 1u : 0u;
        if ((res[i] != 0) != cpp) {
            ++mismatches;
            // Tolerate only a case that sits on the rounding edge of the comparison (the GPU may fuse the multiply-add): within 2 ulp of the boundary.
            const float le = lumaOf(cases[i].e[0], cases[i].e[1], cases[i].e[2]), lf = lumaOf(cases[i].f[0], cases[i].f[1], cases[i].f[2]);
            const float edge = cases[i].share * lf;
            if (std::fabs(le - edge) > 4e-6f * std::max(1.0f, std::fabs(edge)) && std::fabs(le - cases[i].floorValue) > 4e-6f) {
                if (first.empty()) {
                    char msg[200];
                    std::snprintf(msg, sizeof(msg), "case %u: e luma %.9g f luma %.9g share %.3f floor %.5f gpu %u cpp %d", i, static_cast<double>(le), static_cast<double>(lf),
                                  static_cast<double>(cases[i].share), static_cast<double>(cases[i].floorValue), res[i], cpp ? 1 : 0);
                    first = msg;
                }
            }
        }
    }
    c->Unmap(st.Get(), 0);
    checkf(first.empty(), "the HLSL decision and the C++ one disagree away from the boundary: %s", first.c_str());
    checkf(mismatches <= 2, "the HLSL decision and the C++ one disagree on %u of %u cases (rounding edge allowed: 2)", mismatches, n);
    check(hides > 100 && hides < n - 100, "the parity table has both outcomes in quantity");
}

// ---- the stale-marker depth check on a GPU: the table (HLSL against C++), and the shader's own wrapper over a real previous-depth texture -------------
ComPtr<ID3D11Buffer> makeStructured(Rig& r, const std::vector<float>& v, UINT stride, bool uav) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(v.size() * 4);
    bd.BindFlags = uav ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init{v.data(), 0, 0};
    ComPtr<ID3D11Buffer> b;
    hr(r.g.dev->CreateBuffer(&bd, uav ? nullptr : &init, b.GetAddressOf()));
    return b;
}
std::vector<uint32_t> readBuffer(Rig& r, ID3D11Buffer* src, UINT bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> st;
    hr(r.g.dev->CreateBuffer(&bd, nullptr, st.GetAddressOf()));
    r.g.ctx->CopyResource(st.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(r.g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m));
    std::vector<uint32_t> out(bytes / 4);
    std::memcpy(out.data(), m.pData, bytes);
    r.g.ctx->Unmap(st.Get(), 0);
    return out;
}

void testStaleDepth(Rig& r) {
    ID3D11DeviceContext* c = r.g.ctx.Get();
    {
        // The table: 1024 pseudo-random cases around the tolerance against the C++ rule.
        const std::string src = std::string(kStaleDepthHlsl) + R"HLSL(
StructuredBuffer<float4> A : register(t0);   // the four previous depths
StructuredBuffer<float4> B : register(t1);   // expected, offset
RWStructuredBuffer<uint> Out : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { Out[id.x] = staleDepthConfirms(A[id.x], B[id.x].x, B[id.x].y) ? 1u : 0u; }
)HLSL";
        const UINT n = 1024;
        std::vector<float> a(n * 4), b(n * 4);
        uint32_t seed = 777;
        auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
        };
        std::vector<int> want(n);
        for (UINT i = 0; i < n; ++i) {
            const float off = rnd() < 0.5f ? 0.0f : 0.5f;
            const float expected = off + std::pow(10.0f, rnd() * 2.0f - 3.0f);   // an encoded depth of 1e-3 .. 1e-1, where the 1% tolerance (not the floor) decides
            for (int k = 0; k < 4; ++k) a[i * 4 + k] = off + (expected - off) * (1.0f + (rnd() - 0.5f) * 0.05f);   // +-2.5% about the encoded depth
            b[i * 4] = expected;
            b[i * 4 + 1] = off;
            want[i] = staleDepthConfirms(&a[i * 4], expected, off) ? 1 : 0;
        }
        ComPtr<ID3D11Buffer> ba = makeStructured(r, a, 16, false), bb = makeStructured(r, b, 16, false);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_UNKNOWN;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.NumElements = n;
        ComPtr<ID3D11ShaderResourceView> sa, sb;
        hr(r.g.dev->CreateShaderResourceView(ba.Get(), &vd, sa.GetAddressOf()));
        hr(r.g.dev->CreateShaderResourceView(bb.Get(), &vd, sb.GetAddressOf()));
        ComPtr<ID3D11Buffer> out = makeStructured(r, std::vector<float>(n, 0.0f), 4, true);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = n;
        ComPtr<ID3D11UnorderedAccessView> uav;
        hr(r.g.dev->CreateUnorderedAccessView(out.Get(), &ud, uav.GetAddressOf()));
        ComPtr<ID3DBlob> blob = compile(src, "cs_5_0");
        ComPtr<ID3D11ComputeShader> cs;
        hr(r.g.dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, cs.GetAddressOf()));
        ID3D11ShaderResourceView* srvs[2] = {sa.Get(), sb.Get()};
        c->CSSetShader(cs.Get(), nullptr, 0);
        c->CSSetShaderResources(0, 2, srvs);
        ID3D11UnorderedAccessView* u = uav.Get();
        c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        c->Dispatch(n / 64, 1, 1);
        ID3D11ShaderResourceView* ns[2] = {};
        ID3D11UnorderedAccessView* nu = nullptr;
        c->CSSetShaderResources(0, 2, ns);
        c->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
        c->CSSetShader(nullptr, nullptr, 0);
        const std::vector<uint32_t> res = readBuffer(r, out.Get(), n * 4);
        unsigned mismatches = 0, yes = 0;
        for (UINT i = 0; i < n; ++i) {
            yes += res[i] ? 1u : 0u;
            mismatches += (static_cast<int>(res[i]) != want[i]) ? 1u : 0u;
        }
        checkf(mismatches <= 2, "the stale-depth HLSL and the C++ rule disagree on %u of %u cases (rounding edge allowed: 2)", mismatches, n);
        check(yes > 100 && yes < n - 100, "the stale-depth table has both outcomes in quantity");
    }
    // The shader's own wrapper, extracted from the production text, over a real previous-depth texture.
    const std::string full(kTemporalCsHlsl);
    const size_t at = full.find("bool staleMarkerDepthHidden(");
    check(at != std::string::npos, "the production text has the wrapper");
    const size_t end = full.find("\n}\n", at);
    check(end != std::string::npos, "...and it ends");
    const std::string wrapper = full.substr(at, end + 3 - at);
    const std::string src = std::string(R"HLSL(
Texture2D<float> ZP : register(t3);
cbuffer P : register(b0) { int2 size; int2 pad0; float4 knobs; float4 holoJitter; float4 args; float4 zp; };
RWStructuredBuffer<uint> Out : register(u0);
)HLSL") + kStaleDepthHlsl + wrapper + R"HLSL(
[numthreads(1, 1, 1)]
void main() { Out[0] = staleMarkerDepthHidden(args.xy, args.zw, zp.x) ? 1u : 0u; }
)HLSL";
    ComPtr<ID3DBlob> blob = compile(src, "cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    hr(r.g.dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, cs.GetAddressOf()));
    // Last frame's depth: 0.001 (10 m) left of column 12, 0.0002 (50 m) from it on: a rail at 10 m in front of a wall at 50 m.
    std::vector<float> depth(16 * 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) depth[static_cast<size_t>(y) * 16 + x] = x >= 12 ? 0.0002f : 0.001f;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA tinit{depth.data(), 16 * 4, 0};
    ComPtr<ID3D11Texture2D> zpTex;
    hr(r.g.dev->CreateTexture2D(&td, &tinit, zpTex.GetAddressOf()));
    ComPtr<ID3D11ShaderResourceView> zpSrv;
    hr(r.g.dev->CreateShaderResourceView(zpTex.Get(), nullptr, zpSrv.GetAddressOf()));
    ComPtr<ID3D11Buffer> out = makeStructured(r, std::vector<float>(1, 0.0f), 4, true);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 1;
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr(r.g.dev->CreateUnorderedAccessView(out.Get(), &ud, uav.GetAddressOf()));
    // One pixel through the wrapper: p, motion, the predicted depth in metres, and whether last frame's depth is valid (holoJitter.w).
    auto hiddenFor = [&](float px, float py, float mx, float my, float zPred, float prevValid) {
        struct Cb {
            int32_t size[2], pad0[2];
            float knobs[4], holoJitter[4], args[4], zp[4];
        } cbv = {{16, 16}, {0, 0}, {0.0f, 1.0f, 0.01f, 0.0f} /* knobs: A 0, depth bound, B 0.01 */, {0.0f, 0.0f, 3.0f, prevValid} /* z treated frames, w valid */, {px, py, mx, my}, {zPred, 0, 0, 0}};
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(cbv);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA ci{&cbv, 0, 0};
        ComPtr<ID3D11Buffer> cb;
        hr(r.g.dev->CreateBuffer(&cbd, &ci, cb.GetAddressOf()));
        c->CSSetShader(cs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[4] = {nullptr, nullptr, nullptr, zpSrv.Get()};
        c->CSSetShaderResources(0, 4, srvs);
        ID3D11UnorderedAccessView* u = uav.Get();
        c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        ID3D11Buffer* cbp = cb.Get();
        c->CSSetConstantBuffers(0, 1, &cbp);
        c->Dispatch(1, 1, 1);
        ID3D11ShaderResourceView* ns[4] = {};
        ID3D11UnorderedAccessView* nu = nullptr;
        c->CSSetShaderResources(0, 4, ns);
        c->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
        c->CSSetShader(nullptr, nullptr, 0);
        return readBuffer(r, out.Get(), 4)[0] != 0;
    };
    // The camera term predicts 10 m (depth 0.001) for a rail pixel. Left of column 12, that is what was there last frame.
    check(!hiddenFor(4, 4, 0, 0, 10.0f, 1.0f), "kind 4, the previous depth confirms the camera term: history KEPT");
    check(hiddenFor(13, 4, 0, 0, 10.0f, 1.0f), "kind 4, last frame held the far wall (50 m) where the camera term sends the rail (10 m): history HIDDEN");
    check(!hiddenFor(13, 4, 0, 0, 50.0f, 1.0f), "...but a surface the camera term puts at the wall's own depth there is confirmed: KEPT");
    check(!hiddenFor(13, 4, -2, 0, 10.0f, 1.0f), "one of the four texels around the previous position confirming is enough (previous position 11.0: columns 11 and 12): KEPT");
    check(hiddenFor(13, 4, 0.6f, 0, 10.0f, 1.0f), "previous position 13.6: columns 13 and 14, both the wall: HIDDEN");
    check(!hiddenFor(13, 4, 0, 0, 10.0f, 0.0f), "no valid previous depth (holoJitter.w = 0): the pixel cannot be judged, left as it was");
    check(!hiddenFor(13, 4, 0, 0, 0.0f, 1.0f), "no predicted depth (zPred = 0, the far plane): left as it was");
    check(!hiddenFor(13, 4, -100, 0, 10.0f, 1.0f), "a previous position off the image: left as it was");
    std::printf("effect_history_test: the stale-marker depth check matches the C++ rule, and the shader's own wrapper decides as flat does\n");
}

void pumpFrames(int n, ID3D11DeviceContext* ctx) {
    for (int i = 0; i < n; ++i) {
        ctx->Flush();
        Sleep(4);
        effectHistoryFrameBoundary(ctx);
    }
}
std::string lineWith(size_t from, const char* a) {
    for (size_t i = from; i < g_lines.size(); ++i)
        if (has(g_lines[i], a)) return g_lines[i];
    return std::string();
}

void runDevice() {
    Rig r;
    makeRig(r);
    g_rig = &r;
    check(edvr::reportSystemD3D11Only("effect_history_test"), "the rig runs on Windows' own d3d11, not EDVR's proxy beside it");
    ID3D11DeviceContext* ctx = r.g.ctx.Get();

    testParity(r);
    testStaleDepth(r);
    std::printf("effect_history_test: HLSL decision matches the C++ rule on the table\n");

    // A counter buffer for the hidden-pixel total: 16 uints, the counter at byte 8.
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        hr(r.g.dev->CreateBuffer(&bd, nullptr, g_counterBuf.GetAddressOf()));
    }
    auto setCounter = [&](uint32_t v, uint32_t k4 = 0) {
        uint32_t words[16] = {};
        words[2] = v;   // Stats[40]: the pixels the effect rule hid
        words[3] = k4;  // Stats[41]: the pixels the stale-marker depth check hid
        ctx->UpdateSubresource(g_counterBuf.Get(), 0, nullptr, words, 0, 0);
    };
    setCounter(1000);

    g_lines.clear();
    effectHistoryInstall(makeEnv());
    check(!lineWith(0, "effect history: installed. On NVIDIA's upscaler, 1 listed effect draw pairs").empty(), "install says what is listed, in one line");
    check(has(lineWith(0, "effect history: installed"), "particle-billboard 68DDDEF04D9894AF/06332CA168B6DA63") && !has(lineWith(0, "effect history: installed"), "halo-billboard"),
          "the install line names the pair, and not the halo");
    check(has(lineWith(0, "effect history: installed"), "advanced.effect_history"), "...and the temporary key");
    check(has(lineWith(0, "effect history: installed"), "0.35") && has(lineWith(0, "effect history: installed"), "0.002"), "...and the share and the floor");

    // The module is off until the pass says NVIDIA's.
    check(!effectHistoryLive(), "off until the temporal pass says it is on NVIDIA's path");
    effectHistorySetNvidia(true);
    check(effectHistoryLive(), "on when it does");

    const std::vector<uint8_t> noRt;
    const float glow[4] = {0.5f, 0.25f, 0.125f, 1.0f};

    // ---- E1, E2, E3: the replica -------------------------------------------------------------------------------------------------------
    beginFrame(r, 0.1f, true);
    plainDraw(r, {{0, 0, 32, 64, 0.2f, {0.0f, 0.0f, 0.0f, 1}}, 0, 0});   // the wall: the left half, depth 0.2, written
    const std::vector<uint8_t> rtAfterWall = readTexture(r, r.hdr.Get());
    const std::vector<uint8_t> depthAfterWall = readTexture(r, r.ds.Get());
    std::string diffs;
    ReplayArgs ra{ctx, 0};
    {
        // The replica alone, with the game's own draw withheld: it must leave the game's frame exactly as it was.
        setSpec(r, {{16, 16, 48, 48, 0.5f, {glow[0], glow[1], glow[2], glow[3]}}, 1, 1});
        Snapshot before, after;
        takeSnapshot(ctx, &before);
        effectHistoryDraw(ctx, kVs0, kPs0, &replay, &ra);
        takeSnapshot(ctx, &after);
        diffs = diffSnapshots(before, after);
        releaseSnapshot(&before);
        releaseSnapshot(&after);
    }
    checkf(diffs.empty(), "E1: the replica left state changed: %s", diffs.c_str());
    check(ra.draws == 1, "E1: the replica re-issued the draw once");
    check(readTexture(r, r.hdr.Get()) == rtAfterWall, "E2: the game's HDR target is byte for byte what it was");
    check(readTexture(r, r.ds.Get()) == depthAfterWall, "E2: the game's depth buffer is byte for byte what it was");
    {
        // The second render target (bound beside the first) came back too.
        ID3D11RenderTargetView* rtv[8] = {};
        ID3D11DepthStencilView* d = nullptr;
        ctx->OMGetRenderTargets(8, rtv, &d);
        check(rtv[0] == r.hdrRtv.Get() && rtv[1] == r.otherRtv.Get() && !rtv[2] && d == r.dsv.Get(), "E1: both bound render targets and the depth view are the game's");
        for (auto* v : rtv)
            if (v) v->Release();
        if (d) d->Release();
    }
    {
        // E3: what the private target holds -- read through the module's own views and the production decision kernel.
        Decision d;
        check(runDecision(r, kSize, kSize, kEffectHideShare, kEffectHideFloor, &d), "E3: the module hands the pass its two views after a replica");
        auto e = [&](uint32_t x, uint32_t y, int c) { return d.e[px(x, y) * 3 + static_cast<size_t>(c)]; };
        checkf(std::fabs(e(40, 32, 0) - 0.5f) < 0.02f && std::fabs(e(40, 32, 1) - 0.25f) < 0.02f && std::fabs(e(40, 32, 2) - 0.125f) < 0.02f,
               "E3: the private target holds the effect's colour in front of the game's depth (got %.3f %.3f %.3f)", static_cast<double>(e(40, 32, 0)),
               static_cast<double>(e(40, 32, 1)), static_cast<double>(e(40, 32, 2)));
        check(e(24, 32, 0) == 0.0f && e(24, 32, 1) == 0.0f, "E3: nothing where the quad is behind the game's wall");
        check(e(8, 8, 0) == 0.0f && e(60, 60, 0) == 0.0f && e(10, 40, 0) == 0.0f, "E3: nothing outside the quad");
        check(e(47, 47, 0) > 0.4f && e(48, 48, 0) == 0.0f, "E3: the quad's edge is pixel exact");
        // The strong glow over the dim scene: the scene here is the wall (0) and the 0.1 clear, the game has not drawn the glow yet -- so F is 0.1 and E 0.5.
        check(d.hidden[px(40, 32)] == 1, "E4: a strong glow over a dim scene is hidden");
        check(d.hidden[px(8, 8)] == 0, "E4: no glow, not hidden");
    }
    // The views are consumed by the asking.
    {
        ID3D11ShaderResourceView *l = nullptr, *f = nullptr;
        check(!effectHistoryViews(g_eyeTarget,kSize, kSize, &l, &f) && l == nullptr && f == nullptr, "E4: a second ask in the same eye-frame gets nothing");
    }
    // A size that is not the eye's is refused, and counted.
    {
        const size_t from = g_lines.size();
        (void)from;
        effectDraw(r, {{16, 16, 48, 48, 0.5f, {glow[0], glow[1], glow[2], glow[3]}}, 1, 1});
        ID3D11ShaderResourceView *l = nullptr, *f = nullptr;
        check(!effectHistoryViews(g_eyeTarget,kSize / 2, kSize / 2, &l, &f), "E4: views at a size that is not the eye's are refused");
        check(!effectHistoryViews(g_otherEye, kSize, kSize, &l, &f), "E4: another eye (another scene depth) saw nothing");
    }

    // ---- E3 again: two draws of one eye-frame sum; a new eye-frame starts clean ------------------------------------------------------------
    beginFrame(r, 0.1f);
    plainDraw(r, {{0, 0, 32, 64, 0.2f, {0.0f, 0.0f, 0.0f, 1}}, 0, 0});
    check(effectDraw(r, {{36, 8, 52, 24, 0.5f, {0.125f, 0.125f, 0.125f, 1.0f}}, 1, 1}) == 1, "a listed draw is replicated once");
    check(effectDraw(r, {{36, 8, 52, 24, 0.5f, {0.125f, 0.125f, 0.125f, 1.0f}}, 1, 1}) == 1, "a second draw of the pair is replicated too");
    check(effectDraw(r, {{36, 8, 52, 24, 0.5f, {0.125f, 0.125f, 0.125f, 1.0f}}, 1, 1}, kVs1, kPs1) == 0, "the halo/anchor billboard (not listed) is not replicated");
    {
        Decision d;
        check(runDecision(r, kSize, kSize, kEffectHideShare, kEffectHideFloor, &d), "two draws, one pair of views");
        checkf(std::fabs(d.e[px(40, 12) * 3] - 0.25f) < 0.01f, "E3: two draws of one eye-frame sum in the private target (got %.3f)", static_cast<double>(d.e[px(40, 12) * 3]));
    }
    beginFrame(r, 0.1f);
    effectHistoryFrameBoundary(ctx);   // the frame turns: the next listed draw starts the eye over
    check(effectDraw(r, {{4, 4, 12, 12, 0.5f, {0.25f, 0.25f, 0.25f, 1.0f}}, 1, 1}) == 1, "the next eye-frame's first listed draw");
    {
        Decision d;
        check(runDecision(r, kSize, kSize, kEffectHideShare, kEffectHideFloor, &d), "views again");
        check(d.e[px(40, 12) * 3] == 0.0f, "E3: the private target was cleared for the new eye-frame");
        check(d.e[px(8, 8) * 3] > 0.2f, "E3: ...and holds the new draw");
    }

    // ---- E4: the decision on faint glow over a bright scene, and the floor -----------------------------------------------------------------
    beginFrame(r, 0.1f);
    plainDraw(r, {{0, 32, 32, 64, 0.9f, {3.0f, 3.0f, 3.0f, 1}}, 0, 0});   // a bright surface: the lower left
    effectDraw(r, {{8, 8, 24, 24, 0.5f, {0.5f, 0.5f, 0.5f, 1.0f}}, 1, 1});          // strong glow on the dim scene (the game's own draw adds it to the scene: F 0.6)
    effectDraw(r, {{8, 40, 24, 56, 0.5f, {0.02f, 0.02f, 0.02f, 1.0f}}, 1, 1});      // faint glow on the bright surface (F 3.02)
    {
        Decision d;
        check(runDecision(r, kSize, kSize, kEffectHideShare, kEffectHideFloor, &d), "views for the decision frame");
        check(d.hidden[px(16, 16)] == 1, "E4: the strong glow over the dim scene is hidden");
        check(d.hidden[px(16, 48)] == 0, "E4: the faint glow over the bright scene is spared (its anti-aliasing is kept)");
        check(d.hidden[px(40, 40)] == 0 && d.hidden[px(2, 2)] == 0, "E4: pixels with no glow are never hidden");
    }
    beginFrame(r, 0.0f);   // a black scene: the floor decides
    effectDraw(r, {{8, 8, 24, 24, 0.5f, {0.001f, 0.001f, 0.001f, 1.0f}}, 1, 1});
    effectDraw(r, {{40, 8, 56, 24, 0.5f, {0.003f, 0.003f, 0.003f, 1.0f}}, 1, 1});
    {
        Decision d;
        check(runDecision(r, kSize, kSize, kEffectHideShare, kEffectHideFloor, &d), "views for the floor frame");
        check(d.hidden[px(16, 16)] == 0, "E4: a glow below the floor is never hidden, though it is all the light there is");
        check(d.hidden[px(48, 16)] == 1, "E4: a glow just above the floor on a black scene is hidden");
    }

    // ---- E4b: two eyes: each scene depth has its own private target, and each is handed over by its own depth -------------------------------------------
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kSize;
        td.MipLevels = td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R11G11B10_FLOAT;   // the game's real HDR format
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> hdr2, ds2;
        ComPtr<ID3D11RenderTargetView> rtv2;
        ComPtr<ID3D11DepthStencilView> dsv2;
        hr(r.g.dev->CreateTexture2D(&td, nullptr, hdr2.GetAddressOf()));
        hr(r.g.dev->CreateRenderTargetView(hdr2.Get(), nullptr, rtv2.GetAddressOf()));
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr(r.g.dev->CreateTexture2D(&td, nullptr, ds2.GetAddressOf()));
        hr(r.g.dev->CreateDepthStencilView(ds2.Get(), nullptr, dsv2.GetAddressOf()));
        beginFrame(r, 0.1f);
        effectDraw(r, {{4, 4, 20, 20, 0.5f, {0.25f, 0.25f, 0.25f, 1.0f}}, 1, 1});   // eye A (the rig's own target and depth)
        const float scene2[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        ctx->ClearRenderTargetView(rtv2.Get(), scene2);
        ctx->ClearDepthStencilView(dsv2.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        ID3D11RenderTargetView* v2 = rtv2.Get();
        ctx->OMSetRenderTargets(1, &v2, dsv2.Get());
        effectDraw(r, {{40, 40, 56, 56, 0.5f, {0.25f, 0.25f, 0.25f, 1.0f}}, 1, 1});   // eye B (its own target and depth)
        Decision a, b;
        check(runDecision(r, kSize, kSize, kEffectHideShare, kEffectHideFloor, &a, r.ds.Get()), "E4b: eye A gets its views by its own depth");
        check(runDecision(r, kSize, kSize, kEffectHideShare, kEffectHideFloor, &b, ds2.Get()), "E4b: eye B gets its views by its own depth");
        check(a.hidden[px(10, 10)] == 1 && a.hidden[px(48, 48)] == 0, "E4b: eye A's private target holds only eye A's glow");
        check(b.hidden[px(48, 48)] == 1 && b.hidden[px(10, 10)] == 0, "E4b: eye B's private target holds only eye B's glow (R11G11B10 game target)");
        ID3D11ShaderResourceView *l = nullptr, *f = nullptr;
        check(!effectHistoryViews(ds2.Get(), kSize, kSize, &l, &f), "E4b: eye B's views are consumed by the asking");
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // ---- E5: what is left alone --------------------------------------------------------------------------------------------------------
    const uint64_t replicasBefore = effectHistoryStats().replicas;
    const uint64_t refusedBefore = effectHistoryStats().refused;
    beginFrame(r, 0.1f);
    check(effectDraw(r, {{16, 16, 48, 48, 0.5f, {0.5f, 0.5f, 0.5f, 1.0f}}, 1, 1}, 0x1111, 0x2222) == 0, "E5: an unlisted pair is not replicated");
    check(effectDraw(r, {{16, 16, 48, 48, 0.5f, {0.5f, 0.5f, 0.5f, 1.0f}}, 1, 1}, kVs0, kPs1) == 0, "E5: a listed vertex shader with the wrong pixel shader is not replicated");
    check(effectHistoryStats().replicas == replicasBefore, "E5: nothing counted for them");
    {
        // A target with no depth view.
        ID3D11RenderTargetView* v = r.hdrRtv.Get();
        ctx->OMSetRenderTargets(1, &v, nullptr);
        ReplayArgs a{ctx, 0};
        setSpec(r, {{16, 16, 48, 48, 0.5f, {0.5f, 0.5f, 0.5f, 1.0f}}, 1, 1});
        ctx->OMSetRenderTargets(1, &v, nullptr);
        effectHistoryDraw(ctx, kVs0, kPs0, &replay, &a);
        check(a.draws == 0 && effectHistoryStats().refused == refusedBefore + 1, "E5: an eye target with no depth view is refused and counted");
        // With a UAV on the draw.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kSize;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> ut;
        hr(r.g.dev->CreateTexture2D(&td, nullptr, ut.GetAddressOf()));
        ComPtr<ID3D11UnorderedAccessView> uav;
        hr(r.g.dev->CreateUnorderedAccessView(ut.Get(), nullptr, uav.GetAddressOf()));
        ID3D11UnorderedAccessView* u = uav.Get();
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &v, r.dsv.Get(), 1, 1, &u, nullptr);
        effectHistoryDraw(ctx, kVs0, kPs0, &replay, &a);
        check(a.draws == 0 && effectHistoryStats().refused == refusedBefore + 2, "E5: a draw with a UAV is refused (it would be done twice)");
        ID3D11UnorderedAccessView* none = nullptr;
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &v, r.dsv.Get(), 1, 1, &none, nullptr);
    }

    // ---- E6: the census ----------------------------------------------------------------------------------------------------------------
    // Fresh module state: a window that holds exactly what this section makes.
    effectHistoryShutdown();
    effectHistoryInstall(makeEnv());
    effectHistorySetNvidia(true);
    g_lines.clear();
    beginFrame(r, 0.1f);
    setCounter(1000, 5000);
    for (int i = 0; i < 3; ++i) effectDraw(r, {{16, 16, 32, 32, 0.5f, {0.25f, 0.25f, 0.25f, 1.0f}}, 1, 1});          // 3 draws of pair 0, each 16x16 = 256 pixels
    effectDraw(r, {{16, 16, 32, 32, 0.5f, {0.25f, 0.25f, 0.25f, 1.0f}}, 1, 1}, kVs1, kPs1);                       // the halo: not listed, not counted
    effectHistoryNoteSubstituted(kVs0, kPs0);   // two listed draws a fix substituted: left alone, counted
    effectHistoryNoteSubstituted(kVs0, kPs0);
    effectHistoryNoteSubstituted(kVs1, kPs1);   // an unlisted pair: ignored
    {
        ID3D11ShaderResourceView *l = nullptr, *f = nullptr;
        check(effectHistoryViews(g_eyeTarget,kSize, kSize, &l, &f), "an eye-frame handed to the shader (the counter's baseline)");
    }
    pumpFrames(1, ctx);
    for (int round = 0; round < 2; ++round) {
        // 30 frames to the next counter read; the second round has the shader's total grown by 5000 over one more eye-frame.
        if (round == 1) {
            setCounter(6000, 5300);
            for (int k = 0; k < 3; ++k) effectHistorySetNvidia(true);   // three more eye-frames of the pass
            beginFrame(r, 0.1f);
            effectDraw(r, {{16, 16, 32, 32, 0.5f, {0.25f, 0.25f, 0.25f, 1.0f}}, 1, 1});
            ID3D11ShaderResourceView *l = nullptr, *f = nullptr;
            check(effectHistoryViews(g_eyeTarget,kSize, kSize, &l, &f), "the second eye-frame handed to the shader");
        }
        pumpFrames(40, ctx);
    }
    effectHistoryCensusNow();
    const std::string census = lineWith(0, "effect history: last");
    check(!census.empty(), "E6: the census line is written");
    checkf(has(census, "particle-billboard: ") && !has(census, "halo-billboard"), "E6: the listed pair is named, the halo is not, in %s", census.c_str());
    checkf(has(census, "/frame left alone (a fix substitutes them)") && !has(census, " 0.00/frame left alone"), "E6: the draws a fix substituted are counted (%s)", census.c_str());
    checkf(has(census, "256 covered pixels per draw (4 sampled)") || has(census, "256 covered pixels per draw (3 sampled)") || has(census, "256 covered pixels per draw (1 sampled)"),
           "E6: the covered pixels per draw are exact on a pixel-aligned quad (%s)", census.c_str());
    checkf(has(census, "pixels hidden: 5000 per eye-frame with a listed draw"), "E6: the hidden-pixel counter's growth over the eye-frames it was counted for (%s)", census.c_str());
    checkf(has(census, "replica GPU 0.250 ms per replicated draw"), "E6: the GPU timing (%s)", census.c_str());
    const std::string census4 = lineWith(0, "kind-4 depth check: last");
    checkf(has(census4, "100.0 pixels hidden per eye-frame (3 eye-frames counted)"), "E6: the kind-4 census: the second counter's growth (300) over the pass's eye-frames (3): %s", census4.c_str());
    check(has(census4, "within 1%"), "E6: the kind-4 line says the tolerance");
    check(has(census, "private targets 0.0 MiB") || has(census, "private targets 0.") || has(census, "private targets 1."), "E6: the memory held is said");
    // The key: off stops the views (and, in the draw path, every offer); on again restores them. Said in the log.
    {
        const size_t from = g_lines.size();
        effectHistorySetNvidia(true);
        check(effectHistoryLive(), "the key defaults on");
        effectDraw(r, {{16, 16, 32, 32, 0.5f, {0.25f, 0.25f, 0.25f, 1.0f}}, 1, 1});
        effectHistoryConfigure(false);
        check(!effectHistoryLive(), "advanced.effect_history = off: not live, though the pass is on NVIDIA's path");
        ID3D11ShaderResourceView *l = nullptr, *f = nullptr;
        check(!effectHistoryViews(g_eyeTarget, kSize, kSize, &l, &f), "...and a replica made before it is turned off is not handed to the shader");
        check(has(lineWith(from, "effect history (advanced.effect_history, TEMPORARY A/B key): off"), "no listed draw is replicated"), "...and the log says so");
        effectHistoryConfigure(true);
        check(effectHistoryLive(), "on again");
        check(!lineWith(from, "effect history (advanced.effect_history, TEMPORARY A/B key): on").empty(), "...said too");
    }
    // The lapse: the pass stops saying NVIDIA's path.
    effectHistorySetNvidia(true);
    check(effectHistoryLive(), "live");
    pumpFrames(1, ctx);
    for (int i = 0; i < 700; ++i) effectHistoryFrameBoundary(ctx);
    check(!effectHistoryLive(), "E6: the rule stands down by itself when the pass stops asking");

    // ---- E7: the debug layer -----------------------------------------------------------------------------------------------------------
    if (r.g.info) {
        const UINT64 n = r.g.info->GetNumStoredMessages();
        unsigned errors = 0;
        std::string first;
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            r.g.info->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
            if (SUCCEEDED(r.g.info->GetMessage(i, m, &len)) && (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                ++errors;
                if (first.empty()) first = m->pDescription ? m->pDescription : "";
            }
        }
        checkf(errors == 0, "E7: the D3D11 debug layer reported %u error(s); first: %s", errors, first.c_str());
    }
    std::printf("effect_history_test: device half on WARP, debug layer %s\n", r.g.debugLayer ? "on" : "off");
    effectHistoryShutdown();
    g_rig = nullptr;
}

DWORD WINAPI watchdog(LPVOID) {
    Sleep(180000);
    std::printf("effect_history_test: watchdog -- the device half hung\n");
    std::fflush(stdout);
    ExitProcess(3);
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        else if (std::strcmp(argv[i], "--dry-run") == 0) dry = true;
        else if (std::strcmp(argv[i], "--verbose") == 0) edvr::g_verbose = true;
    }
    if (dry) {
        std::printf("effect_history_test: dry run -- no device, no files\n");
        return 0;
    }
    if (!selfTest) {
        std::printf("usage: effect_history_test --self-test [--verbose] | --dry-run\n");
        return 2;
    }
    CreateThread(nullptr, 0, &watchdog, nullptr, 0, nullptr);
    try {
        runPure();
        std::printf("effect_history_test: pure half ok (%u checks)\n", g_checks);
        runDevice();
    } catch (const std::exception& e) {
        std::printf("effect_history_test: FAILED: %s\n", e.what());
        const size_t n = edvr::g_lines.size();
        for (size_t i = n > 12 ? n - 12 : 0; i < n; ++i) std::printf("    log: %s\n", edvr::g_lines[i].c_str());
        return 1;
    }
    std::printf("effect_history_test: %u checks, 0 failed\n", g_checks);
    return 0;
}
