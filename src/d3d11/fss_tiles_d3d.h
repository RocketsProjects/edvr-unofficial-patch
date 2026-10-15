// The D3D11 face of fss_tiles_core.h's Ops: what the hook and the WARP rig both call. Header-only so the rig runs the
// production code, not a copy.
#pragma once

#include <d3d11.h>
#include <d3d11_1.h>

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
    bool reduce(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D** stage, uint32_t* bw, uint32_t* bh, DXGI_FORMAT* typed, const char** why,
                uint32_t mip = 0, uint32_t slice = 0) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        // The mip and slice a view reads are what is reduced (a view of mip 1 is fingerprinted at mip 1's size); a multisampled source is refused.
        if (sd.SampleDesc.Count != 1) { *why = "the texture is multisampled"; return false; }
        if (mip >= sd.MipLevels) mip = 0;
        if (slice >= sd.ArraySize) slice = 0;
        const uint32_t subresource = mip + slice * sd.MipLevels;
        sd.Width = (sd.Width >> mip) ? (sd.Width >> mip) : 1u;
        sd.Height = (sd.Height >> mip) ? (sd.Height >> mip) : 1u;
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
        ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, src, subresource, nullptr);
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


// ---- v11: every input of a draw: pixel-shader textures, constant buffers, vertices, samplers ----------------------------------------------------------------------------------------
// What a capture holds on the GPU side until it is read: the block fingerprint stagings of the textures, one staging buffer for the constant buffer bytes and one for the vertex and index bytes.
struct InputsCap {
    InputsFacts      f;
    ID3D11Texture2D* st[kInSlots] = {};
    ID3D11Buffer*    cbSt = nullptr;
    ID3D11Buffer*    vbSt = nullptr;
    void release() {
        for (auto*& s : st) { if (s) { s->Release(); s = nullptr; } }
        if (cbSt) { cbSt->Release(); cbSt = nullptr; }
        if (vbSt) { vbSt->Release(); vbSt = nullptr; }
    }
};

inline bool ensureStageBuffer(ID3D11Device* dev, uint32_t bytes, ID3D11Buffer** st) {
    if (*st) return true;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(dev->CreateBuffer(&d, nullptr, st)) && *st;
}

// At the draw, on `ctx`: reads every binding the pixel shader and the vertex stage have (CPU side, at once) and queues the copies that need the GPU: the textures' block fingerprints
// (`red` is one reducer a slot, reused between calls), the constant buffers' bytes and the first vertices. `ctx1` may be null: the constant buffers are then read whole from the start.
inline void captureInputs(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11DeviceContext1* ctx1, uint32_t kind, uint32_t count, BlockReducer* red, InputsCap& c) {
    InputsFacts& f = c.f;
    f = InputsFacts();
    ID3D11ShaderResourceView* sv[kInSlots] = {};
    ctx->PSGetShaderResources(0, kInSlots, sv);
    for (uint32_t k = 0; k < kInSlots; ++k) {
        if (!sv[k]) continue;
        SlotInfo& s = f.slot[k];
        s.bound = true;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        sv[k]->GetDesc(&vd);
        s.viewDim = static_cast<uint32_t>(vd.ViewDimension);
        s.viewFmt = static_cast<uint32_t>(vd.Format);
        switch (vd.ViewDimension) {
            case D3D11_SRV_DIMENSION_TEXTURE2D: s.mostMip = vd.Texture2D.MostDetailedMip; s.mipLevels = vd.Texture2D.MipLevels; s.arraySize = 1; break;
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                s.mostMip = vd.Texture2DArray.MostDetailedMip; s.mipLevels = vd.Texture2DArray.MipLevels; s.firstSlice = vd.Texture2DArray.FirstArraySlice; s.arraySize = vd.Texture2DArray.ArraySize; break;
            case D3D11_SRV_DIMENSION_TEXTURECUBE: s.mostMip = vd.TextureCube.MostDetailedMip; s.mipLevels = vd.TextureCube.MipLevels; s.arraySize = 6; break;
            case D3D11_SRV_DIMENSION_TEXTURE3D: s.mostMip = vd.Texture3D.MostDetailedMip; s.mipLevels = vd.Texture3D.MipLevels; break;
            case D3D11_SRV_DIMENSION_BUFFER: s.firstElement = vd.Buffer.FirstElement; s.numElements = vd.Buffer.NumElements; break;
            case D3D11_SRV_DIMENSION_BUFFEREX: s.firstElement = vd.BufferEx.FirstElement; s.numElements = vd.BufferEx.NumElements; break;
            default: break;
        }
        ID3D11Resource* r = nullptr;
        sv[k]->GetResource(&r);
        if (r) {
            s.res = r;
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC bd{};
                static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
                s.isBuffer = true;
                s.bufBytes = bd.ByteWidth;
                s.fpWhy = "a buffer";
            } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td{};
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
                s.isTex2d = true;
                s.w = td.Width; s.h = td.Height; s.fmt = td.Format; s.resMips = td.MipLevels; s.resArray = td.ArraySize; s.samples = td.SampleDesc.Count;
                const char* why = "";
                if (red[k].reduce(dev, ctx, static_cast<ID3D11Texture2D*>(r), &c.st[k], &f.bw[k], &f.bh[k], reinterpret_cast<DXGI_FORMAT*>(&f.typed[k]), &why, s.mostMip, s.firstSlice)) s.fpQueued = true;
                else s.fpWhy = why;
            } else {
                s.fpWhy = "not a 2D texture or a buffer";
            }
            r->Release();   // the pointer stays as an identity only
        }
        sv[k]->Release();
    }
    // constant buffers
    for (uint32_t stage = 0; stage < 2; ++stage) {
        ID3D11Buffer* b[3] = {};
        UINT first[3] = {}, num[3] = {};
        if (ctx1) {
            if (stage == 0) ctx1->VSGetConstantBuffers1(0, 3, b, first, num);
            else ctx1->PSGetConstantBuffers1(0, 3, b, first, num);
        } else if (stage == 0) {
            ctx->VSGetConstantBuffers(0, 3, b);
        } else {
            ctx->PSGetConstantBuffers(0, 3, b);
        }
        for (uint32_t i = 0; i < 3; ++i) {
            if (!b[i]) continue;
            CbInfo& ci = f.cb[stage * 3 + i];
            ci.bound = true;
            ci.buf = b[i];
            ci.first = first[i];
            ci.num = num[i];
            D3D11_BUFFER_DESC bd{};
            b[i]->GetDesc(&bd);
            ci.bufBytes = bd.ByteWidth;
            const uint32_t off = first[i] * 16u;
            const uint32_t avail = bd.ByteWidth > off ? bd.ByteWidth - off : 0;
            uint32_t len = (num[i] && num[i] * 16u < avail) ? num[i] * 16u : avail;
            if (len > kInCbMax) len = kInCbMax;
            len &= ~15u;
            ci.bytes = len;
            if (len && ensureStageBuffer(dev, kInCb * kInCbMax, &c.cbSt)) {
                D3D11_BOX box{};
                box.left = off;
                box.right = off + len;
                box.bottom = box.back = 1;
                ctx->CopySubresourceRegion(c.cbSt, 0, (stage * 3 + i) * kInCbMax, 0, 0, b[i], 0, &box);
            } else {
                ci.bytes = 0;
            }
            b[i]->Release();
        }
    }
    // vertices, topology, layout, viewport, scissor
    VertexInfo& v = f.vtx;
    v.kind = kind;
    v.count = count;
    const bool indexed = kind == 'I' || kind == 'X' || kind == 'Z';
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, voff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibf = DXGI_FORMAT_UNKNOWN;
    UINT ioff = 0;
    ctx->IAGetIndexBuffer(&ib, &ibf, &ioff);
    ID3D11InputLayout* il = nullptr;
    ctx->IAGetInputLayout(&il);
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);
    v.topology = static_cast<uint32_t>(topo);
    v.layout = il;
    v.vb = vb;
    v.ib = ib;
    v.stride = stride;
    v.vbOffset = voff;
    v.ibFormat = static_cast<uint32_t>(ibf);
    v.ibOffset = ioff;
    if (vb && stride && ensureStageBuffer(dev, kInVbBytes + kInIbBytes, &c.vbSt)) {
        D3D11_BUFFER_DESC bd{};
        vb->GetDesc(&bd);
        v.vbSize = bd.ByteWidth;
        const uint32_t avail = bd.ByteWidth > voff ? bd.ByteWidth - voff : 0;
        uint32_t len = stride * (indexed ? 16u : kInVertices);
        if (len > avail) len = avail;
        if (len > kInVbBytes) len = kInVbBytes;
        len -= len % 4;
        if (len) {
            D3D11_BOX box{};
            box.left = voff;
            box.right = voff + len;
            box.bottom = box.back = 1;
            ctx->CopySubresourceRegion(c.vbSt, 0, 0, 0, 0, vb, 0, &box);
            v.vbBytes = len;
        }
    }
    if (indexed && ib && c.vbSt) {
        D3D11_BUFFER_DESC bd{};
        ib->GetDesc(&bd);
        const uint32_t avail = bd.ByteWidth > ioff ? bd.ByteWidth - ioff : 0;
        uint32_t len = 32;
        if (len > avail) len = avail;
        len -= len % 4;
        if (len) {
            D3D11_BOX box{};
            box.left = ioff;
            box.right = ioff + len;
            box.bottom = box.back = 1;
            ctx->CopySubresourceRegion(c.vbSt, 0, kInVbBytes, 0, 0, ib, 0, &box);
            v.ibBytes = len;
        }
    }
    if (il) il->Release();
    if (vb) vb->Release();
    if (ib) ib->Release();
    UINT nv = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&nv, &vp);
    v.vp[0] = vp.TopLeftX; v.vp[1] = vp.TopLeftY; v.vp[2] = vp.Width; v.vp[3] = vp.Height; v.vp[4] = vp.MinDepth; v.vp[5] = vp.MaxDepth;
    UINT ns = 1;
    D3D11_RECT sc{};
    ctx->RSGetScissorRects(&ns, &sc);
    v.sc[0] = sc.left; v.sc[1] = sc.top; v.sc[2] = sc.right; v.sc[3] = sc.bottom;
    // samplers s0-s3
    ID3D11SamplerState* sm[kInSamplers] = {};
    ctx->PSGetSamplers(0, kInSamplers, sm);
    for (uint32_t k = 0; k < kInSamplers; ++k) {
        if (!sm[k]) continue;
        D3D11_SAMPLER_DESC d{};
        sm[k]->GetDesc(&d);
        SamplerInfo& s = f.smp[k];
        s.bound = true;
        s.filter = static_cast<uint32_t>(d.Filter);
        s.addrU = static_cast<uint32_t>(d.AddressU);
        s.addrV = static_cast<uint32_t>(d.AddressV);
        s.addrW = static_cast<uint32_t>(d.AddressW);
        s.maxAniso = d.MaxAnisotropy;
        s.cmp = static_cast<uint32_t>(d.ComparisonFunc);
        for (int i = 0; i < 4; ++i) s.border[i] = d.BorderColor[i];
        s.lodBias = d.MipLODBias;
        s.minLod = d.MinLOD;
        s.maxLod = d.MaxLOD;
        sm[k]->Release();
    }
}

// Maps everything queued by captureInputs without waiting. False: not ready yet (nothing is consumed; call again later).
inline bool readInputs(ID3D11DeviceContext* ctx, InputsCap& c, InputsData& d) {
    D3D11_MAPPED_SUBRESOURCE m[kInSlots] = {};
    bool mapped[kInSlots] = {};
    D3D11_MAPPED_SUBRESOURCE mc{}, mv{};
    bool mappedC = false, mappedV = false;
    bool ok = true;
    for (uint32_t k = 0; k < kInSlots && ok; ++k) {
        if (!c.f.slot[k].fpQueued) continue;
        ok = c.st[k] && SUCCEEDED(ctx->Map(c.st[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[k]));
        mapped[k] = ok;
    }
    bool anyCb = false;
    for (uint32_t k = 0; k < kInCb; ++k) anyCb = anyCb || c.f.cb[k].bytes;
    if (ok && anyCb && c.cbSt) {
        ok = SUCCEEDED(ctx->Map(c.cbSt, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mc));
        mappedC = ok;
    }
    if (ok && (c.f.vtx.vbBytes || c.f.vtx.ibBytes) && c.vbSt) {
        ok = SUCCEEDED(ctx->Map(c.vbSt, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mv));
        mappedV = ok;
    }
    if (ok) {
        static float peak[kMaxBlocks * kMaxBlocks];
        for (uint32_t k = 0; k < kInSlots; ++k) {
            d.haveFp[k] = false;
            d.hash[k] = d.nz[k] = 0;
            if (!c.f.slot[k].fpQueued || !mapped[k]) continue;
            const uint32_t bw = c.f.bw[k], bh = c.f.bh[k], fmt = c.f.typed[k];
            const uint32_t n = bw * bh;
            if (!n || bw > kMaxBlocks || bh > kMaxBlocks) continue;
            colorBlocks(static_cast<const uint8_t*>(m[k].pData), m[k].RowPitch, fmt, bw, bh, peak);
            uint32_t bpp = bytesPerPixel(fmt);
            if (!bpp) bpp = 4;
            uint32_t h = 2166136261u, nz = 0;
            for (uint32_t y = 0; y < bh; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m[k].pData) + static_cast<size_t>(y) * m[k].RowPitch;
                for (uint32_t i = 0; i < bw * bpp; ++i) { h ^= row[i]; h *= 16777619u; }
            }
            for (uint32_t i = 0; i < n; ++i) {
                d.cls[k][i] = peakClass(peak[i]);
                if (peak[i] > 0.0f) ++nz;
            }
            d.hash[k] = h;
            d.nz[k] = nz;
            d.haveFp[k] = true;
            d.box[k][0] = d.box[k][1] = 0xFFFFu;
            d.box[k][2] = d.box[k][3] = 0;
            for (uint32_t y = 0; y < bh; ++y) {
                for (uint32_t x = 0; x < bw; ++x) {
                    if (!(peak[y * bw + x] > 0.0f)) continue;
                    if (x < d.box[k][0]) d.box[k][0] = static_cast<uint16_t>(x);
                    if (y < d.box[k][1]) d.box[k][1] = static_cast<uint16_t>(y);
                    if (x > d.box[k][2]) d.box[k][2] = static_cast<uint16_t>(x);
                    if (y > d.box[k][3]) d.box[k][3] = static_cast<uint16_t>(y);
                }
            }
        }
        for (uint32_t k = 0; k < kInCb; ++k) {
            if (c.f.cb[k].bytes && mappedC) std::memcpy(d.cb[k], static_cast<const uint8_t*>(mc.pData) + k * kInCbMax, c.f.cb[k].bytes);
        }
        if (mappedV) {
            if (c.f.vtx.vbBytes) std::memcpy(d.vb, mv.pData, c.f.vtx.vbBytes);
            if (c.f.vtx.ibBytes) std::memcpy(d.ib, static_cast<const uint8_t*>(mv.pData) + kInVbBytes, c.f.vtx.ibBytes);
        }
    }
    for (uint32_t k = 0; k < kInSlots; ++k) if (mapped[k]) ctx->Unmap(c.st[k], 0);
    if (mappedC) ctx->Unmap(c.cbSt, 0);
    if (mappedV) ctx->Unmap(c.vbSt, 0);
    return ok;
}

}  // namespace fsstiles
}  // namespace edvr
