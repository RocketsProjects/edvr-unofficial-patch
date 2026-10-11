#include "sun_pass_probe.h"
#include "sun_pass_probe_core.h"
#include "journal_watch.h"
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// The hangar sun probe's glue (TEMPORARY). The pure half is sun_pass_probe_core.h. This file reads the game's module under SEH (the build identity, the
// section table, the memory watch), reads the Status words from journal_watch's copy, counts the draws the draw hook hands over, and writes the log
// lines. It patches nothing: the skip is a verdict the draw thunk takes (vscreen.cpp beginPanelOverride), keyed on the bound shader pair.

namespace edvr {
namespace {

namespace sp = sunprobe;

// --- State ---------------------------------------------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_attempted{false};                      // install has been tried (once, ever)
std::atomic<bool> g_live{false};                           // install ran: the draw hook counts, and the skip is armed while the toggle is on
std::atomic<bool> g_toggle{false};                         // the skip: on while true (hotkey.sun_pass_skip flips it; diag.sun_pass_skip sets it at install)
std::atomic<uint64_t> g_counts[sp::kFieldCount] = {};      // the window's counters, by sp::Field; taken and zeroed once per window
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
    w.pairs = g_counts[sp::kFieldPairs].exchange(0, std::memory_order_relaxed);
    w.vsOnly = g_counts[sp::kFieldVsOnly].exchange(0, std::memory_order_relaxed);
    w.psOnly = g_counts[sp::kFieldPsOnly].exchange(0, std::memory_order_relaxed);
    w.skipped = g_counts[sp::kFieldSkipped].exchange(0, std::memory_order_relaxed);
    return w;
}

// --- SEH-guarded reads of the game's module -------------------------------------------------------------------------------------------------------------------
__declspec(noinline) bool readGame(uintptr_t base, uint32_t* timestamp, uint32_t* imageSize) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        std::memcpy(timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
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

}  // namespace

void sunPassProbeInstall(bool flatProfile) {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever
    const std::string initial = Config::get().getString("diag.sun_pass_skip", "off");
    g_toggle.store(initial == "on", std::memory_order_relaxed);
    char line[640];
    if (initial != "on" && initial != "off") {
        std::snprintf(line, sizeof(line), "hangar sun probe: diag.sun_pass_skip = '%s' is not on or off; the skip starts off.", initial.c_str());
        say(line);
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        say("hangar sun probe: memory watch inert: the game's module could not be resolved.");
    } else {
        g_base = base;
        uint32_t timestamp = 0, imageSize = 0;
        if (!readGame(base, &timestamp, &imageSize)) {
            say("hangar sun probe: memory watch inert: a read of the game's module faulted, or its PE header is implausible.");
        } else if (!sp::identityMatches(timestamp, imageSize)) {
            say("hangar sun probe: memory watch inert: not build 332841 (PE timestamp or image size differs). The draw skip is unaffected.");
        } else if (readSections(base, g_sections, sizeof(g_sections) / sizeof(g_sections[0]), &g_sectionCount) &&
                   sp::rvaReadable(sp::kMemoryWatchRva, g_sections, g_sectionCount)) {
            g_memWatchOn = true;
        } else {
            std::snprintf(line, sizeof(line), "hangar sun probe: memory watch inert: exe+0x%X is not inside a readable section of the loaded image.",
                          static_cast<unsigned>(sp::kMemoryWatchRva));
            say(line);
        }
    }
    g_live.store(true, std::memory_order_release);
    std::snprintf(line, sizeof(line),
        "hangar sun probe: live: the skip keys on the sun pair VS %016llX + PS %016llX in the draw hook (no engine hook). profile=%s, "
        "memory watch=%s at exe+0x%X, skip=%s initially.",
        static_cast<unsigned long long>(sp::kSunVs), static_cast<unsigned long long>(sp::kSunPs), flatProfile ? "flat" : "vr",
        g_memWatchOn ? "on" : "off", static_cast<unsigned>(sp::kMemoryWatchRva), g_toggle.load() ? "on" : "off");
    say(line);
}

void sunPassProbeFrame(uint32_t frame) {
    if (!g_live.load(std::memory_order_acquire)) return;
    char line[640];
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
    // The 10 s window: a line when the window has any activity, in either toggle state (so the pair's draws with the skip off are counted and logged
    // too). The window restarts at each line.
    const uint64_t now = GetTickCount64();
    if (g_windowStartMs == 0) g_windowStartMs = now;
    if (sp::windowDue(now, g_windowStartMs)) {
        const sp::Window w = takeWindow();
        if (sp::windowActive(w)) {
            sp::formatWindow(line, sizeof(line), w, g_toggle.load(std::memory_order_relaxed));
            say(line);
        }
        g_windowStartMs = now;
    }
}

void sunPassProbeToggle(uint32_t frame) {
    if (!g_live.load(std::memory_order_acquire)) {
        say("hangar sun probe: the skip key was pressed, but the probe is not armed (its install line says why); nothing changed.");
        return;
    }
    char line[640];
    const bool was = g_toggle.load(std::memory_order_relaxed);
    // Flush the window of the state being left, so each line's counts belong to one state.
    const sp::Window w = takeWindow();
    if (sp::windowActive(w)) {
        sp::formatWindow(line, sizeof(line), w, was);
        say(line);
    }
    g_toggle.store(!was, std::memory_order_relaxed);
    sp::formatToggle(line, sizeof(line), !was, frame);
    say(line);
    g_windowStartMs = GetTickCount64();
}

bool sunPassProbeSkipDraw(uint64_t vs, uint64_t ps) noexcept {
    if (!g_live.load(std::memory_order_acquire)) return false;   // not armed: nothing counted, nothing skipped
    const sp::Match m = sp::classify(vs, ps);
    if (m == sp::Match::kNone) return false;                     // the common case: two compares against the table
    g_counts[sp::fieldFor(m)].fetch_add(1, std::memory_order_relaxed);
    if (!sp::skipsDraw(m, g_toggle.load(std::memory_order_relaxed))) return false;
    g_counts[sp::kFieldSkipped].fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace edvr
