#include "sun_pass_probe.h"
#include "sun_pass_probe_core.h"
#include "journal_watch.h"
#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// The hangar sun probe's glue (TEMPORARY). The pure half is sun_pass_probe_core.h. This file patches RenderDirLights' entry through the project's CodeHook
// and a relay (the same copy the other engine hooks carry), holds the sun-pass scope across the original call, and does its per-frame reads and log lines.
// The detour reads and writes nothing of the game's: the memory watch is a plain read under SEH, the Status words come from journal_watch's copy.

namespace edvr {
namespace {

namespace sp = sunprobe;

// --- Relay machinery, the same copy the other engine hooks carry (grep kRelayBytes). The target is in the game's module and this DLL loads more than two
// gigabytes away, so a five-byte E9 cannot reach a replacement here directly.
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
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]. RAX and the flags are volatile and the sun pass takes
    // nothing in RAX on entry. No stack adjustment, no nonvolatile register touched: the detour sees rcx, rdx, r8 and r9 exactly as the caller left them.
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

// --- State ---------------------------------------------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_attempted{false};          // install has been tried (once, ever)
std::atomic<bool> g_live{false};               // the hook is in and the detour is reachable
std::atomic<bool> g_toggle{false};             // the skip: on while true (hotkey.sun_pass_skip flips it; diag.sun_pass_skip sets it at install)
std::atomic<uint64_t> g_calls{0};              // detour entries since the last window take
std::atomic<uint64_t> g_draws{0};              // draws issued inside the sun pass since the last window take
std::atomic<uint64_t> g_skipped{0};            // of those, not forwarded (toggle on)
// Frame-thread state (the Present path and the toggle, which the same path calls):
bool g_memWatchOn = false;
uintptr_t g_base = 0;
sp::SectionSpan g_sections[16];
size_t g_sectionCount = 0;
sp::WatchSample g_watch;
bool g_statusLogged = false;
bool g_waitNoted = false;
sp::StatusWords g_lastWords;
uint64_t g_windowStartMs = 0;
bool g_memNoted = false;

void say(const char* line) {
    Log::get().note("%s", line);
}

sp::Window takeWindow() {
    sp::Window w;
    w.calls = g_calls.exchange(0, std::memory_order_relaxed);
    w.draws = g_draws.exchange(0, std::memory_order_relaxed);
    w.skipped = g_skipped.exchange(0, std::memory_order_relaxed);
    return w;
}

// --- SEH-guarded reads of the game's module -------------------------------------------------------------------------------------------------------------------
__declspec(noinline) bool readGame(uintptr_t base, uint32_t* timestamp, uint32_t* imageSize, uint8_t* prologue, uint8_t* site) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        std::memcpy(timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        std::memcpy(prologue, reinterpret_cast<const void*>(base + sp::kRenderDirLightsRva), sp::kPrologueBytes);
        std::memcpy(site, reinterpret_cast<const void*>(base + sp::kCallSiteRva), sp::kCallSpan);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The section table of the loaded image, from its headers: the memory watch's readability check.
__declspec(noinline) bool readSections(uintptr_t base, sp::SectionSpan* out, size_t cap, size_t* count) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        uint16_t nSec = 0, optSize = 0;
        std::memcpy(&nSec, reinterpret_cast<const void*>(base + peOff + 6), 2);
        std::memcpy(&optSize, reinterpret_cast<const void*>(base + peOff + 20), 2);
        if (nSec == 0 || nSec > cap) return false;
        const uintptr_t table = base + peOff + 24 + optSize;
        for (size_t i = 0; i < nSec; ++i) {
            uint32_t vsize = 0, va = 0, chars = 0;
            std::memcpy(&vsize, reinterpret_cast<const void*>(table + i * 40 + 8), 4);
            std::memcpy(&va, reinterpret_cast<const void*>(table + i * 40 + 12), 4);
            std::memcpy(&chars, reinterpret_cast<const void*>(table + i * 40 + 36), 4);
            out[i].va = va;
            out[i].size = vsize;
            out[i].characteristics = chars;
        }
        *count = nSec;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool readWatch(uintptr_t address, uint8_t out[sp::kMemoryWatchBytes]) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), sp::kMemoryWatchBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- The detour ------------------------------------------------------------------------------------------------------------------------------------------------------
// RenderDirLights(rcx = this, rdx, r8, r9): four register arguments and no stack argument (sun_pass_probe_core.h says what the disassembly reads).
// The forward call holds the sun-pass scope, so every draw this thread issues inside the original is seen by sunPassProbeSkipDraw.
using RenderDirLightsFn = uintptr_t (__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
__declspec(noinline) uintptr_t __fastcall renderDirLightsObserved(uintptr_t self, uintptr_t rdx, uintptr_t r8, uintptr_t r9) {
    const auto forward = reinterpret_cast<RenderDirLightsFn>(g_forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // never installed; the relay is unreachable then
    g_calls.fetch_add(1, std::memory_order_relaxed);
    sp::DepthScope scope(sp::sunPassDepth);
    return forward(self, rdx, r8, r9);
}

void refuse(const char* why) {
    char line[520];
    std::snprintf(line, sizeof(line), "hangar sun probe: inert: %s. The draws and the sun pass are unchanged.", why);
    say(line);
}

}  // namespace

void sunPassProbeInstall(bool flatProfile) {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final
    const std::string initial = Config::get().getString("diag.sun_pass_skip", "off");
    g_toggle.store(initial == "on", std::memory_order_relaxed);
    char line[520];
    if (initial != "on" && initial != "off") {
        std::snprintf(line, sizeof(line), "hangar sun probe: diag.sun_pass_skip = '%s' is not on or off; the skip starts off.", initial.c_str());
        say(line);
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) { refuse("the game's module could not be resolved"); return; }
    g_base = base;
    uint32_t timestamp = 0, imageSize = 0;
    uint8_t prologue[sp::kPrologueBytes] = {};
    uint8_t site[sp::kCallSpan] = {};
    if (!readGame(base, &timestamp, &imageSize, prologue, site)) {
        refuse("a read of the game's module faulted, or its PE header is implausible");
        return;
    }
    if (const char* why = sp::gateVerdict(timestamp, imageSize, prologue, site)) { refuse(why); return; }
    if (readSections(base, g_sections, sizeof(g_sections) / sizeof(g_sections[0]), &g_sectionCount) &&
        sp::rvaReadable(sp::kMemoryWatchRva, g_sections, g_sectionCount)) {
        g_memWatchOn = true;
    } else {
        std::snprintf(line, sizeof(line), "hangar sun probe: memory watch inert: exe+0x%X is not inside a readable section of the loaded image.",
                      static_cast<unsigned>(sp::kMemoryWatchRva));
        say(line);
    }
    const uintptr_t target = base + sp::kRenderDirLightsRva;
    g_relay = allocateRelay(target);
    if (!g_relay) { refuse("no executable memory could be placed within two gigabytes of RenderDirLights"); return; }
    buildRelay(g_relay, &g_relayGate, reinterpret_cast<void*>(&renderDirLightsObserved));
    if (!g_hook.install(reinterpret_cast<void*>(target), g_relay, nullptr, "hangar-sun-render-dir-lights", &prepareRelay, nullptr)) {
        VirtualFree(g_relay, 0, MEM_RELEASE);
        g_relay = nullptr;
        refuse("CodeHook refused RenderDirLights (its own line above, tagged hangar-sun-render-dir-lights, names why)");
        return;
    }
    g_relayGate.store(1, std::memory_order_release);
    g_live.store(true, std::memory_order_release);
    std::snprintf(line, sizeof(line),
        "hangar sun probe: live at EliteDangerous64.exe+0x%X (RenderDirLights; its call at +0x%X verified), stolen=%zu bytes, prologue %zu/%zu bytes, "
        "profile=%s, memory watch=%s at exe+0x%X, skip=%s initially.",
        static_cast<unsigned>(sp::kRenderDirLightsRva), static_cast<unsigned>(sp::kCallSiteRva), g_hook.stolenBytes(), sp::kPrologueBytes, sp::kPrologueBytes,
        flatProfile ? "flat" : "vr", g_memWatchOn ? "on" : "off", static_cast<unsigned>(sp::kMemoryWatchRva), g_toggle.load() ? "on" : "off");
    say(line);
}

void sunPassProbeFrame(uint32_t frame) {
    if (!g_live.load(std::memory_order_acquire)) return;
    char line[520];
    // The Status words, from journal_watch's copy (ungated by profile). Logged at the first sample and on every change.
    const JournalRawStatus r = journalRawStatus();
    if (r.statusSamples == 0) {
        if (!g_waitNoted) {
            g_waitNoted = true;
            say("hangar sun probe: status: waiting for the first Status.json sample (the journal watcher must be on: d3d11.journal_watch).");
        }
    } else {
        sp::StatusWords w;
        w.flagsKnown = r.flagsKnown;
        w.flags = r.flags;
        w.flags2Known = r.flags2Known;
        w.flags2 = r.flags2;
        if (!g_statusLogged || w.flagsKnown != g_lastWords.flagsKnown || w.flags != g_lastWords.flags ||
            w.flags2Known != g_lastWords.flags2Known || w.flags2 != g_lastWords.flags2) {
            sp::formatStatus(line, sizeof(line), w, frame);
            say(line);
            g_statusLogged = true;
            g_lastWords = w;
        }
    }
    // The memory watch: a plain 4-byte read, once a frame, logged on change (and the first read, as the baseline).
    if (g_memWatchOn) {
        uint8_t now[sp::kMemoryWatchBytes] = {};
        if (!readWatch(g_base + sp::kMemoryWatchRva, now)) {
            g_memWatchOn = false;
            if (!g_memNoted) {
                g_memNoted = true;
                say("hangar sun probe: memory watch stood down: the read at exe+0x5E889C0 faulted.");
            }
        } else if (sp::watchChanged(g_watch, now)) {
            sp::formatWatch(line, sizeof(line), now, frame, g_lastWords);
            say(line);
            g_watch.known = true;
            std::memcpy(g_watch.bytes, now, sp::kMemoryWatchBytes);
        }
    }
    // The 10 s window: a line when the window has any activity, in either toggle state (so the draws the sun pass issues with the skip off are
    // counted and logged too). The window restarts at each line.
    const uint64_t now = GetTickCount64();
    if (g_windowStartMs == 0) g_windowStartMs = now;
    if (sp::windowDue(now, g_windowStartMs)) {
        const sp::Window w = takeWindow();
        if (w.calls || w.draws || w.skipped) {
            sp::formatWindow(line, sizeof(line), w, g_toggle.load(std::memory_order_relaxed));
            say(line);
        }
        g_windowStartMs = now;
    }
}

void sunPassProbeToggle(uint32_t frame) {
    if (!g_live.load(std::memory_order_acquire)) {
        say("hangar sun probe: the skip key was pressed, but the probe is inert (its install line says why); nothing changed.");
        return;
    }
    char line[520];
    const bool was = g_toggle.load(std::memory_order_relaxed);
    // Flush the window of the state being left, so each line's counts belong to one state.
    const sp::Window w = takeWindow();
    if (w.calls || w.draws || w.skipped) {
        sp::formatWindow(line, sizeof(line), w, was);
        say(line);
    }
    g_toggle.store(!was, std::memory_order_relaxed);
    sp::formatToggle(line, sizeof(line), !was, frame);
    say(line);
    g_windowStartMs = GetTickCount64();
}

bool sunPassProbeSkipDraw() noexcept {
    const uint32_t depth = sp::sunPassDepth;
    if (depth == 0) return false;   // the common case: a draw outside the sun pass costs one thread-local read
    g_draws.fetch_add(1, std::memory_order_relaxed);
    if (!sp::skipsDraw(depth, g_toggle.load(std::memory_order_relaxed))) return false;
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace edvr
