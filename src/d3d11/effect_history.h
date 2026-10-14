#pragma once
// The effect-history rule's device half (effect_history_core.h says what the rule is; docs/dlss-blur-elements-2026-10-14.md says why).
//
// Per listed eye draw (effect_history_pairs.h), BEFORE the game's own draw: the draw once more, through the real entry point, into a private
// R11G11B10_FLOAT target the size of the eye (one per eye, made on the first listed draw, 16 MB each at 2016x2017), with the draw's own blend,
// shaders and inputs, the GAME'S depth view (so a glow behind a wall counts for nothing) with the depth test unchanged and every depth and stencil
// write off. Nothing else the game bound is changed: the render targets and the depth-stencil state are put back before it returns.
// The temporal pass then hands the private target and the game's HDR target to the motion shader (effectHistoryViews), which hides DLSS's history
// at the pixels where the first supplies kEffectHideShare of the second's light. Unarmed (no listed draw, or the pass not on the NVIDIA path) the cost
// is one relaxed load per eye draw; with listed draws it is the replica per draw, and the census (every 30 s) prices it.
#include <atomic>
#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;

namespace edvr {

// What the module needs of the rest of the DLL, given once at install, so that it links alone into its test rig.
struct EffectHistoryEnv {
    // OMSetRenderTargets / ClearRenderTargetView past the binding shadow and the stage hooks (vScreenSetRenderTargetsRaw / vScreenClearRenderTargetViewRaw).
    void (*setRenderTargets)(ID3D11DeviceContext* ctx, uint32_t n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) = nullptr;
    void (*clearRenderTarget)(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const float colour[4]) = nullptr;
    // Optional GPU timing around a replica (a GpuIntervals in the DLL): begin returns false when it declined; poll once a frame returns the spans that
    // finished since the last call.
    bool (*timerBegin)(ID3D11DeviceContext* ctx) = nullptr;
    void (*timerEnd)(ID3D11DeviceContext* ctx) = nullptr;
    void (*timerPoll)(ID3D11DeviceContext* ctx, double* ms, uint32_t* samples, uint32_t* skipped, uint32_t* invalid) = nullptr;
    // The buffer holding the motion shader's running count of the pixels it hid (a uint at *byteOffset, which wraps; the pass's Stats buffer, element 40),
    // or null before the pass has made it. Read a few times a minute through a 4-byte staging copy, never waited on.
    ID3D11Buffer* (*hiddenCounter)(uint32_t* byteOffset) = nullptr;
};

void effectHistoryInstall(const EffectHistoryEnv& env);
void effectHistoryShutdown();

namespace detail {
extern std::atomic<bool> g_effectNvidia;   // the temporal pass is on the NVIDIA path (effectHistorySetNvidia): the rule only acts there
}
// The temporal pass says each eye-frame whether it runs NVIDIA's upscaler. FSR and XeSS are not handed the sentinel this rule relies on. The flag lapses by
// itself after a few seconds of the pass not saying (a menu, a loading screen), so no replica runs for a pass that is not there.
void effectHistorySetNvidia(bool on) noexcept;
inline bool effectHistoryLive() noexcept { return detail::g_effectNvidia.load(std::memory_order_relaxed); }

using EffectReplayFn = void (*)(void* arg);
// Offer one eye draw (the draw path asks only when effectHistoryLive() and its vertex shader is listed). Returns at once for a draw whose pair is not
// listed; otherwise replicates it as above. `replay` re-issues the draw through the real entry point, below every EDVR hook.
void effectHistoryDraw(ID3D11DeviceContext* ctx, uint64_t vs, uint64_t ps, EffectReplayFn replay, void* arg) noexcept;

// The temporal pass, per eye, once its motion dispatch is about to run, with the scene depth texture it reads for that eye (the eye's identity: the game
// draws an eye's effects against that depth, and the pass's own eye table is learned late in the frame and not for every target): true (and the two views,
// valid for the call) when a listed draw was replicated against that depth since the last time it asked, the private target and the game's HDR target are
// both w x h, and the HDR target can be read. Asking consumes: the next listed draw starts the eye over. false (views untouched) otherwise, counted when
// the HDR target is what failed.
bool effectHistoryViews(const void* sceneDepth, uint32_t w, uint32_t h, ID3D11ShaderResourceView** light, ID3D11ShaderResourceView** frame) noexcept;

// Once a frame, from the frame boundary: ages an eye whose pass never asked, polls the queries and the timer, prints the census every 30 s.
void effectHistoryFrameBoundary(ID3D11DeviceContext* ctx) noexcept;

// The census now (for the rig: the 30 s clock is the module's own): prints the window's line and starts a new window.
void effectHistoryCensusNow() noexcept;

// What the module has done, for the rig: replicas made and listed draws left alone since install, eye-frames handed to the shader.
struct EffectHistoryStats {
    uint64_t replicas = 0, refused = 0;
    uint32_t eyeFramesViewed = 0;
};
EffectHistoryStats effectHistoryStats() noexcept;

}  // namespace edvr
