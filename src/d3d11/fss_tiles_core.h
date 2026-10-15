// The Full System Scanner's tile renderer, per view: the pure half of the `fss tiles` instrument and of the
// tiles candidate (fix.fss_eye_sync = tiles). No game and no logging here -- fss_tiles.cpp owns the hook glue and
// tools/fss_tiles_test drives this file, including the state restore, on a WARP device.
//
// WHY THIS EXISTS (docs/fss-scanner.md, 2026-10-15). Round 33's measurement: for ~10 frames at each zoom's arrival the
// primary (left) view shows hard-black not-yet-resolved ring tiles that the right view does not. The producer is the
// game's amortized tile renderer, two compute shaders run by DispatchIndirect:
//   5998146D464F5C0E at args+0 and EB0245DE0BB23BB6 at args+12 of the SAME argument buffer, one buffer per view.
// The 2026-10-14 census (edvr_gfx_20261014_144320.log, DCX lines) shows two such pairs a frame, each with its own
// argument buffer, its own UAVs, its own list buffer in SRV slot 2 (and a different one for each of the two shaders).
// Nothing in the arguments was ever read: DispatchIndirect's group counts live on the GPU. The instrument reads them.
//
// THE CANDIDATE. If the second view's tile budget is what the stock look lacks, then drawing the second view's
// dispatches from the FIRST view's argument buffer and buffer bindings makes both views reveal the same tiles in the same
// frame -- the flat screen's look. The substitution is a copy-through at the dispatch: bind the first view's buffers,
// dispatch, bind the second view's back. Textures (the depth and stencil it reads, the targets it writes) and constants
// stay the second view's own.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace fsstiles {

constexpr uint64_t kCsLarge = 0x5998146D464F5C0Eull;   // args+0
constexpr uint64_t kCsSmall = 0xEB0245DE0BB23BB6ull;   // args+12
constexpr uint32_t kSrvSlots = 8;
constexpr uint32_t kMaxViews = 4;            // distinct argument buffers tracked in one frame
constexpr uint32_t kMaxPerFrame = 16;        // dispatches read back per frame
constexpr uint32_t kStagingRing = 4;         // one staging buffer per frame in flight
constexpr uint32_t kReadbackDelay = 2;       // frames between the copy and the Map
constexpr uint32_t kMaxWindows = 6;          // zoom windows instrumented per session
constexpr uint64_t kWindowMs = 10000;        // "about 10 s" from the zoom
constexpr uint32_t kMaxLinesPerWindow = 160;
constexpr uint32_t kDetailFrames = 4;        // every dispatch is a line for this many frames after the window opens
constexpr uint32_t kDetailEvery = 45;        // then one frame in this many

inline bool isTileCs(uint64_t h) { return h == kCsLarge || h == kCsSmall; }
inline int csIndex(uint64_t h) { return h == kCsLarge ? 0 : (h == kCsSmall ? 1 : -1); }

// ---- identities --------------------------------------------------------------------------------------------------
// A pointer is an identity only (never dereferenced); a short id is what a log line carries. Ids are per session.
struct Intern {
    static constexpr uint32_t kCap = 512;
    const void* p[kCap] = {};
    uint32_t n = 0;
    // 0 is "none"; ids start at 1. A full table answers kCap (all later pointers share it, and the log says so by the
    // id being kCap).
    uint32_t id(const void* ptr) {
        if (!ptr) return 0;
        for (uint32_t i = 0; i < n; ++i) if (p[i] == ptr) return i + 1;
        if (n < kCap) { p[n++] = ptr; return n; }
        return kCap;
    }
};

// ---- one view's bindings at one dispatch --------------------------------------------------------------------------
struct Slot {
    const void* view = nullptr;   // the SRV, an identity unless the caller holds a reference
    const void* res = nullptr;    // its resource
    bool        isBuffer = false;
    uint32_t    bytes = 0;        // buffer ByteWidth
};

// Which SRV slots the second view takes from the first: both bound, both buffers, different resources, equal size.
// Equal size is the safety rule -- a list the second view's CS indexes with the first view's counts must be as long.
inline uint32_t substitutionMask(const Slot* e0, const Slot* e1) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < kSrvSlots; ++i) {
        if (e0[i].view && e1[i].view && e0[i].isBuffer && e1[i].isBuffer && e0[i].res != e1[i].res &&
            e0[i].bytes == e1[i].bytes) {
            m |= 1u << i;
        }
    }
    return m;
}

// ---- one dispatch, as recorded --------------------------------------------------------------------------------------
struct Rec {
    uint32_t frame = 0;
    uint32_t eye = 0;             // order of first appearance of the argument buffer in the frame
    uint32_t occ = 0;             // order of this dispatch among the frame's target dispatches
    uint64_t cs = 0;
    uint32_t off = 0;
    uint32_t argsId = 0;
    uint32_t srvId[kSrvSlots] = {};
    uint8_t  srvKind[kSrvSlots] = {};      // 0 unbound, 1 buffer, 2 other (texture)
    uint32_t srvBytes[kSrvSlots] = {};
    uint32_t uavId[2] = {};
    uint32_t subMask = 0;                  // the slots taken from the first view (0 = none)
    bool     substituted = false;
    uint32_t groups[3] = {};
    bool     haveGroups = false;
};

inline int formatRec(char* out, size_t cap, const Rec& r) {
    char srv[8 * 24];
    srv[0] = 0;
    size_t at = 0;
    for (uint32_t i = 0; i < kSrvSlots; ++i) {
        char tok[24];
        if (!r.srvId[i]) std::snprintf(tok, sizeof(tok), "-");
        else if (r.srvKind[i] == 1) std::snprintf(tok, sizeof(tok), "B%u:%u", r.srvId[i], r.srvBytes[i]);
        else std::snprintf(tok, sizeof(tok), "T%u", r.srvId[i]);
        at += static_cast<size_t>(std::snprintf(srv + at, sizeof(srv) - at, "%s%s", i ? "," : "", tok));
    }
    char grp[48];
    if (r.haveGroups) std::snprintf(grp, sizeof(grp), "(%u,%u,%u)", r.groups[0], r.groups[1], r.groups[2]);
    else std::snprintf(grp, sizeof(grp), "(unread)");
    char sub[48] = "";
    if (r.substituted) std::snprintf(sub, sizeof(sub), " lockstep=substituted(slots 0x%02X)", r.subMask);
    return std::snprintf(out, cap, "fss tiles: f=%u eye=%u cs=%016llX groups=%s occ=%u args=A%u+%u srv=[%s] uav=[%u,%u]%s",
                         r.frame, r.eye > 2 ? 2u : r.eye, static_cast<unsigned long long>(r.cs), grp, r.occ, r.argsId,
                         r.off, srv, r.uavId[0], r.uavId[1], sub);
}

// ---- the views of one frame ---------------------------------------------------------------------------------------
// A view is an argument buffer: its first appearance in the frame names it eye 0, the next eye 1.
struct ViewTable {
    const void* args[kMaxViews] = {};
    uint32_t n = 0;
    void reset() { n = 0; for (auto& a : args) a = nullptr; }
    uint32_t eyeOf(const void* a) {
        for (uint32_t i = 0; i < n; ++i) if (args[i] == a) return i;
        if (n < kMaxViews) { args[n] = a; return n++; }
        return kMaxViews - 1;
    }
};

// ---- the zoom window ----------------------------------------------------------------------------------------------
struct WindowMachine {
    enum Event : uint8_t { None = 0, Opened, Closed };
    bool     open = false;          // the zoom window is open (journal-gated, 600 frames, vscreen's arrival window)
    bool     capturing = false;     // the instrument is logging: from the window's opening, for kWindowMs
    uint64_t startMs = 0;
    uint32_t startFrame = 0;
    uint32_t windows = 0;
    // Called once per frame. `zoomOpen` is the arrival window as vscreen computes it.
    Event step(bool zoomOpen, uint32_t frame, uint64_t ms) {
        Event e = None;
        if (zoomOpen && !open) {
            open = true;
            if (windows < kMaxWindows) {
                ++windows;
                capturing = true;
                startMs = ms;
                startFrame = frame;
                e = Opened;
            }
        } else if (!zoomOpen && open) {
            open = false;
            if (capturing) { capturing = false; e = Closed; }
        } else if (open && capturing && ms - startMs > kWindowMs) {
            capturing = false;
            e = Closed;
        }
        return e;
    }
    bool wantsDetail(uint32_t frame) const {
        const uint32_t d = frame - startFrame;
        return d < kDetailFrames || d % kDetailEvery == 0;
    }
};

// ---- do the two views share what they should not ------------------------------------------------------------------
struct Share {
    uint32_t pairs = 0;                      // frames in which both views dispatched the same shader
    uint32_t argsSame = 0;                   // ... with the same argument buffer
    uint32_t slotSame[kSrvSlots] = {};       // per SRV slot: the same resource in both views
    uint32_t slotBoth[kSrvSlots] = {};       // per SRV slot: bound in both views
    uint32_t uavSame = 0;
};
inline void accumulateShare(Share& s, const Rec& a, const Rec& b) {
    ++s.pairs;
    if (a.argsId == b.argsId) ++s.argsSame;
    for (uint32_t i = 0; i < kSrvSlots; ++i) {
        if (a.srvId[i] && b.srvId[i]) {
            ++s.slotBoth[i];
            if (a.srvId[i] == b.srvId[i]) ++s.slotSame[i];
        }
    }
    if (a.uavId[0] && a.uavId[0] == b.uavId[0]) ++s.uavSame;
}

// ---- group-count statistics ----------------------------------------------------------------------------------------
struct Range {
    uint32_t n = 0;
    uint64_t sum = 0;
    uint32_t lo = 0, hi = 0, yzMax = 0;
    void add(uint32_t x, uint32_t y, uint32_t z) {
        if (n == 0 || x < lo) lo = x;
        if (n == 0 || x > hi) hi = x;
        sum += x;
        ++n;
        const uint32_t m = y > z ? y : z;
        if (m > yzMax) yzMax = m;
    }
};

// ---- the state-restoring substitution ------------------------------------------------------------------------------
// Ops supplies: void* getSrv(uint32_t slot) (a reference the caller releases), void setSrv(uint32_t slot, void* view),
// void release(void* view), void dispatchIndirect(void* args, uint32_t off).
struct Swap {
    void*    saved[kSrvSlots] = {};
    uint32_t swapped = 0;
};
template <class Ops>
inline void swapIn(Ops& ops, Swap& sw, void* const* views, uint32_t mask) {
    for (uint32_t i = 0; i < kSrvSlots; ++i) {
        if (!(mask & (1u << i)) || !views[i]) continue;
        sw.saved[i] = ops.getSrv(i);
        ops.setSrv(i, views[i]);
        sw.swapped |= 1u << i;
    }
}
template <class Ops>
inline void swapBack(Ops& ops, Swap& sw) {
    for (uint32_t i = 0; i < kSrvSlots; ++i) {
        if (!(sw.swapped & (1u << i))) continue;
        ops.setSrv(i, sw.saved[i]);   // nullptr is a real answer: the slot was unbound
        ops.release(sw.saved[i]);
        sw.saved[i] = nullptr;
    }
    sw.swapped = 0;
}
// The whole thing for a caller that has no fault to survive (the rig). The hook uses swapIn / swapBack around its own
// guarded dispatch, so a fault inside the driver call still restores the state.
template <class Ops>
inline void lockstepDispatch(Ops& ops, void* const* views, uint32_t mask, void* args0, uint32_t off) {
    Swap sw;
    swapIn(ops, sw, views, mask);
    ops.dispatchIndirect(args0, off);
    swapBack(ops, sw);
}

}  // namespace fsstiles
}  // namespace edvr
