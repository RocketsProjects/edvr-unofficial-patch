// The per-draw census (diag.temporal_draws, 2026-10-11): the record of every recognised eye-target scene draw of the
// newest frames -- the camera rows current at each draw, its vertex shader hash, index count and the projection block's
// floats 784-799 -- and the formatter of the Pause dump's TDRW lines. Pure: no device, no globals, no locks. The draw
// hook, the ring's mutex and the Pause dump live in src/d3d11/temporal_pass.cpp; the rig is tools/temporal_test.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::census {

constexpr int kFrames = 256;   // the ring: frames kept
constexpr int kDraws = 32;     // draws kept per eye per frame; the rest are counted, not kept

struct Draw {
    uint64_t vs = 0;           // the vertex shader hash of the draw
    uint32_t indexCount = 0;   // the draw's index count per instance
    uint32_t seq = 0;          // the scene block's write clock for the latest write before the draw (0 = none this frame)
    bool     hasRows = false;  // that write's rows were a rotation
    bool     hmd = false;      // the rows equal the frame's latest headset-stamped write (hmd_camera_tag.h), made before the draw
    float    T[3] = {};        // the rows' translation: the camera's place in the world
    float    R[4] = {};        // the rows' rotation as a quaternion, w x y z
    float    P[16] = {};       // the projection block's floats 784-799
};

struct Frame {
    uint32_t frame = ~0u;      // ~0 = the slot was never written
    uint32_t drawsSeen[2] = {};
    Draw     draws[2][kDraws];
};

struct Ring {
    Frame frames[kFrames];
};

// The rotation of a row-major 3x4's 3x3 as a quaternion (w, x, y, z).
inline void quat(const float rows[12], float q[4]) {
    const float m00 = rows[0], m01 = rows[1], m02 = rows[2];
    const float m10 = rows[4], m11 = rows[5], m12 = rows[6];
    const float m20 = rows[8], m21 = rows[9], m22 = rows[10];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = 0.5f / std::sqrt(tr + 1.0f);
        q[0] = 0.25f / s; q[1] = (m21 - m12) * s; q[2] = (m02 - m20) * s; q[3] = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        q[0] = (m21 - m12) / s; q[1] = 0.25f * s; q[2] = (m01 + m10) / s; q[3] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        q[0] = (m02 - m20) / s; q[1] = (m01 + m10) / s; q[2] = 0.25f * s; q[3] = (m12 + m21) / s;
    } else {
        const float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        q[0] = (m10 - m01) / s; q[1] = (m02 + m20) / s; q[2] = (m12 + m21) / s; q[3] = 0.25f * s;
    }
}

// Fills one draw. rows is the object's latest write this frame (null when it had none); rowsValid says it was a
// rotation; blk is that block's floats 784-799 (null leaves P zero).
inline void fillDraw(Draw& d, uint64_t vs, uint32_t indexCount, const float* rows, bool rowsValid,
                     uint32_t seq, const float* blk) {
    d = Draw{};
    d.vs = vs;
    d.indexCount = indexCount;
    if (blk) std::memcpy(d.P, blk, sizeof(d.P));
    if (!rows) return;
    d.seq = seq;
    d.hasRows = rowsValid;
    if (rowsValid) {
        d.T[0] = rows[3]; d.T[1] = rows[7]; d.T[2] = rows[11];
        quat(rows, d.R);
    }
}

// Records one recognised draw. Returns false and touches nothing when the census is not wanted: with the key off a draw
// costs one branch here. Counts every draw; keeps the first kDraws of each eye's frame. `calls`, when given, counts the
// draws recorded (the cost line's denominator).
inline bool noteDraw(Ring& ring, bool wanted, uint32_t frame, int eye, const Draw& d, uint64_t* calls = nullptr) {
    if (!wanted || eye < 0 || eye > 1) return false;
    Frame& f = ring.frames[frame % kFrames];
    if (f.frame != frame) {
        f.frame = frame;
        f.drawsSeen[0] = f.drawsSeen[1] = 0;
    }
    const uint32_t k = f.drawsSeen[eye]++;
    if (k < static_cast<uint32_t>(kDraws)) f.draws[eye][k] = d;
    if (calls) ++*calls;
    return true;
}

// One TDRW line, without the logger's prefix. Returns the number of characters written, or 0 when the buffer is too small.
inline int formatDraw(char* buf, size_t n, uint32_t frame, int eye, uint32_t i, const Draw& d) {
    int len = std::snprintf(buf, n,
        "TDRW f%u eye=%d i=%u vs=%016llX n=%u seq=%u rows=%d hmd=%d T=(%+.7g,%+.7g,%+.7g) R=(%+.6g,%+.6g,%+.6g,%+.6g) P=(",
        frame, eye, i, static_cast<unsigned long long>(d.vs), d.indexCount, d.seq, d.hasRows ? 1 : 0, d.hmd ? 1 : 0,
        d.T[0], d.T[1], d.T[2], d.R[0], d.R[1], d.R[2], d.R[3]);
    if (len <= 0 || static_cast<size_t>(len) >= n) return 0;
    for (int j = 0; j < 16; ++j) {
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), "%s%+.7g", j ? "," : "", d.P[j]);
        if (w <= 0 || static_cast<size_t>(len + w) + 2 >= n) return 0;
        len += w;
    }
    len += std::snprintf(buf + len, n - static_cast<size_t>(len), ")");
    return len;
}

// The census cost line's text, without the logger's prefix: the interval's draws, its frames and the census's own QPC ticks (ticksPerSecond from
// QueryPerformanceFrequency). Returns the length written, or a negative value on an encoding error. Zero frames and zero draws print as zeros.
inline int formatCost(char* buf, size_t n, uint64_t calls, uint32_t frames, uint64_t ticks, double ticksPerSecond) {
    const double f = frames ? static_cast<double>(frames) : 1.0;
    const double totalUs = ticksPerSecond > 0.0 ? static_cast<double>(ticks) * 1.0e6 / ticksPerSecond : 0.0;
    return std::snprintf(buf, n,
        "temporal draw census: %llu recognised eye-target draws over %u frames (%.1f a frame); the census "
        "cost %.1f us a frame (%.2f us a draw) on the render thread",
        static_cast<unsigned long long>(calls), frames, static_cast<double>(calls) / f,
        totalUs / f, calls ? totalUs / static_cast<double>(calls) : 0.0);
}

}  // namespace edvr::census
