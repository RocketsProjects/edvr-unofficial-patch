#pragma once
// The jitter check's pure half (docs/dlss-blur-elements-2026-10-14.md, "the jitter check"): what a draw's projection carries, judged against what the pass
// published, the census line, and the maths of the temporary key advanced.jitter_test. No Windows, no D3D11.
//
// THE QUESTION. A static, maximum-contrast edge (the cockpit rim against a star) crawls under DLSS and never converges. DLSS removes the jitter it is told about;
// an edge drawn with a different jitter, or none, crawls by the difference. The pass advertises one tangent shift per eye and the game builds its projection from
// it, but a draw can read ANOTHER block (a separate near/far camera for the cockpit) that never got it. So: read the projection rows each draw actually used and
// say what jitter they carry.
//
// THE READ. The scene block's rows 270..273 (cb1, float4 indices) are the eye's view-projection, one row per WORLD axis (flat_mono_shader_source.h cameraBefore):
// row k holds the clip x, y, z, w coefficients of world axis k, and the jitter is added to clip xy as ndc * clip w (unjitterRow: r.xy -= ndc * r.w). With the
// view rotation orthonormal, c = (r0.w, r1.w, r2.w) = -(view z axis) has unit length and the off-centre terms fall out:
//     a = (r0.x, r1.x, r2.x) = P00 Vx - P02 c     =>   P02 = -a.c / |c|^2,   P00 = |a + P02 c|
//     b = (r0.y, r1.y, r2.y) = P11 Vy - P12 c     =>   P12 = -b.c / |c|^2,   P11 = |b + P12 c|
// and an unjittered frustum {l, r, down, up} has P02 = (r + l) / (r - l), P12 = (up + down) / (up - down). The tangent shift (l, r by dx; down, up by dy) moves them by
// 2 dx / (r - l) and 2 dy / (up - down); with dx = -jx (r - l) / w and dy = jy (up - down) / h that is
//     jx = -(P02 - P02_0) w / 2,    jy = +(P12 - P12_0) h / 2      (content displaced right / down by the pixel jitter, the pass's convention).
// A draw whose rows read the published (jx, jy) carries the jitter; (0, 0) none; a sign-flipped match says the game's rows answer a tangent shift the other way.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "jitter_reference.h"

namespace edvr {
namespace jitterprobe {

constexpr uint32_t kVpRowsFirst = 270;                // cb1[270..273]: the eye's view-projection rows
constexpr uint32_t kDepthRow = 198;                   // cb1[198] = (0, 0, A, B), the depth encoding: depth = A + B / z
constexpr uint32_t kSceneBytesMin = 277 * 16;         // the scene block is at least this big (rows to 276)
constexpr uint32_t kSceneBytesMax = 65536;
constexpr float kJitterTolPx = 0.02f;                 // a match within a fiftieth of a pixel

struct Decomposed {
    bool ok = false;
    float p00 = 0, p11 = 0, p02 = 0, p12 = 0;
};

// rows: cb1[270..273] as 16 floats.
inline Decomposed decomposeVp(const float rows[16]) noexcept {
    Decomposed d;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(rows[i])) return d;
    const float a[3] = {rows[0], rows[4], rows[8]};
    const float b[3] = {rows[1], rows[5], rows[9]};
    const float c[3] = {rows[3], rows[7], rows[11]};
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    const float cc = dot(c, c);
    if (!(cc > 0.96f && cc < 1.04f)) return d;   // the view rotation is not orthonormal here: not a camera block
    const float p02 = -dot(a, c) / cc, p12 = -dot(b, c) / cc;
    const float ax[3] = {a[0] + p02 * c[0], a[1] + p02 * c[1], a[2] + p02 * c[2]};
    const float by[3] = {b[0] + p12 * c[0], b[1] + p12 * c[1], b[2] + p12 * c[2]};
    const float p00 = std::sqrt(dot(ax, ax)), p11 = std::sqrt(dot(by, by));
    if (!(p00 > 1e-3f) || !(p11 > 1e-3f)) return d;
    if (std::fabs(dot(ax, by)) > 0.02f * p00 * p11) return d;   // x and y axes not perpendicular
    d.ok = true;
    d.p00 = p00;
    d.p11 = p11;
    d.p02 = p02;
    d.p12 = p12;
    return d;
}

enum class JitterKind : uint8_t {
    NotAvailable,       // no camera block at that slot, or its rows are not the advertised frustum's
    Published,          // the projection carries this frame's published jitter
    PublishedXFlipped,  // ...with x reversed
    PublishedYFlipped,  // ...with y reversed
    PublishedBothFlipped,
    None,               // it carries no jitter
    Other,              // a different shift
};
struct JitterFacts {
    JitterKind kind = JitterKind::NotAvailable;
    float jx = 0, jy = 0;                 // the jitter the rows carry, pixels (valid unless NotAvailable)
    float p00Ratio = 0, p11Ratio = 0;     // measured over the advertised frustum's (the sanity the NotAvailable verdict rests on)
};

inline JitterFacts judgeJitter(const Decomposed& d, const float frustum[4], uint32_t w, uint32_t h, float pubJx, float pubJy, float tolPx = kJitterTolPx) noexcept {
    JitterFacts f;
    if (!d.ok || !w || !h || !frustum) return f;
    const float l = frustum[0], r = frustum[1], dn = frustum[2], up = frustum[3];
    if (!(r > l) || !(up > dn)) return f;
    const float e00 = 2.0f / (r - l), e11 = 2.0f / (up - dn);
    f.p00Ratio = d.p00 / e00;
    f.p11Ratio = d.p11 / e11;
    if (std::fabs(f.p00Ratio - 1.0f) > 0.05f || std::fabs(f.p11Ratio - 1.0f) > 0.05f) return f;
    f.jx = -(d.p02 - (r + l) / (r - l)) * static_cast<float>(w) * 0.5f;
    f.jy = (d.p12 - (up + dn) / (up - dn)) * static_cast<float>(h) * 0.5f;
    auto closeTo = [tolPx](float a, float b) { return std::fabs(a - b) <= tolPx; };
    if (closeTo(f.jx, pubJx) && closeTo(f.jy, pubJy)) f.kind = JitterKind::Published;
    else if (closeTo(f.jx, 0.0f) && closeTo(f.jy, 0.0f)) f.kind = JitterKind::None;
    else if (closeTo(-f.jx, pubJx) && closeTo(f.jy, pubJy)) f.kind = JitterKind::PublishedXFlipped;
    else if (closeTo(f.jx, pubJx) && closeTo(-f.jy, pubJy)) f.kind = JitterKind::PublishedYFlipped;
    else if (closeTo(-f.jx, pubJx) && closeTo(-f.jy, pubJy)) f.kind = JitterKind::PublishedBothFlipped;
    else f.kind = JitterKind::Other;
    return f;
}

// Against either eye of a published reference: eye 0's frustum first, else eye 1's (the eyes' frusta are mirrored and a block belongs to one of them; the pixel jitter
// is the same for both). A verdict that is Published, flipped or None wins; else Other beats NotAvailable; else NotAvailable.
inline JitterFacts judgeEitherEye(const Decomposed& d, const JitterRef& ref) noexcept {
    JitterFacts best;
    for (int e = 0; e < 2; ++e) {
        const JitterFacts f = judgeJitter(d, ref.frusta[e], ref.width[e], ref.height[e], ref.jx, ref.jy);
        if (f.kind != JitterKind::NotAvailable && f.kind != JitterKind::Other) return f;
        if (f.kind == JitterKind::Other && best.kind == JitterKind::NotAvailable) best = f;
        if (e == 0 && best.kind == JitterKind::NotAvailable) best = f;
    }
    return best;
}

// The draw line's field: published | none | n/a | other(+0.31,-0.12 px) | published(x flipped) ...
inline int jitterText(char* out, size_t cap, const JitterFacts& f) noexcept {
    switch (f.kind) {
        case JitterKind::Published: return std::snprintf(out, cap, "published");
        case JitterKind::PublishedXFlipped: return std::snprintf(out, cap, "published(x flipped)");
        case JitterKind::PublishedYFlipped: return std::snprintf(out, cap, "published(y flipped)");
        case JitterKind::PublishedBothFlipped: return std::snprintf(out, cap, "published(both flipped)");
        case JitterKind::None: return std::snprintf(out, cap, "none");
        case JitterKind::Other: return std::snprintf(out, cap, "other(%+.3f,%+.3f px)", static_cast<double>(f.jx), static_cast<double>(f.jy));
        default: return std::snprintf(out, cap, "n/a");
    }
}

// ---- the census ---------------------------------------------------------------------------------------------------------------------------

struct JitterCensus {
    uint32_t frames = 0;
    uint64_t blocks = 0;        // distinct scene buffers written, summed over the frames
    uint64_t published = 0, none = 0, other = 0, flipped = 0, notAvailable = 0;   // their verdicts (the last write of each, in its frame)
    uint64_t writes = 0;        // block writes, summed
};
inline int formatJitterCensus(char* out, size_t cap, const JitterCensus& c, uint32_t seconds) noexcept {
    const double fr = std::max<double>(1.0, c.frames);
    return std::snprintf(out, cap,
        "jitter census: last %u s, %u frames: %.2f scene blocks written per frame (distinct buffers of %u+ bytes), of which %.2f carry the published jitter, %.2f none, "
        "%.2f another shift, %.2f a sign-flipped match, %.2f not judgeable; %.1f block writes per frame. Only the main camera carrying it, with a block that carries "
        "none drawn into the eye, is the unjittered-edge signature.",
        seconds, c.frames, static_cast<double>(c.blocks) / fr, kSceneBytesMin, static_cast<double>(c.published) / fr, static_cast<double>(c.none) / fr,
        static_cast<double>(c.other) / fr, static_cast<double>(c.flipped) / fr, static_cast<double>(c.notAvailable) / fr, static_cast<double>(c.writes) / fr);
}

// ---- the temporary key advanced.jitter_test ------------------------------------------------------------------------------------------------------
// normal: as shipped. off: the pass advertises no tangent shift and tells DLSS zero. flip_x / flip_y / flip_both: the projection is shifted as always, and what DLSS
// is TOLD has that component negated. The flip is applied on the DLSS side only, on purpose: reversing the jitter on both sides is the mirrored Halton sequence
// and changes nothing DLSS can see, so it would test nothing; the one pairing the pass cannot prove offline is the sign DLSS is told against the rows the game
// builds (settled on tools\smoke's rig, never flown).

enum class JitterTest : uint8_t { Normal, Off, FlipX, FlipY, FlipBoth };
struct JitterTestEffect {
    bool jitter = true;     // advertise a tangent shift at all
    int signX = 1, signY = 1;   // multiplied into the jitter DLSS (and the pass's consumers) are told
};
inline JitterTest parseJitterTest(const char* text, bool* known = nullptr) noexcept {
    std::string s = text ? text : "";
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (known) *known = true;
    if (s.empty() || s == "normal") return JitterTest::Normal;
    if (s == "off") return JitterTest::Off;
    if (s == "flip_x") return JitterTest::FlipX;
    if (s == "flip_y") return JitterTest::FlipY;
    if (s == "flip_both") return JitterTest::FlipBoth;
    if (known) *known = false;
    return JitterTest::Normal;
}
inline JitterTestEffect jitterTestEffect(JitterTest t) noexcept {
    JitterTestEffect e;
    switch (t) {
        case JitterTest::Off: e.jitter = false; break;
        case JitterTest::FlipX: e.signX = -1; break;
        case JitterTest::FlipY: e.signY = -1; break;
        case JitterTest::FlipBoth: e.signX = e.signY = -1; break;
        default: break;
    }
    return e;
}
inline const char* jitterTestName(JitterTest t) noexcept {
    switch (t) {
        case JitterTest::Off: return "off";
        case JitterTest::FlipX: return "flip_x";
        case JitterTest::FlipY: return "flip_y";
        case JitterTest::FlipBoth: return "flip_both";
        default: return "normal";
    }
}

}  // namespace jitterprobe
}  // namespace edvr
