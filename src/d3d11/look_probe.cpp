// The look probe (look_probe.h; TEMPORARY, the aiming measurement of docs/concourse-culling-2026-10-11.md). The rules and the CSV
// rows are in look_probe_core.h. This file collects one frame: the records the admission store writes (store_hook.cpp) and the
// draws the census wrappers pass on (vscreen.cpp). Each draw is then tested for the centre pixel: a duplicate with a one-pixel
// scissor, no colour writes and no depth writes, counted by an occlusion query that is read back over the following Present
// ticks. Phases: a press arms; the next Present starts collecting (the store and the draws of the frame after it); the Present
// that ends that frame starts reading; the report is written when every query has answered or after kReadTicksMax ticks.
#include "look_probe.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "look_probe_core.h"
#include "occlusion_census.h"
#include "store_hook.h"

namespace edvr {
namespace {

// The engine's layouts (decompile-derived; the same offsets as sealed_trial.cpp and cull_gate_probe.cpp).
constexpr uintptr_t kCtxView0 = 0x40;        // the render context's first view (the main view)
constexpr uintptr_t kViewPlanes = 0x30;      // view: pointer to float4 planes
constexpr uintptr_t kViewPlaneCount = 0x44;  // view: u16 plane count
constexpr uintptr_t kViewOrigin = 0x540;     // view: float3 origin, the camera
constexpr uintptr_t kBucketBase = 0x28;      // bucket: the base record
constexpr uintptr_t kRecCentre = 0x240;      // record: float3 world centre
constexpr uintptr_t kRecRadius = 0x280;      // record: float radius
constexpr uintptr_t kRecNode = 0x18;         // record: the node pointer
constexpr uintptr_t kRecChildCount = 0x2B0;  // record: u32 child count

constexpr uint32_t kMaxCandidates = 8192;    // records on the ray kept (more are counted as overflow)
constexpr uint32_t kMaxDrawEntries = 8192;   // draws recorded per frame (more are counted as overflow)
constexpr uint32_t kReadTicksMax = 120;      // Present ticks the queries are polled before the report is written anyway

enum Phase : int { kIdle = 0, kArmed = 1, kCollecting = 2, kReading = 3 };
std::atomic<int> g_phase{kIdle};
std::string g_trigger;        // render thread
uint32_t g_frame = 0;         // the collected frame's number (render thread)
uint32_t g_readTicks = 0;     // Present ticks spent reading (render thread)
uintptr_t g_imageBase = 0;    // the module base, for the vtable RVAs (set at install)

// The camera ray, computed once per collection by the first store call.
std::atomic<bool> g_rayTried{false};
std::atomic<bool> g_rayOk{false};
float g_eye[3] = {};
float g_fwd[3] = {};
float g_nearM = 0.0f, g_farM = 0.0f;

struct Cand {
    uintptr_t address = 0, bucket = 0, bucketBase = 0;
    float radius = 0, t = 0, dist = 0;
    uint32_t childCount = 0;
    uint64_t vtableRva = 0, nodeVtableRva = 0;
};
Cand g_cands[kMaxCandidates];
std::atomic<uint32_t> g_candN{0}, g_candOverflow{0}, g_recScanned{0};

struct DrawEntry {
    char kind = '?';
    uint32_t count = 0, instances = 0;
    uint64_t vs = 0, ps = 0, rt = 0;
    uint32_t rtW = 0, rtH = 0;
    char blend[64] = {};
    char depth[64] = {};
    ID3D11Query* query = nullptr;
    uint64_t samples = 0;
    bool probed = false, done = false;
};
DrawEntry g_draws[kMaxDrawEntries];
std::atomic<uint32_t> g_drawN{0}, g_probedN{0}, g_drawOverflow{0}, g_probeFails{0};

// The probe's own state objects: one rasterizer, blend and depth-stencil state per distinct source state, and the context.
std::map<std::string, ID3D11RasterizerState*> g_rsClones;
std::map<std::string, ID3D11BlendState*> g_bsClones;
std::map<std::string, ID3D11DepthStencilState*> g_dsClones;
ID3D11DeviceContext* g_ctx = nullptr;   // AddRef'd while queries are outstanding

// Releases a COM pointer at scope exit.
template <typename T>
struct LookCom {
    T* p = nullptr;
    LookCom() = default;
    LookCom(const LookCom&) = delete;
    LookCom& operator=(const LookCom&) = delete;
    ~LookCom() {
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

// Releases every non-null pointer of an array at scope exit.
template <typename T, size_t N>
struct LookComArray {
    T* a[N] = {};
    LookComArray() = default;
    LookComArray(const LookComArray&) = delete;
    LookComArray& operator=(const LookComArray&) = delete;
    ~LookComArray() {
        for (T* x : a)
            if (x) x->Release();
    }
};

// Reads engine memory without a C++ object in the frame (the __try rule); false when the read faults or the address is 0.
bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept {
    return readMem(dst, src, sizeof(T));
}

const char* phaseName(int p) {
    switch (p) {
        case kArmed: return "armed";
        case kCollecting: return "collecting";
        case kReading: return "reading its centre tests";
        default: return "idle";
    }
}

// The camera and its frustum, from the main view of the bucket's render context. False when any read fails or the frustum
// has no opposite near and far pair (the records are then not aimed, and the report says so).
bool computeRay(uintptr_t bucket) noexcept {
    uintptr_t ctx = 0;
    if (!bucket || !readVal(&ctx, bucket) || !ctx) return false;
    const uintptr_t view = ctx + kCtxView0;
    float eye[3] = {};
    uint64_t planesAt = 0;
    uint16_t count = 0;
    if (!readMem(eye, view + kViewOrigin, sizeof(eye)) || !readVal(&planesAt, view + kViewPlanes) ||
        !readVal(&count, view + kViewPlaneCount))
        return false;
    if (!planesAt || count == 0) return false;
    const uint32_t n = count > look::kMaxPlanes ? look::kMaxPlanes : count;
    float raw[look::kMaxPlanes][4] = {};
    if (!readMem(raw, static_cast<uintptr_t>(planesAt), static_cast<size_t>(n) * 16)) return false;
    look::Plane planes[look::kMaxPlanes];
    for (uint32_t i = 0; i < n; ++i) {
        planes[i].n[0] = raw[i][0];
        planes[i].n[1] = raw[i][1];
        planes[i].n[2] = raw[i][2];
        planes[i].d = raw[i][3];
    }
    float fwd[3] = {};
    float nearM = 0.0f, farM = 0.0f;
    if (!look::forwardFromPlanes(planes, n, eye, fwd, &nearM, &farM)) return false;
    std::memcpy(g_eye, eye, sizeof(eye));
    std::memcpy(g_fwd, fwd, sizeof(fwd));
    g_nearM = nearM;
    g_farM = farM;
    return true;
}

// The rasterizer, blend and depth-stencil clones: the draw's own state with the one change that makes the duplicate safe.
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

// One draw's centre test. Returns true when the duplicate was issued and its query is queued (e.query set). The draw's states
// are saved and put back; the duplicate writes nothing but the query.
bool probeDraw(ID3D11DeviceContext* ctx, DrawEntry& e, LookReplayFn replay, void* arg) {
    // The draw must be a plain colour draw: a UAV or a stream output target would be written again by the duplicate.
    LookComArray<ID3D11RenderTargetView, 8> rtvs;
    LookComArray<ID3D11UnorderedAccessView, 64> uavs;
    LookCom<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(8, rtvs.a, dsv.put(), 0, 64, uavs.a);
    LookComArray<ID3D11Buffer, 4> so;
    ctx->SOGetTargets(4, so.a);
    if (!rtvs.a[0]) return false;
    for (ID3D11UnorderedAccessView* u : uavs.a)
        if (u) return false;
    for (ID3D11Buffer* b : so.a)
        if (b) return false;

    LookCom<ID3D11Resource> res;
    rtvs.a[0]->GetResource(res.put());
    LookCom<ID3D11Texture2D> tex;
    if (!res.get() || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.put()))) ||
        !tex.get())
        return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    // The centre pixel of the first viewport.
    D3D11_VIEWPORT vps[16] = {};
    UINT nVp = 16;
    ctx->RSGetViewports(&nVp, vps);
    if (nVp == 0) return false;
    const int cx = static_cast<int>(std::floor(vps[0].TopLeftX + vps[0].Width * 0.5f));
    const int cy = static_cast<int>(std::floor(vps[0].TopLeftY + vps[0].Height * 0.5f));
    if (cx < 0 || cy < 0 || static_cast<uint32_t>(cx) >= td.Width || static_cast<uint32_t>(cy) >= td.Height) return false;
    const D3D11_RECT one = {cx, cy, cx + 1, cy + 1};

    // The states the draw runs with (each Get adds a reference, released at scope exit).
    LookCom<ID3D11RasterizerState> rs;
    ctx->RSGetState(rs.put());
    UINT scN = 16;
    D3D11_RECT sc[16] = {};
    ctx->RSGetScissorRects(&scN, sc);
    LookCom<ID3D11BlendState> bs;
    FLOAT bf[4] = {};
    UINT bmask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(bs.put(), bf, &bmask);
    LookCom<ID3D11DepthStencilState> ds;
    UINT dref = 0;
    ctx->OMGetDepthStencilState(ds.put(), &dref);
    if (!rs.get() || !bs.get() || !ds.get()) return false;

    D3D11_RASTERIZER_DESC rsd;
    std::memset(&rsd, 0, sizeof(rsd));
    rs->GetDesc(&rsd);
    D3D11_BLEND_DESC bsd;
    std::memset(&bsd, 0, sizeof(bsd));
    bs->GetDesc(&bsd);
    D3D11_DEPTH_STENCIL_DESC dsd;
    std::memset(&dsd, 0, sizeof(dsd));
    ds->GetDesc(&dsd);

    LookCom<ID3D11Device> dev;
    ctx->GetDevice(dev.put());
    if (!dev.get()) return false;
    ID3D11RasterizerState* rsC = rasterizerClone(dev.get(), rsd);
    ID3D11BlendState* bsC = blendClone(dev.get(), bsd);
    ID3D11DepthStencilState* dsC = depthClone(dev.get(), dsd);
    if (!rsC || !bsC || !dsC) return false;

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    LookCom<ID3D11Query> q;
    if (FAILED(dev->CreateQuery(&qd, q.put())) || !q.get()) return false;

    // The duplicate: the draw's own depth test, against the depth buffer as it is before the draw, writing nothing.
    ctx->RSSetState(rsC);
    ctx->RSSetScissorRects(1, &one);
    ctx->OMSetBlendState(bsC, bf, bmask);
    ctx->OMSetDepthStencilState(dsC, dref);
    ctx->Begin(q.get());
    replay(arg);
    ctx->End(q.get());
    // Put back what the draw itself runs with.
    ctx->RSSetState(rs.get());
    ctx->RSSetScissorRects(scN, sc);
    ctx->OMSetBlendState(bs.get(), bf, bmask);
    ctx->OMSetDepthStencilState(ds.get(), dref);

    e.rt = reinterpret_cast<uint64_t>(rtvs.a[0]);
    e.rtW = td.Width;
    e.rtH = td.Height;
    const D3D11_RENDER_TARGET_BLEND_DESC& rt0 = bsd.RenderTarget[0];
    std::snprintf(e.blend, sizeof(e.blend), "en%d:src%u:dst%u:op%u:wm%u:a2c%d", static_cast<int>(rt0.BlendEnable),
                  static_cast<unsigned>(rt0.SrcBlend), static_cast<unsigned>(rt0.DestBlend),
                  static_cast<unsigned>(rt0.BlendOp), static_cast<unsigned>(rt0.RenderTargetWriteMask),
                  static_cast<int>(bsd.AlphaToCoverageEnable));
    std::snprintf(e.depth, sizeof(e.depth), "dsen%d:fn%u:wm%u:sten%d", static_cast<int>(dsd.DepthEnable),
                  static_cast<unsigned>(dsd.DepthFunc), static_cast<unsigned>(dsd.DepthWriteMask),
                  static_cast<int>(dsd.StencilEnable));
    e.query = q.release();
    return true;
}

// Records one draw of the collected frame (the census wrappers call this before their own bracket) and tests its centre.
void lookProbeDrawImpl(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, LookReplayFn replay,
                       void* arg) {
    const uint32_t index = g_drawN.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMaxDrawEntries) {
        g_drawOverflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    DrawEntry& e = g_draws[index];
    e.kind = kind;
    e.count = count;
    e.instances = instances;
    e.vs = bindingShaderHash(BindSlot::Vs);
    e.ps = bindingShaderHash(BindSlot::Ps);
    e.rt = 0;
    e.rtW = 0;
    e.rtH = 0;
    e.blend[0] = '\0';
    e.depth[0] = '\0';
    e.query = nullptr;
    e.samples = 0;
    e.probed = false;
    e.done = false;
    if (!ctx || !replay || count == 0 || instances == 0) return;
    if (g_probedN.load(std::memory_order_relaxed) >= look::kProbeDraws) return;
    if (probeDraw(ctx, e, replay, arg)) {
        e.probed = true;
        g_probedN.fetch_add(1, std::memory_order_relaxed);
        if (!g_ctx) {
            g_ctx = ctx;
            ctx->AddRef();
        }
    }
}

// Polls every outstanding query; a query that answers keeps its count and is marked done.
void pollQueries() {
    if (!g_ctx) return;
    const uint32_t n = std::min(g_drawN.load(std::memory_order_relaxed), kMaxDrawEntries);
    for (uint32_t i = 0; i < n; ++i) {
        DrawEntry& e = g_draws[i];
        if (!e.query || e.done) continue;
        UINT64 samples = 0;
        if (g_ctx->GetData(e.query, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            e.done = true;
            e.samples = samples;
        }
    }
}

uint32_t outstandingQueries() {
    const uint32_t n = std::min(g_drawN.load(std::memory_order_relaxed), kMaxDrawEntries);
    uint32_t left = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (g_draws[i].query && !g_draws[i].done) ++left;
    return left;
}

void releaseProbeObjects() {
    const uint32_t n = std::min(g_drawN.load(std::memory_order_relaxed), kMaxDrawEntries);
    for (uint32_t i = 0; i < n; ++i) {
        if (g_draws[i].query) {
            g_draws[i].query->Release();
            g_draws[i].query = nullptr;
        }
    }
    for (auto& kv : g_rsClones)
        if (kv.second) kv.second->Release();
    g_rsClones.clear();
    for (auto& kv : g_bsClones)
        if (kv.second) kv.second->Release();
    g_bsClones.clear();
    for (auto& kv : g_dsClones)
        if (kv.second) kv.second->Release();
    g_dsClones.clear();
    if (g_ctx) {
        g_ctx->Release();
        g_ctx = nullptr;
    }
}

void resetCollection() {
    g_candN.store(0, std::memory_order_relaxed);
    g_candOverflow.store(0, std::memory_order_relaxed);
    g_recScanned.store(0, std::memory_order_relaxed);
    g_drawN.store(0, std::memory_order_relaxed);
    g_probedN.store(0, std::memory_order_relaxed);
    g_drawOverflow.store(0, std::memory_order_relaxed);
    g_probeFails.store(0, std::memory_order_relaxed);
    g_rayTried.store(false, std::memory_order_relaxed);
    g_rayOk.store(false, std::memory_order_relaxed);
}

// The CSV and the summary line. The CSV holds the records nearest the ray first, then the draws that cover the centre (the
// last kMaxDraws of them in issue order: later draws are the ones that end up on screen).
void writeReport() {
    const uint32_t nCand = std::min(g_candN.load(std::memory_order_relaxed), kMaxCandidates);
    const uint32_t nDraws = std::min(g_drawN.load(std::memory_order_relaxed), kMaxDrawEntries);
    std::vector<Cand> cands(g_cands, g_cands + nCand);
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.t != b.t) return a.t < b.t;
        return a.dist < b.dist;
    });
    std::vector<uint32_t> covering;
    uint32_t probed = 0, unread = 0;
    for (uint32_t i = 0; i < nDraws; ++i) {
        const DrawEntry& e = g_draws[i];
        if (!e.probed) continue;
        ++probed;
        if (!e.done)
            ++unread;
        else if (e.samples > 0)
            covering.push_back(i);
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[24] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    char csvName[64] = {};
    std::snprintf(csvName, sizeof(csvName), "look_probe_%s.csv", stamp);
    std::wstring path = Config::get().logDir() + L"\\look_probe_";
    for (const char* c = stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";

    char row[512];
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        std::fprintf(f, "%s\n", look::csvHeader());
        for (uint32_t k = 0; k < std::min<uint32_t>(nCand, look::kMaxRecords); ++k) {
            const Cand& c = cands[k];
            look::RecordRow rr;
            rr.index = k;
            rr.address = c.address;
            rr.bucket = c.bucket;
            rr.bucketBase = c.bucketBase;
            rr.radius = c.radius;
            rr.t = c.t;
            rr.dist = c.dist;
            rr.role = (c.address == c.bucketBase) ? "base" : "child";
            rr.childCount = c.childCount;
            rr.vtableRva = c.vtableRva;
            rr.nodeVtableRva = c.nodeVtableRva;
            if (look::formatRecordRow(row, sizeof(row), rr) >= 0) std::fprintf(f, "%s\n", row);
        }
        const size_t first = covering.size() > look::kMaxDraws ? covering.size() - look::kMaxDraws : 0;
        for (size_t k = first; k < covering.size(); ++k) {
            const DrawEntry& e = g_draws[covering[k]];
            look::DrawRow dr;
            dr.index = covering[k];
            dr.vs = e.vs;
            dr.ps = e.ps;
            dr.drawKind = e.kind;
            dr.count = e.count;
            dr.instances = e.instances;
            dr.blend = e.blend;
            dr.depth = e.depth;
            dr.rt = e.rt;
            dr.rtW = e.rtW;
            dr.rtH = e.rtH;
            dr.centreSamples = e.samples;
            dr.covers = true;
            if (look::formatDrawRow(row, sizeof(row), dr) >= 0) std::fprintf(f, "%s\n", row);
        }
        std::fclose(f);
    } else {
        Log::get().note("look probe: could not open edvr_logs\\%s; the summary below is complete.", csvName);
    }

    look::Summary s;
    s.frame = g_frame;
    s.trigger = g_trigger.c_str();
    s.forwardKnown = g_rayOk.load(std::memory_order_acquire);
    for (int k = 0; k < 3; ++k) {
        s.forward[k] = g_fwd[k];
        s.eye[k] = g_eye[k];
    }
    s.recordsScanned = g_recScanned.load(std::memory_order_relaxed);
    s.recordsInRay = nCand;
    s.recordsListed = std::min<uint32_t>(nCand, look::kMaxRecords);
    s.draws = nDraws;
    s.drawsProbed = probed;
    s.drawsCovering = static_cast<uint32_t>(covering.size());
    s.drawsListed = std::min<uint32_t>(s.drawsCovering, look::kMaxDraws);
    s.csvName = csvName;
    char line[640];
    look::formatSummary(line, sizeof(line), s);
    Log::get().note("%s", line);
    if (s.forwardKnown)
        Log::get().note("look probe: the far plane is %.1f m from the eye and the near plane %.3f m; the forward is taken "
                        "from the far plane.", static_cast<double>(g_farM), static_cast<double>(g_nearM));
    if (s.drawsCovering > look::kMaxDraws)
        Log::get().note("look probe: %u draws cover the centre; the last %u in issue order are listed.", s.drawsCovering,
                        look::kMaxDraws);
    if (unread)
        Log::get().note("look probe: %u centre queries had not answered after %u Present ticks; those draws have no verdict.",
                        unread, kReadTicksMax);
    if (g_candOverflow.load(std::memory_order_relaxed) || g_drawOverflow.load(std::memory_order_relaxed))
        Log::get().note("look probe: overflow: %u records on the ray beyond %u, %u draws beyond %u were not recorded.",
                        g_candOverflow.load(std::memory_order_relaxed), kMaxCandidates,
                        g_drawOverflow.load(std::memory_order_relaxed), kMaxDrawEntries);
    if (g_probeFails.load(std::memory_order_relaxed))
        Log::get().note("look probe: %u centre tests failed to set up (a state object or a query was refused).",
                        g_probeFails.load(std::memory_order_relaxed));
}

void finishReport() {
    writeReport();
    releaseProbeObjects();
    g_phase.store(kIdle, std::memory_order_release);
}

// The Present that starts the collection: the frame after this one is the one collected.
void startCollecting(uint32_t frame) {
    resetCollection();
    g_frame = frame + 1;
    g_phase.store(kCollecting, std::memory_order_release);
    storeHookRecompute();
    if (std::strcmp(storeHookStatus(), "installed") != 0) {
        g_phase.store(kIdle, std::memory_order_release);
        storeHookRecompute();
        Log::get().note("look probe: refused (trigger %s): the admission store is not detoured (%s).", g_trigger.c_str(),
                        storeHookStatus());
        return;
    }
    occlusionCensusInstallWrappers(true);
    Log::get().note("look probe: collecting frame %u (trigger %s): the store's records and the draws of that frame; each "
                    "draw is tested at the centre pixel.", g_frame, g_trigger.c_str());
}

// The Present that ends the collected frame: the wrappers and the store come off, and the queries are polled from now on.
void finishCollecting(uint32_t frame) {
    occlusionCensusInstallWrappers(false);
    g_phase.store(kReading, std::memory_order_release);
    storeHookRecompute();
    g_readTicks = 0;
    pollQueries();
    const uint32_t nDraws = std::min(g_drawN.load(std::memory_order_relaxed), kMaxDrawEntries);
    Log::get().note("look probe: frame %u collected: %u records written by the store, %u on the ray; %u draws, %u centre "
                    "tests issued; reading back.", frame + 1, g_recScanned.load(std::memory_order_relaxed),
                    std::min(g_candN.load(std::memory_order_relaxed), kMaxCandidates), nDraws,
                    g_probedN.load(std::memory_order_relaxed));
    if (outstandingQueries() == 0) finishReport();
}

void readTick() {
    pollQueries();
    ++g_readTicks;
    if (outstandingQueries() == 0 || g_readTicks >= kReadTicksMax) finishReport();
}

}  // namespace

void lookProbeInstall(bool flatProfile) {
    g_imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Log::get().note("look probe: off. hotkey.look_probe arms one capture (the next frame's records and draws, then each draw's "
                    "centre test, up to %u draws). Output: edvr_logs\\look_probe_<stamp>.csv and a \"look probe:\" line.%s",
                    look::kProbeDraws,
                    flatProfile ? " Flat: the draws are seen only if the flat draw path goes through the census wrappers "
                                  "(not yet flown)." : "");
}

void lookProbeRequest(const char* trigger) {
    int expected = kIdle;
    if (!g_phase.compare_exchange_strong(expected, kArmed, std::memory_order_acq_rel)) {
        Log::get().note("look probe: refused (trigger %s): a capture is already %s; press again once it has reported.",
                        trigger ? trigger : "", phaseName(expected));
        return;
    }
    g_trigger = trigger ? trigger : "";
    Log::get().note("look probe: armed (trigger %s): the next frame's records and draws are collected from the Present after "
                    "this one.", g_trigger.c_str());
}

void lookProbeFrame(uint32_t frame) {
    const int p = g_phase.load(std::memory_order_acquire);
    if (p == kArmed)
        startCollecting(frame);
    else if (p == kCollecting)
        finishCollecting(frame);
    else if (p == kReading)
        readTick();
}

bool lookProbeCollecting() noexcept {
    return g_phase.load(std::memory_order_acquire) == kCollecting;
}

bool lookProbeWantsDraws() noexcept {
    return g_phase.load(std::memory_order_acquire) == kCollecting;
}

void lookProbeAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    if (g_phase.load(std::memory_order_acquire) != kCollecting || !record) return;
    g_recScanned.fetch_add(1, std::memory_order_relaxed);
    if (!g_rayTried.exchange(true, std::memory_order_acq_rel)) g_rayOk.store(computeRay(bucket), std::memory_order_release);
    if (!g_rayOk.load(std::memory_order_acquire)) return;
    float c[3] = {};
    float r = 0.0f;
    if (!readMem(c, record + kRecCentre, sizeof(c)) || !readVal(&r, record + kRecRadius)) return;
    if (!std::isfinite(r) || !(r > 0.0f) || !std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) return;
    float t = 0.0f;
    if (!look::rayHitsSphere(g_eye, g_fwd, c, r, look::kRangeM, &t)) return;
    const uint32_t have = std::min(g_candN.load(std::memory_order_relaxed), kMaxCandidates);
    for (uint32_t k = 0; k < have; ++k)
        if (g_cands[k].address == record) return;
    const uint32_t k = g_candN.fetch_add(1, std::memory_order_relaxed);
    if (k >= kMaxCandidates) {
        g_candOverflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Cand& cd = g_cands[k];
    cd.address = record;
    cd.bucket = bucket;
    cd.radius = r;
    cd.t = t;
    const float dx = c[0] - g_eye[0], dy = c[1] - g_eye[1], dz = c[2] - g_eye[2];
    cd.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    uintptr_t base = 0;
    if (readVal(&base, bucket + kBucketBase)) cd.bucketBase = base;
    uint32_t children = 0;
    if (readVal(&children, record + kRecChildCount)) cd.childCount = children;
    uint64_t vt = 0;
    if (readVal(&vt, record)) cd.vtableRva = vt >= g_imageBase ? vt - g_imageBase : vt;
    uint64_t node = 0, nodeVt = 0;
    if (readVal(&node, record + kRecNode) && node && readVal(&nodeVt, static_cast<uintptr_t>(node)))
        cd.nodeVtableRva = nodeVt >= g_imageBase ? nodeVt - g_imageBase : nodeVt;
}

void lookProbeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, LookReplayFn replay,
                   void* arg) noexcept {
    try {
        lookProbeDrawImpl(ctx, kind, count, instances, replay, arg);
    } catch (...) {
        g_probeFails.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace edvr
