#pragma once
// The asset census's pure half (asset_census.cpp; TEMPORARY, hotkey.asset_census, docs\concourse-culling-2026-10-11.md, Built v7).
// No Windows and no engine memory: every engine read goes through the Reader the caller supplies, so tools\asset_census_test runs
// the same chain reader, vtable check, grouping and CSV row as the game.
//
// The identity chain, build 332841. decomp_4330194 writes the engine's own copy into each record; decomp_4312040 reads the record.
//   rec+0x2B8   SRC, the record's source node. Zero: the record carries no identity block.
//   SRC+0xF0    key1 (u64).                 SRC+0x1A0  key2 (u64).
//   SRC+0x160   X, the owning component. The store reads X's byte +0x4B and X's pointer +0x470.
//   X+0x470     M, the model.
//   M+0x08      u32; bits 17..27 give the count of M's qwords, which start at M+0x10.
//   rec+0x250   the engine's copy of key1; rec+0x258 of M; rec+0x260 of key2; rec+0x268 the u32 combined hash.
//   combined = mix(key1) ^ mix(key2) ^ (XOR over the list of mix(item)), where mix(x) = low32(h >> 22) ^ low32(h) and
//   h = wang(x): h = (x << 18) + ~x; h = (h >> 31 ^ h) * 21; h = (h >> 11 ^ h) * 65, all in 64-bit wrapping arithmetic.
// The hash and the copy are checked against the live chain, so a read that disagrees is reported, not believed.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "sealed_trial_core.h"

namespace edvr::census {

// Offsets in the record (0x2F0 bytes), its source node, its component and its model.
constexpr size_t kRecordBytes = 0x2F0;
constexpr size_t kSourceBlockBytes = 0x1A8;   // SRC+0x000 .. SRC+0x1A7: holds key1 (+0xF0), X (+0x160) and key2 (+0x1A0)
constexpr uintptr_t kBucketBase = 0x28;       // the collection's base record
constexpr uintptr_t kComponentModel = 0x470;  // X+0x470: M
constexpr uintptr_t kModelCount = 0x08;       // M+0x08: u32, count in bits 17..27
constexpr uintptr_t kModelList = 0x10;        // M+0x10: the qword list
constexpr uint32_t kModelListMax = 0x7FF;
constexpr uintptr_t kUserLow = 0x10000;
constexpr uintptr_t kUserHigh = 0x800000000000ull;   // the user-mode address range (47 bits)
constexpr size_t kMaxNames = 4;                      // names kept for one group
constexpr size_t kNameBytes = 96;                    // bytes read at a pointer when looking for text
constexpr size_t kNameMax = 48;                      // longest text kept

inline bool isUserAddress(uintptr_t a) noexcept { return a >= kUserLow && a < kUserHigh; }

// A bounded read of engine memory: false when the bytes cannot be read. The game's read is SEH-guarded; the rig's is a map.
using ReadFn = bool (*)(void* ctx, uintptr_t address, void* dst, size_t bytes);
struct Reader {
    ReadFn read = nullptr;
    void* ctx = nullptr;
    bool bytes(uintptr_t address, void* dst, size_t n) const {
        return read != nullptr && n > 0 && isUserAddress(address) && read(ctx, address, dst, n);
    }
    bool u64(uintptr_t address, uint64_t* out) const { return bytes(address, out, sizeof(*out)); }
    bool u32(uintptr_t address, uint32_t* out) const { return bytes(address, out, sizeof(*out)); }
};

inline uint16_t le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}
inline uint32_t le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The image's sections the vtable check needs, read from the PE headers in memory.
struct Span {
    uintptr_t lo = 0, hi = 0;
    bool has(uintptr_t a) const noexcept { return hi > lo && a >= lo && a < hi; }
};
struct Image {
    uintptr_t base = 0;
    Span text, rdata, data;
    bool ready() const noexcept { return base != 0 && text.hi > text.lo && (rdata.hi > rdata.lo || data.hi > data.lo); }
};

// Reads the DOS and NT headers at base and the section table. False when the headers do not read as a PE image or .text is absent.
inline bool parseImage(const Reader& r, uintptr_t base, Image* out) {
    *out = Image{};
    uint8_t dos[0x40];
    if (!r.bytes(base, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return false;
    const uint32_t lfanew = le32(dos + 0x3C);
    if (lfanew < 0x40 || lfanew > 0x1000) return false;
    uint8_t nt[24];
    if (!r.bytes(base + lfanew, nt, sizeof(nt)) || le32(nt) != 0x00004550u) return false;   // "PE\0\0"
    const uint16_t sections = le16(nt + 6);
    const uint16_t optionalBytes = le16(nt + 20);
    if (sections == 0 || sections > 64) return false;
    const uintptr_t table = base + lfanew + 24 + optionalBytes;
    Image img;
    img.base = base;
    for (uint16_t i = 0; i < sections; ++i) {
        uint8_t sh[40];
        if (!r.bytes(table + static_cast<uintptr_t>(i) * 40u, sh, sizeof(sh))) return false;
        const uint32_t virtualSize = le32(sh + 8), virtualAddress = le32(sh + 12);
        const Span span{base + virtualAddress, base + virtualAddress + virtualSize};
        if (std::memcmp(sh, ".text\0\0\0", 8) == 0) img.text = span;
        else if (std::memcmp(sh, ".rdata\0\0", 8) == 0) img.rdata = span;
        else if (std::memcmp(sh, ".data\0\0\0", 8) == 0) img.data = span;
    }
    if (!img.ready()) return false;
    *out = img;
    return true;
}

// The vtable check: obj's first qword lies in .rdata or .data, and that vtable's first slot lies in .text. False otherwise.
inline bool vtableOf(const Reader& r, const Image& img, uintptr_t obj, uintptr_t* vtable) {
    uint64_t vt = 0, slot = 0;
    if (!r.u64(obj, &vt) || !(img.rdata.has(static_cast<uintptr_t>(vt)) || img.data.has(static_cast<uintptr_t>(vt)))) return false;
    if (!r.u64(static_cast<uintptr_t>(vt), &slot) || !img.text.has(static_cast<uintptr_t>(slot))) return false;
    if (vtable) *vtable = static_cast<uintptr_t>(vt);
    return true;
}

// An address as an offset from the image base (0 when it is not in the image).
inline uint64_t rvaOf(uintptr_t p, const Image& img) noexcept {
    return (img.base != 0 && p >= img.base) ? static_cast<uint64_t>(p - img.base) : 0;
}

// The engine's hash of one key (decomp_4330194), and the combined identity hash it builds.
inline uint64_t wang(uint64_t x) noexcept {
    uint64_t h = x * 0x40000u + ~x;
    h = (h >> 31 ^ h) * 0x15u;
    return (h >> 11 ^ h) * 0x41u;
}
inline uint32_t mix(uint64_t x) noexcept {
    const uint64_t h = wang(x);
    return static_cast<uint32_t>(h >> 22) ^ static_cast<uint32_t>(h);
}
inline uint32_t identityHash(uint64_t key1, uint64_t key2) noexcept { return mix(key1) ^ mix(key2); }
inline uint32_t listFold(const uint64_t* items, size_t n) noexcept {
    uint32_t f = 0;
    for (size_t i = 0; i < n; ++i) f ^= mix(items[i]);
    return f;
}

enum class Status : uint8_t {
    kOk = 0,
    kNoSource,            // rec+0x2B8 is zero: no identity block
    kRecordUnreadable,    // the record's 0x2F0 bytes do not read
    kSourceUnreadable,    // SRC is not a readable user address
    kKeyUnreadable,       // SRC's key block does not read
    kComponentUnreadable, // X is not a readable user address
    kModelUnreadable,     // X+0x470 (M) is not a readable user address, or M's header does not read
    kListUnreadable,      // M's qword list does not read
};

inline const char* statusName(Status s) noexcept {
    switch (s) {
        case Status::kOk: return "ok";
        case Status::kNoSource: return "no-source";
        case Status::kRecordUnreadable: return "record-unreadable";
        case Status::kSourceUnreadable: return "source-unreadable";
        case Status::kKeyUnreadable: return "key-unreadable";
        case Status::kComponentUnreadable: return "component-unreadable";
        case Status::kModelUnreadable: return "model-unreadable";
        case Status::kListUnreadable: return "list-unreadable";
    }
    return "unknown";
}

// One record as the store observed it, with its live chain (zero where the chain did not read).
struct Observation {
    uintptr_t bucket = 0, record = 0, bucketBase = 0;
    Status status = Status::kRecordUnreadable;
    uint64_t mask = 0;             // rec+0x208, as the store left it
    uint8_t admitted = 0;          // rec+0x234
    float centre[3] = {};          // rec+0x240
    float radius = 0.0f;           // rec+0x280
    uint64_t children = 0;         // rec+0x2B0
    uintptr_t node = 0;            // rec+0x18
    uintptr_t source = 0;          // rec+0x2B8
    uintptr_t component = 0, model = 0;
    uint64_t key1 = 0, key2 = 0;   // SRC+0xF0 and SRC+0x1A0, read now
    uint32_t modelCount = 0;       // M's list length
    uint64_t modelFirst = 0;       // M's first list qword (0 when there is none)
    uint32_t hashLive = 0;         // the combined hash from the live chain
    uint32_t hashStored = 0;       // rec+0x268, the engine's copy
    bool blockAgrees = false;      // rec+0x250, +0x258 and +0x260 equal the live key1, M and key2
    bool hashAgrees = false;       // hashLive == hashStored
    uintptr_t sourceVt = 0, componentVt = 0, modelVt = 0;   // vtable pointers (0: the object has no vtable by the check)
    bool recordVtable = false;     // rec+0 passes the vtable check (the sealed trial's question)
};

// What a chain read cached per source node and per model, so one frame reads each once.
struct Chain {
    Status status = Status::kSourceUnreadable;
    uint64_t key1 = 0, key2 = 0;
    uintptr_t component = 0, model = 0;
    uint32_t modelCount = 0;
    uint64_t modelFirst = 0;
    uint32_t hashLive = 0;
    uintptr_t sourceVt = 0, componentVt = 0, modelVt = 0;
};
struct ModelFold {
    Status status = Status::kModelUnreadable;
    uint32_t count = 0;
    uint64_t first = 0;
    uint32_t fold = 0;
};
struct ChainCache {
    std::unordered_map<uintptr_t, Chain> bySource;
    std::unordered_map<uintptr_t, ModelFold> byModel;
    void clear() {
        bySource.clear();
        byModel.clear();
    }
};

// M's count, first qword and fold. A list that does not read refuses the model.
inline ModelFold readModel(const Reader& r, uintptr_t model, ChainCache* cache) {
    if (cache) {
        const auto it = cache->byModel.find(model);
        if (it != cache->byModel.end()) return it->second;
    }
    ModelFold f;
    uint32_t word = 0;
    if (!r.u32(model + kModelCount, &word)) {
        f.status = Status::kModelUnreadable;
    } else {
        f.status = Status::kOk;
        f.count = (word >> 17) & kModelListMax;
        if (f.count > 0) {
            std::vector<uint64_t> list(f.count);
            if (!r.bytes(model + kModelList, list.data(), static_cast<size_t>(f.count) * sizeof(uint64_t))) {
                f.status = Status::kListUnreadable;
                f.count = 0;
            } else {
                f.first = list[0];
                f.fold = listFold(list.data(), list.size());
            }
        }
    }
    if (cache) cache->byModel.emplace(model, f);
    return f;
}

// The live chain from a source node. Each hop is validated; the first that fails names the refusal.
inline Chain readChain(const Reader& r, const Image& img, uintptr_t source, ChainCache* cache) {
    if (cache) {
        const auto it = cache->bySource.find(source);
        if (it != cache->bySource.end()) return it->second;
    }
    Chain c;
    uint8_t block[kSourceBlockBytes];
    uint64_t probe = 0;
    if (!r.u64(source, &probe)) {
        c.status = Status::kSourceUnreadable;
    } else if (!r.bytes(source, block, sizeof(block))) {
        c.status = Status::kKeyUnreadable;
    } else {
        c.key1 = le64(block + 0xF0);
        c.key2 = le64(block + 0x1A0);
        const uintptr_t x = static_cast<uintptr_t>(le64(block + 0x160));
        uint64_t m = 0;
        if (!isUserAddress(x)) {
            c.status = Status::kComponentUnreadable;
        } else if (!r.u64(x + kComponentModel, &m) || !isUserAddress(static_cast<uintptr_t>(m))) {
            c.component = x;
            c.status = Status::kModelUnreadable;
        } else {
            c.component = x;
            c.model = static_cast<uintptr_t>(m);
            const ModelFold f = readModel(r, c.model, cache);
            c.modelCount = f.count;
            c.modelFirst = f.first;
            c.status = f.status;
            if (f.status == Status::kOk) c.hashLive = identityHash(c.key1, c.key2) ^ f.fold;
        }
        uintptr_t vt = 0;
        if (vtableOf(r, img, source, &vt)) c.sourceVt = vt;
        if (c.component && vtableOf(r, img, c.component, &vt)) c.componentVt = vt;
        if (c.model && vtableOf(r, img, c.model, &vt)) c.modelVt = vt;
    }
    if (cache) cache->bySource.emplace(source, c);
    return c;
}

// One record: its own fields from one read of its 0x2F0 bytes, and its live chain. A record whose chain refuses keeps its own
// fields and an error status, and its key, model and hash stay zero.
inline Observation observeRecord(const Reader& r, const Image& img, uintptr_t bucket, uintptr_t record, ChainCache* cache) {
    Observation o;
    o.bucket = bucket;
    o.record = record;
    uint8_t rec[kRecordBytes];
    if (!r.bytes(record, rec, sizeof(rec))) {
        o.status = Status::kRecordUnreadable;
        return o;
    }
    uint64_t bucketBase = 0;
    if (r.u64(bucket + kBucketBase, &bucketBase)) o.bucketBase = static_cast<uintptr_t>(bucketBase);
    o.mask = le64(rec + 0x208);
    o.admitted = rec[0x234];
    std::memcpy(o.centre, rec + 0x240, sizeof(o.centre));
    std::memcpy(&o.radius, rec + 0x280, sizeof(o.radius));
    o.children = le64(rec + 0x2B0);
    o.node = static_cast<uintptr_t>(le64(rec + 0x18));
    const uint64_t storedKey1 = le64(rec + 0x250), storedModel = le64(rec + 0x258), storedKey2 = le64(rec + 0x260);
    o.hashStored = le32(rec + 0x268);
    o.source = static_cast<uintptr_t>(le64(rec + 0x2B8));
    o.recordVtable = vtableOf(r, img, record, nullptr);
    if (o.source == 0) {
        o.status = Status::kNoSource;
        return o;
    }
    const Chain c = readChain(r, img, o.source, cache);
    o.status = c.status;
    o.sourceVt = c.sourceVt;
    o.componentVt = c.componentVt;
    o.modelVt = c.modelVt;
    o.component = c.component;
    o.model = c.model;
    if (c.status != Status::kOk) return o;
    o.key1 = c.key1;
    o.key2 = c.key2;
    o.modelCount = c.modelCount;
    o.modelFirst = c.modelFirst;
    o.hashLive = c.hashLive;
    o.blockAgrees = storedKey1 == c.key1 && storedModel == static_cast<uint64_t>(c.model) && storedKey2 == c.key2;
    o.hashAgrees = c.hashLive == o.hashStored;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// Grouping: one group per (status, key1, key2, live hash). Refused and no-source records group by status alone.
struct Group {
    Status status = Status::kNoSource;
    uint64_t key1 = 0, key2 = 0;
    uint32_t hashLive = 0;
    std::vector<size_t> members;   // indexes into the observations
};

inline std::vector<Group> groupObservations(const std::vector<Observation>& obs) {
    std::vector<size_t> order(obs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto keyLess = [&obs](size_t a, size_t b) {
        const Observation& x = obs[a];
        const Observation& y = obs[b];
        if (x.status != y.status) return static_cast<uint8_t>(x.status) < static_cast<uint8_t>(y.status);
        if (x.key1 != y.key1) return x.key1 < y.key1;
        if (x.key2 != y.key2) return x.key2 < y.key2;
        return x.hashLive < y.hashLive;
    };
    std::stable_sort(order.begin(), order.end(), keyLess);
    std::vector<Group> groups;
    for (size_t k = 0; k < order.size(); ++k) {
        const Observation& o = obs[order[k]];
        const bool same = !groups.empty() && groups.back().status == o.status && groups.back().key1 == o.key1 &&
                          groups.back().key2 == o.key2 && groups.back().hashLive == o.hashLive;
        if (!same) {
            Group g;
            g.status = o.status;
            g.key1 = o.key1;
            g.key2 = o.key2;
            g.hashLive = o.hashLive;
            groups.push_back(g);
        }
        groups.back().members.push_back(order[k]);
    }
    // Largest first, then the group's key, so the table reads the same from one flight to the next.
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.members.size() != b.members.size()) return a.members.size() > b.members.size();
        if (a.status != b.status) return static_cast<uint8_t>(a.status) < static_cast<uint8_t>(b.status);
        if (a.key1 != b.key1) return a.key1 < b.key1;
        return a.key2 < b.key2;
    });
    return groups;
}

struct Stats {
    size_t observations = 0, records = 0, buckets = 0, models = 0;
    uint64_t children = 0;
    bool radiusKnown = false;
    float radiusMin = 0.0f, radiusMax = 0.0f;
    bool distKnown = false;
    float distMin = 0.0f;
    long long contains = -1;        // -1 when the camera is unknown
    long long mainAdmitted = -1;    // -1 when the main view's bit is unknown
    size_t maskZero = 0, admittedFlag = 0, blockAgree = 0, hashAgree = 0, recordVtable = 0;
    bool key1Pointer = false;
};

// The group's numbers. camera is null when the camera is unknown; mainBit is the main view's bit in the mask (0 when unknown).
inline Stats computeStats(const std::vector<Observation>& obs, const Group& g, const float* camera, uint64_t mainBit) {
    Stats s;
    s.observations = g.members.size();
    std::vector<uintptr_t> records, buckets, models;
    records.reserve(g.members.size());
    buckets.reserve(g.members.size());
    if (camera) s.contains = 0;
    if (mainBit) s.mainAdmitted = 0;
    for (size_t idx : g.members) {
        const Observation& o = obs[idx];
        records.push_back(o.record);
        buckets.push_back(o.bucket);
        if (o.model) models.push_back(o.model);
        s.children += o.children;
        if (std::isfinite(o.radius) && o.radius > 0.0f) {
            if (!s.radiusKnown) {
                s.radiusKnown = true;
                s.radiusMin = s.radiusMax = o.radius;
            } else {
                s.radiusMin = std::min(s.radiusMin, o.radius);
                s.radiusMax = std::max(s.radiusMax, o.radius);
            }
            if (camera && std::isfinite(o.centre[0]) && std::isfinite(o.centre[1]) && std::isfinite(o.centre[2])) {
                const float d = sealed::distance(o.centre, camera);
                if (!s.distKnown || d < s.distMin) s.distMin = d;
                s.distKnown = true;
                if (sealed::containsCamera(o.centre, o.radius, camera)) ++s.contains;
            }
        }
        if (mainBit && (o.mask & mainBit) != 0) ++s.mainAdmitted;
        if (o.mask == 0) ++s.maskZero;
        if (o.admitted) ++s.admittedFlag;
        if (o.blockAgrees) ++s.blockAgree;
        if (o.hashAgrees) ++s.hashAgree;
        if (o.recordVtable) ++s.recordVtable;
        if (isUserAddress(static_cast<uintptr_t>(o.key1))) s.key1Pointer = true;
    }
    std::sort(records.begin(), records.end());
    s.records = static_cast<size_t>(std::unique(records.begin(), records.end()) - records.begin());
    std::sort(buckets.begin(), buckets.end());
    s.buckets = static_cast<size_t>(std::unique(buckets.begin(), buckets.end()) - buckets.begin());
    std::sort(models.begin(), models.end());
    s.models = static_cast<size_t>(std::unique(models.begin(), models.end()) - models.begin());
    if (!s.distKnown) s.distMin = -1.0f;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// Names: text reached through a pointer in an object's first 0x100 bytes. Accepted: 4 to 48 characters from the allowed set,
// as ASCII or as UTF-16LE with ASCII code units, NUL-terminated within kNameBytes. Anything else is not a name.
inline bool nameChar(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' ||
           c == ' ' || c == ':' || c == '(' || c == ')' || c == '#' || c == '+' || c == '-';
}

// Writes the text found in bytes (ascii or utf16) to out. False when the bytes are not a name.
inline bool textName(const uint8_t* bytes, size_t n, bool wide, std::string* out) {
    std::string text;
    const size_t step = wide ? 2 : 1;
    for (size_t i = 0; i + step <= n; i += step) {
        const uint8_t c = bytes[i];
        if (wide && bytes[i + 1] != 0) return false;
        if (c == 0) {
            if (text.size() < 4 || text.size() > kNameMax) return false;
            *out = text;
            return true;
        }
        if (!nameChar(c) || text.size() >= kNameMax) return false;
        text.push_back(static_cast<char>(c));
    }
    return false;
}

// Scans an object's first bytes for pointers to text. Appends "label+0xOFF=text" hits, at most maxHits in total across calls.
inline size_t scanNames(const Reader& r, uintptr_t object, const char* label, size_t maxHits, std::vector<std::string>* out) {
    if (!object || maxHits == 0) return 0;
    uint8_t head[0x100];
    if (!r.bytes(object, head, sizeof(head))) return 0;
    size_t hits = 0;
    for (size_t off = 0; off + 8 <= sizeof(head) && hits < maxHits; off += 8) {
        const uintptr_t p = static_cast<uintptr_t>(le64(head + off));
        if (!isUserAddress(p)) continue;
        uint8_t text[kNameBytes];
        if (!r.bytes(p, text, sizeof(text))) continue;
        std::string name;
        if (!textName(text, sizeof(text), false, &name) && !textName(text, sizeof(text), true, &name)) continue;
        char hit[96];
        std::snprintf(hit, sizeof(hit), "%s+0x%zX=%s", label, off, name.c_str());
        out->push_back(hit);
        ++hits;
    }
    return hits;
}

// ---------------------------------------------------------------------------------------------------------------------
// The CSV: one row per group, after a "#" summary line and a "#" line for the hatch state. Numbers the observation could not
// measure are -1; addresses and keys are 0x-prefixed hex; names are quoted.
inline const char* csvHeader() noexcept {
    return "group,status,hash_live,hash_stored,key1,key2,observations,records,buckets,children,radius_min_m,radius_max_m,"
           "dist_min_m,contains_camera,main_admitted,mask_zero,admitted_flag,block_agree,hash_agree,key1_is_pointer,models,"
           "model_count,model_first,model_vt_rva,component_vt_rva,source_vt_rva,record_vtable,names,sample_record,"
           "sample_bucket_base";
}

// The group's row. names is the joined name hits (may be empty). first is the group's first member, for the sample columns.
inline int formatRow(char* out, size_t cap, size_t index, const Group& g, const Stats& s, const std::string& names,
                     const Observation& first, const Image& img) {
    const float distOut = s.distKnown ? s.distMin : -1.0f;
    const float rmin = s.radiusKnown ? s.radiusMin : -1.0f;
    const float rmax = s.radiusKnown ? s.radiusMax : -1.0f;
    return std::snprintf(out, cap,
                         "%zu,%s,0x%08X,0x%08X,0x%016llX,0x%016llX,%zu,%zu,%zu,%llu,%.3f,%.3f,%.3f,%lld,%lld,%zu,%zu,%zu,%zu,%d,%zu,"
                         "%u,0x%016llX,0x%llX,0x%llX,0x%llX,%zu,\"%s\",0x%llX,0x%llX",
                         index, statusName(g.status), g.hashLive, first.hashStored,
                         static_cast<unsigned long long>(g.key1), static_cast<unsigned long long>(g.key2), s.observations,
                         s.records, s.buckets, static_cast<unsigned long long>(s.children), static_cast<double>(rmin),
                         static_cast<double>(rmax), static_cast<double>(distOut), s.contains, s.mainAdmitted, s.maskZero,
                         s.admittedFlag, s.blockAgree, s.hashAgree, s.key1Pointer ? 1 : 0, s.models,
                         first.modelCount, static_cast<unsigned long long>(first.modelFirst),
                         static_cast<unsigned long long>(rvaOf(first.modelVt, img)),
                         static_cast<unsigned long long>(rvaOf(first.componentVt, img)),
                         static_cast<unsigned long long>(rvaOf(first.sourceVt, img)), s.recordVtable, names.c_str(),
                         static_cast<unsigned long long>(first.record), static_cast<unsigned long long>(first.bucketBase));
}

}  // namespace edvr::census
