#pragma once
// The kept-records report's pure half (sealed_trial.cpp; TEMPORARY, the diagnosis behind hotkey.sealed_trial_report). No Windows, no
// engine memory: why a record is kept, the two lists the report writes (the top kept records by radius, and the top kept base records
// by child count), the counts, and the CSV rows (one header; every row has the header's 18 fields, the last being the record's name). The rig tools\view_source_test
// compiles this header alone.
//
// The rule the report explains: the store keeps a record when its own sphere contains the camera or meets the cell (the sphere of radius R
// round the camera). A child's view mask also inherits its base's verdict (the store recurses over rec+0x2A8), so a child can be kept while
// its own sphere is far away. The report says which of the three applies to each kept record.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr::kept {

constexpr uint32_t kTop = 40;     // records listed per list
constexpr int kColumns = 24;      // fields in every CSV row, the header included (the name column in Built v8; the six centre columns in Built v8.4)

// own: the record's own sphere contains the camera or meets the cell. base: an ancestor's sphere does (levelsUp generations up).
// unjudged: no sphere on the way up meets the cell (the record was kept without a sphere that qualifies, or its sphere did not read).
enum class Why : uint8_t { kOwn = 0, kBase = 1, kUnjudged = 2 };

inline const char* whyName(Why w) noexcept {
    switch (w) {
        case Why::kOwn: return "own";
        case Why::kBase: return "base";
        default: return "unjudged";
    }
}

// The reason for a record: its own test first; else the nearest ancestor whose test meets the cell (levelsUp > 0); else unjudged.
inline Why classify(bool ownMeets, uint32_t levelsUp) noexcept {
    if (ownMeets) return Why::kOwn;
    if (levelsUp > 0) return Why::kBase;
    return Why::kUnjudged;
}

struct Entry {
    uintptr_t address = 0;
    uintptr_t parent = 0;          // the record whose child list holds this one (0 for a root the walk started from)
    uintptr_t base = 0;            // the bucket's base record (bucket + 0x28)
    uintptr_t bucket = 0;
    float radius = 0.0f;
    float rel[3] = {};             // centre minus camera, metres
    float dist = 0.0f;             // |rel|
    bool contains = false;         // dist <= radius: the camera is inside the record's sphere
    bool kept = false;             // the store's view mask is non-zero after the frame
    bool judged = false;           // the admission observer saw this record in the frame
    Why why = Why::kUnjudged;
    uint32_t levelsUp = 0;         // generations up to the ancestor whose test met the cell (0 when none did, or own did)
    uint64_t childCount = 0;
    uint64_t vtableRva = 0;
    bool readable = false;         // the centre, radius, mask and children were read (Built v8.3b)
    const char* label = nullptr;   // named mode: the reason against the cell (cell, explicit, other_named, sphere_no_cell, unjudged_unreadable); null: why
    std::string name;              // the record's name (rec+0x18 -> node, node+0 -> text); empty when unreadable (written as "(none)")
    // Built v8.4: the pose-derived centre (the builder's; sealed_trial_core.h builderCentre) beside the raw rec+0x240 centre. rel and dist above
    // are measured from the centre the rule used (the world centre: the pose when readable, else rec+0x240).
    float poseCentre[3] = {};
    bool poseOk = false;           // the pose centre was readable
    bool rawZero = false;          // rec+0x240 was exactly the origin
    uint8_t centreSource = 2;      // 0 pose, 1 raw (fallback), 2 none (sealed::CentreSource)
    float centreDist = -1.0f;      // |rec+0x240 - pose centre| when both are readable and rec+0x240 is not zero; else -1
};

// Radius first (largest), then address (smallest): equal radii list in a fixed order.
inline bool byRadius(const Entry& a, const Entry& b) noexcept {
    if (a.radius != b.radius) return a.radius > b.radius;
    return a.address < b.address;
}

// Child count first (most), then radius (largest), then address (smallest).
inline bool byChildren(const Entry& a, const Entry& b) noexcept {
    if (a.childCount != b.childCount) return a.childCount > b.childCount;
    if (a.radius != b.radius) return a.radius > b.radius;
    return a.address < b.address;
}

// The best k kept entries (bases: only those with children) into out[], best first; returns how many. Bounded insertion: no allocation,
// O(n * k) at worst. out must hold k entries.
template <typename Before>
inline uint32_t topWhere(const Entry* e, uint32_t n, bool bases, uint32_t k, uint32_t* out, Before before) noexcept {
    if (k == 0) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const Entry& c = e[i];
        if (!c.kept || (bases && c.childCount == 0)) continue;
        if (count == k && !before(c, e[out[k - 1]])) continue;   // not better than the worst one kept so far
        uint32_t pos = count < k ? count : k - 1;
        while (pos > 0 && before(c, e[out[pos - 1]])) {
            out[pos] = out[pos - 1];
            --pos;
        }
        out[pos] = i;
        if (count < k) ++count;
    }
    return count;
}

inline uint32_t topByRadius(const Entry* e, uint32_t n, uint32_t* out, uint32_t k = kTop) noexcept {
    return topWhere(e, n, false, k, out, byRadius);
}

inline uint32_t topBases(const Entry* e, uint32_t n, uint32_t* out, uint32_t k = kTop) noexcept {
    return topWhere(e, n, true, k, out, byChildren);
}

struct Stats {
    uint32_t nodes = 0;               // records in the report (observed plus walked)
    uint32_t kept = 0;
    uint32_t judgedKept = 0;          // kept and seen by the observer
    uint32_t keptOwn = 0, keptBase = 0, keptUnjudged = 0;
    uint32_t children = 0;            // records reached through a child list
    uint32_t keptChildren = 0;
    uint32_t keptChildrenByBase = 0;  // kept children whose own sphere does not qualify but an ancestor's does
};

inline Stats countKept(const Entry* e, uint32_t n) noexcept {
    Stats s;
    s.nodes = n;
    for (uint32_t i = 0; i < n; ++i) {
        const Entry& c = e[i];
        if (c.parent) ++s.children;
        if (!c.kept) continue;
        ++s.kept;
        if (c.judged) ++s.judgedKept;
        switch (c.why) {
            case Why::kOwn: ++s.keptOwn; break;
            case Why::kBase: ++s.keptBase; break;
            default: ++s.keptUnjudged; break;
        }
        if (c.parent) {
            ++s.keptChildren;
            if (c.why == Why::kBase) ++s.keptChildrenByBase;
        }
    }
    return s;
}

inline const char* csvHeader() noexcept {
    return "list,rank,address,radius_m,dx_m,dy_m,dz_m,dist_m,contains_camera,why_kept,levels_up,judged,parent_address,"
           "base_address,child_count,bucket,vtable_rva,name,pose_centre_x,pose_centre_y,pose_centre_z,centre_source,raw_centre_zero,"
           "centre_dist_m";
}

// One row: list is "radius" or "base"; rank counts from 1. The name is the 18th field and the centre columns (Built v8.4) the last six; the
// census's name rules keep names free of commas.
inline int formatRow(char* out, size_t cap, const char* list, uint32_t rank, const Entry& e) noexcept {
    static const char* const kSources[] = {"pose", "raw_fallback", "none"};
    return std::snprintf(out, cap,
                         "%s,%u,0x%llX,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%s,%u,%d,0x%llX,0x%llX,%llu,0x%llX,0x%llX,%s,%.2f,%.2f,%.2f,%s,%d,%.2f",
                         list, rank, static_cast<unsigned long long>(e.address), static_cast<double>(e.radius),
                         static_cast<double>(e.rel[0]), static_cast<double>(e.rel[1]), static_cast<double>(e.rel[2]),
                         static_cast<double>(e.dist), e.contains ? 1 : 0, e.label ? e.label : whyName(e.why), e.levelsUp, e.judged ? 1 : 0,
                         static_cast<unsigned long long>(e.parent), static_cast<unsigned long long>(e.base),
                         static_cast<unsigned long long>(e.childCount), static_cast<unsigned long long>(e.bucket),
                         static_cast<unsigned long long>(e.vtableRva), e.name.empty() ? "(none)" : e.name.c_str(),
                         static_cast<double>(e.poseCentre[0]), static_cast<double>(e.poseCentre[1]), static_cast<double>(e.poseCentre[2]),
                         kSources[e.centreSource > 2 ? 2 : e.centreSource], e.rawZero ? 1 : 0, static_cast<double>(e.centreDist));
}

}  // namespace edvr::kept
