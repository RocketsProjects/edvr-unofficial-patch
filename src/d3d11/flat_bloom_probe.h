// The flat bloom probe: what the game's bloom reads, what it makes, the exposure the tone is given and what the tone puts on the
// screen, measured the same way with the anti-aliasing OFF and ON. TEMPORARY: the 2026-10-14 DLAA bloom entry in
// docs/design-flat-temporal-aa-2026-09-23.md, section 107 (listed in its Status block; removed when that arc closes).
//
// THE QUESTION. With DLAA or DLSS on the flat HDR route and the game's Bloom on, stars, planets' clouds and atmosphere rims come out
// washed out; AA off and Bloom off are both normal. The energy line (flat_hdr_energy.h) showed the resolve lowers H's peaks and
// conserves its total, so the surplus is not DLSS adding energy to H. This probe measures the rest of the chain where the picture is made,
// and it runs with the anti-aliasing off, which the energy line cannot, so one flight holds the two sides of the A/B.
//
// WHAT IT MEASURES, on one frame in about thirty (500 ms), found by structure and not by shader hash (every hash here changes with
// the game's settings, EDHM and the quality tier):
//   H       the scene image the game's bloom reads. The bloom's first level is the first full-screen triangle that draws into an
//           R11G11B10_FLOAT target of half a 2D R11G11B10_FLOAT texture's size and reads that texture; the texture is H. With the route
//           on, the probe runs after the route's resolve for that draw, so H is what the bloom really reads; with the AA off it is the
//           game's own. With Bloom off the probe takes H at the tone instead (bloom=none).
//   bloom   the texture the tone draw adds: the smaller 2D R11G11B10_FLOAT view among the tone's pixel-shader resources. Its mean
//           luma over H's mean luma is the bloom's gain.
//   strip   the exposure the tone is given: the 6x1 R32 texture at the tone's VERTEX-shader slot 0 (texel 0 is the exposure scalar E
//           the tone pass turns into a multiplier, design doc section 107). Texels 1-5 are logged beside it.
//   ldr     the tone's target after the frame, as the player sees it: mean luma, the fraction of pixels at or above 0.95 (washed out),
//           the largest value.
// The tone draw is a full-screen triangle into an R8G8B8A8 target of the render size with a 3D texture (the colour grade) bound in
// its pixel shader. The chain of full-screen draws of the sampled frame is logged once per anti-aliasing mode, with each draw's
// pixel-shader hash, target and the sizes it reads, so a pass that exists in one mode and not the other is plain.
//
// ZERO COST when the key is off or the frame is not a sampled one: one bool load per game draw. It never runs when the game is not
// flat, and it is distinguishable from never-ran: an armed line once, and a window that saw frames but no complete sample prints samples=0.
//
// The pure half (structure tests, the line, the chain entry) is here so tools can pin it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

struct ID3D11DeviceContext;

namespace edvr {

// ---- structure tests (DXGI format numbers: 26 R11G11B10_FLOAT, 27 R8G8B8A8_TYPELESS, 28 R8G8B8A8_UNORM, 39 R32_TYPELESS, 41 R32_FLOAT)
constexpr uint32_t kBloomFmtR11G11B10 = 26, kBloomFmtRgba8Typeless = 27, kBloomFmtRgba8Unorm = 28, kBloomFmtR32Typeless = 39,
                   kBloomFmtR32Float = 41;
constexpr uint32_t kBloomMinSceneWidth = 1280;   // the scene image is at least this wide; a smaller target is no scene

inline bool bloomHalf(uint32_t full, uint32_t half) { return half && full + 1 >= half * 2 && full <= half * 2 + 1; }
// The bloom's first level: a full-screen draw into an R11G11B10_FLOAT target (no depth) that reads a 2D R11G11B10_FLOAT texture of
// twice its size per axis (one pixel of slack for odd sizes), the scene being at least kBloomMinSceneWidth wide.
inline bool bloomIsFirstLevel(uint32_t targetW, uint32_t targetH, uint32_t targetFmt, bool hasDepth,
                              uint32_t srvW, uint32_t srvH, uint32_t srvFmt) {
    return !hasDepth && targetFmt == kBloomFmtR11G11B10 && srvFmt == kBloomFmtR11G11B10 && srvW >= kBloomMinSceneWidth &&
           bloomHalf(srvW, targetW) && bloomHalf(srvH, targetH);
}
// The tone: a full-screen draw into an R8G8B8A8 target at least kBloomMinSceneWidth wide with a 3D texture among its pixel-shader
// resources (the colour grade), no depth.
inline bool bloomIsTone(uint32_t targetW, uint32_t targetFmt, bool hasDepth, bool has3dResource) {
    return !hasDepth && has3dResource && targetW >= kBloomMinSceneWidth &&
           (targetFmt == kBloomFmtRgba8Typeless || targetFmt == kBloomFmtRgba8Unorm);
}
// The exposure strip: a 6x1 R32 texture.
inline bool bloomIsStrip(uint32_t w, uint32_t h, uint32_t fmt) {
    return w == 6 && h == 1 && (fmt == kBloomFmtR32Typeless || fmt == kBloomFmtR32Float);
}

// ---- the window and the lines -------------------------------------------------------------------------------------------
struct FlatBloomProbeWindow {
    char aa[12] = {};
    bool bloomSeen = false;           // some sample in the window had a bloom texture at the tone
    bool hAtBloom = false;            // ... and H was taken at the bloom's first level (else at the tone)
    unsigned frames = 0;              // sampled frames the probe watched
    unsigned samples = 0;             // samples that completed their readback
    unsigned toneSamples = 0, bloomSamples = 0, stripSamples = 0, ldrSamples = 0;
    uint32_t hW = 0, hH = 0, bloomW = 0, bloomH = 0;
    double hMean = 0, hMax = 0, hGm = 0;          // sums over the samples
    double bloomMean = 0, bloomMax = 0;           // sums over the bloom samples
    double strip[6] = {};                         // sums over the strip samples
    double stripMin0 = 0, stripMax0 = 0;          // the exposure scalar's range in the window
    double ldrMean = 0, ldrWhite = 0, ldrMax = 0; // sums over the ldr samples
    uint32_t hazards = 0, dropped = 0, failures = 0;
};
inline int bloomFormatLine(char* out, size_t size, const FlatBloomProbeWindow& w) {
    const double h = w.samples ? static_cast<double>(w.samples) : 1.0;
    const double b = w.bloomSamples ? static_cast<double>(w.bloomSamples) : 1.0;
    const double s = w.stripSamples ? static_cast<double>(w.stripSamples) : 1.0;
    const double l = w.ldrSamples ? static_cast<double>(w.ldrSamples) : 1.0;
    const double hMean = w.samples ? w.hMean / h : 0.0;
    const double bMean = w.bloomSamples ? w.bloomMean / b : 0.0;
    return std::snprintf(out, size,
        "flat bloom probe: aa=%s bloom=%s samples=%u frames=%u tone=%u "
        "H@%s(%ux%u): mean=%.5g max=%.5g gm=%.5g | bloom(%ux%u): mean=%.5g max=%.5g gain=%.5f | "
        "strip: E=%.5g (%.5g..%.5g) t1..t5=%.5g/%.5g/%.5g/%.5g/%.5g (%u) | "
        "ldr: mean=%.5f white=%.3f%% max=%.4f (%u) | hazards=%u dropped=%u failures=%u",
        w.aa, w.bloomSeen ? "yes" : "none", w.samples, w.frames, w.toneSamples,
        w.hAtBloom ? "bloom" : "tone", w.hW, w.hH, hMean, w.samples ? w.hMax / h : 0.0, w.samples ? w.hGm / h : 0.0,
        w.bloomW, w.bloomH, bMean, w.bloomSamples ? w.bloomMax / b : 0.0, hMean > 0 ? bMean / hMean : 0.0,
        w.stripSamples ? w.strip[0] / s : 0.0, w.stripMin0, w.stripMax0,
        w.stripSamples ? w.strip[1] / s : 0.0, w.stripSamples ? w.strip[2] / s : 0.0, w.stripSamples ? w.strip[3] / s : 0.0,
        w.stripSamples ? w.strip[4] / s : 0.0, w.stripSamples ? w.strip[5] / s : 0.0, w.stripSamples,
        w.ldrSamples ? w.ldrMean / l : 0.0, w.ldrSamples ? 100.0 * w.ldrWhite / l : 0.0, w.ldrSamples ? w.ldrMax / l : 0.0, w.ldrSamples,
        w.hazards, w.dropped, w.failures);
}

// One full-screen draw of the sampled frame, for the chain line: the pixel shader, the target and what the pixel shader reads
// (t0..t3: kind '2' 2D, '3' 3D, '.' none; size and format of a 2D one).
struct FlatBloomChainEntry {
    uint64_t ps = 0;
    uint32_t targetW = 0, targetH = 0, targetFmt = 0;
    char kind[4] = {'.', '.', '.', '.'};
    uint32_t w[4] = {}, h[4] = {}, fmt[4] = {};
    bool isH[4] = {};
};
inline int bloomFormatChainEntry(char* out, size_t size, unsigned index, const FlatBloomChainEntry& e) {
    char reads[160] = {};
    size_t at = 0;
    for (int i = 0; i < 4 && at + 40 < sizeof(reads); ++i) {
        if (e.kind[i] == '.') continue;
        at += static_cast<size_t>(std::snprintf(reads + at, sizeof(reads) - at, "%st%d=%s%ux%u/f%u", at ? " " : "", i,
            e.isH[i] ? "H:" : (e.kind[i] == '3' ? "3d:" : ""), e.w[i], e.h[i], e.fmt[i]));
    }
    return std::snprintf(out, size, "#%u PS=%08llX %ux%u/f%u <- %s", index, static_cast<unsigned long long>(e.ps >> 32),
        e.targetW, e.targetH, e.targetFmt, reads[0] ? reads : "-");
}

// ---- the runtime half (flat_bloom_probe.cpp) -----------------------------------------------------------------------------
// Render thread only. `Wants` is one bool load: true on a frame the probe samples (the key is on and 500 ms have passed).
bool flatBloomProbeWants();
// After the flat runtime's scope for a plain Draw of `count` vertices (the route's own work for that draw is done): the structure
// tests, and the reductions on the draws they find.
void flatBloomProbeDraw(ID3D11DeviceContext* ctx, unsigned count);
// Before the frame's Present, flat only: the ldr reduction, the late readback of earlier samples, the 5 s line, the next frame's decision.
void flatBloomProbeFrameEnd();
// Says the window's line now, whatever the clock (a rig's; the runtime lets the 5 s clock say it).
void flatBloomProbeFlush();

}  // namespace edvr
