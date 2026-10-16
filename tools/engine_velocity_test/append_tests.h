#pragma once
// engine_velocity_test: the APPEND producer's emission (FUN_14434D120; engine_velocity_emit.h "THE APPEND PRODUCER").
//
// The shield strike's pool records (5C1D slots 69/70, 820E slots 109/110 in eye run 100257) are built by FUN_14434D470 on the render thread
// and appended through FUN_14434D120(bucket, model key, &record, node key). Nothing hooked them, so they carried the game's own
// previous pose (= the current one) and a stack-leftover marker: never joined (docs/dlss-blur-elements-2026-10-14.md, E1). This file drives
// observeAppend against a fake bucket laid out as the engine lays it (emit_tests.h's Fake: owner = bucket, dictionary entry, 8-record
// nodes), with the PRODUCTION sink (primaryCopy::recordEmission), and reads the record the private pool would hold.
//
// What it pins:
//   A1 the append emission joins a record by its own identity (bucket + model key + node key + LOD): masked on first sight, joined the
//      next frame with exactly the previous frame's pose, through the real claim -> copier -> patch route, and the joined record is
//      read as kind 1 by the production mv consumer (consumer_tests::run is driven with it from the caller).
//   A2 a POSE MATCH NEVER JOINS: a record with the same pose as a joined one but another node key, model key or LOD is a new identity
//      and starts masked; the same identity at another pose joins.
//   A3 the unit-scale filter: a scaled (cockpit/HUD) record costs one read, touches no table and makes no claim.
//   A4 the single-writer guards: a bucket the rigid producers also write, and a bucket written from a second thread, are declined and
//      counted; a count that is not exactly one is unproven and taints the next frame.
//   A5 a tail item that is not the caller's record (a racing append) is a disagreement, never joined.
//   A6 FAIL-BEFORE: with no observer (the code before this change) the same sequence leaves the record's previous pose = its current
//      pose and a marker that is neither tag: kind 3, camera term only.
//   A7 a record the emission did not claim is untouched, and the log line has every form.
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "emit_tests.h"
#include "primary_copy_tests.h"

namespace append_tests {
namespace ev = edvr::engine_velocity_emit;
namespace copy = edvr::engine_velocity_primary_copy;
using emit_tests::Fake;
using Rec = std::array<uint32_t, 84>;
using Pool = std::array<uint8_t, 0x150>;

// The one joined, moving record A1 produces; the caller feeds it to the production mv consumer (frame kJoinedFrame).
inline Rec g_joinedAppend{};
constexpr uint32_t kJoinedFrame = 101;

inline Fake* g_bucket = nullptr;
inline uintptr_t g_modelKey = 0;
inline uintptr_t __fastcall lookup(uintptr_t dictionary, uintptr_t key) {
    if (!g_bucket || dictionary != g_bucket->own() + ev::kOwnerDictionary || key != g_modelKey) return 0;
    return g_bucket->ent();
}
// The production sink, as engine_velocity.cpp's observeAppendEmit builds it.
inline bool realSink(uintptr_t item, const ev::Pose&, const ev::Pose& previous, uint32_t marker, uint32_t frame) noexcept {
    uint32_t native[84]{};
    return ev::read(item, native, sizeof(native)) && copy::recordEmission(item, native, previous, marker, frame);
}
inline void realInvalidate(uintptr_t item) noexcept { copy::invalidateEmission(item); }

// FUN_14434D470's record, as it builds it on its stack: zero base, the source's scale at +0x04 and +0x134, the flags word (LOD bits 18..23),
// the packed quaternion at +0x08, the position at +0x10, the SECOND BLOCK = the first (previous position/quaternion = current), and
// whatever the stack held at +0x120 (never written).
inline Rec record(const ev::Pose& pose, uint32_t flags, float scale = 1.0f) {
    Rec r{};
    uint32_t s = 0;
    std::memcpy(&s, &scale, 4);
    r[0] = 0;
    r[1] = s;
    r[7] = flags;
    r[2] = pose.w[3]; r[3] = pose.w[4];
    r[4] = pose.w[0]; r[5] = pose.w[1]; r[6] = pose.w[2];
    r[72] = 0x447BF39Cu;   // the stack leftover the probe found shared by the shield, E508 and 1989 records
    r[73] = pose.w[0]; r[74] = pose.w[1]; r[75] = pose.w[2];
    r[77] = s;
    r[78] = pose.w[3]; r[79] = pose.w[4];
    return r;
}
inline ev::Pose poseOf(const Rec& r) {
    ev::Pose p;
    p.w[0] = r[4]; p.w[1] = r[5]; p.w[2] = r[6]; p.w[3] = r[2]; p.w[4] = r[3];
    return p;
}
// FUN_14434D120's append of one verbatim 0x150-byte copy to the tail node; the bucket's +0x2A4 counted. Returns the item's address.
inline uintptr_t append(Fake& f, const Rec& r) {
    uintptr_t t = f.tail();
    if (t == f.anchor() || Fake::get<uint64_t>(t + ev::kNodeCount) == ev::kNodeCapacity) {
        f.nodes.push_back(std::make_unique<emit_tests::Node>());
        std::memset(f.nodes.back()->bytes, 0, emit_tests::kNodeBytes);
        const uintptr_t n = reinterpret_cast<uintptr_t>(f.nodes.back()->bytes);
        Fake::put<uint64_t>(n, f.anchor());
        Fake::put<uint64_t>(n + ev::kNodePrev, t);
        if (t != f.anchor()) Fake::put<uint64_t>(t, n); else Fake::put<uint64_t>(f.anchor(), n);
        Fake::put<uint64_t>(f.ent() + ev::kEntryTail, n);
        t = n;
    }
    const uint64_t c = Fake::get<uint64_t>(t + ev::kNodeCount);
    const uintptr_t item = t + ev::kNodeRecords + c * ev::kItemBytes;
    std::memcpy(reinterpret_cast<void*>(item), r.data(), 0x150);
    Fake::put<uint64_t>(t + ev::kNodeCount, c + 1);
    Fake::put<int32_t>(f.own() + ev::kOwnerCount, Fake::get<int32_t>(f.own() + ev::kOwnerCount) + 1);
    return item;
}

struct World {
    std::unique_ptr<Fake> bucket = std::make_unique<Fake>();
    std::unique_ptr<ev::Table> table = std::make_unique<ev::Table>();
    ev::Stats stats;
    ev::AppendStats astats;
    ev::AppendBuckets buckets;
    World() { g_bucket = bucket.get(); }
    ~World() { g_bucket = nullptr; g_modelKey = 0; }
};

struct Call {
    uintptr_t modelKey = 0x2317A8D34F0ull, nodeKey = 0xC05E;
    uint32_t tid = 51656;
    bool rigid = false;
    int32_t extra = 0;        // appended count seen beyond the one append (an interleaved append the bracket must refuse)
    bool observe = true;      // false = the code before this change (no observer)
};
// One 434D120 call: the stack record `r`, the append, the observer. Returns the item address.
inline uintptr_t call(World& w, const Rec& r, const Call& c, uint32_t frame) {
    g_modelKey = c.modelKey;
    const int32_t before = Fake::get<int32_t>(w.bucket->own() + ev::kOwnerCount);
    const uintptr_t item = append(*w.bucket, r);
    const int32_t after = Fake::get<int32_t>(w.bucket->own() + ev::kOwnerCount);
    if (!c.observe) return item;
    ev::AppendIdentity id;
    id.bucket = w.bucket->own(); id.modelKey = c.modelKey; id.nodeKey = c.nodeKey; id.tid = c.tid; id.bucketIsRigid = c.rigid;
    ev::observeAppend(id, reinterpret_cast<uintptr_t>(r.data()), before, after + c.extra, frame, &lookup, *w.table, w.stats, w.astats,
                      w.buckets, &realSink, &realInvalidate);
    return item;
}

// The record the private pool holds for `item` after the copier ran: the patch's marker and previous block laid over the native record
// (what the scatter shader writes), or the native record when no patch exists.
struct Held {
    Rec record{};
    bool patched = false;
};
inline Held held(ID3D11Device* device, uintptr_t item, uint32_t frame, void (*check)(bool, const char*)) {
    static Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    static uint64_t sequence = 1000;
    if (!buffer) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 336;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.StructureByteStride = 336;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        check(SUCCEEDED(device->CreateBuffer(&d, nullptr, &buffer)), "append: the pool fixture buffer");
    }
    Held h;
    Rec mapped{};
    ev::read(item, mapped.data(), 336);
    ++sequence;
    copy::beginMap(buffer.Get(), mapped.data(), 336, 336, D3D11_MAP_WRITE_DISCARD, sequence, frame);
    copy::copier(reinterpret_cast<uintptr_t>(mapped.data()), 336, item, 0, 1, frame);
    copy::endMap(buffer.Get(), sequence);
    h.record = mapped;
    for (const auto& p : copy::patches(buffer.Get(), frame)) {
        h.patched = true;
        h.record[72] = p.marker;
        h.record[73] = p.previous.w[0]; h.record[74] = p.previous.w[1]; h.record[75] = p.previous.w[2];
        h.record[78] = p.previous.w[3]; h.record[79] = p.previous.w[4];
    }
    return h;
}
inline Pool asPool(const Rec& r) { Pool p; std::memcpy(p.data(), r.data(), 0x150); return p; }

inline void run(ID3D11Device* device, void (*check)(bool, const char*)) {
    const ev::Pose p100 = emit_tests::pose(-977.63f, -45.17f, -1287.06f, 32767, 32767, 32767, 65534);
    const ev::Pose p101 = emit_tests::pose(-977.10f, -45.40f, -1286.20f, 32767, 32767, 33000, 65533);
    const ev::Pose p102 = emit_tests::pose(-976.60f, -45.70f, -1285.30f, 32767, 32767, 33233, 65531);
    constexpr uint32_t flags = 0x65F;   // the shield's flags word (probe, eye run 100257), LOD bits 0

    // ---- A1: first sight masked, the next frame joined, through the real claim/copier/patch route -------------------------------
    {
        World w;
        copy::reset();
        Call c;
        const Rec r100 = record(p100, flags);
        const uintptr_t i100 = call(w, r100, c, 100);
        const Held h100 = held(device, i100, 100, check);
        check(h100.patched && emit_tests::kindOf(asPool(h100.record), 100) == 2 && emit_tests::blockPrev(asPool(h100.record)) == p100,
              "append A1: first sight is MASKED (the baseline), never a joined zero motion");
        check(w.astats.masked == 1 && w.astats.joined == 0 && w.stats.firstSeen == 1, "append A1: first-sight counters");
        w.bucket->reset();
        const Rec r101 = record(p101, flags);
        const uintptr_t i101 = call(w, r101, c, 101);
        const Held h101 = held(device, i101, 101, check);
        check(h101.patched && emit_tests::kindOf(asPool(h101.record), 101) == 1 &&
              emit_tests::blockPrev(asPool(h101.record)) == p100 && emit_tests::blockNow(asPool(h101.record)) == p101,
              "append A1: the next frame joins with exactly the previous frame's pose in the previous block (kind 1)");
        check(w.astats.joined == 1 && w.stats.itemsMoving == 1, "append A1: joined and moving counters");
        // The unclaimed bytes are the engine's: the native record in the node is unchanged by the emission.
        Rec native{};
        ev::read(i101, native.data(), 336);
        check(native == r101, "append A1: the emission never writes the native record (the clone carries the marker)");
        g_joinedAppend = h101.record;
        w.bucket->reset();
        // A gap of a frame re-baselines: masked, no phantom delta.
        const uintptr_t i103 = call(w, record(p102, flags), c, 103);
        const Held h103 = held(device, i103, 103, check);
        check(h103.patched && emit_tests::kindOf(asPool(h103.record), 103) == 2 && w.stats.gaps == 1,
              "append A1: a gap re-baselines (masked), never a joined stale delta");
    }

    // ---- A2: a pose match never joins; identity does --------------------------------------------------------------------------------
    {
        World w;
        copy::reset();
        Call c;
        call(w, record(p100, flags), c, 100);
        w.bucket->reset();
        // frame 101: the SAME pose as the first record, three other identities. Each is a first sight, so each is masked.
        Call otherNode = c; otherNode.nodeKey = 0x80000000;
        Call otherModel = c; otherModel.modelKey = 0x2315EB68D60ull;
        const Rec twin = record(p100, flags);
        const uintptr_t a = call(w, twin, otherNode, 101);
        const uintptr_t b = call(w, twin, otherModel, 101);
        const uintptr_t lod = call(w, record(p100, flags | (3u << 18)), c, 101);   // same bucket/model/node as the first: another LOD
        const Held ha = held(device, a, 101, check), hb = held(device, b, 101, check), hl = held(device, lod, 101, check);
        check(ha.patched && emit_tests::kindOf(asPool(ha.record), 101) == 2,
              "append A2: the same pose under another node key is a new identity, MASKED (a pose match never joins)");
        check(hb.patched && emit_tests::kindOf(asPool(hb.record), 101) == 2, "append A2: the same pose under another model key is MASKED");
        check(hl.patched && emit_tests::kindOf(asPool(hl.record), 101) == 2,
              "append A2: the same pose at another LOD of the same key is MASKED");
        check(w.stats.firstSeen == 4 && w.astats.joined == 0, "append A2: four identities seen, none joined by pose");
        // The same identity at a very different pose JOINS with its own previous pose: identity, not pose, decides.
        World v;
        copy::reset();
        const ev::Pose far = emit_tests::pose(500.0f, 20.0f, 40.0f, 32767, 32767, 33233, 65531);
        call(v, record(p100, flags), c, 200);
        v.bucket->reset();
        const uintptr_t k = call(v, record(far, flags), c, 201);
        const Held hk = held(device, k, 201, check);
        check(hk.patched && emit_tests::kindOf(asPool(hk.record), 201) == 1 && emit_tests::blockPrev(asPool(hk.record)) == p100,
              "append A2: the same identity at a different pose JOINS with its own previous pose");
    }

    // ---- A3: the unit-scale filter ------------------------------------------------------------------------------------------------
    {
        World w;
        copy::reset();
        Call c;
        const uint64_t emissionsBefore = copy::stats().emissions;
        const Rec hud = record(p100, flags, 0.65f);   // a cockpit/HUD record at a non-unit scale
        const uintptr_t item = call(w, hud, c, 100);
        const Held h = held(device, item, 100, check);
        check(!h.patched && h.record == hud, "append A3: a record the emission did not claim is untouched in the pool");
        w.bucket->reset();
        call(w, record(p101, flags, 0.65f), c, 101);
        check(w.astats.calls == 2 && w.astats.filteredScale == 2 && w.stats.firstSeen == 0 && w.table->live(101) == 0 &&
              copy::stats().emissions == emissionsBefore,
              "append A3: a non-unit-scale record is dropped after one read: no table entry, no claim, no lookup");
        Rec skinned = record(p100, flags);
        skinned[0] = 7;
        w.bucket->reset();
        call(w, skinned, c, 102);
        check(w.astats.filteredScale == 3, "append A3: a nonzero base (skinned) record is dropped the same way");
    }

    // ---- A4: the single-writer guards, and an unproven count ----------------------------------------------------------------------
    {
        World w;
        copy::reset();
        Call c;
        call(w, record(p100, flags), c, 100);
        w.bucket->reset();
        Call rigid = c; rigid.rigid = true;
        const uintptr_t sh = call(w, record(p101, flags), rigid, 101);
        check(w.astats.sharedBucket == 1 && w.astats.joined == 0 && !held(device, sh, 101, check).patched,
              "append A4: a bucket the rigid producers also write is DECLINED (no history, no claim)");
        w.bucket->reset();
        Call otherThread = c; otherThread.tid = 777;
        const uintptr_t ot = call(w, record(p101, flags), otherThread, 102);
        check(w.astats.threadConflict == 1 && !held(device, ot, 102, check).patched,
              "append A4: a bucket appended from a second thread is DECLINED");
        w.bucket->reset();
        const uintptr_t again = call(w, record(p101, flags), c, 103);
        check(w.astats.threadConflict == 2 && !held(device, again, 103, check).patched,
              "append A4: once a second thread has appended, the bucket stays declined for the first one too");
        w.buckets.clear();
        w.bucket->reset();
        call(w, record(p100, flags), c, 104);
        w.bucket->reset();
        call(w, record(p101, flags), c, 105);
        check(w.astats.joined == 1, "append A4: clearing the guard's memory lets the bucket be observed again");
        // A count that is not exactly one: unproven, and the next frame is masked.
        World u;
        copy::reset();
        call(u, record(p100, flags), c, 100);
        u.bucket->reset();
        Call two = c; two.extra = 1;
        call(u, record(p101, flags), two, 101);
        u.bucket->reset();
        const uintptr_t n = call(u, record(p102, flags), c, 102);
        check(u.astats.unproven == 1 && emit_tests::kindOf(asPool(held(device, n, 102, check).record), 102) == 2,
              "append A4: an unproven count taints the identity: the next frame is masked, not joined on an uncertified pose");
    }

    // ---- A5: the tail item is not the caller's record (a racing append) ------------------------------------------------------------
    {
        World w;
        copy::reset();
        Call c;
        const Rec r = record(p100, flags);
        g_modelKey = c.modelKey;
        const int32_t before = Fake::get<int32_t>(w.bucket->own() + ev::kOwnerCount);
        Rec other = record(p101, flags);
        append(*w.bucket, other);   // another thread's record is the tail when we look
        const int32_t after = Fake::get<int32_t>(w.bucket->own() + ev::kOwnerCount);
        ev::AppendIdentity id;
        id.bucket = w.bucket->own(); id.modelKey = c.modelKey; id.nodeKey = c.nodeKey; id.tid = c.tid;
        ev::observeAppend(id, reinterpret_cast<uintptr_t>(r.data()), before, after, 100, &lookup, *w.table, w.stats, w.astats, w.buckets,
                          &realSink, &realInvalidate);
        check(w.astats.disagreements == 1 && w.astats.masked == 0 && w.astats.joined == 0 && copy::stats().emissions == 0,
              "append A5: a tail item that is not the caller's record is a disagreement: no table move, no claim");
    }

    // ---- A6: FAIL-BEFORE: the same sequence without the observer --------------------------------------------------------------------
    {
        World w;
        copy::reset();
        Call c;
        c.observe = false;
        call(w, record(p100, flags), c, 100);
        w.bucket->reset();
        const uintptr_t item = call(w, record(p101, flags), c, 101);
        const Held h = held(device, item, 101, check);
        check(!h.patched && emit_tests::kindOf(asPool(h.record), 101) == 3 && emit_tests::blockPrev(asPool(h.record)) == p101,
              "append A6 fail-before: without the emission the record keeps previous == current and a marker that is neither tag "
              "(kind 3: camera term only)");
    }

    // ---- A7: the log line, every form ------------------------------------------------------------------------------------------------
    {
        ev::AppendStats as;
        char line[900];
        const auto has = [&](const char* needle) { return std::string(line).find(needle) != std::string::npos; };
        ev::formatAppendLine(as, "not requested", 30, 1800, 0, line, sizeof(line));
        check(has("NOT RUNNING") && has("not requested"), "append A7: the stood-down form says it is not running and why");
        ev::formatAppendLine(as, "hooked", 30, 1800, 0, line, sizeof(line));
        check(has("NOT EXERCISED") && has("made no call"), "append A7: the never-fired form");
        as.calls = 12; as.filteredScale = 12;
        ev::formatAppendLine(as, "hooked", 30, 1800, 0, line, sizeof(line));
        check(has("NOT EXERCISED") && has("all dropped before the join") && has("no shield strike was on screen"),
              "append A7: the all-filtered form names the cause");
        as.calls = 20; as.filteredScale = 16; as.joined = 3; as.masked = 1; as.maxPerFrame = 4;
        ev::formatAppendLine(as, "hooked", 30, 1800, 1, line, sizeof(line));
        check(has("4 unit-scale records observed") && has("at most 4 in a frame, 1 bucket") && has("joined 3, masked 1") &&
              !has("NOT EXERCISED") && std::strlen(line) < 800,
              "append A7: the exercised form reports observed, joined and masked, and fits its buffer");
        ev::formatAppendLine(as, "prologue mismatch at RVA 0x434D120", 30, 1800, 1, line, sizeof(line));
        check(has("NOT RUNNING") && has("prologue mismatch"), "append A7: a hook that stood down reports the reason, not zero counts");
    }
    copy::reset();
}

}  // namespace append_tests
