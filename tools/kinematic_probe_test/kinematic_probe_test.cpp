// Build gate for the KinematicEvalProbe's observe/clock logic -- the four
// probe-side findings of the 2026-09-20 review (2, 3, 7, 9), reproduced
// against the production source. The JSON writer's own gate
// (kinematic_json_test) only exercises serialization; these cases drive
// observe() itself. arm()/finish() are NOT used (they would hook the test
// process); state is seeded directly, the pattern the review harness used.
// VirtualProtect read-fault fixtures run on pages this process owns.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#define private public
#include "../../src/d3d11/kinematic_eval_probe.h"
#undef private
#include "../../src/d3d11/kinematic_eval_hook.h"
// The implementation under test is compiled INTO this TU: MSVC encodes
// access specifiers in decorated names, so a separately linked
// kinematic_eval_probe.obj would mangle clearLocked as private and not
// resolve (the review harness used this same single-TU pattern).
#include "../../src/d3d11/kinematic_eval_probe.cpp"
// The FUN_14434D120 count-and-tag probe's logic (header-only; the hook in kinematic_eval_hook.cpp only reads bytes and calls it).
#include "../../src/d3d11/producer_probe.h"
#include <thread>

namespace edvr {
// Linker stubs: this rig never hooks anything (arm/finish/reset unused).
const char* attachKinematicEvalHooks(KinematicEvalProbe*) noexcept { return "installed"; }
void detachKinematicEvalHooks(KinematicEvalProbe*) noexcept {}
bool kinematicEvalHooksMatch(uintptr_t) noexcept { return false; }
}
using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

// The record as the probe reads it: node +0x18, xf block +0x130 (88 B;
// translation floats xf[8] lo/hi + xf[9] lo, quat lanes xf[9] hi + xf[10]
// lo), hash +0x268, epoch +0x1B8. Descriptor: record pointer at +0x10,
// epoch at +0x38. Render record: flags at +0x688.
struct FakeRec { alignas(16) uint8_t b[0x300]; };
struct FakeDesc { uint8_t b[0x60]; };
struct FakeRender { uint8_t b[0x700]; };

void put64(uint8_t* base, size_t off, uint64_t v) { std::memcpy(base + off, &v, 8); }
void put32(uint8_t* base, size_t off, uint32_t v) { std::memcpy(base + off, &v, 4); }

void setNode(uint8_t* rec, uint64_t node) { put64(rec, 0x18, node); }
// Writes the xf block's translation floats and neutral quat lanes
// (32768,32768,32768,65535), matching the flight-validated packing.
void setPose(uint8_t* rec, float x, float y, float z) {
    uint32_t xb, yb, zb;
    std::memcpy(&xb, &x, 4); std::memcpy(&yb, &y, 4); std::memcpy(&zb, &z, 4);
    const uint64_t w8 = uint64_t(xb) | (uint64_t(yb) << 32);
    const uint64_t w9 = uint64_t(zb) | (uint64_t(32768) << 32) | (uint64_t(32768) << 48);
    const uint64_t w10 = uint64_t(32768) | (uint64_t(65535) << 16);
    put64(rec, 0x130 + 64, w8);
    put64(rec, 0x130 + 72, w9);
    put64(rec, 0x130 + 80, w10);
}
void setTranslationBits(uint8_t* rec, uint32_t xb, float y, float z) {
    uint32_t yb, zb;
    std::memcpy(&yb, &y, 4); std::memcpy(&zb, &z, 4);
    const uint64_t w8 = uint64_t(xb) | (uint64_t(yb) << 32);
    const uint64_t w9 = uint64_t(zb) | (uint64_t(32768) << 32) | (uint64_t(32768) << 48);
    put64(rec, 0x130 + 64, w8);
    put64(rec, 0x130 + 72, w9);
}

FakeDesc descFor(uint8_t* rec) {
    FakeDesc d{};
    put64(d.b, 0x10, reinterpret_cast<uint64_t>(rec));
    return d;
}

// Seed as arm() leaves the probe, minus the hook install: cleared, active,
// frame_ carrying the mesh-domain stamp the first present must overwrite.
void seed(KinematicEvalProbe& p, uint32_t meshStamp) {
    p.clearLocked();
    p.active_.store(true);
    p.frame_.store(meshStamp);
}

// Finding 7: the first present-domain tick must seed WITHOUT flushing --
// before it, sampling is gated, so a flush fabricates an empty frame and
// forces min_frame_records to zero on a healthy feed.
void caseSeedWithoutFlush() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(1000);
    KinematicEvalProbe::Summary s = p.summary();
    check(s.framesCounted == 0, "1: the seed tick counts no frame");
    check(s.zeroRecordFrames == 0, "1: the seed tick is not a zero-record frame");
    check(s.minFrameRecords == ~0ull, "1: min untouched until a real frame ends");
    FakeRec r{}; setNode(r.b, 0xA1); setPose(r.b, 10.f, 20.f, 30.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(1001);
    s = p.summary();
    check(s.framesCounted == 1 && s.zeroRecordFrames == 0 && s.minFrameRecords == 1,
          "1: the first real frame counts exactly one record");
    p.notePresentFrame(1002); // nothing observed
    s = p.summary();
    check(s.zeroRecordFrames == 1, "1: a genuinely empty frame still counts");
}

// Finding 2: a record straddling into an unreadable page must not be created
// from a partial read -- the zero-filled transform baseline used to fabricate
// a 100 m mover on recovery.
void caseFaultedReadCreatesNothing() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(2000);
    auto* pages = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(pages != nullptr, "2: VirtualAlloc");
    if (!pages) return;
    // Low half of the record (node, xf block) in page 0; epoch word (+0x1B8)
    // in page 1, so the early epoch gate can pass while the rest faults.
    uint8_t* rec = pages + 4096 - 0x190;
    setNode(rec, 0xB1); setPose(rec, 100.f, 20.f, 30.f);
    FakeDesc d = descFor(rec); FakeRender rd{};
    DWORD old = 0;
    if (!VirtualProtect(pages, 4096, PAGE_NOACCESS, &old)) { check(false, "2: VirtualProtect"); VirtualFree(pages, 0, MEM_RELEASE); return; }
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    KinematicEvalProbe::Summary s = p.summary();
    check(s.readFaults > 0, "2: the fault is counted");
    check(p.records_.empty(), "2: no record from a partial read");
    if (!VirtualProtect(pages, 4096, PAGE_READWRITE, &old)) { check(false, "2: unprotect"); VirtualFree(pages, 0, MEM_RELEASE); return; }
    p.notePresentFrame(2001);
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(2002);
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    s = p.summary();
    check(p.records_.size() == 1, "2: recovery creates the record once");
    check(s.xfMovers == 0 && s.xfChanges == 0, "2: recovery is not motion");
    check(p.records_[0].totalJump == 0.0 && p.records_[0].maxJump == 0.f,
          "2: no jump fabricated from a zero baseline");
    check(p.records_[0].framesSampled == 2, "2: both good frames sampled");
    VirtualFree(pages, 0, MEM_RELEASE);
}

// Finding 3: a node swap on a continuously-seen pointer ends the previous
// occupant's history -- neither the xf diff nor the pose jump crosses the
// identity boundary.
void caseNodeSwapCrossesNothing() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(3000);
    FakeRec r{}; setNode(r.b, 0xC1); setPose(r.b, 10.f, 20.f, 30.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(3001);
    setNode(r.b, 0xC2); setPose(r.b, 100.f, 20.f, 30.f); // new occupant, 90 m away
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    const KinematicEvalProbe::Summary s = p.summary();
    check(s.nodeChangeEvents == 1, "3: the identity event is kept");
    check(s.xfMovers == 0 && s.xfChanges == 0, "3: the boundary is not an xf change");
    check(p.records_[0].totalJump == 0.0 && p.records_[0].maxJump == 0.f,
          "3: the boundary is not a 90 m jump");
    check(p.records_[0].framesSampled == 2, "3: both occupants sampled");
    check(p.events_.size() == 1 && p.events_[0].kind == 2 &&
          p.events_[0].oldNode == 0xC1 && p.events_[0].newNode == 0xC2,
          "3: the event carries both nodes");
    p.notePresentFrame(3002);
    setPose(r.b, 103.f, 20.f, 30.f); // the new occupant really moves 3 m
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    check(p.records_[0].totalJump > 2.9 && p.records_[0].totalJump < 3.1,
          "3: post-boundary motion measures from the new baseline");
    const KinematicEvalProbe::Summary s2 = p.summary();
    check(s2.xfMovers == 1, "3: the new occupant's own move is a mover");
}

// Finding 9: a non-finite translation must not reach the JSON floats. The
// raw bits stay in prevPose/mover_samples; the measurement is rejected.
void caseNonFiniteRejected() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(4000);
    FakeRec r{}; setNode(r.b, 0xD1); setPose(r.b, 1.f, 2.f, 3.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    p.notePresentFrame(4001);
    setTranslationBits(r.b, 0x7FC00000u, 2.f, 3.f); // NaN x
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    KinematicEvalProbe::Summary s = p.summary();
    check(s.nonFinitePose == 1, "4: NaN jump rejected and counted");
    check(p.records_[0].totalJump == 0.0 && p.records_[0].maxJump == 0.f,
          "4: accumulators stay finite");
    check(s.xfMovers == 1, "4: the raw bits still classify as motion");
    p.notePresentFrame(4002);
    setPose(r.b, 4.f, 2.f, 3.f); // recovery: jump from NaN is NaN, rejected too
    p.observe(reinterpret_cast<uintptr_t>(&d), reinterpret_cast<uintptr_t>(&rd));
    s = p.summary();
    check(s.nonFinitePose == 2 && p.records_[0].totalJump == 0.0,
          "4: recovery off NaN bits stays finite");
    std::ostringstream j;
    p.writeJson(j);
    const std::string out = j.str();
    check(out.find("nan") == std::string::npos && out.find("inf") == std::string::npos,
          "4: the JSON stays parseable");
    check(out.find("\"non_finite_pose\":2") != std::string::npos,
          "4: the counter serializes");
}

// Job attribution (kinematic doc 2026-09-20 17:10): the observation carries
// the bracket TLS bits; the record accumulates the OR of every job it was
// ever seen under, and the mask serializes to the JSON.
void caseJobMaskAccumulates() {
    KinematicEvalProbe p;
    seed(p, 998);
    p.notePresentFrame(5000);
    FakeRec r{}; setNode(r.b, 0xE2); setPose(r.b, 1.f, 2.f, 3.f);
    FakeDesc d = descFor(r.b); FakeRender rd{};
    const uintptr_t dp = reinterpret_cast<uintptr_t>(&d);
    const uintptr_t rp = reinterpret_cast<uintptr_t>(&rd);
    p.observe(dp, rp, 0x2u);
    p.notePresentFrame(5001);
    p.observe(dp, rp, 0x10u);
    check(p.records_[0].jobMask == 0x12u, "5: job bits accumulate across observations");
    p.notePresentFrame(5002);
    p.observe(dp, rp); // default zero mask: no bit, no harm
    check(p.records_[0].jobMask == 0x12u, "5: a zero-mask observation keeps the bits");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"job_mask\":18") != std::string::npos, "5: the mask serializes");
}

// Physics dirty-queue counts (sanctioned 2026-09-20 19:13): entry/exit
// pairs accumulate runs/appended/max-delta per session; exit < entry is a
// mid-run reset and adds only the post-reset residue; only non-trivial
// pairs reach the capped sample vector. No seed(): like jobs[], the counts
// never gate on active().
void casePhysQueue() {
    KinematicEvalProbe p;
    p.notePhysQueue(10, 14); // +4
    p.notePhysQueue(20, 21); // +1
    p.notePhysQueue(9, 2);   // reset mid-run: +2 residue only
    check(p.physQueueRuns_ == 3 && p.physQueueAppended_ == 7 &&
          p.physQueueMaxDelta_.load() == 4 && p.physQueueResets_ == 1,
          "6: counters accumulate; a reset adds only the residue");
    check(p.physQueueSamples_.size() == 3 &&
          p.physQueueSamples_[0].first == 10 && p.physQueueSamples_[0].second == 14 &&
          p.physQueueSamples_[2].first == 9 && p.physQueueSamples_[2].second == 2,
          "6: non-trivial pairs are kept verbatim, resets included");
    p.notePhysQueue(5, 5); // zero delta: counted, not sampled
    check(p.physQueueRuns_ == 4 && p.physQueueAppended_ == 7 &&
          p.physQueueSamples_.size() == 3,
          "6: a zero-delta run counts but does not sample");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"phys_queue\":{\"runs\":4,\"appended\":7,\"max_delta\":4,\"resets\":1,"
                       "\"samples\":[{\"entry\":10,\"exit\":14}") != std::string::npos,
          "6: the counters and samples serialize");
}

// Physics dirty-queue NODE capture (sanctioned 2026-09-20 20:27): the
// probe keeps the last kPhysNodeCap walked pointers per session; overflow
// counts every walked node no longer in the ring, so the gate can assert
// the invariant total == ring + overflow. The hook clamps its walk to the
// cap; the probe still defends against an over-cap batch.
void casePhysNodes() {
    KinematicEvalProbe p;
    uint64_t batch[300];
    for (uint32_t i = 0; i < 300; ++i) batch[i] = 0x1000ull + i;
    p.notePhysNodes(batch, 300, 0); // over-cap batch: newest 256 kept, 44 drop
    check(p.physNodeTotal_ == 300 && p.physNodeOverflow_ == 44 &&
          p.physNodeRing_.size() == KinematicEvalProbe::kPhysNodeCap &&
          p.physNodeRing_.front() == 0x102Cull && p.physNodeRing_.back() == 0x112Bull,
          "7: an over-cap batch keeps the newest and counts the drop");
    uint64_t more[10];
    for (uint32_t i = 0; i < 10; ++i) more[i] = 0xF000ull + i;
    p.notePhysNodes(more, 10, 3); // +10 ring (10 oldest drop), +3 hook-side excess
    check(p.physNodeTotal_ == 313 && p.physNodeOverflow_ == 57 &&
          p.physNodeRing_.size() == KinematicEvalProbe::kPhysNodeCap &&
          p.physNodeRing_.front() == 0x1036ull && p.physNodeRing_.back() == 0xF009ull,
          "7: the ring drops oldest and total stays ring + overflow");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"phys_nodes\":{\"total\":313,\"overflow\":57,\"nodes\":[\"0x1036\"") !=
          std::string::npos, "7: the counters and ring serialize oldest-first");
}

// Draw-item-builder bucket counts (the census join, perf arc): per-session
// counters for the FUN_1442B4420 bracket. A wild entry walk counts and
// returns early; an empty walk counts separately; an exit fault drops the
// call's items but keeps its bucket count; overflow and negative deltas
// accumulate; the maxima track only what their path measured.
void caseBucketItems() {
    KinematicEvalProbe p;
    p.noteBucketBuild(3, 25, 1, 0);                                    // normal: 3 buckets, +25, one drain
    p.noteBucketBuild(0, 0, 0, 0);                                     // legitimately empty rig
    p.noteBucketBuild(0, 0, 0, KinematicEvalProbe::kBucketFlagEntryWild); // faulted/insane walk
    p.noteBucketBuild(5, 99, 0, KinematicEvalProbe::kBucketFlagExitFault);// exit fault: items dropped
    p.noteBucketBuild(2, 7, 0, KinematicEvalProbe::kBucketFlagOverflow);  // cap hit, +7
    check(p.bucketCalls_ == 5 && p.bucketItems_ == 32 &&
          p.bucketEmptyCalls_ == 1 && p.bucketEntryWild_ == 1 &&
          p.bucketExitFault_ == 1 && p.bucketNegDeltas_ == 1 &&
          p.bucketOverflowCalls_ == 1,
          "8: every path lands in its own counter, items skip the faulted call");
    check(p.bucketMaxBuckets_.load() == 5 && p.bucketMaxItems_.load() == 25,
          "8: maxima track the largest measured values");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"bucket_items\":{\"calls\":5,\"items\":32,\"empty_calls\":1,"
                       "\"entry_wild\":1,\"exit_fault\":1,\"neg_deltas\":1,"
                       "\"overflow_calls\":1,\"max_buckets\":5,\"max_items_per_call\":25}") !=
          std::string::npos, "8: the counters serialize");
}

// Direct bucket producers (the census-join completion, perf arc): the
// param_2-bucket brackets accumulate per producer; a read fault counts and
// drops the call's items; an out-of-range producer index is ignored; both
// producers serialize under their function names.
void caseDirectBuild() {
    KinematicEvalProbe p;
    p.noteDirectBuild(0, 40, 0, false);   // producer 0: +40
    p.noteDirectBuild(0, 7, 1, false);    // +7, one mid-call drain
    p.noteDirectBuild(0, 0, 0, true);     // read fault: counted, no items
    p.noteDirectBuild(1, 900, 0, false);  // producer 1: +900
    p.noteDirectBuild(2, 5, 0, false);    // out of range: ignored entirely
    check(p.direct_[0].calls == 3 && p.direct_[0].items == 47 &&
          p.direct_[0].negDeltas == 1 && p.direct_[0].readFaults == 1 &&
          p.direct_[0].maxItems.load() == 40,
          "9: producer 0 accumulates; the fault drops its items");
    check(p.direct_[1].calls == 1 && p.direct_[1].items == 900 &&
          p.direct_[1].maxItems.load() == 900,
          "9: producer 1 keeps a separate stat set (index 2 ignored)");
    std::ostringstream j;
    p.writeJson(j);
    check(j.str().find("\"bucket_items_direct\":[{\"producer\":\"fun_144312e00\","
                       "\"calls\":3,\"items\":47,\"neg_deltas\":1,\"read_faults\":1,"
                       "\"max_items_per_call\":40},{\"producer\":\"fun_14369c9c0\","
                       "\"calls\":1,\"items\":900,\"neg_deltas\":0,\"read_faults\":0,"
                       "\"max_items_per_call\":900}]") != std::string::npos,
          "9: both producers serialize");
}

// ---- The FUN_14434D120 probe (producer_probe.h; the hook only reads bytes and calls these) -------------------------------------------
namespace pp = edvr::producer_probe;
using Bytes = uint8_t[pp::kPrologueBytes];
constexpr uint32_t kTs = KinematicEvalProbe::kExpectedTimestamp, kSize = KinematicEvalProbe::kExpectedImageSize;
const char* const kMismatch = "prologue mismatch at RVA 0x434D120";

// P1: the prologue verdict -- match, mismatch (every byte), a foreign patch, the wrong build, our own patch, and the stand-down it drives.
void caseProducerSignature() {
    // The bytes as the offline disassembly of build 332841 gives them (an independent copy of the constant under test).
    const Bytes spec = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                        0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20};
    check(std::memcmp(spec, pp::kPrologue, sizeof(spec)) == 0 && pp::kRva == 0x434D120u, "P1: the verified prologue is the disassembly's 20 bytes at RVA 0x434D120");
    Bytes got;
    std::memcpy(got, spec, sizeof(got));
    check(pp::signatureVerdict(kTs, kSize, kTs, kSize, got, false, 0) == nullptr, "P1: the stock prologue on the right build installs");
    unsigned mismatches = 0;
    for (unsigned i = 0; i < pp::kPrologueBytes; ++i) {
        Bytes bad;
        std::memcpy(bad, spec, sizeof(bad));
        bad[i] ^= 0x01;
        const char* why = pp::signatureVerdict(kTs, kSize, kTs, kSize, bad, false, 0);
        if (why && std::strcmp(why, kMismatch) == 0) ++mismatches;
    }
    check(mismatches == pp::kPrologueBytes, "P1: a changed byte ANYWHERE in the 20 stands the probe down with the RVA named");
    Bytes foreign;
    std::memcpy(foreign, spec, sizeof(foreign));
    foreign[0] = 0xE9; foreign[1] = 0x10; foreign[2] = 0x20; foreign[3] = 0x30; foreign[4] = 0x00;   // someone else's detour (5-byte jmp over the first instruction)
    const char* why = pp::signatureVerdict(kTs, kSize, kTs, kSize, foreign, false, 0);
    check(why && std::strcmp(why, kMismatch) == 0, "P1: a foreign E9 detour is a mismatch (never patched over)");
    why = pp::signatureVerdict(kTs + 1, kSize, kTs, kSize, got, false, 0);
    check(why && std::strstr(why, "not build 332841"), "P1: another PE timestamp stands down, whatever the bytes");
    why = pp::signatureVerdict(kTs, kSize + 1, kTs, kSize, got, false, 0);
    check(why && std::strstr(why, "not build 332841"), "P1: another image size stands down");
    // Our own patch after install: E9 + rel32 to the relay + the stock tail.
    Bytes ours;
    std::memcpy(ours, spec, sizeof(ours));
    const int32_t rel = 0x01234567;
    ours[0] = 0xE9;
    std::memcpy(ours + 1, &rel, 4);
    check(pp::signatureVerdict(kTs, kSize, kTs, kSize, ours, true, rel) == nullptr, "P1: our own E9 to the relay, stock tail intact, verifies after install");
    why = pp::signatureVerdict(kTs, kSize, kTs, kSize, ours, true, rel + 1);
    check(why && std::strcmp(why, kMismatch) == 0, "P1: an E9 to somewhere else is not ours");
    Bytes tail;
    std::memcpy(tail, ours, sizeof(tail));
    tail[12] ^= 0x40;
    why = pp::signatureVerdict(kTs, kSize, kTs, kSize, tail, true, rel);
    check(why && std::strcmp(why, kMismatch) == 0, "P1: our E9 over a damaged tail does not verify");
    why = pp::signatureVerdict(kTs, kSize, kTs, kSize, got, true, rel);
    check(why && std::strstr(why, "our patch is gone"), "P1: stock bytes where our patch should be: 'our patch is gone'");
    why = pp::signatureVerdict(kTs, kSize, kTs, kSize, ours, false, 0);
    check(why && std::strcmp(why, kMismatch) == 0, "P1: before install, an already-patched entry is NOT taken for stock");
    // The stand-down: the status carries the reason, the probe counts nothing, and its line says so instead of a count.
    pp::resetAll();
    check(!pp::enabled() && std::strcmp(pp::status(), "not requested") == 0, "P1: before any attach: disabled, 'not requested'");
    pp::applyVerdict(kMismatch);
    check(!pp::enabled() && std::strcmp(pp::status(), kMismatch) == 0, "P1: a mismatch verdict disables the probe and keeps the reason");
    pp::noteCall(10, 99, 0x1000, 0x434D741, pp::kModelBlockSet);
    pp::noteRigid(pp::kBuilder, 10, 0x1000);
    pp::Snapshot s = pp::takeWindow();
    check(s.calls == 0 && s.buckets == 0 && s.rigidBuckets == 0, "P1: stood down, a call and a rigid note count nothing");
    char line[pp::kLineBytes];
    pp::formatLine(s, pp::status(), 30.0, 1800.0, line);
    check(std::strstr(line, kMismatch) && std::strstr(line, "nothing is counted") && !std::strstr(line, "calls "), "P1: the stood-down line names the reason and shows no counts");
    pp::applyVerdict(nullptr);
    check(pp::enabled() && std::strcmp(pp::status(), "hooked") == 0, "P1: a null verdict is 'hooked' and enables the counts");
    pp::resetAll();
}

// P2: the window -- counts, distinct sets, overlap, flips, a full table, and four threads at once.
void caseProducerWindow() {
    pp::resetAll();
    pp::applyVerdict(nullptr);
    for (int i = 0; i < 3; ++i) pp::noteCall(10, 99, 0x1000, 0x434D741, pp::kModelBlockZero);
    pp::noteCall(11, 99, 0x2000, 0x434D741, pp::kModelBlockSet);
    pp::noteCall(99, 99, 0x1000, 0x434D800, pp::kUnreadable);   // the render thread, a second caller
    pp::noteCall(12, 99, 0, 0x434D741, pp::kModelBlockZero);    // bucket 0 is a valid key, not "empty"
    pp::noteRigid(pp::kBuilder, 10, 0x1000);                    // shared bucket, shared thread
    pp::noteRigid(pp::kBuilder, 10, 0x1000);
    pp::noteRigid(pp::kDirect0, 77, 0x3000);
    pp::noteRigid(pp::kDirect1, 77, 0x4000);
    pp::Snapshot s = pp::takeWindow();
    check(s.calls == 6 && s.kinds[pp::kModelBlockZero] == 4 && s.kinds[pp::kModelBlockSet] == 1 && s.kinds[pp::kUnreadable] == 1,
          "P2: six calls, and the record kinds (zero 4, set 1, unreadable 1) add up");
    check(s.buckets == 3 && s.threads == 4 && s.renderSeen, "P2: three distinct buckets (0 counts), four threads, the render thread seen");
    check(s.rigidBuckets == 3 && s.rigidThreads == 2 && s.rigidNotes[pp::kBuilder] == 2 && s.rigidNotes[pp::kDirect0] == 1 && s.rigidNotes[pp::kDirect1] == 1,
          "P2: the rigid producers' distinct buckets and threads, and their notes per source");
    check(s.shared == 1 && s.sharedThreads == 1, "P2: one probe bucket (0x1000) and one thread (10) are also the rigid producers'");
    check(s.callerCountListed == 2 && s.callerRva[0] == 0x434D741 && s.callerCount[0] == 5 && s.callerRva[1] == 0x434D800 && s.callerCount[1] == 1,
          "P2: callers listed busiest first with their counts");
    check(s.listedThreadCount == 4 && s.listedThreads[0] == 10 && s.listedThreads[3] == 99, "P2: the thread list is sorted");
    // The next window starts empty, and a call made after the flip lands in it, not in the one just read.
    pp::noteCall(10, 99, 0x9000, 0x434D741, pp::kModelBlockZero);
    pp::Snapshot t = pp::takeWindow();
    check(t.calls == 1 && t.buckets == 1 && t.rigidBuckets == 0 && t.shared == 0, "P2: the flip: the next window holds only the later call");
    pp::Snapshot u = pp::takeWindow();
    check(u.calls == 0 && u.buckets == 0 && u.threads == 0 && !u.renderSeen, "P2: an idle window is empty (the cell cleared on retiring)");
    // A table that fills counts its overflow instead of looping or corrupting.
    for (uint64_t b = 1; b <= 1100; ++b) pp::noteCall(10, 99, b << 4, 0x434D741, pp::kModelBlockZero);
    pp::Snapshot v = pp::takeWindow();
    check(v.calls == 1100 && v.buckets == 1024 && v.bucketOverflow == 76, "P2: 1100 distinct buckets: 1024 kept, 76 lost to the full table, all 1100 calls counted");
    // Four threads, the same 64 buckets: nothing lost, nothing double counted.
    std::vector<std::thread> ts;
    for (unsigned t2 = 0; t2 < 4; ++t2)
        ts.emplace_back([t2] {
            for (uint64_t i = 0; i < 20000; ++i) {
                pp::noteCall(100 + t2, 1, 0x7000 + (i % 64) * 0x40, 0x434D741, pp::kModelBlockZero);
                pp::noteRigid(pp::kDirect0, 100 + t2, 0x7000 + (i % 64) * 0x40);
            }
        });
    for (auto& th : ts) th.join();
    pp::Snapshot w = pp::takeWindow();
    check(w.calls == 80000 && w.buckets == 64 && w.threads == 4 && w.shared == 64 && w.sharedThreads == 4 && w.rigidNotes[pp::kDirect0] == 80000 &&
          w.callerCount[0] == 80000, "P2: four threads x 20000 calls: every call counted, 64 buckets and 4 threads exactly, all shared with the rigid notes");
    pp::resetAll();
}

// P3: the 30 s line in its three forms -- counts, never fired, stood down -- and the buffer it must fit.
void caseProducerLine() {
    pp::resetAll();
    pp::applyVerdict(nullptr);
    pp::noteCall(10, 99, 0x1000, 0x434D741, pp::kModelBlockZero);
    pp::noteCall(11, 99, 0x2000, 0x434D741, pp::kModelBlockSet);
    pp::noteCall(99, 99, 0x2000, 0x434D741, pp::kUnreadable);
    pp::noteRigid(pp::kBuilder, 10, 0x1000);
    pp::noteRigid(pp::kDirect0, 55, 0x5000);
    char line[pp::kLineBytes];
    pp::formatLine(pp::takeWindow(), pp::status(), 30.0, 1800.0, line);
    const std::string counted = line;
    check(counted.rfind("engine motion: producer 434D120 probe (hooked, 30 s, 1800 frames): calls 3 (0.0 a frame)", 0) == 0, "P3: the line opens with the status, window and calls");
    check(counted.find(", 2 distinct buckets (0 lost to a full table); 3 threads [10 11 99], render thread YES, 1 of them also appended for the rigid producers (2 rigid threads); ") != std::string::npos,
          "P3: distinct buckets, the threads listed, the render thread, and the threads shared with the rigid producers");
    check(counted.find("the rigid producers appended to 2 buckets (builder 42B4420 1, direct 4312E00 1, direct 369C9C0 0; 0 lost), 1 of the probe's 2 buckets among them; callers 0x434D741 x3;") != std::string::npos,
          "P3: the rigid producers' buckets per source, the overlap, and the callers");
    check(counted.find("model block +0x20..+0x9F zero 1, set 1, unreadable 1.") != std::string::npos, "P3: the record kinds close the line");
    // NEVER FIRED: hooked, an idle window. The rigid side is still reported, so a quiet probe is told apart from a quiet game.
    pp::noteRigid(pp::kBuilder, 10, 0x1000);
    pp::formatLine(pp::takeWindow(), pp::status(), 30.0, 1800.0, line);
    const std::string idle = line;
    std::fprintf(stderr, "[P3 never-fired line] %s\n", line);   // stderr only: the form is on record in the build log
    check(idle.find("(hooked, 30 s, 1800 frames): calls 0 (0.0 a frame), NEVER FIRED in this window") != std::string::npos && idle.find("distinct") == std::string::npos &&
          idle.find("appended to 1 buckets this window (builder 42B4420 1, direct 4312E00 0, direct 369C9C0 0)") != std::string::npos,
          "P3: never fired: 'calls 0', NEVER FIRED, no distinct counts, and the rigid producers' activity beside it");
    // Stood down, and never requested.
    pp::formatLine(pp::Snapshot{}, "prologue mismatch at RVA 0x434D120", 30.0, 1800.0, line);
    check(std::strstr(line, "producer 434D120 probe (prologue mismatch at RVA 0x434D120): the probe is not running, so nothing is counted") != nullptr,
          "P3: stood down: the reason in the status slot, no counts");
    pp::formatLine(pp::Snapshot{}, "not requested", 30.0, 1800.0, line);
    check(std::strstr(line, "probe (not requested): the probe is not running") != nullptr && std::strstr(line, "\"not requested\" means engine motion never attached it") != nullptr,
          "P3: never requested: the line says so and says what that means");
    // Worst case: every field at its limit still fits the buffer and ends with its closing full stop (nothing cut off).
    pp::Snapshot big;
    big.calls = ~0ull; big.kinds[0] = big.kinds[1] = big.kinds[2] = ~0ull; big.rigidNotes[0] = big.rigidNotes[1] = big.rigidNotes[2] = ~0ull;
    big.buckets = big.rigidBuckets = big.shared = big.bucketOverflow = big.rigidOverflow = ~0u;
    big.threads = big.rigidThreads = big.sharedThreads = ~0u;
    big.renderSeen = true;
    big.listedThreadCount = pp::kListedThreads;
    for (unsigned i = 0; i < pp::kListedThreads; ++i) big.listedThreads[i] = 4000000000u + i;
    big.callerCountListed = pp::kListedCallers;
    for (unsigned i = 0; i < pp::kListedCallers; ++i) { big.callerRva[i] = 0x7FFFFFFFFFFFull; big.callerCount[i] = ~0ull; }
    big.callerOverflow = ~0u;
    pp::formatLine(big, "hooked", 30.0, 1800.0, line);
    const size_t n = std::strlen(line);
    check(n > 600 && n < pp::kLineBytes - 1 && line[n - 1] == '.', "P3: the worst-case line fits the buffer with its last sentence whole");
}

// P4: the eye run's capture -- the frame window, the buffer cap, the file layout the Python reader expects.
void caseProducerCapture() {
    pp::resetAll();
    uint8_t rec[pp::kRecordBytes];
    for (unsigned i = 0; i < sizeof(rec); ++i) rec[i] = uint8_t(i * 7 + 3);
    check(!pp::captureOn(), "P4: not capturing before the eye run arms it");
    pp::noteCapture(1, 2, 3, 4, 5, rec);
    check(pp::armCapture(10, 20) && pp::captureOn(), "P4: armed for frames 10..20");
    pp::setFrame(9);  pp::noteCapture(1, 0xB, 0xC, 0xD, 0x434D741, rec);   // before the window
    pp::setFrame(10); pp::noteCapture(1, 0xB, 0xC, 0xD, 0x434D741, rec);
    const uint8_t last = rec[335];
    rec[0] = uint8_t(rec[0] ^ 0xFFu);
    const uint8_t flipped = rec[0];
    pp::setFrame(20); pp::noteCapture(2, 0xB2, 0xC2, 0xD2, 0x434D800, rec);
    pp::setFrame(21); pp::noteCapture(3, 0xB3, 0xC3, 0xD3, 0x434D741, rec);   // after the window
    pp::CaptureView v = pp::stopCapture();
    check(!pp::captureOn() && v.header.count == 2 && v.header.dropped == 0 && v.header.frame0 == 10 && v.header.frame1 == 20 &&
          std::memcmp(v.header.magic, "EDVRP434", 8) == 0 && v.header.version == 1 && v.header.entryBytes == 376 && v.header.recordBytes == 336,
          "P4: two records inside the window kept, the ones outside dropped silently, header as the reader expects");
    check(v.entries && v.entries[0].frame == 10 && v.entries[0].tid == 1 && v.entries[0].bucket == 0xB && v.entries[0].modelKey == 0xC &&
          v.entries[0].nodeKey == 0xD && v.entries[0].ret == 0x434D741 && v.entries[0].record[0] == 3 && v.entries[0].record[335] == last &&
          v.entries[1].frame == 20 && v.entries[1].record[0] == flipped,
          "P4: each entry keeps frame, thread, bucket, model key, node key, caller and all 336 record bytes");
    pp::noteCapture(9, 9, 9, 9, 9, rec);
    check(pp::stopCapture().header.count == 2, "P4: nothing is kept after the capture stopped");
    // Re-arm: starts clean. Overfill: the cap holds and the excess is counted, not written.
    check(pp::armCapture(0, 100), "P4: re-armed");
    pp::setFrame(50);
    for (uint32_t i = 0; i < pp::kCaptureCap + 5; ++i) pp::noteCapture(i, i, i, i, i, rec);
    pp::CaptureView full = pp::stopCapture();
    check(full.header.count == pp::kCaptureCap && full.header.dropped == 5 && full.entries[pp::kCaptureCap - 1].tid == pp::kCaptureCap - 1,
          "P4: the buffer holds its cap, the 5 past it are counted as dropped");
    // The empty capture is still a valid file: zero records means 'the producer did not run', which the reader reports.
    check(pp::armCapture(1, 2), "P4: armed again");
    check(pp::stopCapture().header.count == 0, "P4: an armed capture with no calls is a zero-record file");
    pp::resetAll();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("kinematic_probe_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseSeedWithoutFlush();
    caseFaultedReadCreatesNothing();
    caseNodeSwapCrossesNothing();
    caseNonFiniteRejected();
    caseJobMaskAccumulates();
    casePhysQueue();
    casePhysNodes();
    caseBucketItems();
    caseDirectBuild();
    caseProducerSignature();
    caseProducerWindow();
    caseProducerLine();
    caseProducerCapture();
    std::printf("kinematic_probe_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
