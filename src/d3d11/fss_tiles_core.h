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
constexpr uint32_t kCaptureSlots = 12;       // v5: the dispatch's SRV slots read, 0-11 (t8-t11 are the G-buffer the pass shades)
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

// ---- v2: what the lists hold (2026-10-15, after flight f9f62bbc refuted the per-view budget) --------------------------
// Flight f9f62bbc: large + small is 509 in BOTH eyes every frame; only the split between the two shaders differs. So the
// instrument now reads what each eye's two LIST buffers (SRV slot 2, 8-byte entries, 2312 bytes on build 332841) and its
// tile-info buffer (SRV slot 1, 149,248 bytes) hold, and who writes to the window's buffers. Nothing here changes a bind.
constexpr uint32_t kMaxListLines = 120;      // sample lines per window
constexpr uint32_t kMaxWriteLines = 60;      // write lines per window
constexpr uint32_t kMaxDumpFrames = 24;      // frames whose raw buffers are written to disk per window
constexpr uint32_t kEyeDumpFrames = 16;      // consecutive frames written after the eye-dump key (the eye run is sixteen)
constexpr uint32_t kRegions = 6;             // [eye 0/1] x {large list, small list, tile info}
inline uint32_t regionOf(uint32_t eye, uint32_t role) { return eye * 3 + role; }

inline uint64_t fnv64(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

struct ListSummary {
    uint32_t bytes = 0;
    uint32_t entries = 0;                 // bytes / 8
    uint32_t nonzero = 0;                 // entries with any bit set
    int32_t  lastNonzero = -1;
    uint64_t hash = 0;
    uint32_t firstN = 0, lastN = 0;
    uint32_t first[4][2] = {};            // the first four non-zero entries, as two u32 words each
    uint32_t last[4][2] = {};             // the last four non-zero entries, in index order
};

inline ListSummary summarizeList(const void* data, uint32_t bytes) {
    ListSummary s;
    s.bytes = bytes;
    s.entries = bytes / 8;
    s.hash = fnv64(data, bytes);
    const uint32_t* w = static_cast<const uint32_t*>(data);
    uint32_t lastIdx[4] = {};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < s.entries; ++i) {
        const uint32_t a = w[2 * i], b = w[2 * i + 1];
        if (!a && !b) continue;
        ++s.nonzero;
        s.lastNonzero = static_cast<int32_t>(i);
        if (s.firstN < 4) { s.first[s.firstN][0] = a; s.first[s.firstN][1] = b; ++s.firstN; }
        lastIdx[seen % 4] = i;
        ++seen;
    }
    s.lastN = seen < 4 ? seen : 4;
    for (uint32_t k = 0; k < s.lastN; ++k) {
        const uint32_t idx = lastIdx[(seen - s.lastN + k) % 4];
        s.last[k][0] = w[2 * idx];
        s.last[k][1] = w[2 * idx + 1];
    }
    return s;
}

inline int formatList(char* out, size_t cap, const char* tag, const ListSummary& s) {
    char f[120], l[120];
    size_t a = 0, b = 0;
    f[0] = l[0] = 0;
    for (uint32_t i = 0; i < s.firstN; ++i)
        a += static_cast<size_t>(std::snprintf(f + a, sizeof(f) - a, "%s%X:%X", i ? "," : "", s.first[i][0], s.first[i][1]));
    for (uint32_t i = 0; i < s.lastN; ++i)
        b += static_cast<size_t>(std::snprintf(l + b, sizeof(l) - b, "%s%X:%X", i ? "," : "", s.last[i][0], s.last[i][1]));
    return std::snprintf(out, cap, "%s n=%u/%u last@%d hash=%016llX first=[%s] last=[%s]", tag, s.nonzero, s.entries, s.lastNonzero,
                         static_cast<unsigned long long>(s.hash), f, l);
}

// The tile-info buffer is large and its layout unread: a hash and how much of it is non-zero.
inline int formatInfo(char* out, size_t cap, const char* tag, const void* data, uint32_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t nz = 0;
    for (uint32_t i = 0; i < bytes; ++i) nz += p[i] != 0;
    return std::snprintf(out, cap, "%s bytes=%u nonzero=%u hash=%016llX", tag, bytes, nz,
                         static_cast<unsigned long long>(fnv64(data, bytes)));
}

// The split between the shaders: small / (large + small), in percent. 0 when nothing ran.
inline double splitPercent(uint32_t large, uint32_t smallCount) {
    const uint64_t t = static_cast<uint64_t>(large) + smallCount;
    return t ? 100.0 * smallCount / static_cast<double>(t) : 0.0;
}

// The roles a window's buffer can have, for the write watch.
enum class Role : uint8_t { None = 0, Args, Out, List, Info, Big, Gbuf8, Gbuf9, Gbuf10, Gbuf11, Ring0, Ring1, Ring2, Ring3 };
constexpr uint32_t kRoles = 14;
inline const char* roleName(Role r) {
    switch (r) {
        case Role::Args: return "args";
        case Role::Out:  return "output";
        case Role::List: return "list";
        case Role::Info: return "info";
        case Role::Big:  return "t0";
        case Role::Gbuf8: return "t8";
        case Role::Gbuf9: return "t9";
        case Role::Gbuf10: return "t10";
        case Role::Gbuf11: return "t11";
        case Role::Ring0: return "ring_t0";
        case Role::Ring1: return "ring_t1";
        case Role::Ring2: return "ring_t2";
        case Role::Ring3: return "ring_t3";
        default:         return "?";
    }
}

struct WatchTable {
    static constexpr uint32_t kCap = 64;
    const void* ptr[kCap] = {};
    Role        role[kCap] = {};
    uint8_t     eye[kCap] = {};
    uint32_t    n = 0;
    void clear() { n = 0; }
    void add(const void* p, Role r, uint32_t e) {
        if (!p) return;
        for (uint32_t i = 0; i < n; ++i) if (ptr[i] == p) return;
        if (n < kCap) { ptr[n] = p; role[n] = r; eye[n] = static_cast<uint8_t>(e); ++n; }
    }
    int find(const void* p) const {
        for (uint32_t i = 0; i < n; ++i) if (ptr[i] == p) return static_cast<int>(i);
        return -1;
    }
};

// What was written to the watched buffers, by call and role, for the closing summary.
constexpr uint32_t kWriteCalls = 6;
inline const char* writeCallName(uint32_t i) {
    static const char* const n[kWriteCalls] = {"ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat", "ClearRenderTargetView",
                                               "CopyResource", "CopySubresourceRegion", "UpdateSubresource"};
    return i < kWriteCalls ? n[i] : "?";
}

// ---- v3: the band event (2026-10-15, after flight e2e182a6) ------------------------------------------------------------
// Flight e2e182a6: the two lists partition the 17 x 17 grid of 128-pixel cells in BOTH eyes (entries (x, y), the pair
// (17, 17) filling the tail); what differed was the row where cells change from the large shader's list to the small
// one's, by a whole row between the eyes at the zoom (f34170: large 136 against 152 cells), and at f34171 every cell went to
// the small list (large 0). The band watch finds that moment from the lists, without a keypress.
constexpr uint32_t kGrid = 17;
constexpr uint32_t kBandBefore = 10;         // frames logged before the event
constexpr uint32_t kBandAfter = 60;          // frames logged after it
constexpr uint32_t kMaxAutoRuns = 2;         // automatic eye runs a session
constexpr uint32_t kMaxBandLines = 100;      // band lines a window
constexpr uint32_t kBandRaw = 4096;          // raw bytes kept per list (the list is 2312)

struct Band {
    uint32_t valid = 0;     // entries other than the (17, 17) filler
    uint32_t minY = 0;      // the smallest row among them
};

inline Band bandOf(const void* data, uint32_t bytes) {
    Band b;
    const uint32_t* w = static_cast<const uint32_t*>(data);
    const uint32_t n = bytes / 8;
    bool any = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t x = w[2 * i], y = w[2 * i + 1];
        if (x == kGrid && y == kGrid) continue;
        ++b.valid;
        if (!any || y < b.minY) { b.minY = y; any = true; }
    }
    return b;
}

// The row at which the large list starts, from its size: the large list holds the rows from there to the bottom.
inline double rowOfLarge(uint32_t largeCells) { return kGrid - largeCells / static_cast<double>(kGrid); }

struct BandRec {
    uint32_t frame = 0;
    uint32_t large[2] = {};    // valid cells in each eye's large list
    uint32_t smallN[2] = {};
    uint32_t minY[2] = {};     // smallest row in each eye's large list
    bool     ranEye0 = false;  // eye 1 drew eye 0's lists on this frame (the tiles candidate)
};

inline int formatBand(char* out, size_t cap, const BandRec& r) {
    char ran[96] = "";
    if (r.ranEye0) std::snprintf(ran, sizeof(ran), " | ran: eye1 drew eye0's lists (row %.2f)", rowOfLarge(r.large[0]));
    const double r0 = rowOfLarge(r.large[0]), r1 = rowOfLarge(r.large[1]);
    return std::snprintf(out, cap, "fss tiles: band f=%u eye0 L=%u (row %.2f, minY %u) S=%u | eye1 L=%u (row %.2f, minY %u) S=%u | own row gap %.2f%s",
                         r.frame, r.large[0], r0, r.minY[0], r.smallN[0], r.large[1], r1, r.minY[1], r.smallN[1],
                         r1 > r0 ? r1 - r0 : r0 - r1, ran);
}

// The watch: fed each frame's two large-list sizes in frame order.
struct BandWatch {
    enum Reason : uint8_t { None = 0, RowStep, AllSmall };
    bool     havePrev = false;
    uint32_t prev[2] = {};
    uint32_t lo[2] = {}, hi[2] = {};
    uint32_t maxStep[2] = {};
    uint32_t frames = 0;
    Reason feed(uint32_t l0, uint32_t l1) {
        const uint32_t l[2] = {l0, l1};
        Reason r = None;
        for (int e = 0; e < 2; ++e) {
            if (frames == 0 || l[e] < lo[e]) lo[e] = l[e];
            if (frames == 0 || l[e] > hi[e]) hi[e] = l[e];
            if (!havePrev) continue;
            const uint32_t step = l[e] > prev[e] ? l[e] - prev[e] : prev[e] - l[e];
            if (step > maxStep[e]) maxStep[e] = step;
            if (l[e] == 0 && prev[e] > 0) r = AllSmall;
            else if (step >= kGrid && r == None) r = RowStep;
        }
        prev[0] = l0;
        prev[1] = l1;
        havePrev = true;
        ++frames;
        return r;
    }
};

// ---- v4: the reveal window, the inputs and the ring's offscreen draws (2026-10-15, after flight bca9d6b8) --------------------
// Flight bca9d6b8: `tiles` ran and changed nothing; the eyes' lists are identical from the all-small event on, and both eye
// runs caught the black transit void because the zoom window closed as the reveal began. Sean: only the LEFT eye shows the
// black squares, the right eye shows the ring already filled. So the instrument now (1) keeps capturing for 600 frames after
// the all-small event and starts the eye runs inside that stretch, (2) fingerprints each eye's inputs to the tile dispatch
// (slot 0, the buffer of 21,936,256 bytes; slots 3 and 4, the per-eye textures; slot 6, the cube array) by sampling small
// regions, and (3) counts the draws that render into slots 3 and 4, per eye, with their viewports and scissors.
constexpr uint32_t kRevealFrames = 600;      // frames captured after the all-small event
constexpr uint32_t kRunOffsets[2] = {20, 150};   // the two automatic eye runs, in frames after the event
constexpr uint32_t kFingerEvery = 10;        // fingerprint and gbuf lines: one frame in this many
constexpr uint32_t kMaxInputLines = 260;     // input lines a window (four a sampled frame)
constexpr uint32_t kMaxGbufLines = 130;      // gbuf lines a window
constexpr uint32_t kTileGrid = 8;            // sampled boxes per axis in a texture
constexpr uint32_t kTilePx = 16;             // box size in pixels (the squares' own size)
constexpr uint32_t kBufRegions = 16;         // sampled regions of the 21 MB buffer
constexpr uint32_t kBufRegionBytes = 256;
constexpr uint32_t kFpItems = 7;             // per eye: slot 0, slot 3 (depth), slot 4 (stencil), slots 8-11 (the G-buffer)
constexpr uint32_t kFpSlots[kFpItems] = {0, 3, 4, 8, 9, 10, 11};

// Bytes per pixel for the colour formats this can sample; 0 for anything else (depth, block-compressed, unknown).
inline uint32_t bytesPerPixel(uint32_t f) {
    if (f >= 1 && f <= 4) return 16;
    if (f >= 5 && f <= 8) return 12;
    if (f >= 9 && f <= 14) return 8;
    if (f >= 15 && f <= 18) return 8;
    if (f >= 23 && f <= 38) return 4;      // 10:10:10:2, 11:11:10, 8:8:8:8, 16:16
    if (f >= 39 && f <= 43) return 4;      // R32 (not the depth ones' views)
    if (f >= 48 && f <= 60) return 2;      // 8:8, 16
    if (f >= 61 && f <= 65) return 1;
    if (f >= 87 && f <= 93 && f != 89) return 4;   // B8G8R8A8 and its relatives
    return 0;
}

// One character for a sampled box: '.' nothing non-zero, '#' every byte non-zero, 1-8 the share between.
inline char nzChar(uint32_t nonzero, uint32_t n) {
    if (!n || !nonzero) return '.';
    if (nonzero >= n) return '#';
    const uint32_t d = 1 + (nonzero * 8) / n;
    return static_cast<char>('0' + (d > 9 ? 9 : d));
}

inline uint32_t countNonzero(const uint8_t* p, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; ++i) c += p[i] != 0;
    return c;
}

// An 8 x 8 grid as "row/row/.../row" (71 characters), from a function of (x, y).
template <class F>
inline int formatGrid(char* out, size_t cap, F&& at) {
    size_t n = 0;
    for (uint32_t y = 0; y < kTileGrid; ++y) {
        for (uint32_t x = 0; x < kTileGrid; ++x) {
            if (n + 2 < cap) out[n++] = at(x, y);
        }
        if (y + 1 < kTileGrid && n + 2 < cap) out[n++] = '/';
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return static_cast<int>(n);
}

// ---- v6: the eye-diff ledger and the ring quad (2026-10-15, after flight db982c64) ---------------------------------------------------
// Flight db982c64: the ring pass at 0x36558A0 is not on the reveal's path (33,600 calls outside it, none inside), the scene G-buffer t9-t11 is symmetric
// (62 + 1 draws a frame per eye), t8 is one shared texture. The tile pass is symmetric end to end. What is left is a producer after it, or the ring quad /
// mesh renderer with its own t0-t3 ring G-buffer (rounds 16-18). v6 therefore lists every draw into the eye-sized and ring targets for 60 frames from the
// all-small event, per eye, and logs only where the two eyes' lists differ; and it follows the ring family's own inputs.
constexpr unsigned kReduceLevel = 2;           // the automatic eye run's final images: the whole eye at 1/4 size (mip level 2)
constexpr uint32_t kLedgerFrames = 60;        // frames the ledger runs after the all-small event
constexpr uint32_t kLedgerCap = 1024;         // distinct draw entries kept in one frame
constexpr uint32_t kMaxEyediffLines = 80;     // eyediff lines a window
constexpr uint32_t kMaxRingLines = 80;        // ring quad lines a window
constexpr uint32_t kMinTargetWidth = 128;     // smaller render targets are not listed (icons, atlases)
constexpr uint32_t kDiffCap = 64;
constexpr uint32_t kRankCap = 64;

// The ring family, hashes from docs/build-332753.md (verified there: the ring quad and the segment mesh) and the deferred-VS notes: the VS hashes of the
// full-screen deferred quad and the segment mesh, the quad's PS and the ring mesh's index count. A draw matching any is tagged `ring`.
constexpr uint64_t kRingVs[3] = {0x7E38A6AA1269C901ull, 0x0357BBB2DEE43C1Full, 0x53211E8C072CD02Eull};
constexpr uint64_t kRingPs = 0x7CECABDE34FFBE9Eull;
constexpr uint32_t kRingMeshIndices = 13248;
inline bool isRingDraw(char kind, uint32_t count, uint64_t vs, uint64_t ps) {
    for (uint64_t h : kRingVs) if (vs == h) return true;
    return ps == kRingPs || (kind == 'X' && count == kRingMeshIndices);
}

// What names a draw across the two eyes: its kind, per-draw count, the shape of its render target, and its shaders. How many instances it ran, the shapes of
// its pixel-shader inputs t0-t3 and its viewport are reported as data, the way tools/diff_draw_census.py does.
struct DrawKey {
    char     kind = 0;
    uint32_t count = 0;
    uint32_t rtShape = 0;
    uint64_t vs = 0, ps = 0;
    bool operator==(const DrawKey& o) const {
        return kind == o.kind && count == o.count && rtShape == o.rtShape && vs == o.vs && ps == o.ps;
    }
};
struct DrawStat {
    DrawKey  k;
    uint32_t n = 0;          // draws
    uint32_t inst = 0;       // instances over them
    uint32_t srvSig = 0;     // the pixel-shader inputs' shapes, hashed
    uint32_t vpW = 0, vpH = 0;
};
struct DiffItem {
    enum What : uint8_t { Only0 = 0, Only1, Draws, Instances, Inputs, Viewport } what = Only0;
    DrawKey  k;
    uint32_t a = 0, b = 0;   // the two eyes' values where both have the draw
};

// eye 0's entries against eye 1's, by key. Returns how many differences were written (at most cap).
inline uint32_t diffEyes(const DrawStat* e0, uint32_t n0, const DrawStat* e1, uint32_t n1, DiffItem* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < n0 && n < cap; ++i) {
        const DrawStat* m = nullptr;
        for (uint32_t j = 0; j < n1; ++j) if (e1[j].k == e0[i].k) { m = &e1[j]; break; }
        if (!m) { out[n].what = DiffItem::Only0; out[n].k = e0[i].k; out[n].a = e0[i].n; out[n].b = 0; ++n; continue; }
        if (e0[i].n != m->n && n < cap) { out[n].what = DiffItem::Draws; out[n].k = e0[i].k; out[n].a = e0[i].n; out[n].b = m->n; ++n; }
        if (e0[i].inst != m->inst && n < cap) { out[n].what = DiffItem::Instances; out[n].k = e0[i].k; out[n].a = e0[i].inst; out[n].b = m->inst; ++n; }
        if (e0[i].srvSig != m->srvSig && n < cap) { out[n].what = DiffItem::Inputs; out[n].k = e0[i].k; out[n].a = e0[i].srvSig; out[n].b = m->srvSig; ++n; }
        if ((e0[i].vpW != m->vpW || e0[i].vpH != m->vpH) && n < cap) {
            out[n].what = DiffItem::Viewport; out[n].k = e0[i].k; out[n].a = (e0[i].vpW << 16) | (e0[i].vpH & 0xFFFFu); out[n].b = (m->vpW << 16) | (m->vpH & 0xFFFFu); ++n;
        }
    }
    for (uint32_t j = 0; j < n1 && n < cap; ++j) {
        bool found = false;
        for (uint32_t i = 0; i < n0; ++i) if (e0[i].k == e1[j].k) { found = true; break; }
        if (!found) { out[n].what = DiffItem::Only1; out[n].k = e1[j].k; out[n].a = 0; out[n].b = e1[j].n; ++n; }
    }
    return n;
}

inline int formatDiff(char* out, size_t cap, uint32_t frame, const char* pairText, const DiffItem* items, uint32_t n) {
    static const char* const kNames[6] = {"only-eye0", "only-eye1", "draws differ", "instances differ", "inputs differ", "viewport differ"};
    size_t at = static_cast<size_t>(std::snprintf(out, cap, "fss tiles: eyediff f=%u %s", frame, pairText));
    bool firstCategory = true;
    for (uint32_t w = 0; w < 6; ++w) {
        uint32_t shown = 0, total = 0;
        for (uint32_t i = 0; i < n; ++i) if (items[i].what == w) ++total;
        if (!total) continue;
        at += static_cast<size_t>(std::snprintf(out + at, cap > at ? cap - at : 0, "%s %s:", firstCategory ? "" : " |", kNames[w]));
        firstCategory = false;
        for (uint32_t i = 0; i < n && at + 1 < cap; ++i) {
            const DiffItem& d = items[i];
            if (d.what != w) continue;
            if (shown == 5) break;
            ++shown;
            const unsigned long long vs = static_cast<unsigned long long>(d.k.vs >> 32), ps = static_cast<unsigned long long>(d.k.ps >> 32);
            if (w == DiffItem::Only0 || w == DiffItem::Only1) {
                at += static_cast<size_t>(std::snprintf(out + at, cap > at ? cap - at : 0, " %08llX/%08llX %c n=%u x%u (rt shape %u)", vs, ps, d.k.kind, d.k.count,
                                                        w == DiffItem::Only0 ? d.a : d.b, d.k.rtShape));
            } else if (w == DiffItem::Viewport) {
                at += static_cast<size_t>(std::snprintf(out + at, cap > at ? cap - at : 0, " %08llX/%08llX %ux%u vs %ux%u", vs, ps, d.a >> 16, d.a & 0xFFFFu, d.b >> 16,
                                                        d.b & 0xFFFFu));
            } else if (w == DiffItem::Inputs) {
                at += static_cast<size_t>(std::snprintf(out + at, cap > at ? cap - at : 0, " %08llX/%08llX %c n=%u inputs %08X vs %08X", vs, ps, d.k.kind, d.k.count, d.a, d.b));
            } else {
                at += static_cast<size_t>(std::snprintf(out + at, cap > at ? cap - at : 0, " %08llX/%08llX %c n=%u %u vs %u", vs, ps, d.k.kind, d.k.count, d.a, d.b));
            }
        }
        if (total > shown) at += static_cast<size_t>(std::snprintf(out + at, cap > at ? cap - at : 0, " (+%u more)", total - shown));
    }
    return static_cast<int>(at);
}

// The window's ranking of differing draws: by how many frames each differed, then by the size of the difference.
struct RankEntry {
    DiffItem::What what = DiffItem::Only0;
    DrawKey  k;
    uint32_t frames = 0;
    uint64_t total = 0;
};
struct Ranking {
    RankEntry e[kRankCap];
    uint32_t used = 0;
    uint32_t dropped = 0;
    void add(const DiffItem& d) {
        const uint32_t delta = d.what == DiffItem::Only0 ? d.a : d.what == DiffItem::Only1 ? d.b : (d.a > d.b ? d.a - d.b : d.b - d.a);
        for (uint32_t i = 0; i < used; ++i) {
            if (e[i].what == d.what && e[i].k == d.k) { ++e[i].frames; e[i].total += delta; return; }
        }
        if (used < kRankCap) { e[used].what = d.what; e[used].k = d.k; e[used].frames = 1; e[used].total = delta; ++used; }
        else ++dropped;
    }
    // Indices, best first.
    uint32_t order(uint32_t* idx, uint32_t cap) const {
        const uint32_t n = used < cap ? used : cap;
        // a simple selection sort over all entries, keeping the best n
        uint32_t all[kRankCap];
        for (uint32_t i = 0; i < used; ++i) all[i] = i;
        for (uint32_t i = 0; i < used; ++i) {
            uint32_t best = i;
            for (uint32_t j = i + 1; j < used; ++j) {
                const RankEntry& a = e[all[j]];
                const RankEntry& b = e[all[best]];
                if (a.frames > b.frames || (a.frames == b.frames && a.total > b.total)) best = j;
            }
            const uint32_t t = all[i]; all[i] = all[best]; all[best] = t;
        }
        for (uint32_t i = 0; i < n; ++i) idx[i] = all[i];
        return n;
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
