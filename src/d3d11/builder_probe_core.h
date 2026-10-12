#pragma once
// The builder probe's pure half (builder_probe.cpp; TEMPORARY, hotkey.builder_probe, docs\concourse-culling-2026-10-11.md, Built v8).
// No Windows and no engine memory: the record base and argument agreement at the builder's entry, the two call sites, the grouping by
// record name, and the text of the CSV rows and the "builder probe:" summary line. The rig tools\builder_probe_test compiles it alone.
//
// The builder 0x42B4420 is called from two sites (PROVEN, docs Dynamic actors D2): RVA 0x43204F3 (the traversal) and RVA 0x4321ADB (the
// single-entry form). A call's return address is therefore 0x43204F8 and 0x4321AE0. At the builder's entry r9 = rec+0x210, so the record base
// is r9 - 0x210; rcx is the VALUE stored at rec+0x290 (corrected in Built v8, see argsAgree). Agreement and disagreement are both counted.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace edvr::builder {

constexpr uint64_t kRecFromR9 = 0x210;     // record base = r9 - 0x210
constexpr uint64_t kRecFromRcx = 0x290;    // rcx = record base + 0x290
constexpr uint32_t kCallerA = 0x43204F8u;  // RVA: return address after the call at 0x43204F3 (the traversal)
constexpr uint32_t kCallerB = 0x4321AE0u;  // RVA: return address after the call at 0x4321ADB (the single-entry form)
constexpr size_t kCallRowCap = 4000;       // per-call rows written to the CSV per frame
constexpr size_t kTopNames = 20;           // names in the summary line

enum class Caller : uint8_t { kA = 0, kB = 1, kOther = 2 };

inline Caller callerOf(uint32_t rva) noexcept {
    if (rva == kCallerA) return Caller::kA;
    if (rva == kCallerB) return Caller::kB;
    return Caller::kOther;
}

inline const char* callerName(Caller c) noexcept {
    switch (c) {
        case Caller::kA: return "A";
        case Caller::kB: return "B";
        default: return "other";
    }
}

// The entry check. The disassembly (docs Built v8, PROVEN) shows rcx is the VALUE stored at rec+0x290 (the caller loads [rbx+0x5C]), not the
// address of that field, and r9 is the address rec+0x210. So the record's base is r9 - 0x210, and the builder's rcx agrees with it when the
// memory at base+0x290 (read by the probe, SEH-guarded) holds exactly rcx. A failed read or a too-small r9 counts as a disagreement.
inline bool argsAgree(uint64_t rcx, uint64_t r9, bool value290Read, uint64_t value290) noexcept {
    if (r9 < kRecFromR9 || !value290Read) return false;
    return value290 == rcx;
}

// The record base the builder was given (r9 - 0x210). Zero when r9 is too small to hold one.
inline uint64_t recordBase(uint64_t r9) noexcept { return r9 < kRecFromR9 ? 0 : r9 - kRecFromR9; }

// One builder call, as the probe saw it at the call (the fields the store wrote are read then; the store's visit is checked at the frame's end).
struct Call {
    uint64_t rec = 0;
    uint64_t mask = 0;           // rec+0x208
    uint32_t admitted = 0;       // rec+0x234 (one byte)
    float centre[3] = {};        // rec+0x240
    float radius = 0.0f;         // rec+0x280
    bool sphereOk = false;       // the centre and radius were read and are finite
    float dist = -1.0f;          // metres from the frame's camera (-1 when no camera or no sphere)
    bool hasName = false;
    std::string name;            // rec+0x18 -> node, node+0 -> text; empty when unreadable
    bool agree = false;          // rcx agreed with r9 at the entry
    Caller caller = Caller::kOther;
    uint32_t retRva = 0;
    bool storeSeen = false;      // set at the frame's end: the admission store visited this record in the frame
};

// Per-name group: the count, the store-seen split, the radius range and the nearest distance.
struct Group {
    std::string name;
    uint64_t count = 0;
    uint64_t seenYes = 0;
    uint64_t seenNo = 0;
    float radiusMin = 0.0f, radiusMax = 0.0f;
    bool radiusSeen = false;
    float distMin = -1.0f;       // -1 when no call had a distance
};

inline std::map<std::string, Group> groupByName(const std::vector<Call>& calls) {
    std::map<std::string, Group> out;
    for (const Call& c : calls) {
        const std::string key = c.hasName ? c.name : std::string("(no name)");
        Group& g = out[key];
        if (g.count == 0) g.name = key;
        ++g.count;
        if (c.storeSeen) ++g.seenYes;
        else ++g.seenNo;
        if (c.sphereOk) {
            if (!g.radiusSeen || c.radius < g.radiusMin) g.radiusMin = c.radius;
            if (!g.radiusSeen || c.radius > g.radiusMax) g.radiusMax = c.radius;
            g.radiusSeen = true;
        }
        if (c.dist >= 0.0f && (g.distMin < 0.0f || c.dist < g.distMin)) g.distMin = c.dist;
    }
    return out;
}

struct Summary {
    uint64_t calls = 0;
    uint64_t distinctRecords = 0;
    uint64_t unseen = 0;          // calls whose record the store did not visit in the frame (the H5 signature)
    uint64_t agree = 0, disagree = 0;
    uint64_t callerA = 0, callerB = 0, callerOther = 0;
    std::vector<Group> topByCount;   // up to kTopNames groups, most calls first (ties by name)
};

inline Summary summarise(const std::vector<Call>& calls) {
    Summary s;
    std::map<uint64_t, bool> distinct;
    for (const Call& c : calls) {
        ++s.calls;
        distinct[c.rec] = true;
        if (!c.storeSeen) ++s.unseen;
        if (c.agree) ++s.agree;
        else ++s.disagree;
        switch (c.caller) {
            case Caller::kA: ++s.callerA; break;
            case Caller::kB: ++s.callerB; break;
            default: ++s.callerOther; break;
        }
    }
    s.distinctRecords = distinct.size();
    std::vector<Group> groups;
    for (auto& kv : groupByName(calls)) groups.push_back(kv.second);
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.name < b.name;
    });
    if (groups.size() > kTopNames) groups.resize(kTopNames);
    s.topByCount = groups;
    return s;
}

// "builder probe: frame F: calls N, distinct records R, not seen by the store U (H5 signature), agree A, disagree D, callers A/B/other a/b/o".
// Always written, even at zero: a zero line is a probe that ran. The names follow in formatNamesLine lines (the log line holds 1200 bytes).
inline int formatSummary(char* out, size_t cap, uint32_t frame, const Summary& s) noexcept {
    return std::snprintf(out, cap,
                         "builder probe: frame %u: calls %llu, distinct records %llu, not seen by the store this frame %llu (H5 "
                         "signature), args agree %llu, disagree %llu, callers A/B/other %llu/%llu/%llu",
                         frame, static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.distinctRecords),
                         static_cast<unsigned long long>(s.unseen), static_cast<unsigned long long>(s.agree),
                         static_cast<unsigned long long>(s.disagree), static_cast<unsigned long long>(s.callerA),
                         static_cast<unsigned long long>(s.callerB), static_cast<unsigned long long>(s.callerOther));
}

constexpr size_t kNamesPerLine = 8;

// One line of the top names, from index `from` (the top 20 by calls, most first): "builder probe: frame F names (top 20 by calls, from K):
// name xCOUNT (unseen U); ...". Returns the number of names written, through *written (0 when there are none from `from`).
inline int formatNamesLine(char* out, size_t cap, uint32_t frame, const Summary& s, size_t from, size_t* written) noexcept {
    *written = 0;
    if (from >= s.topByCount.size()) return 0;
    int n = std::snprintf(out, cap, "builder probe: frame %u names (top %zu by calls, from %zu):", frame, s.topByCount.size(), from + 1);
    if (n < 0) return n;
    for (size_t i = from; i < s.topByCount.size() && *written < kNamesPerLine; ++i) {
        const Group& g = s.topByCount[i];
        if (static_cast<size_t>(n) >= cap) break;
        const int m = std::snprintf(out + n, cap - static_cast<size_t>(n), " %s x%llu (unseen %llu);", g.name.c_str(),
                                    static_cast<unsigned long long>(g.count), static_cast<unsigned long long>(g.seenNo));
        if (m < 0) break;
        n += m;
        ++*written;
    }
    return n;
}

inline const char* groupCsvHeader() noexcept {
    return "name,count,seen_yes,seen_no,radius_min_m,radius_max_m,dist_min_m";
}

// One group's row: radii and distance are -1 when no call measured them.
inline int formatGroupRow(char* out, size_t cap, const Group& g) noexcept {
    return std::snprintf(out, cap, "%s,%llu,%llu,%llu,%.2f,%.2f,%.2f", g.name.c_str(), static_cast<unsigned long long>(g.count),
                         static_cast<unsigned long long>(g.seenYes), static_cast<unsigned long long>(g.seenNo),
                         static_cast<double>(g.radiusSeen ? g.radiusMin : -1.0f), static_cast<double>(g.radiusSeen ? g.radiusMax : -1.0f),
                         static_cast<double>(g.distMin));
}

inline const char* callCsvHeader() noexcept {
    return "rec,name,mask,admitted,centre_x,centre_y,centre_z,radius_m,dist_m,store_seen,args_agree,caller,ret_rva";
}

inline int formatCallRow(char* out, size_t cap, const Call& c) noexcept {
    return std::snprintf(out, cap, "0x%llX,%s,0x%llX,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%s,0x%X",
                         static_cast<unsigned long long>(c.rec), c.hasName ? c.name.c_str() : "(no name)",
                         static_cast<unsigned long long>(c.mask), c.admitted, static_cast<double>(c.centre[0]),
                         static_cast<double>(c.centre[1]), static_cast<double>(c.centre[2]),
                         static_cast<double>(c.sphereOk ? c.radius : -1.0f), static_cast<double>(c.dist), c.storeSeen ? 1 : 0,
                         c.agree ? 1 : 0, callerName(c.caller), c.retRva);
}

// ---------------------------------------------------------------------------
// The shared collection (Built v8.1, the crash fix). The store and builder detours write it on the game's threads; the frame tick drains it on
// the render thread. Every container is touched only inside the collector's lock, and the lock covers that one container operation: nothing
// holds it across a call into the game (the store is recursive, and the builder's forward re-enters the store). Storage is reserved before
// the lock is taken, so a write takes no allocation: a write past the reserve is counted as a drop.

// A fixed-capacity open-addressing set of record addresses. reset() sets the storage (it allocates; call it before taking a lock); insert()
// and contains() never allocate. Key 0 is ignored.
class RecordSet {
public:
    enum class Result : uint8_t { kAdded, kPresent, kFull };

    void reset(size_t records) {
        size_t slots = 16;
        while (slots < records * 2) slots <<= 1;
        table_.assign(slots, 0);
        limit_ = records;
        used_ = 0;
    }

    Result insert(uintptr_t key) noexcept {
        if (!key || table_.empty()) return Result::kPresent;
        const size_t mask = table_.size() - 1;
        size_t i = hashOf(key) & mask;
        for (size_t probes = 0; probes < table_.size(); ++probes, i = (i + 1) & mask) {
            if (table_[i] == key) return Result::kPresent;
            if (table_[i] == 0) {
                if (used_ >= limit_) return Result::kFull;
                table_[i] = key;
                ++used_;
                return Result::kAdded;
            }
        }
        return Result::kFull;
    }

    bool contains(uintptr_t key) const noexcept {
        if (!key || table_.empty()) return false;
        const size_t mask = table_.size() - 1;
        size_t i = hashOf(key) & mask;
        for (size_t probes = 0; probes < table_.size(); ++probes, i = (i + 1) & mask) {
            if (table_[i] == key) return true;
            if (table_[i] == 0) return false;
        }
        return false;
    }

    size_t size() const noexcept { return used_; }

private:
    static size_t hashOf(uintptr_t key) noexcept {
        return static_cast<size_t>((static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull) >> 17);
    }
    std::vector<uintptr_t> table_;
    size_t limit_ = 0;
    size_t used_ = 0;
};

// The distinct thread ids one detour saw in the frame: up to kThreadIds are kept; a thread past them sets `more`.
constexpr size_t kThreadIds = 16;
struct ThreadSet {
    uint32_t ids[kThreadIds] = {};
    uint32_t count = 0;
    bool more = false;
    void note(uint32_t tid) noexcept {
        for (uint32_t i = 0; i < count; ++i)
            if (ids[i] == tid) return;
        if (count < kThreadIds) ids[count++] = tid;
        else more = true;
    }
};

// One collection's storage: the calls, the store's visits, the thread ids of each detour, the drops, and the render context the store read.
struct Storage {
    std::vector<Call> calls;
    RecordSet seen;
    ThreadSet storeThreads;
    ThreadSet builderThreads;
    uint64_t callDrops = 0;
    uint64_t seenDrops = 0;
    uintptr_t ctx = 0;

    // Allocates the reserves (call before taking the lock).
    void reserve(size_t callCap, size_t recordCap) {
        calls.clear();
        calls.reserve(callCap);
        seen.reset(recordCap);
        storeThreads = ThreadSet{};
        builderThreads = ThreadSet{};
        callDrops = 0;
        seenDrops = 0;
        ctx = 0;
    }
};

// The lock is any type with lock() and unlock() (std::mutex in the engine; the rig drives the same code with threads).
template <class Mutex>
class Collector {
public:
    // Puts `next` in place of the current storage and returns the old storage whole. Both are moved, never copied; the lock covers the swap.
    Storage exchange(Storage&& next) {
        std::lock_guard<Mutex> lock(mutex_);
        std::swap(storage_, next);
        return std::move(next);
    }

    // A builder call. Pushed when the reserve has room; otherwise counted as a drop. The call's name and fields were read before this.
    void addCall(Call&& c, uint32_t tid) {
        std::lock_guard<Mutex> lock(mutex_);
        storage_.builderThreads.note(tid);
        if (storage_.calls.size() < storage_.calls.capacity()) storage_.calls.push_back(std::move(c));
        else ++storage_.callDrops;
    }

    // The store visited a record in the frame (and read the render context from its bucket, when it could).
    void noteStore(uintptr_t record, uintptr_t ctx, uint32_t tid) {
        std::lock_guard<Mutex> lock(mutex_);
        storage_.storeThreads.note(tid);
        if (ctx) storage_.ctx = ctx;
        if (storage_.seen.insert(record) == RecordSet::Result::kFull) ++storage_.seenDrops;
    }

private:
    Mutex mutex_;
    Storage storage_;
};

}  // namespace edvr::builder
