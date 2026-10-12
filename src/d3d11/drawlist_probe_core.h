#pragma once
// The draw-list probe's pure half (drawlist_probe.cpp; TEMPORARY, Built v9, hypothesis H-PERSIST in docs\concourse-culling-2026-10-11.md).
// No Windows and no engine memory: the origin classes, the set of record keys appended in each collected frame (and which ones an earlier
// frame of the window appended, the carried candidates), the per-frame and per-row text. The rig tools\drawlist_probe_test compiles this alone.
//
// What the probe can and cannot say (the join is NOT live, and the summary says so): a scene draw is not mapped to its pool record on the CPU.
// Engine motion's draw-side join is per pixel on the GPU (MRT6 slot codes, engine_velocity.h), so every scene draw's origin is "unattributed".
// What the probe does see is producer-side: which pool records were appended in each collected frame (path A, the builder; path B, producer
// FUN_144312E00), the pool copy, merge and clear calls per collection, and the substituted pool draws per family. A record appended in an
// earlier frame of the window and not appended in this one is a CARRIED CANDIDATE: it can be drawn from the pool without being appended again.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace edvr::drawlist {

constexpr uint32_t kFrames = 3;          // the collected window: three consecutive frames
constexpr uint32_t kFamilySlots = 16;    // engine_velocity.h kMaxFamilies
constexpr size_t kKeyCap = 1024;         // distinct keys tracked per window (the rest are counted, not tracked)
constexpr int kColumns = 15;             // fields in every CSV row, the header included

// Where a record was seen, in the order the classes are tested.
enum class Origin : uint8_t { kAppendedA = 0, kAppendedB = 1, kCarried = 2, kUnattributed = 3 };

// appendedA / appendedB: appended in this frame by path A (the builder) or path B (producer FUN_144312E00). earlier: appended in an earlier
// frame of the window and not in this one. Nothing seen: unattributed (a scene draw with no producer-side record has no origin here).
inline Origin classifyOrigin(bool appendedA, bool appendedB, bool earlier) noexcept {
    if (appendedA) return Origin::kAppendedA;
    if (appendedB) return Origin::kAppendedB;
    if (earlier) return Origin::kCarried;
    return Origin::kUnattributed;
}

inline const char* originName(Origin o) noexcept {
    switch (o) {
        case Origin::kAppendedA: return "appended_A";
        case Origin::kAppendedB: return "appended_B";
        case Origin::kCarried: return "carried";
        default: return "unattributed";
    }
}

// The keys appended in each frame of the window. beginFrame closes the current frame into the union of the earlier frames; noteAppended
// returns true the first time a key is seen in the current frame. carried() counts keys in an earlier frame that the current frame did not
// append. A full set counts the overflow and tracks no more keys (overflow() says how many).
class KeySet {
public:
    void reset() {
        cur_.clear();
        earlier_.clear();
        overflow_ = 0;
    }
    void beginFrame() {
        for (uint64_t k : cur_) addUnique(earlier_, k);
        cur_.clear();
    }
    bool noteAppended(uint64_t key) {
        for (uint64_t k : cur_)
            if (k == key) return false;
        if (cur_.size() >= kKeyCap) {
            ++overflow_;
            return false;
        }
        cur_.push_back(key);
        return true;
    }
    bool appendedThisFrame(uint64_t key) const {
        for (uint64_t k : cur_)
            if (k == key) return true;
        return false;
    }
    bool seenEarlier(uint64_t key) const {
        for (uint64_t k : earlier_)
            if (k == key) return true;
        return false;
    }
    size_t distinctThisFrame() const { return cur_.size(); }
    // Keys in an earlier frame that this frame did not append: the carried candidates of this frame.
    size_t carried() const {
        size_t n = 0;
        for (uint64_t k : earlier_)
            if (!appendedThisFrame(k)) ++n;
        return n;
    }
    uint64_t overflow() const { return overflow_; }

private:
    static void addUnique(std::vector<uint64_t>& v, uint64_t k) {
        for (uint64_t x : v)
            if (x == k) return;
        if (v.size() < kKeyCap) v.push_back(k);
    }
    std::vector<uint64_t> cur_;
    std::vector<uint64_t> earlier_;
    uint64_t overflow_ = 0;
};

// The numbers of one collected frame, as the summary line states them.
struct FrameCounts {
    uint32_t frame = 0;
    uint32_t window = 0;             // 1..kFrames: which frame of the window this is
    const char* mode = "off";        // the trial's record rule, or "off" (the probe runs the same in any mode)
    bool engineLive = false;         // engineVelocityActive(): the producer B and family taps can fire
    bool builderPathBracket = false; // the builder taps fire on the bracket path only (builder_probe.cpp)
    uint64_t builderCalls = 0;       // path A calls seen through the bracket observer
    uint64_t emitCalls = 0;          // path B calls seen (FUN_144312E00's observer)
    int64_t emitItems = 0;           // pool records those path B calls appended (after - before, summed)
    uint64_t copyCalls = 0;          // pool copy calls (mapped base, records)
    uint64_t copyRecords = 0;
    uint64_t mergeCalls = 0;
    uint64_t clearCalls = 0;
    uint64_t familyDraws = 0;        // substituted pool draws this frame (every family)
    size_t appendedA = 0;            // distinct record keys path A appended this frame
    size_t appendedB = 0;            // distinct record keys path B appended this frame
    size_t carried = 0;              // carried candidates: appended earlier in the window, not this frame
    size_t rebuilt = 0;              // distinct collections a copy, merge or clear touched this frame
    uint64_t keysOverflow = 0;       // keys past the tracking cap (counted, not tracked)
    uint64_t cCalls = 0;             // Built v11: path C (FUN_14369C9C0, producer 1's bracket) calls, after the forward
    int64_t cItems = 0;              // the pool records those calls appended (the owner's count delta)
    uint64_t cFaults = 0;            // C calls whose counters could not be read (their items are not counted)
    // Built v12: the early rig veto's decisions taken in this frame (sealedTrialRigTake): seen, vetoed (or would veto), mixed, unknown, refresh,
    // the records matched to a rig evaluated this frame, and the decision time in ms.
    uint64_t rigSeen = 0;
    uint64_t rigVetoed = 0;
    uint64_t rigMixed = 0;
    uint64_t rigUnknown = 0;
    uint64_t rigRefresh = 0;
    uint64_t rigMatched = 0;
    double rigMs = 0.0;
};

// The per-frame summary line. joinLine is the fixed statement about the draw join (joinStatement()).
// names: the caller's top-15 name lists per origin class (already formatted; may be empty).
inline int formatFrameLine(char* out, size_t cap, const FrameCounts& c, const char* names) noexcept {
    const char* engine = c.engineLive ? "live" : "NOT live (the emit observer is not installed, so producer B and the family draws are zero "
                                                 "for that reason, not because nothing was drawn)";
    const char* bracket = c.builderPathBracket ? "bracket path" : "NOT the bracket path, so A is zero for that reason";
    return std::snprintf(out, cap,
                         "drawlist probe: frame %u (window %u of %u; mode %s; engine motion %s): producer taps: builder A calls %llu "
                         "(%s), producer B calls %llu appended %lld items, copy %llu calls %llu records, merge %llu, clear %llu; "
                         "substituted pool draws %llu; appended this frame A %zu, B %zu; carried candidates %zu; collections rebuilt %zu. "
                         "path C (FUN_14369C9C0, the producer-1 bracket): calls %llu, items %lld, faults %llu; path D (FUN_14434D120 via "
                         "D470): not hooked in this build, not counted. Early rig veto this frame: seen %llu, vetoed %llu, mixed %llu, unknown %llu, "
                         "refresh %llu, records matched %llu, decision ms %.3f.%s%s%s",
                         c.frame, c.window, kFrames, c.mode, engine, static_cast<unsigned long long>(c.builderCalls), bracket,
                         static_cast<unsigned long long>(c.emitCalls), static_cast<long long>(c.emitItems),
                         static_cast<unsigned long long>(c.copyCalls), static_cast<unsigned long long>(c.copyRecords),
                         static_cast<unsigned long long>(c.mergeCalls), static_cast<unsigned long long>(c.clearCalls),
                         static_cast<unsigned long long>(c.familyDraws), c.appendedA, c.appendedB, c.carried, c.rebuilt,
                         static_cast<unsigned long long>(c.cCalls), static_cast<long long>(c.cItems),
                         static_cast<unsigned long long>(c.cFaults), static_cast<unsigned long long>(c.rigSeen),
                         static_cast<unsigned long long>(c.rigVetoed), static_cast<unsigned long long>(c.rigMixed),
                         static_cast<unsigned long long>(c.rigUnknown), static_cast<unsigned long long>(c.rigRefresh),
                         static_cast<unsigned long long>(c.rigMatched), c.rigMs,
                         c.keysOverflow ? " (key tracking overflowed; the counts are exact, the key sets are not)" : "",
                         names && names[0] ? " " : "", names ? names : "");
}

// The fixed statement of the draw join, printed in every window summary (the join is not live; see the header comment).
inline const char* joinStatement() noexcept {
    return "Per-draw join: NOT LIVE. The CPU has no map from a scene draw to its pool record; engine motion joins per pixel on the GPU. "
           "Every scene draw's origin is unattributed; carried counts are candidates from the producer side, not draws.";
}

// One CSV row. Every field is text except the numbers, which print as decimal (or "-" when not counted).
struct CsvRow {
    const char* table = "event";     // event, family, collection or frame
    uint32_t frame = 0;
    const char* kind = "-";          // builder, emit, copy, merge, clear, family
    uint64_t record = 0;
    uint64_t owner = 0;
    uint64_t key = 0;
    int64_t before = -1;             // -1 prints "-"
    int64_t after = -1;
    int64_t appended = -1;
    const char* family = "-";
    int64_t draws = -1;
    const char* name = "-";
    const char* origin = "-";
    const char* mode = "-";
    const char* rebuilt = "-";
};

inline const char* csvHeader() noexcept {
    return "table,frame,kind,record,owner,key,before,after,appended,family,draws,name,origin,mode,rebuilt";
}

// Commas and newlines in a name would split the row; they are replaced, so the field count holds for any name.
inline void sanitise(char* dst, size_t cap, const char* src) noexcept {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) {
        const char ch = src[i];
        dst[i] = (ch == ',' || ch == '\n' || ch == '\r') ? ';' : ch;
    }
    dst[i] = '\0';
}

inline int formatRow(char* out, size_t cap, const CsvRow& r) noexcept {
    char name[96];
    sanitise(name, sizeof(name), r.name);
    auto num = [](char* buf, size_t n, int64_t v) {
        if (v < 0) std::snprintf(buf, n, "-");
        else std::snprintf(buf, n, "%lld", static_cast<long long>(v));
    };
    char before[24], after[24], appended[24], draws[24];
    num(before, sizeof(before), r.before);
    num(after, sizeof(after), r.after);
    num(appended, sizeof(appended), r.appended);
    num(draws, sizeof(draws), r.draws);
    return std::snprintf(out, cap, "%s,%u,%s,0x%llX,0x%llX,0x%llX,%s,%s,%s,%s,%s,%s,%s,%s,%s", r.table, r.frame, r.kind,
                         static_cast<unsigned long long>(r.record), static_cast<unsigned long long>(r.owner),
                         static_cast<unsigned long long>(r.key), before, after, appended, r.family, draws, name, r.origin, r.mode,
                         r.rebuilt);
}

}  // namespace edvr::drawlist
