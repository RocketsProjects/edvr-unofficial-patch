// The draw class census: how many of a settlement frame's draws are of which kind, and how many repeat the previous draw's state. The sizing
// instrument for the structural fast path (docs\flat-cz-fps-2026-10-11.md, 2026-10-16 round 3 design): a memoised per-pipeline-state verdict
// that lets a draw skip the constructor can only save what the draws that need nothing cost, so the first thing to know is how many there are
// and what separates them. Counters only: one gate test per draw, and on the census's clocked frames (one in 16) a classification and a handful
// of compares against the previous watched draw. Observes; nothing reads it back.
//
// THE CLASSES, in precedence order (a draw is in the first it fits):
//   post chain        tone passes, the map tonemap, the UI-only source tones, the final copy: the draws the contract is read from
//   pool-family       a keyed pool-family pair: the motion-substituted producers
//   HUD / UI          a pair the UI layer's family rule can ever name
//   offscreen         no colour target, or a target that is not of the scene's extent (shadow maps, depth-only, blur chains)
//   HDR scene         a scene-extent draw into an HDR (format 26) target that is none of the above: lighting, glass, terrain, sky
//   other scene       a scene-extent draw into any other target
// "SAME" is the previous WATCHED draw's state: same target = colour, depth view, depth, format, extent and viewport all equal; same pair =
// vertex and pixel shader equal; same all = both, and the same constant buffer at b1 and the same camera rows hash. A draw that is "same all" as
// the one before it is the kind a run-length fold of the contract could absorb.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

enum class DrawClass : unsigned { Post = 0, Pool, Ui, Offscreen, HdrScene, OtherScene, Count };
inline constexpr unsigned kDrawClasses = static_cast<unsigned>(DrawClass::Count);
inline constexpr const char* kDrawClassNames[kDrawClasses] = {"post chain", "pool-family", "HUD/UI", "offscreen", "HDR scene", "other scene"};

// What classification needs of a draw (the constructor has all of it by the end of its lookup block).
struct DrawClassFacts {
    bool post = false;        // a tone pass, the map tonemap, a source tone or the final copy
    bool pool = false;        // a keyed pool-family pair
    bool uiMayAsk = false;    // the UI layer's family rule can name the pair
    bool hasColor = false;    // a colour target is bound
    bool sceneExtent = false; // the target is of the scene's (or the output's) extent: the contract kind is not None
    bool hdr = false;         // the target is format 26
};
constexpr DrawClass classifyDraw(const DrawClassFacts& f) noexcept {
    if (f.post) return DrawClass::Post;
    if (f.pool) return DrawClass::Pool;
    if (f.uiMayAsk) return DrawClass::Ui;
    if (!f.hasColor || !f.sceneExtent) return DrawClass::Offscreen;
    return f.hdr ? DrawClass::HdrScene : DrawClass::OtherScene;
}

// The state a draw is compared by.
struct DrawClassState {
    const void* color = nullptr;
    const void* depth = nullptr;
    const void* dsv = nullptr;
    const void* b1 = nullptr;
    uint32_t format = 0, width = 0, height = 0, depthWidth = 0, depthHeight = 0, viewportCount = 0;
    float viewport[6] = {};
    uint64_t vs = 0, ps = 0, cameraHash = 0;
    bool sameTarget(const DrawClassState& o) const noexcept {
        return color == o.color && depth == o.depth && dsv == o.dsv && format == o.format && width == o.width && height == o.height &&
               depthWidth == o.depthWidth && depthHeight == o.depthHeight && viewportCount == o.viewportCount &&
               std::memcmp(viewport, o.viewport, sizeof(viewport)) == 0;
    }
    bool samePair(const DrawClassState& o) const noexcept { return vs == o.vs && ps == o.ps; }
    bool sameAll(const DrawClassState& o) const noexcept { return sameTarget(o) && samePair(o) && b1 == o.b1 && cameraHash == o.cameraHash; }
};

struct DrawClassCounts {
    uint64_t draws[kDrawClasses] = {};
    uint64_t sameTarget[kDrawClasses] = {}, samePair[kDrawClasses] = {}, sameAll[kDrawClasses] = {};
    uint64_t frames = 0;   // the clocked frames these counts cover (the line divides by them)
    uint64_t total() const noexcept {
        uint64_t n = 0;
        for (unsigned i = 0; i < kDrawClasses; ++i) n += draws[i];
        return n;
    }
    void add(const DrawClassCounts& o) noexcept {
        for (unsigned i = 0; i < kDrawClasses; ++i) {
            draws[i] += o.draws[i];
            sameTarget[i] += o.sameTarget[i];
            samePair[i] += o.samePair[i];
            sameAll[i] += o.sameAll[i];
        }
        frames += o.frames;
    }
};

// Owner thread. `begin` at the first watched draw of a clocked frame resets the "previous" so the first draw of a frame is not compared with
// the last of the one before.
class DrawClassTracker {
public:
    void beginFrame() noexcept { havePrevious_ = false; ++counts_.frames; }
    void note(DrawClass c, const DrawClassState& now) noexcept {
        const unsigned i = static_cast<unsigned>(c);
        ++counts_.draws[i];
        if (havePrevious_) {
            const bool t = now.sameTarget(previous_), p = now.samePair(previous_);
            counts_.sameTarget[i] += t;
            counts_.samePair[i] += p;
            counts_.sameAll[i] += t && p && now.b1 == previous_.b1 && now.cameraHash == previous_.cameraHash;
        }
        previous_ = now;
        havePrevious_ = true;
    }
    DrawClassCounts take() noexcept {
        const DrawClassCounts out = counts_;
        counts_ = DrawClassCounts{};
        return out;
    }

private:
    DrawClassCounts counts_;
    DrawClassState previous_;
    bool havePrevious_ = false;
};

// One window's text, three classes a token: "post chain 12.0 (target 80% pair 75% all 70%), pool-family ..." per clocked frame. "-" for a class
// with no draw, never a zero percentage.
inline void formatDrawClasses(char* out, size_t cap, const DrawClassCounts& c, unsigned first, unsigned last) noexcept {
    size_t used = 0;
    const double frames = c.frames ? static_cast<double>(c.frames) : 1.0;
    for (unsigned i = first; i < last && i < kDrawClasses && used + 1 < cap; ++i) {
        char share[64] = "-";
        if (c.draws[i])
            std::snprintf(share, sizeof(share), "target %.0f%% pair %.0f%% all %.0f%%", 100.0 * static_cast<double>(c.sameTarget[i]) / static_cast<double>(c.draws[i]),
                          100.0 * static_cast<double>(c.samePair[i]) / static_cast<double>(c.draws[i]),
                          100.0 * static_cast<double>(c.sameAll[i]) / static_cast<double>(c.draws[i]));
        const int n = std::snprintf(out + used, cap - used, "%s%s %.1f a frame (same as the previous draw: %s)", used ? ", " : "", kDrawClassNames[i],
                                    static_cast<double>(c.draws[i]) / frames, share);
        if (n <= 0) break;
        used = (used + static_cast<size_t>(n) < cap) ? used + static_cast<size_t>(n) : cap - 1;
    }
    out[used < cap ? used : cap - 1] = 0;
}

}  // namespace edvr
