#pragma once
// The hangar sun probe (TEMPORARY diagnostic; docs\hangar-sunlight-2026-10-11.md says what it is for). Pure: the build-332841 identity (for the memory
// watch only), the sun pair and its skip table, the draw classification and the skip decision, the Status.json decode, the memory-watch change
// detector, the 10 s window, and the text of every log line. No Windows and no globals. The glue is src\d3d11\sun_pass_probe.cpp; the rig is
// tools\sun_pass_probe_test.
//
// WHAT THE SKIP KEYS ON. The sun's per-frame light is a full-screen deferred pass. Its vertex shader VS 7E38A6AA1269C901 forms the per-eye view rays
// from a five-float quad, and its pixel shader PS 7CECABDE34FFBE9E is the deferred resolve: it reads the shadow mask at t3 and multiplies the direct
// diffuse and specular by one channel of it (docs\ambient-occlusion.md, P5; docs\scanner-body.md, "The mechanism"). A draw is the sun pass when its
// bound pair is an entry of the skip table. The draw hook reads the bound content hashes (binding_shadow.h), so no engine code is patched.
//
// WHAT IS NOT THE SKIP. RenderDirLights (EliteDangerous64.exe RVA 0x5AC9C0) binds the directional constants and the G-buffer and shadow inputs into a
// pass object. Its one call in the 2026-10-11 flight issued no draw through the draw hooks, and its body makes no ID3D11 draw call. The probe does not
// key on it. (docs\hangar-sunlight-2026-10-11.md, the second 2026-10-11 entry.)
//
// WHAT THE STATUS DECODE TRUSTS. EDVR's journal_watch.h/.cpp reads Flags bit 4 (supercruise), bits 24-26 (seated: main ship, fighter, SRV), bit 30
// (the FSD jump), and Flags2 bit 0 (OnFoot) and bits 1-2 (taxi, multicrew), all from Elite's Status.json, and those are the verified bits here.
// Flags bit 0 (Docked) and bit 1 (Landed) are the design doc's reading (its I3), NOT yet confirmed on a flight; the flight logs the raw words so
// they can be checked. No hangar bit is confirmed in any published table reachable from here, so the probe prints the raw Flags and Flags2 in hex on
// every line and reports the design doc's inside gate as `inside_gate`, not as an in-hangar bit.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::sunprobe {

// ---- build identity: the memory watch's RVA is build-specific (build 332841 only) -----------------------------------------------------------------------
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;
inline bool identityMatches(uint32_t timestamp, uint32_t imageSize) {
    return timestamp == kExpectedTimestamp && imageSize == kExpectedImageSize;
}

// ---- the sun pair and the skip table ----------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kSunVs = 0x7E38A6AA1269C901ull;   // the full-screen deferred vertex shader (per-eye view rays)
constexpr uint64_t kSunPs = 0x7CECABDE34FFBE9Eull;   // the deferred resolve pixel shader (direct light times the shadow mask)
struct ShaderPair {
    uint64_t vs;
    uint64_t ps;
    const char* name;
};
// A draw is skipped (behind the toggle) when its bound VS and PS are both an entry here. One entry: the sun.
inline constexpr ShaderPair kSkipTable[] = {
    {kSunVs, kSunPs, "sun"},
};
constexpr size_t kSkipTableCount = sizeof(kSkipTable) / sizeof(kSkipTable[0]);

// What one draw's bound shaders say. A pair is an entry that lists both hashes. The two half matches are counted apart, so a shader that changed in
// some later build shows as a half match with no pairs.
enum class Match { kNone, kVsOnly, kPsOnly, kPair };
inline Match classify(uint64_t vs, uint64_t ps) {
    bool vsHit = false;
    bool psHit = false;
    for (size_t i = 0; i < kSkipTableCount; ++i) {
        const bool v = vs == kSkipTable[i].vs;
        const bool p = ps == kSkipTable[i].ps;
        if (v && p) return Match::kPair;
        vsHit = vsHit || v;
        psHit = psHit || p;
    }
    if (vsHit) return Match::kVsOnly;
    if (psHit) return Match::kPsOnly;
    return Match::kNone;
}

// The window's counters, as indices into the glue's array of atomics. A pair counts in kFieldPairs and, when it is skipped, in kFieldSkipped as well.
enum Field : size_t { kFieldPairs = 0, kFieldVsOnly, kFieldPsOnly, kFieldSkipped, kFieldCount };
// The counter a draw of this match adds to first (kFieldCount for none).
inline size_t fieldFor(Match m) {
    switch (m) {
        case Match::kPair: return kFieldPairs;
        case Match::kVsOnly: return kFieldVsOnly;
        case Match::kPsOnly: return kFieldPsOnly;
        default: return kFieldCount;
    }
}
// The skip: only a pair, and only while the toggle is on. Every other draw is forwarded.
inline bool skipsDraw(Match m, bool toggleOn) noexcept {
    return m == Match::kPair && toggleOn;
}

// ---- the Status.json decode -------------------------------------------------------------------------------------------------------------------------------
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

// ---- the memory watch ------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMemoryWatchRva = 0x5E889C0u;         // the 4-byte group the light dispatch reads (design doc P8); read-only
constexpr size_t kMemoryWatchBytes = 4;
constexpr uint32_t kImageScnMemRead = 0x40000000u;       // IMAGE_SCN_MEM_READ
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

// ---- the 10 s window --------------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kWindowMs = 10000;                    // the periodic line's period
struct Window {
    uint64_t pairs = 0;     // draws whose bound VS and PS are a table pair, toggle on or off
    uint64_t vsOnly = 0;    // draws whose VS is the table's VS and whose PS is not its partner
    uint64_t psOnly = 0;    // draws whose PS is the table's PS and whose VS is not its partner
    uint64_t skipped = 0;   // of the pair draws, the ones not forwarded (toggle on)
};
inline bool windowActive(const Window& w) {
    return w.pairs != 0 || w.vsOnly != 0 || w.psOnly != 0 || w.skipped != 0;
}
inline bool windowDue(uint64_t nowMs, uint64_t sinceMs) {
    return nowMs >= sinceMs && nowMs - sinceMs >= kWindowMs;
}

// ---- the log lines' text ----------------------------------------------------------------------------------------------------------------------------------
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
    return std::snprintf(buf, n, "hangar sun probe: sun pair draws=%llu skipped=%llu (VS only=%llu, PS only=%llu) toggle=%s",
        static_cast<unsigned long long>(w.pairs), static_cast<unsigned long long>(w.skipped),
        static_cast<unsigned long long>(w.vsOnly), static_cast<unsigned long long>(w.psOnly), toggleOn ? "on" : "off");
}
inline int formatToggle(char* buf, size_t n, bool toggleOn, uint32_t frame) {
    return std::snprintf(buf, n, "hangar sun probe: skip toggle -> %s at frame=%u (the sun pair's draws are %s)",
        toggleOn ? "on" : "off", static_cast<unsigned>(frame), toggleOn ? "not forwarded" : "forwarded as before");
}

}  // namespace edvr::sunprobe
