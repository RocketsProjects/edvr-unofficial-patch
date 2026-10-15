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
enum class Role : uint8_t { None = 0, Args, Out, List, Info, Big, Gbuf8, Gbuf9, Gbuf10, Gbuf11 };
constexpr uint32_t kRoles = 10;
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
