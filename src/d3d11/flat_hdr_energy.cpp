// The HDR route's energy instrument and measured exposure (flat_hdr_energy.h). TEMPORARY: design doc section 107.
#include "flat_hdr_energy.h"

#include "flat_compute_readback.h"  // FlatComputeInternalScope
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

namespace edvr {
namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr ULONGLONG kSampleIntervalMs = 500;
constexpr ULONGLONG kLineIntervalMs = 5000;

// One sample: the pass over H before the resolve (side 0) and after it (side 1), each its own buffer and staging copy, and
// the exposure state as it stood on the frame (when the measurement ran).
struct Slot {
    Ptr<ID3D11Buffer> buffer[2], stage[2];
    Ptr<ID3D11UnorderedAccessView> uav[2];
    Ptr<ID3D11Buffer> exposureStage;
    enum State { Free, InCounted, Pending } state = Free;
    bool exposureCopied = false;
    uint32_t width = 0, height = 0;
    char route[12] = {};
    char backend[16] = {};
    char exposure[12] = {};
    unsigned preset = 0;
    float threshold = 0;
};

ID3D11Device* g_device = nullptr;   // identity only: a different device drops everything below
Ptr<ID3D11ComputeShader> g_energyCs, g_exposureCs;
Ptr<ID3D11Buffer> g_energyParams, g_exposureParams, g_exposureState;
Ptr<ID3D11UnorderedAccessView> g_exposureStateUav, g_exposureTexUav;
Ptr<ID3D11Texture2D> g_exposureTex;
Slot g_slots[2];
int g_active = -1;                  // the slot whose input was counted this frame
ULONGLONG g_lastSampleMs = 0, g_windowStartMs = 0;
double g_lastGeoMean = 0;
FlatHdrEnergyWindow g_window;
char g_lastBackend[16] = {};
unsigned g_lastPreset = 0;
char g_lastExposure[12] = {};
uint32_t g_hazards = 0, g_dropped = 0, g_failures = 0, g_fallbacks = 0;
bool g_armedSaid = false;
bool g_exposureFresh = false;       // the 1x1 texture holds this frame's value
bool g_exposureRanThisFrame = false;

void releaseAll() {
    for (Slot& s : g_slots) s = Slot{};
    g_energyCs.Reset(); g_exposureCs.Reset(); g_energyParams.Reset(); g_exposureParams.Reset();
    g_exposureState.Reset(); g_exposureStateUav.Reset(); g_exposureTexUav.Reset(); g_exposureTex.Reset();
    g_device = nullptr; g_active = -1; g_exposureFresh = false;
}

bool sameDevice(ID3D11DeviceContext* ctx, Ptr<ID3D11Device>& dev) {
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (dev.Get() != g_device) { releaseAll(); g_device = dev.Get(); }
    return true;
}

bool readyEnergy(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11Device> dev;
    if (!sameDevice(ctx, dev)) return false;
    if (!g_energyCs) g_energyCs.Attach(shaderSwapCreateCs(ctx, kFlatHdrEnergyBytecode, sizeof(kFlatHdrEnergyBytecode),
                                                           "flat hdr energy", "flat hdr energy"));
    if (!g_energyCs) return false;
    if (!g_energyParams) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_energyParams))) return false;
    }
    for (Slot& s : g_slots) {
        for (int side = 0; side < 2; ++side) {
            if (s.buffer[side]) continue;
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = kFlatHdrEnergyWords * 4; bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, &s.buffer[side]))) return false;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kFlatHdrEnergyWords; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            if (FAILED(dev->CreateUnorderedAccessView(s.buffer[side].Get(), &ud, &s.uav[side]))) return false;
            D3D11_BUFFER_DESC sd{}; sd.ByteWidth = kFlatHdrEnergyWords * 4; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateBuffer(&sd, nullptr, &s.stage[side]))) return false;
        }
        if (!s.exposureStage) {
            D3D11_BUFFER_DESC sd{}; sd.ByteWidth = 32; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateBuffer(&sd, nullptr, &s.exposureStage))) return false;
        }
    }
    return true;
}

bool readyExposure(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11Device> dev;
    if (!sameDevice(ctx, dev)) return false;
    if (!g_exposureCs) g_exposureCs.Attach(shaderSwapCreateCs(ctx, kFlatHdrExposureBytecode, sizeof(kFlatHdrExposureBytecode),
                                                               "flat hdr exposure", "flat hdr exposure"));
    if (!g_exposureCs) return false;
    if (!g_exposureParams) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_exposureParams))) return false;
    }
    if (!g_exposureState) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_exposureState))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 8; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(dev->CreateUnorderedAccessView(g_exposureState.Get(), &ud, &g_exposureStateUav))) { g_exposureState.Reset(); return false; }
        const UINT zero[4] = {0, 0, 0, 0};
        ctx->ClearUnorderedAccessViewUint(g_exposureStateUav.Get(), zero);
    }
    if (!g_exposureTex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_exposureTex))) return false;
        if (FAILED(dev->CreateUnorderedAccessView(g_exposureTex.Get(), nullptr, &g_exposureTexUav))) { g_exposureTex.Reset(); return false; }
    }
    return true;
}

// Runs `cs` with the game's compute slot 0 (shader, SRV, two UAVs, constant buffer) saved and put back exactly. `uavCount` is 1
// or 2; `grid` is the dispatch. Returns false when H was still bound for output (the SRV read back null): counted, not dispatched.
bool dispatchOver(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11Buffer* params, ID3D11ShaderResourceView* h,
                  ID3D11UnorderedAccessView* uav0, ID3D11UnorderedAccessView* uav1, UINT gx, UINT gy) {
    Ptr<ID3D11ComputeShader> prior; ID3D11ClassInstance* classes[256]{}; UINT classCount = 256;
    ctx->CSGetShader(&prior, classes, &classCount);
    Ptr<ID3D11ShaderResourceView> savedSrv; ctx->CSGetShaderResources(0, 1, &savedSrv);
    Ptr<ID3D11UnorderedAccessView> savedUav[2]; ctx->CSGetUnorderedAccessViews(0, 2, savedUav[0].GetAddressOf());
    Ptr<ID3D11Buffer> savedCb; ctx->CSGetConstantBuffers(0, 1, &savedCb);
    ID3D11Buffer* cb = params;
    ID3D11UnorderedAccessView* uavs[2] = {uav0, uav1};
    ctx->CSSetShader(cs, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 1, &h);
    ctx->CSSetUnorderedAccessViews(0, uav1 ? 2 : 1, uavs, nullptr);
    // A target still bound for output refuses the read silently (the view reads back null).
    Ptr<ID3D11ShaderResourceView> bound; ctx->CSGetShaderResources(0, 1, &bound);
    const bool ok = bound.Get() == h;
    if (ok) ctx->Dispatch(gx, gy, 1);
    ID3D11ShaderResourceView* nullSrv = nullptr; ID3D11UnorderedAccessView* nullUavs[2] = {nullptr, nullptr};
    ctx->CSSetShaderResources(0, 1, &nullSrv); ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
    ctx->CSSetShader(prior.Get(), classes, classCount);
    ID3D11Buffer* oldCb = savedCb.Get(); ctx->CSSetConstantBuffers(0, 1, &oldCb);
    ID3D11ShaderResourceView* oldSrv = savedSrv.Get(); ctx->CSSetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUavs[2] = {savedUav[0].Get(), savedUav[1].Get()}; UINT keep[2] = {~0u, ~0u};
    ctx->CSSetUnorderedAccessViews(0, 2, oldUavs, keep);
    for (UINT i = 0; i < classCount; ++i) if (classes[i]) classes[i]->Release();
    return ok;
}

// One counting pass of `h` into the slot's side.
bool count(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, Slot& s, int side) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; h->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) { ++g_failures; return false; }
    uint32_t params[4] = {s.width, s.height, 0, 0};
    std::memcpy(&params[2], &s.threshold, 4);
    ctx->UpdateSubresource(g_energyParams.Get(), 0, nullptr, params, 0, 0);
    const UINT zero[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(s.uav[side].Get(), zero);
    const bool ok = dispatchOver(ctx, g_energyCs.Get(), g_energyParams.Get(), h, s.uav[side].Get(), nullptr, 32, 32);
    if (!ok) ++g_hazards;
    if (ok) ctx->CopyResource(s.stage[side].Get(), s.buffer[side].Get());
    return ok;
}

void copyText(char* dst, size_t size, const char* src) { std::snprintf(dst, size, "%s", src ? src : "unknown"); }

void flush(ULONGLONG now) {
    FlatHdrEnergyWindow& w = g_window;
    if (w.samples || w.frames) {
        if (!w.backend[0]) copyText(w.backend, sizeof(w.backend), g_lastBackend[0] ? g_lastBackend : "none");
        if (!w.exposure[0]) copyText(w.exposure, sizeof(w.exposure), g_lastExposure[0] ? g_lastExposure : "none");
        if (!w.preset) w.preset = g_lastPreset;
        w.hazards = g_hazards; w.dropped = g_dropped; w.failures = g_failures; w.fallbacks = g_fallbacks;
        char line[900];
        flatHdrEnergyFormatLine(line, sizeof(line), w);
        Log::get().note("%s", line);
    }
    g_window = FlatHdrEnergyWindow{};
    g_windowStartMs = now;
}

}  // namespace

bool flatHdrEnergyBegin(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, uint32_t width, uint32_t height, const char* route) {
    g_active = -1;
    if (!ctx || !h || !width || !height) return false;
    const ULONGLONG now = GetTickCount64();
    if (!g_windowStartMs) g_windowStartMs = now;
    if (g_window.route[0] && route && std::strcmp(g_window.route, route)) flush(now);   // a line never mixes two routes
    copyText(g_window.route, sizeof(g_window.route), route);
    g_window.width = width; g_window.height = height;
    ++g_window.frames;
    if (g_lastSampleMs && now - g_lastSampleMs < kSampleIntervalMs) return false;
    int slot = -1;
    for (int i = 0; i < 2; ++i) if (g_slots[i].state == Slot::Free) { slot = i; break; }
    if (slot < 0) { ++g_dropped; g_lastSampleMs = now; return false; }
    FlatComputeInternalScope internal;
    if (!readyEnergy(ctx)) { ++g_failures; g_lastSampleMs = now; return false; }
    if (!g_armedSaid) {
        g_armedSaid = true;
        Log::get().note("flat hdr energy: armed (temporary instrument, design doc section 107): the scene image before and after the "
                        "resolve, one sample every %llu ms, one line every %llu s; route=%s",
                        static_cast<unsigned long long>(kSampleIntervalMs), static_cast<unsigned long long>(kLineIntervalMs / 1000),
                        route ? route : "?");
    }
    Slot& s = g_slots[slot];
    s.width = width; s.height = height;
    copyText(s.route, sizeof(s.route), route);
    s.threshold = flatHdrEnergyThreshold(g_lastGeoMean);
    g_lastSampleMs = now;
    s.exposureCopied = false;
    if (g_exposureFresh && g_exposureState) {
        ctx->CopyResource(s.exposureStage.Get(), g_exposureState.Get());
        s.exposureCopied = true;
    }
    if (!count(ctx, h, s, 0)) return false;
    s.state = Slot::InCounted; g_active = slot;
    return true;
}

void flatHdrEnergyEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, const char* backend, unsigned preset,
                      const char* exposureName) {
    if (g_active < 0 || !ctx || !h) return;
    Slot& s = g_slots[g_active]; g_active = -1;
    FlatComputeInternalScope internal;
    copyText(s.backend, sizeof(s.backend), backend);
    copyText(s.exposure, sizeof(s.exposure), exposureName);
    s.preset = preset;
    if (!count(ctx, h, s, 1)) { s.state = Slot::Free; return; }
    s.state = Slot::Pending;
}

void flatHdrEnergyAbandon() {
    if (g_active >= 0) g_slots[g_active].state = Slot::Free;
    g_active = -1;
}

void flatHdrEnergyPoll(ID3D11DeviceContext* ctx) {
    g_exposureFresh = false;   // each treated frame measures its own exposure, or the evaluation falls back
    if (!ctx) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_windowStartMs) g_windowStartMs = now;
    FlatComputeInternalScope internal;
    for (Slot& s : g_slots) {
        if (s.state != Slot::Pending) continue;
        D3D11_MAPPED_SUBRESOURCE in{}, out{}, ex{};
        const HRESULT hrIn = ctx->Map(s.stage[0].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &in);
        if (hrIn == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        const HRESULT hrOut = SUCCEEDED(hrIn) ? ctx->Map(s.stage[1].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &out) : hrIn;
        if (hrOut == DXGI_ERROR_WAS_STILL_DRAWING) { ctx->Unmap(s.stage[0].Get(), 0); continue; }
        if (FAILED(hrIn) || FAILED(hrOut)) {
            if (SUCCEEDED(hrIn)) ctx->Unmap(s.stage[0].Get(), 0);
            ++g_failures; s.state = Slot::Free; continue;
        }
        const FlatHdrEnergySample a = flatHdrEnergyDecode(static_cast<const uint32_t*>(in.pData));
        const FlatHdrEnergySample b = flatHdrEnergyDecode(static_cast<const uint32_t*>(out.pData));
        ctx->Unmap(s.stage[1].Get(), 0); ctx->Unmap(s.stage[0].Get(), 0);
        float applied = 0.0f; bool haveApplied = false;
        if (s.exposureCopied && SUCCEEDED(ctx->Map(s.exposureStage.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ex))) {
            std::memcpy(&applied, static_cast<const uint8_t*>(ex.pData) + 8, sizeof(applied));
            haveApplied = true;
            ctx->Unmap(s.exposureStage.Get(), 0);
        }
        s.state = Slot::Free;
        FlatHdrEnergyWindow& w = g_window;
        if (w.samples && (std::strcmp(w.backend, s.backend) || w.preset != s.preset || std::strcmp(w.exposure, s.exposure) ||
                          std::strcmp(w.route, s.route) || w.width != s.width || w.height != s.height)) {
            const unsigned frames = w.frames;
            flush(now);   // a change of route, backend, preset, exposure or size closes the window: a line never mixes two
            g_window.frames = frames ? 1 : 0;
        }
        copyText(w.route, sizeof(w.route), s.route);
        std::memcpy(w.backend, s.backend, sizeof(w.backend)); std::memcpy(w.exposure, s.exposure, sizeof(w.exposure));
        w.preset = s.preset; w.width = s.width; w.height = s.height;
        copyText(g_lastBackend, sizeof(g_lastBackend), s.backend); copyText(g_lastExposure, sizeof(g_lastExposure), s.exposure);
        g_lastPreset = s.preset;
        ++w.samples;
        w.brightIn += a.bright; w.brightOut += b.bright; w.totalIn += a.total; w.totalOut += b.total;
        w.maxIn += a.max; w.maxOut += b.max;
        if (a.max > 0 && b.max / a.max > w.maxRatioPeak) w.maxRatioPeak = b.max / a.max;
        w.overIn += a.over; w.overOut += b.over; w.satIn += a.saturated; w.satOut += b.saturated;
        w.thresholdSum += s.threshold; w.geoMeanIn += a.geoMean; w.badIn += a.bad; w.badOut += b.bad;
        if (haveApplied) { w.exposureSum += applied; ++w.exposureSamples; }
        if (a.geoMean > 0) g_lastGeoMean = a.geoMean;
    }
    if (now - g_windowStartMs >= kLineIntervalMs) flush(now);
}

void flatHdrEnergyFlush() { flush(GetTickCount64()); }

bool flatHdrExposureUpdate(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, uint32_t width, uint32_t height, bool reset) {
    g_exposureFresh = false;
    if (!ctx || !h || !width || !height) return false;
    FlatComputeInternalScope internal;
    if (!readyExposure(ctx)) { ++g_failures; return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; h->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) { ++g_failures; return false; }
    uint32_t params[4] = {width, height, 0, reset ? 1u : 0u};
    const float alpha = static_cast<float>(kFlatHdrExposureAlpha);
    std::memcpy(&params[2], &alpha, 4);
    ctx->UpdateSubresource(g_exposureParams.Get(), 0, nullptr, params, 0, 0);
    if (!dispatchOver(ctx, g_exposureCs.Get(), g_exposureParams.Get(), h, g_exposureStateUav.Get(), g_exposureTexUav.Get(), 1, 1)) {
        ++g_hazards; return false;
    }
    g_exposureFresh = true;
    return true;
}

ID3D11Texture2D* flatHdrExposureTexture() { return g_exposureFresh ? g_exposureTex.Get() : nullptr; }

void flatHdrExposureCountFallback() { ++g_fallbacks; }

}  // namespace edvr
