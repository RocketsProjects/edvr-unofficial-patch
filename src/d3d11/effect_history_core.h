#pragma once
// The effect-history rule's pure half (effect_history.cpp; docs/dlss-blur-elements-2026-10-14.md). No Windows, no D3D11.
//
// WHAT IT DOES. A bright glow that moves on its own (a particle, a halo, a flare) has no motion vector of its own: DLSS takes the surface behind it, so its
// history drags the glow along the wrong path and smears it. EDVR cannot give such a pixel a truthful vector (there is no per-particle velocity), but it can
// tell DLSS to start the pixel's history afresh: the motion shader writes the out-of-range vector (2 x the frame size) it already writes for a masked engine
// pixel and for a background a nearer surface just uncovered (temporal_shader_source.h, `hidden`). This rule adds one more reason for that sentinel:
//
//     the effect draws on the list (effect_history_pairs.h) supply at least kEffectHideShare of the pixel's light.
//
// The effect's light is measured by drawing each listed draw once more, before the game's own, into a private R11G11B10 target with the draw's own blend,
// depth test (against the game's depth, so a glow behind a wall is not counted) and shaders, writing nothing else. The "pixel's light" is the game's HDR
// target at the temporal pass. Both are read in the motion shader, per pixel, only while an effect drew this eye-frame.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "effect_history_pairs.h"

namespace edvr {

// The share of a pixel's light the listed draws must supply for its history to be hidden. 0.35: the effect then contributes at least 0.54 times what is
// under it (E / (E + U) >= 0.35 means E >= 0.54 U), enough that its own motion, not the scene's, decides the pixel's appearance; below it the scene
// dominates, its vector is right, and the glow keeps DLSS's anti-aliasing (a faint halo around a lamp must not become a ring of aliased pixels).
// A named constant, not a key: nobody sets it per session; the census and the inspector's `effect_hidden` count are how a flight judges it.
constexpr float kEffectHideShare = 0.35f;
// The effect's own luma below which a pixel is never hidden, whatever the share (a black pixel's ratio is noise). Linear HDR units.
constexpr float kEffectHideFloor = 0.002f;
// Decision-flag bit the trace variant of the motion shader sets for a pixel hidden by this rule (temporal_shader_source.h decisionFlags; bits 12..14 are the
// engine kind). probe.w bit that says the two views are bound this eye-frame.
constexpr uint32_t kEffectDecisionBit = 32768u;
constexpr uint32_t kEffectProbeBit = 32768u;

constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;
inline float lumaOf(float r, float g, float b) noexcept {
    return std::max(r, 0.0f) * kLumaR + std::max(g, 0.0f) * kLumaG + std::max(b, 0.0f) * kLumaB;
}
// The rule on two lumas: the effect's and the pixel's whole.
inline bool effectHides(float effectLuma, float pixelLuma, float share = kEffectHideShare, float floorValue = kEffectHideFloor) noexcept {
    return effectLuma > floorValue && effectLuma >= share * pixelLuma;
}

// The same rule in HLSL, compiled by tools\effect_history_test against effectHides. temporal_shader_source.h carries a LITERAL copy (the tools that read the
// shader's raw literals, terrain_retired_test and the engine-velocity rig, cannot follow a macro); the rig fails if the copy differs from this text.
#define EDVR_EFFECT_HIDE_HLSL \
    "\n// effect_history_core.h: the effect-history rule on the effect's light (e) and the pixel's (f), linear HDR.\n" \
    "bool effectHideTest(float3 e, float3 f, float share, float floorValue) {\n" \
    "    const float3 lw = float3(0.2126, 0.7152, 0.0722);\n" \
    "    float le = dot(max(e, 0.0), lw), lf = dot(max(f, 0.0), lw);\n" \
    "    return le > floorValue && le >= share * lf;\n" \
    "}\n"
constexpr char kEffectHideHlsl[] = EDVR_EFFECT_HIDE_HLSL;

// ---- the census --------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kEffectPairSlots = 8;   // room beyond the list, so a later pair needs no change here
static_assert(kEffectHistoryPairCount <= kEffectPairSlots, "the census holds kEffectPairSlots pairs; widen it with the list");

struct EffectWindow {
    uint32_t frames = 0;                 // frame boundaries in the window
    uint32_t eyeFrames = 0;              // eye-frames in which a listed draw was replicated
    uint32_t draws[kEffectPairSlots] = {};      // listed draws replicated, per pair
    uint64_t samples[kEffectPairSlots] = {};    // samples the replicas wrote (occlusion queries answered), per pair: the pixels the pair covers, an upper bound on those hidden
    uint32_t sampled[kEffectPairSlots] = {};    // draws whose query answered (the divisor for samples)
    uint64_t hiddenPixels = 0;           // pixels the motion shader hid by this rule (its running total's growth), over hiddenEyeFrames
    uint64_t hiddenEyeFrames = 0;        // the eye-frames those pixels were counted over (eye-frames handed the two views)
    uint32_t counterReads = 0;           // times the running total was read; 0 means hiddenPixels is not a measurement
    uint32_t queryGaps = 0;              // draws whose query had not answered when its ring slot was reused
    uint32_t refused = 0;                // listed draws left alone (no scratch, a target that is not an eye, a UAV on the draw)
    double gpuMs = 0.0;                  // replica GPU time, summed over the timed samples
    uint32_t gpuSamples = 0, gpuSkipped = 0, gpuInvalid = 0;
    double scratchMiB = 0.0;             // the private targets held now
    bool targetReadable = true;          // the game's HDR target could be read in the motion shader (else nothing is hidden)
    uint32_t targetUnreadable = 0;       // eye-frames that wanted the target and could not read it
};

// "effect history: ..." one line, every 30 s while the pass is live (a heartbeat, printed with nothing replicated too).
inline int formatEffectCensus(char* out, size_t cap, const EffectWindow& w, uint32_t seconds) noexcept {
    int n = std::snprintf(out, cap, "effect history: last %u s, %u frames, %u eye-frames with a listed draw;", seconds, w.frames, w.eyeFrames);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    const double frames = std::max<double>(1.0, w.frames);
    {
        char piece[260];
        int m;
        if (w.counterReads > 0 && w.hiddenEyeFrames > 0)
            m = std::snprintf(piece, sizeof(piece), " pixels hidden: %.0f per eye-frame with a listed draw, %.0f per frame (both eyes; %llu eye-frames counted);",
                              static_cast<double>(w.hiddenPixels) / static_cast<double>(w.hiddenEyeFrames), static_cast<double>(w.hiddenPixels) / frames,
                              static_cast<unsigned long long>(w.hiddenEyeFrames));
        else
            m = std::snprintf(piece, sizeof(piece), " pixels hidden: not measured this window (%u reads of the GPU counter, %llu eye-frames);", w.counterReads,
                              static_cast<unsigned long long>(w.hiddenEyeFrames));
        if (m < 0 || static_cast<size_t>(n + m) >= cap) return -1;
        std::memcpy(out + n, piece, static_cast<size_t>(m) + 1);
        n += m;
    }
    for (uint32_t i = 0; i < kEffectHistoryPairCount; ++i) {
        char piece[200];
        const double perSample = w.sampled[i] ? static_cast<double>(w.samples[i]) / w.sampled[i] : 0.0;
        const int m = std::snprintf(piece, sizeof(piece), " %s: %.2f draws/frame, %.0f covered pixels per draw (%u sampled);", kEffectHistoryPairs[i].name,
                                    w.draws[i] / frames, perSample, w.sampled[i]);
        if (m < 0 || static_cast<size_t>(n + m) >= cap) return -1;
        std::memcpy(out + n, piece, static_cast<size_t>(m) + 1);
        n += m;
    }
    const int k = std::snprintf(out + n, cap - static_cast<size_t>(n),
        " replica GPU %.3f ms per replicated draw (%u timed, %u skipped, %u invalid); %u refused, %u unanswered queries; private targets %.1f MiB; the game's HDR target %s%s. "
        "Hidden pixels are not split by pair (one private target holds every pair's light): the covered pixels are per pair, an upper bound on the hidden ones "
        "(the share test, %.2f of the pixel's light, thins them); the inspector's box stats count the hidden pixels exactly.",
        w.gpuSamples ? w.gpuMs / w.gpuSamples : 0.0, w.gpuSamples, w.gpuSkipped, w.gpuInvalid, w.refused, w.queryGaps, w.scratchMiB,
        w.targetReadable ? "was readable" : "COULD NOT BE READ, so nothing was hidden", w.targetUnreadable ? " in some eye-frames" : "", static_cast<double>(kEffectHideShare));
    if (k < 0 || static_cast<size_t>(n + k) >= cap) return -1;
    return n + k;
}

}  // namespace edvr
