#pragma once
// The effect-history list (effect_history.h; docs/dlss-blur-elements-2026-10-14.md): the shader PAIRS whose draws are bright, self-moving glows the camera
// term cannot describe, so that where such a draw supplies a real share of a pixel's light the pass tells DLSS to start that pixel's history afresh.
//
// One list, one header, so the scene inspector can add a pair after a capture names it. A pair is added only on evidence: a capture that shows the
// draw covering the element (the inspector's draw line), and what the shader is (a disassembly, a dump, or the flat recipes' notes).
//
// NOT on this list, on purpose (docs/dlss-blur-elements-2026-10-14.md, findings 3-5):
//   0357BBB2DEE43C1F / 81812EF97FB4A361, 8289669D93A18C1D / C6E6E419DA9F6FAD, A1B7CFCD0BE7493E / 2DB678B6B558B604   the deferred LIGHT VOLUMES (a 14-index box,
//     an 84-index sphere or cone, smaller instanced volumes; gpu_pass_profile.cpp:68). They are the scene's lighting: they are additive and cover every
//     lit pixel, so hiding history under them would remove anti-aliasing from the whole frame.
//   B75A6FF2CA9FA5D6 / D56F859BE4781431   the anchor billboard (flat_projection_recipes.h: "Billboard VS reads CB0 only at instructions 5-7 for anchor xyw"): it was on this
//     list on build D (v0.20.0-rc.1-39-gd3013197) and the census said 5.6-7.2 MILLION covered pixels per draw, about 2 draws a frame: it draws across the whole sky
//     (stars, the sun billboard, glare elements), and hiding history under it made the skybox shimmer. RULED OUT as an effect-only pair
//     (docs/dlss-blur-elements-2026-10-14.md). The inspector's per-draw coverage is how a pair is judged before it is added: a few thousand pixels, not millions.
//   124D7F3F649138D4 / 8085AE8DD1906CDC   an alpha quad over the G-buffer (the force field, cap 4): its output is albedo into four render targets, not light, so
//     "supplies a share of the pixel's light" does not describe it.
#include <cstddef>
#include <cstdint>

namespace edvr {

struct EffectPair {
    uint64_t vs, ps;
    const char* name;   // one word, for the log
};

constexpr EffectPair kEffectHistoryPairs[] = {
    // A stretched, age-ramped lit particle billboard (docs/shaders/particle-stretched-vs.asm: the quad is laid out along camera-supplied axes with a size
    // curve over the particle's age; the PS is lit, cb1[91], three material textures). Additive. Seen in inspector caps 5 and 6 (flight
    // v0.20.0-rc.1-4): sparks and trails that move on their own.
    {0x68DDDEF04D9894AFull, 0x06332CA168B6DA63ull, "particle-billboard"},
};
constexpr uint32_t kEffectHistoryPairCount = static_cast<uint32_t>(sizeof(kEffectHistoryPairs) / sizeof(kEffectHistoryPairs[0]));

// The list's index of a pair, or -1. Both hashes must match: a shader alone is not enough (the particle vertex shader is shared with other pixel
// shaders in the flat recipes).
constexpr int effectPairIndex(uint64_t vs, uint64_t ps) {
    for (uint32_t i = 0; i < kEffectHistoryPairCount; ++i)
        if (kEffectHistoryPairs[i].vs == vs && kEffectHistoryPairs[i].ps == ps) return static_cast<int>(i);
    return -1;
}
// Whether a vertex shader is on the list at all (the cheap first test the draw path makes before it reads the pixel shader).
constexpr bool effectVsListed(uint64_t vs) {
    for (uint32_t i = 0; i < kEffectHistoryPairCount; ++i)
        if (kEffectHistoryPairs[i].vs == vs) return true;
    return false;
}

}  // namespace edvr
