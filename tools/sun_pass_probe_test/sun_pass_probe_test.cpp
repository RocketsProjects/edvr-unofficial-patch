// The hangar sun probe's pure half (src\d3d11\sun_pass_probe_core.h), held to the exe. TEMPORARY, like the probe.
//
//   sun_pass_probe_test --self-test   runs every check and exits 0 when all pass.
//
// The bytes below are NOT copied from the header. They are read from EliteDangerous64.exe build 332841 (the Epic install, PE timestamp
// 1788384820, image size 104894464) by reading the file at the RVAs and are typed here independently, so a typo in the header fails this rig.

#include "../../src/d3d11/sun_pass_probe_core.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

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

// Read from the exe at RenderDirLights (RVA 0x5AC9C0), 32 bytes.
const uint8_t kExeProlog[32] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x60, 0x33, 0xFF, 0x48, 0x89, 0x51, 0x40, 0x48, 0x8B, 0xD9};
// Read from the exe at the call in the deferred lighting frame (RVA 0x284E159), 5 bytes.
const uint8_t kExeCall[5] = {0xE8, 0x62, 0xE8, 0xD5, 0xFD};

void gateChecks() {
    check(sp::kRenderDirLightsRva == 0x5AC9C0u, "RenderDirLights RVA");
    check(sp::kCallSiteRva == 0x284E159u, "call site RVA");
    check(std::memcmp(sp::kPrologue, kExeProlog, 32) == 0, "header prologue equals the exe's 32 bytes");
    check(std::memcmp(sp::kCallSite, kExeCall, 5) == 0, "header call bytes equal the exe's 5 bytes");
    check(sp::kMemoryWatchRva == 0x5E889C0u, "memory watch RVA");
    check(sp::kPrologueBytes == 32, "prologue span is 32");

    // Accepted: the real bytes, identity and call site.
    check(sp::gateVerdict(1788384820u, 104894464u, kExeProlog, kExeCall) == nullptr, "gate accepts the real build");
    // Identity refusals.
    check(sp::gateVerdict(1788384821u, 104894464u, kExeProlog, kExeCall) != nullptr, "gate refuses a PE timestamp off by one");
    check(sp::gateVerdict(1788384820u, 104894465u, kExeProlog, kExeCall) != nullptr, "gate refuses an image size off by one");
    // Every single-byte change in the prologue is refused.
    bool allRefused = true;
    for (size_t i = 0; i < 32; ++i) {
        uint8_t mut[32];
        std::memcpy(mut, kExeProlog, 32);
        mut[i] ^= 0x01;
        if (sp::gateVerdict(1788384820u, 104894464u, mut, kExeCall) == nullptr) allRefused = false;
    }
    check(allRefused, "a one-bit change at any of the 32 prologue bytes is refused");
    // A changed call site is refused, and so is a call whose bytes are right but whose target is another function.
    uint8_t badCall[5];
    std::memcpy(badCall, kExeCall, 5);
    badCall[4] ^= 0x01;
    check(sp::gateVerdict(1788384820u, 104894464u, kExeProlog, badCall) != nullptr, "gate refuses a one-byte change in the call");
    uint8_t otherTarget[5] = {0xE8, 0x00, 0x00, 0x00, 0x00};   // a call to the next instruction: not the build's bytes, so refused on the bytes
    check(sp::gateVerdict(1788384820u, 104894464u, kExeProlog, otherTarget) != nullptr, "gate refuses a call to another target (the bytes differ)");
    check(sp::gateVerdict(1788384820u, 104894464u, nullptr, kExeCall) != nullptr, "gate refuses a missing prologue");
    check(sp::gateVerdict(1788384820u, 104894464u, kExeProlog, nullptr) != nullptr, "gate refuses a missing call site");
}

void scopeChecks() {
    check(sp::sunPassDepth == 0, "depth starts at zero");
    {
        sp::DepthScope a(sp::sunPassDepth);
        check(sp::sunPassDepth == 1, "one scope: depth 1");
        {
            sp::DepthScope b(sp::sunPassDepth);
            check(sp::sunPassDepth == 2, "nested scope: depth 2");
        }
        check(sp::sunPassDepth == 1, "nested scope closed: depth 1");
    }
    check(sp::sunPassDepth == 0, "outer scope closed: depth 0");
    // An exception thrown inside the scope still restores the count.
    try {
        sp::DepthScope c(sp::sunPassDepth);
        throw std::runtime_error("the forward throws");
    } catch (const std::runtime_error&) {
    }
    check(sp::sunPassDepth == 0, "a throw out of the scope restores depth 0");
    // The depth is per thread: another thread's count does not show here.
    bool otherThreadZero = false;
    {
        sp::DepthScope d(sp::sunPassDepth);
        std::thread t([&] { otherThreadZero = (sp::sunPassDepth == 0); });
        t.join();
    }
    check(otherThreadZero, "the depth is thread-local (another thread reads zero)");
}

void skipChecks() {
    check(!sp::skipsDraw(0, false), "outside the scope, toggle off: forwarded");
    check(!sp::skipsDraw(0, true), "outside the scope, toggle on: forwarded (never skipped outside the sun pass)");
    check(!sp::skipsDraw(1, false), "inside the scope, toggle off: forwarded (the count still runs)");
    check(sp::skipsDraw(1, true), "inside the scope, toggle on: skipped");
    check(sp::skipsDraw(2, true), "nested inside the scope, toggle on: skipped");
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
}

void formatChecks() {
    char buf[400];
    sp::Window w;
    w.calls = 3;
    w.draws = 40;
    w.skipped = 40;
    sp::formatWindow(buf, sizeof(buf), w, true);
    check(std::strcmp(buf, "hangar sun probe: dirlights_calls=3 draws_in_scope=40 skipped=40 toggle=on") == 0, "window line text (on)");
    sp::formatWindow(buf, sizeof(buf), w, false);
    check(std::strcmp(buf, "hangar sun probe: dirlights_calls=3 draws_in_scope=40 skipped=40 toggle=off") == 0, "window line text (off)");

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
    check(std::strcmp(buf, "hangar sun probe: skip toggle -> on at frame=100 (the draws in scope are not forwarded)") == 0, "toggle line text (on)");
    sp::formatToggle(buf, sizeof(buf), false, 101);
    check(std::strcmp(buf, "hangar sun probe: skip toggle -> off at frame=101 (the draws in scope are forwarded as before)") == 0, "toggle line text (off)");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: sun_pass_probe_test --self-test\n");
        return 2;
    }
    gateChecks();
    scopeChecks();
    skipChecks();
    statusChecks();
    watchChecks();
    readableChecks();
    windowChecks();
    formatChecks();
    std::printf("sun_pass_probe_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
