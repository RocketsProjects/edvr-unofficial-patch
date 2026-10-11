#include "edge_black.h"
#include "edge_black_core.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"   // edvr::kEdgeBlackBytecode, from build\gen
#include "../common/log.h"
#include "../common/periodic_work.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace {

using namespace edgeblack;
template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr unsigned kDepth = kReadbackDepth;
constexpr unsigned kGiveUpAfter = 30;   // calls an unfinished readback is waited for (never blocked on) before the ring is let go

struct Slot {
    Ptr<ID3D11Buffer> staging;
    FrameInfo info;
};

struct EyeGpu {
    void* srcId = nullptr;                       // identity of the texture the view is over
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;    // its format, and the size it had, with the region: any change ends the history
    UINT texW = 0, texH = 0;
    uint32_t regionW = 0, regionH = 0;
    bool known = false;
    bool refusedNoted = false;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11Buffer> out;
    Ptr<ID3D11UnorderedAccessView> uav;
    Slot ring[kDepth];
    unsigned head = 0, count = 0, waited = 0;    // next slot to write, slots in flight, calls the oldest has been waited for
    float prevHead[12] = {};
    bool havePrevHead = false;
    int64_t prevHeadQpc = 0;
};

EyeGpu g_eye[2];
Ptr<ID3D11ComputeShader> g_cs;
bool g_csTried = false;
Ptr<ID3D11Buffer> g_cb;
Watch g_watch;
bool g_started = false, g_closed = false;
PeriodicWork g_work{"edge_black", "eyes"};

void emit(const char* line) { Log::get().note("%s", line); }

uint64_t nowMs() {
    const int64_t f = qpcFrequency();
    return f > 0 ? static_cast<uint64_t>(qpcNow()) * 1000ull / static_cast<uint64_t>(f) : 0;
}

// The format of the game's texture read through a plain typed view of its own family: the numbers are the texture's own encoding (an 8-bit texture reads as
// code / 255, a float texture as its float). sRGB-typed textures are refused (a view of them would hand back linear values, not the encoding the thresholds
// are about), as the temporal pass refuses them.
DXGI_FORMAT viewFormatOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return f;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

const char* formatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        default: return "an unsupported format";
    }
}

const char* encodingOf(DXGI_FORMAT view) {
    switch (view) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "code / 255";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "code / 1023";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "code / 65535";
        default: return "the float value";
    }
}

void noteStartOnce(DXGI_FORMAT format, DXGI_FORMAT view) {
    if (g_started) return;
    g_started = true;
    char line[640];
    formatStart(line, formatName(format), encodingOf(view));
    emit(line);
}

void refuse(EyeGpu& e, int eye, const char* why, DXGI_FORMAT format) {
    if (e.refusedNoted) return;
    e.refusedNoted = true;
    Log::get().note("edge black: eye %d is not watched: %s (the submitted texture is %s).", eye, why, formatName(format));
}

// Frames the eye has in flight are let go (a size change, a readback that never came): they are counted lost, and the history ends with them.
void dropRing(EyeGpu& e, int eye) {
    g_watch.noteLost(e.count);
    g_watch.resetEye(eye);
    e.count = 0;
    e.waited = 0;
}

// Reads what is ready, oldest first, and hands it to the judgement. Never waits: a buffer the GPU is still writing is left for the next call.
void drain(ID3D11DeviceContext* ctx, EyeGpu& e, int eye, uint64_t now) {
    while (e.count > 0) {
        const unsigned idx = (e.head + kDepth - e.count) % kDepth;
        Slot& slot = e.ring[idx];
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = ctx->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (++e.waited > kGiveUpAfter) dropRing(e, eye);
            return;
        }
        e.waited = 0;
        --e.count;
        if (FAILED(hr) || !mapped.pData) {
            g_watch.noteLost(1);
            continue;
        }
        g_watch.ingest(eye, static_cast<const float*>(mapped.pData), slot.info, now, emit);
        ctx->Unmap(slot.staging.Get(), 0);
    }
}

bool ensureShared(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!g_cs && !g_csTried) {
        g_csTried = true;
        g_cs.Attach(shaderSwapCreateCs(ctx, kEdgeBlackBytecode, sizeof(kEdgeBlackBytecode), "edge black", "edge black"));
    }
    if (!g_cs) return false;
    if (!g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) return false;
    }
    return true;
}

bool ensureEye(ID3D11Device* dev, EyeGpu& e) {
    if (!e.out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kPoints * sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(float);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kPoints;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &e.out)) || FAILED(dev->CreateUnorderedAccessView(e.out.Get(), &ud, &e.uav))) {
            e.out.Reset();
            e.uav.Reset();
            return false;
        }
    }
    for (unsigned i = 0; i < kDepth; ++i) {
        if (e.ring[i].staging) continue;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kPoints * sizeof(float);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &e.ring[i].staging))) return false;
    }
    return true;
}

}  // namespace

void edgeBlackSample(ID3D11Device* device, ID3D11Texture2D* tex, int eye, uint64_t seq, const uint32_t region[4], bool flipU, bool flipV, const float* head34) {
    if (!device || !tex || !region || eye < 0 || eye > 1 || g_closed) return;
    PeriodicWorkScope timing(g_work, 1);
    EyeGpu& e = g_eye[eye];
    Ptr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const DXGI_FORMAT view = viewFormatOf(td.Format);
    if (view == DXGI_FORMAT_UNKNOWN) { refuse(e, eye, "its format is not one the watch reads", td.Format); return; }
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) { refuse(e, eye, "the texture refuses a shader view", td.Format); return; }
    const uint32_t rw = region[2] > region[0] ? region[2] - region[0] : 0;
    const uint32_t rh = region[3] > region[1] ? region[3] - region[1] : 0;
    if (!rw || !rh || region[2] > td.Width || region[3] > td.Height) return;
    const uint64_t now = nowMs();

    drain(ctx.Get(), e, eye, now);

    // A change of format, size or submitted region ends the history: what is in flight belongs to the old picture.
    if (e.known && (e.format != td.Format || e.texW != td.Width || e.texH != td.Height || e.regionW != rw || e.regionH != rh)) {
        dropRing(e, eye);
        e.srv.Reset();
        e.srcId = nullptr;
    }
    e.known = true;
    e.format = td.Format;
    e.texW = td.Width;
    e.texH = td.Height;
    e.regionW = rw;
    e.regionH = rh;

    if (!ensureShared(device, ctx.Get()) || !ensureEye(device, e)) { refuse(e, eye, "its resources could not be made", td.Format); return; }
    if (e.srcId != static_cast<void*>(tex) || !e.srv) {
        e.srv.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = view;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(tex, &vd, &e.srv)) || !e.srv) { e.srcId = nullptr; refuse(e, eye, "a shader view of it could not be made", td.Format); return; }
        e.srcId = tex;
    }
    noteStartOnce(td.Format, view);

    if (e.count >= kDepth) {   // the readback has not kept up: this frame is not sampled, and the missing frame ends the history at the next one
        g_watch.noteLost(1);
        return;
    }

    FrameInfo info;
    info.seq = seq;
    SYSTEMTIME st;
    GetLocalTime(&st);
    info.wall = {st.wHour, st.wMinute, st.wSecond, st.wMilliseconds};
    const int64_t nowQpc = qpcNow();
    if (head34) {
        const double seconds = e.havePrevHead ? static_cast<double>(nowQpc - e.prevHeadQpc) / static_cast<double>(qpcFrequency()) : 0.0;
        info.headDegPerSec = headSpeedDegPerSec(e.havePrevHead ? e.prevHead : nullptr, head34, seconds);
        std::memcpy(e.prevHead, head34, sizeof(e.prevHead));
        e.havePrevHead = true;
        e.prevHeadQpc = nowQpc;
    }

    const int32_t params[8] = {static_cast<int32_t>(region[0]), static_cast<int32_t>(region[1]), static_cast<int32_t>(region[2]), static_cast<int32_t>(region[3]),
                               flipU ? 1 : 0, flipV ? 1 : 0, 0, 0};
    ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, params, 0, 0);
    ID3D11ShaderResourceView* srvs[1] = {e.srv.Get()};
    ID3D11UnorderedAccessView* uavs[1] = {e.uav.Get()};
    ID3D11Buffer* cbs[1] = {g_cb.Get()};
    ctx->CSSetShader(g_cs.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(kGrid / 8, kGrid / 8, 1);
    ID3D11ShaderResourceView* noSrv[1] = {nullptr};
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ID3D11Buffer* noCb[1] = {nullptr};
    ctx->CSSetShaderResources(0, 1, noSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, noCb);
    ctx->CSSetShader(nullptr, nullptr, 0);

    Slot& slot = e.ring[e.head];
    ctx->CopyResource(slot.staging.Get(), e.out.Get());
    slot.info = info;
    e.head = (e.head + 1) % kDepth;
    ++e.count;
}

void edgeBlackClose() {
    if (g_closed) return;
    g_closed = true;
    if (!g_started) return;
    g_watch.close(nowMs(), emit);
}

}  // namespace edvr
