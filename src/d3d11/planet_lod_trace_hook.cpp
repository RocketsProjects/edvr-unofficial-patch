#include "planet_lod_trace_hook.h"
#include "planet_lod_trace_core.h"
#include "../common/code_hook.h"
#include "../common/log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// The planet LOD trace (planet_lod_trace_core.h). Glue: pin the five sites, patch each with the project's CodeHook through a relay, and in each detour COUNT and forward. Nothing is
// changed: every argument and every result goes through as the game made it.

namespace edvr {
namespace {

// --- Relay machinery, the same copy terrain_cull_cover_hook.cpp carries (the target is in the game's module and this DLL loads more than two gigabytes away).
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]. Every site's first instruction either overwrites RAX or ignores it (these are
    // not variadic: RAX carries nothing), and no stack adjustment or nonvolatile register is touched, so a detour sees the arguments as the caller left them.
    const uint8_t body[kRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0,
        0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

struct SiteState {
    uint8_t* relay = nullptr;
    std::atomic<uintptr_t> forward{0};
    std::atomic<uintptr_t> gate{0};   // 0: the relay forwards straight to the original
    CodeHook hook;
};
SiteState g_site[plt::kSiteCount];

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto* s = static_cast<SiteState*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(s->relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(s->relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), s->relay, kRelayBytes)) return false;
    s->forward.store(address, std::memory_order_release);
    return true;
}

// --- State ------------------------------------------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_installed{false};
plt::Trace g_trace;
uintptr_t g_imgLo = 0, g_imgHi = 0;

// The poll thread's own.
bool g_pollStarted = false;
uint64_t g_windowStartMs = 0, g_lastIdleSayMs = 0;

#ifdef EDVR_PLT_TEST
std::mutex g_testMutex;
std::vector<std::string> g_testLines;
#endif

void say(const char* line) {
    Log::get().note("%s", line);
#ifdef EDVR_PLT_TEST
    std::lock_guard<std::mutex> lock(g_testMutex);
    g_testLines.emplace_back(line);
#endif
}

template <class Fn> Fn forwardOf(int site) { return reinterpret_cast<Fn>(g_site[site].forward.load(std::memory_order_acquire)); }

// --- Guarded reads of the game's memory (a freed or moved object must cost a count, never a fault) -------------------------------------------------------------------------
__declspec(noinline) bool guardedCounts(uintptr_t st, plt::RingCounts* out) noexcept {
    __try {
        *out = plt::readCounts(reinterpret_cast<const uint8_t*>(st));
        return out->ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __fastcall priorityOf(uintptr_t task, uintptr_t vtable) {
    using Fn = int(__fastcall*)(void*);
    const auto f = *reinterpret_cast<const Fn*>(vtable + 0x48);
    return f(reinterpret_cast<void*>(task));
}
// The rig can put a stand-in here. The hook calls the game's priority getter (vtable +0x48) only on a task whose vtable lies inside the game's image.
int priorityThunk(uintptr_t task, uintptr_t vtable) { return priorityOf(task, vtable); }

__declspec(noinline) bool guardedSample(const plt::RingCounts& c, plt::QueueSample* s) noexcept {
    __try {
        if (c.pending) plt::sampleRing(c.pendingBase, g_imgLo, g_imgHi, &priorityThunk, s);
        if (c.active) plt::sampleRing(c.activeBase, g_imgLo, g_imgHi, &priorityThunk, s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool guardedByte(uintptr_t at, uint8_t* out) noexcept {
    __try { *out = *reinterpret_cast<const uint8_t*>(at); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct NodeRead { uint32_t level = 0, state = 0, flags = 0; bool children = false; };
__declspec(noinline) bool guardedNode(uintptr_t node, NodeRead* out) noexcept {
    __try {
        const auto* p = reinterpret_cast<const uint8_t*>(node);
        out->level = p[0x2B7];
        out->state = plt::load32(p + 0x158);
        out->flags = plt::load16(p + 0x2B4);
        out->children = plt::load64(p + 8) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool guardedHandle(uintptr_t node, uint64_t* out, uint32_t* level) noexcept {
    __try {
        const auto* p = reinterpret_cast<const uint8_t*>(node);
        *out = plt::load64(p + 0x30);
        *level = p[0x2B7];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool guardedGrant(const uint32_t* out, const uint16_t* typeId, uint32_t* work, float* ms, uint16_t* id) noexcept {
    __try {
        *work = out[0];
        std::memcpy(ms, out + 1, 4);
        *id = *typeId;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- The detours ----------------------------------------------------------------------------------------------------------------------------------------------
using BudgetFn = void(__fastcall*)(uintptr_t, uintptr_t, uint64_t, uintptr_t, double, float);
__declspec(noinline) void __fastcall budgetDetour(uintptr_t front, uintptr_t ctx, uint64_t flag, uintptr_t stamp, double T, float mult) noexcept {
    const auto forward = forwardOf<BudgetFn>(plt::kBudget);
    if (!forward) return;
    uint8_t boost = 0;
    guardedByte(front + 0x20, &boost);
    g_trace.noteBudgetCall(T, mult, boost != 0);
    forward(front, ctx, flag, stamp, T, mult);
}

using PumpFn = double(__fastcall*)(uintptr_t, uintptr_t, double, uintptr_t);
__declspec(noinline) double __fastcall pumpDetour(uintptr_t st, uintptr_t ctx, double budget, uintptr_t r9) noexcept {
    const auto forward = forwardOf<PumpFn>(plt::kPump);
    if (!forward) return 0.0;
    plt::RingCounts before;
    if (!guardedCounts(st, &before)) return forward(st, ctx, budget, r9);   // unreadable: the game's call, uncounted
    if (before.pending == 0 && before.active == 0) {
        g_trace.notePump(true, 0.0, 0.0, 0, 0, 0);   // the pump returns at once; so do we
        return forward(st, ctx, budget, r9);
    }
    const double used = forward(st, ctx, budget, r9);
    plt::RingCounts after;
    uint64_t pa = 0, aa = 0;
    const bool haveAfter = guardedCounts(st, &after);
    if (haveAfter) { pa = after.pending; aa = after.active; }
    const uint64_t now = GetTickCount64();
    g_trace.notePump(false, budget, used, pa, aa, now);
    if (haveAfter && pa + aa > 0 && (g_trace.lastSampleMs == 0 || now - g_trace.lastSampleMs >= 250)) {
        static std::atomic<bool> busy{false};
        static plt::QueueSample sample;
        if (!busy.exchange(true, std::memory_order_acquire)) {
            sample.n = 0;
            if (guardedSample(after, &sample)) g_trace.noteSample(sample, now);
            busy.store(false, std::memory_order_release);
        }
    }
    return used;
}

using GrantFn = uint32_t*(__fastcall*)(uintptr_t, uint32_t*, uint16_t*, uint32_t, uint32_t, float);
__declspec(noinline) uint32_t* __fastcall grantDetour(uintptr_t table, uint32_t* out, uint16_t* typeId, uint32_t gran, uint32_t avail, float remaining) noexcept {
    const auto forward = forwardOf<GrantFn>(plt::kGrant);
    if (!forward) return out;
    uint32_t* const r = forward(table, out, typeId, gran, avail, remaining);
    uint32_t work = 0;
    float ms = 0.0f;
    uint16_t id = 0;
    if (r && guardedGrant(r, typeId, &work, &ms, &id)) g_trace.noteGrant(id, gran, work, ms);
    return r;
}

using NodeFn = void(__fastcall*)(uintptr_t, uint8_t*, uintptr_t, int, void*, uint8_t, void*, double);
__declspec(noinline) void __fastcall nodeDetour(uintptr_t node, uint8_t* o, uintptr_t ctx, int p4, void* p5, uint8_t p6, void* p7, double p8) noexcept {
    const auto forward = forwardOf<NodeFn>(plt::kNode);
    if (!forward) return;
    forward(node, o, ctx, p4, p5, p6, p7, p8);
    NodeRead n;
    if (guardedNode(node, &n)) g_trace.noteNode(n.level, n.state, n.flags, n.children);
}

using RequestFn = void(__fastcall*)(uintptr_t, uintptr_t, uint32_t, double, int);
__declspec(noinline) void __fastcall requestDetour(uintptr_t node, uintptr_t ctx, uint32_t p3, double p4, int p5) noexcept {
    const auto forward = forwardOf<RequestFn>(plt::kRequest);
    if (!forward) return;
    uint64_t before = 0, after = 0;
    uint32_t level = 0, level2 = 0;
    const bool haveBefore = guardedHandle(node, &before, &level);
    forward(node, ctx, p3, p4, p5);
    const bool haveAfter = guardedHandle(node, &after, &level2);
    g_trace.noteRequest(level2, haveBefore && haveAfter && before == 0 && after != 0);
}

void* const kDetours[plt::kSiteCount] = {
    reinterpret_cast<void*>(&budgetDetour), reinterpret_cast<void*>(&pumpDetour), reinterpret_cast<void*>(&grantDetour),
    reinterpret_cast<void*>(&nodeDetour), reinterpret_cast<void*>(&requestDetour)};

// --- Threads: which groups of threads are busy while work is queued ----------------------------------------------------------------------------------------------------
using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
struct ThreadRec { DWORD tid = 0; uint64_t cpu = 0; uintptr_t start = 0; };
std::vector<ThreadRec> g_prevThreads;
uint64_t g_prevThreadsMs = 0;

std::vector<ThreadRec> threadCensus() {
    std::vector<ThreadRec> out;
    static NtQueryInformationThreadFn query = reinterpret_cast<NtQueryInformationThreadFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 e{};
    e.dwSize = sizeof(e);
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &e)) {
        do {
            if (e.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, e.th32ThreadID);
            if (!h) h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID);
            if (!h) continue;
            ThreadRec r;
            r.tid = e.th32ThreadID;
            FILETIME c{}, x{}, k{}, u{};
            if (GetThreadTimes(h, &c, &x, &k, &u)) {
                r.cpu = ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
                PVOID start = nullptr;
                if (query && query(h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) >= 0) r.start = reinterpret_cast<uintptr_t>(start);
                out.push_back(r);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

std::string startLabel(uintptr_t start) {
    if (!start) return "unknown";
    HMODULE m = nullptr;
    char name[MAX_PATH] = {};
    char line[MAX_PATH + 40];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(start), &m) && m &&
        GetModuleFileNameA(m, name, MAX_PATH)) {
        const char* base = std::strrchr(name, '\\');
        std::snprintf(line, sizeof(line), "%s+0x%llX", base ? base + 1 : name, (unsigned long long)(start - reinterpret_cast<uintptr_t>(m)));
        return line;
    }
    std::snprintf(line, sizeof(line), "0x%llX", (unsigned long long)start);
    return line;
}

// `planet lod trace: threads (busy = CPU time / wall time of the window, one core = 100%): <start> x<count> sum=<pct> max=<pct> | ...` top six groups by sum; the first window after
// an idle stretch has no earlier census and says so.
void threadsLine(uint64_t nowMs, char* out, size_t n) {
    std::vector<ThreadRec> now = threadCensus();
    const uint64_t wallMs = nowMs - g_prevThreadsMs;
    const bool haveBase = !g_prevThreads.empty() && g_prevThreadsMs != 0 && wallMs > 0 && wallMs < 20000;
    struct Group { uintptr_t start; unsigned count; double sum, max; };
    std::vector<Group> groups;
    for (const ThreadRec& t : now) {
        double pct = 0.0;
        if (haveBase) {
            for (const ThreadRec& p : g_prevThreads)
                if (p.tid == t.tid) { pct = 100.0 * (double(t.cpu - p.cpu) / 10000.0) / double(wallMs); break; }
        }
        bool found = false;
        for (Group& g : groups)
            if (g.start == t.start) { ++g.count; g.sum += pct; g.max = std::max(g.max, pct); found = true; break; }
        if (!found) groups.push_back(Group{t.start, 1, pct, pct});
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.sum > b.sum; });
    g_prevThreads = now;
    g_prevThreadsMs = nowMs;
    size_t used = size_t(std::snprintf(out, n, "planet lod trace: threads (%zu in process%s; busy = CPU/wall, one core = 100%%): ", now.size(), haveBase ? "" : "; first census, no percentages yet"));
    for (size_t i = 0; i < groups.size() && i < 6 && used + 100 < n; ++i) {
        const std::string label = startLabel(groups[i].start);
        if (haveBase)
            used += size_t(std::snprintf(out + used, n - used, "%s%s x%u sum=%.0f%% max=%.0f%%", i ? " | " : "", label.c_str(), groups[i].count, groups[i].sum, groups[i].max));
        else
            used += size_t(std::snprintf(out + used, n - used, "%s%s x%u", i ? " | " : "", label.c_str(), groups[i].count));
    }
}

// --- Image identity, guarded ----------------------------------------------------------------------------------------------------------------------------------------
__declspec(noinline) bool guardedCheck(const uint8_t* base, plt::Verdict* v, size_t* size) noexcept {
    __try {
        const uint32_t peOff = plt::load32(base + 0x3C);
        *size = (peOff >= 0x40 && peOff <= 0x300) ? plt::load32(base + peOff + 0x50) : 0;
        *v = plt::checkImage(base, *size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *v = plt::Verdict{};
        std::snprintf(v->why, sizeof(v->why), "a read of the game's module faulted while checking its identity");
        return false;
    }
}

void sayInert(const char* why) {
    char line[520];
    plt::formatInert(line, sizeof(line), why);
    say(line);
}

}  // namespace

void planetLodTraceInstall() {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    plt::Verdict verdict;
    size_t size = 0;
    guardedCheck(base, &verdict, &size);
    if (!verdict.ok) {
        sayInert(verdict.why[0] ? verdict.why : "the game's module did not match");
        return;
    }
    int done = 0;
    for (int i = 0; i < plt::kSiteCount; ++i) {
        SiteState& s = g_site[i];
        const uintptr_t target = reinterpret_cast<uintptr_t>(base) + plt::kPins[i].rva;
        s.relay = allocateRelay(target);
        char why[260] = {};
        if (!s.relay) {
            std::snprintf(why, sizeof(why), "no executable memory within two gigabytes of the %s function", plt::kPins[i].name);
        } else {
            buildRelay(s.relay, &s.gate, kDetours[i]);
            if (s.hook.install(reinterpret_cast<void*>(target), s.relay, nullptr, "planet-lod-trace", &prepareRelay, &s)) { ++done; continue; }
            VirtualFree(s.relay, 0, MEM_RELEASE);
            s.relay = nullptr;
            std::snprintf(why, sizeof(why), "CodeHook refused the %s function (its own line above, tagged planet-lod-trace, names why)", plt::kPins[i].name);
        }
        for (int k = 0; k < i; ++k) {   // all or nothing: half a trace reads as a real result
            g_site[k].gate.store(0, std::memory_order_release);
            g_site[k].hook.uninstall();
        }
        sayInert(why);
        return;
    }
    (void)done;
    g_imgLo = reinterpret_cast<uintptr_t>(base);
    g_imgHi = g_imgLo + size;
    for (int i = 0; i < plt::kSiteCount; ++i) g_site[i].gate.store(1, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    char line[600];
    plt::formatLive(line, sizeof(line), "PE stamp and image size are build 332841's; every function head and call site verified");
    say(line);
}

void planetLodTracePoll(uint64_t nowMs) {
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (!g_pollStarted) {   // the first tick starts the window
        g_pollStarted = true;
        g_windowStartMs = nowMs;
        g_lastIdleSayMs = nowMs;
        return;
    }
    if (nowMs - g_windowStartMs < 5000) return;
    const plt::Window w = plt::closeWindow(g_trace, nowMs - g_windowStartMs);
    g_windowStartMs = nowMs;
    char line[1100];
    if (w.busy()) {
        plt::formatSummary(w, line, sizeof(line)); say(line);
        plt::formatBudgetAndTypes(w, line, sizeof(line)); say(line);
        if (w.nodes) { plt::formatTree(w, line, sizeof(line)); say(line); }
        threadsLine(nowMs, line, sizeof(line)); say(line);
        g_lastIdleSayMs = nowMs;
        return;
    }
    g_prevThreads.clear();   // an idle stretch: the next busy window takes a fresh baseline
    g_prevThreadsMs = 0;
    if (nowMs - g_lastIdleSayMs >= 60000) {
        g_lastIdleSayMs = nowMs;
        plt::formatIdle(w, line, sizeof(line));
        say(line);
    }
}

#ifdef EDVR_PLT_TEST
void* planetLodTraceTestDetour(int site) { return site >= 0 && site < plt::kSiteCount ? kDetours[site] : nullptr; }
void planetLodTraceTestSetForward(int site, uintptr_t fn) { if (site >= 0 && site < plt::kSiteCount) g_site[site].forward.store(fn, std::memory_order_release); }
void planetLodTraceTestSetImage(uintptr_t lo, uintptr_t hi) { g_imgLo = lo; g_imgHi = hi; }
void planetLodTraceTestMarkLive() { g_installed.store(true, std::memory_order_release); }
void planetLodTraceTestReset() {
    g_installed.store(false);
    g_attempted.store(false);
    g_pollStarted = false;
    g_windowStartMs = 0;
    g_lastIdleSayMs = 0;
    g_prevThreads.clear();
    g_prevThreadsMs = 0;
    (void)plt::closeWindow(g_trace, 0);
    for (auto& l : g_trace.life) l.store(0);
    g_trace.streakStartMs = 0;
    g_trace.lastSampleMs = 0;
    g_trace.seenN = 0;
    std::lock_guard<std::mutex> lock(g_testMutex);
    g_testLines.clear();
}
size_t planetLodTraceTestLineCount() {
    std::lock_guard<std::mutex> lock(g_testMutex);
    return g_testLines.size();
}
const char* planetLodTraceTestLine(size_t i) {
    std::lock_guard<std::mutex> lock(g_testMutex);
    return i < g_testLines.size() ? g_testLines[i].c_str() : "";
}
#endif

}  // namespace edvr
