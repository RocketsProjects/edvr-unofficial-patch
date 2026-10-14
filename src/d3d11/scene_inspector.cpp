// The scene inspector (scene_inspector.h; docs/scene-inspector-2026-10-14.md). The rules, the text and the image conversions are in
// scene_inspector_core.h (tools\scene_inspector_test). This file collects: the left eye's draws of ONE frame (vscreen.cpp offers them),
// each duplicated twice -- scissored to the box and to its centre pixel -- under occlusion queries; the temporal pass's inputs and DLSS's
// output, staged in the same frame (temporal_pass.cpp offers them); and it writes the log block, the CSVs and the PNGs.
// Phases: a press arms; the next frame boundary starts collecting; the boundary after closes the frame; the answers are polled at every
// boundary until every query and staging copy has answered or kReadTicksMax boundaries have passed, and the report is written.
//
// Everything the duplicate touches is put back before the draw call returns: the rasterizer state, the scissor rects, the blend state
// (and its factor and sample mask) and the depth-stencil state (and its reference). tools\scene_inspector_test runs this file on a WARP
// device and compares every one of those, and the render target's pixels, with and without the duplicate.
#include "scene_inspector.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "effect_history_core.h"   // the effect rule's list and share: the inspector names the pairs that drew and counts the pixels the rule hid
#include "engine_velocity_families.h"
#include "scene_inspector_core.h"
#include "ui_layer_math.h"

namespace edvr {
namespace {

using namespace inspect;

enum Phase : int { kIdle = 0, kArmed = 1, kCollecting = 2, kReading = 3 };
std::atomic<int> g_phase{kIdle};
std::atomic<uint32_t> g_seen{0};          // eye-0 (or unknown-eye) draws offered in the collecting frame
std::atomic<bool> g_inputsOffered{false};
bool g_installed = false;
bool g_flat = false;
SceneInspectEnv g_env;
uint32_t g_capId = 0;                      // the running (or last) capture's number; set at the boundary that starts it
uint32_t g_pendingCap = 0;                 // the armed capture's number and trigger, written by the press before it publishes Armed
char g_pendingTrigger[64] = {};

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T* get() const { return p; }
    T* operator->() const { return p; }
    T** put() { return &p; }
    T* release() {
        T* x = p;
        p = nullptr;
        return x;
    }
};
template <typename T, size_t N>
struct ComArray {
    T* a[N] = {};
    ComArray() = default;
    ComArray(const ComArray&) = delete;
    ComArray& operator=(const ComArray&) = delete;
    ~ComArray() {
        for (T* x : a)
            if (x) x->Release();
    }
};

// ---- the capture ----------------------------------------------------------------------------------------------------------------------

struct Cand {
    DrawRow row;
    DrawFact fact;
    Cls cls = Cls::Opaque;
    BlendDesc bd;
    bool depthWrite = false, depthTest = false;
    // routes, as booleans for the rules
    bool routeNone = false, uiTaken = false, holo = false, masked = false, keyed = false, engineSub = false;
    char maskSlot[24] = {};
    ID3D11Query* qBox = nullptr;
    ID3D11Query* qCtr = nullptr;
    bool boxAnswered = false, ctrAnswered = false;
    uint32_t clearEpoch = 0;       // the number of clears of its target seen before it
    const char* untested = "";     // why it was not measured, "" if it was queried
    bool neverDrawn = false;       // EDVR's verdict withholds the draw: it is not on screen, so it is out of every stack
};

struct Target {
    const void* res = nullptr;
    uint32_t fmt = 0, w = 0, h = 0, dsFmt = 0, dsW = 0, dsH = 0;
    float vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    Rect box;
    int32_t cx = 0, cy = 0;
    uint32_t draws = 0, wrote = 0;
    const void* dsRes = nullptr;   // the depth view's texture, compared with the scene depth the temporal pass reads
    Role role = Role::Unknown;     // set in the report, once the pass has said which textures DLSS sees
};

struct BoxFix {
    bool fixed = false;
    BoxInfo info;
    float u = 0.5f, v = 0.5f;      // the point in the image, 0..1
};

struct Staged {
    ID3D11Texture2D* tex[7] = {};   // colour, motion, depth, decision, coverage, bias, engine slots
    bool have[7] = {};
    Rect crop;
    bool offered = false, staged = false, read = false;
    uint32_t w = 0, h = 0, sceneFrame = 0;
    SceneInputsIn meta;             // the scalar parts only; the texture pointers are cleared
    std::string sync;
    // decoded
    std::vector<uint8_t> rgb;       // colour crop, 3 per pixel
    std::vector<float> mv, depth, dec;
    std::vector<uint8_t> cov, bias;
    bool haveCov = false, haveBias = false;
    // The engine records under the box: the slot target's x channel over the crop, and the pool as the pass indexes it (read whole: it is
    // 3 MB, and the slots in the box are not known until the crop is read).
    uint32_t slotFmt = 0;
    std::vector<float> slotX;
    ID3D11Buffer* poolStaging = nullptr;
    uint32_t poolFirst = 0, poolCount = 0, poolStride = 0;
    std::vector<uint32_t> poolWords;
    bool poolRead = false;
};

struct StagedOut {
    ID3D11Texture2D* tex[2] = {};   // dlss, final
    uint32_t fmt[2] = {};
    Rect crop;
    bool offered = false, staged = false, read = false;
    SceneOutputsIn meta;
    std::vector<uint8_t> rgb[2];
    uint32_t cw = 0, ch = 0;
    bool decoded[2] = {};
};

std::vector<Cand> g_cands;
std::vector<Target> g_targets;
std::vector<uint32_t> g_clearAt;          // for each clear seen: the cand index it precedes, and the target in g_clearRt
std::vector<const void*> g_clearRt;
BoxFix g_box;
FrameCounts g_counts;
Staged g_in;
StagedOut g_out;
uint32_t g_readTicks = 0;
uint32_t g_startFrame = 0;
std::string g_trigger, g_inputsWhy;
ProducerTotals g_prodAtStart, g_prodAtEnd;   // the kinematic producers' totals when the capture frame began and when it ended
ID3D11DeviceContext* g_ctx = nullptr;   // AddRef'd while a query or a staging copy is outstanding
bool g_queryFailed = false;
HRESULT g_queryFailHr = S_OK;
uint32_t g_faults = 0;

std::map<std::string, ID3D11RasterizerState*> g_rsClones;
std::map<std::string, ID3D11BlendState*> g_bsClones;
std::map<std::string, ID3D11DepthStencilState*> g_dsClones;

void say(const char* line) { Log::get().note("%s", line); }

void refuse(Refusal r, const char* detail = nullptr) {
    Log::get().note("scene inspect: refused (%s)%s%s", refusalText(r), detail ? ": " : "", detail ? detail : "");
}
void holdCtx(ID3D11DeviceContext* ctx) {
    if (!g_ctx && ctx) {
        g_ctx = ctx;
        ctx->AddRef();
    }
}
void copyText(char* dst, size_t cap, const char* src) { std::snprintf(dst, cap, "%s", src ? src : ""); }
void copyToken(char* dst, size_t cap, const char* src) {
    copyText(dst, cap, src);
    for (char* c = dst; *c; ++c)
        if (*c == ' ') *c = '_';
}
const char* fmtName(uint32_t f, char* buf, size_t cap) { return formatName(f, buf, cap); }

// ---- the duplicate's states ------------------------------------------------------------------------------------------------------------

ID3D11RasterizerState* rasterizerClone(ID3D11Device* dev, const D3D11_RASTERIZER_DESC& src) {
    D3D11_RASTERIZER_DESC d = src;
    d.ScissorEnable = TRUE;
    const std::string key(reinterpret_cast<const char*>(&d), sizeof(d));
    const auto it = g_rsClones.find(key);
    if (it != g_rsClones.end()) return it->second;
    ID3D11RasterizerState* out = nullptr;
    if (FAILED(dev->CreateRasterizerState(&d, &out)) || !out) return nullptr;
    g_rsClones.emplace(key, out);
    return out;
}
ID3D11BlendState* blendClone(ID3D11Device* dev, const D3D11_BLEND_DESC& src) {
    D3D11_BLEND_DESC d = src;
    for (D3D11_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget) rt.RenderTargetWriteMask = 0;
    const std::string key(reinterpret_cast<const char*>(&d), sizeof(d));
    const auto it = g_bsClones.find(key);
    if (it != g_bsClones.end()) return it->second;
    ID3D11BlendState* out = nullptr;
    if (FAILED(dev->CreateBlendState(&d, &out)) || !out) return nullptr;
    g_bsClones.emplace(key, out);
    return out;
}
ID3D11DepthStencilState* depthClone(ID3D11Device* dev, const D3D11_DEPTH_STENCIL_DESC& src) {
    D3D11_DEPTH_STENCIL_DESC d = src;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilWriteMask = 0;
    const std::string key(reinterpret_cast<const char*>(&d), sizeof(d));
    const auto it = g_dsClones.find(key);
    if (it != g_dsClones.end()) return it->second;
    ID3D11DepthStencilState* out = nullptr;
    if (FAILED(dev->CreateDepthStencilState(&d, &out)) || !out) return nullptr;
    g_dsClones.emplace(key, out);
    return out;
}
D3D11_RASTERIZER_DESC defaultRasterizer() {
    D3D11_RASTERIZER_DESC d{};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = D3D11_CULL_BACK;
    d.DepthClipEnable = TRUE;
    return d;
}
D3D11_BLEND_DESC defaultBlend() {
    D3D11_BLEND_DESC d{};
    for (D3D11_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget) {
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    return d;
}
D3D11_DEPTH_STENCIL_DESC defaultDepthStencil() {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_LESS;
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0xFF;
    d.FrontFace.StencilFunc = d.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp = d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    d.BackFace = d.FrontFace;
    return d;
}
void releaseClones() {
    for (auto& kv : g_rsClones)
        if (kv.second) kv.second->Release();
    g_rsClones.clear();
    for (auto& kv : g_bsClones)
        if (kv.second) kv.second->Release();
    g_bsClones.clear();
    for (auto& kv : g_dsClones)
        if (kv.second) kv.second->Release();
    g_dsClones.clear();
}

// ---- the draw's routes, as the row text and the rules' booleans ---------------------------------------------------------------------------

void fillRoutes(Cand& c, const SceneDrawInfo& i) {
    DrawRow& r = c.row;
    copyToken(r.verdict, sizeof(r.verdict), i.verdict);
    if (i.uiFamily > 0 && i.uiFamily < static_cast<int>(UiLayerFamily::kCount))
        copyToken(r.uiFamily, sizeof(r.uiFamily), uiLayerFamilyName(static_cast<UiLayerFamily>(i.uiFamily)));
    else
        copyText(r.uiFamily, sizeof(r.uiFamily), "-");
    if (!i.uiDecided) copyText(r.uiDecision, sizeof(r.uiDecision), "none");
    else if (i.uiTaken) copyText(r.uiDecision, sizeof(r.uiDecision), "taken");
    else std::snprintf(r.uiDecision, sizeof(r.uiDecision), "left:%s", uiLayerDecisionKey(static_cast<UiLayerDecision>(i.uiDecision)));
    copyText(r.holo, sizeof(r.holo), i.holoMatched ? (i.holoWorld ? "world" : "cockpit") : "-");
    if (!i.udCoverage) {
        copyText(r.mask, sizeof(r.mask), "-");
    } else if (!i.udWantMask) {
        std::snprintf(r.mask, sizeof(r.mask), "%s:nomask", blend::maskSlotName(i.udMaskSlot));
    } else {
        int f = 1, a = 1;
        blend::maskQuanta(i.udMaskSlot, &f, &a);
        std::snprintf(r.mask, sizeof(r.mask), "%s:m%d/%d", blend::maskSlotName(i.udMaskSlot), f, a);
    }
    copyText(r.engine, sizeof(r.engine), i.engineSubstituted ? "sub" : "plain");
    namespace ev = engine_velocity_family;
    const int fam = ev::familyForProfile(r.vs, false);
    bool selfMarking = false;
    if (fam >= 0 && ev::keyedPs(fam, r.ps, false)) {
        std::snprintf(r.keyed, sizeof(r.keyed), "%s#%d", ev::skinPair(r.vs, r.ps) ? "skinned" : "rigid", fam);
        c.keyed = true;
    } else if (ev::selfMarkingPair(r.vs, r.ps)) {
        copyText(r.keyed, sizeof(r.keyed), "self-marking");
        c.keyed = true;
        selfMarking = true;
    } else if (fam >= 0) {
        std::snprintf(r.keyed, sizeof(r.keyed), "vs-only#%d", fam);
    } else {
        copyText(r.keyed, sizeof(r.keyed), "-");
    }
    // The motion this draw's pixels get from EDVR: an exact per-object source only when its shader pair is keyed AND the pool shaders were
    // substituted for it; everything else rides the camera term of whatever lies behind it.
    if (selfMarking) copyText(r.motion, sizeof(r.motion), "self-marking");
    else if (c.keyed && i.engineSubstituted) copyText(r.motion, sizeof(r.motion), "engine-record");
    else copyText(r.motion, sizeof(r.motion), "camera-row");
    c.uiTaken = i.uiTaken;
    c.holo = i.holoMatched;
    c.masked = i.udCoverage && i.udWantMask;
    copyText(c.maskSlot, sizeof(c.maskSlot), blend::maskSlotName(i.udMaskSlot));
    c.engineSub = i.engineSubstituted;
    c.routeNone = std::strcmp(r.verdict, "none") == 0 && r.uiFamily[0] == '-' && !i.uiDecided && !i.holoMatched && !i.udCoverage && !c.keyed &&
                  !i.engineSubstituted;
}

int targetIndex(const void* res) {
    for (size_t i = 0; i < g_targets.size(); ++i)
        if (g_targets[i].res == res) return static_cast<int>(i);
    return -1;
}

void fixBox(int eye, float vpX, float vpY, float vpW, float vpH, uint32_t ordinal) {
    g_box = BoxFix{};
    BoxInfo& b = g_box.info;
    b.fixedAtOrdinal = ordinal;
    b.vpX = vpX;
    b.vpY = vpY;
    b.vpW = vpW;
    b.vpH = vpH;
    b.side = kBox;
    float fr[4] = {}, rot[9] = {};
    bool haveRot = false;
    bool ok = false;
    if (g_env.eyeGeometry) {
        // eyeToHead is only trusted when the runtime handed one back (the callee leaves it untouched otherwise: start from the identity)
        rot[0] = rot[4] = rot[8] = 1.0f;
        ok = g_env.eyeGeometry(eye >= 0 ? static_cast<uint32_t>(eye) : 0u, fr, rot);
        haveRot = ok;
    }
    Centre c;
    if (ok) {
        Frustum f;
        f.left = fr[0];
        f.right = fr[1];
        f.down = fr[2];
        f.up = fr[3];
        c = headForwardToImage(f, haveRot ? rot : nullptr);
    }
    if (ok && c.ok) {
        b.fromGeometry = true;
        b.inside = c.inside;
        b.cantDeg = c.cantDeg;
        g_box.u = std::min(0.9999f, std::max(0.0f, c.u));
        g_box.v = std::min(0.9999f, std::max(0.0f, c.v));
        b.u = c.u;
        b.v = c.v;
    } else {
        g_box.u = g_box.v = 0.5f;
        b.u = b.v = 0.5f;
        b.inside = true;
    }
    b.centreOffsetPx = std::sqrt((g_box.u - 0.5f) * (g_box.u - 0.5f) * vpW * vpW + (g_box.v - 0.5f) * (g_box.v - 0.5f) * vpH * vpH);
    g_box.fixed = true;
}

// ---- one draw -----------------------------------------------------------------------------------------------------------------------------

void drawImpl(ID3D11DeviceContext* ctx, const SceneDrawInfo& info, SceneReplayFn replay, void* arg) {
    if (!ctx) return;
    FrameCounts& c = g_counts;
    if (c.gameFrame == 0) c.gameFrame = info.gameFrame;
    ++c.eyeDraws;

    ComArray<ID3D11RenderTargetView, 8> rtvs;
    ComArray<ID3D11UnorderedAccessView, 64> uavs;
    Com<ID3D11DepthStencilView> dsv;
    // 64 unordered-access slots exist from feature level 11.1; an 11.0 device has 8 and the debug layer refuses a longer list.
    Com<ID3D11Device> devFl;
    ctx->GetDevice(devFl.put());
    const UINT nUav = devFl.get() && devFl->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(8, rtvs.a, dsv.put(), 0, nUav, uavs.a);
    if (!rtvs.a[0]) return;
    Com<ID3D11Resource> res;
    rtvs.a[0]->GetResource(res.put());
    Com<ID3D11Texture2D> tex;
    if (!res.get() || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.put()))) || !tex.get()) return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const int eye = g_env.eyeOfTarget ? g_env.eyeOfTarget(res.get()) : -1;
    if (eye == 0) ++c.eye0;
    else if (eye == 1) ++c.eye1;
    else ++c.eyeUnknown;
    if (eye == 1) return;   // the left eye is enough: the right eye's pass is the same draws
    g_seen.fetch_add(1, std::memory_order_relaxed);
    if (g_cands.size() >= kMaxDraws) {
        ++c.overCap;
        return;
    }

    D3D11_VIEWPORT vps[16] = {};
    UINT nVp = 16;
    ctx->RSGetViewports(&nVp, vps);
    if (nVp == 0) {
        ++c.untestedState;
        return;
    }
    if (!g_box.fixed) fixBox(eye, vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height, info.ordinal);

    int ti = targetIndex(res.get());
    if (ti < 0) {
        Target t;
        t.res = res.get();
        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        rtvs.a[0]->GetDesc(&rvd);
        t.fmt = static_cast<uint32_t>(rvd.Format != DXGI_FORMAT_UNKNOWN ? rvd.Format : td.Format);
        t.w = td.Width;
        t.h = td.Height;
        t.vpX = vps[0].TopLeftX;
        t.vpY = vps[0].TopLeftY;
        t.vpW = vps[0].Width;
        t.vpH = vps[0].Height;
        if (dsv.get()) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
            dsv->GetDesc(&dvd);
            t.dsFmt = static_cast<uint32_t>(dvd.Format);
            Com<ID3D11Resource> dres;
            dsv->GetResource(dres.put());
            t.dsRes = dres.get();   // an address to compare; the texture outlives the draw that bound it
            Com<ID3D11Texture2D> dtex;
            if (dres.get() && SUCCEEDED(dres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(dtex.put()))) && dtex.get()) {
                D3D11_TEXTURE2D_DESC dd{};
                dtex->GetDesc(&dd);
                t.dsW = dd.Width;
                t.dsH = dd.Height;
            }
        }
        float px = 0.0f, py = 0.0f;
        imagePointToTarget(g_box.u, g_box.v, t.vpX, t.vpY, t.vpW, t.vpH, &px, &py);
        if (!boxAround(px, py, kBox, t.w, t.h, &t.box)) {
            ++c.untestedState;
            return;
        }
        centrePixel(px, py, t.box, &t.cx, &t.cy);
        g_targets.push_back(t);
        ti = static_cast<int>(g_targets.size()) - 1;
    }
    Target& tg = g_targets[static_cast<size_t>(ti)];
    ++tg.draws;

    // The state the draw runs with. A null state is D3D11's default.
    Com<ID3D11RasterizerState> rs;
    ctx->RSGetState(rs.put());
    Com<ID3D11BlendState> bs;
    FLOAT bf[4] = {};
    UINT bmask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(bs.put(), bf, &bmask);
    Com<ID3D11DepthStencilState> ds;
    UINT dref = 0;
    ctx->OMGetDepthStencilState(ds.put(), &dref);
    D3D11_RASTERIZER_DESC rsd = defaultRasterizer();
    D3D11_BLEND_DESC bsd = defaultBlend();
    D3D11_DEPTH_STENCIL_DESC dsd = defaultDepthStencil();
    if (rs.get()) {
        std::memset(&rsd, 0, sizeof(rsd));
        rs->GetDesc(&rsd);
    }
    if (bs.get()) {
        std::memset(&bsd, 0, sizeof(bsd));
        bs->GetDesc(&bsd);
    }
    if (ds.get()) {
        std::memset(&dsd, 0, sizeof(dsd));
        ds->GetDesc(&dsd);
    }
    const D3D11_RENDER_TARGET_BLEND_DESC& rt0 = bsd.RenderTarget[0];

    g_cands.emplace_back();
    Cand& cand = g_cands.back();
    DrawRow& row = cand.row;
    row.idx = static_cast<uint32_t>(g_cands.size() - 1);
    row.rt = static_cast<uint32_t>(ti);
    row.eye = eye;
    row.order = info.ordinal;
    row.gameFrame = info.gameFrame;
    row.vs = info.vs;
    row.ps = info.ps;
    row.kind = info.kind;
    row.count = info.count;
    row.instances = info.instances;
    UINT topo = 0;
    D3D11_PRIMITIVE_TOPOLOGY pt = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&pt);
    topo = static_cast<UINT>(pt);
    row.topology = topo;

    BlendDesc& bd = cand.bd;
    bd.enable = rt0.BlendEnable != FALSE;
    bd.src = static_cast<uint32_t>(rt0.SrcBlend);
    bd.dst = static_cast<uint32_t>(rt0.DestBlend);
    bd.op = static_cast<uint32_t>(rt0.BlendOp);
    bd.srcA = static_cast<uint32_t>(rt0.SrcBlendAlpha);
    bd.dstA = static_cast<uint32_t>(rt0.DestBlendAlpha);
    bd.opA = static_cast<uint32_t>(rt0.BlendOpAlpha);
    bd.writeMask = rt0.RenderTargetWriteMask;
    bd.alphaToCoverage = bsd.AlphaToCoverageEnable != FALSE;
    DepthDesc dd;
    dd.dsvBound = dsv.get() != nullptr;
    dd.test = dsd.DepthEnable != FALSE;
    dd.func = static_cast<uint32_t>(dsd.DepthFunc);
    dd.write = dsd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.stencil = dsd.StencilEnable != FALSE;
    cand.depthWrite = blend::depthWrites(dd);
    cand.depthTest = dd.dsvBound && dd.test;
    cand.cls = classify(bd);
    blend::formatBlend(row.blend, sizeof(row.blend), bd);
    blend::formatDepth(row.depth, sizeof(row.depth), dd);
    StencilDesc sd;
    sd.enable = dsd.StencilEnable != FALSE;
    sd.ref = dref;
    sd.readMask = dsd.StencilReadMask;
    sd.writeMask = dsd.StencilWriteMask;
    sd.func = static_cast<uint32_t>(dsd.FrontFace.StencilFunc);
    sd.passOp = static_cast<uint32_t>(dsd.FrontFace.StencilPassOp);
    sd.failOp = static_cast<uint32_t>(dsd.FrontFace.StencilFailOp);
    sd.depthFailOp = static_cast<uint32_t>(dsd.FrontFace.StencilDepthFailOp);
    formatStencil(row.stencil, sizeof(row.stencil), sd);
    copyText(row.cull, sizeof(row.cull), cullName(static_cast<uint32_t>(rsd.CullMode)));
    copyText(row.cls, sizeof(row.cls), clsName(cand.cls));
    uint32_t rtCount = 0;
    for (ID3D11RenderTargetView* v : rtvs.a)
        if (v) ++rtCount;
    row.rtCount = rtCount;
    char fb[24];
    copyText(row.rt0, sizeof(row.rt0), fmtName(tg.fmt, fb, sizeof(fb)));
    row.rt0W = td.Width;
    row.rt0H = td.Height;
    if (dsv.get()) copyText(row.ds, sizeof(row.ds), fmtName(tg.dsFmt, fb, sizeof(fb)));
    else copyText(row.ds, sizeof(row.ds), "none");
    row.dsW = tg.dsW;
    row.dsH = tg.dsH;
    // The pixel shader's first sampled textures: what a hologram or an advert draws from.
    {
        ComArray<ID3D11ShaderResourceView, 8> srvs;
        ctx->PSGetShaderResources(0, 8, srvs.a);
        std::string s;
        int shown = 0, total = 0;
        for (int k = 0; k < 8; ++k) {
            if (!srvs.a[k]) continue;
            ++total;
            if (shown >= 4) continue;
            Com<ID3D11Resource> r;
            srvs.a[k]->GetResource(r.put());
            char item[64] = "?";
            Com<ID3D11Texture2D> t2;
            if (r.get() && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(t2.put()))) && t2.get()) {
                D3D11_TEXTURE2D_DESC d2{};
                t2->GetDesc(&d2);
                char f2[24];
                std::snprintf(item, sizeof(item), "%d:%s %ux%u", k, fmtName(static_cast<uint32_t>(d2.Format), f2, sizeof(f2)), d2.Width, d2.Height);
            } else {
                std::snprintf(item, sizeof(item), "%d:buffer", k);
            }
            if (!s.empty()) s += ",";
            s += item;
            ++shown;
        }
        if (total > shown) {
            char more[16];
            std::snprintf(more, sizeof(more), ",+%d", total - shown);
            s += more;
        }
        copyText(row.srv, sizeof(row.srv), s.c_str());
    }
    fillRoutes(cand, info);
    cand.fact.rt = row.rt;
    cand.fact.occluder = isOccluder(cand.cls, cand.depthWrite, bd);
    cand.fact.depthWrite = cand.depthWrite;
    cand.clearEpoch = static_cast<uint32_t>(g_clearAt.size());
    if (info.verdict && std::strcmp(info.verdict, "skip") == 0) {
        // Offered at vscreen's skip return: the game's draw is never issued, so it wrote nothing and must not bury what is under it.
        cand.neverDrawn = true;
        cand.untested = "withheld";
        ++c.withheld;
        return;
    }

    // The duplicate. Anything that stops it leaves the draw unmeasured (box=?), never absent.
    for (ID3D11UnorderedAccessView* u : uavs.a)
        if (u) {
            ++c.untestedState;
            cand.untested = "uav";
            return;   // a UAV write would be done twice
        }
    ComArray<ID3D11Buffer, 4> so;
    ctx->SOGetTargets(4, so.a);
    for (ID3D11Buffer* b : so.a)
        if (b) {
            ++c.untestedState;
            cand.untested = "stream-output";
            return;
        }
    if (!replay || !g_env.setBlendRaw) {
        ++c.noReplay;
        cand.untested = "no-replay";
        return;
    }
    Com<ID3D11Device> dev;
    ctx->GetDevice(dev.put());
    if (!dev.get()) {
        ++c.untestedState;
        cand.untested = "no-device";
        return;
    }
    ID3D11RasterizerState* rsC = rasterizerClone(dev.get(), rsd);
    ID3D11BlendState* bsC = blendClone(dev.get(), bsd);
    ID3D11DepthStencilState* dsC = depthClone(dev.get(), dsd);
    if (!rsC || !bsC || !dsC) {
        ++c.untestedState;
        cand.untested = "no-state-clone";
        return;
    }
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    Com<ID3D11Query> qa, qb;
    const HRESULT h1 = dev->CreateQuery(&qd, qa.put());
    const HRESULT h2 = FAILED(h1) ? h1 : dev->CreateQuery(&qd, qb.put());
    if (FAILED(h1) || FAILED(h2) || !qa.get() || !qb.get()) {
        if (!g_queryFailed) {
            g_queryFailed = true;
            g_queryFailHr = FAILED(h1) ? h1 : h2;
        }
        ++c.untestedState;
        cand.untested = "no-query";
        return;
    }
    UINT scN = 16;
    D3D11_RECT sc[16] = {};
    ctx->RSGetScissorRects(&scN, sc);
    const D3D11_RECT boxRect = {tg.box.x0, tg.box.y0, tg.box.x1, tg.box.y1};
    const D3D11_RECT ctrRect = {tg.cx, tg.cy, tg.cx + 1, tg.cy + 1};
    ctx->RSSetState(rsC);
    ctx->RSSetScissorRects(1, &boxRect);
    g_env.setBlendRaw(ctx, bsC, bf, bmask);   // past the binding shadow: the duplicate is not the game's bind
    ctx->OMSetDepthStencilState(dsC, dref);
    ctx->Begin(qa.get());
    replay(arg);
    ctx->End(qa.get());
    ctx->RSSetScissorRects(1, &ctrRect);
    ctx->Begin(qb.get());
    replay(arg);
    ctx->End(qb.get());
    ctx->RSSetState(rs.get());
    ctx->RSSetScissorRects(scN, sc);
    g_env.setBlendRaw(ctx, bs.get(), bf, bmask);
    ctx->OMSetDepthStencilState(ds.get(), dref);
    cand.qBox = qa.release();
    cand.qCtr = qb.release();
    ++c.tested;
    holdCtx(ctx);
}

// ---- answers --------------------------------------------------------------------------------------------------------------------------------

// The kinematic producers' totals (the environment's); have = false when the module has no such source.
ProducerTotals readProducers() {
    ProducerTotals t;
    uint64_t v[5] = {};
    if (g_env.producerTotals && g_env.producerTotals(v)) {
        t.have = true;
        t.emitCalls = v[0];
        t.emitItems = v[1];
        t.secondCalls = v[2];
        t.secondItems = v[3];
        t.primaryCalls = v[4];
    }
    return t;
}

void pollQueries() {
    if (!g_ctx) return;
    for (Cand& c : g_cands) {
        UINT64 samples = 0;
        if (c.qBox && !c.boxAnswered && g_ctx->GetData(c.qBox, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            c.boxAnswered = true;
            c.fact.box = static_cast<int64_t>(samples);
        }
        if (c.qCtr && !c.ctrAnswered && g_ctx->GetData(c.qCtr, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            c.ctrAnswered = true;
            c.fact.ctr = static_cast<int64_t>(samples);
        }
    }
}
uint32_t outstandingQueries() {
    uint32_t n = 0;
    for (const Cand& c : g_cands) {
        if (c.qBox && !c.boxAnswered) ++n;
        if (c.qCtr && !c.ctrAnswered) ++n;
    }
    return n;
}

// One crop copied to a staging texture. false (and nothing kept) when the source is absent or not the expected format (expect 0 = any).
bool stageOne(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, DXGI_FORMAT expect, const Rect& crop, ID3D11Texture2D** out,
              uint32_t* fmtOut = nullptr) {
    if (!src) return false;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (expect != DXGI_FORMAT_UNKNOWN && d.Format != expect) return false;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Width = crop.width();
    sd.Height = crop.height();
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (crop.x1 > static_cast<int32_t>(d.Width) || crop.y1 > static_cast<int32_t>(d.Height)) return false;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, out)) || !*out) {
        *out = nullptr;
        return false;
    }
    const D3D11_BOX box{static_cast<UINT>(crop.x0), static_cast<UINT>(crop.y0), 0, static_cast<UINT>(crop.x1), static_cast<UINT>(crop.y1), 1};
    ctx->CopySubresourceRegion(*out, 0, 0, 0, 0, src, 0, &box);
    if (fmtOut) *fmtOut = static_cast<uint32_t>(d.Format);
    return true;
}

// Maps every staged texture of the inputs (all or none), decodes the crop into plain arrays, and unmaps. false while any copy is unfinished.
bool tryReadInputs(Staged& s) {
    if (!g_ctx) return false;
    D3D11_MAPPED_SUBRESOURCE m[7] = {};
    bool mapped[7] = {};
    D3D11_MAPPED_SUBRESOURCE mp{};
    bool poolMapped = false;
    bool pending = false;
    for (int k = 0; k < 7 && !pending; ++k) {
        if (!s.have[k]) continue;
        const HRESULT hr = g_ctx->Map(s.tex[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[k]);
        if (hr == S_OK) mapped[k] = true;
        else pending = true;
    }
    if (!pending && s.poolStaging) {
        if (g_ctx->Map(s.poolStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp) == S_OK) poolMapped = true;
        else pending = true;
    }
    if (!pending) {
        const uint32_t cw = s.crop.width(), ch = s.crop.height();
        const size_t n = static_cast<size_t>(cw) * ch;
        if (s.have[0]) {
            s.rgb.assign(n * 3, 0);
            for (uint32_t y = 0; y < ch; ++y)
                for (uint32_t x = 0; x < cw; ++x) {
                    const uint8_t* p = static_cast<const uint8_t*>(m[0].pData) + static_cast<size_t>(y) * m[0].RowPitch + static_cast<size_t>(x) * 4;
                    uint8_t* o = &s.rgb[(static_cast<size_t>(y) * cw + x) * 3];
                    o[0] = p[0];
                    o[1] = p[1];
                    o[2] = p[2];
                }
        }
        if (s.have[1]) {
            s.mv.assign(n * 2, 0.0f);
            for (uint32_t y = 0; y < ch; ++y)
                for (uint32_t x = 0; x < cw; ++x) {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m[1].pData) + static_cast<size_t>(y) * m[1].RowPitch) + x * 2;
                    s.mv[(static_cast<size_t>(y) * cw + x) * 2] = halfToFloat(p[0]);
                    s.mv[(static_cast<size_t>(y) * cw + x) * 2 + 1] = halfToFloat(p[1]);
                }
        }
        if (s.have[2]) {
            s.depth.assign(n, 0.0f);
            for (uint32_t y = 0; y < ch; ++y)
                std::memcpy(&s.depth[static_cast<size_t>(y) * cw], static_cast<const uint8_t*>(m[2].pData) + static_cast<size_t>(y) * m[2].RowPitch, cw * 4);
        }
        if (s.have[3]) {
            s.dec.assign(n * 4, 0.0f);
            for (uint32_t y = 0; y < ch; ++y)
                std::memcpy(&s.dec[static_cast<size_t>(y) * cw * 4], static_cast<const uint8_t*>(m[3].pData) + static_cast<size_t>(y) * m[3].RowPitch, cw * 16);
        }
        if (s.have[4]) {
            s.cov.assign(n, 0);
            for (uint32_t y = 0; y < ch; ++y)
                std::memcpy(&s.cov[static_cast<size_t>(y) * cw], static_cast<const uint8_t*>(m[4].pData) + static_cast<size_t>(y) * m[4].RowPitch, cw);
        }
        if (s.have[5]) {
            s.bias.assign(n, 0);
            for (uint32_t y = 0; y < ch; ++y)
                std::memcpy(&s.bias[static_cast<size_t>(y) * cw], static_cast<const uint8_t*>(m[5].pData) + static_cast<size_t>(y) * m[5].RowPitch, cw);
        }
        s.haveCov = s.have[4];
        s.haveBias = s.have[5];
        if (s.have[6] && (s.slotFmt == 16 || s.slotFmt == 2)) {   // DXGI R32G32_FLOAT, R32G32B32A32_FLOAT: x is the first float of each texel
            const uint32_t bpp = s.slotFmt == 16 ? 8u : 16u;
            s.slotX.assign(n, -1.0f);
            for (uint32_t y = 0; y < ch; ++y)
                for (uint32_t x = 0; x < cw; ++x)
                    std::memcpy(&s.slotX[static_cast<size_t>(y) * cw + x], static_cast<const uint8_t*>(m[6].pData) + static_cast<size_t>(y) * m[6].RowPitch + static_cast<size_t>(x) * bpp, 4);
        }
        if (poolMapped && s.poolStride >= kEngineRecordWords * 4u) {
            const size_t words = static_cast<size_t>(s.poolCount) * (s.poolStride / 4u);
            s.poolWords.resize(words);
            std::memcpy(s.poolWords.data(), static_cast<const uint8_t*>(mp.pData) + static_cast<size_t>(s.poolFirst) * s.poolStride, words * 4u);
            s.poolRead = true;
        }
    }
    if (poolMapped) g_ctx->Unmap(s.poolStaging, 0);
    for (int k = 0; k < 7; ++k)
        if (mapped[k]) g_ctx->Unmap(s.tex[k], 0);
    if (!pending) s.read = true;
    return !pending;
}

bool tryReadOutputs(StagedOut& o) {
    if (!g_ctx) return false;
    D3D11_MAPPED_SUBRESOURCE m[2] = {};
    bool mapped[2] = {};
    bool pending = false;
    for (int k = 0; k < 2 && !pending; ++k) {
        if (!o.tex[k]) continue;
        const HRESULT hr = g_ctx->Map(o.tex[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[k]);
        if (hr == S_OK) mapped[k] = true;
        else pending = true;
    }
    if (!pending) {
        const uint32_t cw = o.crop.width(), ch = o.crop.height();
        for (int k = 0; k < 2; ++k) {
            if (!mapped[k]) continue;
            const uint32_t bpp = texelBytes(o.fmt[k]);
            if (!bpp) continue;
            o.rgb[k].assign(static_cast<size_t>(cw) * ch * 3, 0);
            bool ok = true;
            for (uint32_t y = 0; y < ch && ok; ++y)
                for (uint32_t x = 0; x < cw && ok; ++x) {
                    const uint8_t* p = static_cast<const uint8_t*>(m[k].pData) + static_cast<size_t>(y) * m[k].RowPitch + static_cast<size_t>(x) * bpp;
                    ok = texelToRgb(o.fmt[k], p, &o.rgb[k][(static_cast<size_t>(y) * cw + x) * 3]);
                }
            o.decoded[k] = ok;
        }
        o.cw = cw;
        o.ch = ch;
    }
    for (int k = 0; k < 2; ++k)
        if (mapped[k]) g_ctx->Unmap(o.tex[k], 0);
    if (!pending) o.read = true;
    return !pending;
}

void releaseAll() {
    for (Cand& c : g_cands) {
        if (c.qBox) c.qBox->Release();
        if (c.qCtr) c.qCtr->Release();
    }
    g_cands.clear();
    g_cands.shrink_to_fit();
    g_targets.clear();
    g_clearAt.clear();
    g_clearRt.clear();
    for (ID3D11Texture2D*& t : g_in.tex)
        if (t) {
            t->Release();
            t = nullptr;
        }
    for (ID3D11Texture2D*& t : g_out.tex)
        if (t) {
            t->Release();
            t = nullptr;
        }
    if (g_in.poolStaging) {
        g_in.poolStaging->Release();
        g_in.poolStaging = nullptr;
    }
    g_in = Staged{};
    g_out = StagedOut{};
    g_box = BoxFix{};
    g_counts = FrameCounts{};
    releaseClones();
    if (g_ctx) {
        g_ctx->Release();
        g_ctx = nullptr;
    }
    g_seen.store(0, std::memory_order_relaxed);
    g_inputsOffered.store(false, std::memory_order_relaxed);
}

// ---- the report -------------------------------------------------------------------------------------------------------------------------------

std::string stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char b[40];
    std::snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
bool writeFile(const std::wstring& path, const void* data, size_t n) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const size_t w = n ? std::fwrite(data, 1, n, f) : 0;
    std::fclose(f);
    return w == n;
}

// The decision texture etc. at one input pixel (coordinates in the input, which the crop covers), as the facts the rules read.
PixelFacts pixelAt(int x, int y) {
    PixelFacts p;
    const Staged& s = g_in;
    p.depth = std::numeric_limits<float>::quiet_NaN();
    if (!s.read || s.dec.empty()) return p;
    const int lx = x - s.crop.x0, ly = y - s.crop.y0;
    const int cw = static_cast<int>(s.crop.width()), ch = static_cast<int>(s.crop.height());
    if (lx < 0 || ly < 0 || lx >= cw || ly >= ch) return p;
    const size_t i = static_cast<size_t>(ly) * cw + lx;
    p.valid = true;
    p.mvx = s.dec[i * 4];
    p.mvy = s.dec[i * 4 + 1];
    p.flags = blend::decodeFlags(s.dec[i * 4 + 3]);
    if (!s.depth.empty()) p.depth = s.depth[i];
    if (!s.mv.empty()) {
        p.haveDlss = true;
        p.dlx = s.mv[i * 2];
        p.dly = s.mv[i * 2 + 1];
    }
    if (s.haveCov) p.coverage = s.cov[i];
    if (s.haveBias) p.bias = s.bias[i];
    if (!s.slotX.empty()) p.slot = slotFromCode(s.slotX[i]);
    p.far = true;
    if (std::isfinite(p.depth)) {
        float m = 0.0f;
        if (depthToMetres(p.depth, s.meta.knobs[0], s.meta.knobs[2], &m)) {
            p.far = false;
            p.metres = m;
        }
    }
    CamTerm w;
    w.w = s.w;
    w.h = s.h;
    std::memcpy(w.tanNow, s.meta.tanNow, sizeof(w.tanNow));
    std::memcpy(w.tanPrev, s.meta.tanPrev, sizeof(w.tanPrev));
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) w.rows[r][k] = s.meta.worldRows[r][k];
    for (int k = 0; k < 3; ++k) w.tv[k] = s.meta.tvCam[k];
    w.valid = s.meta.tvCam[3] != 0.0f;
    CamTerm h = w;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) h.rows[r][k] = s.meta.headRows[r][k];
    for (int k = 0; k < 3; ++k) h.tv[k] = s.meta.tvUsed[k];
    h.valid = true;
    p.haveWorld = expectedMotion(w, static_cast<float>(x), static_cast<float>(y), p.far, p.metres, &p.expWx, &p.expWy);
    p.haveHead = expectedMotion(h, static_cast<float>(x), static_cast<float>(y), p.far, p.metres, &p.expHx, &p.expHy);
    return p;
}

bool writePng(const std::wstring& path, const uint8_t* rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> png;
    if (!encodePngRgb(rgb, w, h, &png)) return false;
    return writeFile(path, png.data(), png.size());
}

void writeReport() {
    const uint32_t capId = g_capId;
    const std::string st = stamp();
    FrameCounts& c = g_counts;
    c.targets = static_cast<uint32_t>(g_targets.size());
    char line[kLineBuf];

    // Draws before the latest clear of their own target are not on screen: they are out of the stacks and the listing.
    std::vector<bool> cleared(g_cands.size(), false);
    for (size_t k = 0; k < g_clearAt.size(); ++k) {
        for (size_t i = 0; i < g_cands.size() && i < g_clearAt[k]; ++i)
            if (g_targets.size() > g_cands[i].row.rt && g_targets[g_cands[i].row.rt].res == g_clearRt[k]) cleared[i] = true;
    }
    std::vector<DrawFact> facts(g_cands.size());
    for (size_t i = 0; i < g_cands.size(); ++i) {
        facts[i] = g_cands[i].fact;
        if (cleared[i]) facts[i].box = facts[i].ctr = 0;
        Cand& cd = g_cands[i];
        if (!cd.untested[0] && cd.qBox && !cd.boxAnswered) {
            ++c.unanswered;
            facts[i].box = kUnknown;
        }
        if (cd.qCtr && !cd.ctrAnswered) facts[i].ctr = kUnknown;
        if (cd.untested[0]) facts[i].box = facts[i].ctr = cd.neverDrawn ? 0 : kUnknown;
    }
    std::vector<StatusOut> status(g_cands.size());
    classifyStatus(facts.data(), facts.size(), status.data());
    for (size_t i = 0; i < g_cands.size(); ++i) {
        DrawRow& r = g_cands[i].row;
        r.box = facts[i].box;
        r.ctr = facts[i].ctr;
        copyText(r.status, sizeof(r.status), statusName(status[i].status));
        switch (status[i].status) {
            case Status::Top: ++c.top; ++c.involved; ++g_targets[r.rt].wrote; break;
            case Status::Partial: ++c.partial; ++c.involved; ++g_targets[r.rt].wrote; break;
            case Status::Buried: ++c.buried; ++c.involved; ++g_targets[r.rt].wrote; break;
            case Status::Unmeasured: ++c.unmeasured; break;
            default: break;
        }
    }

    // The roles: which targets DLSS sees. Known only when the temporal pass offered its inputs, which name the texture the game submitted and the
    // scene depth it reads; without them no target is called an intermediate pass.
    const bool rolesKnown = g_in.staged && (g_in.meta.colourSource || g_in.meta.sceneDepth);
    bool colourSeen = false;
    for (Target& tg : g_targets) {
        const bool isColour = rolesKnown && g_in.meta.colourSource && tg.res == g_in.meta.colourSource;
        const bool sharesDepth = rolesKnown && g_in.meta.sceneDepth && tg.dsRes == g_in.meta.sceneDepth;
        tg.role = roleOf(rolesKnown, isColour, sharesDepth);
        colourSeen = colourSeen || isColour;
    }
    for (size_t i = 0; i < g_cands.size(); ++i) {
        const Role role = g_targets[g_cands[i].row.rt].role;
        copyText(g_cands[i].row.role, sizeof(g_cands[i].row.role), roleName(role));
        const Status st2 = status[i].status;
        if ((st2 == Status::Top || st2 == Status::Partial || st2 == Status::Buried) && roleIsDlssInput(role)) ++c.inDlssTargets;
    }

    // The scene target (the pixel rules and the depth owner apply to its draws): the targets that share the scene depth DLSS reads; with the roles
    // unknown, the one with a depth view that took the most draws into the box.
    int scene = -1;
    if (!rolesKnown)
        for (size_t t = 0; t < g_targets.size(); ++t)
            if (g_targets[t].dsFmt != 0 && (scene < 0 || g_targets[t].wrote > g_targets[static_cast<size_t>(scene)].wrote)) scene = static_cast<int>(t);
    auto sceneLike = [&](uint32_t rt) { return rolesKnown ? roleHasSceneDepth(g_targets[rt].role) : static_cast<int>(rt) == scene; };
    // The depth owner is the LAST draw that wrote depth at the centre pixel into the scene depth, whichever colour target it wrote: Elite's
    // opaque pass and its effects write different colour targets over one depth.
    int32_t depthOwner = -1;
    for (size_t i = 0; i < g_cands.size(); ++i)
        if (sceneLike(g_cands[i].row.rt) && facts[i].ctr > 0 && g_cands[i].depthWrite) depthOwner = static_cast<int32_t>(i);

    // The stacks, per target.
    std::vector<std::vector<int32_t>> stacks(g_targets.size());
    std::vector<int32_t> owner(g_targets.size(), -1);
    for (size_t t = 0; t < g_targets.size(); ++t) {
        std::vector<int32_t> idx(g_cands.size() + 1);
        int32_t own = -1;
        const size_t m = buildStack(facts.data(), facts.size(), static_cast<uint32_t>(t), idx.data(), idx.size(), &own);
        stacks[t].assign(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(m));
        owner[t] = own;
    }

    // The pixels (the DLSS inputs at the box and its centre).
    const bool inputsRead = g_in.read && !g_in.dec.empty();
    InputBox ib;
    SampleStats stats;
    std::vector<BoxSample> samples;
    PixelFacts centrePx, minPx;
    int centreX = 0, centreY = 0;
    if (inputsRead) {
        Rect b;
        const float icx = g_box.u * static_cast<float>(g_in.w), icy = g_box.v * static_cast<float>(g_in.h);
        boxAround(icx, icy, kBox, g_in.w, g_in.h, &b);
        centrePixel(icx, icy, b, &centreX, &centreY);
        ib.x0 = b.x0;
        ib.y0 = b.y0;
        ib.x1 = b.x1;
        ib.y1 = b.y1;
        ib.cx = centreX;
        ib.cy = centreY;
        ib.w = g_in.w;
        ib.h = g_in.h;
        for (int y = b.y0; y < b.y1; ++y)
            for (int x = b.x0; x < b.x1; ++x) {
                BoxSample s;
                s.x = x;
                s.y = y;
                s.px = pixelAt(x, y);
                s.valid = s.px.valid;
                accumulate(&stats, s);
                samples.push_back(s);
            }
        centrePx = pixelAt(centreX, centreY);
    }

    // The log block.
    std::snprintf(line, sizeof(line), "scene inspect: report cap=%u begin (trigger %s)", capId, g_trigger.c_str());
    say(line);
    if (c.eyeDraws == 0) refuse(Refusal::kNoEyePass);
    if (g_queryFailed) {
        char d[48];
        std::snprintf(d, sizeof(d), "CreateQuery 0x%08lX", static_cast<unsigned long>(g_queryFailHr));
        refuse(Refusal::kQueriesUnavailable, d);
    }
    if (c.unanswered) {
        char d[96];
        std::snprintf(d, sizeof(d), "%u queries, after %u frame boundaries", c.unanswered, g_readTicks);
        refuse(Refusal::kQueriesUnanswered, d);
    }
    if (!inputsRead) refuse(Refusal::kNoInputs, g_inputsWhy.empty() ? (g_in.offered ? "the staged copy had not finished when the report was written" : "the temporal pass offered none") : g_inputsWhy.c_str());
    if (g_faults) Log::get().note("scene inspect: %u draw offers faulted and were skipped.", g_faults);
    say(legendText());
    if (g_box.fixed) {
        formatBoxLine(line, sizeof(line), capId, g_box.info);
        say(line);
    }
    // The listing: the Top and Partial draws in issue order up to the cap; Buried ones are listed as occluded, Unmeasured ones separately.
    c.listed = std::min(c.top + c.partial, kMaxListed);
    c.truncated = (c.top + c.partial) - c.listed;
    formatFrameLine(line, sizeof(line), capId, c);
    say(line);
    if (c.eyeUnknown > 0)
        Log::get().note("scene inspect note: %u eye draws wrote targets the pass's eye table does not know (eye=-1 in their lines); they are taken for the left eye, "
                        "so a right-eye draw may share a stack with the left's. The table learns a target at the first draw the UI-depth pass classifies.",
                        c.eyeUnknown);
    if (c.eyeDraws > 0 && c.involved == 0 && c.unmeasured == 0) {
        formatEmptyBoxLine(line, sizeof(line), capId, c.tested);
        say(line);
    }
    for (size_t t = 0; t < g_targets.size(); ++t) {
        const Target& tg = g_targets[t];
        char f1[24], f2[24];
        formatTargetLine(line, sizeof(line), static_cast<uint32_t>(t), fmtName(tg.fmt, f1, sizeof(f1)), tg.w, tg.h,
                         tg.dsFmt ? fmtName(tg.dsFmt, f2, sizeof(f2)) : "none", tg.vpX, tg.vpY, tg.vpW, tg.vpH, tg.box, tg.cx, tg.cy, tg.draws, tg.wrote,
                         tg.role);
        say(line);
    }
    if (rolesKnown && !colourSeen)
        say("scene inspect note: no draw wrote the texture the game submitted to DLSS (it is composed outside the eye-sized draws); the DLSS targets named are the ones sharing the scene depth.");
    for (size_t t = 0; t < g_targets.size(); ++t) {
        if (stacks[t].empty() || g_targets[t].role == Role::Intermediate) continue;
        std::string s;
        char item[96];
        for (size_t k = 0; k < stacks[t].size(); ++k) {
            const Cand& cd = g_cands[static_cast<size_t>(stacks[t][k])];
            std::snprintf(item, sizeof(item), "%s#%d[%s%s%s]", k ? " < " : "", stacks[t][k], cd.row.cls, cd.depthWrite ? " depth-write" : " no-depth-write",
                          stacks[t][k] == owner[t] ? " DEPTH-OWNER" : "");
            s += item;
        }
        std::snprintf(line, sizeof(line), "scene inspect stack: rt=#%zu at the centre pixel, bottom to top: %s%s", t, s.c_str(),
                      stacks[t].size() && !g_cands[static_cast<size_t>(stacks[t][0])].fact.occluder ? " (base: whatever the target held before: a clear or an earlier pass)" : "");
        say(line);
    }

    // Per involved draw. The verdicts and the CSV in issue order; the lines in two groups, the draws in DLSS input targets first (all three lines),
    // then the intermediate passes' (draw and verdict only), so the cap never hides a draw DLSS sees behind the lighting and bloom passes.
    std::string csv = drawsCsvHeader();
    uint32_t occluded = 0, unmeasuredListed = 0;
    std::vector<VerdictOut> verdicts(g_cands.size());
    for (size_t i = 0; i < g_cands.size(); ++i) {
        Cand& cd = g_cands[i];
        const Status stt = status[i].status;
        if (stt == Status::Outside) continue;
        VerdictIn vin;
        vin.role = g_targets[cd.row.rt].role;
        vin.cls = cd.cls;
        vin.status = stt;
        vin.buriedBy = status[i].by;
        vin.depthWrite = cd.depthWrite;
        vin.depthTest = cd.depthTest;
        vin.box = cd.row.box;
        vin.ctr = cd.row.ctr;
        vin.topology = cd.row.topology;
        vin.routeNone = cd.routeNone;
        vin.uiTaken = cd.uiTaken;
        vin.holo = cd.holo;
        vin.masked = cd.masked;
        vin.maskSlot = cd.maskSlot;
        vin.keyed = cd.keyed;
        vin.engineSub = cd.engineSub;
        const uint32_t rt = cd.row.rt;
        if (sceneLike(rt)) {
            vin.isDepthOwner = static_cast<int32_t>(i) == depthOwner;
            vin.aboveOwner = !vin.isDepthOwner && !cd.depthWrite && cd.row.ctr > 0 && (depthOwner < 0 || static_cast<int32_t>(i) > depthOwner);
            vin.depthOwnerIdx = depthOwner;
            vin.havePixel = centrePx.valid;
            vin.px = centrePx;
        }
        verdictFor(vin, &verdicts[i]);
        csv += drawsCsvRow(capId, cd.row, status[i].by, verdicts[i]);
        if (stt == Status::Unmeasured) {
            if (unmeasuredListed++ < 30) {
                std::snprintf(line, sizeof(line), "scene inspect unmeasured: #%u rt=#%u vs=%016llX ps=%016llX why=%s", cd.row.idx, cd.row.rt,
                              static_cast<unsigned long long>(cd.row.vs), static_cast<unsigned long long>(cd.row.ps), cd.untested[0] ? cd.untested : "query unanswered");
                say(line);
            }
        } else if (stt == Status::Buried && roleIsDlssInput(vin.role)) {
            ++occluded;
        }
    }
    uint32_t listed = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const bool intermediatePass = pass == 1;
        for (size_t i = 0; i < g_cands.size(); ++i) {
            Cand& cd = g_cands[i];
            const Status stt = status[i].status;
            if (stt != Status::Top && stt != Status::Partial) continue;
            if ((g_targets[cd.row.rt].role == Role::Intermediate) != intermediatePass) continue;
            if (listed >= kMaxListed) continue;
            ++listed;
            if (formatDrawLine(line, sizeof(line), cd.row) < 0) std::snprintf(line, sizeof(line), "scene inspect draw: #%u (the draw text did not fit)", cd.row.idx);
            say(line);
            if (!intermediatePass) {
                if (formatRouteLine(line, sizeof(line), cd.row) < 0) std::snprintf(line, sizeof(line), "scene inspect route: #%u (the route text did not fit)", cd.row.idx);
                say(line);
            }
            if (formatVerdictLine(line, sizeof(line), cd.row.idx, verdicts[i]) < 0) std::snprintf(line, sizeof(line), "scene inspect verdict: #%u %s", cd.row.idx, verdicts[i].code);
            say(line);
        }
    }
    if (c.truncated) Log::get().note("scene inspect: %u more involved draws were not listed (cap %u); the CSV has them all.", c.truncated, kMaxListed);
    for (size_t i = 0; i < g_cands.size(); ++i) {
        if (status[i].status != Status::Buried || g_targets[g_cands[i].row.rt].role == Role::Intermediate) continue;
        if (formatOccludedLine(line, sizeof(line), g_cands[i].row, status[i].by) > 0) say(line);
    }

    // The pixels, DLSS and the files.
    if (inputsRead) {
        std::snprintf(line, sizeof(line), "scene inspect input: DLSS input %ux%u, box [%d,%d)x[%d,%d) centre %d,%d, crop %ux%u at %d,%d (%s)", g_in.w, g_in.h, ib.x0, ib.x1,
                      ib.y0, ib.y1, ib.cx, ib.cy, g_in.crop.width(), g_in.crop.height(), g_in.crop.x0, g_in.crop.y0, g_in.sync.c_str());
        say(line);
        formatPixelLine(line, sizeof(line), "centre", centreX, centreY, centrePx);
        say(line);
        // The box corners that bound the range are summarised; the CSV has every pixel.
        formatBoxStatsLine(line, sizeof(line), stats);
        say(line);
        // The effect rule (effect_history.h): how many box pixels it hid, and the listed pairs that drew in the capture with their coverage of the box.
        {
            constexpr size_t kMaxEffectHits = 12;
            EffectHit hits[kMaxEffectHits];
            size_t nHits = 0, more = 0;
            for (const Cand& cd : g_cands) {
                const int pi = effectPairIndex(cd.row.vs, cd.row.ps);
                if (pi < 0 || cd.neverDrawn) continue;
                if (nHits >= kMaxEffectHits) {
                    ++more;
                    continue;
                }
                hits[nHits].name = kEffectHistoryPairs[pi].name;
                hits[nHits].idx = cd.row.idx;
                hits[nHits].box = cd.row.box;
                hits[nHits].ctr = cd.row.ctr;
                ++nHits;
            }
            if (formatEffectLine(line, sizeof(line), stats, hits, nHits, more, kEffectHistoryPairCount, kEffectHideShare) < 0)
                std::snprintf(line, sizeof(line), "scene inspect effect: %u of %u box pixels hidden by the effect rule (the pair text did not fit)", stats.effectHidden, stats.n);
            say(line);
        }
        // The engine records under the box: one line per slot (the most pixels first), what the pool holds for it.
        if (!g_in.slotX.empty()) {
            std::vector<SlotInfo> slots;
            for (const BoxSample& bs : samples) {
                if (!bs.valid || bs.px.slot < 0) continue;
                SlotInfo* si = nullptr;
                for (SlotInfo& x : slots)
                    if (x.slot == bs.px.slot) si = &x;
                if (!si) {
                    slots.emplace_back();
                    si = &slots.back();
                    si->slot = bs.px.slot;
                }
                ++si->pixels;
                ++si->kinds[bs.px.flags.engineKind & 7u];
            }
            std::sort(slots.begin(), slots.end(), [](const SlotInfo& a, const SlotInfo& b) { return a.pixels > b.pixels; });
            const uint32_t recWords = g_in.poolStride / 4u;
            say(engineSlotsLegend());
            std::snprintf(line, sizeof(line), "scene inspect engine: %zu distinct slots under the box; pool %s (%u records of %u bytes)", slots.size(),
                          g_in.poolRead ? "read" : "NOT read", g_in.poolCount, g_in.poolStride);
            say(line);
            size_t shown = 0;
            for (SlotInfo& x : slots) {
                if (shown++ >= 12) break;
                x.poolRecords = g_in.poolCount;
                if (g_in.poolRead && recWords >= kEngineRecordWords && static_cast<uint32_t>(x.slot) < g_in.poolCount) {
                    x.haveRecord = true;
                    x.rec = decodeEngineRecord(&g_in.poolWords[static_cast<size_t>(x.slot) * recWords]);
                }
                formatEngineSlotLine(line, sizeof(line), x);
                say(line);
            }
        } else {
            say("scene inspect engine: the slot target was not staged (engine-record velocity is not bound this frame, or its slot target is not a format the reader decodes)");
        }
        {
            ProducerTotals frame;
            frame.have = g_prodAtStart.have && g_prodAtEnd.have;
            if (frame.have) {
                frame.emitCalls = g_prodAtEnd.emitCalls - g_prodAtStart.emitCalls;
                frame.emitItems = g_prodAtEnd.emitItems - g_prodAtStart.emitItems;
                frame.secondCalls = g_prodAtEnd.secondCalls - g_prodAtStart.secondCalls;
                frame.secondItems = g_prodAtEnd.secondItems - g_prodAtStart.secondItems;
                frame.primaryCalls = g_prodAtEnd.primaryCalls - g_prodAtStart.primaryCalls;
            }
            formatProducersLine(line, sizeof(line), g_prodAtEnd, frame);
            say(line);
        }
    }
    DlssFacts df;
    if (g_out.offered) {
        const SceneOutputsIn& m = g_out.meta;
        df.have = true;
        df.preset = m.preset;
        df.w = m.w;
        df.h = m.h;
        df.outW = m.outW;
        df.outH = m.outH;
        df.jx = m.jx;
        df.jy = m.jy;
        df.reset = m.reset;
        df.resetWhy = m.resetWhy;
        df.reactiveHanded = m.reactiveHanded;
        df.preExposure = m.preExposure;
        df.exposureScale = m.exposureScale;
        df.exposureTexture = m.exposureTexture;
        df.hdr = m.hdr;
        df.frameMs = m.frameMs;
        df.uiResolved = m.uiResolved;
        df.sync = g_in.sync.c_str();
        df.sceneFrame = g_in.sceneFrame;
    }
    formatDlssLine(line, sizeof(line), df);
    say(line);

    FilesInfo files;
    const std::wstring dir = g_env.logDir ? g_env.logDir() : std::wstring();
    if (!dir.empty()) {
        ensureDirectory(dir);
        const std::string base = "scene_inspect_" + st + "_cap" + std::to_string(capId);
        std::string csvName = base + ".csv", boxName = base + "_box.csv";
        if (writeFile(dir + L"\\" + widen(csvName), csv.data(), csv.size())) std::snprintf(files.csv, sizeof(files.csv), "%s", csvName.c_str());
        if (inputsRead) {
            std::string bc = boxCsvHeader();
            for (const BoxSample& s : samples)
                if (s.valid) bc += boxCsvRow(capId, s);
            if (writeFile(dir + L"\\" + widen(boxName), bc.data(), bc.size())) std::snprintf(files.boxCsv, sizeof(files.boxCsv), "%s", boxName.c_str());
            // The crops.
            const uint32_t cw = g_in.crop.width(), ch = g_in.crop.height();
            // The box, in each crop's own pixels: a 1-pixel magenta ring just outside it is drawn on a copy, so the box's pixels stay as read.
            const Rect inBox{ib.x0 - g_in.crop.x0, ib.y0 - g_in.crop.y0, ib.x1 - g_in.crop.x0, ib.y1 - g_in.crop.y0};
            Rect outBox = inBox;
            if (g_out.read && g_out.offered && g_out.meta.w) {
                const double sx = static_cast<double>(g_out.meta.outW ? g_out.meta.outW : g_out.meta.w) / static_cast<double>(g_out.meta.w);
                const double sy = static_cast<double>(g_out.meta.outH ? g_out.meta.outH : g_out.meta.h) / static_cast<double>(g_out.meta.h);
                outBox.x0 = static_cast<int32_t>(std::lround(ib.x0 * sx)) - g_out.crop.x0;
                outBox.x1 = static_cast<int32_t>(std::lround(ib.x1 * sx)) - g_out.crop.x0;
                outBox.y0 = static_cast<int32_t>(std::lround(ib.y0 * sy)) - g_out.crop.y0;
                outBox.y1 = static_cast<int32_t>(std::lround(ib.y1 * sy)) - g_out.crop.y0;
            }
            auto addPng = [&](const std::string& suffix, const uint8_t* rgb, uint32_t w, uint32_t h, const char* note, const Rect& box) {
                const std::string name = base + "_" + suffix + ".png";
                std::vector<uint8_t> marked(rgb, rgb + static_cast<size_t>(w) * h * 3);
                drawBoxOutline(marked.data(), w, h, box.x0, box.y0, box.x1, box.y1);
                if (writePng(dir + L"\\" + widen(name), marked.data(), w, h)) {
                    std::string cur = files.pngs;
                    if (!cur.empty()) cur += ",";
                    cur += name;
                    std::snprintf(files.pngs, sizeof(files.pngs), "%s", cur.c_str());
                    Log::get().note("scene inspect image: %s %ux%u: %s%sthe magenta ring just outside the %ux%u box is drawn on the file; the box's own pixels are untouched (box at %d,%d..%d,%d in the image)",
                                    suffix.c_str(), w, h, note ? note : "", note && note[0] ? "; " : "", kBox, kBox, box.x0, box.y0, box.x1, box.y1);
                }
            };
            const size_t n = static_cast<size_t>(cw) * ch;
            if (!g_in.rgb.empty()) addPng("colour_in", g_in.rgb.data(), cw, ch, "the colour DLSS was handed (the crop's top-left is the 'crop' x,y in the input line)", inBox);
            if (!g_in.depth.empty()) {
                float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
                for (float d : g_in.depth)
                    if (std::isfinite(d)) {
                        lo = std::min(lo, d);
                        hi = std::max(hi, d);
                    }
                std::vector<uint8_t> img(n * 3);
                for (size_t i = 0; i < n; ++i) img[i * 3] = img[i * 3 + 1] = img[i * 3 + 2] = depthToGrey(g_in.depth[i], lo, hi);
                char note[96];
                std::snprintf(note, sizeof(note), "grey = raw reversed-Z depth stretched over %.6f (black)..%.6f (white, nearer)", static_cast<double>(lo), static_cast<double>(hi));
                addPng("depth", img.data(), cw, ch, note, inBox);
            }
            if (!g_in.mv.empty()) {
                float mx = 0.0f;
                for (float v : g_in.mv)
                    if (std::isfinite(v)) mx = std::max(mx, std::fabs(v));
                const float scale = motionScale(mx);
                std::vector<uint8_t> img(n * 3);
                for (size_t i = 0; i < n; ++i) motionToRgb(g_in.mv[i * 2], g_in.mv[i * 2 + 1], scale, &img[i * 3]);
                char note[220];
                std::snprintf(note, sizeof(note), "the motion DLSS was handed; scale %.3f per px (largest component %.3f px); %s", static_cast<double>(scale),
                              static_cast<double>(mx), motionImageLegend());
                addPng("motion", img.data(), cw, ch, note, inBox);
            }
            if (!g_in.dec.empty()) {
                std::vector<uint8_t> img(n * 3);
                for (size_t i = 0; i < n; ++i) pathToRgb(blend::decodeFlags(g_in.dec[i * 4 + 3]), &img[i * 3]);
                addPng("path", img.data(), cw, ch, pathImageLegend(), inBox);
            }
            if (g_out.read) {
                if (g_out.decoded[0]) addPng("dlss_out", g_out.rgb[0].data(), g_out.cw, g_out.ch, "DLSS's output over the same area, before the UI resolve", outBox);
                if (g_out.decoded[1]) addPng("final", g_out.rgb[1].data(), g_out.cw, g_out.ch, "the frame as it goes to the headset, after the UI resolve", outBox);
                if (!g_out.decoded[0] && g_out.tex[0]) Log::get().note("scene inspect image: dlss_out not written (format %u is not converted)", g_out.fmt[0]);
            }
        }
    }
    formatFilesLine(line, sizeof(line), capId, files);
    say(line);
    formatCompleteLine(line, sizeof(line), capId, c.involved, occluded, inputsRead, g_out.read && (g_out.decoded[0] || g_out.decoded[1]));
    say(line);
}

void finishCapture() {
    writeReport();
    releaseAll();
    g_phase.store(kIdle, std::memory_order_release);
}

const char* phaseName(int p) {
    switch (p) {
        case kArmed: return "armed";
        case kCollecting: return "collecting";
        case kReading: return "reading its answers";
        default: return "idle";
    }
}

}  // namespace

void sceneInspectInstall(bool flatProfile, bool keyBound, const char* key, const SceneInspectEnv& env) {
    g_installed = true;
    g_flat = flatProfile;
    g_env = env;
    char line[kLineBuf];
    formatInstallLine(line, sizeof(line), flatProfile, keyBound, key, env.iniName ? env.iniName() : nullptr);
    say(line);
}

void sceneInspectRequest(const char* trigger) {
    if (g_flat) {
        refuse(Refusal::kNotVr);
        return;
    }
    if (!g_installed) {
        refuse(Refusal::kNoHooks);
        return;
    }
    const int now = g_phase.load(std::memory_order_acquire);
    if (now != kIdle) {
        Log::get().note("scene inspect: refused (%s): the capture is %s (trigger %s)", refusalText(Refusal::kBusy), phaseName(now), trigger ? trigger : "");
        return;
    }
    // Written before the phase is published, so the boundary that sees Armed (an acquire load) sees the trigger and the number with it. Only the
    // key's own tick calls this, so two presses cannot race each other here.
    std::snprintf(g_pendingTrigger, sizeof(g_pendingTrigger), "%s", trigger ? trigger : "");
    g_pendingCap = g_capId + 1;
    g_phase.store(kArmed, std::memory_order_release);
    char line[kLineBuf];
    formatArmedLine(line, sizeof(line), g_pendingCap, g_pendingTrigger);
    say(line);
}

void sceneInspectFrameBoundary(uint32_t gameFrame) noexcept {
    const int p = g_phase.load(std::memory_order_acquire);
    if (p == kIdle) return;
    try {
        if (p == kArmed) {
            releaseAll();
            g_capId = g_pendingCap;
            g_trigger = g_pendingTrigger;
            g_cands.reserve(kMaxDraws);   // no reallocation mid-frame (about 5 MB, freed again by releaseAll)
            g_readTicks = 0;
            g_startFrame = gameFrame + 1;
            g_queryFailed = false;
            g_queryFailHr = S_OK;
            g_faults = 0;
            g_inputsWhy.clear();
            g_prodAtStart = readProducers();
            g_prodAtEnd = ProducerTotals{};
            g_phase.store(kCollecting, std::memory_order_release);
            char line[kLineBuf];
            formatCollectingLine(line, sizeof(line), g_capId, g_startFrame);
            say(line);
        } else if (p == kCollecting) {
            g_prodAtEnd = readProducers();
            g_phase.store(kReading, std::memory_order_release);
            g_readTicks = 0;
            if (g_ctx) g_ctx->Flush();
            pollQueries();
            if (g_in.staged && !g_in.read) tryReadInputs(g_in);
            if (g_out.staged && !g_out.read) tryReadOutputs(g_out);
        } else if (p == kReading) {
            ++g_readTicks;
            pollQueries();
            if (g_in.staged && !g_in.read) tryReadInputs(g_in);
            if (g_out.staged && !g_out.read) tryReadOutputs(g_out);
            const bool inputsDone = !g_in.staged || g_in.read;
            const bool outputsDone = !g_out.staged || g_out.read;
            // Staged inputs are waited for; inputs the pass has not offered yet are waited for too, for a few frames (it runs after the draws).
            const bool offerAwaited = !g_in.offered && g_inputsWhy.empty() && g_readTicks < 6;
            if ((outstandingQueries() == 0 && inputsDone && outputsDone && !offerAwaited) || g_readTicks >= kReadTicksMax) finishCapture();
        }
    } catch (...) {
        Log::get().note("scene inspect: the capture faulted at a frame boundary and was abandoned.");
        releaseAll();
        g_phase.store(kIdle, std::memory_order_release);
    }
}

bool sceneInspectWantsDraws() noexcept { return g_phase.load(std::memory_order_relaxed) == kCollecting; }

void sceneInspectDraw(ID3D11DeviceContext* ctx, const SceneDrawInfo& info, SceneReplayFn replay, void* arg) noexcept {
    if (g_phase.load(std::memory_order_relaxed) != kCollecting) return;
    try {
        drawImpl(ctx, info, replay, arg);
    } catch (...) {
        ++g_faults;
    }
}

void sceneInspectNoteIndirect() noexcept {
    if (g_phase.load(std::memory_order_relaxed) == kCollecting) ++g_counts.indirect;
}

void sceneInspectNoteClear(const void* resource) noexcept {
    if (g_phase.load(std::memory_order_relaxed) != kCollecting || !resource) return;
    try {
        g_clearAt.push_back(static_cast<uint32_t>(g_cands.size()));
        g_clearRt.push_back(resource);
    } catch (...) {
        ++g_faults;
    }
}

bool sceneInspectInputsWanted() noexcept {
    if (g_inputsOffered.load(std::memory_order_relaxed)) return false;
    const int p = g_phase.load(std::memory_order_relaxed);
    // The pass runs after the frame's draws (its offer for the PREVIOUS frame can land just as a capture starts: no draw has been seen yet,
    // so that one is not ours).
    return (p == kCollecting && g_seen.load(std::memory_order_relaxed) > 0) || p == kReading;
}

void sceneInspectInputsRefuse(const char* why) noexcept {
    try {
        if (g_inputsWhy.empty()) g_inputsWhy = why ? why : "";
        g_inputsOffered.store(true, std::memory_order_relaxed);
    } catch (...) {
    }
}

void sceneInspectInputsOffer(ID3D11DeviceContext* ctx, const SceneInputsIn& in) noexcept {
    if (!sceneInspectInputsWanted()) return;
    try {
        g_inputsOffered.store(true, std::memory_order_relaxed);
        Staged& s = g_in;
        s.offered = true;
        s.sync = g_phase.load(std::memory_order_relaxed) == kCollecting ? "same frame as the draws" : "after the draws' frame boundary";
        if (!ctx || in.w == 0 || in.h == 0 || !in.decision) {
            if (g_inputsWhy.empty())
                g_inputsWhy = !in.decision ? "the pass's decision texture was not built this frame (the capture-only shader variant or its texture was unavailable)"
                                           : "the pass offered an empty input";
            return;
        }
        if (!g_box.fixed) {
            if (g_inputsWhy.empty()) g_inputsWhy = "no left-eye draw fixed the box before the pass ran";
            return;
        }
        Com<ID3D11Device> dev;
        ctx->GetDevice(dev.put());
        if (!dev.get()) {
            if (g_inputsWhy.empty()) g_inputsWhy = "no device";
            return;
        }
        const float icx = g_box.u * static_cast<float>(in.w), icy = g_box.v * static_cast<float>(in.h);
        if (!cropAround(icx, icy, kCrop, in.w, in.h, &s.crop)) return;
        ID3D11Texture2D* srcs[7] = {in.colour, in.motion, in.depth, in.decision, in.coverage, in.bias, in.engineSlots};
        const DXGI_FORMAT want[7] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                     DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN};
        for (int k = 0; k < 7; ++k) s.have[k] = stageOne(dev.get(), ctx, srcs[k], want[k], s.crop, &s.tex[k], k == 6 ? &s.slotFmt : nullptr);
        if (s.have[6] && s.slotFmt != 16 && s.slotFmt != 2) {   // not a format the reader decodes: drop it, say nothing is read
            s.tex[6]->Release();
            s.tex[6] = nullptr;
            s.have[6] = false;
        }
        // The pool, whole, as the pass indexes it (the slots under the box are known only after the crop is read).
        if (in.enginePool && in.poolStride >= kEngineRecordWords * 4u && in.poolCount) {
            D3D11_BUFFER_DESC bd{};
            in.enginePool->GetDesc(&bd);
            const uint64_t need = (static_cast<uint64_t>(in.poolFirst) + in.poolCount) * in.poolStride;
            if (need <= bd.ByteWidth && bd.ByteWidth <= (16u << 20)) {
                D3D11_BUFFER_DESC sd = bd;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                sd.StructureByteStride = 0;
                if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &s.poolStaging)) && s.poolStaging) {
                    ctx->CopyResource(s.poolStaging, in.enginePool);
                    s.poolFirst = in.poolFirst;
                    s.poolCount = in.poolCount;
                    s.poolStride = in.poolStride;
                } else {
                    s.poolStaging = nullptr;
                }
            }
        }
        if (!s.have[3]) {
            if (g_inputsWhy.empty()) g_inputsWhy = "the decision texture could not be staged (format or allocation)";
            for (int k = 0; k < 6; ++k)
                if (s.tex[k]) {
                    s.tex[k]->Release();
                    s.tex[k] = nullptr;
                    s.have[k] = false;
                }
            return;
        }
        s.staged = true;
        s.w = in.w;
        s.h = in.h;
        s.sceneFrame = in.sceneFrame;
        s.meta = in;
        s.meta.colour = s.meta.motion = s.meta.depth = s.meta.decision = s.meta.coverage = s.meta.bias = nullptr;
        s.meta.engineSlots = nullptr;
        s.meta.enginePool = nullptr;
        holdCtx(ctx);
    } catch (...) {
        ++g_faults;
    }
}

void sceneInspectOutputsOffer(ID3D11DeviceContext* ctx, const SceneOutputsIn& in) noexcept {
    if (!g_in.staged || g_out.offered) return;
    const int p = g_phase.load(std::memory_order_relaxed);
    if (p != kCollecting && p != kReading) return;
    try {
        StagedOut& o = g_out;
        o.offered = true;
        o.meta = in;
        o.meta.dlss = o.meta.final = nullptr;
        if (!ctx || !in.dlss || in.w == 0 || in.h == 0) return;
        Com<ID3D11Device> dev;
        ctx->GetDevice(dev.put());
        if (!dev.get()) return;
        // The same area of the output: the box's centre scaled by the upscale, a crop of the same field of view.
        const uint32_t oW = in.outW ? in.outW : in.w, oH = in.outH ? in.outH : in.h;
        const float ocx = g_box.u * static_cast<float>(oW), ocy = g_box.v * static_cast<float>(oH);
        const uint32_t side = static_cast<uint32_t>(std::lround(static_cast<double>(kCrop) * static_cast<double>(oW) / static_cast<double>(in.w)));
        if (!cropAround(ocx, ocy, side, oW, oH, &o.crop)) return;
        ID3D11Texture2D* srcs[2] = {in.dlss, in.final};
        for (int k = 0; k < 2; ++k)
            if (srcs[k]) stageOne(dev.get(), ctx, srcs[k], DXGI_FORMAT_UNKNOWN, o.crop, &o.tex[k], &o.fmt[k]);
        o.staged = o.tex[0] || o.tex[1];
        holdCtx(ctx);
    } catch (...) {
        ++g_faults;
    }
}

}  // namespace edvr
