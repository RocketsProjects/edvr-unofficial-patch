// The D3D11 face of fss_tiles_core.h's Ops: what the hook and the WARP rig both call. Header-only so the rig runs the
// production code, not a copy.
#pragma once

#include <d3d11.h>

#include <cstdint>

#include "fss_tiles_core.h"

namespace edvr {
namespace fsstiles {

struct D3dOps {
    ID3D11DeviceContext* ctx = nullptr;
    // The un-hooked DispatchIndirect (or a plain member call in the rig).
    void (*real)(ID3D11DeviceContext*, ID3D11Buffer*, UINT) = nullptr;

    void* getSrv(uint32_t slot) {
        ID3D11ShaderResourceView* v = nullptr;
        ctx->CSGetShaderResources(slot, 1, &v);
        return v;   // a reference; swapBack releases it
    }
    void setSrv(uint32_t slot, void* view) {
        ID3D11ShaderResourceView* v = static_cast<ID3D11ShaderResourceView*>(view);
        ctx->CSSetShaderResources(slot, 1, &v);
    }
    void release(void* view) {
        if (view) static_cast<IUnknown*>(view)->Release();
    }
    void dispatchIndirect(void* args, uint32_t off) {
        if (real) real(ctx, static_cast<ID3D11Buffer*>(args), off);
        else ctx->DispatchIndirect(static_cast<ID3D11Buffer*>(args), off);
    }
};

// A CPU-readable staging twin of a buffer, made to match the source's size and structure so a CopyResource into it is
// legal for structured and raw buffers alike (the SDK's BasicCompute11 readback does the same). `*st` is reused while the
// size and structure still match, and replaced when they do not.
inline bool ensureStagingLike(ID3D11Device* dev, ID3D11Buffer* src, ID3D11Buffer** st) {
    D3D11_BUFFER_DESC sd{};
    src->GetDesc(&sd);
    D3D11_BUFFER_DESC d = sd;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags &= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) d.StructureByteStride = 0;
    if (*st) {
        D3D11_BUFFER_DESC have{};
        (*st)->GetDesc(&have);
        if (have.ByteWidth == d.ByteWidth && have.MiscFlags == d.MiscFlags && have.StructureByteStride == d.StructureByteStride) return true;
        (*st)->Release();
        *st = nullptr;
    }
    return SUCCEEDED(dev->CreateBuffer(&d, nullptr, st));
}


// ---- v10: a render target and a depth buffer, reduced to 16 x 16 pixel blocks -------------------------------------------------------------------------------------------------
inline DXGI_FORMAT typedOfFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return f;
    }
}

// A CPU-readable staging texture of the given size and format, reused while they match.
inline bool ensureStageTexture(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, ID3D11Texture2D** st) {
    if (*st) {
        D3D11_TEXTURE2D_DESC have{};
        (*st)->GetDesc(&have);
        if (have.Width == w && have.Height == h && have.Format == fmt) return true;
        (*st)->Release();
        *st = nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, st)) && *st;
}

// Queues, on `ctx`, the reduction of `src` to one texel per 16 x 16 block (the mip chain averages) into `*stage`. The mip texture is reused while the source's shape and format
// match. `why` names a refusal.
struct BlockReducer {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    void release() {
        if (srv) { srv->Release(); srv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
        w = h = 0;
        fmt = DXGI_FORMAT_UNKNOWN;
    }
    bool reduce(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D** stage, uint32_t* bw, uint32_t* bh, DXGI_FORMAT* typed, const char** why) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.MipLevels != 1 || sd.ArraySize != 1 || sd.SampleDesc.Count != 1) { *why = "the target is multisampled, mipped or an array"; return false; }
        if (sd.Width < kBlockPx || sd.Height < kBlockPx || sd.Width / kBlockPx > kMaxBlocks || sd.Height / kBlockPx > kMaxBlocks) { *why = "the target is too small or too large for 16 px blocks"; return false; }
        const DXGI_FORMAT t = typedOfFormat(sd.Format);
        UINT sup = 0;
        if (FAILED(dev->CheckFormatSupport(t, &sup)) || !(sup & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) || !(sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) { *why = "the format cannot generate mips"; return false; }
        if (!tex || w != sd.Width || h != sd.Height || fmt != sd.Format) {
            release();
            D3D11_TEXTURE2D_DESC md{};
            md.Width = sd.Width;
            md.Height = sd.Height;
            md.MipLevels = kBlockLevel + 1;
            md.ArraySize = 1;
            md.Format = t;
            md.SampleDesc.Count = 1;
            md.Usage = D3D11_USAGE_DEFAULT;
            md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev->CreateTexture2D(&md, nullptr, &tex)) || !tex) { tex = nullptr; *why = "the mip texture could not be made"; return false; }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = t;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = kBlockLevel + 1;
            if (FAILED(dev->CreateShaderResourceView(tex, &sv, &srv)) || !srv) { release(); *why = "the mip view could not be made"; return false; }
            w = sd.Width;
            h = sd.Height;
            fmt = sd.Format;
        }
        const uint32_t ow = sd.Width >> kBlockLevel, oh = sd.Height >> kBlockLevel;
        if (!ensureStageTexture(dev, ow, oh, t, stage)) { *why = "the staging texture could not be made"; return false; }
        ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, src, 0, nullptr);
        ctx->GenerateMips(srv);
        ctx->CopySubresourceRegion(*stage, 0, 0, 0, 0, tex, kBlockLevel, nullptr);
        *bw = ow;
        *bh = oh;
        *typed = t;
        return true;
    }
};

// Queues a copy of a depth-stencil texture into a staging twin of the same description. The twin keeps the source's depth format, which is what a CopyResource requires.
inline bool queueDepthCopy(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Resource* depth, ID3D11Texture2D** stage, D3D11_TEXTURE2D_DESC* outDesc, const char** why) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    depth->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { *why = "the depth view is not a 2D texture"; return false; }
    D3D11_TEXTURE2D_DESC sd{};
    static_cast<ID3D11Texture2D*>(depth)->GetDesc(&sd);
    if (sd.SampleDesc.Count != 1 || sd.ArraySize != 1 || sd.MipLevels != 1) { *why = "the depth texture is multisampled, mipped or an array"; return false; }
    if (depthKind(sd.Format) == DepthKind::None) { *why = "an unknown depth format"; return false; }
    if (*stage) {
        D3D11_TEXTURE2D_DESC have{};
        (*stage)->GetDesc(&have);
        if (have.Width != sd.Width || have.Height != sd.Height || have.Format != sd.Format) { (*stage)->Release(); *stage = nullptr; }
    }
    if (!*stage) {
        D3D11_TEXTURE2D_DESC d = sd;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, stage)) || !*stage) { *stage = nullptr; *why = "a staging twin of the depth texture could not be made"; return false; }
    }
    ctx->CopyResource(*stage, depth);
    *outDesc = sd;
    return true;
}

}  // namespace fsstiles
}  // namespace edvr
