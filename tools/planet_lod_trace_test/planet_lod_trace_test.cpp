// The planet LOD trace (src/d3d11/planet_lod_trace_core.h, planet_lod_trace_hook.cpp; docs\planet-lod-streaming-2026-10-14.md). TEMPORARY instrument; delete with it.
//
//   T1  the identity check on synthetic images: build 332841's stamp and size with the five heads and call sites in place passes; a wrong stamp, a wrong size, one flipped head byte per
//       site and one wrong call target per site each refuse, and the reason names the site
//   T2  the pump state's two rings read from synthetic memory: counts, a wrapped ring, an implausible count, a vtable outside the image (skipped, never called)
//   T3  the real detours against stand-ins for the originals: every argument and result passes through unchanged; the counters see the call
//   T4  the window lines: a scripted session pinned byte for byte (idle, backlog, bound frames, grants by type, tree levels, requests); an idle stretch writes nothing for a minute and then
//       one line that says which hooks have EVER run, so a zero is a measurement and a silent hook is a hook that did not run
//   T5  the real CodeHook accepts the five real prologues (installed on synthetic copies of the 24 bytes read from the game)
//   T6  the wiring, read as text
//
//   planet_lod_trace_test --self-test | --dry-run
#include "../../src/d3d11/planet_lod_trace_core.h"
#include "../../src/d3d11/planet_lod_trace_hook.h"
#include "../../src/common/code_hook.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace plt = edvr::plt;

namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
}

static unsigned checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", what); std::exit(1); }
}
static std::string slurp(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}
static bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// ---- T1 -----------------------------------------------------------------------------------------------------------------------------------------------------
static uint8_t* makeImage(uint32_t stamp, uint32_t size) {
    auto* img = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(img != nullptr, "T1 a 100 MB synthetic image can be allocated");
    const uint32_t peOff = 0x80;
    std::memcpy(img + 0x3C, &peOff, 4);
    std::memcpy(img + peOff, "PE\0\0", 4);
    const uint16_t magic = 0x20B;
    std::memcpy(img + peOff + 24, &magic, 2);
    std::memcpy(img + peOff + 8, &stamp, 4);
    std::memcpy(img + peOff + 0x50, &size, 4);
    for (int i = 0; i < plt::kSiteCount; ++i) {
        const plt::SitePin& p = plt::kPins[i];
        std::memcpy(img + p.rva, p.head, plt::kHeadBytes);
        img[p.callRva] = 0xE8;
        const int32_t rel = int32_t(int64_t(p.rva) - int64_t(p.callRva) - 5);
        std::memcpy(img + p.callRva + 1, &rel, 4);
    }
    return img;
}

static void caseIdentity() {
    uint8_t* img = makeImage(plt::kExpectedTimestamp, plt::kExpectedImageSize);
    plt::Verdict v = plt::checkImage(img, plt::kExpectedImageSize);
    check(v.ok, "T1 build 332841's stamp, size, heads and call sites pass");
    {
        uint8_t* other = makeImage(plt::kExpectedTimestamp + 1, plt::kExpectedImageSize);
        v = plt::checkImage(other, plt::kExpectedImageSize);
        check(!v.ok && has(v.why, "not build 332841's"), "T1 another build's stamp is refused, and says whose it is not");
        VirtualFree(other, 0, MEM_RELEASE);
    }
    for (int i = 0; i < plt::kSiteCount; ++i) {
        const plt::SitePin& p = plt::kPins[i];
        const uint8_t saved = img[p.rva + 9];
        img[p.rva + 9] ^= 0x40;
        v = plt::checkImage(img, plt::kExpectedImageSize);
        // The first site whose pin fails is named; flipping site i's head fails at site i unless an earlier site's pin already failed (none did).
        check(!v.ok && has(v.why, p.name) && has(v.why, "head"), "T1 a flipped head byte refuses and names that function");
        img[p.rva + 9] = saved;
        const uint8_t savedCall = img[p.callRva + 1];
        img[p.callRva + 1] ^= 0x01;
        v = plt::checkImage(img, plt::kExpectedImageSize);
        check(!v.ok && has(v.why, p.name) && has(v.why, "call"), "T1 a call that lands elsewhere refuses and names that function");
        img[p.callRva + 1] = savedCall;
    }
    check(plt::checkImage(img, plt::kExpectedImageSize).ok, "T1 restored, it passes again");
    check(!plt::checkImage(nullptr, 0).ok, "T1 no module is a refusal, not a fault");
    VirtualFree(img, 0, MEM_RELEASE);
}

// ---- synthetic game memory for T2..T4 --------------------------------------------------------------------------------------------------------------------------
// One block stands for the game's image (the vtables live inside it); tasks are heap objects whose first word is such a vtable.
struct Block {
    alignas(16) uint8_t bytes[4096];
};
static Block g_image;
static int __fastcall prioConst1(void*) { return 1; }
static int __fastcall prioField(void* task) { return *reinterpret_cast<int*>(static_cast<uint8_t*>(task) + 0x24); }
static int g_called = 0;
static int __fastcall prioForbidden(void*) { ++g_called; return -999; }

struct Task { uintptr_t vt; uint8_t pad[0x1C]; int prio; };   // prio at +0x24
static uintptr_t makeVtable(size_t slotOffset, void* fn) {   // a vtable inside g_image: slot 9 (+0x48) is the priority getter
    auto* vt = reinterpret_cast<uintptr_t*>(g_image.bytes + slotOffset);
    std::memset(vt, 0, 0x60);
    vt[9] = reinterpret_cast<uintptr_t>(fn);
    return reinterpret_cast<uintptr_t>(vt);
}

struct Ring {
    uint8_t body[0x40];   // data +8, capacity +0x10, start +0x18, count +0x20
    void set(uintptr_t* data, uint64_t cap, uint64_t start, uint64_t count) {
        std::memset(body, 0, sizeof(body));
        const uint64_t d = reinterpret_cast<uintptr_t>(data);
        std::memcpy(body + 8, &d, 8); std::memcpy(body + 0x10, &cap, 8); std::memcpy(body + 0x18, &start, 8); std::memcpy(body + 0x20, &count, 8);
    }
};

static uint8_t g_state[0x120];
static Ring g_active;
static uintptr_t g_pendData[8], g_actData[4];
static void putQ(uint8_t* at, uintptr_t v) { std::memcpy(at, &v, 8); }
static void layout(uint64_t pendCap, uint64_t pendStart, uint64_t pendCount, uint64_t actCount) {
    std::memset(g_state, 0, sizeof(g_state));
    const uint32_t set = 0;
    std::memcpy(g_state + 0x18, &set, 4);
    g_active.set(g_actData, 4, 0, actCount);
    putQ(g_state + 0x48, reinterpret_cast<uintptr_t>(g_active.body));
    putQ(g_state + 0x28, reinterpret_cast<uintptr_t>(g_pendData));
    putQ(g_state + 0x30, pendCap);
    putQ(g_state + 0x38, pendStart);
    putQ(g_state + 0x40, pendCount);
}

static void caseRings() {
    static Task t[6];
    const uintptr_t vtField = makeVtable(0x000, reinterpret_cast<void*>(&prioField));
    const uintptr_t vtConst = makeVtable(0x100, reinterpret_cast<void*>(&prioConst1));
    for (int i = 0; i < 6; ++i) { t[i].vt = (i % 2) ? vtConst : vtField; t[i].prio = 10 + i; }
    // pending ring: capacity 8, start 6, count 4 -> elements at 6, 7, 0, 1 (a wrapped ring)
    g_pendData[6] = reinterpret_cast<uintptr_t>(&t[0]); g_pendData[7] = reinterpret_cast<uintptr_t>(&t[1]);
    g_pendData[0] = reinterpret_cast<uintptr_t>(&t[2]); g_pendData[1] = reinterpret_cast<uintptr_t>(&t[3]);
    g_actData[0] = reinterpret_cast<uintptr_t>(&t[4]); g_actData[1] = reinterpret_cast<uintptr_t>(&t[5]);
    layout(8, 6, 4, 2);
    plt::RingCounts c = plt::readCounts(g_state);
    check(c.ok && c.pending == 4 && c.active == 2, "T2 the two ring counts are read from state+40h and the active ring's +20h");
    plt::QueueSample s;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(g_image.bytes), hi = lo + sizeof(g_image.bytes);
    auto pf = [](uintptr_t task, uintptr_t vt) -> int { using Fn = int(__fastcall*)(void*); return (*reinterpret_cast<const Fn*>(vt + 0x48))(reinterpret_cast<void*>(task)); };
    plt::sampleRing(c.pendingBase, lo, hi, pf, &s);
    plt::sampleRing(c.activeBase, lo, hi, pf, &s);
    check(s.n == 6, "T2 all six tasks are sampled");
    check(s.task[0] == reinterpret_cast<uintptr_t>(&t[0]) && s.task[1] == reinterpret_cast<uintptr_t>(&t[1]) && s.task[2] == reinterpret_cast<uintptr_t>(&t[2]) &&
              s.task[3] == reinterpret_cast<uintptr_t>(&t[3]), "T2 a wrapped ring is read in queue order (start 6, then 7, then 0, 1)");
    check(s.prio[0] == 10 && s.prio[1] == 1 && s.prio[2] == 12 && s.prio[3] == 1 && s.prio[4] == 14 && s.prio[5] == 1, "T2 the priority is the task vtable's +48h getter");
    // an implausible count and a bad state are refused
    layout(8, 0, 4, 2);
    uint64_t huge = 1ull << 40;
    putQ(g_state + 0x40, huge);
    check(!plt::readCounts(g_state).ok, "T2 an implausible pending count is not a ring");
    layout(8, 0, 4, 2);
    uint32_t badSet = 9;
    std::memcpy(g_state + 0x18, &badSet, 4);
    check(!plt::readCounts(g_state).ok, "T2 a set index past 7 is not a state");
    // a vtable outside the image is skipped and its getter never runs
    static Task evil;
    static uintptr_t outsideVt[16];
    outsideVt[9] = reinterpret_cast<uintptr_t>(&prioForbidden);
    evil.vt = reinterpret_cast<uintptr_t>(outsideVt);
    layout(8, 0, 1, 0);
    g_pendData[0] = reinterpret_cast<uintptr_t>(&evil);
    plt::QueueSample s2;
    c = plt::readCounts(g_state);
    plt::sampleRing(c.pendingBase, lo, hi, pf, &s2);
    check(s2.n == 0 && g_called == 0, "T2 a task whose vtable is outside the game's image is skipped and its getter is never called");
}

// ---- T3 / T4: the real detours ----------------------------------------------------------------------------------------------------------------------------------
struct Seen {
    uintptr_t a = 0, b = 0, c = 0, d = 0; double dbl = 0; float flt = 0; uint32_t u = 0; int i = 0; uint8_t byte = 0; void* p5 = nullptr; void* p7 = nullptr; int calls = 0;
};
static Seen g_seenBudget, g_seenPump, g_seenGrant, g_seenNode, g_seenReq;
static uint32_t g_grantOut[2];
static int g_pumpDrain = 0;

static void __fastcall fakeBudget(uintptr_t a, uintptr_t b, uint64_t c, uintptr_t d, double T, float mult) {
    g_seenBudget = Seen{a, b, c, d, T, mult, 0, 0, 0, nullptr, nullptr, g_seenBudget.calls + 1};
}
static double __fastcall fakePump(uintptr_t st, uintptr_t ctx, double budget, uintptr_t r9) {
    g_seenPump = Seen{st, ctx, r9, 0, budget, 0, 0, 0, 0, nullptr, nullptr, g_seenPump.calls + 1};
    if (g_pumpDrain) {   // the pump drains the pending ring by g_pumpDrain tasks
        uint64_t n;
        std::memcpy(&n, g_state + 0x40, 8);
        n = n > uint64_t(g_pumpDrain) ? n - uint64_t(g_pumpDrain) : 0;
        std::memcpy(g_state + 0x40, &n, 8);
    }
    return budget * 0.5;
}
static uint32_t* __fastcall fakeGrant(uintptr_t table, uint32_t* out, uint16_t* typeId, uint32_t gran, uint32_t avail, float remaining) {
    g_seenGrant = Seen{table, reinterpret_cast<uintptr_t>(out), reinterpret_cast<uintptr_t>(typeId), 0, 0, remaining, gran, int(avail), 0, nullptr, nullptr, g_seenGrant.calls + 1};
    return out;
}
static void __fastcall fakeNode(uintptr_t node, uint8_t* o, uintptr_t ctx, int p4, void* p5, uint8_t p6, void* p7, double p8) {
    g_seenNode = Seen{node, reinterpret_cast<uintptr_t>(o), ctx, 0, p8, 0, 0, p4, p6, p5, p7, g_seenNode.calls + 1};
}
static uint64_t g_reqHandle;   // what the fake request submit writes into node+30h
static void __fastcall fakeReq(uintptr_t node, uintptr_t ctx, uint32_t p3, double p4, int p5) {
    g_seenReq = Seen{node, ctx, 0, 0, p4, 0, p3, p5, 0, nullptr, nullptr, g_seenReq.calls + 1};
    if (g_reqHandle) std::memcpy(reinterpret_cast<uint8_t*>(node) + 0x30, &g_reqHandle, 8);
}

using BudgetFn = void(__fastcall*)(uintptr_t, uintptr_t, uint64_t, uintptr_t, double, float);
using PumpFn = double(__fastcall*)(uintptr_t, uintptr_t, double, uintptr_t);
using GrantFn = uint32_t*(__fastcall*)(uintptr_t, uint32_t*, uint16_t*, uint32_t, uint32_t, float);
using NodeFn = void(__fastcall*)(uintptr_t, uint8_t*, uintptr_t, int, void*, uint8_t, void*, double);
using ReqFn = void(__fastcall*)(uintptr_t, uintptr_t, uint32_t, double, int);

static std::vector<std::string> lines() {
    std::vector<std::string> out;
    for (size_t i = 0; i < edvr::planetLodTraceTestLineCount(); ++i) out.emplace_back(edvr::planetLodTraceTestLine(i));
    return out;
}
static bool anyLine(const std::vector<std::string>& v, const char* prefix) {
    for (const auto& l : v) if (l.compare(0, std::strlen(prefix), prefix) == 0) return true;
    return false;
}
static std::string lineWith(const std::vector<std::string>& v, const char* prefix) {
    for (const auto& l : v) if (l.compare(0, std::strlen(prefix), prefix) == 0) return l;
    return std::string();
}

static void wire() {
    edvr::planetLodTraceTestReset();
    edvr::planetLodTraceTestSetImage(reinterpret_cast<uintptr_t>(g_image.bytes), reinterpret_cast<uintptr_t>(g_image.bytes) + sizeof(g_image.bytes));
    edvr::planetLodTraceTestSetForward(plt::kBudget, reinterpret_cast<uintptr_t>(&fakeBudget));
    edvr::planetLodTraceTestSetForward(plt::kPump, reinterpret_cast<uintptr_t>(&fakePump));
    edvr::planetLodTraceTestSetForward(plt::kGrant, reinterpret_cast<uintptr_t>(&fakeGrant));
    edvr::planetLodTraceTestSetForward(plt::kNode, reinterpret_cast<uintptr_t>(&fakeNode));
    edvr::planetLodTraceTestSetForward(plt::kRequest, reinterpret_cast<uintptr_t>(&fakeReq));
    edvr::planetLodTraceTestMarkLive();
    g_seenBudget = g_seenPump = g_seenGrant = g_seenNode = g_seenReq = Seen{};
}

static void caseDetours() {
    wire();
    const auto budget = reinterpret_cast<BudgetFn>(edvr::planetLodTraceTestDetour(plt::kBudget));
    const auto pump = reinterpret_cast<PumpFn>(edvr::planetLodTraceTestDetour(plt::kPump));
    const auto grant = reinterpret_cast<GrantFn>(edvr::planetLodTraceTestDetour(plt::kGrant));
    const auto node = reinterpret_cast<NodeFn>(edvr::planetLodTraceTestDetour(plt::kNode));
    const auto req = reinterpret_cast<ReqFn>(edvr::planetLodTraceTestDetour(plt::kRequest));
    check(budget && pump && grant && node && req, "T3 the five detours exist");

    static uint8_t front[0x40];
    front[0x20] = 1;
    budget(reinterpret_cast<uintptr_t>(front), 0x1111, 1, 0x2222, 11.125, 0.36075f);
    check(g_seenBudget.calls == 1 && g_seenBudget.a == reinterpret_cast<uintptr_t>(front) && g_seenBudget.b == 0x1111 && g_seenBudget.c == 1 && g_seenBudget.d == 0x2222 &&
              g_seenBudget.dbl == 11.125 && g_seenBudget.flt == 0.36075f, "T3 the budget call reaches the original with all six arguments as given (the double and the float bit for bit)");

    // an empty pump: the original runs, the answer is its answer, and it is counted as idle
    layout(8, 0, 0, 0);
    double r = pump(reinterpret_cast<uintptr_t>(g_state), 0x3333, 2.75, 0);
    check(r == 1.375 && g_seenPump.calls == 1 && g_seenPump.a == reinterpret_cast<uintptr_t>(g_state) && g_seenPump.b == 0x3333 && g_seenPump.dbl == 2.75, "T3 an idle pump is forwarded with its arguments and returns the original's value");

    // a pump with a backlog: three pending + one active before, drains two
    static Task t[4];
    const uintptr_t vt = makeVtable(0x000, reinterpret_cast<void*>(&prioField));
    for (int i = 0; i < 4; ++i) { t[i].vt = vt; t[i].prio = 100 + (i % 2); }
    g_pendData[0] = reinterpret_cast<uintptr_t>(&t[0]); g_pendData[1] = reinterpret_cast<uintptr_t>(&t[1]); g_pendData[2] = reinterpret_cast<uintptr_t>(&t[2]);
    g_actData[0] = reinterpret_cast<uintptr_t>(&t[3]);
    layout(8, 0, 3, 1);
    g_pumpDrain = 2;
    r = pump(reinterpret_cast<uintptr_t>(g_state), 0x3333, 3.0, 0);
    g_pumpDrain = 0;
    check(r == 1.5 && g_seenPump.calls == 2, "T3 a busy pump returns the original's value");

    // grants: a typed grant with work, and one with none
    uint16_t typeA = 0x1234, typeB = 0x00AB;
    float ms1 = 0.20f;
    g_grantOut[0] = 17216; std::memcpy(&g_grantOut[1], &ms1, 4);
    uint32_t* gr = grant(0x4444, g_grantOut, &typeA, 64, 20000, 2.5f);
    check(gr == g_grantOut && g_seenGrant.a == 0x4444 && g_seenGrant.c == reinterpret_cast<uintptr_t>(&typeA) && g_seenGrant.u == 64 && g_seenGrant.i == 20000 && g_seenGrant.flt == 2.5f,
          "T3 the grant is forwarded with its six arguments and its out pointer comes back");
    grant(0x4444, g_grantOut, &typeA, 64, 20000, 2.0f);
    g_grantOut[0] = 0; float z = 0.0f; std::memcpy(&g_grantOut[1], &z, 4);
    grant(0x4444, g_grantOut, &typeB, 256, 9000, 1.0f);

    // nodes: level 3 pending (state 1) wanting children, level 3 ready, level 5 none
    static uint8_t nodeMem[3][0x400];
    auto setNode = [](int n, uint8_t level, uint32_t state, uint16_t flags, uint64_t children) {
        std::memset(nodeMem[n], 0, sizeof(nodeMem[n]));
        nodeMem[n][0x2B7] = level; std::memcpy(nodeMem[n] + 0x158, &state, 4); std::memcpy(nodeMem[n] + 0x2B4, &flags, 2); std::memcpy(nodeMem[n] + 8, &children, 8);
    };
    setNode(0, 3, 1, 0x400, 0x1000); setNode(1, 3, 2, 0x200, 0); setNode(2, 5, 0, 0, 0);
    uint8_t out2[2] = {};
    float f1 = 1.0f, f2 = 2.0f;
    for (int i = 0; i < 3; ++i) node(reinterpret_cast<uintptr_t>(nodeMem[i]), out2, 0x5555, 7, &f1, 0x9C, &f2, 4.5);
    check(g_seenNode.calls == 3 && g_seenNode.c == 0x5555 && g_seenNode.i == 7 && g_seenNode.p5 == &f1 && g_seenNode.p7 == &f2 && g_seenNode.byte == 0x9C && g_seenNode.dbl == 4.5,
          "T3 the node update is forwarded with its eight arguments (int, two pointers, a byte and a double on the stack)");

    // requests: one that makes a handle, one that finds one
    static uint8_t reqNode[0x400];
    std::memset(reqNode, 0, sizeof(reqNode));
    reqNode[0x2B7] = 4;
    g_reqHandle = 0xABCD0;
    req(reinterpret_cast<uintptr_t>(reqNode), 0x6666, 0x3F800000u, 1.5, 9);
    req(reinterpret_cast<uintptr_t>(reqNode), 0x6666, 0x3F800000u, 1.5, 9);
    g_reqHandle = 0;
    check(g_seenReq.calls == 2 && g_seenReq.b == 0x6666 && g_seenReq.u == 0x3F800000u && g_seenReq.dbl == 1.5 && g_seenReq.i == 9, "T3 the request submit is forwarded with its five arguments");
}

static void caseLines() {
    wire();
    const auto budget = reinterpret_cast<BudgetFn>(edvr::planetLodTraceTestDetour(plt::kBudget));
    const auto pump = reinterpret_cast<PumpFn>(edvr::planetLodTraceTestDetour(plt::kPump));
    const auto grant = reinterpret_cast<GrantFn>(edvr::planetLodTraceTestDetour(plt::kGrant));
    const auto node = reinterpret_cast<NodeFn>(edvr::planetLodTraceTestDetour(plt::kNode));
    const auto req = reinterpret_cast<ReqFn>(edvr::planetLodTraceTestDetour(plt::kRequest));

    // WINDOW 0: nothing queued. No line for the first minute.
    static uint8_t front[0x40];
    edvr::planetLodTracePoll(1000);   // starts the window
    for (int i = 0; i < 10; ++i) { layout(8, 0, 0, 0); budget(reinterpret_cast<uintptr_t>(front), 0, 0, 0, 11.111, 0.36075f); pump(reinterpret_cast<uintptr_t>(g_state), 0, 3.15, 0); }
    edvr::planetLodTracePoll(6000);
    check(lines().empty(), "T4 a window with nothing queued writes no line");
    edvr::planetLodTracePoll(61000);
    edvr::planetLodTracePoll(66500);
    {
        const auto v = lines();
        const std::string idle = lineWith(v, "planet lod trace: idle:");
        check(v.size() == 1 && !idle.empty() && has(idle.c_str(), "budget=10 pump=10 grant=0 node=0 request=0") && has(idle.c_str(), "pump calls with empty rings=0"),
              "T4 after a minute of nothing queued, one idle line says which hooks have EVER run (budget 10, pump 10, the rest 0)");
        check(has(idle.c_str(), "hooks ever run"), "T4 ...and says it is the lifetime count");
    }

    // WINDOW 1: a backlog of 20 frames: each pump finds 4 tasks (3 pending + 1 active), drains 1, uses 90 percent of a 3.0 ms budget on the 2nd half of frames
    edvr::planetLodTraceTestReset();
    wire();
    edvr::planetLodTracePoll(100000);
    static Task t[4];
    const uintptr_t vt = makeVtable(0x000, reinterpret_cast<void*>(&prioField));
    for (int i = 0; i < 4; ++i) { t[i].vt = vt; t[i].prio = (i < 2) ? 100 : 200; }
    uint16_t typeA = 0x1234, typeB = 0x00AB;
    float msA = 0.20f, msB = 0.05f;
    static uint8_t nodeMem[2][0x400];
    for (int n = 0; n < 2; ++n) { std::memset(nodeMem[n], 0, sizeof(nodeMem[n])); nodeMem[n][0x2B7] = uint8_t(6 + n); uint32_t st = uint32_t(1 + n); std::memcpy(nodeMem[n] + 0x158, &st, 4); }
    uint16_t want = 0x400; std::memcpy(nodeMem[0] + 0x2B4, &want, 2);
    uint8_t o[2] = {};
    float f = 1.0f;
    for (int frame = 0; frame < 20; ++frame) {
        g_pendData[0] = reinterpret_cast<uintptr_t>(&t[0]); g_pendData[1] = reinterpret_cast<uintptr_t>(&t[1]); g_pendData[2] = reinterpret_cast<uintptr_t>(&t[2]);
        g_actData[0] = reinterpret_cast<uintptr_t>(&t[3]);
        layout(8, 0, 3, 1);
        g_pumpDrain = 1;
        budget(reinterpret_cast<uintptr_t>(front), 0, 0, 0, 11.111, 0.375f);
        // the original pump would call the grant once per task it considers: two per frame, one with work
        g_grantOut[0] = 17216; std::memcpy(&g_grantOut[1], &msA, 4); grant(0, g_grantOut, &typeA, 64, 20000, 3.0f);
        g_grantOut[0] = 256;   std::memcpy(&g_grantOut[1], &msB, 4); grant(0, g_grantOut, &typeB, 256, 20000, 2.8f);
        pump(reinterpret_cast<uintptr_t>(g_state), 0, 3.0, 0);   // returns 1.5 (half the budget): no frame is "bound"
        for (int n = 0; n < 2; ++n) node(reinterpret_cast<uintptr_t>(nodeMem[n]), o, 0, 0, &f, 0, &f, 0.0);
        g_pumpDrain = 0;
    }
    static uint8_t reqNode[0x400];
    std::memset(reqNode, 0, sizeof(reqNode)); reqNode[0x2B7] = 7;
    g_reqHandle = 0x77;
    req(reinterpret_cast<uintptr_t>(reqNode), 0, 0, 0.0, 0);
    g_reqHandle = 0;
    edvr::planetLodTracePoll(105200);
    const auto v = lines();
    check(v.size() == 4, "T4 a busy window writes four lines: summary, budget call, tree, threads");
    const std::string sum = lineWith(v, "planet lod trace: 5.2 s:");
    const std::string head = "planet lod trace: 5.2 s: pumps=20 idle=0 budget_ms avg=3.00 max=3.00 used_est_ms avg=1.50 max=1.50 bound=0% zero_budget=0 tasks considered=40 run=40 (2.0/frame) "
                             "queue_after pending avg=2.0 max=2 active avg=1.0 max=1 backlog=100% streak_max_ms=";
    check(sum.compare(0, head.size(), head) == 0, "T4 the summary line, field by field up to the wall-clock streak (budget 3.00, used 1.50, 2.0 tasks a frame, queue after 2 pending and 1 active, backlog 100%)");
    check(has(sum.c_str(), " oldest_age_ms=0 prio=3 of [100:2,200:1]"), "T4 ...then the oldest age (one sample: 0) and the sampled priorities (value:count)");
    const std::string b = lineWith(v, "planet lod trace: budget call:");
    check(b == "planet lod trace: budget call: n=20 T_ms=11.11..11.11 multiplier=0.3750..0.3750 boost_seen=0 est_ms_per_run=0.125 work_per_run=8736 "
               "types=[0x1234/64:20:344320:4.00 0x00AB/256:20:5120:1.00]",
          "T4 the budget-call line: T, multiplier, boost, the cost and the work of a granted task, and the task types by estimated ms (id/size:runs:work:ms)");
    const std::string tree = lineWith(v, "planet lod trace: tree:");
    check(tree == "planet lod trace: tree: nodes=40 (2/frame) wanting_finer=20 children=0 requests=1 new_handles=1 by_level [6:0/20/0 7:0/0/20] new_by_level [7:1]",
          "T4 the tree line: nodes per frame, nodes wanting finer, nodes by level and state (none/pending/ready), requests and the new handles by level");
    check(anyLine(v, "planet lod trace: threads ("), "T4 the threads line is written");
    // a following busy window carries percentages
    for (int frame = 0; frame < 3; ++frame) { layout(8, 0, 3, 1); pump(reinterpret_cast<uintptr_t>(g_state), 0, 3.0, 0); }
    edvr::planetLodTracePoll(110300);
    check(lines().size() == 7, "T4 the next busy window writes three lines (no node was visited, so no tree line)");
    check(has(lines().back().c_str(), "busy = CPU/wall") && !has(lines().back().c_str(), "first census"), "T4 ...and its threads line has percentages (a census from the window before)");
}

// ---- T5 ------------------------------------------------------------------------------------------------------------------------------------------------------
static void caseCodeHook() {
    for (int i = 0; i < plt::kSiteCount; ++i) {
        auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        check(mem != nullptr, "T5 executable memory for a synthetic prologue");
        std::memset(mem, 0xCC, 4096);
        std::memcpy(mem, plt::kPins[i].head, plt::kHeadBytes);
        // the replacement within reach of the target (a five-byte E9), as the production relay is
        uint8_t* repl = nullptr;
        for (size_t d = 0x100000; d <= 0x4000000 && !repl; d += 0x100000)
            repl = static_cast<uint8_t*>(VirtualAlloc(mem + d, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        check(repl != nullptr, "T5 a replacement page near the synthetic prologue");
        repl[0] = 0xC3;
        edvr::CodeHook hook;
        const bool ok = hook.install(mem, repl, nullptr, "planet-lod-trace-test");
        char what[160];
        std::snprintf(what, sizeof(what), "T5 CodeHook accepts the real %s prologue (24 bytes read from the game)", plt::kPins[i].name);
        check(ok && hook.installed() && hook.stolenBytes() >= 5, what);
        hook.uninstall();
        check(std::memcmp(mem, plt::kPins[i].head, plt::kHeadBytes) == 0, "T5 uninstall restores the bytes");
        VirtualFree(mem, 0, MEM_RELEASE);
        VirtualFree(repl, 0, MEM_RELEASE);
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("planet_lod_trace_test: dry-run (no module patched, no file)");
        return 0;
    }
    if (argc < 2 || std::string(argv[1]) != "--self-test") return 2;
    const std::string root = argc >= 3 ? argv[2] : std::string(".");

    caseIdentity();
    caseRings();
    caseDetours();
    caseLines();
    caseCodeHook();

    // T6 the wiring
    {
        const std::string device = slurp(root + "\\src\\d3d11\\device_hook.cpp");
        const std::string build = slurp(root + "\\build.bat");
        if (!device.empty() && !build.empty()) {
            check(has(device, "planetLodTraceInstall();") && has(device, "planetLodTracePoll(stampMs());"), "T6 device_hook.cpp installs the trace and polls it once a second");
            check(has(build, "\"src\\d3d11\\planet_lod_trace_hook.cpp\" ^"), "T6 build.bat compiles the hook into the DLL");
        }
    }
    std::printf("planet_lod_trace_test: %u checks passed\n", checks);
    return 0;
}
