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

}  // namespace fsstiles
}  // namespace edvr
