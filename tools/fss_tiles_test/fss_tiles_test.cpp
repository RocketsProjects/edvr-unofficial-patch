// Test rig for the `fss tiles` instrument and the tiles candidate (src/d3d11/fss_tiles_core.h, fss_tiles_d3d.h, fss_tiles.cpp).
//
//   1. The pure half: identities, the view table, the zoom-window machine, the substitution rule, the exact log line, the sharing
//      tally, and the key's value parsing (tiles is new; lockstep must still mean the composite freeze).
//   2. The state restore, on a fake context: every slot swapped in comes back to what it was, including a slot that was unbound,
//      and the dispatch sees the substituted state.
//   3. The same on a WARP device with the debug layer, through the production D3dOps: two "views" (their own argument buffers,
//      list buffers and a texture), a stand-in tile shader that writes list[0] + group id into an output buffer. Stock draws the
//      second view's groups from the second view's list; the substitution draws the FIRST view's group count from the FIRST
//      view's list; afterwards the context's compute SRV state is exactly what it was, and a following stock dispatch is stock.
//   4. Source pins on the glue no rig can run (the hook is linked into the DLL only).
//
// No window, no files written. --self-test runs it; --dry-run says what it would do.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/common/eye_sync.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/eye_final_capture.h"
#include "../../src/d3d11/fss_ring_core.h"
#include "../../src/d3d11/fss_tiles_core.h"
#include "../../src/d3d11/fss_tiles_d3d.h"

using Microsoft::WRL::ComPtr;
namespace ft = edvr::fsstiles;

namespace edvr {
int guardFilter(unsigned long, const char*) { return EXCEPTION_EXECUTE_HANDLER; }   // the rigs that include guard.h without linking guard.cpp stub it
}  // namespace edvr

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 1. The pure half

void testIdentities() {
    ft::Intern in;
    check(in.id(nullptr) == 0, "a null pointer is id 0");
    int a, b;
    const uint32_t ia = in.id(&a), ib = in.id(&b);
    check(ia == 1 && ib == 2 && in.id(&a) == 1 && in.id(&b) == 2, "ids are stable and start at 1");
    ft::Intern full;
    static int pool[ft::Intern::kCap + 4];
    uint32_t last = 0;
    for (int i = 0; i < ft::Intern::kCap + 4; ++i) last = full.id(&pool[i]);
    check(last == ft::Intern::kCap, "a full table answers its cap, not a fresh id");
}

void testViews() {
    ft::ViewTable v;
    int a, b, c;
    check(v.eyeOf(&a) == 0 && v.eyeOf(&b) == 1 && v.eyeOf(&a) == 0 && v.eyeOf(&c) == 2, "views number by first appearance");
    v.reset();
    check(v.n == 0 && v.eyeOf(&b) == 0, "a new frame renumbers");
}

void testWindow() {
    ft::WindowMachine w;
    check(w.step(false, 1, 0) == ft::WindowMachine::None && !w.capturing, "closed stays closed");
    check(w.step(true, 10, 1000) == ft::WindowMachine::Opened && w.capturing && w.open && w.windows == 1, "the zoom window opens a capture");
    check(w.step(true, 11, 1500) == ft::WindowMachine::None && w.capturing, "an open window keeps capturing");
    check(w.wantsDetail(10) && w.wantsDetail(13) && !w.wantsDetail(14) && w.wantsDetail(10 + ft::kDetailEvery), "detail lines: the first frames, then one in N");
    check(w.step(true, 900, 1000 + ft::kWindowMs + 1) == ft::WindowMachine::Closed && !w.capturing && w.open,
          "the capture ends after kWindowMs while the zoom window stays open");
    check(w.step(true, 901, 20000) == ft::WindowMachine::None, "no second close event");
    check(w.step(false, 902, 20001) == ft::WindowMachine::None && !w.open, "the zoom window closing after its capture says nothing more");
    check(w.step(true, 1000, 30000) == ft::WindowMachine::Opened && w.windows == 2, "the next zoom opens window 2");
    check(w.step(false, 1001, 30100) == ft::WindowMachine::Closed && !w.capturing, "the zoom window closing ends a running capture");
    for (uint32_t i = 0; i < 10; ++i) {
        w.step(true, 2000 + i * 10, 40000 + i * 1000);
        w.step(false, 2001 + i * 10, 40100 + i * 1000);
    }
    check(w.windows == ft::kMaxWindows, "windows are capped per session");
    check(w.step(true, 9000, 90000) == ft::WindowMachine::None && !w.capturing && w.open, "past the cap the window opens without a capture (the candidate still follows `open`)");
}

ft::Slot buf(const void* view, const void* res, uint32_t bytes) {
    ft::Slot s;
    s.view = view;
    s.res = res;
    s.isBuffer = true;
    s.bytes = bytes;
    return s;
}

void testSubstitutionRule() {
    int v0[8], v1[8], r0[8], r1[8];
    ft::Slot a[8], b[8];
    a[0].view = &v0[0]; a[0].res = &r0[0];            // textures: never taken
    b[0].view = &v1[0]; b[0].res = &r1[0];
    a[2] = buf(&v0[2], &r0[2], 10656); b[2] = buf(&v1[2], &r1[2], 10656);     // the list: taken
    a[3] = buf(&v0[3], &r0[3], 64);    b[3] = buf(&v1[3], &r1[3], 128);       // different sizes: not taken
    a[4] = buf(&v0[4], &r0[4], 64);    b[4] = buf(&v1[4], &r0[4], 64);        // the same resource already: not taken
    a[5] = buf(&v0[5], &r0[5], 64);                                           // unbound in the second view: not taken
    b[6] = buf(&v1[6], &r1[6], 64);                                           // unbound in the first view: not taken
    check(ft::substitutionMask(a, b) == 0x04u, "only a bound, equal-size buffer that differs between the views is taken");
    a[7] = buf(&v0[7], &r0[7], 16); b[7] = buf(&v1[7], &r1[7], 16);
    check(ft::substitutionMask(a, b) == 0x84u, "every such slot is taken");
    ft::Slot none[8];
    check(ft::substitutionMask(none, none) == 0, "nothing bound, nothing taken");
}

void testLineAndTallies() {
    ft::Rec r;
    r.frame = 812;
    r.eye = 1;
    r.occ = 2;
    r.cs = ft::kCsSmall;
    r.off = 12;
    r.argsId = 7;
    r.srvId[2] = 14; r.srvKind[2] = 1; r.srvBytes[2] = 10656;
    r.srvId[0] = 3; r.srvKind[0] = 2;
    r.uavId[0] = 20; r.uavId[1] = 21;
    r.groups[0] = 311; r.groups[1] = 1; r.groups[2] = 1; r.haveGroups = true;
    char line[512];
    ft::formatRec(line, sizeof(line), r);
    check(std::strcmp(line, "fss tiles: f=812 eye=1 cs=EB0245DE0BB23BB6 groups=(311,1,1) occ=2 args=A7+12 srv=[T3,-,B14:10656,-,-,-,-,-] uav=[20,21]") == 0,
          "the line has the documented shape");
    r.substituted = true;
    r.subMask = 0x04;
    r.haveGroups = false;
    r.eye = 3;
    ft::formatRec(line, sizeof(line), r);
    check(std::strstr(line, "groups=(unread)") && std::strstr(line, "eye=2 ") && std::strstr(line, "lockstep=substituted(slots 0x04)"),
          "unread groups, a clamped eye and the substitution are all visible");

    ft::Range g;
    g.add(10, 1, 1);
    g.add(40, 1, 3);
    g.add(20, 1, 1);
    check(g.n == 3 && g.lo == 10 && g.hi == 40 && g.sum == 70 && g.yzMax == 3, "the group-count range");

    ft::Rec a, b;
    a.argsId = 1; b.argsId = 2;
    a.srvId[2] = 5; b.srvId[2] = 6;
    a.srvId[4] = 9; b.srvId[4] = 9;
    a.uavId[0] = 30; b.uavId[0] = 31;
    ft::Share s;
    ft::accumulateShare(s, a, b);
    b.argsId = 1;
    ft::accumulateShare(s, a, b);
    check(s.pairs == 2 && s.argsSame == 1 && s.slotBoth[2] == 2 && s.slotSame[2] == 0 && s.slotBoth[4] == 2 && s.slotSame[4] == 2 && s.uavSame == 0,
          "the sharing tally says what the two views have in common");
}

void testV2Pure() {
    check(ft::fnv64("a", 1) == 0xaf63dc4c8601ec8cull && ft::fnv64("", 0) == 0xcbf29ce484222325ull, "FNV-1a 64 matches its reference values");
    uint32_t buf[2 * 12] = {};
    buf[2 * 3] = 0x10; buf[2 * 3 + 1] = 0x11;      // entry 3
    buf[2 * 5] = 0x20;                              // entry 5
    buf[2 * 6 + 1] = 0x30;                          // entry 6 (second word only)
    buf[2 * 7] = 0x40; buf[2 * 7 + 1] = 0x41;      // 7
    buf[2 * 8] = 0x50;                              // 8
    buf[2 * 11] = 0x60; buf[2 * 11 + 1] = 0x61;    // 11
    ft::ListSummary ls = ft::summarizeList(buf, sizeof(buf));
    check(ls.entries == 12 && ls.nonzero == 6 && ls.lastNonzero == 11, "the list tally: entries, non-zero entries, last non-zero index");
    check(ls.firstN == 4 && ls.first[0][0] == 0x10 && ls.first[0][1] == 0x11 && ls.first[3][0] == 0x40, "the first four non-zero entries");
    check(ls.lastN == 4 && ls.last[0][1] == 0x30 && ls.last[1][0] == 0x40 && ls.last[2][0] == 0x50 && ls.last[3][0] == 0x60 && ls.last[3][1] == 0x61,
          "the last four non-zero entries, in index order");
    uint32_t two[4] = {1, 2, 3, 4};
    ft::ListSummary l2 = ft::summarizeList(two, sizeof(two));
    check(l2.firstN == 2 && l2.lastN == 2 && l2.last[0][0] == 1 && l2.last[1][0] == 3, "fewer than four entries: first and last agree");
    ft::ListSummary l0 = ft::summarizeList(two, 0);
    check(l0.entries == 0 && l0.nonzero == 0 && l0.lastNonzero == -1 && l0.hash == 0xcbf29ce484222325ull, "an empty list is an empty tally");
    char line[400];
    ft::formatList(line, sizeof(line), "L", ls);
    check(std::strstr(line, "L n=6/12 last@11 hash=") != nullptr && std::strstr(line, "first=[10:11,20:0,0:30,40:41]") != nullptr &&
              std::strstr(line, "last=[0:30,40:41,50:0,60:61]") != nullptr,
          "the list line shows the tally, the hash and both ends");
    check(ft::splitPercent(382, 127) > 24.9 && ft::splitPercent(382, 127) < 25.0 && ft::splitPercent(509, 0) == 0.0 && ft::splitPercent(0, 0) == 0.0,
          "the split is small over large plus small, in percent");
    uint8_t info[16] = {0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3};
    ft::formatInfo(line, sizeof(line), "eye0", info, sizeof(info));
    check(std::strstr(line, "eye0 bytes=16 nonzero=3 hash=") != nullptr, "the tile-info line counts non-zero bytes");

    ft::WatchTable w;
    int a, b;
    w.add(&a, ft::Role::Out, 0);
    w.add(&a, ft::Role::List, 1);
    w.add(&b, ft::Role::Info, 1);
    w.add(nullptr, ft::Role::Args, 0);
    check(w.n == 2 && w.find(&a) == 0 && w.role[0] == ft::Role::Out && w.find(&b) == 1 && w.find(line) == -1,
          "the watch table keeps the first role of a buffer, ignores null, and finds by identity");
    check(ft::regionOf(0, 0) == 0 && ft::regionOf(1, 2) == 5 && ft::kRegions == 6, "regions: two eyes by three roles");
}

void testBand() {
    // A list as the engine writes it: valid cells then the (17, 17) filler.
    uint32_t list[2 * 20];
    for (int i = 0; i < 20; ++i) { list[2 * i] = 17; list[2 * i + 1] = 17; }
    const uint32_t cells[5][2] = {{13, 16}, {7, 9}, {0, 0}, {15, 9}, {3, 12}};
    for (int i = 0; i < 5; ++i) { list[2 * i] = cells[i][0]; list[2 * i + 1] = cells[i][1]; }
    ft::Band b = ft::bandOf(list, sizeof(list));
    check(b.valid == 5 && b.minY == 0, "bandOf counts the valid cells, the (0, 0) cell included, and finds the smallest row");
    check(ft::bandOf(list, 0).valid == 0, "an empty list has no cells");
    check(ft::rowOfLarge(136) == 9.0 && ft::rowOfLarge(0) == 17.0 && ft::rowOfLarge(153) == 8.0, "the large list's starting row from its size");

    ft::BandWatch w;
    check(w.feed(136, 136) == ft::BandWatch::None, "the first frame is never an event");
    check(w.feed(136, 152) == ft::BandWatch::None, "the eyes disagreeing by a row is not an event by itself");
    check(w.feed(136, 152) == ft::BandWatch::None, "steady is quiet");
    check(w.feed(136, 150) == ft::BandWatch::None && w.feed(120, 140) == ft::BandWatch::None, "steps under a row are quiet");
    check(w.feed(100, 140) == ft::BandWatch::RowStep, "a step of a whole row in one eye is a row event");
    check(w.feed(0, 0) == ft::BandWatch::AllSmall, "a large list going to zero is the all-small event");
    check(w.feed(0, 0) == ft::BandWatch::None, "and it is said once");
    check(w.lo[0] == 0 && w.hi[0] == 136 && w.lo[1] == 0 && w.hi[1] == 152 && w.maxStep[1] == 140 && w.maxStep[0] == 100 && w.frames == 8,
          "the watch keeps the ranges and the largest step for the window summary");
    ft::BandWatch w2;
    w2.feed(136, 152);
    check(w2.feed(0, 140) == ft::BandWatch::AllSmall, "one eye going to zero is enough, whatever the other does");
    ft::BandWatch w3;
    w3.feed(50, 50);
    check(w3.feed(50, 67) == ft::BandWatch::RowStep && w3.maxStep[1] == 17, "exactly one row (17 cells) is an event");

    ft::BandRec r;
    r.frame = 34170;
    r.large[0] = 136; r.smallN[0] = 153; r.minY[0] = 9;
    r.large[1] = 152; r.smallN[1] = 137; r.minY[1] = 8;
    char line[400];
    ft::formatBand(line, sizeof(line), r);
    check(std::strcmp(line, "fss tiles: band f=34170 eye0 L=136 (row 9.00, minY 9) S=153 | eye1 L=152 (row 8.06, minY 8) S=137 | own row gap 0.94") == 0,
          "the band line shows each eye's large list, boundary row and the gap");
    r.ranEye0 = true;
    ft::formatBand(line, sizeof(line), r);
    check(std::strstr(line, "| ran: eye1 drew eye0's lists (row 9.00)") != nullptr,
          "under the tiles candidate the line says eye 1 drew eye 0's lists, at eye 0's row");
    check(ft::kBandBefore == 10 && ft::kBandAfter == 60 && ft::kMaxAutoRuns == 9 && ft::kGrid == 17, "the caps the task named");
}

void testV4Pure() {
    check(ft::bytesPerPixel(10) == 8 && ft::bytesPerPixel(11) == 8 && ft::bytesPerPixel(2) == 16 && ft::bytesPerPixel(26) == 4 &&
              ft::bytesPerPixel(28) == 4 && ft::bytesPerPixel(41) == 4 && ft::bytesPerPixel(61) == 1 && ft::bytesPerPixel(87) == 4,
          "bytes per pixel of the colour formats the samples read");
    check(ft::bytesPerPixel(19) == 0 && ft::bytesPerPixel(20) == 0 && ft::bytesPerPixel(0) == 0 && ft::bytesPerPixel(71) == 0 &&
              ft::bytesPerPixel(89) == 0,
          "depth-stencil, block-compressed and unknown formats are not sampled");
    check(ft::nzChar(0, 256) == '.' && ft::nzChar(256, 256) == '#' && ft::nzChar(1, 256) == '1' && ft::nzChar(128, 256) == '5' &&
              ft::nzChar(255, 256) == '8' && ft::nzChar(5, 0) == '.',
          "one character per sampled box: none, all, or the share between");
    uint8_t px[8] = {0, 0, 1, 0, 0, 0, 0, 9};
    check(ft::countNonzero(px, 8) == 2 && ft::countNonzero(px, 0) == 0, "non-zero byte count");
    char grid[100];
    const int n = ft::formatGrid(grid, sizeof(grid), [](uint32_t x, uint32_t y) { return static_cast<char>(x == y ? '#' : '.'); });
    check(n == 71 && std::strcmp(grid, "#......./.#....../..#...../...#..../....#.../.....#../......#./.......#") == 0,
          "the 8 x 8 grid is eight rows of eight, joined by slashes");
    char tiny[10];
    ft::formatGrid(tiny, sizeof(tiny), [](uint32_t, uint32_t) { return 'x'; });
    check(std::strlen(tiny) < 10, "a short buffer is cut, never overrun");
    check(ft::kRevealFrames == 600 && ft::kRunOffsets[0] == 0 && ft::kRunOffsets[1] == 30 && ft::kRunOffsets[2] == 270 && ft::kRunCount == 3 && ft::kFillEvery == 5 && ft::kFingerEvery == 10 && ft::kTileGrid == 8 &&
              ft::kTilePx == 16 && ft::kBufRegions * ft::kBufRegionBytes == 4096,
          "the numbers the task named");
    check(std::strcmp(ft::roleName(ft::Role::Gbuf8), "t8") == 0 && std::strcmp(ft::roleName(ft::Role::Gbuf11), "t11") == 0 && ft::kRoles == 14 &&
              static_cast<int>(ft::Role::Gbuf11) == 9,
          "the write watch names the G-buffer textures t8-t11");
    check(std::strcmp(ft::roleName(ft::Role::Ring0), "ring_t0") == 0 && std::strcmp(ft::roleName(ft::Role::Ring3), "ring_t3") == 0 &&
              static_cast<int>(ft::Role::Ring3) == 13,
          "and the ring quad's own inputs ring_t0-t3");
    check(ft::kCaptureSlots == 12 && ft::kSrvSlots == 8 && ft::kFpItems == 7 && ft::kFpSlots[0] == 0 && ft::kFpSlots[1] == 3 && ft::kFpSlots[2] == 4 &&
              ft::kFpSlots[3] == 8 && ft::kFpSlots[6] == 11,
          "the dispatch's slots 0-11 are read, and the fingerprint covers 0, depth 3, stencil 4 and the G-buffer 8-11");
}

std::string slurp(const std::string& path);

void testRingCore(const std::string& root) {
    namespace rh = edvr::ringhook;
    check(rh::kRingPassRva == 0x36558A0u && rh::kPrologueBytes == 16, "the ring pass entry and the prologue length are the RE's");
    const uint8_t good[16] = {0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};
    uint8_t bad[16];
    std::memcpy(bad, good, 16);
    bad[15] = 0x01;
    check(rh::prologueMatches(good, 16) && !rh::prologueMatches(bad, 16) && !rh::prologueMatches(good, 15) && !rh::prologueMatches(nullptr, 16),
          "the prologue check accepts the bytes read from the exe and nothing else");
    const std::string doc = slurp(root + "/docs/fss-scanner.md");
    check(doc.find("0x36558a0, `40 55 57 41 54 41 55 41 56 48 81 EC 90 00 00 00`") != std::string::npos ||
              doc.find("`FUN_1436558a0` 0x36558a0, `40 55 57 41 54 41 55 41 56 48 81 EC 90 00 00 00`") != std::string::npos,
          "the prologue is the one in the RE's hook-point table (docs/fss-scanner.md)");
    check(rh::kCameraId == 0x8e8 && rh::kCameraSettings == 0x8f8 && rh::kSettingsRole == 0x10 && rh::kDataBodyMode == 0x104 && rh::kRingScratch == 0x6b0 &&
              rh::kRingAlpha == 0x718 && rh::kRingActive == 0x720 && rh::kRingRenderer == 0x48 && rh::kVtableGetCamera == 0xa8 && rh::kVtableGetId == 0x70,
          "the offsets are the RE's table");

    rh::TupleTable t;
    rh::Call a;
    a.camera = 3; a.role = 1; a.pass = 7; a.mode = 2; a.scratch = true;
    rh::Call b = a;
    b.role = 2;
    b.alpha = 0.5f;
    check(t.add(a) && !t.add(a) && t.add(b) && !t.add(b) && t.used == 2 && t.e[0].n == 2 && t.e[1].n == 2, "the tuple table counts combinations, new ones said once");
    rh::Call c2 = a;
    c2.alpha = 0.9f;
    check(!t.add(c2) && t.used == 2, "the alpha is not part of the combination");
    rh::TupleTable full;
    for (int i = 0; i < 40; ++i) { rh::Call x; x.camera = i; full.add(x); }
    check(full.used == rh::TupleTable::kCap && full.overflow == 8, "a full table counts what it cannot hold");
    char line[300];
    rh::formatCall(line, sizeof(line), 13152, 4, a, "new combination");
    check(std::strcmp(line, "fss tiles: ring f=13152 call=4 cam=3 role=1 pass=7 mode=2 ring+0x6b0=set alpha=0.000 dir=0 active=0 (new combination)") == 0,
          "the call line has the documented shape");
    char tup[300];
    rh::formatTuples(tup, sizeof(tup), t);
    check(std::strcmp(tup, "cam=3 role=1 pass=7 mode=2 6b0=set x3; cam=3 role=2 pass=7 mode=2 6b0=set x2") == 0, "the combinations line");
    rh::formatTuples(tup, 20, t);
    check(std::strlen(tup) < 20, "a short buffer is cut, never overrun");
}

void testLedger() {
    auto stat = [](char kind, uint32_t count, uint32_t shape, uint64_t vs, uint64_t ps, uint32_t n, uint32_t inst, uint32_t sig, uint32_t vw, uint32_t vh) {
        ft::DrawStat d;
        d.k.kind = kind; d.k.count = count; d.k.rtShape = shape; d.k.vs = vs; d.k.ps = ps;
        d.n = n; d.inst = inst; d.srvSig = sig; d.vpW = vw; d.vpH = vh;
        return d;
    };
    const uint64_t V1 = 0x1111111122222222ull, P1 = 0x3333333344444444ull, V2 = 0x5555555566666666ull, P2 = 0x7777777788888888ull;
    ft::DrawStat e0[4], e1[4];
    e0[0] = stat('X', 100, 1, V1, P1, 62, 62, 7, 2016, 2016);
    e1[0] = stat('X', 100, 1, V1, P1, 62, 62, 7, 2016, 2016);
    e0[1] = stat('N', 4, 1, V2, P2, 1, 1, 9, 2016, 2016);        // only in eye 0
    e0[2] = stat('X', 7, 1, V2, P1, 3, 30, 5, 2016, 2016);       // instances and inputs and viewport differ
    e1[1] = stat('X', 7, 1, V2, P1, 3, 36, 6, 1008, 1008);
    e1[2] = stat('D', 3, 1, V1, P2, 2, 2, 1, 2016, 2016);        // only in eye 1
    ft::DiffItem items[ft::kDiffCap];
    const uint32_t n = ft::diffEyes(e0, 3, e1, 3, items, ft::kDiffCap);
    uint32_t cnt[6] = {};
    for (uint32_t i = 0; i < n; ++i) ++cnt[items[i].what];
    check(n == 5 && cnt[ft::DiffItem::Only0] == 1 && cnt[ft::DiffItem::Only1] == 1 && cnt[ft::DiffItem::Instances] == 1 && cnt[ft::DiffItem::Inputs] == 1 &&
              cnt[ft::DiffItem::Viewport] == 1 && cnt[ft::DiffItem::Draws] == 0,
          "the eye diff finds the draw only one eye has, and the instances, inputs and viewport of a draw both have");
    check(ft::diffEyes(e0, 1, e1, 1, items, ft::kDiffCap) == 0, "identical lists differ in nothing");
    check(ft::diffEyes(e0, 0, e1, 0, items, ft::kDiffCap) == 0, "two empty lists differ in nothing");
    ft::DrawStat f0 = stat('X', 100, 1, V1, P1, 62, 62, 7, 2016, 2016), f1 = stat('X', 100, 1, V1, P1, 61, 62, 7, 2016, 2016);
    check(ft::diffEyes(&f0, 1, &f1, 1, items, ft::kDiffCap) == 1 && items[0].what == ft::DiffItem::Draws && items[0].a == 62 && items[0].b == 61, "a draw one eye runs 61 times and the other 62");
    char line[800];
    ft::diffEyes(e0, 3, e1, 3, items, ft::kDiffCap);
    ft::formatDiff(line, sizeof(line), 13152, "pair=2016x2016 fmt28", items, n);
    check(std::strstr(line, "fss tiles: eyediff f=13152 pair=2016x2016 fmt28 only-eye0: 55555555/77777777 N n=4 x1") != nullptr,
          "the line starts as documented: eyediff, the frame, the pair, then the categories");
    check(std::strstr(line, "| only-eye1:") != nullptr && std::strstr(line, "| instances differ:") != nullptr && std::strstr(line, "| inputs differ:") != nullptr &&
              std::strstr(line, "| viewport differ:") != nullptr && std::strstr(line, "2016x2016 vs 1008x1008") != nullptr,
          "each category is named once, separated by bars");
    char tiny[40];
    ft::formatDiff(tiny, sizeof(tiny), 1, "p", items, n);
    check(std::strlen(tiny) < 40, "a short buffer is cut, never overrun");

    ft::Ranking r;
    ft::DiffItem a;
    a.what = ft::DiffItem::Only0; a.k = e0[1].k; a.a = 1;
    ft::DiffItem b;
    b.what = ft::DiffItem::Instances; b.k = e0[2].k; b.a = 30; b.b = 36;
    r.add(a); r.add(a); r.add(a); r.add(b); r.add(b);
    uint32_t idx[8];
    const uint32_t m = r.order(idx, 8);
    check(m == 2 && r.e[idx[0]].what == ft::DiffItem::Only0 && r.e[idx[0]].frames == 3 && r.e[idx[1]].frames == 2 && r.e[idx[1]].total == 12,
          "the ranking puts the draw that differed in most frames first and sums the differences");
    ft::Ranking full;
    for (uint32_t i = 0; i < ft::kRankCap + 5; ++i) { ft::DiffItem d; d.k.count = i; full.add(d); }
    check(full.used == ft::kRankCap && full.dropped == 5, "a full ranking counts what it cannot hold");

    check(ft::isRingDraw('N', 4, 0x7E38A6AA1269C901ull, 0) && ft::isRingDraw('X', 14, 0x0357BBB2DEE43C1Full, 0) && ft::isRingDraw('N', 3, 0x53211E8C072CD02Eull, 0) &&
              ft::isRingDraw('N', 4, 0, 0x7CECABDE34FFBE9Eull) && ft::isRingDraw('X', 13248, 0, 0) && !ft::isRingDraw('N', 13248, 0, 0) && !ft::isRingDraw('X', 100, 1, 2),
          "the ring family: the 332753 vertex hashes, the quad's pixel shader, the 13248-index mesh, and nothing else");
    check(ft::kLedgerFrames == 60 && ft::kReduceLevel == 2 && ft::kMaxEyediffLines == 80, "the numbers the task named");
}

void testGate() {
    ft::ZoomGate gate;
    check(gate.step(100, 0, true, true) == ft::ZoomGate::Idle, "a closed gate is idle");
    check(gate.signal(100, 6) == ft::ZoomGate::NoChrome && !gate.open && gate.signal(100, 0xFFFFFFFFu) == ft::ZoomGate::NoChrome,
          "a jump with the chrome gone (more than 5 frames, or never) opens nothing");
    check(gate.signal(100, 5) == ft::ZoomGate::Accepted && gate.open && gate.startFrame == 100, "a jump with the chrome held opens the window");
    check(gate.step(101, 0, false, false) == ft::ZoomGate::Open && gate.step(102, 3, true, true) == ft::ZoomGate::Open, "an unknown focus does not close it, and neither does a focus that agrees");
    check(gate.signal(300, 0) == ft::ZoomGate::Accepted && gate.startFrame == 300, "a second accepted signal (the arrival after the departure) restarts the clock");
    check(gate.step(300 + ft::kGateWindowFrames, 0, true, true) == ft::ZoomGate::Open && gate.step(300 + ft::kGateWindowFrames + 1, 0, true, true) == ft::ZoomGate::Expired && !gate.open,
          "it lives 600 frames after the last accepted signal");
    ft::ZoomGate g2;
    g2.signal(10, 0);
    check(g2.step(11, 0, true, false) == ft::ZoomGate::FocusEnded && !g2.open, "the game saying FSS focus ended closes it at once");
    ft::ZoomGate g3;
    g3.signal(10, 0);
    check(g3.step(20, ft::kChromeDropFrames, true, true) == ft::ZoomGate::Open && g3.step(21, ft::kChromeDropFrames + 1, true, true) == ft::ZoomGate::ChromeDropped && !g3.open,
          "the chrome gone for more than 45 frames closes it");
    check(ft::chromeHeld(0) && ft::chromeHeld(5) && !ft::chromeHeld(6) && !ft::chromeHeld(0xFFFFFFFFu), "the chrome is held within five frames");

    // The timeline of flight e05b6be6 (docs/fss-scanner.md, "v6 flown"): the scanner screen up from f11890 with the chrome matching every frame, the zoom's departure
    // jump at f13006 and its arrival jump at f13273, the GuiFocus 9 read only 6 frames after the arrival (late: the fast poll did not run), the exit jump at f15096 after the
    // chrome stopped at f15090, with the stale focus still reading 9 until f15100.
    struct Event { uint32_t frame; bool jump; };
    const Event jumps[] = {{13006, true}, {13273, true}, {15096, true}};
    ft::ZoomGate z;
    uint32_t openedAt = 0, openedAtExit = 0, closedAt = 0;
    ft::ZoomGate::Close why = ft::ZoomGate::Idle;
    uint32_t lastChrome = 0;
    int refused = 0, accepted = 0;
    for (uint32_t f = 11890; f <= 15200; ++f) {
        if (f < 15090) lastChrome = f;                                   // the chrome matches every frame until the scanner is left
        const uint32_t ago = lastChrome ? f - lastChrome : 0xFFFFFFFFu;
        const bool focusKnown = f >= 13279 && f < 15100 + 1 ? true : f > 15100;   // read at f13279, and again (ended) after f15100
        const bool focus = f >= 13279 && f <= 15100;
        const bool wasOpen = z.open;
        for (const Event& e : jumps) {
            if (e.frame != f) continue;
            if (z.signal(f, ago) == ft::ZoomGate::Accepted) ++accepted; else ++refused;
        }
        const ft::ZoomGate::Close c = z.step(f, ago, focusKnown, focus);
        if (z.open && !wasOpen && !openedAt) openedAt = f;
        if (z.open && f >= 15096 && !wasOpen) openedAtExit = f;
        if (c >= ft::ZoomGate::Expired && !closedAt) { closedAt = f; why = c; }
    }
    check(openedAt == 13006 && openedAtExit == 0 && z.startFrame == 13273, "with the departure flash counted too, the window opens at that first jump while the scanner is up, the arrival restarts its clock, and the exit jump opens nothing");
    check(accepted == 2 && refused == 1, "the two jumps with the chrome held are accepted and the exit jump, with the chrome gone, is refused");
    check(closedAt == 13273 + ft::kGateWindowFrames + 1 && why == ft::ZoomGate::Expired, "the window lives 600 frames from the arrival and is gone long before the exit");
    // The same flight with only the arrival and the exit jumps (the departure flash not counted): the window opens at the arrival.
    ft::ZoomGate arr;
    uint32_t arrOpenedAt = 0, arrOpenedAtExit = 0;
    {
        uint32_t lc2 = 0;
        for (uint32_t f = 13200; f <= 15200; ++f) {
            if (f < 15090) lc2 = f;
            const uint32_t ago = lc2 ? f - lc2 : 0xFFFFFFFFu;
            const bool was = arr.open;
            if (f == 13273 || f == 15096) arr.signal(f, ago);
            arr.step(f, ago, f >= 13279, f >= 13279 && f <= 15100);
            if (arr.open && !was && !arrOpenedAt) arrOpenedAt = f;
            if (arr.open && !was && f >= 15096) arrOpenedAtExit = f;
        }
    }
    check(arrOpenedAt == 13273 && arrOpenedAtExit == 0 && !arr.open, "replaying e05b6be6's arrival jump (late FSS focus) and exit jump: the window opens at the arrival, not at the exit");
    // The exit with no arrival signal at all (the fast poll missed everything): nothing opens.
    ft::ZoomGate none;
    int acc = 0;
    for (uint32_t f = 15090; f <= 15100; ++f) {
        const uint32_t ago = f >= 15091 ? f - 15090 : 0;
        if (f == 15096 && none.signal(f, ago) == ft::ZoomGate::Accepted) ++acc;
        none.step(f, ago, true, true);
    }
    check(acc == 0 && !none.open, "an exit jump alone, with the chrome stopped six frames earlier, opens nothing");
    // Leaving the scanner while a window is open closes it with the reason.
    ft::ZoomGate leave;
    leave.signal(13273, 0);
    ft::ZoomGate::Close lc = ft::ZoomGate::Idle;
    uint32_t leftAt = 0;
    for (uint32_t f = 13274; f < 13500 && lc != ft::ZoomGate::ChromeDropped; ++f) {
        const uint32_t ago = f < 13400 ? 0 : f - 13399;   // the chrome stops after f13399
        lc = leave.step(f, ago, true, true);
        if (lc == ft::ZoomGate::ChromeDropped) leftAt = f;
    }
    check(lc == ft::ZoomGate::ChromeDropped && leftAt == 13399 + ft::kChromeDropFrames + 1, "leaving the scanner closes an open window once the chrome has been gone 45 frames");
    check(std::strcmp(ft::closeText(ft::ZoomGate::ChromeDropped), "the scanner chrome dropped (the scanner was left)") == 0 &&
              std::strcmp(ft::closeText(ft::ZoomGate::FocusEnded), "the game says FSS focus ended") == 0,
          "the close reasons read as the log says them");
}

void testKeyParsing() {
    edvr::EyeSync t = edvr::eyeSyncParse("tiles");
    check(t.tiles && t.recognised && t.healMode == 0 && !t.lockstep && !t.steady && !t.any(), "tiles is its own value: no heal, no composite freeze");
    edvr::EyeSync tu = edvr::eyeSyncParse("Tiles");
    check(tu.tiles, "case is folded");
    edvr::EyeSync l = edvr::eyeSyncParse("lockstep");
    check(l.lockstep && !l.tiles, "the legacy spelling lockstep still means the composite freeze");
    edvr::EyeSync on = edvr::eyeSyncParse("on");
    check(on.healMode == 1 && on.lockstep && !on.tiles, "on is the shipped pair and nothing more");
    edvr::EyeSync off = edvr::eyeSyncParse("off");
    check(!off.tiles && !off.any(), "off is stock");
    check(t.needsFssSignals() && on.needsFssSignals() && !off.needsFssSignals(),
          "the FSS key watcher and Status poll run for tiles as for on, and not for off (flight f9f62bbc: tiles left them off and its zoom window never opened)");
    edvr::EyeSync mir = edvr::eyeSyncParse("mirror");
    check(mir.healMode == 2 && !mir.tiles, "mirror is unchanged");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 2. The restore, on a fake context

struct FakeOps {
    void* state[8] = {};
    int released = 0;
    int dispatched = 0;
    void* seenAtDispatch[8] = {};
    void* seenArgs = nullptr;
    uint32_t seenOff = 0;
    void* getSrv(uint32_t i) { return state[i]; }
    void setSrv(uint32_t i, void* v) { state[i] = v; }
    void release(void*) { ++released; }
    void dispatchIndirect(void* args, uint32_t off) {
        ++dispatched;
        std::memcpy(seenAtDispatch, state, sizeof(state));
        seenArgs = args;
        seenOff = off;
    }
};

void testFakeRestore() {
    int own[8], first[8], args0;
    FakeOps o;
    for (int i = 0; i < 8; ++i) o.state[i] = &own[i];
    o.state[5] = nullptr;                       // an unbound slot must come back unbound
    void* before[8];
    std::memcpy(before, o.state, sizeof(before));
    void* views[8] = {};
    for (int i = 0; i < 8; ++i) views[i] = &first[i];
    ft::lockstepDispatch(o, views, 0x2C, &args0, 12);   // slots 2, 3, 5
    check(o.dispatched == 1 && o.seenArgs == &args0 && o.seenOff == 12, "the dispatch got the first view's arguments and offset");
    check(o.seenAtDispatch[2] == &first[2] && o.seenAtDispatch[3] == &first[3] && o.seenAtDispatch[5] == &first[5], "the dispatch saw the first view's bindings");
    check(o.seenAtDispatch[0] == &own[0] && o.seenAtDispatch[1] == &own[1] && o.seenAtDispatch[4] == &own[4], "slots outside the mask kept the second view's own");
    check(std::memcmp(o.state, before, sizeof(before)) == 0, "every slot is back as it was, the unbound one unbound");
    check(o.released == 3, "each saved reference was released once");

    // The hook's fault path: swapIn, the dispatch never returns, swapBack runs from outside.
    FakeOps p;
    for (int i = 0; i < 8; ++i) p.state[i] = &own[i];
    ft::Swap sw;
    ft::swapIn(p, sw, views, 0xFF);
    check(sw.swapped == 0xFF && p.state[7] == &first[7], "swapIn takes the whole mask");
    ft::swapBack(p, sw);
    bool same = true;
    for (int i = 0; i < 8; ++i) same = same && p.state[i] == &own[i];
    check(same && sw.swapped == 0, "swapBack from outside restores everything");
    ft::swapBack(p, sw);
    check(p.released == 8, "a second swapBack is a no-op");
    FakeOps q;
    ft::Swap sw2;
    void* none[8] = {};
    ft::swapIn(q, sw2, none, 0xFF);
    check(sw2.swapped == 0, "a mask over slots with no first-view binding swaps nothing");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 3. The same on WARP, through the production ops

const char* kTileHlsl = R"(
StructuredBuffer<uint> list : register(t2);
Texture2D<float4> tex : register(t0);
RWByteAddressBuffer outBuf : register(u0);
[numthreads(1,1,1)]
void main(uint3 g : SV_GroupID) {
    outBuf.Store(g.x * 4, list[0] + g.x);
}
)";

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
};

ComPtr<ID3D11Buffer> makeArgs(ID3D11Device* d, uint32_t x) {
    uint32_t init[3] = {x, 1, 1};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 12;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
    ComPtr<ID3D11Buffer> b;
    d->CreateBuffer(&bd, &sd, &b);
    return b;
}

ComPtr<ID3D11ShaderResourceView> makeList(ID3D11Device* d, uint32_t first, ID3D11Buffer** outBuf) {
    uint32_t init[4] = {first, 0, 0, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
    ComPtr<ID3D11Buffer> b;
    d->CreateBuffer(&bd, &sd, &b);
    ComPtr<ID3D11ShaderResourceView> v;
    d->CreateShaderResourceView(b.Get(), nullptr, &v);
    if (outBuf) *outBuf = b.Detach();
    return v;
}

std::vector<uint32_t> readOut(Gpu& g, ID3D11Buffer* out) {
    D3D11_BUFFER_DESC bd{};
    out->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> st;
    g.dev->CreateBuffer(&bd, nullptr, &st);
    g.ctx->CopyResource(st.Get(), out);
    D3D11_MAPPED_SUBRESOURCE m{};
    std::vector<uint32_t> v(bd.ByteWidth / 4);
    if (SUCCEEDED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        std::memcpy(v.data(), m.pData, bd.ByteWidth);
        g.ctx->Unmap(st.Get(), 0);
    }
    return v;
}

void clearOut(Gpu& g, ID3D11UnorderedAccessView* uav) {
    const UINT z[4] = {0, 0, 0, 0};
    g.ctx->ClearUnorderedAccessViewUint(uav, z);
}

void testWarp(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(kTileHlsl, std::strlen(kTileHlsl), "fss_tiles_test", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err);
    check(SUCCEEDED(hr), "the stand-in tile shader compiles");
    if (FAILED(hr)) return;
    ComPtr<ID3D11ComputeShader> cs;
    d->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);

    ComPtr<ID3D11Buffer> args0 = makeArgs(d, 3), args1 = makeArgs(d, 5);
    ID3D11Buffer *list0b = nullptr, *list1b = nullptr;
    ComPtr<ID3D11ShaderResourceView> list0 = makeList(d, 1000, &list0b), list1 = makeList(d, 2000, &list1b);
    ComPtr<ID3D11Buffer> l0(list0b), l1(list1b);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    d->CreateTexture2D(&td, nullptr, &tex);
    ComPtr<ID3D11ShaderResourceView> texView;
    d->CreateShaderResourceView(tex.Get(), nullptr, &texView);

    D3D11_BUFFER_DESC ob{};
    ob.ByteWidth = 64;
    ob.Usage = D3D11_USAGE_DEFAULT;
    ob.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ob.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> out;
    d->CreateBuffer(&ob, nullptr, &out);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 16;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> uav;
    d->CreateUnorderedAccessView(out.Get(), &ud, &uav);

    ID3D11DeviceContext* c = g.ctx.Get();
    c->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[1] = {uav.Get()};
    c->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // The second view's own state, and what the first view's dispatch left recorded.
    ID3D11ShaderResourceView* own[8] = {texView.Get(), nullptr, list1.Get(), nullptr, nullptr, nullptr, nullptr, nullptr};
    c->CSSetShaderResources(0, 8, own);

    // Stock: the second view draws 5 groups from its own list (2000 + g).
    clearOut(g, uav.Get());
    c->DispatchIndirect(args1.Get(), 0);
    std::vector<uint32_t> stock = readOut(g, out.Get());
    bool stockOk = true;
    for (uint32_t i = 0; i < 5; ++i) stockOk = stockOk && stock[i] == 2000 + i;
    check(stockOk && stock[5] == 0, "stock: the second view's dispatch ran 5 groups over its own list");

    // The substitution: 3 groups over the FIRST view's list (1000 + g), arguments from the first view's buffer.
    ft::D3dOps ops;
    ops.ctx = c;
    void* views[8] = {};
    views[2] = list0.Get();
    ft::Slot e0[8], e1[8];
    e0[2] = buf(list0.Get(), l0.Get(), 16);
    e1[2] = buf(list1.Get(), l1.Get(), 16);
    e0[0].view = texView.Get(); e0[0].res = tex.Get();
    e1[0] = e0[0];
    const uint32_t mask = ft::substitutionMask(e0, e1);
    check(mask == 0x04u, "the rule picks exactly the list slot (the shared texture stays)");
    clearOut(g, uav.Get());
    ft::lockstepDispatch(ops, views, mask, args0.Get(), 0);
    std::vector<uint32_t> sub = readOut(g, out.Get());
    bool subOk = true;
    for (uint32_t i = 0; i < 3; ++i) subOk = subOk && sub[i] == 1000 + i;
    check(subOk && sub[3] == 0 && sub[4] == 0, "substituted: 3 groups (the first view's count) over the first view's list, nothing beyond");

    // The restore.
    ID3D11ShaderResourceView* after[8] = {};
    c->CSGetShaderResources(0, 8, after);
    bool restored = true;
    for (int i = 0; i < 8; ++i) restored = restored && after[i] == own[i];
    for (auto* v : after) if (v) v->Release();
    check(restored, "the context's compute SRVs are exactly the second view's own again (slot 2 included)");

    // And the next stock dispatch is stock.
    clearOut(g, uav.Get());
    c->DispatchIndirect(args1.Get(), 0);
    std::vector<uint32_t> again = readOut(g, out.Get());
    bool againOk = true;
    for (uint32_t i = 0; i < 5; ++i) againOk = againOk && again[i] == 2000 + i;
    check(againOk, "after the substitution the next dispatch is stock again");

    // A held reference is balanced: the list view's count returns to what it was.
    list1->AddRef();
    ULONG with = list1->Release();
    clearOut(g, uav.Get());
    ft::lockstepDispatch(ops, views, mask, args0.Get(), 0);
    list1->AddRef();
    ULONG after2 = list1->Release();
    check(with == after2, "no reference to the second view's list leaked or was dropped");

    // The fault path: swap in, restore from outside, no dispatch at all.
    ft::Swap sw;
    ft::swapIn(ops, sw, views, mask);
    ID3D11ShaderResourceView* mid = nullptr;
    c->CSGetShaderResources(2, 1, &mid);
    check(mid == list0.Get(), "swapped in");
    if (mid) mid->Release();
    ft::swapBack(ops, sw);
    ID3D11ShaderResourceView* back = nullptr;
    c->CSGetShaderResources(2, 1, &back);
    check(back == list1.Get(), "restored from outside the dispatch");
    if (back) back->Release();
}

// The staging twin the v2 samples copy into, on structured and raw buffers, and its reuse and replacement.
void testStagingLike(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    uint32_t init[8] = {11, 22, 33, 44, 55, 66, 77, 88};
    D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
    D3D11_BUFFER_DESC sb{};
    sb.ByteWidth = 32;
    sb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    sb.StructureByteStride = 8;
    ComPtr<ID3D11Buffer> structured;
    d->CreateBuffer(&sb, &sd, &structured);
    D3D11_BUFFER_DESC rb{};
    rb.ByteWidth = 32;
    rb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    rb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> raw;
    d->CreateBuffer(&rb, &sd, &raw);
    for (int pass = 0; pass < 2; ++pass) {
        ID3D11Buffer* src = pass ? raw.Get() : structured.Get();
        ComPtr<ID3D11Buffer> st;
        check(ft::ensureStagingLike(d, src, st.GetAddressOf()), pass ? "a staging twin of a raw buffer" : "a staging twin of a structured buffer");
        g.ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
        check(SUCCEEDED(hr) && std::memcmp(m.pData, init, sizeof(init)) == 0, "the copy reads back byte for byte");
        if (SUCCEEDED(hr)) g.ctx->Unmap(st.Get(), 0);
        ID3D11Buffer* before = st.Get();
        check(ft::ensureStagingLike(d, src, st.GetAddressOf()) && st.Get() == before, "a matching twin is reused");
    }
    D3D11_BUFFER_DESC big = sb;
    big.ByteWidth = 64;
    ComPtr<ID3D11Buffer> bigSrc;
    d->CreateBuffer(&big, nullptr, &bigSrc);
    ComPtr<ID3D11Buffer> st;
    ft::ensureStagingLike(d, structured.Get(), st.GetAddressOf());
    ID3D11Buffer* first = st.Get();
    first->AddRef();
    check(ft::ensureStagingLike(d, bigSrc.Get(), st.GetAddressOf()), "a differently sized source gets a new twin");
    D3D11_BUFFER_DESC have{};
    st->GetDesc(&have);
    check(have.ByteWidth == 64 && st.Get() != first, "the old twin was replaced by one of the new size");
    first->Release();
}

// The v4 sampling path on a real device: 8 x 8 boxes of 16 x 16 pixels copied into a 128 x 128 staging twin of a 256 x 256
// R16G16B16A16 texture whose lit region is known, and 16 x 256-byte regions of a 64 KB buffer.
void testSampling(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    const uint32_t W = 256, H = 256, bpp = 8;
    std::vector<uint8_t> img(W * H * bpp, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            if (x >= 128 && y >= 100 && y < 200) img[(y * W + x) * bpp + 1] = 7;   // a lit block, right of centre
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{img.data(), W * bpp, 0};
    ComPtr<ID3D11Texture2D> tex;
    d->CreateTexture2D(&td, &sd, &tex);
    D3D11_TEXTURE2D_DESC st{};
    st.Width = st.Height = ft::kTileGrid * ft::kTilePx; st.MipLevels = st.ArraySize = 1; st.Format = td.Format; st.SampleDesc.Count = 1;
    st.Usage = D3D11_USAGE_STAGING; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    check(SUCCEEDED(d->CreateTexture2D(&st, nullptr, &stage)), "a staging twin of the colour format");
    for (uint32_t gy = 0; gy < ft::kTileGrid; ++gy) {
        for (uint32_t gx = 0; gx < ft::kTileGrid; ++gx) {
            D3D11_BOX box{};
            box.left = (W - ft::kTilePx) * gx / (ft::kTileGrid - 1);
            box.top = (H - ft::kTilePx) * gy / (ft::kTileGrid - 1);
            box.right = box.left + ft::kTilePx; box.bottom = box.top + ft::kTilePx; box.back = 1;
            g.ctx->CopySubresourceRegion(stage.Get(), 0, gx * ft::kTilePx, gy * ft::kTilePx, 0, tex.Get(), 0, &box);
        }
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = g.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m);
    check(SUCCEEDED(hr), "the sampled boxes map");
    if (SUCCEEDED(hr)) {
        const uint8_t* p = static_cast<const uint8_t*>(m.pData);
        char grid[100];
        ft::formatGrid(grid, sizeof(grid), [&](uint32_t x, uint32_t y) {
            uint32_t nz = 0;
            for (uint32_t row = 0; row < ft::kTilePx; ++row) nz += ft::countNonzero(p + (y * ft::kTilePx + row) * m.RowPitch + x * ft::kTilePx * bpp, ft::kTilePx * bpp);
            return ft::nzChar(nz, ft::kTilePx * ft::kTilePx * bpp);
        });
        g.ctx->Unmap(stage.Get(), 0);
        // boxes start at (240 * gx / 7): the lit block (x >= 128, 100 <= y < 200) covers whole boxes of columns 4..7 (x0 = 137.. 240)
        // in rows 3..5 (y0 = 102.., 137, 171 ... 205 > 199 is out)
        check(std::strlen(grid) == 71 && grid[0] == '.' && grid[4 + 3 * 9] != '.' && grid[3 + 3 * 9] == '.',
              "the sampled grid shows the lit block where it is and nothing where it is not");
    }
    std::vector<uint8_t> big(65536, 0);
    for (uint32_t k = 5; k <= 6; ++k) {
        const uint64_t o = ((65536ull - ft::kBufRegionBytes) * k / (ft::kBufRegions - 1)) & ~3ull;
        for (uint32_t i = 0; i < ft::kBufRegionBytes; ++i) big[o + i] = 1;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 65536; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA bsd{big.data(), 0, 0};
    ComPtr<ID3D11Buffer> buf;
    d->CreateBuffer(&bd, &bsd, &buf);
    D3D11_BUFFER_DESC sbd{};
    sbd.ByteWidth = ft::kBufRegions * ft::kBufRegionBytes; sbd.Usage = D3D11_USAGE_STAGING; sbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> sbuf;
    check(SUCCEEDED(d->CreateBuffer(&sbd, nullptr, &sbuf)), "a staging buffer for the regions");
    for (uint32_t i = 0; i < ft::kBufRegions; ++i) {
        const uint64_t off = (static_cast<uint64_t>(bd.ByteWidth) - ft::kBufRegionBytes) * i / (ft::kBufRegions - 1);
        D3D11_BOX box{};
        box.left = static_cast<UINT>(off & ~3ull); box.right = box.left + ft::kBufRegionBytes; box.bottom = 1; box.back = 1;
        g.ctx->CopySubresourceRegion(sbuf.Get(), 0, i * ft::kBufRegionBytes, 0, 0, buf.Get(), 0, &box);
    }
    D3D11_MAPPED_SUBRESOURCE bm{};
    const HRESULT hb = g.ctx->Map(sbuf.Get(), 0, D3D11_MAP_READ, 0, &bm);
    check(SUCCEEDED(hb), "the sampled regions map");
    if (SUCCEEDED(hb)) {
        const uint8_t* p = static_cast<const uint8_t*>(bm.pData);
        uint32_t litRegions = 0;
        for (uint32_t i = 0; i < ft::kBufRegions; ++i) {
            if (ft::countNonzero(p + i * ft::kBufRegionBytes, ft::kBufRegionBytes) != 0) ++litRegions;
        }
        g.ctx->Unmap(sbuf.Get(), 0);
        check(litRegions == 2, "exactly the two regions that hold non-zero bytes show as non-zero");
    }
}

// The automatic eye run's reduced capture, through the production Run: a 256 x 256 R8G8B8A8_TYPELESS eye, left half white and right half black, captured whole at
// 1/4 size by the mip chain. The written image is 64 x 64 with the half and half preserved, and the unreduced path is unchanged.
void testReducedCapture(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    std::vector<uint32_t> px(256 * 256);
    for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x) px[y * 256 + x] = x < 128 ? 0xFFFFFFFFu : 0xFF000000u;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 256; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), 256 * 4, 0};
    ComPtr<ID3D11Texture2D> src;
    check(SUCCEEDED(d->CreateTexture2D(&td, &sd, &src)), "a typeless eye texture");
    const uint32_t region[4] = {0, 0, 256, 256};
    namespace efc = edvr::eye_final_capture;
    efc::g_reduceLevel = ft::kReduceLevel;
    {
        efc::Run run;
        run.arm();
        run.schedule(0, 1);
        run.capture(1, 1, 0, g.ctx.Get(), src.Get(), region, false, false, false);
        efc::Image& im = run.rows[0].eye[0];
        check(!std::strcmp(im.status, "copied") && im.staging && im.crop[0] == 0 && im.crop[1] == 0 && im.crop[2] == 64 && im.crop[3] == 64,
              "the reduced row image is staged at 1/4 size and records its own size");
        if (im.staging) {
            D3D11_TEXTURE2D_DESC sdsc{};
            im.staging->GetDesc(&sdsc);
            check(sdsc.Width == 64 && sdsc.Height == 64, "the staging twin is 64 x 64");
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g.ctx->Map(im.staging, 0, D3D11_MAP_READ, 0, &m))) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(m.pData) + 32 * m.RowPitch);
                const uint8_t left = reinterpret_cast<const uint8_t*>(&row[8])[0], right = reinterpret_cast<const uint8_t*>(&row[56])[0];
                g.ctx->Unmap(im.staging, 0);
                check(left > 240 && right < 16, "the half of the eye that was white is white and the half that was black is black after the reduction");
            } else {
                check(false, "the reduced staging maps");
            }
        }
        run.reset();
    }
    efc::g_reduceLevel = 0;
    {
        efc::Run run;
        run.arm();
        run.schedule(0, 1);
        run.capture(1, 1, 0, g.ctx.Get(), src.Get(), region, false, false, false);
        efc::Image& im = run.rows[0].eye[0];
        check(!std::strcmp(im.status, "copied") && im.crop[2] - im.crop[0] == 256 && im.crop[3] - im.crop[1] == 256, "with the level at 0 the row image is the unreduced crop of an eye this small");
        run.reset();
    }
}

void testV10Gpu(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    // the block reduction on a real device: a typeless 64 x 64 eye, white in x < 24, black beyond (block column 1 is half white)
    std::vector<uint32_t> px(64 * 64);
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) px[y * 64 + x] = x < 24 ? 0xFFFFFFFFu : 0xFF000000u;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), 64 * 4, 0};
    ComPtr<ID3D11Texture2D> src;
    check(SUCCEEDED(d->CreateTexture2D(&td, &sd, &src)), "a typeless eye texture for the block reduction");
    ft::BlockReducer red;
    ID3D11Texture2D* stage = nullptr;
    uint32_t bw = 0, bh = 0;
    DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
    const char* why = "";
    const bool ok = src && red.reduce(d, g.ctx.Get(), src.Get(), &stage, &bw, &bh, &typed, &why);
    check(ok && bw == 4 && bh == 4 && typed == DXGI_FORMAT_R8G8B8A8_UNORM, ok ? "the reduction yields one texel per 16 px block and names the typed format" : why);
    if (ok) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
            float peak[16];
            ft::colorBlocks(static_cast<const uint8_t*>(m.pData), m.RowPitch, static_cast<uint32_t>(typed), bw, bh, peak);
            g.ctx->Unmap(stage, 0);
            check(peak[0] > 0.99f && peak[1] > 0.4f && peak[1] < 0.6f && peak[2] == 0.0f && peak[3] == 0.0f && peak[12] > 0.99f,
                  "the white blocks are white, the half-white block is half bright, the black blocks are exactly black");
        } else {
            check(false, "the block staging maps");
        }
        // the same reducer again with a second source reuses its mip texture
        ID3D11Texture2D* first = red.tex;
        const bool again = red.reduce(d, g.ctx.Get(), src.Get(), &stage, &bw, &bh, &typed, &why);
        check(again && red.tex == first, "the mip texture is reused for the same shape");
    }
    if (stage) stage->Release();
    red.release();
    // a target the mip chain cannot reduce is refused with a reason, not a fault
    {
        D3D11_TEXTURE2D_DESC tinyDesc = td;
        tinyDesc.Width = tinyDesc.Height = 8;
        ComPtr<ID3D11Texture2D> tiny;
        std::vector<uint32_t> p2(64, 0);
        D3D11_SUBRESOURCE_DATA s2{p2.data(), 32, 0};
        d->CreateTexture2D(&tinyDesc, &s2, &tiny);
        ft::BlockReducer r2;
        ID3D11Texture2D* st2 = nullptr;
        const char* w2 = "";
        check(tiny && !r2.reduce(d, g.ctx.Get(), tiny.Get(), &st2, &bw, &bh, &typed, &w2) && w2[0], "an 8 px target is refused with a reason");
        if (st2) st2->Release();
    }
    // depth: D32_FLOAT, D32_FLOAT_S8X24_UINT and D24_UNORM_S8_UINT, cleared to a depth and a stencil, copied whole and read as blocks
    struct Case { DXGI_FORMAT fmt; ft::DepthKind kind; float depth; uint8_t stencil; };
    const Case cases[3] = {{DXGI_FORMAT_D32_FLOAT, ft::DepthKind::D32, 0.25f, 0}, {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, ft::DepthKind::D32S8, 0.75f, 3}, {DXGI_FORMAT_D24_UNORM_S8_UINT, ft::DepthKind::D24S8, 0.5f, 9}};
    for (const Case& c : cases) {
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = dd.Height = 64; dd.MipLevels = dd.ArraySize = 1; dd.Format = c.fmt; dd.SampleDesc.Count = 1; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> dtex;
        ComPtr<ID3D11DepthStencilView> dsv;
        if (FAILED(d->CreateTexture2D(&dd, nullptr, &dtex)) || FAILED(d->CreateDepthStencilView(dtex.Get(), nullptr, &dsv))) { check(false, "a depth texture of this format on this device"); continue; }
        g.ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | (ft::hasStencil(c.kind) ? D3D11_CLEAR_STENCIL : 0u), c.depth, c.stencil);
        ID3D11Texture2D* ds = nullptr;
        D3D11_TEXTURE2D_DESC got{};
        const char* w3 = "";
        const bool q = ft::queueDepthCopy(d, g.ctx.Get(), dtex.Get(), &ds, &got, &w3);
        check(q && ft::depthKind(got.Format) == c.kind, q ? "the depth copy keeps the depth format" : w3);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (q && SUCCEEDED(g.ctx->Map(ds, 0, D3D11_MAP_READ, 0, &m))) {
            float mn[16], mx[16];
            uint8_t sm[16];
            ft::depthBlocks(static_cast<const uint8_t*>(m.pData), m.RowPitch, c.kind, 64, 64, 4, 4, mn, mx, sm);
            g.ctx->Unmap(ds, 0);
            const bool depthOk = mn[0] > c.depth - 0.001f && mx[15] < c.depth + 0.001f;
            check(depthOk && (!ft::hasStencil(c.kind) || sm[5] == c.stencil), "the cleared depth and stencil come back out of the staging copy");
        } else if (q) {
            check(false, "the depth staging maps");
        }
        if (ds) ds->Release();
    }
}

void testV11Gpu(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    ComPtr<ID3D11DeviceContext1> c1;
    g.ctx.As(&c1);
    // t0: 64 x 64, white in x < 16 (block column 0), black beyond; t3: 32 x 32, all black
    std::vector<uint32_t> px(64 * 64), px2(32 * 32, 0xFF000000u);
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) px[y * 64 + x] = x < 16 ? 0xFFFFFFFFu : 0xFF000000u;
    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Width = td.Height = 64;
    D3D11_SUBRESOURCE_DATA s1{px.data(), 64 * 4, 0};
    ComPtr<ID3D11Texture2D> t0, t3;
    check(SUCCEEDED(d->CreateTexture2D(&td, &s1, &t0)), "the first input texture");
    td.Width = td.Height = 32;
    D3D11_SUBRESOURCE_DATA s2{px2.data(), 32 * 4, 0};
    check(SUCCEEDED(d->CreateTexture2D(&td, &s2, &t3)), "the second input texture");
    ComPtr<ID3D11ShaderResourceView> v0, v3;
    d->CreateShaderResourceView(t0.Get(), nullptr, &v0);
    d->CreateShaderResourceView(t3.Get(), nullptr, &v3);
    ID3D11ShaderResourceView* views[1] = {v0.Get()};
    g.ctx->PSSetShaderResources(0, 1, views);
    views[0] = v3.Get();
    g.ctx->PSSetShaderResources(3, 1, views);
    // constant buffers: vs cb0 (64 bytes, floats 0..15) and ps cb1 bound at constant 16 for 16 constants, in a 1024 byte buffer of floats 0..255
    std::vector<float> fl(256);
    for (uint32_t i = 0; i < 256; ++i) fl[i] = static_cast<float>(i);
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 64;
    D3D11_SUBRESOURCE_DATA sb{fl.data(), 0, 0};
    ComPtr<ID3D11Buffer> cbV, cbP, vbuf;
    d->CreateBuffer(&bd, &sb, &cbV);
    bd.ByteWidth = 1024;
    d->CreateBuffer(&bd, &sb, &cbP);
    ID3D11Buffer* b0 = cbV.Get();
    g.ctx->VSSetConstantBuffers(0, 1, &b0);
    if (c1) {
        ID3D11Buffer* b1 = cbP.Get();
        UINT first = 16, num = 16;
        c1->PSSetConstantBuffers1(1, 1, &b1, &first, &num);
    }
    // six vertices, stride 16
    std::vector<float> vtx(6 * 4);
    for (uint32_t i = 0; i < 24; ++i) vtx[i] = static_cast<float>(100 + i);
    D3D11_BUFFER_DESC vd{};
    vd.ByteWidth = 96; vd.Usage = D3D11_USAGE_DEFAULT; vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sv{vtx.data(), 0, 0};
    d->CreateBuffer(&vd, &sv, &vbuf);
    ID3D11Buffer* vb = vbuf.Get();
    UINT stride = 16, off = 0;
    g.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER; sd.BorderColor[0] = 1.0f; sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> smp;
    d->CreateSamplerState(&sd, &smp);
    ID3D11SamplerState* ss = smp.Get();
    g.ctx->PSSetSamplers(0, 1, &ss);

    static ft::InputsCap cap;
    static ft::InputsData data;
    ft::BlockReducer red[ft::kInSlots];
    ft::captureInputs(d, g.ctx.Get(), c1.Get(), 'N', 6, red, cap);
    const ft::InputsFacts& f = cap.f;
    check(f.slot[0].bound && f.slot[0].isTex2d && f.slot[0].w == 64 && f.slot[0].fpQueued && f.slot[3].bound && f.slot[3].w == 32 && f.slot[3].fpQueued && !f.slot[1].bound && !f.slot[7].bound,
          "t0 and t3 are found with their shape and a fingerprint queued, the unbound slots are not");
    check(f.slot[0].viewDim == D3D11_SRV_DIMENSION_TEXTURE2D && f.slot[0].mipLevels == 1 && f.slot[0].res == t0.Get() && f.slot[3].res == t3.Get(), "the view's range and the resource's identity");
    check(f.cb[0].bound && f.cb[0].bytes == 64 && f.cb[0].bufBytes == 64 && !f.cb[1].bound && !f.cb[3].bound, "vertex cb0 is found with its size");
    if (c1) check(f.cb[4].bound && f.cb[4].first == 16 && f.cb[4].num == 16 && f.cb[4].bytes == 256 && f.cb[4].bufBytes == 1024, "pixel cb1 is read for its bound range only (constant 16 for 16 constants)");
    check(f.vtx.kind == 'N' && f.vtx.count == 6 && f.vtx.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && f.vtx.stride == 16 && f.vtx.vbBytes == 96 && f.vtx.vb == vbuf.Get() && f.vtx.ib == nullptr,
          "the vertex buffer, stride, topology and the draw's kind and count");
    check(f.smp[0].bound && f.smp[0].addrU == D3D11_TEXTURE_ADDRESS_BORDER && f.smp[0].border[0] == 1.0f && f.smp[0].filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR && !f.smp[1].bound, "the sampler state with its border colour");
    bool ready = false;
    for (int tries = 0; tries < 200 && !ready; ++tries) {
        g.ctx->Flush();
        ready = ft::readInputs(g.ctx.Get(), cap, data);
        if (!ready) Sleep(5);
    }
    check(ready, "the inputs read back without waiting");
    if (ready) {
        check(data.haveFp[0] && data.cls[0][0] == 7 && data.cls[0][1] == 0 && data.nz[0] == 4 && data.haveFp[3] && data.nz[3] == 0, "t0's first block column is white (class 7) and the rest black; t3 is all black");
        float w0, w1;
        std::memcpy(&w0, data.cb[0], 4);
        std::memcpy(&w1, data.cb[0] + 4 * 15, 4);
        check(w0 == 0.0f && w1 == 15.0f, "vertex cb0's words");
        if (c1) {
            float p0;
            std::memcpy(&p0, data.cb[4], 4);
            check(p0 == 64.0f, "pixel cb1 starts at constant 16 of the buffer (float 64)");
        }
        float vf0, vf5;
        std::memcpy(&vf0, data.vb, 4);
        std::memcpy(&vf5, data.vb + 5 * 16 + 12, 4);
        check(vf0 == 100.0f && vf5 == 123.0f, "the first and last of the six vertices");
    }
    cap.release();
    for (auto& r : red) r.release();
    // a mipped texture is reduced at the mip the view reads: mip 0 black (64 px, 4 x 4 blocks), mip 1 white (32 px, 2 x 2 blocks)
    {
        std::vector<uint32_t> m0(64 * 64, 0xFF000000u), m1(32 * 32, 0xFFFFFFFFu);
        D3D11_TEXTURE2D_DESC md{};
        md.Width = md.Height = 64; md.MipLevels = 2; md.ArraySize = 1; md.Format = DXGI_FORMAT_R8G8B8A8_UNORM; md.SampleDesc.Count = 1; md.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sdm[2] = {{m0.data(), 64 * 4, 0}, {m1.data(), 32 * 4, 0}};
        ComPtr<ID3D11Texture2D> mt;
        check(SUCCEEDED(d->CreateTexture2D(&md, sdm, &mt)), "a two-mip texture");
        ft::BlockReducer r;
        ID3D11Texture2D* st = nullptr;
        uint32_t bw = 0, bh = 0;
        DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
        const char* why = "";
        float peak[16];
        for (uint32_t mip = 1; mip != 2; mip = (mip == 1 ? 0 : 2)) {
            const bool ok = mt && r.reduce(d, g.ctx.Get(), mt.Get(), &st, &bw, &bh, &typed, &why, mip, 0);
            D3D11_MAPPED_SUBRESOURCE mm{};
            bool read = false;
            if (ok) {
                g.ctx->Flush();
                for (int tries = 0; tries < 200 && !read; ++tries) {
                    read = SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mm));
                    if (!read) Sleep(5);
                }
            }
            if (read) {
                ft::colorBlocks(static_cast<const uint8_t*>(mm.pData), mm.RowPitch, static_cast<uint32_t>(typed), bw, bh, peak);
                g.ctx->Unmap(st, 0);
            }
            if (mip == 1) check(ok && read && bw == 2 && bh == 2 && peak[0] > 0.99f && peak[3] > 0.99f, "mip 1 of the texture is reduced at its own size and is white");
            else check(ok && read && bw == 4 && bh == 4 && peak[0] == 0.0f && peak[15] == 0.0f, "mip 0 of the same texture, reduced by the same reducer, is its own size and black");
        }
        if (st) st->Release();
        r.release();
    }
    // leave the pipeline clean for the tests after this one
    ID3D11ShaderResourceView* none[8] = {};
    g.ctx->PSSetShaderResources(0, 8, none);
    ID3D11Buffer* nb[3] = {};
    g.ctx->VSSetConstantBuffers(0, 3, nb);
    if (c1) { UINT z[3] = {}; c1->PSSetConstantBuffers1(0, 3, nb, z, z); }
    ID3D11Buffer* nvb[1] = {};
    UINT z0 = 0;
    g.ctx->IASetVertexBuffers(0, 1, nvb, &z0, &z0);
    ID3D11SamplerState* ns[1] = {};
    g.ctx->PSSetSamplers(0, 1, ns);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 4. Source pins

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void testV8(const std::string& root) {
    namespace rh = edvr::ringhook;
    // The arrival signature (v9): ring+0x6b0 set. The alpha, the body mode and the direction are not part of it (flight f5c7ad89: the alpha is 1.000 from the first call).
    rh::Call c;
    c.mode = 2; c.alpha = 1.0f; c.direction = 0; c.scratch = true;
    check(rh::isArrival(c), "a call with ring+0x6b0 set at alpha 1.0 is the arrival");
    c.scratch = false;
    check(!rh::isArrival(c), "a call with ring+0x6b0 clear is not");
    c.scratch = true; c.mode = 1; c.alpha = 0.25f; c.direction = 1;
    check(rh::isArrival(c), "the body mode, the alpha and its direction do not matter");

    // A press 0.5 s after the capture closed finds the gate open and restarts the capture; the old one is over.
    ft::WindowMachine w;
    check(w.step(true, 100, 1000) == ft::WindowMachine::Opened && w.windows == 1, "a press-started capture opens");
    check(w.restart(900, 9000) == ft::WindowMachine::Opened && w.windows == 2 && w.capturing && w.open && w.startFrame == 900 && w.startMs == 9000,
          "a restart opens a new capture whatever state the old one is in");
    w.capturing = false;
    check(w.restart(1500, 12000) == ft::WindowMachine::Opened && w.capturing && w.windows == 3, "a restart after the capture closed (the gate still open) starts one");
    for (uint32_t i = 0; i < 20; ++i) w.restart(2000 + i, 20000 + i);
    check(w.windows == ft::kMaxWindows && w.restart(3000, 30000) == ft::WindowMachine::None, "restarts are capped with the session's windows");

    // The exit guard: the FSS latch closed at t=10000 ms; a jump 180 ms later is the exit, one 1100 ms later is not.
    ft::LatchGuard lg;
    lg.update(true, 5000);
    check(!lg.jumpRefused(5100), "an open latch refuses nothing");
    lg.update(false, 10000);
    check(lg.jumpRefused(10180) && lg.jumpRefused(10999) && !lg.jumpRefused(11000) && !lg.jumpRefused(12000), "a jump within 1 s of the latch closing is refused");

    // The validity line.
    ft::Validity v;
    v.chromeAtStart = true; v.arrivalFrame = 13273; v.runsRan = 3; v.runsPlanned = 3;
    char line[800];
    ft::formatValidity(line, sizeof(line), 1, "ring arrival (the first ring-pass call with ring+0x6b0 set, camera 1, scan alpha 1.000, seen at f=13273)", v);
    check(std::strcmp(line, "fss tiles: capture 1 valid=yes (started by ring arrival (the first ring-pass call with ring+0x6b0 set, camera 1, scan alpha 1.000, seen at f=13273), ring+0x6b0 set seen at f=13273, eye runs ran: 3 of 3 planned)") == 0,
          "a capture with the arrival, the chrome and its eye runs is valid");
    ft::Validity bad;
    bad.chromeAtStart = true; bad.runsPlanned = 3; bad.endedBy = "the scanner chrome dropped (the scanner was left)";
    ft::formatValidity(line, sizeof(line), 2, "zoom press at f=100", bad);
    check(std::strstr(line, "capture 2 valid=no (started by zoom press at f=100, ring+0x6b0 set seen at never, eye runs ran: 0 of 3 planned, ended by: the scanner chrome dropped") &&
              std::strstr(line, "never showed ring+0x6b0 set") && std::strstr(line, "no eye run ran") && std::strstr(line, "cut short"),
          "an invalid capture says it is invalid and every reason");

    // Source pins.
    const std::string cpp = slurp(root + "/src/d3d11/fss_tiles.cpp");
    const std::string rhs = slurp(root + "/src/d3d11/fss_ring_hook.cpp");
    check(!cpp.empty() && has(cpp, "fssRingHookTakeArrival(&aFrame, &aAlpha, &aCam)") && has(cpp, "g.rv.step(in)") && has(cpp, "g.rv.restartRequested = true;") &&
              has(cpp, "g.latch.jumpRefused(nowMs())"),
          "the arrival starts a capture, a later accepted press or jump restarts it, and a jump after the latch closed is refused");
    check(has(cpp, "formatValidity(vl, sizeof(vl), g.windowNo,") && has(cpp, "ring offscreen target identified: T%u") && has(cpp, "fss tiles: ring path f=%u cam=%d role=%u 6b0=%s") &&
              has(cpp, "ring path -- offscreen ring target (ring+0x6b0)") && has(cpp, "ring offscreen target fill") && has(cpp, "fss tiles: ring fill f=%u T%u"),
          "the validity line, the ring path lines and the offscreen target lines are written");
    check(has(cpp, "if (fssRingHookCurrent(&rc)) ringPathRecord(ctx, rtv, res, rc, instances, seq);") && has(cpp, "detail::g_fssTilesDraws = g.rv.win.capturing;"),
          "the draws made inside a ring-pass call are recorded for the whole capture");
    check(!rhs.empty() && has(rhs, "if (active || g_scan.load(std::memory_order_relaxed)) observe(ring, pass, camera, data, active);") && has(rhs, "g_latch.offer(c, frame)") &&
              has(rhs, "forward(ring, pass, camera, a4, a5, a6, a7, data);") && has(rhs, "t_cur.inCall = false;"),
          "every argument is forwarded unchanged, and the observation runs inside a capture or while the scanner is on show");
}

// ---- v10: the block dumps, the verdict, the suspect families, the schedule --------------------------------------------------------------------------------------------
void testV10Pure() {
    check(ft::kDumpFrames == 31 && ft::kBlockPx == 16 && ft::kBlockLevel == 4 && ft::kBlockSample == 4 && ft::kMaxBlocks == 128, "the dump runs arrival +0..+30 in 16 px blocks");
    // the suspects are named by the first eight hex digits of the 64-bit hashes the census prints
    check(ft::hi32(0x5DA53D8B0133341Eull) == 0x5DA53D8Bu && ft::suspectIndex(0x5DA53D8B0133341Eull, 0xE23C45251B7ECDFEull) == 0 && ft::suspectIndex(0x5DA53D8B0133341Eull, 0xBF0CE0DA543D491Full) == 1 &&
              ft::suspectIndex(0x4A0748B600000000ull, 0x1E1E004B00000001ull) == 2 && ft::suspectIndex(0x4A0748B600000000ull, 0x4411D5EF00000000ull) == 3 &&
              ft::suspectIndex(0x953C8123AD8DC13Bull, 0xA473A867BDDBC342ull) == -1 && ft::suspectIndex(0x5DA53D8B0133341Eull, 0xA473A867BDDBC342ull) == -1,
          "the four suspect families are found by their shader prefixes, the composite and a mismatched pair are not");

    // the schedule: flight aba75537's order within a frame (ring draws #176-#182, eye 0 composite #183, eye 1 composite #217)
    ft::SnapSchedule sc;
    uint32_t due1 = 99, due2 = 99, due3 = 99;
    const bool c0 = sc.composite(0, 183);
    due1 = sc.takeAfter();            // the draw #184: eye 0's "after" is due
    const bool c0again = sc.composite(0, 190);
    const bool c1 = sc.composite(1, 217);
    due2 = sc.takeAfter();            // the draw #218
    due3 = sc.takeAfter();
    check(c0 && !c0again && c1 && due1 == 1u && due2 == 2u && due3 == 0u && sc.compositeSeq[0] == 183 && sc.compositeSeq[1] == 217, "the after-snapshot is due on the first draw after each eye's composite, once, and a second composite draw of an eye is ignored");
    sc.newFrame();
    check(sc.composite(0, 5) && sc.pendingAfter[0] && !sc.composite(2, 6), "a new frame starts clean; a composite that is the last draw stays pending for the boundary");

    // brightness decode
    bool known = false;
    uint8_t half1[8] = {0x00, 0x3C, 0, 0, 0, 0, 0, 0};                    // 1.0, 0, 0 in R16G16B16A16_FLOAT
    check(ft::texelPeak(10, half1, &known) == 1.0f && known, "R16G16B16A16_FLOAT decodes");
    uint8_t half0[8] = {};
    check(ft::texelPeak(10, half0, &known) == 0.0f && known, "black decodes to exactly zero");
    const uint32_t r11 = 15u << 6;                                       // R = 1.0 in R11G11B10_FLOAT
    uint8_t p11[4];
    std::memcpy(p11, &r11, 4);
    check(ft::texelPeak(26, p11, &known) == 1.0f && known, "R11G11B10_FLOAT decodes");
    uint8_t p8[4] = {255, 0, 0, 255};
    check(ft::texelPeak(28, p8, &known) == 1.0f && known && (ft::texelPeak(0, p8, &known), !known), "R8G8B8A8_UNORM decodes and an unknown format says so");
    check(ft::peakClass(0.0f) == 0 && ft::peakClass(5e-4f) == 1 && ft::peakClass(0.5f) == 6 && ft::peakClass(2.0f) == 7 && ft::peakClass(100.0f) == 9, "brightness classes");
    float peaks[4];
    uint8_t px[2 * 2 * 4] = {255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 0, 10, 0, 0, 255};
    ft::colorBlocks(px, 8, 28, 2, 2, peaks);
    check(peaks[0] == 1.0f && peaks[1] == 0.0f && peaks[2] == 0.0f && peaks[3] > 0.0f && peaks[3] < 0.1f, "colorBlocks reads one texel a block, alpha ignored");

    // depth and stencil blocks
    std::vector<uint8_t> d32(32 * 32 * 4);
    for (uint32_t i = 0; i < 32 * 32; ++i) { const float v = 0.5f; std::memcpy(&d32[i * 4], &v, 4); }
    const float hi = 0.9f;
    std::memcpy(&d32[(4 * 32 + 4) * 4], &hi, 4);                         // inside block (0,0), on a sample position
    std::memcpy(&d32[(5 * 32 + 5) * 4], &hi, 4);                         // inside block (0,0), NOT a sample position
    float dmin[4], dmax[4];
    uint8_t smax[4];
    ft::depthBlocks(d32.data(), 32 * 4, ft::DepthKind::D32, 32, 32, 2, 2, dmin, dmax, smax);
    check(dmin[0] == 0.5f && dmax[0] > 0.89f && dmin[3] == 0.5f && dmax[3] == 0.5f && smax[0] == 0, "depth min and max a block, from every 4th texel");
    std::vector<uint8_t> ds(16 * 16 * 8, 0);
    for (uint32_t i = 0; i < 16 * 16; ++i) { const float v = 0.25f; std::memcpy(&ds[i * 8], &v, 4); ds[i * 8 + 4] = 7; }
    ft::depthBlocks(ds.data(), 16 * 8, ft::DepthKind::D32S8, 16, 16, 1, 1, dmin, dmax, smax);
    check(dmin[0] == 0.25f && smax[0] == 7 && ft::hasStencil(ft::DepthKind::D32S8) && !ft::hasStencil(ft::DepthKind::D32), "D32_FLOAT_S8X24 keeps the stencil in the second dword");
    std::vector<uint8_t> d24(16 * 16 * 4, 0);
    for (uint32_t i = 0; i < 16 * 16; ++i) { const uint32_t v = (3u << 24) | 0x800000u; std::memcpy(&d24[i * 4], &v, 4); }
    ft::depthBlocks(d24.data(), 16 * 4, ft::DepthKind::D24S8, 16, 16, 1, 1, dmin, dmax, smax);
    check(dmin[0] > 0.49f && dmin[0] < 0.51f && smax[0] == 3, "D24_UNORM_S8_UINT");
    check(ft::depthKind(40) == ft::DepthKind::D32 && ft::depthKind(39) == ft::DepthKind::D32 && ft::depthKind(20) == ft::DepthKind::D32S8 && ft::depthKind(45) == ft::DepthKind::D24S8 &&
              ft::depthKind(55) == ft::DepthKind::D16 && ft::depthKind(28) == ft::DepthKind::None, "depth formats");

    // the verdict: six blocks, each with a different story (reversed Z; the composite is GREATER_EQUAL and writes only where the depth is at or below the quad's)
    {
        const float before[6] = {0, 0, 0, 0, 0.2f, 0}, after[6] = {0.5f, 0, 0, 0.4f, 0, 0}, fin[6] = {0.5f, 0, 0, 0, 0, 0.7f};
        const float dminv[6] = {0.0f, 0.9f, 0.0f, 0.0f, 0.0f, 0.0f}, dmaxv[6] = {0.0f, 0.95f, 0.0f, 0.0f, 0.0f, 0.0f};
        uint8_t kl[6];
        const ft::Verdict v = ft::classifyBlocks(before, after, fin, dminv, dmaxv, true, 6, kl);
        check(kl[0] == ft::kNotBlack && kl[1] == ft::kRejected && kl[2] == ft::kNeverCovered && kl[3] == ft::kOverwritten && kl[4] == ft::kWroteBlack && kl[5] == ft::kNotBlack,
              "a black block is rejected by depth, overwritten after the composite, never covered, or written black, and a bright one is none of them");
        check(v.blocks == 6 && v.black == 4 && v.wrote == 3 && v.qzSeen && v.qz == 0.0f && v.count[ft::kRejected] == 1 && v.count[ft::kOverwritten] == 1 && v.count[ft::kNeverCovered] == 1 &&
                  v.count[ft::kWroteBlack] == 1,
              "the counts");
        uint8_t kl2[6];
        const ft::Verdict v2 = ft::classifyBlocks(before, after, fin, dminv, dmaxv, false, 6, kl2);
        check(kl2[1] == ft::kNoDepth && kl2[2] == ft::kNoDepth && kl2[3] == ft::kOverwritten && !v2.qzSeen, "without depth, an untouched black block is 'no depth to judge by' and the composite's own writes still classify");
        char line[900];
        ft::formatVerdict(line, sizeof(line), 23372, 0, 10, 40, 126, 126, v);
        check(std::strstr(line, "verdict f=23372 eye0 composite target T10, depth T40, 126x126 blocks of 16 px: black at the end of the frame 4;") &&
                  std::strstr(line, "rejected) 1, overwritten after the composite 1, never covered 1, written black by the composite 1, no depth to judge 0"),
              "the verdict line");
        // the asymmetry between the eyes
        uint8_t other[6] = {0, 0, 1, 0, 0, 0};   // the other eye is black only at block 2
        uint32_t a[ft::kClassCount];
        ft::asymmetric(kl, other, 6, a);
        check(a[ft::kRejected] == 1 && a[ft::kOverwritten] == 1 && a[ft::kNeverCovered] == 0 && a[ft::kWroteBlack] == 1, "blocks black here and not black in the other eye, by class");
    }
    char head[32];
    check(ft::pgmHeader(head, sizeof(head), 126, 126) == 15 && std::strcmp(head, "P5\n126 126\n255\n") == 0, "the grey image header");
    ft::DriverTally dt;
    for (uint32_t h : {1u, 2u, 1u, 3u, 1u, 4u, 5u, 6u, 7u, 8u}) dt.add(h);
    check(dt.distinct == 8 && dt.used == ft::DriverTally::kCap && dt.hash[0] == 1 && dt.n[0] == 3, "the binding-hash tally counts every distinct hash and keeps the first few");
    check(ft::fnvStep(ft::kFnvBasis, 1) != ft::fnvStep(ft::kFnvBasis, 2), "the binding hash tells draws apart");
}

// ---- v11: every input of the composite draw, compared between the eyes -----------------------------------------------------------------------------------------------------------
void testV11Pure() {
    check(ft::kInSlots == 8 && ft::kInCb == 6 && ft::kInCbMax == 512 && ft::kInVertices == 6 && ft::kInSamplers == 4 && ft::kInFullAt0 == 0 && ft::kInFullAt1 == 5,
          "the composite's inputs: t0-t7, vs/ps cb0-cb2 up to 512 bytes, six vertices, s0-s3, in full at arrival +0 and +5");
    static ft::InputsFacts fa, fb;
    static ft::InputsData da, db;
    fa = ft::InputsFacts();
    fb = ft::InputsFacts();
    std::memset(&da, 0, sizeof(da));
    std::memset(&db, 0, sizeof(db));
    int layerA = 0, otherA = 0, otherB = 0, cbA = 0, vbA = 0;
    auto bindSlot = [](ft::InputsFacts& f, ft::InputsData& d, uint32_t k, const void* res, uint32_t w, uint32_t hash, uint32_t nz) {
        ft::SlotInfo& s = f.slot[k];
        s.bound = true; s.res = res; s.isTex2d = true; s.w = s.h = w; s.fmt = 26; s.resMips = 1; s.resArray = 1; s.viewDim = 4; s.viewFmt = 26; s.mipLevels = 1; s.arraySize = 1;
        s.fpQueued = true; f.bw[k] = f.bh[k] = w / 16; f.typed[k] = 26;
        d.haveFp[k] = true; d.hash[k] = hash; d.nz[k] = nz;
    };
    // t2 is the layer, the same resource in both eyes; t3 is a different texture in each, with different contents
    bindSlot(fa, da, 2, &layerA, 1008, 0xAAAA0001u, 40);
    bindSlot(fb, db, 2, &layerA, 1008, 0xAAAA0001u, 40);
    bindSlot(fa, da, 3, &otherA, 252, 0x11111111u, 0);
    bindSlot(fb, db, 3, &otherB, 252, 0x22222222u, 12);
    for (ft::InputsFacts* f : {&fa, &fb}) {
        f->cb[4].bound = true; f->cb[4].first = 16; f->cb[4].num = 16; f->cb[4].bytes = 64; f->cb[4].bufBytes = 1024;
        f->smp[1].bound = true; f->smp[1].addrU = f->smp[1].addrV = 3;
        f->vtx.kind = 'N'; f->vtx.count = 6; f->vtx.topology = 4; f->vtx.stride = 16; f->vtx.vbBytes = 96; f->vtx.vp[2] = f->vtx.vp[3] = 2016; f->vtx.layout = &vbA;
    }
    fa.cb[4].buf = &cbA;
    fb.cb[4].buf = &cbA;
    for (uint32_t i = 0; i < 16; ++i) { const float v = static_cast<float>(i); std::memcpy(da.cb[4] + 4 * i, &v, 4); std::memcpy(db.cb[4] + 4 * i, &v, 4); }
    // identical but for t3
    ft::InputDiffResult r;
    ft::InputDiffTotals t;
    ft::compareInputs(fa, da, fb, db, &r, &t);
    check(r.any && (r.slot[2] & ~ft::kSlotCompared) == 0 && (r.slot[3] & ft::kSlotIdDiff) && (r.slot[3] & ft::kSlotContentDiff) && !(r.slot[3] & ft::kSlotSameIdContentDiff) && r.cbWords[4] == 0 &&
              r.smpMask == 0 && r.vtx == 0 && t.pairs == 1 && t.slot[3].idDiff == 1 && t.slot[3].contentDiff == 1 && t.slot[2].idDiff == 0 && t.slot[2].frames == 1 && t.slot[0].frames == 0,
          "a texture that is a different resource with different contents is the only difference found");
    // a constant that differs, a sampler that differs, a vertex that differs, the same layer changed between the two draws
    uint32_t w = 0x3F000000u;                                  // 0.5
    std::memcpy(db.cb[4] + 4 * 7, &w, 4);
    std::memcpy(db.cb[4] + 4 * 9, &w, 4);
    fb.smp[1].addrU = 4;
    da.vb[20] = 1; db.vb[20] = 2;
    db.hash[2] = 0xBBBB0002u;
    ft::compareInputs(fa, da, fb, db, &r, &t);
    check(r.cbWords[4] == 2 && r.cbFlags[4] == 0 && (r.smpMask == 2u) && (r.vtx & ft::kVtxBytes) && !(r.vtx & ft::kVtxLayout) && (r.slot[2] & ft::kSlotContentDiff) && (r.slot[2] & ft::kSlotSameIdContentDiff) &&
              !(r.slot[2] & ft::kSlotIdDiff) && t.cb[4].words == 2 && t.cb[4].framesWithWords == 1 && t.smpDiff[1] == 1 && t.vtxDiff == 1 && t.slot[2].sameIdContentDiff == 1,
          "differing constant words, a sampler address, vertex bytes and the same resource changing between the draws are each named");
    uint32_t a[4] = {1, 2, 3, 4}, b[4] = {1, 9, 3, 8}, idx[1];
    check(ft::wordDiffs(a, b, 4, idx, 1) == 2 && idx[0] == 1 && ft::wordDiffs(a, a, 4, idx, 1) == 0, "word diffs count them all and keep the first few");
    ft::InputsFacts fc = fa;
    fc.slot[2].bound = false;
    ft::InputDiffResult r3;
    ft::InputDiffTotals t3;
    ft::compareInputs(fa, da, fc, db, &r3, &t3);
    check((r3.slot[2] & ft::kSlotBoundDiff) && t3.slot[2].boundDiff == 1, "a texture bound in one eye only is named");
    ft::InputDiffResult r4;
    ft::InputDiffTotals t4;
    static ft::InputsData dd;
    std::memcpy(&dd, &da, sizeof(dd));
    ft::compareInputs(fa, da, fa, dd, &r4, &t4);
    check(!r4.any, "identical inputs report no difference");
    char line[500];
    ft::SlotInfo si = fa.slot[2];
    ft::formatSlot(line, sizeof(line), 2, si, 95, true, 63, 63, 0xAAAA0001u, 40);
    check(std::strstr(line, "t2 T95 1008x1008 fmt26 (resource mips 1 array 1 samples 0) view dim 4 fmt 26 mip 0+1 slice 0+1 | blocks 63x63 fingerprint AAAA0001, 40 not black"), "the slot line");
    check(ft::hashBytes(reinterpret_cast<const uint8_t*>("ab"), 2) == ft::hashBytes(reinterpret_cast<const uint8_t*>("ab"), 2) && ft::hashBytes(reinterpret_cast<const uint8_t*>("ab"), 2) !=
              ft::hashBytes(reinterpret_cast<const uint8_t*>("ba"), 2), "the byte hash tells orders apart");
}

// ---- v9: the replay of flight f5c7ad89 (capture 4) through the decisions that missed three flights -----------------------------------------------------------
// The sequence: a zoom press in the scanner at f18832 opens a gate-started capture; the ring pass starts calling at f18855 with ring+0x6b0 set and the scan alpha at 1.000 (never
// below it); camera 1 draws into T42 252x252 from f18856 and into T43 1008x1008 from f18905; cameras 22 and 8 make the same calls and draw nothing. The scanner chrome is held
// on every frame. The capture must arm at the arrival, re-identify the target at the change, and the eye runs must fire at +0, +30 and +270.
void testV9Replay(const std::string& root) {
    namespace rh = edvr::ringhook;
    ft::RevealMachine m;
    rh::ArrivalLatch latch;
    ft::TargetTracker tracker;
    int a_dummy = 0, b_dummy = 0;
    const void* T42 = &a_dummy;
    const void* T43 = &b_dummy;
    uint32_t triggers = 0, firstTrigger = 0;
    uint32_t armedAt = 0, openedAtArrival = 0, closedAt = 0, windowsAfter = 0;
    uint32_t fired[3] = {0, 0, 0}, firedN = 0;
    uint32_t firsts = 0, changes = 0, changedAt = 0, firstAt = 0;
    uint32_t refusedRestartAt = 0;
    bool pressRestartIgnored = false, ledgerStillOnAt100 = true;
    ft::RevealMachine::Out armedOut;
    for (uint32_t f = 18800; f <= 19600; ++f) {
        // the glue: a zoom press while the scanner chrome is held opens the gate (fssTilesNoteZoomSignal)
        if (f == 18832) m.gate.signal(f, 0);
        // a later press while the arrival's reveal capture runs (the second jump of a zoom): it must not restart it
        if (f == 18900) m.restartRequested = true;
        // the ring pass runs inside this frame (before the boundary that ends it): cameras 1, 22, 8, passes 0 1 2 5 6 8, ring+0x6b0 clear then set
        bool offered = false;
        if (f >= 18855) {
            const int cams[3] = {1, 22, 8};
            const unsigned roles[3] = {1, 2, 0};
            const int passes[6] = {0, 1, 2, 5, 6, 8};
            for (int scratch = 0; scratch < 2; ++scratch) {
                for (int ci = 0; ci < 3; ++ci) {
                    for (int pi = 0; pi < 6; ++pi) {
                        rh::Call c;
                        c.camera = cams[ci]; c.role = roles[ci]; c.pass = passes[pi]; c.mode = 2; c.scratch = scratch != 0; c.alpha = 1.0f; c.direction = 0; c.active = 1;
                        if (latch.offer(c, f)) { offered = true; ++triggers; if (!firstTrigger) firstTrigger = f; }
                    }
                }
            }
            // camera 1's four draws a frame into the layer: T42, then T43 from f18905
            const void* target = f >= 18905 ? T43 : T42;
            const ft::TargetTracker::Result r = tracker.observe(target);
            if (r == ft::TargetTracker::First) { ++firsts; firstAt = f; }
            if (r == ft::TargetTracker::Changed) { ++changes; changedAt = f; }
        }
        // the boundary that ends frame f-1 and starts f; the arrival seen in frame f is taken at the next one (the glue's order)
        ft::RevealMachine::In in;
        in.frame = f;
        in.ms = 1000 + static_cast<uint64_t>(f - 18800) * 11;
        in.chromeAgo = 0;
        in.arrivalOffered = false;
        static bool pending = false;
        if (f == 18800) pending = false;
        in.arrivalOffered = pending;
        pending = offered;
        const bool wasCapturing = m.win.capturing;
        const ft::RevealMachine::Out o = m.step(in);
        if (o.armed) { armedAt = f; armedOut = o; windowsAfter = m.win.windows; }
        if (o.ev == ft::WindowMachine::Opened && o.start == ft::RevealMachine::StartArrival) openedAtArrival = f;
        if (o.restartIgnored) refusedRestartAt = f;
        if (o.fire) { if (firedN < 3) fired[firedN] = f; ++firedN; }
        if (o.ev == ft::WindowMachine::Closed && closedAt == 0 && wasCapturing) closedAt = f;
        if (f == armedAt + 100 && armedAt) { ledgerStillOnAt100 = m.ledgerOn; pressRestartIgnored = m.win.windows == windowsAfter && m.win.capturing && m.revealUntil == armedAt + ft::kRevealFrames; }
        (void)ledgerStillOnAt100;
    }
    check(triggers == 1 && firstTrigger == 18855, "the first ring-pass call with ring+0x6b0 set triggers, once, at f18855, though the alpha is 1.000 throughout (flight f5c7ad89)");
    check(armedAt == 18856 && openedAtArrival == 18856 && armedOut.armed && armedOut.armedRuns == 3 && armedOut.restarted && armedOut.start == ft::RevealMachine::StartArrival,
          "the capture arms on the arrival, replacing the press-started one, with three eye runs planned");
    check(m.win.windows == 2 && windowsAfter == 2, "the arrival restarts the capture a press had started: two windows, not three");
    check(firedN == 3 && fired[0] == 18856 && fired[1] == 18886 && fired[2] == 19126, "the eye runs fire at +0, +30 and +270 frames from the arrival and at no other frame");
    check(m.runs.ran == 3 && m.runs.planned == 3 && m.runs.sessionPlanned == 3, "all three runs ran");
    check(pressRestartIgnored && refusedRestartAt == 18900, "a later press or jump during the reveal capture does not restart it");
    check(closedAt == 18856 + ft::kRevealFrames + 1, "the capture runs its 600 frames and ends there");
    check(!m.ledgerOn && m.revealUntil == 0, "the ledger and the reveal clock are done");
    check(firsts == 1 && firstAt == 18855 && changes == 1 && changedAt == 18905 && tracker.cur == T43 && tracker.changes == 1,
          "the offscreen target is identified at its first draw and re-identified once when it moves from T42 to T43");
    ft::TargetTracker t2;
    check(t2.observe(nullptr) == ft::TargetTracker::Same && t2.observe(T42) == ft::TargetTracker::First && t2.observe(T42) == ft::TargetTracker::Same &&
              t2.observe(T43) == ft::TargetTracker::Changed && t2.observe(T42) == ft::TargetTracker::Changed && t2.changes == 2,
          "the tracker reports a first target, no change, and every move");

    // the validity of that capture
    ft::Validity v;
    v.chromeAtStart = true; v.arrivalFrame = 18855; v.runsRan = m.runs.ran; v.runsPlanned = m.runs.planned;
    check(v.valid(), "the replayed capture is valid");

    // the latch: one trigger per zoom, a new one after the layer was away for more than a minute of frames, or after the scanner was left
    {
        rh::ArrivalLatch l;
        rh::Call on;
        on.scratch = true;
        rh::Call off;
        check(!l.offer(off, 10) && l.offer(on, 11) && !l.offer(on, 12) && !l.offer(off, 13) && !l.offer(on, 14), "the latch triggers on the first call with ring+0x6b0 set and then stays quiet");
        check(!l.offer(on, 14 + rh::kRearmGapFrames) && l.offer(on, 14 + rh::kRearmGapFrames + rh::kRearmGapFrames + 1), "a gap of more than kRearmGapFrames frames makes the next zoom a new arrival");
        l.rearm();
        check(l.offer(on, 400), "leaving the scanner re-arms it");
    }

    // an arrival while the chrome is not held is refused and starts nothing
    {
        ft::RevealMachine mm;
        ft::RevealMachine::In in;
        in.frame = 500; in.ms = 5000; in.chromeAgo = 20; in.arrivalOffered = true;
        const ft::RevealMachine::Out o = mm.step(in);
        check(o.arrivalRefused && !o.arrivalAccepted && o.ev == ft::WindowMachine::None && !mm.win.capturing && !o.armed && !mm.runs.pending[0], "an arrival without the scanner chrome is refused");
    }
    // leaving the scanner mid-capture ends it, cancels the pending runs, and the runs wait for the chrome
    {
        ft::RevealMachine mm;
        ft::RevealMachine::In in;
        in.frame = 100; in.ms = 1000; in.chromeAgo = 0; in.arrivalOffered = true;
        ft::RevealMachine::Out o = mm.step(in);
        check(o.armed && o.fire == 1u && mm.runs.pending[1] && mm.runs.pending[2], "armed with the first run fired at once");
        in.arrivalOffered = false;
        uint32_t refusedBits = 0, fired1 = 0;
        for (uint32_t f = 101; f <= 135; ++f) {
            in.frame = f; in.ms += 11;
            in.chromeAgo = (f >= 130 && f < 134) ? 10u : 0u;   // the chrome is lost for a few frames when the second run is due
            o = mm.step(in);
            refusedBits |= o.refused;
            if (o.fire == 2u) fired1 = f;
        }
        check(refusedBits == 2u && fired1 == 134, "a run that is due while the chrome is lost waits, is refused once, and starts when the chrome is back");
        in.frame = 150; in.ms += 11; in.chromeAgo = ft::kChromeDropFrames + 1;
        o = mm.step(in);
        check(o.exited && o.endedBy[0] && !mm.win.capturing && o.ev == ft::WindowMachine::Closed && o.cancelledExit == 4u && mm.revealUntil == 0,
              "leaving the scanner ends the capture and cancels the third run");
    }
    // a press-started capture that has not seen the arrival is restarted by a later press
    {
        ft::RevealMachine mm;
        mm.gate.signal(10, 0);
        ft::RevealMachine::In in;
        in.frame = 10; in.ms = 100; in.chromeAgo = 0;
        ft::RevealMachine::Out o = mm.step(in);
        check(o.ev == ft::WindowMachine::Opened && o.start == ft::RevealMachine::StartGate && !o.armed, "a press opens a gate-started capture that arms nothing");
        mm.gate.signal(40, 0);
        mm.restartRequested = true;
        in.frame = 40; in.ms = 500;
        o = mm.step(in);
        check(o.ev == ft::WindowMachine::Opened && o.restarted && o.start == ft::RevealMachine::StartRestart && mm.win.windows == 2 && !o.restartIgnored,
              "a later press restarts a capture that has not found the arrival");
    }
    // the order of a draw among camera 1's four ring draws
    check(ft::orderOf(50, 100, 103, 4) == ft::RingOrder::Before && ft::orderOf(101, 100, 103, 4) == ft::RingOrder::Between && ft::orderOf(100, 100, 103, 4) == ft::RingOrder::Between &&
              ft::orderOf(200, 100, 103, 4) == ft::RingOrder::After && ft::orderOf(50, 0, 0, 0) == ft::RingOrder::NoRingDraws,
          "a compositor draw is classified before, between or after camera 1's ring draws, or as having no ring draw to compare with");
    check(std::strstr(ft::orderText(ft::RingOrder::Before), "BEFORE") && std::strstr(ft::orderText(ft::RingOrder::After), "AFTER") && std::strstr(ft::orderText(ft::RingOrder::Between), "BETWEEN"),
          "the order text names the position");

    // the glue uses all of it
    const std::string cpp = slurp(root + "/src/d3d11/fss_tiles.cpp");
    const std::string rhs = slurp(root + "/src/d3d11/fss_ring_hook.cpp");
    check(has(cpp, "const RevealMachine::Out o = g.rv.step(in);") && has(cpp, "fssRingHookScan(chromeHeld(chromeAgo));") && has(cpp, "in.arrivalOffered = fssRingHookTakeArrival(&aFrame, &aAlpha, &aCam);") &&
              has(rhs, "if (g_latch.offer(c, frame)) {"),
          "the boundary feeds the machine the ring hook's trigger, and the hook feeds the latch every ring-pass call while the scanner chrome is held");
    check(has(cpp, "if (rc.scratch && e->tw >= kMinTargetWidth) adoptRingTarget(res, e->tw, e->th, e->tfmt, rc);") && has(cpp, "const TargetTracker::Result tr = g.tracker.observe(res);") &&
              has(cpp, "ring offscreen target CHANGED at f=%u"),
          "every change of the offscreen target is logged and followed");
    check(has(cpp, "*e = State::RPEnt();   // the entries are reused every frame") && has(cpp, "g.drawSeq = g.ringSeqFirst = g.ringSeqLast = g.ringSeqN = 0;"),
          "the ring path's per-frame entries start clean, so draws= is a per-frame count");
    check(has(cpp, "else if (g.ringTargetRef) {") && has(cpp, "compositorCheck(ctx, rtv, res, shape, instances, seq, &isComposite);") && has(cpp, "orderOf(c.firstSeq, g.ringSeqFirst, g.ringSeqLast, g.ringSeqN)") &&
              has(cpp, "fss tiles: compositor f=%u %s draw #%u..#%u") && has(cpp, "window %u compositors -- no draw outside the ring pass sampled the offscreen ring layer"),
          "the draws that sample the layer are found, ordered against camera 1's ring draws and said, nothing found included");
    check(has(cpp, "ringSnapshot(g.frame);") && has(cpp, "drainRingSnap(frameNo);") && has(cpp, "frame % kFillEvery != 0") && has(cpp, "if (g.snap.used && !g.snap.drained) return true;"),
          "the layer's fingerprint is taken at the end of a frame every kFillEvery frames and read back without a stall");
}

void testSourcePins(const std::string& root) {
    const std::string ef = slurp(root + "/src/d3d11/exposure_fix.cpp");
    check(!ef.empty(), "exposure_fix.cpp is readable");
    const size_t hook = ef.find("void STDMETHODCALLTYPE hookedDispatchIndirect");
    const size_t call = ef.find("fssTilesDispatchIndirect(self, args, off, hashOf(bindingGet(BindSlot::Cs))", hook);
    const size_t real = ef.find("s->realDispatchIndirect(self, args, off);", call);
    check(hook != std::string::npos && call != std::string::npos && real != std::string::npos && hook < call && call < real,
          "the DispatchIndirect hook offers the dispatch to fss_tiles before forwarding it");
    const size_t flat = ef.find("if (runtimeFlatProfile())", hook);
    check(flat != std::string::npos && flat < call, "the flat profile never reaches fss_tiles (one view, nothing to compare)");
    check(has(ef, "fssTilesActive() && !foreignContext(self)"), "owner context only, behind the one-bool gate");

    const std::string vs = slurp(root + "/src/d3d11/vscreen.cpp");
    size_t n = 0, at = 0;
    while ((at = vs.find("fssTilesConfigure(cfg);", at)) != std::string::npos) { ++n; ++at; }
    check(n == 2, "both config sites (install and reload) configure fss_tiles");
    check(has(vs, "fssTilesFrameBoundary(s->frameNo, chromeAgo, journalFssFocusKnown(), journalFssFocus())"),
          "the frame boundary hands fss_tiles the chrome age and the journal focus, and its own gate decides the window");

    {
        struct Pin { const char* fn; const char* call; };
        const Pin pins[] = {{"hookedClearRtv", "fssTilesNoteWriteView(rtv, 2)"},
                            {"hookedClearUavUint", "fssTilesNoteWriteView(uav, 0)"},
                            {"hookedClearUavFloat", "fssTilesNoteWriteView(uav, 1)"},
                            {"hookedCopyResource", "fssTilesNoteWrite(dst, 3)"},
                            {"hookedCopySubresourceRegion", "fssTilesNoteWrite(dst, 4)"},
                            {"hookedUpdateSubresource", "fssTilesNoteWrite(dst, 5)"}};
        for (const Pin& pin : pins) {
            const size_t f = vs.find(std::string("void STDMETHODCALLTYPE ") + pin.fn + "(");
            const size_t c = vs.find(pin.call, f);
            const size_t real = vs.find("g_state->real", c);
            char what[160];
            std::snprintf(what, sizeof(what), "%s offers its destination to fss_tiles (owner context) before forwarding", pin.fn);
            check(f != std::string::npos && c != std::string::npos && c > f && c - f < 1200 && real > c, what);
        }
    }
    {
        struct DPin { const char* fn; const char* call; };
        const DPin dpins[] = {{"hookedDraw", "fssTilesNoteDraw(self, 'D', count, 1)"},
                              {"hookedDrawIndexed", "fssTilesNoteDraw(self, 'I', count, 1)"},
                              {"hookedDrawInstanced", "fssTilesNoteDraw(self, 'N', perInstance, instances)"},
                              {"hookedDrawIndexedInstanced", "fssTilesNoteDraw(self, 'X', perInstance, instances)"},
                              {"hookedDrawIndexedInstancedIndirect", "fssTilesNoteDraw(self, 'Z', 0, 0)"},
                              {"hookedDrawInstancedIndirect", "fssTilesNoteDraw(self, 'Y', 0, 0)"}};
        for (const DPin& pin : dpins) {
            const size_t f = vs.find(std::string("void STDMETHODCALLTYPE ") + pin.fn + "(");
            const size_t c = vs.find(pin.call, f);
            const size_t next = vs.find("if (runtimeFlatProfile())", f);
            char what[160];
            std::snprintf(what, sizeof(what), "%s counts its draw for fss_tiles (owner context) before anything else", pin.fn);
            check(f != std::string::npos && c != std::string::npos && c > f && c - f < 600 && next > c, what);
        }
    }
    const std::string tp = slurp(root + "/src/d3d11/temporal_pass.cpp");
    const size_t arm = tp.find("void temporalPassArmEyeDump()");
    const size_t note = tp.find("fssTilesNoteEyeDump();", arm);
    const size_t begin = tp.find("beginEyeRun();", arm);
    check(arm != std::string::npos && note != std::string::npos && begin != std::string::npos && note < begin,
          "the eye-dump key tells fss_tiles before it starts the eye run");
    const std::string dh = slurp(root + "/src/d3d11/device_hook.cpp");
    size_t nd = 0, ad = 0;
    while ((ad = dh.find("eyeSyncFromConfig(Config::get()).needsFssSignals()", ad)) != std::string::npos) { ++nd; ++ad; }
    size_t nw = 0, aw = 0;
    while ((aw = dh.find("eyeSyncFromConfig(Config::get()).needsFssSignals() || fssTilesWantsSignals();", aw)) != std::string::npos) { ++nw; ++aw; }
    check(nw == 2, "at both sites the instrument alone is enough to run the fast Status poll and the zoom watchers, whatever fix.fss_eye_sync is");
    check(nd == 2 && !has(dh, "eyeSyncFromConfig(Config::get()).any()"),
          "the mode latch and the eager Status poll are wanted by needsFssSignals() at both sites, never by any()");
    check(has(vs, "fssTilesNoteZoomSignal(s->frameNo, true, chromeAgo)") && has(vs, "fssTilesNoteZoomSignal(s->frameNo, false, chromeAgo)") &&
              has(vs, "const uint32_t chromeAgo = s->fssChromeFrame != 0 ? s->frameNo - s->fssChromeFrame : 0xFFFFFFFFu;") && has(vs, "fssTilesWantsChrome()"),
          "both zoom signals reach fss_tiles, which says why a zoom opened no window");

    const std::string rhs = slurp(root + "/src/d3d11/fss_ring_hook.cpp");
    check(!rhs.empty() && has(rhs, "fss tiles: ring hook inert --") && has(rhs, "fss tiles: ring hook armed at EliteDangerous64.exe+0x") &&
              has(rhs, "fss tiles: window %u ring hook -- armed,") && has(rhs, "the entry has NOT been reached this session") &&
              has(rhs, "it saw nothing because it was never armed"),
          "the ring hook says when it is inert and why, when it armed, and every window counts the calls it saw, zero included");
    check(has(rhs, "vertex_resync_core.h") && has(rhs, "vresync::identityMatches") && has(rhs, "sehCheckBytes(target, kPrologue, kPrologueBytes)") &&
              has(rhs, "g_hook.install(reinterpret_cast<void*>(target), g_relay, nullptr, \"fss-ring-pass\""),
          "the build identity and the prologue are checked before the CodeHook is placed, through the vertex resync's relay");
    check(has(rhs, "forward(ring, pass, camera, a4, a5, a6, a7, data);"),
          "every argument is forwarded unchanged (the observation is pinned in testV8)");
    check(has(rhs, "__try") && has(rhs, "readCall(") && has(rhs, "readMainId(") && has(rhs, "runtimeFlatProfile()"), "the reads are SEH-guarded and the flat profile is left alone");
    check(!has(rhs, "WriteProcessMemory"), "the observer does not write the game's memory itself (CodeHook places the one patch)");
    const std::string dhk = slurp(root + "/src/d3d11/device_hook.cpp");
    const size_t vr = dhk.find("vertexResyncInstall();");
    const size_t rr = dhk.find("fssRingHookInstall();", vr);
    check(vr != std::string::npos && rr != std::string::npos && rr > vr && rr - vr < 900, "the ring hook is installed at startup beside the vertex resync");
    const std::string bat = slurp(root + "/build.bat");
    check(has(bat, "src\\d3d11\\fss_tiles.cpp") && has(bat, "src\\d3d11\\fss_ring_hook.cpp"), "build.bat compiles fss_tiles.cpp and fss_ring_hook.cpp into the graphics DLL");

    const std::string cpp = slurp(root + "/src/d3d11/fss_tiles.cpp");
    check(has(cpp, "swapBack(ops, g.sw)") && has(cpp, "if (!ok || g.sw.swapped)"), "the fault path restores the bindings from outside the guarded dispatch");
    check(has(cpp, "D3D11_MAP_FLAG_DO_NOT_WAIT"), "the read-back never waits");
    check(has(cpp, "fssRingHookWindow(g.rv.win.capturing, g.windowNo, frameNo);") && has(cpp, "fssRingHookWindowSummary(g.windowNo);"),
          "the capture window is handed to the ring hook each frame and its summary is said with every window's");
    check(has(cpp, "fss tiles: window %u lists --") && has(cpp, "fss tiles: window %u writes --") &&
              has(cpp, "no clear, copy or update touched any of them") && has(cpp, "samples read=%u"),
          "every window closes with a lists line and a writes line, zero counts included, so nothing-happened reads differently from never-ran");
    check(has(cpp, "fss tiles: band event at f=%u") && has(cpp, "fss tiles: window %u band watch --") &&
              has(cpp, "an event needs a step of %u") && has(cpp, "Band watch: armed"),
          "the band watch says it armed, what it saw in every window (zero events included) and each event");
    {
        const std::string core = slurp(root + "/src/d3d11/fss_tiles_core.h");
        const size_t pend = core.find("pending[k] = true;");
        const size_t bnd = cpp.find("void fssTilesFrameBoundary(");
        const size_t arm = cpp.find("temporalPassArmEyeDump();", bnd);
        check(pend != std::string::npos && bnd != std::string::npos && arm != std::string::npos && arm > bnd &&
                  cpp.find("temporalPassArmEyeDump();") == arm && has(cpp, "if (o.fire & (1u << k)) {"),
              "the automatic eye run is only ever started from the frame boundary, never from inside the dispatch hook, and only for a run the machine fired");
        check(has(cpp, "g.rv.runs.sessionPlanned < kMaxAutoRuns") && has(cpp, "!g.windowEventFired") && has(cpp, "why == BandWatch::AllSmall") &&
                  has(core, "const uint32_t room = kMaxAutoRuns > sessionPlanned ? kMaxAutoRuns - sessionPlanned : 0;"),
              "one all-small event a window and at most kMaxAutoRuns planned runs a session");
        check(has(core, "revealUntil = frame + kRevealFrames;") && has(core, "if (o.ev == WindowMachine::Closed) o.ev = WindowMachine::None;") &&
                  has(core, "win.capturing = true;"),
              "after the event the capture continues for kRevealFrames whatever the zoom window does");
    }
    check(has(cpp, "fss tiles: gbuf f=%u") && has(cpp, "fss tiles: input f=%u eye%u") && has(cpp, "fss tiles: window %u upstream --") &&
              has(cpp, "fss tiles: window %u inputs --") && has(cpp, "no render target of the window matched them"),
          "the new lines exist, and every window says what the draw count and the fingerprints found, none included");
    check(!has(cpp, "RSGetViewports(&nv, &vp);\n            UINT ns = 1;\n            D3D11_RECT sc{};\n            ctx->RSGetScissorRects(&ns, &sc);\n            if (!d->haveVp) {\n                d->vp0 = vp;") ||
              has(cpp, "d->draws <= 2 || (d->draws & 15u) == 0"),
          "the viewport and scissor are read for the first draws and one in sixteen, not for every draw");
    check(has(cpp, "td.BindFlags & D3D11_BIND_DEPTH_STENCIL") && has(cpp, "td.SampleDesc.Count > 1"),
          "depth-stencil and multisampled textures are named, never copied");
    check(has(cpp, "fss tiles: window %u eyediff --") && has(cpp, "fss tiles: window %u eyediff unpaired --") && has(cpp, "fss tiles: window %u ring quad --") &&
              has(cpp, "never armed: no all-small event in this window") && has(cpp, "none: the two eyes' lists matched in every compared frame") &&
              has(cpp, "no draw matched the ring family"),
          "every window says what the eye diff compared and found, what went to unpaired targets, and what the ring family did, none included");
    check(has(slurp(root + "/src/d3d11/fss_tiles_core.h"), "ledgerOn = true;\n        ledgerUntil = frame + kLedgerFrames;") && has(slurp(root + "/src/d3d11/fss_tiles_core.h"), "if (ledgerOn && in.frame > ledgerUntil) ledgerOn = false;") &&
              has(cpp, "flushLedger(g.frame);"),
          "the ledger starts at the all-small event, runs kLedgerFrames and is flushed once a frame at the boundary");
    check(has(cpp, "eye_final_capture::g_reduceLevel = kReduceLevel;") && has(cpp, "eye_final_capture::g_reduceLevel = 0;"),
          "the automatic eye run reduces the final images and the reduction is switched off again");
    check(has(cpp, "fss tiles: ring quad f=%u") && has(cpp, "Role::Ring0"), "the ring quad's own inputs are watched and its draws logged per eye");
    check(has(cpp, "fss tiles: band event at f=%u refused: the scanner chrome is not held on this frame") &&
              has(cpp, "fss tiles: automatic eye run %u refused at f=%u: the scanner chrome is not held") &&
              has(cpp, "fss tiles: a %s at frame %u is not a window opener: the scanner chrome is not held") &&
              has(cpp, "chrome held=%s, last matched %s"),
          "the band event, each eye run and each jump or press the gate refuses is logged with its reason, and the window-opened line says whether the chrome was held");
    check(has(cpp, "fss tiles: window %u gate closed at f=%u: %s.") && has(cpp, "The reveal capture ends with it, and any pending eye run is cancelled.") &&
              has(slurp(root + "/src/d3d11/fss_tiles_core.h"), "gate.step(in.frame, in.chromeAgo, in.focusKnown, in.focus)") && has(cpp, "o.refused & (1u << k)"),
          "leaving the scanner or losing FSS focus closes the window and ends the capture, and an eye run waits for the chrome");
    // v11 source pins
    {
        const std::string c11 = slurp(root + "/src/d3d11/fss_tiles.cpp");
        const std::string d11 = slurp(root + "/src/d3d11/fss_tiles_d3d.h");
        check(has(c11, "captureCompositeInputs(ctx, eye, seq, kind, count);") && has(c11, "g.dumpFrom = frameNo;") && has(c11, "drainInputs(frameNo);") && has(c11, "captureInputs(dev, ctx, g.ctx1,") &&
                  has(c11, "compareInputs(set.f[0], set.d[0], set.f[1], set.d[1], &r, &g.inTot);"),
              "the composite's inputs are captured at the composite draw, read back later and compared between the eyes");
        check(has(c11, "fss tiles: composite inputs f=%u eye%u %s%s") && has(c11, "fss tiles: composite diff f=%u (arrival +%u): %s") && has(c11, "words that differ, eye0 | eye1") &&
                  has(c11, "composite diff -- inputs that DIFFERED between the eyes: %s | inputs that never differed: %s") && has(c11, "v11_f%u_e%u_t%u.pgm"),
              "the inputs lines, the per-frame diff, the differing words, the capture summary naming what differed and the images are written");
        check(has(d11, "ctx->PSGetShaderResources(0, kInSlots, sv);") && has(d11, "ctx1->VSGetConstantBuffers1(0, 3, b, first, num);") && has(d11, "ctx1->PSGetConstantBuffers1(0, 3, b, first, num);") &&
                  has(d11, "ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);") && has(d11, "ctx->PSGetSamplers(0, kInSamplers, sm);") && has(d11, "D3D11_MAP_FLAG_DO_NOT_WAIT"),
              "every pixel-shader texture, both stages' constant buffers over their bound range, the vertex buffer and the samplers are read, and the read-back never waits");
        check(has(c11, "fss tiles: composite producer f=%u eye%u's %s T%u %ux%u fmt%u: vs %016llX ps %016llX") && has(c11, "NO draw rendered into it this frame") && has(c11, "flushProducers(frame);") &&
                  has(c11, "producerNote(res, dvr ? dvr->res : nullptr, bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps), seq);") && has(c11, "lit blocks span x %u-%u y %u-%u") &&
                  has(c11, "const bool deciding = rel >= kProdFrom && rel <= kProdTo && k >= 1 && k <= 3;") && has(c11, "constexpr uint32_t kProdFrom = 9, kProdTo = 17;") &&
                  has(d11, "s.mostMip, s.firstSlice)) s.fpQueued = true;"),
              "t1-t3 are logged and imaged at full block resolution in every frame +9..+17, each with the span of its lit blocks, and the draws that render into them are listed with their camera or eye and their order against both composites");
    }
    // v10 source pins
    {
        const std::string c10 = slurp(root + "/src/d3d11/fss_tiles.cpp");
        check(has(c10, "g.dumpTo = frameNo + kDumpFrames - 1;") && has(c10, "const bool dumping = g.dumpTo != 0 && g.frame <= g.dumpTo;") && has(c10, "const uint32_t due = g.sched.takeAfter();") &&
                  has(c10, "if (eye < 2 && g.sched.composite(eye, seq) && dumping) {") && has(c10, "snapBefore(ctx, res, eye, seq);") && has(c10, "finishFrameSnaps(g.frame);"),
              "the dump window opens with the arrival, the composite is snapshotted before, the first draw after it takes the after-snapshot, the boundary the end-of-frame one");
        check(has(c10, "compositorCheck(ctx, rtv, res, shape, instances, seq, &isComposite);") && has(c10, "*composite = shape != 0 && e->w >= 512 && e->h >= 512;") &&
                  has(c10, "const int fam = suspectIndex(bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps));") && has(c10, "suspectRecord(ctx, rtv, res, fam, instances, seq);"),
              "the composite is the eye-sized draw that samples the layer and the suspect families are recorded by shader prefix");
        check(has(c10, "formatVerdict(line, sizeof(line), c.frame, c.eye, c.rtId, c.depthId, c.bw, c.bh, v);") && has(c10, "fss tiles: verdict f=%u pair -- black in eye0 and not in eye1") &&
                  has(c10, "fss tiles: suspect f=%u %s family %u") && has(c10, "fss tiles: suspect cb f=%u draw #%u family %u") && has(c10, "window %u ring dump -- %u eye-frame(s) queued") &&
                  has(c10, "window %u verdict asymmetry over %u frame(s) with both eyes") && has(c10, "v10_f%u_e%u_%S.pgm"),
              "the verdict line per eye-frame, the pair line, the suspect lines, the constant-buffer samples, the dump summary and the grey images are written");
        check(has(c10, "drainCaps(frameNo);") && has(c10, "drainCb(frameNo);") && has(c10, "D3D11_MAP_FLAG_DO_NOT_WAIT") && has(c10, "if (capsPending()) return true;"),
              "the dump's read-backs never wait and keep the summary pending");
        const std::string dh10 = slurp(root + "/src/d3d11/fss_tiles_d3d.h");
        check(has(dh10, "ctx->GenerateMips(srv);") && has(dh10, "ctx->CopyResource(*stage, depth);") && has(dh10, "D3D11_FORMAT_SUPPORT_MIP_AUTOGEN"), "the block reduction uses the mip chain and the depth is copied whole into a staging twin");
    }
    check(has(cpp, "fss tiles: eye-dump key at f=%u") && has(cpp, "outside a capturing zoom window"), "the eye-dump key's reception is logged either way");
    check(has(cpp, "fss tiles: window %u opened") && has(cpp, "fss tiles: window %u closed") && has(cpp, "fss tiles: instrument armed"),
          "the instrument says it armed, opened and closed, so never-ran reads differently from zero");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "--self-test") && std::strcmp(argv[1], "--dry-run"))) {
        std::puts("usage: fss_tiles_test --self-test [repo root] | --dry-run");
        return 2;
    }
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("Would run the fss tiles pure-half tests, a fake-context restore test, a WARP-device substitution test and source pins; writes no files.");
        return 0;
    }
    const std::string root = argc > 2 ? argv[2] : ".";

    testIdentities();
    testViews();
    testWindow();
    testSubstitutionRule();
    testLineAndTallies();
    testKeyParsing();
    testV2Pure();
    testBand();
    testV4Pure();
    testRingCore(root);
    testLedger();
    testGate();
    testV8(root);
    testV9Replay(root);
    testV10Pure();
    testV11Pure();
    testFakeRestore();

    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "System32's D3D11CreateDevice");
    if (createDevice) {
        Gpu g;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &level, &g.ctx);
        if (FAILED(hr)) {
            std::puts("(the D3D debug layer is not installed here: running without it)");
            hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &level, &g.ctx);
        }
        check(SUCCEEDED(hr), "a WARP device");
        check(edvr::reportSystemD3D11Only("fss_tiles_test"), "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
        if (SUCCEEDED(hr)) {
            testWarp(g);
            testStagingLike(g);
            testSampling(g);
            testReducedCapture(g);
            testV10Gpu(g);
            testV11Gpu(g);
            ComPtr<ID3D11InfoQueue> q;
            if (SUCCEEDED(g.dev.As(&q))) {
                const UINT64 cnt = q->GetNumStoredMessagesAllowedByRetrievalFilter();
                bool bad = false;
                for (UINT64 i = 0; i < cnt; ++i) {
                    SIZE_T len = 0;
                    q->GetMessage(i, nullptr, &len);
                    std::vector<char> raw(len);
                    auto* m = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
                    if (SUCCEEDED(q->GetMessage(i, m, &len)) &&
                        (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                        std::printf("  debug layer: %s\n", m->pDescription);
                        bad = true;
                    }
                }
                check(!bad, "the D3D debug layer stayed clean");
            }
        }
    }

    testSourcePins(root);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
