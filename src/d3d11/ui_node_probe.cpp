#include "ui_node_probe.h"
#include "../common/log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

// The UI node probe (ui_node_probe.h says what it asks and why). This file is the glue: pin the slot, swap the pointer, count in the forwarder.

namespace edvr {

// ui_depth.cpp (declared in ui_depth.h; restated here so this file, and the rig that links it, need nothing of that header): the eye draws the census has
// counted this frame, and the thread that issued the last one.
void uiDepthBoundaryDrawPosition(uint32_t* lit, uint32_t* ldr, unsigned long* thread);

namespace {

std::atomic<bool> g_attempted{false};
std::atomic<bool> g_installed{false};
std::atomic<uintptr_t> g_original{0};
uintptr_t g_slotAddress = 0;
char g_why[260] = {};

// The window the "ui boundary cost" line reports, and the probe's own first lines.
std::atomic<uint64_t> g_calls{0};            // calls this window
std::atomic<uint64_t> g_firstOfFrame{0};     // ...that were the first of their frame
std::atomic<uint64_t> g_otherThread{0};      // ...made on a thread other than the eye draws'
std::atomic<uint32_t> g_lastLit{0}, g_lastLdr{0};   // where the last call fell
std::atomic<uint32_t> g_callsThisFrame{0};
std::atomic<uint32_t> g_framesSaid{0};
std::atomic<bool> g_haveLast{false};
constexpr uint32_t kFramesSaid = 6;   // frames whose calls are logged one by one (the first two calls of each)

#ifdef EDVR_UI_NODE_PROBE_TEST
const uint8_t* g_testBase = nullptr;
#endif

using NodeFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

// The routine's arguments are the node and one more integer (the decompilation shows two); four registers are forwarded untouched, so a
// caller that left something else in RDX/R8/R9 gets it back, and the original's return value (none, read as RAX) passes through.
__declspec(noinline) uintptr_t __fastcall nodeDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) noexcept {
    const auto original = reinterpret_cast<NodeFn>(g_original.load(std::memory_order_acquire));
    if (!original) return 0;
    uint32_t lit = 0, ldr = 0;
    unsigned long drawThread = 0;
    uiDepthBoundaryDrawPosition(&lit, &ldr, &drawThread);
    const uint32_t last = g_lastLit.load(std::memory_order_relaxed);
    // A new frame: the lit-target count restarted below where the last call stood.
    const bool newFrame = !g_haveLast.load(std::memory_order_relaxed) || lit < last;
    const uint32_t nth = newFrame ? 1u : g_callsThisFrame.load(std::memory_order_relaxed) + 1u;
    g_callsThisFrame.store(nth, std::memory_order_relaxed);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (newFrame) g_firstOfFrame.fetch_add(1, std::memory_order_relaxed);
    const unsigned long thread = GetCurrentThreadId();
    const bool same = drawThread != 0 && drawThread == thread;
    if (drawThread != 0 && !same) g_otherThread.fetch_add(1, std::memory_order_relaxed);
    g_lastLit.store(lit, std::memory_order_relaxed);
    g_lastLdr.store(ldr, std::memory_order_relaxed);
    g_haveLast.store(true, std::memory_order_release);
    if (newFrame) {
        const uint32_t said = g_framesSaid.load(std::memory_order_relaxed);
        if (said <= kFramesSaid) g_framesSaid.store(said + 1, std::memory_order_relaxed);   // counts one past the last frame said, so it stops
    }
    if (g_framesSaid.load(std::memory_order_relaxed) <= kFramesSaid && nth <= 2) {   // frames 1..kFramesSaid
        Log::get().note("ui node probe: call %u of its frame (node %p, arg %llX) after %u lit-target and %u 8-bit-target eye draws of the frame; "
                        "%s (thread %lu, eye draws on %lu).",
                        nth, reinterpret_cast<void*>(a), static_cast<unsigned long long>(b), lit, ldr,
                        drawThread == 0 ? "no eye draw has been counted on any thread yet" : (same ? "the same thread as the eye draws" : "ANOTHER thread than the eye draws"),
                        thread, drawThread);
    }
    return original(a, b, c, d);
}

void sayInert(const char* why) {
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    Log::get().note("ui node probe: inert: %s", why);
}

__declspec(noinline) bool guardedLocate(const uint8_t* base, unode::Located* out) noexcept {
    __try {
        return unode::locate(base, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = unode::Located{};
        std::snprintf(out->why, sizeof(out->why), "a read of the game's module faulted while checking the UI node's slot");
        return false;
    }
}

}  // namespace

void uiNodeProbeInstall() {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final
    const uint8_t* base = nullptr;
#ifdef EDVR_UI_NODE_PROBE_TEST
    base = g_testBase;
#endif
    if (!base) base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    unode::Located found;
    if (!guardedLocate(base, &found)) {
        sayInert(found.why[0] ? found.why : "the UI node's slot was not found");
        return;
    }
    auto* slot = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(base) + unode::kSlotRva);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
        sayInert("the slot's page could not be made writable");
        return;
    }
    g_original.store(*slot, std::memory_order_release);
    g_slotAddress = reinterpret_cast<uintptr_t>(slot);
    *slot = reinterpret_cast<uintptr_t>(&nodeDetour);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(uintptr_t), old, &ignored);
    g_installed.store(true, std::memory_order_release);
    Log::get().note("ui node probe: live -- exe+0x%X (the UI node class's vtable slot %u, 0x%X) now forwards through EDVR's counter to exe+0x%X; PE stamp %u and "
                    "image size %u are build 332841's, the slot's value and the routine's first %u bytes verified. Observe only: it counts and calls the "
                    "original.",
                    unode::kSlotRva, unode::kSlot, unode::kVtableRva, unode::kFunctionRva, found.stamp, unode::kExpectedImageSize,
                    static_cast<unsigned>(unode::kHeadBytes));
}

void uiNodeProbeClause(char* out, size_t cap) {
    if (!g_attempted.load(std::memory_order_acquire)) {
        std::snprintf(out, cap, "node probe: not attempted.");
        return;
    }
    if (!g_installed.load(std::memory_order_acquire)) {
        std::snprintf(out, cap, "node probe: inert (%s).", g_why[0] ? g_why : "no reason recorded");
        return;
    }
    const uint64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t firsts = g_firstOfFrame.exchange(0, std::memory_order_relaxed);
    const uint64_t other = g_otherThread.exchange(0, std::memory_order_relaxed);
    if (!calls) {
        std::snprintf(out, cap, "node probe: live (exe+0x%X -> exe+0x%X) but not called in this window.", unode::kSlotRva, unode::kFunctionRva);
        return;
    }
    std::snprintf(out, cap,
                  "node probe: live (exe+0x%X -> exe+0x%X), %llu calls in %llu frames, %llu of them on another thread than the eye draws; the last "
                  "fell after %u lit-target and %u 8-bit-target eye draws of its frame.",
                  unode::kSlotRva, unode::kFunctionRva, static_cast<unsigned long long>(calls), static_cast<unsigned long long>(firsts),
                  static_cast<unsigned long long>(other), g_lastLit.load(std::memory_order_relaxed), g_lastLdr.load(std::memory_order_relaxed));
}

#ifdef EDVR_UI_NODE_PROBE_TEST
void uiNodeProbeTestSetImage(const uint8_t* base) { g_testBase = base; }
void uiNodeProbeTestReset() {
    if (g_installed.load() && g_slotAddress) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(g_slotAddress), sizeof(uintptr_t), PAGE_READWRITE, &old);
        *reinterpret_cast<uintptr_t*>(g_slotAddress) = g_original.load();
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(g_slotAddress), sizeof(uintptr_t), old, &ignored);
    }
    g_installed.store(false);
    g_attempted.store(false);
    g_original.store(0);
    g_slotAddress = 0;
    g_why[0] = 0;
    g_calls.store(0);
    g_firstOfFrame.store(0);
    g_otherThread.store(0);
    g_lastLit.store(0);
    g_lastLdr.store(0);
    g_callsThisFrame.store(0);
    g_framesSaid.store(0);
    g_haveLast.store(false);
}
bool uiNodeProbeTestInstalled() { return g_installed.load(); }
bool uiNodeProbeTestAttempted() { return g_attempted.load(); }
uint64_t uiNodeProbeTestCalls() { return g_calls.load(); }
#endif

}  // namespace edvr
