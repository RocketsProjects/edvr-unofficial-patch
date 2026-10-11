#pragma once
// The look probe's pure half (look_probe.cpp; TEMPORARY, the aiming measurement of docs/concourse-culling-2026-10-11.md): the
// ray test against a bounding sphere, the forward direction from the main view's frustum, the CSV rows (one header; every row
// has the header's 24 fields), and the summary line. No Windows, no engine memory. The rig tools\look_probe_test compiles this
// header alone.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr::look {

constexpr float kRangeM = 2000.0f;        // the ray is followed this far
constexpr uint32_t kMaxRecords = 64;      // record rows listed, nearest first
constexpr uint32_t kMaxDraws = 64;        // draw rows listed: the last covering draws, in issue order
constexpr uint32_t kProbeDraws = 4096;    // draws given the centre test in the frame (one duplicate each)
constexpr uint32_t kMaxPlanes = 32;
constexpr int kColumns = 24;              // fields in every CSV row, the header included

struct Plane {
    float n[3];   // the normal as the engine stores it; its side is not assumed here
    float d;      // n . x + d is zero on the plane
};

inline float signedDistance(const Plane& p, const float x[3]) noexcept {
    return p.n[0] * x[0] + p.n[1] * x[1] + p.n[2] * x[2] + p.d;
}

// Whether the ray from o along f (unit) meets the sphere (c, r) within maxT. tEntry is the distance to the first contact
// (zero when the origin is inside the sphere).
inline bool rayHitsSphere(const float o[3], const float f[3], const float c[3], float r, float maxT, float* tEntry) noexcept {
    const float oc[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const float t = oc[0] * f[0] + oc[1] * f[1] + oc[2] * f[2];
    const float d2 = (oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2]) - t * t;
    const float r2 = r * r;
    if (!(d2 <= r2)) return false;            // the ray's line misses the sphere
    const float half = std::sqrt(r2 - d2);
    if (t + half < 0.0f) return false;        // the sphere is behind the origin
    const float entry = t - half > 0.0f ? t - half : 0.0f;
    if (entry > maxT) return false;
    if (tEntry) *tEntry = entry;
    return true;
}

// The forward direction of a perspective view, from its frustum. The near and far planes are an opposite pair (antiparallel
// normals) with the eye between them: the eye lies on the outer side of one and the inner side of the other, so their signed
// distances have opposite signs. The far plane is the one the eye is farther from, and the displacement from the eye onto that
// plane is the forward direction: forward = -sign(s) * n / |n|, where s is the eye's signed distance to the far plane. That
// holds whichever side the engine's normals point to, so no convention is assumed. nearM and farM (optional) receive the eye's
// distances to the pair. False when no such pair is present.
inline bool forwardFromPlanes(const Plane* planes, uint32_t n, const float eye[3], float out[3], float* nearM = nullptr,
                              float* farM = nullptr) noexcept {
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t j = i + 1; j < n; ++j) {
            const Plane& a = planes[i];
            const Plane& b = planes[j];
            const float dot = a.n[0] * b.n[0] + a.n[1] * b.n[1] + a.n[2] * b.n[2];
            if (!(dot < -0.999f)) continue;
            const float sa = signedDistance(a, eye);
            const float sb = signedDistance(b, eye);
            if (!(sa * sb < 0.0f)) continue;   // the eye must be between the two planes, not on one side of both
            const bool aFar = std::fabs(sa) >= std::fabs(sb);
            const Plane& farPl = aFar ? a : b;   // not "far": windows.h defines it
            const float s = aFar ? sa : sb;
            const float len = std::sqrt(farPl.n[0] * farPl.n[0] + farPl.n[1] * farPl.n[1] + farPl.n[2] * farPl.n[2]);
            if (!(len > 0.0f) || !(std::fabs(s) > 0.0f)) continue;
            const float k = (s > 0.0f ? -1.0f : 1.0f) / len;
            out[0] = farPl.n[0] * k;
            out[1] = farPl.n[1] * k;
            out[2] = farPl.n[2] * k;
            if (nearM) *nearM = std::fabs(aFar ? sb : sa);
            if (farM) *farM = std::fabs(s);
            return true;
        }
    }
    return false;
}

// The CSV: one header for both kinds of row. A record row leaves the draw columns empty and a draw row leaves the record
// columns empty. index is the record's rank (nearest first) for a record and the draw's issue order for a draw.
inline const char* csvHeader() noexcept {
    return "kind,index,address,radius_m,t_m,dist_m,bucket,bucket_base,role,child_count,vtable_rva,node_vtable_rva,"
           "vs,ps,draw_kind,count,instances,blend,depth,rt,rt_w,rt_h,centre_samples,covers_centre";
}

struct RecordRow {
    uint32_t index = 0;
    uintptr_t address = 0, bucket = 0, bucketBase = 0;
    float radius = 0, t = 0, dist = 0;
    const char* role = "";            // base (the bucket's base record) | child
    uint32_t childCount = 0;
    uint64_t vtableRva = 0, nodeVtableRva = 0;
};

struct DrawRow {
    uint32_t index = 0;               // the draw's issue order in the frame
    uint64_t vs = 0, ps = 0;
    char drawKind = '?';              // D, I, N or X (the kinds the centre test replays); Y, Z, A are listed untested
    uint32_t count = 0, instances = 1;
    const char* blend = "";           // no commas in the text (the CSV has no quoting)
    const char* depth = "";
    uint64_t rt = 0;                  // the render target's address (for the log only)
    uint32_t rtW = 0, rtH = 0;
    uint64_t centreSamples = 0;
    bool covers = false;              // the draw has visible samples at the viewport's centre pixel
};

// Appends commas until the row holds `columns` fields (an empty field is two commas with nothing between them). The text is
// kept terminated. The caller has checked that n is inside cap.
inline int padFields(char* out, size_t cap, int n, int columns) noexcept {
    int fields = 1;
    for (int i = 0; i < n; ++i)
        if (out[i] == ',') ++fields;
    while (fields < columns && static_cast<size_t>(n) + 1 < cap) {
        out[n++] = ',';
        ++fields;
    }
    out[n] = '\0';
    return n;
}

inline int formatRecordRow(char* out, size_t cap, const RecordRow& r) noexcept {
    const int n = std::snprintf(out, cap, "record,%u,0x%llX,%.2f,%.2f,%.2f,0x%llX,0x%llX,%s,%u,0x%llX,0x%llX", r.index,
                                static_cast<unsigned long long>(r.address), static_cast<double>(r.radius),
                                static_cast<double>(r.t), static_cast<double>(r.dist),
                                static_cast<unsigned long long>(r.bucket), static_cast<unsigned long long>(r.bucketBase),
                                r.role, r.childCount, static_cast<unsigned long long>(r.vtableRva),
                                static_cast<unsigned long long>(r.nodeVtableRva));
    if (n < 0 || static_cast<size_t>(n) >= cap) return n;
    return padFields(out, cap, n, kColumns);
}

inline int formatDrawRow(char* out, size_t cap, const DrawRow& r) noexcept {
    int n = std::snprintf(out, cap, "draw,%u", r.index);
    if (n < 0 || static_cast<size_t>(n) >= cap) return n;
    n = padFields(out, cap, n, 12);   // fields 3 to 12 are the record columns, left empty
    const int m = std::snprintf(out + n, cap - static_cast<size_t>(n),
                                ",0x%016llX,0x%016llX,%c,%u,%u,%s,%s,0x%llX,%u,%u,%llu,%d",
                                static_cast<unsigned long long>(r.vs), static_cast<unsigned long long>(r.ps), r.drawKind,
                                r.count, r.instances, r.blend, r.depth, static_cast<unsigned long long>(r.rt), r.rtW, r.rtH,
                                static_cast<unsigned long long>(r.centreSamples), r.covers ? 1 : 0);
    if (m < 0 || static_cast<size_t>(n) + static_cast<size_t>(m) >= cap) return -1;
    return n + m;
}

// The summary line: what was aimed at, how many records and draws were found, and where the CSV went.
struct Summary {
    uint32_t frame = 0;
    const char* trigger = "";
    bool forwardKnown = false;
    float forward[3] = {};
    float eye[3] = {};
    uint32_t recordsScanned = 0, recordsInRay = 0, recordsListed = 0;
    uint32_t draws = 0, drawsProbed = 0, drawsCovering = 0, drawsListed = 0;
    const char* viewSource = "";   // explorer cam | eye rows | vscreen | origin | frustum (view_source_core.h; the view the aim came from)
    const char* csvName = "";
};

inline int formatSummary(char* out, size_t cap, const Summary& s) noexcept {
    if (!s.forwardKnown)
        return std::snprintf(out, cap, "look probe: frame=%u trigger=%s view source = %s forward=unknown (no pose from the "
                                       "rows and no frustum pair; records not aimed) draws=%u probed=%u covering=%u csv=%s",
                             s.frame, s.trigger, s.viewSource, s.draws, s.drawsProbed, s.drawsCovering, s.csvName);
    return std::snprintf(out, cap,
                         "look probe: frame=%u trigger=%s view source = %s eye=(%.2f,%.2f,%.2f) forward=(%.4f,%.4f,%.4f) records=%u scanned, "
                         "%u on the ray within %.0f m, %u listed; draws=%u probed=%u covering the centre=%u listed=%u csv=%s",
                         s.frame, s.trigger, s.viewSource, static_cast<double>(s.eye[0]), static_cast<double>(s.eye[1]),
                         static_cast<double>(s.eye[2]), static_cast<double>(s.forward[0]), static_cast<double>(s.forward[1]),
                         static_cast<double>(s.forward[2]), s.recordsScanned, s.recordsInRay,
                         static_cast<double>(kRangeM), s.recordsListed, s.draws, s.drawsProbed, s.drawsCovering,
                         s.drawsListed, s.csvName);
}

}  // namespace edvr::look
