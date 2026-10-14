// The effect-history rule's device half: see effect_history.h and effect_history_core.h. tools\effect_history_test runs this file on a WARP device.
#include "effect_history.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "../common/log.h"
#include "effect_history_core.h"

namespace edvr {
namespace detail {
std::atomic<bool> g_effectNvidia{false};
}

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kQueryRing = 64;          // occlusion queries in flight; a draw finding the ring full goes unmeasured, never waits
constexpr uint32_t kCensusSeconds = 30;
constexpr uint32_t kLapseFrames = 600;       // frames of the pass not saying it is on NVIDIA's path before the rule stands down
constexpr uint64_t kArmedMaxAge = 4;         // frames an eye may stay armed with its pass never asking
constexpr size_t kStateCache = 32;
constexpr uint32_t kSlots = 4;               // two eyes, and room for a depth texture the game swaps

struct EyeSlot {
    // The scene depth this slot's draws were tested against: the eye's identity. Held so its address cannot be reused while the slot keeps it.
    ComPtr<ID3D11Resource> depth;
    uint64_t usedFrame = 0;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
    bool armed = false;
    uint64_t armedFrame = 0;
    // The game's HDR target as the last listed draw of this eye drew into it. Held (not just remembered) so its address cannot be reused while the
    // cached view is.
    ComPtr<ID3D11Resource> hdr;
    ComPtr<ID3D11ShaderResourceView> hdrSrv;
    uint32_t hdrW = 0, hdrH = 0;
    DXGI_FORMAT hdrFmt = DXGI_FORMAT_UNKNOWN;
    bool hdrBindable = true;
};

struct QuerySlot {
    ComPtr<ID3D11Query> q;
    bool pending = false;
    uint32_t pair = 0;
};

struct StateEntry {
    D3D11_DEPTH_STENCIL_DESC desc{};
    ComPtr<ID3D11DepthStencilState> state;
};

// The device objects live in a block that is never destroyed: the DLL's statics are torn down at process exit, after the device may be gone, and a
// ComPtr releasing into a dead device there is a crash the module has no reason to risk. effectHistoryShutdown releases them on purpose.
struct Module {
    EyeSlot eye[kSlots];   // one per scene depth texture the effects were drawn against: the eyes
    QuerySlot ring[kQueryRing];
    std::vector<StateEntry> states;
    ComPtr<ID3D11Buffer> counterStage;   // 4 bytes: the hidden-pixel counter, copied out and read a few frames later
};
Module* const g_mod = new Module;

// The hidden-pixel counter's reader: a copy at most every kCounterEvery frames, mapped kCounterWait frames later without waiting. The counter is a uint
// that wraps and that a diagnostic eye run clears, so the growth is taken modulo 2^32 and a growth no eye-frame could have made is dropped.
constexpr uint64_t kCounterEvery = 30, kCounterWait = 3;
bool g_counterPending = false;
uint64_t g_counterCopyFrame = 0, g_viewedTotal = 0, g_viewedAtCopy = 0, g_viewedAtLast = 0;
uint32_t g_counterLast = 0;
bool g_counterHaveLast = false;
constexpr uint64_t kMaxHiddenPerEyeFrame = 1ull << 24;   // 16.7 M pixels: more than any eye has

std::mutex g_mu;
EffectHistoryEnv g_env;
bool g_installed = false;
uint32_t g_ringNext = 0;
uint64_t g_frame = 0;
uint32_t g_idleFrames = 0;
EffectWindow g_win;
ULONGLONG g_censusAt = 0;
EffectHistoryStats g_stats;
bool g_unreadableSaid = false;

template <class T>
struct Held {   // an array of interface pointers released together
    T* a[8] = {};
    Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    ~Held() {
        for (T* x : a)
            if (x) x->Release();
    }
};

DXGI_FORMAT typedFor(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return f;
    }
}

bool makeScratch(ID3D11Device* dev, EyeSlot& s, uint32_t w, uint32_t h) {
    if (s.tex && s.w == w && s.h == h) return true;
    s.tex.Reset();
    s.rtv.Reset();
    s.srv.Reset();
    s.w = s.h = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, s.tex.GetAddressOf())) || !s.tex) return false;
    if (FAILED(dev->CreateRenderTargetView(s.tex.Get(), nullptr, s.rtv.GetAddressOf())) ||
        FAILED(dev->CreateShaderResourceView(s.tex.Get(), nullptr, s.srv.GetAddressOf()))) {
        s.tex.Reset();
        s.rtv.Reset();
        s.srv.Reset();
        return false;
    }
    s.w = w;
    s.h = h;
    return true;
}

// The game's depth-stencil state with every write off and the tests as they were. A null state is D3D11's default.
ID3D11DepthStencilState* readOnlyDepth(ID3D11Device* dev, ID3D11DepthStencilState* game) {
    D3D11_DEPTH_STENCIL_DESC d;
    std::memset(&d, 0, sizeof(d));
    if (game) {
        game->GetDesc(&d);
    } else {
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilReadMask = d.StencilWriteMask = 0xFF;
        d.FrontFace.StencilFunc = d.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp = d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        d.BackFace = d.FrontFace;
    }
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilWriteMask = 0;
    for (const StateEntry& e : g_mod->states)
        if (std::memcmp(&e.desc, &d, sizeof(d)) == 0) return e.state.Get();
    if (g_mod->states.size() >= kStateCache) g_mod->states.clear();
    StateEntry e;
    e.desc = d;
    if (FAILED(dev->CreateDepthStencilState(&d, e.state.GetAddressOf())) || !e.state) return nullptr;
    g_mod->states.push_back(e);
    return g_mod->states.back().state.Get();
}

void refuse() {
    ++g_win.refused;
    ++g_stats.refused;
}

void drawImpl(ID3D11DeviceContext* ctx, uint64_t vs, uint64_t ps, EffectReplayFn replay, void* arg) {
    const int pair = effectPairIndex(vs, ps);
    if (pair < 0) return;
    if (!ctx || !replay || !g_installed || !g_env.setRenderTargets || !g_env.clearRenderTarget) return;
    std::lock_guard<std::mutex> lock(g_mu);

    Held<ID3D11RenderTargetView> rtvs;
    ID3D11DepthStencilView* dsv = nullptr;
    // 64 unordered-access slots exist from feature level 11.1; an 11.0 device has 8 and the debug layer refuses a longer list.
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(dev.GetAddressOf());
    if (!dev) return;
    const UINT nUav = dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
    std::vector<ID3D11UnorderedAccessView*> uavList(nUav, nullptr);
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(8, rtvs.a, &dsv, 0, nUav, uavList.data());
    ComPtr<ID3D11DepthStencilView> dsvHold;
    dsvHold.Attach(dsv);
    bool anyUav = false;
    for (ID3D11UnorderedAccessView* u : uavList)
        if (u) {
            anyUav = true;
            u->Release();
        }
    if (!rtvs.a[0]) return;
    ComPtr<ID3D11Resource> res;
    rtvs.a[0]->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex)) || !tex) return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (anyUav || !dsv) {   // a UAV write would be done twice; without the game's depth the replica could not be occluded
        refuse();
        return;
    }
    if (td.SampleDesc.Count != 1) {
        refuse();
        return;
    }
    ComPtr<ID3D11Resource> dres;
    {
        dsv->GetResource(dres.GetAddressOf());
        ComPtr<ID3D11Texture2D> dtex;
        D3D11_TEXTURE2D_DESC dd{};
        if (!dres || FAILED(dres.As(&dtex)) || !dtex) {
            refuse();
            return;
        }
        dtex->GetDesc(&dd);
        if (dd.Width != td.Width || dd.Height != td.Height) {   // the replica's target and the game's depth must be the same size
            refuse();
            return;
        }
    }
    // The eye is the scene depth the draw tests against (the temporal pass asks by the depth texture it reads for the eye): the slot that holds it, else
    // the one unused longest, which starts over.
    EyeSlot* sp = nullptr;
    for (EyeSlot& c : g_mod->eye)
        if (c.depth.Get() == dres.Get()) sp = &c;
    if (!sp) {
        sp = &g_mod->eye[0];
        for (EyeSlot& c : g_mod->eye)
            if (c.usedFrame < sp->usedFrame) sp = &c;
        sp->depth = dres;
        sp->armed = false;
        sp->hdr.Reset();
        sp->hdrSrv.Reset();
    }
    EyeSlot& s = *sp;
    s.usedFrame = g_frame;
    if (!makeScratch(dev.Get(), s, td.Width, td.Height)) {
        refuse();
        return;
    }
    // The first listed draw of this eye-frame starts the eye over: the private target cleared, the game's HDR target noted.
    if (!s.armed || s.armedFrame != g_frame || s.hdr.Get() != res.Get()) {
        static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g_env.clearRenderTarget(ctx, s.rtv.Get(), zero);
        if (s.hdr.Get() != res.Get()) {
            s.hdr = res;
            s.hdrSrv.Reset();
            s.hdrW = td.Width;
            s.hdrH = td.Height;
            D3D11_RENDER_TARGET_VIEW_DESC rvd{};
            rtvs.a[0]->GetDesc(&rvd);
            s.hdrFmt = typedFor(rvd.Format != DXGI_FORMAT_UNKNOWN ? rvd.Format : td.Format);
            s.hdrBindable = (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 && s.hdrFmt != DXGI_FORMAT_UNKNOWN;
        }
        s.armed = true;
        s.armedFrame = g_frame;
        ++g_win.eyeFrames;
    }

    ComPtr<ID3D11DepthStencilState> gameDs;
    UINT dref = 0;
    ctx->OMGetDepthStencilState(gameDs.GetAddressOf(), &dref);
    ID3D11DepthStencilState* readOnly = readOnlyDepth(dev.Get(), gameDs.Get());
    if (!readOnly) {
        refuse();
        return;
    }

    // The query for the covered-pixel count: a ring slot that is free, else the draw goes unmeasured.
    QuerySlot* slot = nullptr;
    for (uint32_t k = 0; k < kQueryRing && !slot; ++k) {
        QuerySlot& c = g_mod->ring[(g_ringNext + k) % kQueryRing];
        if (c.pending) continue;
        if (!c.q) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_OCCLUSION;
            if (FAILED(dev->CreateQuery(&qd, c.q.GetAddressOf()))) continue;
        }
        slot = &c;
        g_ringNext = (g_ringNext + k + 1) % kQueryRing;
    }
    if (!slot) ++g_win.queryGaps;

    const bool timed = g_env.timerBegin && g_env.timerBegin(ctx);
    ID3D11RenderTargetView* scratch = s.rtv.Get();
    g_env.setRenderTargets(ctx, 1, &scratch, dsv);
    ctx->OMSetDepthStencilState(readOnly, dref);
    if (slot) ctx->Begin(slot->q.Get());
    replay(arg);
    if (slot) {
        ctx->End(slot->q.Get());
        slot->pending = true;
        slot->pair = static_cast<uint32_t>(pair);
    }
    ctx->OMSetDepthStencilState(gameDs.Get(), dref);
    uint32_t bound = 0;
    for (uint32_t k = 0; k < 8; ++k)
        if (rtvs.a[k]) bound = k + 1;
    g_env.setRenderTargets(ctx, bound, rtvs.a, dsv);
    if (timed && g_env.timerEnd) g_env.timerEnd(ctx);

    ++g_win.draws[pair];
    ++g_stats.replicas;
}

void sampleCounter(ID3D11DeviceContext* ctx) {
    if (!g_env.hiddenCounter) return;
    if (g_counterPending) {
        if (g_frame < g_counterCopyFrame + kCounterWait || !g_mod->counterStage) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT h = ctx->Map(g_mod->counterStage.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (h == DXGI_ERROR_WAS_STILL_DRAWING) return;   // try again next frame
        g_counterPending = false;
        if (FAILED(h) || !m.pData) return;
        uint32_t cur = 0;
        std::memcpy(&cur, m.pData, sizeof(cur));
        ctx->Unmap(g_mod->counterStage.Get(), 0);
        if (g_counterHaveLast) {
            const uint32_t grew = cur - g_counterLast;   // modulo 2^32
            const uint64_t eyeFrames = g_viewedAtCopy - g_viewedAtLast;
            if (grew <= eyeFrames * kMaxHiddenPerEyeFrame) {
                g_win.hiddenPixels += grew;
                g_win.hiddenEyeFrames += eyeFrames;
                ++g_win.counterReads;
            }
        }
        g_counterLast = cur;
        g_counterHaveLast = true;
        g_viewedAtLast = g_viewedAtCopy;
        return;
    }
    if (g_frame % kCounterEvery != 0 || g_viewedTotal == g_viewedAtLast) return;   // nothing handed to the shader since the last read
    uint32_t offset = 0;
    ID3D11Buffer* counter = g_env.hiddenCounter(&offset);
    if (!counter) return;
    if (!g_mod->counterStage) {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(dev.GetAddressOf());
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(uint32_t);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!dev || FAILED(dev->CreateBuffer(&bd, nullptr, g_mod->counterStage.GetAddressOf()))) return;
    }
    const D3D11_BOX box = {offset, 0, 0, offset + static_cast<UINT>(sizeof(uint32_t)), 1, 1};
    ctx->CopySubresourceRegion(g_mod->counterStage.Get(), 0, 0, 0, 0, counter, 0, &box);
    g_counterPending = true;
    g_counterCopyFrame = g_frame;
    g_viewedAtCopy = g_viewedTotal;
}

void sayCensus() {
    double mib = 0.0;
    for (const EyeSlot& s : g_mod->eye) mib += static_cast<double>(s.w) * s.h * 4.0 / (1024.0 * 1024.0);
    g_win.scratchMiB = mib;
    g_win.targetReadable = g_win.targetUnreadable == 0;
    char line[1400];
    if (formatEffectCensus(line, sizeof(line), g_win, kCensusSeconds) > 0) Log::get().note("%s", line);
    else Log::get().note("effect history: the census line did not fit");
    g_win = EffectWindow();
}

}  // namespace

void effectHistoryInstall(const EffectHistoryEnv& env) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_env = env;
    g_installed = env.setRenderTargets && env.clearRenderTarget;
    // A fresh start: the window, the counters and the counter reader's baseline (a reinstall in the rig; once at launch in the DLL).
    g_win = EffectWindow();
    g_stats = EffectHistoryStats();
    g_frame = 0;
    g_idleFrames = 0;
    g_counterPending = false;
    g_counterHaveLast = false;
    g_viewedTotal = g_viewedAtCopy = g_viewedAtLast = 0;
    for (QuerySlot& q : g_mod->ring) q.pending = false;
    for (EyeSlot& s : g_mod->eye) s.armed = false;
    g_censusAt = GetTickCount64();
    char names[240] = {};
    size_t n = 0;
    for (uint32_t i = 0; i < kEffectHistoryPairCount && n + 40 < sizeof(names); ++i)
        n += static_cast<size_t>(std::snprintf(names + n, sizeof(names) - n, "%s%s %016llX/%016llX", i ? ", " : "", kEffectHistoryPairs[i].name,
                                               static_cast<unsigned long long>(kEffectHistoryPairs[i].vs),
                                               static_cast<unsigned long long>(kEffectHistoryPairs[i].ps)));
    Log::get().note("effect history: %s. On NVIDIA's upscaler, %u listed effect draw pairs (%s) are drawn once more into a private target; where one supplies at "
                    "least %.2f of a pixel's light (and more than %.3f luma) DLSS is told to start that pixel's history afresh. No key: the rule is on whenever the "
                    "pass runs DLSS. The line every 30 s reports it.",
                    g_installed ? "installed" : "NOT installed (missing hooks)", kEffectHistoryPairCount, names, static_cast<double>(kEffectHideShare),
                    static_cast<double>(kEffectHideFloor));
}

void effectHistoryShutdown() {
    std::lock_guard<std::mutex> lock(g_mu);
    detail::g_effectNvidia.store(false, std::memory_order_relaxed);
    g_installed = false;
    for (EyeSlot& s : g_mod->eye) s = EyeSlot();
    for (QuerySlot& q : g_mod->ring) q = QuerySlot();
    g_mod->states.clear();
}

void effectHistorySetNvidia(bool on) noexcept {
    if (on) g_idleFrames = 0;   // read and written on the pass's thread and the boundary's, a stale value only delays the lapse by a frame
    detail::g_effectNvidia.store(on, std::memory_order_relaxed);
}

void effectHistoryDraw(ID3D11DeviceContext* ctx, uint64_t vs, uint64_t ps, EffectReplayFn replay, void* arg) noexcept {
    try {
        drawImpl(ctx, vs, ps, replay, arg);
    } catch (...) {
    }
}

bool effectHistoryViews(const void* sceneDepth, uint32_t w, uint32_t h, ID3D11ShaderResourceView** light, ID3D11ShaderResourceView** frame) noexcept {
    if (!sceneDepth || !light || !frame) return false;
    try {
        std::lock_guard<std::mutex> lock(g_mu);
        EyeSlot* sp = nullptr;
        for (EyeSlot& c : g_mod->eye)
            if (c.depth.Get() == sceneDepth) sp = &c;
        if (!sp) return false;
        EyeSlot& s = *sp;
        if (!s.armed) return false;
        s.armed = false;   // asking consumes
        if (!s.tex || !s.srv || !s.hdr) return false;
        if (s.w != w || s.h != h || s.hdrW != w || s.hdrH != h) {
            ++g_win.targetUnreadable;
            return false;
        }
        if (!s.hdrBindable) {
            ++g_win.targetUnreadable;
            if (!g_unreadableSaid) {
                g_unreadableSaid = true;
                Log::get().note("effect history: the game's HDR target (%ux%u, format %d) cannot be bound as a shader input, so no pixel is hidden by the rule.",
                                s.hdrW, s.hdrH, static_cast<int>(s.hdrFmt));
            }
            return false;
        }
        if (!s.hdrSrv) {
            ComPtr<ID3D11Device> dev;
            s.hdr->GetDevice(dev.GetAddressOf());
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = s.hdrFmt;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            if (!dev || FAILED(dev->CreateShaderResourceView(s.hdr.Get(), &vd, s.hdrSrv.GetAddressOf())) || !s.hdrSrv) {
                s.hdrSrv.Reset();
                s.hdrBindable = false;
                ++g_win.targetUnreadable;
                return false;
            }
        }
        *light = s.srv.Get();
        *frame = s.hdrSrv.Get();
        ++g_stats.eyeFramesViewed;
        ++g_viewedTotal;
        return true;
    } catch (...) {
        return false;
    }
}

void effectHistoryFrameBoundary(ID3D11DeviceContext* ctx) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_installed) return;
        ++g_frame;
        if (detail::g_effectNvidia.load(std::memory_order_relaxed) && ++g_idleFrames > kLapseFrames) detail::g_effectNvidia.store(false, std::memory_order_relaxed);
        for (EyeSlot& s : g_mod->eye)
            if (s.armed && g_frame - s.armedFrame > kArmedMaxAge) s.armed = false;
        if (!ctx) return;
        for (QuerySlot& q : g_mod->ring) {
            if (!q.pending || !q.q) continue;
            UINT64 samples = 0;
            const HRESULT h = ctx->GetData(q.q.Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (h == S_FALSE) continue;
            q.pending = false;
            if (h == S_OK && q.pair < kEffectPairSlots) {
                g_win.samples[q.pair] += samples;
                ++g_win.sampled[q.pair];
            }
        }
        if (g_env.timerPoll) {
            double ms = 0.0;
            uint32_t n = 0, skipped = 0, invalid = 0;
            g_env.timerPoll(ctx, &ms, &n, &skipped, &invalid);
            g_win.gpuMs += ms;
            g_win.gpuSamples += n;
            g_win.gpuSkipped += skipped;
            g_win.gpuInvalid += invalid;
        }
        sampleCounter(ctx);
        ++g_win.frames;
        const ULONGLONG now = GetTickCount64();
        if (now - g_censusAt >= static_cast<ULONGLONG>(kCensusSeconds) * 1000ull) {
            g_censusAt = now;
            if (detail::g_effectNvidia.load(std::memory_order_relaxed) || g_win.eyeFrames) sayCensus();
            else g_win = EffectWindow();
        }
    } catch (...) {
    }
}

void effectHistoryCensusNow() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mu);
        sayCensus();
    } catch (...) {
    }
}

EffectHistoryStats effectHistoryStats() noexcept {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_stats;
}

}  // namespace edvr
