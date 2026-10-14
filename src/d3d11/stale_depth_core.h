#pragma once
// The stale-marker depth check (docs/dlss-blur-elements-2026-10-14.md; the VR motion shader, temporal_shader_source.h `staleMarkerDepthHidden`). No Windows, no D3D11.
//
// WHAT IT DOES. An engine-record pixel of kind 4 carries a marker whose depth is not the scene's: the pixel's owner is a draw that wrote no marker, so the record
// says nothing about it and the pixel takes the camera term. If that surface MOVES (an elevator's railing), the camera term sends the history to where the
// surface was not, and DLSS blends the background that was there. The flat runtime asks the same pixel one more question (flat_mono_shader_source.h
// `historyDepthMatches`, flat_mono_resolve.cpp "steady detail"): does last frame's depth, at the position the camera term names, hold the depth the camera
// term predicts? If not, history is refused. This is that rule, ported: the previous depth is the pass's own ZP (last frame's depth copy, raw), the expectation
// the camera term's predicted depth in the same encoding, the tolerance 1% of the pixel's encoded depth either way, the four texels around the previous raster
// position. A pixel the rule cannot judge (no previous depth, no predicted depth, the position off the image) is left as it was.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

constexpr float kStaleDepthRel = 0.01f;      // as flat's kStaleDepthRel: 1% of the encoded depth, nearer or farther
constexpr float kStaleDepthFloor = 1e-6f;    // as flat's kStaleDepthFloor
// The decision-flag bit the trace variant sets for a pixel this rule hid (temporal_shader_source.h decisionFlags; bit 15 is the effect rule's, 16 hidden).
constexpr uint32_t kStaleDepthDecisionBit = 65536u;

// The four previous-depth samples around the previous raster position (raw), the depth the camera term predicts (raw, A + B / z), and the encoding's offset A
// (knobs.x): true when one of the four is within tolerance of the prediction, which is the camera term confirmed.
inline bool staleDepthConfirms(const float prev[4], float expected, float depthOffset) noexcept {
    const float tol = std::max(kStaleDepthFloor, (expected - depthOffset) * kStaleDepthRel);
    float best = 3.402823e38f;
    for (int i = 0; i < 4; ++i) best = std::min(best, std::fabs(prev[i] - expected));
    return best <= tol;
}

// The same rule in HLSL, compiled by tools\effect_history_test against staleDepthConfirms. temporal_shader_source.h carries a LITERAL copy (the tools that read
// the shader's raw literals cannot follow a macro); the rig fails if the copy differs from this text.
#define EDVR_STALE_DEPTH_HLSL \
    "\n// stale_depth_core.h: the stale-marker depth check on the four previous-depth samples around the previous raster position.\n" \
    "bool staleDepthConfirms(float4 prev, float expected, float depthOffset) {\n" \
    "    float tol = max(1e-6, (expected - depthOffset) * 0.01);\n" \
    "    float4 d = abs(prev - expected);\n" \
    "    return min(min(d.x, d.y), min(d.z, d.w)) <= tol;\n" \
    "}\n"
constexpr char kStaleDepthHlsl[] = EDVR_STALE_DEPTH_HLSL;

// ---- the census --------------------------------------------------------------------------------------------------------------------------

struct StaleDepthWindow {
    uint64_t hiddenPixels = 0;     // pixels the motion shader hid by this rule (the running total's growth), over passEyeFrames
    uint64_t passEyeFrames = 0;    // the eye-frames of NVIDIA's pass those were counted over
    uint32_t counterReads = 0;     // times the running total was read; 0 means the figure is not a measurement
    uint32_t frames = 0;
};
// "kind-4 depth check: ..." one line, every 30 s while the pass is live.
inline int formatStaleDepthCensus(char* out, size_t cap, const StaleDepthWindow& w, uint32_t seconds) noexcept {
    if (w.counterReads > 0 && w.passEyeFrames > 0)
        return std::snprintf(out, cap,
            "kind-4 depth check: last %u s, %u frames: %.1f pixels hidden per eye-frame (%llu eye-frames counted). A stale-marker pixel (a marker whose depth is not the "
            "scene's: another draw owns it) keeps its history only where last frame's depth at the camera term's position confirms it, within %.0f%% (flat's rule); "
            "a moving rail over background shows here, a still scene shows next to none.",
            seconds, w.frames, static_cast<double>(w.hiddenPixels) / static_cast<double>(w.passEyeFrames), static_cast<unsigned long long>(w.passEyeFrames),
            static_cast<double>(kStaleDepthRel) * 100.0);
    return std::snprintf(out, cap, "kind-4 depth check: last %u s, %u frames: not measured this window (%u reads of the GPU counter, %llu eye-frames).", seconds, w.frames,
                         w.counterReads, static_cast<unsigned long long>(w.passEyeFrames));
}

}  // namespace edvr
