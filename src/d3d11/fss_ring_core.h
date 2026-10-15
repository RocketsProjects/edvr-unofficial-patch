// The ring pass observer's pure half: where the game's ring pass is, how to know it is the one that was read, which fields it reads, and the log lines.
// No game, no D3D, no logging here -- fss_ring_hook.cpp owns the CodeHook and the SEH-guarded reads; tools/fss_tiles_test pins this file against the engine RE.
//
// WHY (docs/fss-scanner.md, "2026-10-15: engine RE of the ring reveal (Opus)"). Flight bca9d6b8: only the LEFT eye shows the FSS ring's black squares; the
// tile pass's schedule is symmetric. The RE found four "main camera only" tests and one stereo-role test on the ring path, and no tile budget. The one
// observation it asked for is what the ring pass entry sees per call in the zoom window: which camera, which stereo role, which pass, which body mode, and
// whether the offscreen target and the scan alpha are set. Read-only: nothing is written, the original runs unchanged.
//
// Offsets are the RE's, build 332841 (PE stamp 1788384820, image size 104894464, the pair every build-keyed hook here checks):
//   FUN_1436558a0 (RVA 0x36558A0)  void(ring, int pass, camera, a4, a5, a6, a7, data)   -- eight parameters, four in registers and four on the stack
//   camera+0x8e8   the camera ID (a bit index from an allocator mask)
//   camera+0x8f8   the owning pipeline's settings block; block+0x10 is the stereo role (1 first eye, 2 second eye, 0 mono; INFERRED)
//   data+0x104     the body mode (2 = the offscreen mono body layer path)
//   ring+0x6b0     the offscreen target pointer (cleared every frame by 0x365b1c0)
//   ring+0x718/0x71c/0x720  the scan alpha, its direction and its active byte
//   ring+0x48      the renderer; renderer->vtbl[0xa8]() -> object; object->vtbl[0x70]() is the main camera's ID
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace ringhook {

constexpr uintptr_t kRingPassRva = 0x36558A0u;
constexpr size_t kPrologueBytes = 16;
// push rbp / push rdi / push r12 / push r13 / push r14 / sub rsp,90h. CodeHook steals the first five bytes (40 55, 57, 41 54): three whole
// instructions, no relative operand.
constexpr uint8_t kPrologue[kPrologueBytes] = {0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};
inline bool prologueMatches(const uint8_t* bytes, size_t n) { return bytes && n >= kPrologueBytes && std::memcmp(bytes, kPrologue, kPrologueBytes) == 0; }

constexpr size_t kCameraId = 0x8e8;
constexpr size_t kCameraSettings = 0x8f8;
constexpr size_t kSettingsRole = 0x10;
constexpr size_t kDataBodyMode = 0x104;
constexpr size_t kRingScratch = 0x6b0;
constexpr size_t kRingAlpha = 0x718;
constexpr size_t kRingDirection = 0x71c;
constexpr size_t kRingActive = 0x720;
constexpr size_t kRingRenderer = 0x48;
constexpr size_t kVtableGetCamera = 0xa8;
constexpr size_t kVtableGetId = 0x70;

constexpr uint32_t kFirstFrames = 12;        // every call is a line for this many frames after a window opens
constexpr uint32_t kEvery = 10;              // then the calls of one frame in this many
constexpr uint32_t kMaxCallLines = 200;      // call lines a window
constexpr uint32_t kMaxFaults = 8;           // reads that fault before the hook stands down

struct Call {
    int      camera = 0;
    unsigned role = 255;      // 255: the settings block could not be followed
    int      pass = 0;
    int      mode = -1;
    bool     scratch = false; // ring+0x6b0 != 0
    float    alpha = 0.0f;
    int      direction = 0;
    unsigned active = 0;
};

// The combinations seen in a window, with counts: the camera, the role, the pass, the mode and whether the offscreen target was set.
struct TupleTable {
    static constexpr uint32_t kCap = 32;
    struct Entry { Call c; uint32_t n = 0; };
    Entry    e[kCap];
    uint32_t used = 0;
    uint32_t overflow = 0;
    static bool same(const Call& a, const Call& b) {
        return a.camera == b.camera && a.role == b.role && a.pass == b.pass && a.mode == b.mode && a.scratch == b.scratch;
    }
    // Returns true when this combination is new in the window.
    bool add(const Call& c) {
        for (uint32_t i = 0; i < used; ++i) {
            if (same(e[i].c, c)) { ++e[i].n; return false; }
        }
        if (used < kCap) { e[used].c = c; e[used].n = 1; ++used; return true; }
        ++overflow;
        return false;
    }
    void clear() { used = 0; overflow = 0; for (auto& x : e) x = Entry(); }
};

inline int formatCall(char* out, size_t cap, uint32_t frame, uint32_t callNo, const Call& c, const char* why) {
    return std::snprintf(out, cap, "fss tiles: ring f=%u call=%u cam=%d role=%u pass=%d mode=%d ring+0x6b0=%s alpha=%.3f dir=%d active=%u (%s)", frame, callNo,
                         c.camera, c.role, c.pass, c.mode, c.scratch ? "set" : "clear", static_cast<double>(c.alpha), c.direction, c.active, why);
}

inline int formatTuples(char* out, size_t cap, const TupleTable& t) {
    size_t n = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < t.used; ++i) {
        const Call& c = t.e[i].c;
        const int w = std::snprintf(out + n, cap > n ? cap - n : 0, "%scam=%d role=%u pass=%d mode=%d 6b0=%s x%u", i ? "; " : "", c.camera, c.role, c.pass, c.mode,
                                    c.scratch ? "set" : "clear", t.e[i].n);
        if (w < 0 || static_cast<size_t>(w) >= (cap > n ? cap - n : 0)) break;
        n += static_cast<size_t>(w);
    }
    return static_cast<int>(n);
}

}  // namespace ringhook
}  // namespace edvr
