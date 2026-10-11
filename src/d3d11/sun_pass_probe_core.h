#pragma once
// The hangar sun probe (TEMPORARY diagnostic; docs\hangar-sunlight-2026-10-11.md says what it is for). Pure: the build-332841 identity and the
// RenderDirLights gate, the nesting scope that marks a draw as issued inside the sun pass, the skip decision, the Status.json decode, the memory-watch
// change detector, the 10 s window, and the text of every log line. No Windows and no globals beyond the thread's depth. The glue is
// src\d3d11\sun_pass_probe.cpp; the rig is tools\sun_pass_probe_test.
//
// WHAT THE GATE PINS, read out of EliteDangerous64.exe build 332841 (PE timestamp 1788384820, image size 104894464):
//   RenderDirLights, RVA 0x5AC9C0 (3492 bytes). It binds the sun's constants and issues the directional light's draws. Its only direct caller in this
//   build is the deferred lighting frame at RVA 0x284CB70, through the call at RVA 0x284E159 (e8 62 e8 d5 fd; 0x284E15E - 0x22A179E = 0x5AC9C0).
//   Arguments, from the disassembly: rcx is `this` (stored at [rcx+0x40] and [rcx+0x48], and returned in rax as rbx); rdx is a pointer that is
//   dereferenced at [rdx+0xE0] and stored at [rcx+0x40]; r8 is an object pointer that is tested and addref'd through r13 (lock inc [r13+8]). r9 is
//   written (xor r9d,r9d) before it is read. No stack argument is read: the only rsp-relative reads above the frame are the restore of rbx from its
//   own save slot at entry+0x20. The detour forwards rcx, rdx, r8 and r9 (r9 is harmless when unused) and returns the original's rax.
//
// WHAT THE STATUS DECODE TRUSTS. EDVR's journal_watch.h/.cpp reads Flags bit 4 (supercruise), bits 24-26 (seated: main ship, fighter, SRV), bit 30
// (the FSD jump), and Flags2 bit 0 (OnFoot) and bits 1-2 (taxi, multicrew), all from Elite's Status.json, and those are the verified bits here.
// Flags bit 0 (Docked) and bit 1 (Landed) are the design doc's reading (its I3), NOT yet confirmed on a flight; the first flight logs the raw words so
// they can be checked. No hangar bit is confirmed in any published table reachable from here, so the probe prints the raw Flags and Flags2 in hex on
// every line and reports the design doc's inside gate as `inside_gate`, not as an in-hangar bit.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::sunprobe {

// ---- identity and the gate, build 332841 ----------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;
constexpr uint32_t kRenderDirLightsRva = 0x5AC9C0u;     // RenderDirLights, the sun's direct light
constexpr size_t kPrologueBytes = 32;                    // the first 32 bytes, read from the exe; CodeHook steals the first 5 (one mov)
constexpr uint8_t kPrologue[kPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x20,  0x55,  0x56,  0x57,  0x41, 0x54,  0x41, 0x55,  0x41, 0x56,  0x41, 0x57,
    0x48, 0x8B, 0xEC,  0x48, 0x83, 0xEC, 0x60,  0x33, 0xFF,  0x48, 0x89, 0x51, 0x40,  0x48, 0x8B, 0xD9};
constexpr uint32_t kCallSiteRva = 0x284E159u;            // the deferred lighting frame's call to RenderDirLights
constexpr size_t kCallSpan = 5;                          // e8 rel32
constexpr uint8_t kCallSite[kCallSpan] = {0xE8, 0x62, 0xE8, 0xD5, 0xFD};

// ---- the memory watch and the window ---------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMemoryWatchRva = 0x5E889C0u;         // the 4-byte group the light dispatch reads (design doc P8); read-only
constexpr size_t kMemoryWatchBytes = 4;
constexpr uint32_t kImageScnMemRead = 0x40000000u;       // IMAGE_SCN_MEM_READ
constexpr uint64_t kWindowMs = 10000;                    // the periodic line's period

// Gate verdict. nullptr when every check passes; otherwise the reason, for the one log line.
inline bool identityMatches(uint32_t timestamp, uint32_t imageSize) {
    return timestamp == kExpectedTimestamp && imageSize == kExpectedImageSize;
}
inline bool prologueMatches(const uint8_t* bytes) {
    return bytes && std::memcmp(bytes, kPrologue, kPrologueBytes) == 0;
}
// site: kCallSpan bytes at kCallSiteRva. The bytes must be the build's call, and its target (next instruction plus rel32) must be RenderDirLights.
inline bool callSiteMatches(const uint8_t* site) {
    if (!site || std::memcmp(site, kCallSite, kCallSpan) != 0) return false;
    int32_t rel = 0;
    std::memcpy(&rel, site + 1, 4);
    const int64_t target = int64_t(kCallSiteRva) + int64_t(kCallSpan) + int64_t(rel);
    return target == int64_t(kRenderDirLightsRva);
}
inline const char* gateVerdict(uint32_t timestamp, uint32_t imageSize, const uint8_t* prologue, const uint8_t* site) {
    if (!identityMatches(timestamp, imageSize)) return "not build 332841 (PE timestamp or image size differs)";
    if (!prologueMatches(prologue)) return "the first 32 bytes of RenderDirLights differ from build 332841's";
    if (!callSiteMatches(site)) return "the deferred lighting frame's call to RenderDirLights at exe+0x284E159 is not where build 332841 has it";
    return nullptr;
}

// ---- the sun pass's nesting on this thread ----------------------------------------------------------------------------------------------------------------------
// The detour holds a DepthScope across the original call. A draw issued on this thread while the depth is above zero was issued by the sun pass.
// The scope's destructor restores the count on every exit, a C++ throw included, so the count cannot drift.
inline thread_local uint32_t sunPassDepth = 0;
class DepthScope {
public:
    explicit DepthScope(uint32_t& depth) noexcept : depth_(depth) { ++depth_; }
    ~DepthScope() { --depth_; }
    DepthScope(const DepthScope&) = delete;
    DepthScope& operator=(const DepthScope&) = delete;
private:
    uint32_t& depth_;
};
// The skip: only a draw inside the sun pass, and only while the toggle is on. Outside the scope nothing is ever skipped.
inline bool skipsDraw(uint32_t depth, bool toggleOn) noexcept {
    return depth > 0 && toggleOn;
}

// ---- the Status.json decode -------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kFlagDocked = 1u << 0;        // design doc I3: unconfirmed
constexpr uint32_t kFlagLanded = 1u << 1;        // design doc I3: unconfirmed
constexpr uint32_t kFlagSupercruise = 1u << 4;   // journal_watch.cpp (verified in EDVR)
constexpr uint32_t kFlagInMainShip = 1u << 24;   // journal_watch.h seated group (verified in EDVR); the doc's inShip
constexpr uint32_t kFlagsSeated = (1u << 24) | (1u << 25) | (1u << 26);
constexpr uint32_t kFlags2OnFoot = 1u << 0;      // journal_watch.cpp (verified in EDVR)
constexpr uint32_t kFlags2Seated = (1u << 1) | (1u << 2);

struct StatusWords {
    bool flagsKnown = false;     // "Flags" is in the sample
    uint32_t flags = 0;
    bool flags2Known = false;    // "Flags2" is in the sample (absent on menus)
    uint32_t flags2 = 0;
};
struct StatusView {
    bool docked = false;
    bool landed = false;
    bool supercruise = false;
    bool onFoot = false;
    bool seated = false;
    bool inside = false;         // the design doc's gate: docked && !landed && !supercruise && (inMainShip || onFoot); unknown is off
};
inline StatusView decodeStatus(const StatusWords& w) {
    StatusView v;
    v.docked = w.flagsKnown && (w.flags & kFlagDocked) != 0;
    v.landed = w.flagsKnown && (w.flags & kFlagLanded) != 0;
    v.supercruise = w.flagsKnown && (w.flags & kFlagSupercruise) != 0;
    const bool inMainShip = w.flagsKnown && (w.flags & kFlagInMainShip) != 0;
    v.onFoot = w.flags2Known && (w.flags2 & kFlags2OnFoot) != 0;
    v.seated = (w.flagsKnown && (w.flags & kFlagsSeated) != 0) || (w.flags2Known && (w.flags2 & kFlags2Seated) != 0);
    v.inside = v.docked && !v.landed && !v.supercruise && (inMainShip || v.onFoot);
    return v;
}

// ---- the memory watch ------------------------------------------------------------------------------------------------------------------------------------------
struct WatchSample {
    bool known = false;
    uint8_t bytes[kMemoryWatchBytes] = {};
};
// True on the first sample (the baseline is logged) and whenever the four bytes differ from the last logged sample.
inline bool watchChanged(const WatchSample& last, const uint8_t now[kMemoryWatchBytes]) {
    return !last.known || std::memcmp(last.bytes, now, kMemoryWatchBytes) != 0;
}
// The section that holds an RVA and its four bytes must be readable: a section table entry with IMAGE_SCN_MEM_READ whose virtual span covers them.
struct SectionSpan {
    uint32_t va = 0;
    uint32_t size = 0;
    uint32_t characteristics = 0;
};
inline bool rvaReadable(uint32_t rva, const SectionSpan* secs, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint64_t start = secs[i].va;
        const uint64_t end = start + secs[i].size;
        if ((secs[i].characteristics & kImageScnMemRead) == 0) continue;
        if (uint64_t(rva) >= start && uint64_t(rva) + kMemoryWatchBytes <= end) return true;
    }
    return false;
}

// ---- the 10 s window --------------------------------------------------------------------------------------------------------------------------------------------
struct Window {
    uint64_t calls = 0;     // detour entries (RenderDirLights calls)
    uint64_t draws = 0;     // draws issued inside the sun pass, toggle on or off
    uint64_t skipped = 0;   // of those, the draws not forwarded (toggle on)
};
inline bool windowDue(uint64_t nowMs, uint64_t sinceMs) {
    return nowMs >= sinceMs && nowMs - sinceMs >= kWindowMs;
}

// ---- the log lines' text ----------------------------------------------------------------------------------------------------------------------------------------
inline int formatStatus(char* buf, size_t n, const StatusWords& w, uint32_t frame) {
    const StatusView v = decodeStatus(w);
    char f[16] = "-", f2[16] = "-";
    if (w.flagsKnown) std::snprintf(f, sizeof(f), "0x%08X", static_cast<unsigned>(w.flags));
    if (w.flags2Known) std::snprintf(f2, sizeof(f2), "0x%08X", static_cast<unsigned>(w.flags2));
    return std::snprintf(buf, n,
        "hangar sun probe: status Flags=%s Flags2=%s frame=%u (docked=%d landed=%d supercruise=%d onfoot=%d seated=%d inside_gate=%d)",
        f, f2, static_cast<unsigned>(frame), int(v.docked), int(v.landed), int(v.supercruise), int(v.onFoot), int(v.seated), int(v.inside));
}
inline int formatWatch(char* buf, size_t n, const uint8_t bytes[kMemoryWatchBytes], uint32_t frame, const StatusWords& w) {
    char f[16] = "-";
    if (w.flagsKnown) std::snprintf(f, sizeof(f), "0x%08X", static_cast<unsigned>(w.flags));
    return std::snprintf(buf, n, "hangar sun probe: 0x%X bytes=%02X %02X %02X %02X (frame=%u, status Flags=%s)",
        static_cast<unsigned>(kMemoryWatchRva), bytes[0], bytes[1], bytes[2], bytes[3], static_cast<unsigned>(frame), f);
}
inline int formatWindow(char* buf, size_t n, const Window& w, bool toggleOn) {
    return std::snprintf(buf, n, "hangar sun probe: dirlights_calls=%llu draws_in_scope=%llu skipped=%llu toggle=%s",
        static_cast<unsigned long long>(w.calls), static_cast<unsigned long long>(w.draws), static_cast<unsigned long long>(w.skipped),
        toggleOn ? "on" : "off");
}
inline int formatToggle(char* buf, size_t n, bool toggleOn, uint32_t frame) {
    return std::snprintf(buf, n, "hangar sun probe: skip toggle -> %s at frame=%u (the draws in scope are %s)",
        toggleOn ? "on" : "off", static_cast<unsigned>(frame), toggleOn ? "not forwarded" : "forwarded as before");
}

}  // namespace edvr::sunprobe
