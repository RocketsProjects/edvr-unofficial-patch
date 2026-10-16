// A measurement-only look at cb1 rows 275..279 of the game's 5376-byte scene block, in two short bursts
// (docs/particle-billboards.md, 2026-10-16). Log only: nothing here changes any draw or any write.
//
// What it answers: whether rows 277..279 differ per eye on canted displays, and how many 5376-byte writes the
// game makes a frame. Row 275 is the eye origin; 276 is the next row (the particle fix reads 276..279's
// neighbours); 278 and 279 are the camera's up and forward.
//
// WHEN. Armed once about 20 s after the first eye draw of the session, and once more 60 s after that first
// arming; each burst covers 4 consecutive frames. No hotkey, no ini key.
// WHAT. For every Unmap of the camera block while a burst is open (vscreen.cpp's camera tee, as a fourth
// reader beside glitchFrameObserve, sunglareSceneRows and temporalPassNoteSceneWrite): the frame, the write's
// ordinal within the frame, the resource pointer and the five rows, 80 bytes at byte offset 4400, %.6g.
// At the first eye draw of each burst frame: the resource bound at vertex-shader slot 1, so the eye blocks can
// be told from shadow or cube views. If the camera tee has never seen the block by arming time, one line says so.
//
// The reader only copies its 80 bytes; every line is formatted and logged at the frame boundary, so the Unmap
// path stays as cheap as the other readers. A frame logs at most kMaxPerFrame writes and says how many it saw.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace cb1burst {

constexpr uint32_t kFirstRow = 275;
constexpr uint32_t kRows = 5;                               // 275..279
constexpr uint32_t kBytes = kRows * 16;                     // 80
constexpr uint32_t kOffset = kFirstRow * 16;                // 4400
constexpr uint32_t kNeedBytes = kOffset + kBytes;           // the block must reach row 279's end
constexpr uint32_t kBurstFrames = 4;
constexpr uint32_t kBursts = 2;
constexpr uint32_t kMaxPerFrame = 256;
constexpr uint64_t kFirstDelayMs = 20000;                   // after the first eye draw
constexpr uint64_t kGapMs = 60000;                          // after the first arming

struct Record {
    uint32_t    frame = 0;
    uint32_t    ordinal = 0;
    const void* res = nullptr;
    float       rows[kRows * 4] = {};
};

struct Burst {
    enum Phase { kWaitEye, kWaitFirst, kActive, kWaitNext, kDone };
    Phase    phase = kWaitEye;
    uint32_t armed = 0;                // bursts armed so far
    uint64_t firstEyeMs = 0;
    uint64_t lastArmMs = 0;
    uint32_t framesDone = 0;           // of the open burst
    uint32_t writes = 0;               // this frame's writes seen
    uint32_t kept = 0;                 // ...and recorded
    uint32_t slot1Frame = 0xFFFFFFFFu;
    bool     slot1Have = false;
    const void* slot1Res = nullptr;
    uint64_t blockSeen = 0;            // camera-tee Unmaps of the block, ever
    Record   rec[kMaxPerFrame];

    bool active() const { return phase == kActive; }
    void noteBlockSeen() { ++blockSeen; }

    // The reader. `block` is the mapped block, at least kNeedBytes long (the caller checks). Copies kBytes.
    void noteWrite(uint32_t frame, const void* res, const void* block) {
        if (phase != kActive) return;
        const uint32_t ordinal = ++writes;
        if (kept >= kMaxPerFrame) return;
        Record& r = rec[kept++];
        r.frame = frame;
        r.ordinal = ordinal;
        r.res = res;
        std::memcpy(r.rows, static_cast<const uint8_t*>(block) + kOffset, kBytes);
    }

    // The first eye draw of a frame (vscreen). Starts the 20 s clock once; in a burst frame asks queryFn() once for
    // the resource bound at VS slot 1.
    template <class Query>
    void firstEyeDraw(uint64_t nowMs, uint32_t frame, Query&& queryFn) {
        if (phase == kWaitEye) {
            firstEyeMs = nowMs ? nowMs : 1;
            phase = kWaitFirst;
            return;
        }
        if (phase == kActive && slot1Frame != frame) {
            slot1Frame = frame;
            slot1Res = queryFn();
            slot1Have = true;
        }
    }

    // Once a frame, before the frame counter advances. emit(const char*) takes each finished line.
    template <class Emit>
    void frameBoundary(uint32_t frame, uint64_t nowMs, Emit&& emit) {
        char buf[512];
        if (phase == kActive) {
            if (slot1Have) {
                std::snprintf(buf, sizeof buf, "cb1 rows burst: frame %u first eye draw: VS slot 1 res=%p", frame, slot1Res);
                emit(buf);
            }
            for (uint32_t i = 0; i < kept; ++i) {
                const Record& r = rec[i];
                const float* f = r.rows;
                std::snprintf(buf, sizeof buf,
                              "cb1 rows burst: frame %u write %u res=%p [275]=(%.6g %.6g %.6g %.6g) [276]=(%.6g %.6g %.6g %.6g) "
                              "[277]=(%.6g %.6g %.6g %.6g) [278]=(%.6g %.6g %.6g %.6g) [279]=(%.6g %.6g %.6g %.6g)",
                              r.frame, r.ordinal, r.res, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10],
                              f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19]);
                emit(buf);
            }
            std::snprintf(buf, sizeof buf,
                          "cb1 rows burst: frame %u: %u write(s) of the camera block, %u logged (burst %u of %u, frame %u of %u)",
                          frame, writes, kept, armed, kBursts, framesDone + 1, kBurstFrames);
            emit(buf);
            writes = 0;
            kept = 0;
            slot1Have = false;
            if (++framesDone >= kBurstFrames) phase = armed >= kBursts ? kDone : kWaitNext;
            return;
        }
        const bool due = (phase == kWaitFirst && nowMs - firstEyeMs >= kFirstDelayMs) ||
                         (phase == kWaitNext && nowMs - lastArmMs >= kGapMs);
        if (!due) return;
        ++armed;
        lastArmMs = nowMs ? nowMs : 1;
        framesDone = 0;
        writes = 0;
        kept = 0;
        slot1Have = false;
        slot1Frame = 0xFFFFFFFFu;
        phase = kActive;
        std::snprintf(buf, sizeof buf,
                      "cb1 rows burst: burst %u of %u armed at frame %u; the next %u frames log every Unmap of the camera block "
                      "(rows 275..279) and the VS slot 1 resource at each frame's first eye draw",
                      armed, kBursts, frame, kBurstFrames);
        emit(buf);
        if (blockSeen == 0) {
            std::snprintf(buf, sizeof buf,
                          "cb1 rows burst: the camera tee has not seen the camera block (5376 bytes) in this session, so no write "
                          "lines will follow; the engine-fix tap is off or the block is not mapped through this context");
            emit(buf);
        }
    }
};

inline Burst g_burst;

}  // namespace cb1burst
}  // namespace edvr
