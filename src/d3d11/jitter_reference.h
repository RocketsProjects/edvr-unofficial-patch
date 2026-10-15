#pragma once
// What the VR temporal pass last told the game about the eye jitter (native_temporal.cpp begin()), readable from any thread with no lock: the unjittered frusta,
// the render size, the pixel jitter and the tangent shift advertised for the frame. The scene inspector reads it to judge the projection each draw used, and the
// pass's own 30 s census reads it at each scene-block write (docs/dlss-blur-elements-2026-10-14.md, the jitter check). Header-only: a seqlock over plain words, so
// the inspector's rig links without the pass.
#include <atomic>
#include <cstdint>
#include <cstring>

namespace edvr {

struct JitterRef {
    bool known = false;
    uint64_t frame = 0;                  // the pass's own frame counter at the begin() that published it
    float jx = 0.0f, jy = 0.0f;          // the pixel jitter of the frame (the Halton point), content displaced right/down by it
    float shift[2][2] = {};              // the tangent shift advertised per eye: l and r move by [0], t and b by [1]
    float frusta[2][4] = {};             // per eye {left, right, down, up}, unjittered, as advertised
    uint32_t width[2] = {}, height[2] = {};   // the eye's render size the shift was computed for
    uint32_t test = 0;                   // advanced.jitter_test as a number (jitterprobe::JitterTest), so the report can name the mode without reading the config
};

namespace detail {
constexpr unsigned kJitterRefWords = 32;
inline std::atomic<uint32_t> g_jitRefSeq{0};
inline std::atomic<uint32_t> g_jitRefWords[kJitterRefWords];
}  // namespace detail

inline void jitterRefPublish(const JitterRef& r) noexcept {
    uint32_t w[detail::kJitterRefWords] = {};
    unsigned k = 0;
    auto putF = [&](float v) { std::memcpy(&w[k++], &v, 4); };
    w[k++] = 1u;
    w[k++] = static_cast<uint32_t>(r.frame & 0xFFFFFFFFu);
    w[k++] = static_cast<uint32_t>(r.frame >> 32);
    putF(r.jx);
    putF(r.jy);
    for (int e = 0; e < 2; ++e) {
        putF(r.shift[e][0]);
        putF(r.shift[e][1]);
        for (int i = 0; i < 4; ++i) putF(r.frusta[e][i]);
        w[k++] = r.width[e];
        w[k++] = r.height[e];
    }
    w[k++] = r.test;
    const uint32_t s = detail::g_jitRefSeq.load(std::memory_order_relaxed);
    detail::g_jitRefSeq.store(s | 1u, std::memory_order_release);   // odd: being written
    for (unsigned i = 0; i < detail::kJitterRefWords; ++i) detail::g_jitRefWords[i].store(w[i], std::memory_order_relaxed);
    detail::g_jitRefSeq.store((s | 1u) + 1u, std::memory_order_release);   // even again, and never zero
}

// False when nothing was published yet or a publish raced the read twice.
inline bool jitterRefRead(JitterRef* out) noexcept {
    if (!out) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t a = detail::g_jitRefSeq.load(std::memory_order_acquire);
        if (a == 0u || (a & 1u)) continue;
        uint32_t w[detail::kJitterRefWords];
        for (unsigned i = 0; i < detail::kJitterRefWords; ++i) w[i] = detail::g_jitRefWords[i].load(std::memory_order_relaxed);
        if (detail::g_jitRefSeq.load(std::memory_order_acquire) != a) continue;
        unsigned k = 0;
        auto getF = [&]() { float v; std::memcpy(&v, &w[k++], 4); return v; };
        JitterRef r;
        r.known = w[k++] == 1u;
        r.frame = static_cast<uint64_t>(w[k]) | (static_cast<uint64_t>(w[k + 1]) << 32);
        k += 2;
        r.jx = getF();
        r.jy = getF();
        for (int e = 0; e < 2; ++e) {
            r.shift[e][0] = getF();
            r.shift[e][1] = getF();
            for (int i = 0; i < 4; ++i) r.frusta[e][i] = getF();
            r.width[e] = w[k++];
            r.height[e] = w[k++];
        }
        r.test = w[k++];
        *out = r;
        return r.known;
    }
    return false;
}

}  // namespace edvr
