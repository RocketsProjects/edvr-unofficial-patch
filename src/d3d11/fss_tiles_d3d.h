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

}  // namespace fsstiles
}  // namespace edvr
