// The builder probe's engine side (builder_probe.h; TEMPORARY). The pure half (the entry agreement, the call sites, the grouping, the CSV rows, the
// summary, and the shared collection's containers and lock) is in builder_probe_core.h. The detour is a CodeHook on the builder 0x42B4420 with
// its sixteen prologue bytes pinned, built the way store_hook.cpp builds the store's detour. The observer takes the builder's six arguments: four
// in registers, then the fifth and sixth on the stack (the caller's [rsp+0x20] and [rsp+0x28]), and forwards all six unchanged.
//
// Built v8.1 (the crash fix): the detours do no container work unless a frame is being collected. With only the sealed trial on, a builder
// call costs one flag read and one atomic add. During the collected frame, every container is written under the collector's lock, which is
// never held across a call into the game (the store is recursive: its observer runs again inside the forward of the builder's observer).
#include "builder_probe.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "builder_probe_core.h"
#include "sealed_trial.h"
#include "store_hook.h"
#include "view_source.h"

namespace edvr {
namespace {

namespace bp = edvr::builder;

constexpr uintptr_t kBuilderRva = 0x42B4420u;
constexpr uint8_t kBuilderPin[16] = {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x41, 0x54, 0x41,
                                     0x57, 0x49, 0x8D, 0xAB, 0x08, 0xFC, 0xFF, 0xFF};
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;
constexpr size_t kSummaryBytes = 2048;
constexpr size_t kCallReserve = 16384;      // builder calls a collected frame can hold (more are counted as drops)
constexpr size_t kRecordReserve = 32768;    // distinct records the store visits a collected frame can hold (more are counted as drops)

// The six-argument form of the builder (its fifth and sixth arguments are on the stack).
using BuilderFn = uintptr_t(__fastcall*)(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9, uintptr_t p5, uintptr_t p6);

enum Phase : int { kIdle = 0, kArmed = 1, kCollecting = 2 };
std::atomic<int> g_phase{kIdle};             // the press arms one frame; the tick starts its collection; the next tick writes it
uint32_t g_collectFrame = 0;                 // the frame collected (render thread only)
std::atomic<uintptr_t> g_imageBase{0};       // set at install; the caller's return address is classed by its RVA
// Builder calls whose record's rec+0x234 flag read 0 at the builder's entry, since the last tick: calls after the trial cleared the record.
// Counted in every mode (the trial-only mode does nothing else). The tick takes it into the trial's window.
std::atomic<uint64_t> g_afterCull{0};
bp::Collector<std::mutex> g_collector;       // the collected frame's storage; its own lock covers each container operation

bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept { return readMem(dst, src, sizeof(T)); }

// The relay: the store's pattern (store_hook.cpp), kept as a copy as the other probes keep theirs.
struct Detour {
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<const char*> status{"not wanted yet"};
};
Detour g_builder;
alignas(8) std::atomic<uintptr_t> g_builderGate{0};
static_assert(decltype(g_builderGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

bool bytesMatch(const uint8_t* got, const uint8_t* want, size_t n) noexcept {
    return got && want && std::memcmp(got, want, n) == 0;
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline].
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

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& d = *static_cast<Detour*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(d.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(d.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), d.relay, kRelayBytes))
        return false;
    d.forward.store(address, std::memory_order_release);
    return true;
}

// Counts a builder call after the trial cleared its record: rec+0x234 reads 0 at the entry (rec = r9 - 0x210, so the flag is at r9 + 0x24).
// One read and one atomic add; nothing else is touched.
void countAfterCull(uintptr_t r9) noexcept {
    if (r9 < bp::kRecFromR9) return;
    uint8_t flag = 1;
    if (readVal(&flag, r9 + 0x24 /* rec+0x234 */) && flag == 0) g_afterCull.fetch_add(1, std::memory_order_relaxed);
}

// Records one builder call for the collected frame. Its fields are read here, before the builder runs; the name is read outside the collector's
// lock (the sealed trial's name cache has its own lock), and only the push itself is under the collector's lock.
void noteCall(uintptr_t rcx, uintptr_t r9, void* ret) noexcept {
    bp::Call c;
    c.rec = bp::recordBase(static_cast<uint64_t>(r9));
    const uintptr_t r = reinterpret_cast<uintptr_t>(ret);
    const uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    c.retRva = (base && r >= base) ? static_cast<uint32_t>(r - base) : 0u;
    c.caller = bp::callerOf(c.retRva);
    uint64_t value290 = 0;
    const bool read290 = c.rec != 0 && readVal(&value290, c.rec + bp::kRecFromRcx);
    c.agree = bp::argsAgree(static_cast<uint64_t>(rcx), static_cast<uint64_t>(r9), read290, value290);
    if (c.rec != 0) {
        uint8_t admitted = 0;
        float centre[3] = {};
        float radius = 0.0f;
        readVal(&c.mask, c.rec + 0x208);             // a failed read leaves the zero default
        if (readVal(&admitted, c.rec + 0x234)) {
            c.admitted = admitted;
            if (admitted == 0) g_afterCull.fetch_add(1, std::memory_order_relaxed);
        }
        const bool sphere = readMem(centre, c.rec + 0x240, sizeof(centre)) && readVal(&radius, c.rec + 0x280) &&
                            std::isfinite(radius) && std::isfinite(centre[0]) && std::isfinite(centre[1]) && std::isfinite(centre[2]);
        c.sphereOk = sphere;
        if (sphere) {
            c.radius = radius;
            c.centre[0] = centre[0];
            c.centre[1] = centre[1];
            c.centre[2] = centre[2];
        }
        std::string name;
        c.hasName = sealedTrialRecordName(c.rec, &name);
        if (c.hasName) c.name = name;
    }
    g_collector.addCall(std::move(c), GetCurrentThreadId());
}

// The builder's detour. The gate is closed unless the probe or the trial wants the builder. A collected frame records the call; a trial-only
// frame counts the cleared records and nothing more. Nothing is held across forward(): the store re-enters itself inside it.
__declspec(noinline) uintptr_t __fastcall builderObserved(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9, uintptr_t p5,
                                                          uintptr_t p6) noexcept {
    const auto forward = reinterpret_cast<BuilderFn>(g_builder.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    if (g_phase.load(std::memory_order_acquire) == kCollecting) noteCall(rcx, r9, _ReturnAddress());
    else if (sealedTrialOn()) countAfterCull(r9);
    return forward(rcx, rdx, r8, r9, p5, p6);
}

// Installs the builder detour once: the pinned prologue must match, and the CodeHook must accept it. Logs the refusal or the install.
void installBuilderOnce() noexcept {
    if (g_builder.ready.load(std::memory_order_acquire)) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        g_builder.status.store("the module base is unknown", std::memory_order_release);
        return;
    }
    g_imageBase.store(base, std::memory_order_release);
    uint8_t got[sizeof(kBuilderPin)] = {};
    bool readOk = false;
    __try {
        std::memcpy(got, reinterpret_cast<const void*>(base + kBuilderRva), sizeof(got));
        readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }
    if (!readOk || !bytesMatch(got, kBuilderPin, sizeof(kBuilderPin))) {
        g_builder.status.store("the builder's prologue does not match build 332841", std::memory_order_release);
        Log::get().note("builder probe: refused: the builder's sixteen prologue bytes are not build 332841's; no builder calls are "
                        "recorded (the trial counts stay at zero).");
        return;
    }
    g_builder.relay = allocateRelay(base + kBuilderRva);
    if (!g_builder.relay) {
        g_builder.status.store("no relay within range", std::memory_order_release);
        Log::get().note("builder probe: refused: no relay within range of the builder.");
        return;
    }
    buildRelay(g_builder.relay, &g_builderGate, reinterpret_cast<void*>(&builderObserved));
    if (!g_builder.hook.install(reinterpret_cast<void*>(base + kBuilderRva), g_builder.relay, nullptr, "builder-0x42B4420",
                                &prepareRelay, &g_builder)) {
        VirtualFree(g_builder.relay, 0, MEM_RELEASE);
        g_builder.relay = nullptr;
        g_builder.status.store("CodeHook refused the builder (its own line says why)", std::memory_order_release);
        Log::get().note("builder probe: refused: CodeHook refused the builder; its own line above says why.");
        return;
    }
    g_builder.ready.store(true, std::memory_order_release);
    g_builder.status.store("installed", std::memory_order_release);
    Log::get().note("builder probe: the builder (0x42B4420) is detoured; the gate is closed until a press or the sealed trial wants it.");
}

float distanceTo(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Writes the collected frame, taken whole from the collector (no lock is held here): the summary, the names, the hook and thread lines, then
// edvr_logs\builder_probe_<stamp>.csv (one row per name, then the calls, capped).
void writeFrame(uint32_t frame, bp::Storage& st) {
    float cam[3] = {};
    bool camOk = false;
    const char* camSource = "unknown";
    if (st.ctx) {
        view::Resolved v;
        view::resolveView(st.ctx, &v);
        camOk = view::sphereCentre(v, cam);
        camSource = view::sourceName(v.source);
    }
    for (size_t i = 0; i < st.calls.size(); ++i) {
        bp::Call& c = st.calls[i];
        c.storeSeen = st.seen.contains(c.rec);
        c.dist = (camOk && c.sphereOk) ? distanceTo(c.centre, cam) : -1.0f;
    }

    const bp::Summary s = bp::summarise(st.calls);
    char line[kSummaryBytes];
    bp::formatSummary(line, sizeof(line), frame, s);
    Log::get().note("%s", line);
    for (size_t from = 0;;) {   // the names, eight to a line
        size_t written = 0;
        if (bp::formatNamesLine(line, sizeof(line), frame, s, from, &written) < 0 || written == 0) break;
        Log::get().note("%s", line);
        from += written;
    }
    Log::get().note("builder probe: frame %u: builder hook %s; store hook %s; camera %s%s.", frame,
                    g_builder.status.load(std::memory_order_acquire), storeHookStatus(), camSource,
                    camOk ? "" : " (no camera: distances are -1)");
    // Built v8.1: the threads that reached each detour this frame, and what the bounded storage dropped.
    Log::get().note("builder probe: frame %u threads: store detour %u distinct%s, builder detour %u distinct%s; dropped: calls %llu "
                    "(reserve %zu), store visits %llu (reserve %zu).",
                    frame, st.storeThreads.count, st.storeThreads.more ? " and more" : "", st.builderThreads.count,
                    st.builderThreads.more ? " and more" : "", static_cast<unsigned long long>(st.callDrops), kCallReserve,
                    static_cast<unsigned long long>(st.seenDrops), kRecordReserve);

    SYSTEMTIME sysTime{};
    GetLocalTime(&sysTime);
    char stamp[24] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(sysTime.wYear),
                  static_cast<unsigned>(sysTime.wMonth), static_cast<unsigned>(sysTime.wDay), static_cast<unsigned>(sysTime.wHour),
                  static_cast<unsigned>(sysTime.wMinute), static_cast<unsigned>(sysTime.wSecond));
    char csvName[64] = {};
    std::snprintf(csvName, sizeof(csvName), "builder_probe_%s.csv", stamp);
    std::wstring path = Config::get().logDir() + L"\\builder_probe_";
    for (const char* c = stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        Log::get().note("builder probe: could not open edvr_logs\\%s; the summary line above is complete.", csvName);
        return;
    }
    std::fprintf(f, "# builder probe frame %u: calls %llu, distinct records %llu, not seen by the store %llu, args agree %llu, "
                    "disagree %llu, camera %s\n",
                 frame, static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.distinctRecords),
                 static_cast<unsigned long long>(s.unseen), static_cast<unsigned long long>(s.agree),
                 static_cast<unsigned long long>(s.disagree), camSource);
    std::fprintf(f, "%s\n", bp::groupCsvHeader());
    const auto groups = bp::groupByName(st.calls);
    char row[512];
    for (const auto& kv : groups)
        if (bp::formatGroupRow(row, sizeof(row), kv.second) >= 0) std::fprintf(f, "%s\n", row);
    const size_t written = st.calls.size() < bp::kCallRowCap ? st.calls.size() : bp::kCallRowCap;
    std::fprintf(f, "# calls: %zu of %zu written\n", written, st.calls.size());
    std::fprintf(f, "%s\n", bp::callCsvHeader());
    for (size_t i = 0; i < written; ++i)
        if (bp::formatCallRow(row, sizeof(row), st.calls[i]) >= 0) std::fprintf(f, "%s\n", row);
    std::fclose(f);
    Log::get().note("builder probe: frame %u written: csv=%s, %zu name groups, %zu of %zu calls in the csv.", frame, csvName,
                    groups.size(), written, st.calls.size());
}

}  // namespace

void builderProbeInstall(bool flatProfile) {
    (void)flatProfile;
    Log::get().note("builder probe: off. hotkey.builder_probe arms one frame; the builder detour (0x42B4420) goes in on the first want "
                    "(a press, or the sealed trial on).");
}

bool builderProbeWanted() noexcept {
    return g_phase.load(std::memory_order_acquire) != kIdle || sealedTrialOn();
}

bool builderProbeCollecting() noexcept {
    return g_phase.load(std::memory_order_acquire) == kCollecting;
}

void builderProbeRecompute() noexcept {
    const bool want = builderProbeWanted();
    if (want) installBuilderOnce();
    g_builderGate.store(want && g_builder.ready.load(std::memory_order_acquire) ? 1u : 0u, std::memory_order_release);
}

void builderProbeRequest(const char* trigger, uint32_t frame) {
    if (g_phase.load(std::memory_order_acquire) != kIdle) {
        Log::get().note("builder probe: refused (trigger %s): a frame is already armed or collecting.", trigger);
        return;
    }
    g_phase.store(kArmed, std::memory_order_release);
    Log::get().note("builder probe: armed (trigger %s) at frame %u; the next frame's builder calls are collected, and written at the "
                    "frame after.", trigger, frame);
    storeHookRecompute();   // the store is wanted too: the probe's visit set comes from it
}

void builderProbeAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    if (g_phase.load(std::memory_order_acquire) != kCollecting || !record) return;
    uintptr_t ctx = 0;
    if (bucket) readVal(&ctx, bucket);
    g_collector.noteStore(record, ctx, GetCurrentThreadId());
}

void builderProbeFrame(uint32_t frame) {
    // The trial's window: the builder calls after a cull since the last tick (counted in every mode; zero when the trial is off).
    sealedTrialNoteBuilderCalls(g_afterCull.exchange(0, std::memory_order_relaxed));
    if (g_phase.load(std::memory_order_acquire) == kCollecting) {
        g_phase.store(kIdle, std::memory_order_release);
        // A detour that read the phase before this store may still add one call or visit; it lands in the empty storage the exchange leaves,
        // which the next arm replaces. The taken storage is the frame's.
        bp::Storage taken = g_collector.exchange(bp::Storage{});
        writeFrame(g_collectFrame, taken);
    }
    if (g_phase.load(std::memory_order_acquire) == kArmed) {
        bp::Storage next;
        next.reserve(kCallReserve, kRecordReserve);   // every allocation happens here, before the lock
        (void)g_collector.exchange(std::move(next));  // the previous storage (empty after the write) is dropped
        g_collectFrame = frame + 1;
        g_phase.store(kCollecting, std::memory_order_release);
        Log::get().note("builder probe: collecting frame %u.", g_collectFrame);
    }
    storeHookRecompute();   // the gates follow the phase (the store's recompute also sets the builder's)
}

}  // namespace edvr
