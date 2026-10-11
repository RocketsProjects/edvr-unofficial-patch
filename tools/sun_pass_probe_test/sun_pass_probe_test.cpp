// The hangar sun probe's pure half (src\d3d11\sun_pass_probe_core.h), held to the pair it keys on. TEMPORARY, like the probe.
//
//   sun_pass_probe_test --self-test   runs every check and exits 0 when all pass.
//
// The hashes below are NOT copied from the header. They are typed here from the census notes that name the sun pair (docs\scanner-body.md and
// docs\ambient-occlusion.md), so a typo in the header fails this rig.

#include "../../src/d3d11/sun_pass_probe_core.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sp = edvr::sunprobe;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

const uint64_t kVs = 0x7E38A6AA1269C901ull;   // typed here, not copied from the header
const uint64_t kPs = 0x7CECABDE34FFBE9Eull;

void identityChecks() {
    check(sp::identityMatches(1788384820u, 104894464u), "the build 332841 identity is accepted");
    check(!sp::identityMatches(1788384821u, 104894464u), "a PE timestamp off by one is refused");
    check(!sp::identityMatches(1788384820u, 104894465u), "an image size off by one is refused");
    check(sp::kMemoryWatchRva == 0x5E889C0u, "memory watch RVA");
    check(sp::kMemoryWatchBytes == 4, "memory watch reads four bytes");
}

void pairChecks() {
    check(sp::kSunVs == kVs, "the sun VS constant is 7E38A6AA1269C901");
    check(sp::kSunPs == kPs, "the sun PS constant is 7CECABDE34FFBE9E");
    check(sp::kSkipTableCount == 1, "the skip table has one entry (the sun)");
    check(std::strcmp(sp::kSkipTable[0].name, "sun") == 0, "the entry is named sun");
    check(sp::kSkipTable[0].vs == kVs && sp::kSkipTable[0].ps == kPs, "the entry lists the sun pair");
}

void classifyChecks() {
    check(sp::classify(kVs, kPs) == sp::Match::kPair, "VS 7E38 with PS 7CEC: a pair");
    check(sp::classify(kVs, 0x1234ull) == sp::Match::kVsOnly, "VS 7E38 with another PS: VS only");
    check(sp::classify(0x1234ull, kPs) == sp::Match::kPsOnly, "PS 7CEC with another VS: PS only");
    check(sp::classify(kPs, kVs) == sp::Match::kNone, "the two hashes swapped: nothing");
    check(sp::classify(0, 0) == sp::Match::kNone, "no shaders bound: nothing");
    check(sp::classify(0, kPs) == sp::Match::kPsOnly, "no VS bound, the sun PS: PS only");
    check(sp::classify(kVs, 0) == sp::Match::kVsOnly, "the sun VS, no PS bound: VS only");
    check(sp::classify(kVs ^ 1u, kPs ^ 1u) == sp::Match::kNone, "both hashes off by one bit: nothing");
}

void fieldChecks() {
    check(sp::fieldFor(sp::Match::kPair) == sp::kFieldPairs, "a pair counts in pairs");
    check(sp::fieldFor(sp::Match::kVsOnly) == sp::kFieldVsOnly, "a VS-only draw counts in VS only");
    check(sp::fieldFor(sp::Match::kPsOnly) == sp::kFieldPsOnly, "a PS-only draw counts in PS only");
    check(sp::fieldFor(sp::Match::kNone) == sp::kFieldCount, "an unmatched draw counts nowhere");
    check(sp::kFieldCount == 4, "four window counters");
}

void skipChecks() {
    check(sp::skipsDraw(sp::Match::kPair, true), "a pair, toggle on: skipped");
    check(!sp::skipsDraw(sp::Match::kPair, false), "a pair, toggle off: forwarded (still counted)");
    check(!sp::skipsDraw(sp::Match::kVsOnly, true), "a VS-only draw, toggle on: forwarded");
    check(!sp::skipsDraw(sp::Match::kPsOnly, true), "a PS-only draw, toggle on: forwarded");
    check(!sp::skipsDraw(sp::Match::kNone, true), "an unmatched draw, toggle on: forwarded");
}

// The glue's counting, replayed on a draw sequence: a pair counts in pairs and, with the toggle on, in skipped too.
void countingChecks() {
    uint64_t counts[sp::kFieldCount] = {};
    const sp::Match seq[] = {sp::Match::kPair, sp::Match::kVsOnly, sp::Match::kNone, sp::Match::kPair, sp::Match::kPsOnly, sp::Match::kPair};
    for (sp::Match m : seq) {
        const size_t f = sp::fieldFor(m);
        if (f != sp::kFieldCount) ++counts[f];
        if (sp::skipsDraw(m, true)) ++counts[sp::kFieldSkipped];
    }
    check(counts[sp::kFieldPairs] == 3, "three pairs counted");
    check(counts[sp::kFieldVsOnly] == 1 && counts[sp::kFieldPsOnly] == 1, "one VS-only and one PS-only counted");
    check(counts[sp::kFieldSkipped] == 3, "all three pairs skipped with the toggle on");

    uint64_t off[sp::kFieldCount] = {};
    for (sp::Match m : seq) {
        const size_t f = sp::fieldFor(m);
        if (f != sp::kFieldCount) ++off[f];
        if (sp::skipsDraw(m, false)) ++off[sp::kFieldSkipped];
    }
    check(off[sp::kFieldPairs] == 3 && off[sp::kFieldSkipped] == 0, "toggle off: pairs counted, none skipped");
}

void statusChecks() {
    sp::StatusWords none;
    sp::StatusView v = sp::decodeStatus(none);
    check(!v.docked && !v.landed && !v.supercruise && !v.onFoot && !v.seated && !v.inside, "no words: every answer is off");

    sp::StatusWords docked;
    docked.flagsKnown = true;
    docked.flags = 0x1u;
    docked.flags2Known = true;
    docked.flags2 = 0x1u;
    v = sp::decodeStatus(docked);
    check(v.docked && !v.landed && v.onFoot && v.inside, "docked and on foot in a station: inside");

    sp::StatusWords dockedLanded = docked;
    dockedLanded.flags = 0x3u;
    v = sp::decodeStatus(dockedLanded);
    check(v.docked && v.landed && !v.inside, "docked AND landed (an outdoor pad): not inside");

    sp::StatusWords supercruise = docked;
    supercruise.flags = 0x11u;
    v = sp::decodeStatus(supercruise);
    check(v.supercruise && !v.inside, "docked bit with supercruise: not inside");

    sp::StatusWords noFoot = docked;
    noFoot.flags2Known = false;
    noFoot.flags2 = 0;
    v = sp::decodeStatus(noFoot);
    check(!v.onFoot && !v.inside, "no Flags2 and no main ship: not inside");

    sp::StatusWords mainShip;
    mainShip.flagsKnown = true;
    mainShip.flags = 0x1u | (1u << 24);
    v = sp::decodeStatus(mainShip);
    check(v.docked && v.seated && v.inside, "docked in the main ship: inside");

    sp::StatusWords taxi;
    taxi.flagsKnown = true;
    taxi.flags = 0u;
    taxi.flags2Known = true;
    taxi.flags2 = 0x2u;
    v = sp::decodeStatus(taxi);
    check(v.seated && !v.onFoot && !v.docked, "a taxi (Flags2 bit 1): seated, not on foot");

    sp::StatusWords unknownFlags;
    unknownFlags.flags2Known = true;
    unknownFlags.flags2 = 0x1u;
    v = sp::decodeStatus(unknownFlags);
    check(!v.docked && !v.inside && v.onFoot, "Flags absent but Flags2 present: docked stays off, on foot reads true");
}

void watchChecks() {
    sp::WatchSample last;
    const uint8_t a[4] = {0x12, 0x34, 0xAB, 0xCD};
    check(sp::watchChanged(last, a), "the first sample is logged as the baseline");
    last.known = true;
    std::memcpy(last.bytes, a, 4);
    check(!sp::watchChanged(last, a), "the same four bytes: no change");
    const uint8_t b[4] = {0x12, 0x34, 0xAB, 0xCC};
    check(sp::watchChanged(last, b), "one byte differs: a change");
}

void readableChecks() {
    // Sections as build 332841 has them (virtual address, virtual size, characteristics).
    const sp::SectionSpan secs[] = {
        {0x1000u, 0x4DB1E9Eu, 0x60000020u},     // .text: code, readable
        {0x4DB3000u, 0x10AF00Cu, 0x40000040u},  // .rdata
        {0x5E63000u, 0x1F0920u, 0xC0000040u},   // .data, read/write
        {0x6054000u, 0x2E6824u, 0x40000040u},   // .pdata
        {0x7000000u, 0x100u, 0x80000000u},      // write-only (not readable)
    };
    const size_t n = sizeof(secs) / sizeof(secs[0]);
    check(sp::rvaReadable(sp::kMemoryWatchRva, secs, n), "exe+0x5E889C0 is inside the readable .data");
    check(!sp::rvaReadable(0x7000000u, secs, n), "a write-only section is not readable");
    check(!sp::rvaReadable(0x7000000u + 0x100u - 2, secs, n), "a read that runs past its section is refused");
    check(!sp::rvaReadable(0x6F00000u, secs, n), "an RVA in no section is refused");
    check(sp::rvaReadable(0x5E63000u + 0x1F0920u - 4, secs, n), "the last four bytes of .data are readable");
    check(!sp::rvaReadable(0x5E63000u + 0x1F0920u - 3, secs, n), "a four-byte read crossing the end of .data is refused");
}

void windowChecks() {
    check(!sp::windowDue(9999, 0), "9.999 s: not due");
    check(sp::windowDue(10000, 0), "10 s: due");
    check(!sp::windowDue(5, 10), "clock behind the window start: not due");
    sp::Window empty;
    check(!sp::windowActive(empty), "an empty window is not written");
    sp::Window halfOnly;
    halfOnly.psOnly = 1;
    check(sp::windowActive(halfOnly), "a PS-only count alone makes the window active");
    sp::Window skippedOnly;
    skippedOnly.skipped = 1;
    check(sp::windowActive(skippedOnly), "a skip alone makes the window active");
}

void formatChecks() {
    char buf[400];
    sp::Window w;
    w.pairs = 40;
    w.skipped = 40;
    w.vsOnly = 2;
    w.psOnly = 1;
    sp::formatWindow(buf, sizeof(buf), w, true);
    check(std::strcmp(buf, "hangar sun probe: sun pair draws=40 skipped=40 (VS only=2, PS only=1) toggle=on") == 0, "window line text (on)");
    sp::formatWindow(buf, sizeof(buf), w, false);
    check(std::strcmp(buf, "hangar sun probe: sun pair draws=40 skipped=40 (VS only=2, PS only=1) toggle=off") == 0, "window line text (off)");

    const uint8_t bytes[4] = {0x12, 0x34, 0xAB, 0xCD};
    sp::StatusWords words;
    words.flagsKnown = true;
    words.flags = 0x01000001u;
    sp::formatWatch(buf, sizeof(buf), bytes, 42, words);
    check(std::strcmp(buf, "hangar sun probe: 0x5E889C0 bytes=12 34 AB CD (frame=42, status Flags=0x01000001)") == 0, "memory watch line text");
    sp::StatusWords none;
    sp::formatWatch(buf, sizeof(buf), bytes, 7, none);
    check(std::strcmp(buf, "hangar sun probe: 0x5E889C0 bytes=12 34 AB CD (frame=7, status Flags=-)") == 0, "memory watch line text with no status");

    sp::formatStatus(buf, sizeof(buf), words, 9);
    check(std::strcmp(buf,
        "hangar sun probe: status Flags=0x01000001 Flags2=- frame=9 (docked=1 landed=0 supercruise=0 onfoot=0 seated=1 inside_gate=1)") == 0,
        "status line text");

    sp::formatToggle(buf, sizeof(buf), true, 100);
    check(std::strcmp(buf, "hangar sun probe: skip toggle -> on at frame=100 (the sun pair's draws are not forwarded)") == 0, "toggle line text (on)");
    sp::formatToggle(buf, sizeof(buf), false, 101);
    check(std::strcmp(buf, "hangar sun probe: skip toggle -> off at frame=101 (the sun pair's draws are forwarded as before)") == 0, "toggle line text (off)");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: sun_pass_probe_test --self-test\n");
        return 2;
    }
    identityChecks();
    pairChecks();
    classifyChecks();
    fieldChecks();
    skipChecks();
    countingChecks();
    statusChecks();
    watchChecks();
    readableChecks();
    windowChecks();
    formatChecks();
    std::printf("sun_pass_probe_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
