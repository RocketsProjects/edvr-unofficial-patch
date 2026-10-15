// The flat bloom probe (flat_bloom_probe.h). TEMPORARY: design doc section 107.
#include "flat_bloom_probe.h"

#include "flat_compute_readback.h"  // FlatComputeInternalScope
#include "flat_hdr_energy.h"        // the reduction primitive and its decode
#include "exposure_fix.h"           // lookupShaderHash
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {
namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr ULONGLONG kSampleIntervalMs = 500;
constexpr ULONGLONG kLineIntervalMs = 5000;
constexpr unsigned kChainMax = 48;
enum Pt { kPtH = 0, kPtBloom = 1, kPtLdr = 2, kPts = 3 };

struct Point {
    Ptr<ID3D11Buffer> buf, stage;
    Ptr<ID3D11UnorderedAccessView> uav;
    bool taken = false;
    uint32_t w = 0, h = 0;
};
struct Slot {
    Point pt[kPts];
    Ptr<ID3D11Texture2D> stripStage;
    D3D11_TEXTURE2D_DESC stripDesc{};
    bool stripTaken = false;
    bool pending = false;
    bool bloomSeen = false, hAtBloom = false;
    char aa[12] = {};
};

ID3D11Device* g_device = nullptr;   // identity only
Slot g_slots[2];
int g_cur = -1;                      // the slot this sampled frame fills; -1 until its first full-screen draw
bool g_wants = false;                // this frame is sampled
bool g_keyOn = true;
bool g_armedSaid = false;
ULONGLONG g_lastSampleMs = 0, g_windowStartMs = 0;
double g_lastGm = 0;
bool g_haveH = false, g_haveTone = false;
Ptr<ID3D11Texture2D> g_toneTarget;
ID3D11DeviceContext* g_ctx = nullptr;
float g_threshold = 8.0f;
FlatBloomChainEntry g_chain[kChainMax];
unsigned g_chainN = 0;
char g_chainDumpedAa[12] = {};
FlatBloomProbeWindow g_window;
uint32_t g_hazards = 0, g_dropped = 0, g_failures = 0;

void copyAa(char* dst, size_t size) {
    const std::string mode = Config::get().requestedTemporalMode();
    std::snprintf(dst, size, "%s", mode.empty() ? "off" : mode.c_str());
}

void releaseAll() {
    for (Slot& s : g_slots) s = Slot{};
    g_device = nullptr; g_cur = -1; g_toneTarget.Reset();
}

bool ready(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (dev.Get() != g_device) { releaseAll(); g_device = dev.Get(); }
    for (Slot& s : g_slots) for (Point& p : s.pt) {
        if (p.buf) continue;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = kFlatHdrEnergyWords * 4; bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &p.buf))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kFlatHdrEnergyWords; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(dev->CreateUnorderedAccessView(p.buf.Get(), &ud, &p.uav))) return false;
        D3D11_BUFFER_DESC sd{}; sd.ByteWidth = kFlatHdrEnergyWords * 4; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &p.stage))) return false;
    }
    return true;
}

// One reduction of `srv` into the slot's point, then its staging copy.
bool measure(ID3D11DeviceContext* ctx, Slot& s, Pt which, ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, float threshold) {
    Point& p = s.pt[which];
    if (!flatHdrEnergyReduce(ctx, srv, p.uav.Get(), w, h, threshold)) return false;
    ctx->CopyResource(p.stage.Get(), p.buf.Get());
    p.taken = true; p.w = w; p.h = h;
    return true;
}

struct Seen {
    char kind = '.';
    uint32_t w = 0, h = 0, fmt = 0;
    Ptr<ID3D11ShaderResourceView> view;
};

void closeWindow(ULONGLONG now) {
    FlatBloomProbeWindow& w = g_window;
    if (w.frames || w.samples) {
        w.hazards = g_hazards; w.dropped = g_dropped; w.failures = g_failures;
        char line[1100];
        bloomFormatLine(line, sizeof(line), w);
        Log::get().note("%s", line);
    }
    g_window = FlatBloomProbeWindow{};
    g_windowStartMs = now;
}

void dumpChain(const char* aa) {
    if (!g_chainN || std::strcmp(aa, g_chainDumpedAa) == 0) return;
    std::snprintf(g_chainDumpedAa, sizeof(g_chainDumpedAa), "%s", aa);
    Log::get().note("flat bloom probe chain (aa=%s): %u full-screen draw(s) of the sampled frame, in order; t0..t3 are the pixel-shader "
                    "resources (H: = the scene image, 3d: = a 3D texture)", aa, g_chainN);
    char text[900];
    for (unsigned i = 0; i < g_chainN;) {
        size_t at = 0;
        text[0] = 0;
        for (unsigned k = 0; k < 4 && i < g_chainN; ++k, ++i) {
            char one[220];
            bloomFormatChainEntry(one, sizeof(one), i + 1, g_chain[i]);
            at += static_cast<size_t>(std::snprintf(text + at, sizeof(text) - at, "%s%s", k ? " ;; " : "", one));
        }
        Log::get().note("flat bloom probe chain: %s", text);
    }
}

}  // namespace

bool flatBloomProbeWants() { return g_wants; }

void flatBloomProbeFlush() { closeWindow(GetTickCount64()); }

void flatBloomProbeDraw(ID3D11DeviceContext* ctx, unsigned count) {
    if (!g_wants || count != 3 || !ctx) return;
    FlatComputeInternalScope internal;
    Ptr<ID3D11RenderTargetView> rtv; Ptr<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    if (!rtv) return;
    Ptr<ID3D11Resource> res; rtv->GetResource(&res);
    Ptr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return;
    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
    const bool hasDepth = dsv != nullptr;
    Ptr<ID3D11ShaderResourceView> views[4];
    ctx->PSGetShaderResources(0, 4, views[0].GetAddressOf());
    Seen seen[4];
    bool has3d = false;
    for (int i = 0; i < 4; ++i) {
        if (!views[i]) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; views[i]->GetDesc(&vd);
        Ptr<ID3D11Resource> r; views[i]->GetResource(&r);
        if (!r) continue;
        seen[i].view = views[i];
        if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D) {
            seen[i].kind = '3'; has3d = true;
            Ptr<ID3D11Texture3D> t3; D3D11_TEXTURE3D_DESC d3{};
            if (SUCCEEDED(r.As(&t3))) { t3->GetDesc(&d3); seen[i].w = d3.Width; seen[i].h = d3.Height; seen[i].fmt = d3.Format; }
        } else if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            Ptr<ID3D11Texture2D> t2; D3D11_TEXTURE2D_DESC d2{};
            if (SUCCEEDED(r.As(&t2))) { t2->GetDesc(&d2); seen[i].kind = '2'; seen[i].w = d2.Width; seen[i].h = d2.Height; seen[i].fmt = d2.Format; }
        }
    }
    if (g_cur < 0) {
        int slot = -1;
        for (int i = 0; i < 2; ++i) if (!g_slots[i].pending) { slot = i; break; }
        if (slot < 0) { ++g_dropped; g_wants = false; return; }
        if (!ready(ctx)) { ++g_failures; g_wants = false; return; }
        g_cur = slot;
        Slot& fresh = g_slots[slot];
        for (Point& p : fresh.pt) p.taken = false;
        fresh.stripTaken = false; fresh.bloomSeen = false; fresh.hAtBloom = false;
        copyAa(fresh.aa, sizeof(fresh.aa));
        g_chainN = 0; g_haveH = false; g_haveTone = false; g_toneTarget.Reset();
        g_ctx = ctx;
        g_threshold = flatHdrEnergyThreshold(g_lastGm);
    }
    Slot& s = g_slots[g_cur];
    // The chain entry (the pixel shader's hash is for reading the log by eye; nothing here depends on it).
    FlatBloomChainEntry entry;
    {
        Ptr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps, nullptr, nullptr);
        entry.ps = ps ? lookupShaderHash(ps.Get()) : 0;
        entry.targetW = td.Width; entry.targetH = td.Height; entry.targetFmt = td.Format;
        for (int i = 0; i < 4; ++i) { entry.kind[i] = seen[i].kind; entry.w[i] = seen[i].w; entry.h[i] = seen[i].h; entry.fmt[i] = seen[i].fmt; }
    }
    if (!g_haveH) {
        for (int i = 0; i < 4; ++i) {
            if (seen[i].kind != '2') continue;
            if (!bloomIsFirstLevel(td.Width, td.Height, td.Format, hasDepth, seen[i].w, seen[i].h, seen[i].fmt)) continue;
            if (measure(ctx, s, kPtH, seen[i].view.Get(), seen[i].w, seen[i].h, g_threshold)) {
                g_haveH = true; s.hAtBloom = true; entry.isH[i] = true;
            }
            break;
        }
    }
    if (!g_haveTone && bloomIsTone(td.Width, td.Format, hasDepth, has3d)) {
        // H at the tone when the bloom's first level was not seen (Bloom off, or the depth-of-field result): the largest
        // R11G11B10_FLOAT 2D resource. The bloom texture: a smaller one.
        int bigIdx = -1, smallIdx = -1;
        for (int i = 0; i < 4; ++i) {
            if (seen[i].kind != '2' || seen[i].fmt != kBloomFmtR11G11B10) continue;
            if (seen[i].w >= kBloomMinSceneWidth) { if (bigIdx < 0 || seen[i].w > seen[bigIdx].w) bigIdx = i; }
            else if (seen[i].w >= 2) { if (smallIdx < 0 || seen[i].w > seen[smallIdx].w) smallIdx = i; }
        }
        if (!g_haveH && bigIdx >= 0 && measure(ctx, s, kPtH, seen[bigIdx].view.Get(), seen[bigIdx].w, seen[bigIdx].h, g_threshold)) {
            g_haveH = true; s.hAtBloom = false; entry.isH[bigIdx] = true;
        }
        if (smallIdx >= 0 && measure(ctx, s, kPtBloom, seen[smallIdx].view.Get(), seen[smallIdx].w, seen[smallIdx].h, 0.0f)) s.bloomSeen = true;
        // The exposure strip: the vertex shader's slot 0.
        Ptr<ID3D11ShaderResourceView> vsView;
        ctx->VSGetShaderResources(0, 1, &vsView);
        if (vsView) {
            Ptr<ID3D11Resource> r; vsView->GetResource(&r);
            Ptr<ID3D11Texture2D> st;
            D3D11_TEXTURE2D_DESC sd{};
            if (r && SUCCEEDED(r.As(&st))) {
                st->GetDesc(&sd);
                if (bloomIsStrip(sd.Width, sd.Height, sd.Format)) {
                    Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
                    if (!s.stripStage || s.stripDesc.Format != sd.Format) {
                        D3D11_TEXTURE2D_DESC stg = sd;
                        stg.Usage = D3D11_USAGE_STAGING; stg.BindFlags = 0; stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ; stg.MiscFlags = 0;
                        stg.MipLevels = 1; stg.ArraySize = 1;
                        s.stripStage.Reset();
                        if (dev && SUCCEEDED(dev->CreateTexture2D(&stg, nullptr, &s.stripStage))) s.stripDesc = stg;
                    }
                    if (s.stripStage) { ctx->CopySubresourceRegion(s.stripStage.Get(), 0, 0, 0, 0, st.Get(), 0, nullptr); s.stripTaken = true; }
                }
            }
        }
        g_toneTarget = tex;
        g_haveTone = true;
    }
    if (g_chainN < kChainMax) g_chain[g_chainN++] = entry;
}

void flatBloomProbeFrameEnd() {
    const ULONGLONG now = GetTickCount64();
    if (!g_windowStartMs) g_windowStartMs = now;
    FlatComputeInternalScope internal;
    const bool wasSampling = g_wants;
    g_wants = false;
    if (wasSampling) {
        ++g_window.frames;
        if (!g_window.aa[0]) copyAa(g_window.aa, sizeof(g_window.aa));
        if (g_cur >= 0 && g_ctx) {
            Slot& s = g_slots[g_cur];
            // The tone's target as the player sees it, now the frame is done with it.
            if (g_haveTone && g_toneTarget) {
                Ptr<ID3D11Device> dev; g_ctx->GetDevice(&dev);
                D3D11_TEXTURE2D_DESC td{}; g_toneTarget->GetDesc(&td);
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                Ptr<ID3D11ShaderResourceView> ldr;
                if (dev && SUCCEEDED(dev->CreateShaderResourceView(g_toneTarget.Get(), &vd, &ldr)))
                    measure(g_ctx, s, kPtLdr, ldr.Get(), td.Width, td.Height, 0.95f);
            }
            s.pending = s.pt[kPtH].taken || s.pt[kPtBloom].taken || s.pt[kPtLdr].taken || s.stripTaken;
            if (!g_armedSaid) {
                g_armedSaid = true;
                Log::get().note("flat bloom probe: armed (temporary instrument, design doc section 107): the bloom's input, the bloom texture, "
                                "the exposure strip and the tone's output, one sampled frame every %llu ms, one line every %llu s; "
                                "aa=%s", static_cast<unsigned long long>(kSampleIntervalMs),
                                static_cast<unsigned long long>(kLineIntervalMs / 1000), s.aa);
            }
            dumpChain(s.aa);
            if (!s.pending) ++g_failures;
        }
        g_cur = -1; g_haveH = false; g_haveTone = false; g_toneTarget.Reset();
    }
    // Late readback of the finished slots, never waiting.
    if (g_ctx) {
        for (Slot& s : g_slots) {
            if (!s.pending) continue;
            D3D11_MAPPED_SUBRESOURCE m[kPts]{}, sm{};
            bool mapped[kPts] = {}, stripMapped = false, busy = false, failed = false;
            for (int i = 0; i < kPts && !busy && !failed; ++i) {
                if (!s.pt[i].taken) continue;
                const HRESULT hr = g_ctx->Map(s.pt[i].stage.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[i]);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) busy = true; else if (FAILED(hr)) failed = true; else mapped[i] = true;
            }
            if (!busy && !failed && s.stripTaken) {
                const HRESULT hr = g_ctx->Map(s.stripStage.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &sm);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) busy = true; else if (FAILED(hr)) failed = true; else stripMapped = true;
            }
            if (!busy && !failed) {
                FlatBloomProbeWindow& w = g_window;
                if (w.samples && std::strcmp(w.aa, s.aa) != 0) closeWindow(now);
                std::snprintf(w.aa, sizeof(w.aa), "%s", s.aa);
                ++w.samples; ++w.toneSamples;
                if (mapped[kPtH]) {
                    const FlatHdrEnergySample h = flatHdrEnergyDecode(static_cast<const uint32_t*>(m[kPtH].pData));
                    w.hAtBloom = s.hAtBloom; w.hW = s.pt[kPtH].w; w.hH = s.pt[kPtH].h;
                    w.hMean += h.finite ? h.total / h.finite : 0.0; w.hMax += h.max; w.hGm += h.geoMean;
                    if (h.geoMean > 0) g_lastGm = h.geoMean;
                }
                if (mapped[kPtBloom]) {
                    const FlatHdrEnergySample b = flatHdrEnergyDecode(static_cast<const uint32_t*>(m[kPtBloom].pData));
                    w.bloomSeen = true; ++w.bloomSamples; w.bloomW = s.pt[kPtBloom].w; w.bloomH = s.pt[kPtBloom].h;
                    w.bloomMean += b.finite ? b.total / b.finite : 0.0; w.bloomMax += b.max;
                }
                if (mapped[kPtLdr]) {
                    const FlatHdrEnergySample l = flatHdrEnergyDecode(static_cast<const uint32_t*>(m[kPtLdr].pData));
                    ++w.ldrSamples;
                    w.ldrMean += l.finite ? l.total / l.finite : 0.0;
                    w.ldrWhite += l.finite ? static_cast<double>(l.over) / l.finite : 0.0; w.ldrMax += l.max;
                }
                if (stripMapped) {
                    float v[6];
                    std::memcpy(v, sm.pData, sizeof(v));
                    if (!w.stripSamples) w.stripMin0 = w.stripMax0 = v[0];
                    if (v[0] < w.stripMin0) w.stripMin0 = v[0];
                    if (v[0] > w.stripMax0) w.stripMax0 = v[0];
                    for (int k = 0; k < 6; ++k) w.strip[k] += v[k];
                    ++w.stripSamples;
                }
            }
            for (int i = 0; i < kPts; ++i) if (mapped[i]) g_ctx->Unmap(s.pt[i].stage.Get(), 0);
            if (stripMapped) g_ctx->Unmap(s.stripStage.Get(), 0);
            if (busy) continue;
            if (failed) ++g_failures;
            s.pending = false;
        }
    }
    if (now - g_windowStartMs >= kLineIntervalMs) closeWindow(now);
    // The next frame's decision: the key, the interval.
    g_keyOn = _stricmp(Config::get().getString("advanced.flat_bloom_probe", "on").c_str(), "off") != 0;
    if (g_keyOn && (!g_lastSampleMs || now - g_lastSampleMs >= kSampleIntervalMs)) { g_wants = true; g_lastSampleMs = now; }
}

}  // namespace edvr
