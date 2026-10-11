#include "hmd_camera_tag_hook.h"
#include "../common/hmd_camera_tag.h"
#include "../common/code_hook.h"
#include "../common/log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

// The headset camera tag's glue (hmd_camera_tag.h says what and why): find the coordinator by its build identity and bytes, patch its entry with the
// project's CodeHook through a relay, and in the detour hold a depth scope across the original call. The detour reads and writes nothing of the game's and
// allocates and logs nothing; the stamp is taken by temporal_pass on the thread that does the Unmap.

namespace edvr {
namespace {

// --- Relay machinery, the same copy the other engine hooks carry (grep kRelayBytes): the target is in the game's module and this DLL loads more than two
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
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]. RAX and the flags are volatile and the coordinator takes
    // nothing in RAX on entry. No stack adjustment, no nonvolatile register touched: the detour sees rcx and rdx exactly as the caller left them.
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
std::atomic<uintptr_t> g_relayGate{0};   // 0: the relay forwards straight to the original (never installed)
CodeHook g_hook;

bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), g_relay, kRelayBytes)) return false;
    g_forward.store(address, std::memory_order_release);
    return true;
}

// --- State and instruments -----------------------------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_attempted{false};    // install has been tried (once, ever)
std::atomic<bool> g_live{false};
std::mutex g_whyMutex;                   // the reason the hook is inert, written once at install
char g_why[200] = "not installed yet";
std::atomic<uint64_t> g_calls{0};        // detour calls since the last take
std::atomic<uint64_t> g_lastC{0};        // the last call's coordinator pointer (rcx)
std::atomic<uint32_t> g_lastThread{0};   // the last call's thread id
std::atomic<uint32_t> g_lastFrame{0};    // the frame the chooser was on at the last call
std::atomic<uint32_t> g_frameNow{0};

void setWhy(const char* why) {
    std::lock_guard<std::mutex> lock(g_whyMutex);
    std::snprintf(g_why, sizeof(g_why), "%s", why);
}

// The one place the install writes a line: the log. Nothing calls this from the detour.
void say(const char* prefix, const char* why) {
    char line[520];
    std::snprintf(line, sizeof(line), "hmd camera tag: %s%s", prefix, why);
    Log::get().note("%s", line);
}

// --- SEH-guarded reads of the game's module ------------------------------------------------------------------------------------------------------------------------
__declspec(noinline) bool readGame(uintptr_t base, uint32_t* timestamp, uint32_t* imageSize, uint8_t* prologue, uint8_t* site) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        std::memcpy(timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        std::memcpy(prologue, reinterpret_cast<const void*>(base + hmdtag::kCoordinatorRva), hmdtag::kPrologueBytes);
        std::memcpy(site, reinterpret_cast<const void*>(base + hmdtag::kCallSiteRva), hmdtag::kCallSpan);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The detour. The forward call holds the depth scope, so every camera-block Unmap this thread makes inside the coordinator is stamped. The scope is RAII: a
// C++ throw out of the coordinator still restores the count.
using CoordinatorFn = uintptr_t (__fastcall*)(uintptr_t, uintptr_t);
__declspec(noinline) uintptr_t __fastcall coordinatorObserved(uintptr_t c, uintptr_t flag) {
    const auto forward = reinterpret_cast<CoordinatorFn>(g_forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // never installed; the relay is unreachable then
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_lastC.store(static_cast<uint64_t>(c), std::memory_order_relaxed);
    g_lastThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_lastFrame.store(g_frameNow.load(std::memory_order_relaxed), std::memory_order_relaxed);
    hmdtag::DepthScope scope(hmdtag::coordinatorDepth);
    return forward(c, flag);
}

void refuse(const char* why) {
    setWhy(why);
    say("inert: ", why);
    say("", "the chooser is unchanged; fix.hmd_camera_tag has nothing to tag.");
}

}  // namespace

void hmdCameraTagInstall(bool flatProfile) {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final
    if (flatProfile) {
        setWhy("the flat profile has no headset camera; nothing was patched");
        say("inert: ", g_why);
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) { refuse("the game's module could not be resolved"); return; }
    uint32_t timestamp = 0, imageSize = 0;
    uint8_t prologue[hmdtag::kPrologueBytes] = {};
    uint8_t site[hmdtag::kCallSpan] = {};
    if (!readGame(base, &timestamp, &imageSize, prologue, site)) {
        refuse("a read of the game's module faulted, or its PE header is implausible");
        return;
    }
    if (const char* why = hmdtag::gateVerdict(timestamp, imageSize, prologue, site)) { refuse(why); return; }
    const uintptr_t target = base + hmdtag::kCoordinatorRva;
    g_relay = allocateRelay(target);
    if (!g_relay) { refuse("no executable memory could be placed within two gigabytes of the coordinator"); return; }
    buildRelay(g_relay, &g_relayGate, reinterpret_cast<void*>(&coordinatorObserved));
    if (!g_hook.install(reinterpret_cast<void*>(target), g_relay, nullptr, "hmd-camera-tag", &prepareRelay, nullptr)) {
        VirtualFree(g_relay, 0, MEM_RELEASE);
        g_relay = nullptr;
        refuse("CodeHook refused the coordinator (its own line above, tagged hmd-camera-tag, names why)");
        return;
    }
    g_relayGate.store(1, std::memory_order_release);
    g_live.store(true, std::memory_order_release);
    char line[400];
    std::snprintf(line, sizeof(line),
        "hook live at EliteDangerous64.exe+0x%llX (the camera coordinator, build 332841; its call from the VR job at +0x%llX), stolen=%zu bytes, "
        "prologue %zu/%zu bytes and the call site verified. A camera-block write made inside it is the headset's; fix.hmd_camera_tag picks it.",
        static_cast<unsigned long long>(hmdtag::kCoordinatorRva), static_cast<unsigned long long>(hmdtag::kCallSiteRva), g_hook.stolenBytes(),
        hmdtag::kPrologueBytes, hmdtag::kPrologueBytes);
    say("", line);
}

void hmdCameraTagNoteFrame(uint32_t frame) {
    g_frameNow.store(frame, std::memory_order_relaxed);
}

HmdCameraTagState hmdCameraTagState() {
    HmdCameraTagState s;
    s.live = g_live.load(std::memory_order_acquire);
    s.lastC = g_lastC.load(std::memory_order_relaxed);
    s.lastThread = g_lastThread.load(std::memory_order_relaxed);
    s.lastFrame = g_lastFrame.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_whyMutex);
    std::snprintf(s.why, sizeof(s.why), "%s", g_why);
    return s;
}

uint64_t hmdCameraTagTakeCalls() {
    return g_calls.exchange(0, std::memory_order_relaxed);
}

}  // namespace edvr
