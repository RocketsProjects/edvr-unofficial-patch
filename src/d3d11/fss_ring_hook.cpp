#include "fss_ring_hook.h"

#include "fss_ring_core.h"
#include "vertex_resync_core.h"   // the build identity pair every build-keyed hook checks
#include "../common/code_hook.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

// See fss_ring_core.h for the question. This file is the glue, and it is a copy of the vertex resync's glue (vertex_resync_hook.cpp), which has flown: the relay
// that gets a five-byte E9 from the game's module to this DLL, the identity and prologue checks, the refusals. The observation runs under SEH before the original,
// reads only, and writes nothing in the game's memory.

namespace edvr {
namespace {

using namespace ringhook;

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
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]. RAX and the flags are volatile and the ring pass takes nothing in
    // RAX on entry. No stack adjustment, no nonvolatile register touched, so the eight parameters (four on the stack) arrive at the callback exactly as the game passed them.
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

uint8_t* g_relay = nullptr;
std::atomic<uintptr_t> g_forward{0};
std::atomic<uintptr_t> g_relayGate{0};   // 0: the relay forwards straight to the original (never installed, or stood down)
CodeHook g_hook;

bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), g_relay, kRelayBytes)) return false;
    g_forward.store(address, std::memory_order_release);
    return true;
}

// --- state ---------------------------------------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_installed{false};
std::atomic<uint64_t> g_callsTotal{0};        // every call of the ring pass since the hook armed
std::atomic<uint32_t> g_faults{0};
std::atomic<bool> g_active{false};            // a zoom/reveal capture is running
std::atomic<uint32_t> g_windowNo{0};
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_windowStartFrame{0};
std::atomic<bool> g_scan{false};                  // the scanner is on show: look for the arrival even outside a capture
std::atomic<bool> g_arrivalLatched{false};        // one trigger per zoom: cleared when the ramp finishes or the scanner is left
std::atomic<bool> g_trigPending{false};
std::atomic<uint32_t> g_trigFrame{0}, g_trigAlphaBits{0};
std::atomic<int> g_trigCamera{0};
std::atomic<uint32_t> g_arrivalInWindow{0};
thread_local Current t_cur;                       // the ring-pass call this thread is inside, while the capture runs
char g_inertReason[200] = "not attempted (the flat profile, or the install was never reached)";

// The window's tallies; touched by the calling thread only (the render thread), the summary reads them from the boundary tick on the same thread.
uint32_t g_tallyWindow = 0;
uint32_t g_callsInWindow = 0;
uint32_t g_linesInWindow = 0;
TupleTable g_tuples;
int g_mainId = 0;
bool g_mainIdKnown = false, g_mainIdFaulted = false;
uint32_t g_mainIdWindow = 0;

void sayf(const char* fmt, ...) {
    char line[1500];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Log::get().note("%s", line);
}

// --- SEH-guarded reads of the game's memory, each its own small function (no destructors in a __try body). -----------------------------------------------------
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool checkIdentity(uintptr_t base, const char** why) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) { *why = "the PE header offset is implausible"; return false; }
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (!vresync::identityMatches(timestamp, imageSize)) { *why = "not build 332841 (PE timestamp or image size differs)"; return false; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}
__declspec(noinline) bool readCall(uintptr_t ring, int pass, uintptr_t camera, uintptr_t data, Call* out) noexcept {
    __try {
        out->pass = pass;
        out->camera = *reinterpret_cast<const int*>(camera + kCameraId);
        const uintptr_t settings = *reinterpret_cast<const uintptr_t*>(camera + kCameraSettings);
        out->role = settings ? *reinterpret_cast<const uint8_t*>(settings + kSettingsRole) : 255u;
        out->mode = data ? *reinterpret_cast<const int*>(data + kDataBodyMode) : -1;
        out->scratch = *reinterpret_cast<const uint64_t*>(ring + kRingScratch) != 0;
        out->alpha = *reinterpret_cast<const float*>(ring + kRingAlpha);
        out->direction = *reinterpret_cast<const int*>(ring + kRingDirection);
        out->active = *reinterpret_cast<const uint8_t*>(ring + kRingActive);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The main camera's ID, the way the ring code asks for it (0x3659a1d: renderer = [ring+0x48]; renderer->vtbl[0xa8]() -> object; object->vtbl[0x70]() -> ID). Two calls
// into the game, both getters by the RE; a fault is absorbed, counted and never retried.
__declspec(noinline) bool readMainId(uintptr_t ring, int* out) noexcept {
    __try {
        const uintptr_t renderer = *reinterpret_cast<const uintptr_t*>(ring + kRingRenderer);
        const uintptr_t* vt = *reinterpret_cast<const uintptr_t* const*>(renderer);
        using GetObject = uintptr_t(__fastcall*)(uintptr_t);
        const uintptr_t object = reinterpret_cast<GetObject>(vt[kVtableGetCamera / 8])(renderer);
        const uintptr_t* vt2 = *reinterpret_cast<const uintptr_t* const*>(object);
        using GetId = int(__fastcall*)(uintptr_t);
        *out = reinterpret_cast<GetId>(vt2[kVtableGetId / 8])(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void noteFault() noexcept {
    const uint32_t n = g_faults.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) sayf("fss tiles: ring hook: a read inside the ring pass faulted and was absorbed; the game's call ran unchanged. Counting; %u stand the hook down.", kMaxFaults);
    if (n == kMaxFaults) {
        g_relayGate.store(0, std::memory_order_release);
        sayf("fss tiles: ring hook: stood down after %u faults; the relay now forwards straight to the game's ring pass.", kMaxFaults);
    }
}

void observe(uintptr_t ring, int pass, uintptr_t camera, uintptr_t data, bool active) noexcept {
    const uint32_t window = g_windowNo.load(std::memory_order_relaxed);
    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    Call c;
    if (!ring || !camera || !readCall(ring, pass, camera, data, &c)) {
        noteFault();
        return;
    }
    // The arrival: body mode 2 with the scan alpha ramping up. One trigger per zoom (the latch); inside a capture the first sighting is also recorded for the validity line.
    if (isArrival(c)) {
        if (active && g_arrivalInWindow.load(std::memory_order_relaxed) == 0) g_arrivalInWindow.store(frame ? frame : 1u, std::memory_order_relaxed);
        if (!g_arrivalLatched.exchange(true, std::memory_order_acq_rel)) {
            uint32_t bits = 0;
            std::memcpy(&bits, &c.alpha, sizeof(bits));
            g_trigAlphaBits.store(bits, std::memory_order_relaxed);
            g_trigCamera.store(c.camera, std::memory_order_relaxed);
            g_trigFrame.store(frame, std::memory_order_relaxed);
            g_trigPending.store(true, std::memory_order_release);
        }
    } else if (rampDone(c)) {
        g_arrivalLatched.store(false, std::memory_order_relaxed);
    }
    if (!active) return;
    if (g_tallyWindow != window) {      // a new window: its own tallies
        g_tallyWindow = window;
        g_callsInWindow = 0;
        g_linesInWindow = 0;
        g_tuples.clear();
    }
    ++g_callsInWindow;
    t_cur.inCall = true;
    t_cur.camera = c.camera;
    t_cur.role = c.role;
    t_cur.pass = c.pass;
    t_cur.mode = c.mode;
    t_cur.scratch = c.scratch;
    const bool isNew = g_tuples.add(c);
    if (g_mainIdWindow != window) {
        g_mainIdWindow = window;
        g_mainIdKnown = false;
        if (!g_mainIdFaulted) {
            int id = 0;
            if (readMainId(ring, &id)) {
                g_mainId = id;
                g_mainIdKnown = true;
                sayf("fss tiles: ring main camera id=%d (window %u, f=%u; read through [ring+0x48] vtable 0xa8 then 0x70, the way the ring code asks)", id, window, frame);
            } else {
                g_mainIdFaulted = true;
                sayf("fss tiles: ring main camera id could not be read (a fault was absorbed; not retried this session)");
            }
        }
    }
    const uint32_t since = frame - g_windowStartFrame.load(std::memory_order_relaxed);
    const char* why = nullptr;
    if (since < kFirstFrames) why = "first frames";
    else if (isNew) why = "new combination";
    else if (frame % kEvery == 0) why = "sample";
    if (why && g_linesInWindow < kMaxCallLines) {
        char line[400];
        formatCall(line, sizeof(line), frame, g_callsInWindow, c, why);
        Log::get().note("%s", line);
        ++g_linesInWindow;
        if (g_linesInWindow == kMaxCallLines) sayf("fss tiles: ring call-line cap (%u) reached for this window; the closing summary still counts every call.", kMaxCallLines);
    }
}

using RingFn = void(__fastcall*)(uintptr_t, int, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
// FUN_1436558a0(ring = rcx, pass = edx, camera = r8, a4 = r9, then a5-a8 on the stack, a8 the render data). Every argument is forwarded unchanged.
__declspec(noinline) void __fastcall ringPassObserved(uintptr_t ring, int pass, uintptr_t camera, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7,
                                                      uintptr_t data) noexcept {
    const auto forward = reinterpret_cast<RingFn>(g_forward.load(std::memory_order_acquire));
    if (!forward) return;   // stood down at install; the relay is unreachable then
    g_callsTotal.fetch_add(1, std::memory_order_relaxed);
    const bool active = g_active.load(std::memory_order_relaxed);
    if (active || g_scan.load(std::memory_order_relaxed)) observe(ring, pass, camera, data, active);
    forward(ring, pass, camera, a4, a5, a6, a7, data);
    t_cur.inCall = false;
}

}  // namespace

void fssRingHookInstall() {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final
    if (runtimeFlatProfile()) {
        std::snprintf(g_inertReason, sizeof(g_inertReason), "the flat profile (one camera, nothing to compare)");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        std::snprintf(g_inertReason, sizeof(g_inertReason), "the game's module could not be resolved");
        sayf("fss tiles: ring hook inert -- %s. The game's ring pass runs as it always did.", g_inertReason);
        return;
    }
    const char* why = nullptr;
    if (!checkIdentity(base, &why)) {
        std::snprintf(g_inertReason, sizeof(g_inertReason), "the game build differs (%s)", why ? why : "?");
        sayf("fss tiles: ring hook inert -- %s; EliteDangerous64.exe+0x%llX was not touched.", g_inertReason, static_cast<unsigned long long>(kRingPassRva));
        return;
    }
    const uintptr_t target = base + kRingPassRva;
    if (!sehCheckBytes(target, kPrologue, kPrologueBytes)) {
        std::snprintf(g_inertReason, sizeof(g_inertReason), "the %zu bytes at EliteDangerous64.exe+0x%llX are not build 332841's ring pass prologue", kPrologueBytes,
                      static_cast<unsigned long long>(kRingPassRva));
        sayf("fss tiles: ring hook inert -- %s.", g_inertReason);
        return;
    }
    g_relay = allocateRelay(target);
    if (!g_relay) {
        std::snprintf(g_inertReason, sizeof(g_inertReason), "no executable memory could be placed within two gigabytes of the ring pass");
        sayf("fss tiles: ring hook inert -- %s.", g_inertReason);
        return;
    }
    buildRelay(g_relay, &g_relayGate, reinterpret_cast<void*>(&ringPassObserved));
    if (!g_hook.install(reinterpret_cast<void*>(target), g_relay, nullptr, "fss-ring-pass", &prepareRelay, nullptr)) {
        VirtualFree(g_relay, 0, MEM_RELEASE);
        g_relay = nullptr;
        std::snprintf(g_inertReason, sizeof(g_inertReason), "CodeHook refused it (its own line above, tagged fss-ring-pass, names why)");
        sayf("fss tiles: ring hook inert -- %s.", g_inertReason);
        return;
    }
    g_relayGate.store(1, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    sayf("fss tiles: ring hook armed at EliteDangerous64.exe+0x%llX (the ring pass entry, build 332841), stolen=%zu bytes, prologue %zu/%zu bytes verified. Read-only: inside "
         "an FSS zoom/reveal capture it logs per call the camera id, the stereo role, the pass, the body mode, ring+0x6b0 and the scan alpha (the first %u frames of a window, "
         "every new combination, then one frame in %u; at most %u lines a window); every call is forwarded unchanged. Every window closes with a count of the calls it saw.",
         static_cast<unsigned long long>(kRingPassRva), g_hook.stolenBytes(), kPrologueBytes, kPrologueBytes, kFirstFrames, kEvery, kMaxCallLines);
}

void fssRingHookScan(bool wanted) {
    g_scan.store(wanted && g_installed.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (!wanted) g_arrivalLatched.store(false, std::memory_order_relaxed);
}

bool fssRingHookTakeArrival(uint32_t* frame, float* alpha, int* camera) {
    if (!g_trigPending.exchange(false, std::memory_order_acq_rel)) return false;
    if (frame) *frame = g_trigFrame.load(std::memory_order_relaxed);
    if (alpha) {
        const uint32_t bits = g_trigAlphaBits.load(std::memory_order_relaxed);
        std::memcpy(alpha, &bits, sizeof(bits));
    }
    if (camera) *camera = g_trigCamera.load(std::memory_order_relaxed);
    return true;
}

uint32_t fssRingHookArrivalInWindow() { return g_arrivalInWindow.load(std::memory_order_relaxed); }

bool fssRingHookCurrent(ringhook::Current* out) {
    if (!t_cur.inCall) return false;
    *out = t_cur;
    return true;
}

bool fssRingHookMainCamera(int* id) {
    if (!g_mainIdKnown) return false;
    *id = g_mainId;
    return true;
}

void fssRingHookWindow(bool active, uint32_t windowNo, uint32_t frame) {
    if (windowNo != g_windowNo.load(std::memory_order_relaxed) && active) {
        g_windowStartFrame.store(frame, std::memory_order_relaxed);
        g_arrivalInWindow.store(0, std::memory_order_relaxed);
    }
    g_windowNo.store(windowNo, std::memory_order_relaxed);
    g_frame.store(frame, std::memory_order_relaxed);
    g_active.store(active && g_installed.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void fssRingHookWindowSummary(uint32_t windowNo) {
    if (!g_installed.load(std::memory_order_acquire)) {
        sayf("fss tiles: window %u ring hook -- inert (%s); it saw nothing because it was never armed", windowNo, g_inertReason);
        return;
    }
    const uint64_t total = g_callsTotal.load(std::memory_order_relaxed);
    const bool stoodDown = g_relayGate.load(std::memory_order_acquire) == 0;
    if (g_tallyWindow != windowNo) {
        sayf("fss tiles: window %u ring hook -- armed, %llu ring-pass call(s) since it armed (%s), none inside this window's capture%s", windowNo,
             static_cast<unsigned long long>(total), total ? "so the entry is reached" : "the entry has NOT been reached this session",
             stoodDown ? "; stood down after faults" : "");
        return;
    }
    char tuples[640];
    formatTuples(tuples, sizeof(tuples), g_tuples);
    sayf("fss tiles: window %u ring hook -- armed, %llu ring-pass call(s) since it armed, %u inside this window's capture, %u distinct combination(s)%s: %s; call lines %u; "
         "main camera id %s%s",
         windowNo, static_cast<unsigned long long>(total), g_callsInWindow, g_tuples.used, g_tuples.overflow ? " (more than the table holds)" : "",
         g_tuples.used ? tuples : "none", g_linesInWindow, g_mainIdKnown ? "known" : (g_mainIdFaulted ? "unreadable (a fault was absorbed)" : "not read"),
         stoodDown ? "; stood down after faults" : "");
    if (g_mainIdKnown) sayf("fss tiles: window %u ring main camera id=%d", windowNo, g_mainId);
}

}  // namespace edvr
