#pragma once
// The planet LOD trace (TEMPORARY, docs\planet-lod-streaming-2026-10-14.md): a read-only instrument on Frontier's terrain generation scheduler and its tile tree. It exists to
// say, in ONE flight, where the seconds go between a tile being wanted and a tile being clean: a queue that never drains (throughput: more budget or more threads help), or a queue
// that is empty while tiles are still coarse (the requests come late: the LOD distance thresholds or prediction are the fix). Pure: pins, ring reading, counters and formats, no
// Windows, so tools\planet_lod_trace_test drives every case. planet_lod_trace_hook.cpp is the glue that patches the five sites.
//
// WHAT IS READ (build 332841, EliteDangerous64.exe; docs\planet-lod-streaming-2026-10-14.md has the evidence; READ = a disassembled instruction):
//   0x4C97460  budget.   (rcx front, rdx ctx, r8b flag, r9 &stamp, [rsp+28h] double T, [rsp+30h] float multiplier). T = 1000/HMD Hz; budget ms = T*(0.24+0.12*multiplier); [front+20h] is a
//              boost byte that, when set, makes the budget a flat 30 ms. Called once per frame from 0x12904B0 (call at 0x1290591) and calls the pump below (call at 0x4C97622).
//   0x4C97730  the pump. (rcx state, rdx ctx, xmm2 double budget ms, r9 0). Returns the sum of the work table's ESTIMATED ms it granted. Two rings of tasks:
//                pending: base state+20h;  active: base [state+48h + uint[state+18h]*8].  A ring at base B: data [B+8], capacity [B+10h], start [B+18h], count [B+20h]; the element at
//                logical index i is data[(start+i) mod capacity] (a task pointer; its vtable +48h is its priority). It returns 0.0 at once when both counts are zero.
//   0x4C95BF0  the grant. Called once per task the pump considers: (rcx table, rdx out{u32 work, float ms}, r8 &u16 type id, r9d granularity, [rsp+28h] u32 available, [rsp+30h] float
//              remaining ms). out.work == 0 means the task got nothing.
//   0x444D6C0  the quadtree node update (recursive). (rcx node, rdx out byte[2], r8 ctx, r9d, [rsp+28h] ptr, [rsp+30h] byte, [rsp+38h] ptr, [rsp+40h] double).
//              node: +2B4h u16 flags (0x400 wants children, 0x200 keep), +2B7h u8 level, +158h u32 state (0 none, 1 pending, 2 ready; the names are INFERRED), +8 children pointer.
//   0x4467100  the tile request submit. (rcx node, rdx ctx, r8d, xmm3 double, [rsp+28h] int). node+30h is the request handle (zero until a request exists).
//
// A site is hooked only when the PE stamp and image size are build 332841's, the first 24 bytes of the function are what was read, and the one E8 that calls it is where it was read.
// Anything else is one line (`planet lod trace: inert: <why>`) and nothing is patched. The hooks never change an argument, a result or a byte of the game's memory.
//
// Cost: the pump and grant detours do a counter and a forward; when both of the pump's rings are empty the pump detour is two loads and a forward (the pump itself returns at once).
// The node and request detours run per tile node per frame whenever a planet is near, a handful of relaxed adds each.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace plt {

constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;

constexpr size_t kHeadBytes = 24;
enum Site : int { kBudget = 0, kPump, kGrant, kNode, kRequest, kSiteCount };
struct SitePin {
    const char* name;
    uint32_t rva;
    uint32_t callRva;   // the E8 that calls it, and where the pin says it must land
    uint8_t head[kHeadBytes];
};
constexpr SitePin kPins[kSiteCount] = {
    {"budget", 0x4C97460u, 0x1290591u, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"pump", 0x4C97730u, 0x4C97622u, {0x40, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90, 0x02, 0x00, 0x00, 0x48, 0x83, 0x79}},
    {"grant", 0x4C95BF0u, 0x4C97B66u, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"node", 0x444D6C0u, 0x444D5DCu, {0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x49, 0x8D, 0xAB, 0xD8, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF8, 0x01, 0x00}},
    {"request", 0x4467100u, 0x444E1CBu, {0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x82, 0xE0, 0x00, 0x00, 0x00, 0x48}},
};

// ---- the pins ---------------------------------------------------------------------------------------------------------------------------------------------------
inline uint32_t load32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline int32_t load32s(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t load16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint64_t load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

struct Verdict {
    bool ok = false;
    char why[300] = {};
};

// `base` is a mapped PE image of `size` bytes (SizeOfImage). Reads only inside it.
inline Verdict checkImage(const uint8_t* base, size_t size) {
    Verdict v;
    if (!base || size < 0x1000) { std::snprintf(v.why, sizeof(v.why), "the game's module could not be resolved"); return v; }
    const uint32_t peOff = load32(base + 0x3C);
    if (peOff < 0x40 || peOff > 0x300 || std::memcmp(base + peOff, "PE\0\0", 4) != 0 || load16(base + peOff + 24) != 0x20B) {
        std::snprintf(v.why, sizeof(v.why), "the module is not a PE32+ image");
        return v;
    }
    const uint32_t stamp = load32(base + peOff + 8);
    const uint32_t sizeOfImage = load32(base + peOff + 0x50);
    if (stamp != kExpectedTimestamp || sizeOfImage != kExpectedImageSize) {
        std::snprintf(v.why, sizeof(v.why), "PE stamp %u and image size %u are not build 332841's (%u, %u)", stamp, sizeOfImage, kExpectedTimestamp, kExpectedImageSize);
        return v;
    }
    for (int i = 0; i < kSiteCount; ++i) {
        const SitePin& p = kPins[i];
        if (uint64_t(p.rva) + kHeadBytes > size || std::memcmp(base + p.rva, p.head, kHeadBytes) != 0) {
            std::snprintf(v.why, sizeof(v.why), "the head of the %s function at exe+0x%X is not the one read", p.name, p.rva);
            return v;
        }
        if (uint64_t(p.callRva) + 5 > size || base[p.callRva] != 0xE8 || int64_t(p.callRva) + 5 + int64_t(load32s(base + p.callRva + 1)) != int64_t(p.rva)) {
            std::snprintf(v.why, sizeof(v.why), "the call at exe+0x%X does not land on the %s function (exe+0x%X)", p.callRva, p.name, p.rva);
            return v;
        }
    }
    v.ok = true;
    return v;
}

// ---- reading the pump's rings -----------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxSample = 64;       // per ring
constexpr uint64_t kMaxCount = 1u << 20;  // a count above this is not a ring
struct RingCounts {
    bool ok = false;
    uint64_t pending = 0, active = 0;
    const uint8_t* pendingBase = nullptr;
    const uint8_t* activeBase = nullptr;
};
// The caller guards the reads (a game pointer); this only does the pointer arithmetic. `state` is the pump's rcx.
inline RingCounts readCounts(const uint8_t* state) {
    RingCounts r;
    if (!state) return r;
    const uint32_t set = load32(state + 0x18);
    if (set >= 8) return r;
    const uint64_t act = load64(state + 0x48 + size_t(set) * 8);
    if (act < 0x10000) return r;
    r.pendingBase = state + 0x20;
    r.activeBase = reinterpret_cast<const uint8_t*>(act);
    r.pending = load64(state + 0x40);
    r.active = load64(r.activeBase + 0x20);
    r.ok = r.pending <= kMaxCount && r.active <= kMaxCount;
    return r;
}

using PriorityFn = int (*)(uintptr_t task, uintptr_t vtable);
struct QueueSample {
    uint32_t n = 0;
    uintptr_t task[2 * kMaxSample] = {};
    uintptr_t vt[2 * kMaxSample] = {};
    int prio[2 * kMaxSample] = {};
};
// Add up to kMaxSample tasks of the ring at `B` to `out`. A task whose vtable is not inside [lo, hi) (the game's image) is skipped, never called.
inline void sampleRing(const uint8_t* B, uintptr_t lo, uintptr_t hi, PriorityFn pf, QueueSample* out) {
    const uint64_t count = load64(B + 0x20), cap = load64(B + 0x10), start = load64(B + 0x18);
    const uint64_t data = load64(B + 8);
    if (count == 0 || count > kMaxCount || cap == 0 || cap > kMaxCount * 16 || data < 0x10000) return;
    const uint64_t take = count < kMaxSample ? count : kMaxSample;
    for (uint64_t i = 0; i < take; ++i) {
        uint64_t pos = start + i;
        pos %= cap;
        const uint64_t task = load64(reinterpret_cast<const uint8_t*>(data) + pos * 8);
        if (task < 0x10000) continue;
        const uint64_t vt = load64(reinterpret_cast<const uint8_t*>(task));
        if (vt < lo || vt + 0x50 > hi) continue;
        if (out->n >= 2 * kMaxSample) return;
        out->task[out->n] = uintptr_t(task);
        out->vt[out->n] = uintptr_t(vt);
        out->prio[out->n] = pf ? pf(uintptr_t(task), uintptr_t(vt)) : 0;
        ++out->n;
    }
}

// ---- the counters ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr int kLevels = 24;
constexpr int kTypeSlots = 32;
constexpr int kSeen = 160;

inline void bumpMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
inline void bumpMin(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
inline uint64_t usOf(double ms) { return ms > 0.0 && ms < 1.0e9 ? uint64_t(ms * 1000.0 + 0.5) : 0u; }
inline int levelOf(unsigned v) { return v < unsigned(kLevels) ? int(v) : kLevels - 1; }

struct Trace {
    // lifetime, never reset: which hooks have EVER run (the "never ran" test)
    std::atomic<uint64_t> life[kSiteCount]{};

    // window: budget call
    std::atomic<uint64_t> bCalls{0}, bBoost{0}, bTminUs{~0ull}, bTmaxUs{0}, bMultMinMicro{~0ull}, bMultMaxMicro{0};
    // window: pump
    std::atomic<uint64_t> pumps{0}, idlePumps{0}, budgetSumUs{0}, budgetMaxUs{0}, usedSumUs{0}, usedMaxUs{0}, boundFrames{0}, zeroBudgetFrames{0};
    std::atomic<uint64_t> pendAfterSum{0}, pendAfterMax{0}, actAfterSum{0}, actAfterMax{0}, backlogFrames{0}, streakMaxMs{0}, ageMaxMs{0};
    // window: grant
    std::atomic<uint64_t> considered{0}, runs{0}, workSum{0}, estUs{0};
    struct TypeSlot { std::atomic<uint32_t> key{0}; std::atomic<uint32_t> gran{0}; std::atomic<uint64_t> runs{0}, work{0}, us{0}; };
    TypeSlot types[kTypeSlots];
    std::atomic<uint64_t> typeOverflow{0};
    // window: tree
    std::atomic<uint64_t> nodes{0}, wantFiner{0}, withChildren{0};
    std::atomic<uint64_t> stateCount[kLevels][3]{};
    std::atomic<uint64_t> reqCalls{0}, reqNew{0};
    std::atomic<uint64_t> reqByLevel[kLevels]{};
    // latest queue sample (written by the pump thread, read by the window)
    std::atomic<uint32_t> prioN{0}, prioSampled{0};
    std::atomic<int32_t> prioVal[8]{};
    std::atomic<uint32_t> prioCnt[8]{};
    // pump-thread only
    uint64_t streakStartMs = 0;
    uint64_t lastSampleMs = 0;
    struct Seen { uintptr_t task = 0, vt = 0; uint64_t firstMs = 0; };
    Seen seen[kSeen];
    uint32_t seenN = 0;

    void noteBudgetCall(double T, float mult, bool boost) {
        life[kBudget].fetch_add(1, std::memory_order_relaxed);
        bCalls.fetch_add(1, std::memory_order_relaxed);
        if (boost) bBoost.fetch_add(1, std::memory_order_relaxed);
        const uint64_t t = usOf(T);
        bumpMin(bTminUs, t); bumpMax(bTmaxUs, t);
        const uint64_t m = mult > 0.0f && mult < 1.0e6f ? uint64_t(double(mult) * 1.0e6 + 0.5) : 0u;
        bumpMin(bMultMinMicro, m); bumpMax(bMultMaxMicro, m);
    }

    // Pump finished. `idle`: both rings were empty at entry (nothing else is meaningful then).
    void notePump(bool idle, double budgetMs, double usedMs, uint64_t pendAfter, uint64_t actAfter, uint64_t nowMs) {
        life[kPump].fetch_add(1, std::memory_order_relaxed);
        if (idle) { idlePumps.fetch_add(1, std::memory_order_relaxed); streakStartMs = 0; return; }
        pumps.fetch_add(1, std::memory_order_relaxed);
        const uint64_t b = usOf(budgetMs), u = usOf(usedMs);
        budgetSumUs.fetch_add(b, std::memory_order_relaxed); bumpMax(budgetMaxUs, b);
        usedSumUs.fetch_add(u, std::memory_order_relaxed); bumpMax(usedMaxUs, u);
        if (b == 0) zeroBudgetFrames.fetch_add(1, std::memory_order_relaxed);
        else if (u * 10 >= b * 9) boundFrames.fetch_add(1, std::memory_order_relaxed);
        pendAfterSum.fetch_add(pendAfter, std::memory_order_relaxed); bumpMax(pendAfterMax, pendAfter);
        actAfterSum.fetch_add(actAfter, std::memory_order_relaxed); bumpMax(actAfterMax, actAfter);
        if (pendAfter + actAfter > 0) {
            backlogFrames.fetch_add(1, std::memory_order_relaxed);
            if (streakStartMs == 0) streakStartMs = nowMs ? nowMs : 1;
            bumpMax(streakMaxMs, nowMs - streakStartMs);
        } else {
            streakStartMs = 0;
        }
    }

    void noteGrant(uint16_t typeId, uint32_t granularity, uint32_t work, float ms) {
        life[kGrant].fetch_add(1, std::memory_order_relaxed);
        considered.fetch_add(1, std::memory_order_relaxed);
        if (work == 0) return;
        runs.fetch_add(1, std::memory_order_relaxed);
        workSum.fetch_add(work, std::memory_order_relaxed);
        const uint64_t us = ms > 0.0f && ms < 1.0e6f ? uint64_t(double(ms) * 1000.0 + 0.5) : 0u;
        estUs.fetch_add(us, std::memory_order_relaxed);
        const uint32_t key = uint32_t(typeId) | 0x10000u;
        for (int i = 0; i < kTypeSlots; ++i) {
            uint32_t cur = types[i].key.load(std::memory_order_relaxed);
            if (cur == 0 && types[i].key.compare_exchange_strong(cur, key, std::memory_order_relaxed)) { cur = key; types[i].gran.store(granularity, std::memory_order_relaxed); }
            if (cur == key) {
                types[i].runs.fetch_add(1, std::memory_order_relaxed);
                types[i].work.fetch_add(work, std::memory_order_relaxed);
                types[i].us.fetch_add(us, std::memory_order_relaxed);
                return;
            }
        }
        typeOverflow.fetch_add(1, std::memory_order_relaxed);
    }

    void noteNode(unsigned level, unsigned state, unsigned flags, bool hasChildren) {
        life[kNode].fetch_add(1, std::memory_order_relaxed);
        nodes.fetch_add(1, std::memory_order_relaxed);
        if (flags & 0x400u) wantFiner.fetch_add(1, std::memory_order_relaxed);
        if (hasChildren) withChildren.fetch_add(1, std::memory_order_relaxed);
        stateCount[levelOf(level)][state < 3 ? state : 2].fetch_add(1, std::memory_order_relaxed);
    }

    void noteRequest(unsigned level, bool newHandle) {
        life[kRequest].fetch_add(1, std::memory_order_relaxed);
        reqCalls.fetch_add(1, std::memory_order_relaxed);
        if (newHandle) { reqNew.fetch_add(1, std::memory_order_relaxed); reqByLevel[levelOf(level)].fetch_add(1, std::memory_order_relaxed); }
    }

    // A fresh queue sample: the priority histogram and the oldest task's age (a task pointer first seen this long ago and still queued; pointers can be reused, so an age is a floor
    // of how long something has waited and is not a proof).
    void noteSample(const QueueSample& s, uint64_t nowMs) {
        int32_t vals[8] = {};
        uint32_t cnts[8] = {};
        uint32_t nv = 0;
        for (uint32_t i = 0; i < s.n; ++i) {
            uint32_t k = 0;
            while (k < nv && vals[k] != s.prio[i]) ++k;
            if (k == nv) { if (nv >= 8) continue; vals[nv++] = s.prio[i]; }
            ++cnts[k];
        }
        for (uint32_t k = 0; k < 8; ++k) { prioVal[k].store(vals[k], std::memory_order_relaxed); prioCnt[k].store(cnts[k], std::memory_order_relaxed); }
        prioN.store(nv, std::memory_order_relaxed);
        prioSampled.store(s.n, std::memory_order_relaxed);
        Seen next[kSeen];
        uint32_t nn = 0;
        uint64_t oldest = 0;
        for (uint32_t i = 0; i < s.n && nn < uint32_t(kSeen); ++i) {
            uint64_t first = nowMs;
            for (uint32_t k = 0; k < seenN; ++k) {
                if (seen[k].task == s.task[i] && seen[k].vt == s.vt[i]) { first = seen[k].firstMs; break; }
            }
            next[nn++] = Seen{s.task[i], s.vt[i], first};
            if (nowMs - first > oldest) oldest = nowMs - first;
        }
        for (uint32_t k = 0; k < nn; ++k) seen[k] = next[k];
        seenN = nn;
        bumpMax(ageMaxMs, oldest);
        lastSampleMs = nowMs;
    }
};

// ---- the window's lines ----------------------------------------------------------------------------------------------------------------------------------------
// One closed window, copied out and cleared. A line is `planet lod trace: ...`; the formats are the contract tools\edvr_log.py will tally.
struct Window {
    uint64_t ms = 0;
    uint64_t pumps = 0, idle = 0, budgetSumUs = 0, budgetMaxUs = 0, usedSumUs = 0, usedMaxUs = 0, bound = 0, zeroBudget = 0;
    uint64_t pendSum = 0, pendMax = 0, actSum = 0, actMax = 0, backlog = 0, streakMax = 0, ageMax = 0;
    uint64_t considered = 0, runs = 0, work = 0, estUs = 0, typeOverflow = 0;
    uint64_t bCalls = 0, bBoost = 0, bTminUs = 0, bTmaxUs = 0, bMultMin = 0, bMultMax = 0;
    uint64_t nodes = 0, wantFiner = 0, withChildren = 0, reqCalls = 0, reqNew = 0;
    uint64_t state[kLevels][3] = {};
    uint64_t reqLevel[kLevels] = {};
    struct Type { uint32_t id = 0, gran = 0; uint64_t runs = 0, work = 0, us = 0; };
    Type types[kTypeSlots];
    uint32_t typeN = 0;
    uint32_t prioN = 0, prioSampled = 0;
    int32_t prioVal[8] = {};
    uint32_t prioCnt[8] = {};
    uint64_t life[kSiteCount] = {};
    bool busy() const { return pumps > 0 || runs > 0 || reqNew > 0; }
};

inline uint64_t take(std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); }

inline Window closeWindow(Trace& t, uint64_t windowMs) {
    Window w;
    w.ms = windowMs;
    w.pumps = take(t.pumps); w.idle = take(t.idlePumps);
    w.budgetSumUs = take(t.budgetSumUs); w.budgetMaxUs = take(t.budgetMaxUs);
    w.usedSumUs = take(t.usedSumUs); w.usedMaxUs = take(t.usedMaxUs);
    w.bound = take(t.boundFrames); w.zeroBudget = take(t.zeroBudgetFrames);
    w.pendSum = take(t.pendAfterSum); w.pendMax = take(t.pendAfterMax);
    w.actSum = take(t.actAfterSum); w.actMax = take(t.actAfterMax);
    w.backlog = take(t.backlogFrames); w.streakMax = take(t.streakMaxMs); w.ageMax = take(t.ageMaxMs);
    w.considered = take(t.considered); w.runs = take(t.runs); w.work = take(t.workSum); w.estUs = take(t.estUs);
    w.typeOverflow = take(t.typeOverflow);
    w.bCalls = take(t.bCalls); w.bBoost = take(t.bBoost);
    w.bTminUs = t.bTminUs.exchange(~0ull, std::memory_order_relaxed); w.bTmaxUs = take(t.bTmaxUs);
    w.bMultMin = t.bMultMinMicro.exchange(~0ull, std::memory_order_relaxed); w.bMultMax = take(t.bMultMaxMicro);
    w.nodes = take(t.nodes); w.wantFiner = take(t.wantFiner); w.withChildren = take(t.withChildren);
    w.reqCalls = take(t.reqCalls); w.reqNew = take(t.reqNew);
    for (int l = 0; l < kLevels; ++l) {
        for (int s = 0; s < 3; ++s) w.state[l][s] = take(t.stateCount[l][s]);
        w.reqLevel[l] = take(t.reqByLevel[l]);
    }
    for (int i = 0; i < kTypeSlots; ++i) {
        const uint32_t key = t.types[i].key.load(std::memory_order_relaxed);
        if (key == 0) continue;
        Window::Type& ty = w.types[w.typeN];
        ty.id = key & 0xFFFFu; ty.gran = t.types[i].gran.load(std::memory_order_relaxed);
        ty.runs = take(t.types[i].runs); ty.work = take(t.types[i].work); ty.us = take(t.types[i].us);
        if (ty.runs) ++w.typeN;
    }
    w.prioN = t.prioN.load(std::memory_order_relaxed); w.prioSampled = t.prioSampled.load(std::memory_order_relaxed);
    for (int k = 0; k < 8; ++k) { w.prioVal[k] = t.prioVal[k].load(std::memory_order_relaxed); w.prioCnt[k] = t.prioCnt[k].load(std::memory_order_relaxed); }
    for (int i = 0; i < kSiteCount; ++i) w.life[i] = t.life[i].load(std::memory_order_relaxed);
    return w;
}

inline double ms(uint64_t us) { return double(us) / 1000.0; }
inline double per(uint64_t sum, uint64_t n) { return n ? double(sum) / double(n) : 0.0; }

// LINE 1. `planet lod trace: 5.0 s: pumps=N idle=M budget_ms avg=A max=B used_est_ms avg=C max=D bound=P% tasks considered=X run=Y (Z/frame) queue_after pending avg=a max=b active avg=c max=d
// backlog=P% streak_max_ms=S oldest_age_ms=O prio=[v:n,...]`. A frame is a pump call with something queued at entry; idle is a pump call with both rings empty.
inline void formatSummary(const Window& w, char* out, size_t n) {
    char prio[160] = "-";
    size_t used = 0;
    if (w.prioN) {
        used += size_t(std::snprintf(prio, sizeof(prio), "%u of ", w.prioSampled));
        for (uint32_t k = 0; k < w.prioN && k < 8 && used + 24 < sizeof(prio); ++k)
            used += size_t(std::snprintf(prio + used, sizeof(prio) - used, "%s%d:%u", k ? "," : "[", w.prioVal[k], w.prioCnt[k]));
        if (used + 2 < sizeof(prio)) std::snprintf(prio + used, sizeof(prio) - used, "]");
    }
    std::snprintf(out, n,
                  "planet lod trace: %.1f s: pumps=%llu idle=%llu budget_ms avg=%.2f max=%.2f used_est_ms avg=%.2f max=%.2f bound=%.0f%% zero_budget=%llu tasks considered=%llu run=%llu (%.1f/frame) "
                  "queue_after pending avg=%.1f max=%llu active avg=%.1f max=%llu backlog=%.0f%% streak_max_ms=%llu oldest_age_ms=%llu prio=%s",
                  double(w.ms) / 1000.0, (unsigned long long)w.pumps, (unsigned long long)w.idle, ms(uint64_t(per(w.budgetSumUs, w.pumps))), ms(w.budgetMaxUs),
                  ms(uint64_t(per(w.usedSumUs, w.pumps))), ms(w.usedMaxUs), 100.0 * per(w.bound, w.pumps), (unsigned long long)w.zeroBudget,
                  (unsigned long long)w.considered, (unsigned long long)w.runs, per(w.runs, w.pumps), per(w.pendSum, w.pumps), (unsigned long long)w.pendMax,
                  per(w.actSum, w.pumps), (unsigned long long)w.actMax, 100.0 * per(w.backlog, w.pumps), (unsigned long long)w.streakMax, (unsigned long long)w.ageMax, prio);
}

// LINE 2. `planet lod trace: budget call: n=N T_ms=a..b multiplier=c..d boost_seen=K est_ms_per_run=e work_per_run=f types=[0xID/gran:runs:work:ms ...]` (top five by estimated ms; the
// id is the task type's interned name id, the granularity is the work table's `size`: 64 Combined_Planet, 256 TransformToSAPatchSpace / PatchLighting / Atmospheres, 153 Skirts, 1 Assembly / BC).
inline void formatBudgetAndTypes(const Window& w, char* out, size_t n) {
    Window::Type top[5];
    int tn = 0;
    bool taken[kTypeSlots] = {};
    for (int r = 0; r < 5; ++r) {
        int best = -1;
        for (uint32_t i = 0; i < w.typeN; ++i)
            if (!taken[i] && (best < 0 || w.types[i].us > w.types[best].us)) best = int(i);
        if (best < 0) break;
        taken[best] = true;
        top[tn++] = w.types[best];
    }
    char types[400] = "-";
    size_t used = 0;
    for (int i = 0; i < tn && used + 60 < sizeof(types); ++i)
        used += size_t(std::snprintf(types + used, sizeof(types) - used, "%s0x%04X/%u:%llu:%llu:%.2f", i ? " " : "", top[i].id, top[i].gran, (unsigned long long)top[i].runs,
                                     (unsigned long long)top[i].work, ms(top[i].us)));
    const bool haveB = w.bCalls > 0;
    std::snprintf(out, n, "planet lod trace: budget call: n=%llu T_ms=%.2f..%.2f multiplier=%.4f..%.4f boost_seen=%llu est_ms_per_run=%.3f work_per_run=%.0f types=[%s]%s",
                  (unsigned long long)w.bCalls, haveB ? ms(w.bTminUs) : 0.0, ms(w.bTmaxUs), haveB ? double(w.bMultMin) / 1.0e6 : 0.0, double(w.bMultMax) / 1.0e6, (unsigned long long)w.bBoost,
                  ms(uint64_t(per(w.estUs, w.runs))), per(w.work, w.runs), types, w.typeOverflow ? " (type table full)" : "");
}

// LINE 3 (only when a node was visited). `planet lod trace: tree: nodes=N/frame wanting_finer=W children=C requests=Q new_handles=H by_level [L:none/pending/ready ...] new_by_level [L:n ...]`
inline void formatTree(const Window& w, char* out, size_t n) {
    const uint64_t frames = w.pumps + w.idle;   // the budget call's frames are the best per-frame divisor the tree has; the rig pins this
    char lv[420] = "-", rq[200] = "-";
    size_t u = 0, r = 0;
    for (int l = 0; l < kLevels; ++l) {
        const uint64_t a = w.state[l][0], b = w.state[l][1], c = w.state[l][2];
        if (a + b + c && u + 48 < sizeof(lv)) u += size_t(std::snprintf(lv + u, sizeof(lv) - u, "%s%d:%llu/%llu/%llu", u ? " " : "", l, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c));
        if (w.reqLevel[l] && r + 24 < sizeof(rq)) r += size_t(std::snprintf(rq + r, sizeof(rq) - r, "%s%d:%llu", r ? " " : "", l, (unsigned long long)w.reqLevel[l]));
    }
    std::snprintf(out, n, "planet lod trace: tree: nodes=%llu (%.0f/frame) wanting_finer=%llu children=%llu requests=%llu new_handles=%llu by_level [%s] new_by_level [%s]",
                  (unsigned long long)w.nodes, per(w.nodes, frames), (unsigned long long)w.wantFiner, (unsigned long long)w.withChildren, (unsigned long long)w.reqCalls,
                  (unsigned long long)w.reqNew, lv, rq);
}

// The idle line, at most once a minute while nothing was queued: proves which hooks have EVER run, so a zero is a measurement and a missing line is a hook that did not.
inline void formatIdle(const Window& w, char* out, size_t n) {
    std::snprintf(out, n, "planet lod trace: idle: nothing queued in the last window; hooks ever run: budget=%llu pump=%llu grant=%llu node=%llu request=%llu (this window: pump calls with empty rings=%llu, nodes=%llu)",
                  (unsigned long long)w.life[kBudget], (unsigned long long)w.life[kPump], (unsigned long long)w.life[kGrant], (unsigned long long)w.life[kNode],
                  (unsigned long long)w.life[kRequest], (unsigned long long)w.idle, (unsigned long long)w.nodes);
}

inline void formatLive(char* out, size_t n, const char* how) {
    std::snprintf(out, n,
                  "planet lod trace: hooks live: budget exe+0x%X pump exe+0x%X grant exe+0x%X node exe+0x%X request exe+0x%X (%s). Read-only: no argument, result or game byte is changed. "
                  "Per 5 s while work is queued: summary, budget call, tree, threads lines; idle line at most once a minute.",
                  kPins[kBudget].rva, kPins[kPump].rva, kPins[kGrant].rva, kPins[kNode].rva, kPins[kRequest].rva, how);
}
inline void formatInert(char* out, size_t n, const char* why) {
    std::snprintf(out, n, "planet lod trace: inert: %s; nothing was patched", why);
}

}  // namespace plt
}  // namespace edvr
