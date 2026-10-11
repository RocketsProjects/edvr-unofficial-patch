// The camera writes (diag.camera_writes, 2026-10-11): one record per observed camera-block write of the newest frames -- the object,
// the byte range, the thread, the coordinator depth, the rows (translation and quaternion), the projection floats, a return-address
// stack inside EliteDangerous64.exe as RVAs, which of the two pickers chose it this frame, and whether an eye's scene draw observed
// it -- plus the formatter and parser of the Pause dump's TCWR lines and the 20 s cost line. Pure: no device, no Windows, no globals,
// no locks. The write hook, the draw hook, the stack capture and the ring's mutex live in src/d3d11/temporal_pass.cpp; the rig is
// tools/temporal_test. Temporary, default off: the write hook checks the key before doing any work.
//
// The projection floats: block float 784 + k for k in {0, 2, 5, 6, 10, 11, 14}, printed as P0, P2, P5, P6, P10, P11, P14 (the
// 4x4 projection's flat row-major indices). P10 and P11 are the z row's A and B (depth = A + B / z).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "temporal_census.h"

namespace edvr::camwrites {

constexpr int kFrames = 64;          // the ring: frames kept
constexpr int kPerFrame = 256;       // writes kept per frame; the rest are counted as overflow
constexpr int kStack = 6;            // exe return addresses kept per write, innermost first
constexpr size_t kDumpLines = 6000;  // the Pause dump's cap, newest frames first
constexpr uint8_t kPickLegacy = 1u;  // the write today's chooser picked this frame
constexpr uint8_t kPickTag = 2u;     // the write the headset tag names: the latest stamped write this frame
constexpr uint8_t kPickIpd = 4u;     // the write the IPD pair rule chose this frame (camera_ipd_pair.h); printed as "ipd" ahead of the others
constexpr uint8_t kPickNearest = 8u; // the write the nearest-write rule applied in place of the legacy pick (camera_nearest.h, key on); printed as "nearest"
constexpr int kProjIndex[7] = {0, 2, 5, 6, 10, 11, 14};

struct Write {
    uint32_t seq = 0;          // the write's clock: the temporal pass's observed clock, the number a draw carries as its seq
    uintptr_t obj = 0;         // the constant buffer object
    uint32_t off = 0;          // the written byte range
    uint32_t len = 0;
    uint32_t tid = 0;          // the writing thread
    uint32_t co = 0;           // the camera coordinator's depth on that thread (hmd_camera_tag.h)
    bool rot = false;          // the rows were a rotation; T and R are zero otherwise
    uint8_t pick = 0;          // kPickLegacy | kPickTag | kPickIpd
    bool pair = false;         // the write is in an IPD pair this frame: a reference or its partner (camera_ipd_pair.h), whether or not chosen
    bool drawn = false;        // an eye's scene draw this frame observed this write as the object's latest
    uint32_t stkN = 0;
    uint32_t stk[kStack] = {}; // exe RVAs, innermost first
    float T[3] = {};           // the rows' translation (column 3)
    float R[4] = {};           // the rows' rotation quaternion, w x y z
    float P[7] = {};           // the projection floats P0, P2, P5, P6, P10, P11, P14
};

struct Frame {
    uint32_t frame = ~0u;      // ~0 = the slot was never written
    uint32_t n = 0;            // writes kept, in write-clock order
    uint32_t overflow = 0;     // writes past kPerFrame, counted only
    Write w[kPerFrame];
};

struct Ring {
    Frame frames[kFrames];
};

// One observed write, as the hook reads it.
struct In {
    uint32_t seq = 0;
    uintptr_t obj = 0;
    uint32_t off = 0;
    uint32_t len = 0;
    uint32_t tid = 0;
    uint32_t co = 0;
    bool rot = false;
    const float* rows = nullptr;   // the 3x4 rows, 12 floats (block float 932)
    const float* blk = nullptr;    // the block's floats 784..799, 16 floats
    const uint32_t* stk = nullptr; // exe RVAs
    uint32_t stkN = 0;
};

// Records one write. Returns false and touches nothing when the diagnostic is off: with the key off a write costs one branch.
// Counts every write in *calls, kept or overflowed (the cost line's denominator).
inline bool noteWrite(Ring& ring, bool on, uint32_t frame, const In& in, uint64_t* calls = nullptr) {
    if (!on) return false;
    Frame& f = ring.frames[frame % kFrames];
    if (f.frame != frame) {
        f.frame = frame;
        f.n = 0;
        f.overflow = 0;
    }
    if (calls) ++*calls;
    if (f.n >= static_cast<uint32_t>(kPerFrame)) {
        ++f.overflow;
        return true;
    }
    Write& w = f.w[f.n++];
    w = Write{};
    w.seq = in.seq;
    w.obj = in.obj;
    w.off = in.off;
    w.len = in.len;
    w.tid = in.tid;
    w.co = in.co;
    w.rot = in.rot;
    if (in.rot && in.rows) {
        w.T[0] = in.rows[3];
        w.T[1] = in.rows[7];
        w.T[2] = in.rows[11];
        census::quat(in.rows, w.R);
    }
    if (in.blk) {
        for (int k = 0; k < 7; ++k) w.P[k] = in.blk[kProjIndex[k]];
    }
    w.stkN = std::min<uint32_t>(in.stkN, static_cast<uint32_t>(kStack));
    for (uint32_t k = 0; k < w.stkN; ++k) w.stk[k] = in.stk[k];
    return true;
}

// The frame's record with write clock seq, or null. Records are appended in clock order, so the search is a binary search.
inline Write* findWrite(Ring& ring, uint32_t frame, uint32_t seq) {
    Frame& f = ring.frames[frame % kFrames];
    if (f.frame != frame) return nullptr;
    Write* end = f.w + f.n;
    Write* it = std::lower_bound(f.w, end, seq, [](const Write& a, uint32_t s) { return a.seq < s; });
    return (it != end && it->seq == seq) ? it : nullptr;
}

// Marks the write with clock seq as picked by the chooser (kPickLegacy or kPickTag). False when it has no record this frame.
inline bool markPick(Ring& ring, uint32_t frame, uint32_t seq, uint8_t bit) {
    Write* w = findWrite(ring, frame, seq);
    if (!w) return false;
    w->pick = static_cast<uint8_t>(w->pick | bit);
    return true;
}

// Marks the write with clock seq as a member of an IPD pair this frame (camera_ipd_pair.h). False when it has no record this frame.
inline bool markPair(Ring& ring, uint32_t frame, uint32_t seq) {
    Write* w = findWrite(ring, frame, seq);
    if (!w) return false;
    w->pair = true;
    return true;
}

// Marks the write with clock seq as observed by an eye's scene draw (the draw's CB object's latest write before the draw).
inline bool markDrawn(Ring& ring, uint32_t frame, uint32_t seq) {
    Write* w = findWrite(ring, frame, seq);
    if (!w) return false;
    w->drawn = true;
    return true;
}

// The return addresses that fall inside [base, base + size) -- the exe's image -- as RVAs, innermost first, at most cap. Frames outside
// the image (EDVR's own, the driver's, the loader's) are skipped. Returns the count written.
inline int rvasInRange(const void* const* frames, int n, uintptr_t base, size_t size, uint32_t* out, int cap) {
    int k = 0;
    for (int i = 0; i < n && k < cap; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a < base || a - base >= size) continue;
        out[k++] = static_cast<uint32_t>(a - base);
    }
    return k;
}

inline const char* pickName(uint8_t p) {
    if (p & kPickNearest) return "nearest";
    if (p & kPickIpd) return "ipd";
    switch (p & 3u) {
        case 1u: return "legacy";
        case 2u: return "tag";
        case 3u: return "both";
        default: return "-";
    }
}

// One TCWR line, without the logger's prefix. Returns the number of characters written, or 0 when the buffer is too small.
inline int formatWrite(char* buf, size_t n, uint32_t frame, const Write& w) {
    int len = std::snprintf(buf, n,
        "TCWR f%u s%u obj=%llX off=%u len=%u tid=%u co=%u T=(%+.7g,%+.7g,%+.7g) R=(%+.6g,%+.6g,%+.6g,%+.6g) P=(",
        frame, w.seq, static_cast<unsigned long long>(w.obj), w.off, w.len, w.tid, w.co,
        w.T[0], w.T[1], w.T[2], w.R[0], w.R[1], w.R[2], w.R[3]);
    if (len <= 0 || static_cast<size_t>(len) + 16 >= n) return 0;
    for (int k = 0; k < 7; ++k) {
        const int m = std::snprintf(buf + len, n - static_cast<size_t>(len), "%s%+.7g", k ? "," : "", w.P[k]);
        if (m <= 0 || static_cast<size_t>(len + m) + 16 >= n) return 0;
        len += m;
    }
    int m = std::snprintf(buf + len, n - static_cast<size_t>(len), ") stk=");
    if (m <= 0) return 0;
    len += m;
    if (w.stkN == 0) {
        m = std::snprintf(buf + len, n - static_cast<size_t>(len), "-");
        if (m <= 0) return 0;
        len += m;
    } else {
        for (uint32_t k = 0; k < w.stkN; ++k) {
            m = std::snprintf(buf + len, n - static_cast<size_t>(len), "%s%X", k ? "," : "", w.stk[k]);
            if (m <= 0 || static_cast<size_t>(len + m) + 40 >= n) return 0;
            len += m;
        }
    }
    if (m <= 0 || static_cast<size_t>(len) + 40 >= n) return 0;
    m = std::snprintf(buf + len, n - static_cast<size_t>(len), " pick=%s drawn=%d rot=%d pair=%d",
                      pickName(w.pick), w.drawn ? 1 : 0, w.rot ? 1 : 0, w.pair ? 1 : 0);
    if (m <= 0 || static_cast<size_t>(len + m) >= n) return 0;
    return len + m;
}

// One TCWRF line per frame: the frame's kept writes and its overflow.
inline int formatFrame(char* buf, size_t n, uint32_t frame, uint32_t kept, uint32_t overflow) {
    return std::snprintf(buf, n, "TCWRF f%u writes=%u overflow=%u", frame, kept, overflow);
}

// The fields of a TCWR line, as parsed back.
struct Parsed {
    uint32_t frame = 0, seq = 0, off = 0, len = 0, tid = 0, co = 0;
    uintptr_t obj = 0;
    float T[3] = {}, R[4] = {}, P[7] = {};
    uint32_t stk[kStack] = {};
    uint32_t stkN = 0;
    char pick[8] = {};
    bool drawn = false, rot = false, pair = false;
};

// Parses one TCWR line (formatWrite's format). False when a field is missing or malformed.
inline bool parseWrite(const char* line, Parsed& out) {
    unsigned frame = 0, seq = 0, off = 0, len = 0, tid = 0, co = 0;
    unsigned long long obj = 0;
    float T[3] = {}, R[4] = {}, P[7] = {};
    int used = -1;
    const int got = std::sscanf(line,
        "TCWR f%u s%u obj=%llX off=%u len=%u tid=%u co=%u T=(%f,%f,%f) R=(%f,%f,%f,%f) P=(%f,%f,%f,%f,%f,%f,%f) stk=%n",
        &frame, &seq, &obj, &off, &len, &tid, &co, &T[0], &T[1], &T[2], &R[0], &R[1], &R[2], &R[3],
        &P[0], &P[1], &P[2], &P[3], &P[4], &P[5], &P[6], &used);
    if (got != 21 || used <= 0) return false;
    out.frame = frame;
    out.seq = seq;
    out.obj = static_cast<uintptr_t>(obj);
    out.off = off;
    out.len = len;
    out.tid = tid;
    out.co = co;
    std::memcpy(out.T, T, sizeof(T));
    std::memcpy(out.R, R, sizeof(R));
    std::memcpy(out.P, P, sizeof(P));
    // The stack: "-", or hex RVAs separated by commas, ending at the space before " pick=".
    const char* p = line + used;
    out.stkN = 0;
    if (*p == '-') {
        ++p;
    } else {
        for (;;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p) return false;
            if (out.stkN < static_cast<uint32_t>(kStack)) out.stk[out.stkN] = static_cast<uint32_t>(v);
            ++out.stkN;
            p = end;
            if (*p == ',') {
                ++p;
                continue;
            }
            break;
        }
    }
    int drawn = -1, rot = -1, pair = -1;
    char pick[8] = {};
    // A line written with the jump guard's rejected= field still parses: sscanf stops at the fifth field.
    const int tail = std::sscanf(p, " pick=%7s drawn=%d rot=%d pair=%d", pick, &drawn, &rot, &pair);
    if (tail != 4) return false;
    std::memcpy(out.pick, pick, sizeof(out.pick));
    out.drawn = drawn != 0;
    out.rot = rot != 0;
    out.pair = pair != 0;
    if (out.stkN > static_cast<uint32_t>(kStack)) out.stkN = static_cast<uint32_t>(kStack);
    return true;
}

// The projection floats of a frustum, the game's order: P0, P2 (left, right), P5, P6 (top, bottom). Jitter-free: a tangent shift moves
// l and r (and t and b) together, so it adds 2*shift/(r-l) to P2 and 2*shift/(t-b) to P6 (the reader applies it).
inline void expectedProjection(float left, float right, float top, float bottom, float P[4]) {
    P[0] = 2.0f / (right - left);
    P[1] = (right + left) / (right - left);
    P[2] = 2.0f / (top - bottom);
    P[3] = (top + bottom) / (top - bottom);
}

// The once-per-session expected projection line: the four tangents as the temporal pass holds them (tan[0] left, tan[1] right,
// tan[2] bottom, tan[3] top) and the jitter, with the expected P0, P2, P5, P6 for the eye.
inline int formatExpect(char* buf, size_t n, int eye, const float tan[4], float jx, float jy) {
    float P[4] = {};
    expectedProjection(tan[0], tan[1], tan[3], tan[2], P);
    return std::snprintf(buf, n,
        "TCWRE eye=%d tan=(%+.7g,%+.7g,%+.7g,%+.7g) jitter=(%+.6g,%+.6g) expect P0=%+.7g P2=%+.7g P5=%+.7g P6=%+.7g",
        eye, tan[0], tan[1], tan[2], tan[3], jx, jy, P[0], P[1], P[2], P[3]);
}

// The 20 s cost line's text, without the logger's prefix: the interval's writes and frames, and the diagnostic's own QPC ticks (ticksPerSecond
// from QueryPerformanceFrequency). Zero frames and zero writes print as zeros.
inline int formatCost(char* buf, size_t n, uint64_t writes, uint32_t frames, uint64_t ticks, double ticksPerSecond) {
    const double f = frames ? static_cast<double>(frames) : 1.0;
    const double totalUs = ticksPerSecond > 0.0 ? static_cast<double>(ticks) * 1.0e6 / ticksPerSecond : 0.0;
    return std::snprintf(buf, n,
        "temporal camera writes: %llu writes over %u frames (%.1f a frame), cost %.1f us a frame",
        static_cast<unsigned long long>(writes), frames, static_cast<double>(writes) / f, totalUs / f);
}

}  // namespace edvr::camwrites
