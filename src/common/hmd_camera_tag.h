#pragma once
// The headset camera tag (fix.hmd_camera_tag; docs\camera-rows-carry-2026-09-25.md, "2026-10-11 -- the headset block's writer, traced statically").
//
// Elite's VR job calls the camera coordinator 0x594B60 once per frame (call site 0x286906E). Inside it the headset eye camera's block is committed. EDVR's
// chooser reads the camera block's writes, and the tag says which of them is the headset's: a write made by an Unmap on a thread that is inside the
// coordinator is stamped. The chooser then takes the frame's latest stamped write. With no stamp, or with the key off, the chooser is today's.
//
// Pure: the build-332841 identity and the call-site gate, the per-thread depth that marks the coordinator's nesting, the chooser's pick, and the 30 s
// line's text. No Windows and no globals beyond the thread's depth. The hook is src\d3d11\hmd_camera_tag_hook.cpp, the chooser is chooseCameraRows in
// src\d3d11\temporal_pass.cpp, and the rig is tools\temporal_test.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::hmdtag {

// ---- identity and the gate, build 332841 (EliteDangerous64.exe) --------------------------------------------------------------------------------------------------
constexpr uint32_t kExpectedTimestamp = 1788384820u;   // the PE timestamp (the pair every build-keyed hook here checks)
constexpr uint32_t kExpectedImageSize = 104894464u;
constexpr uint32_t kCoordinatorRva = 0x594B60u;        // the camera coordinator: its only direct caller is the VR job's call at 0x286906E
constexpr uint32_t kCallSiteRva = 0x2869068u;          // B2 01 mov dl,1 ; 48 8B 48 18 mov rcx,[rax+18h] ; E8 the call (at +6) and its rel32 (+7..+10)
constexpr size_t kPrologueBytes = 30;                  // the coordinator's first 30 bytes, read from the exe; CodeHook steals the first 5
constexpr uint8_t kPrologue[kPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,  0x48, 0x89, 0x6C, 0x24, 0x10,  0x48, 0x89, 0x74, 0x24, 0x18,
    0x48, 0x89, 0x7C, 0x24, 0x20,  0x41, 0x54,  0x41, 0x56,  0x41, 0x57,  0x48, 0x83, 0xEC, 0x20};
constexpr size_t kCallSiteBytes = 7;                   // B2 01 48 8B 48 18 E8, from kCallSiteRva
constexpr uint8_t kCallSite[kCallSiteBytes] = {0xB2, 0x01, 0x48, 0x8B, 0x48, 0x18, 0xE8};
constexpr size_t kCallSpan = kCallSiteBytes + 4;       // the call plus its rel32: the bytes the gate reads at kCallSiteRva

inline bool identityMatches(uint32_t timestamp, uint32_t imageSize) {
    return timestamp == kExpectedTimestamp && imageSize == kExpectedImageSize;
}
inline bool prologueMatches(const uint8_t* bytes) {
    return bytes && std::memcmp(bytes, kPrologue, kPrologueBytes) == 0;
}
// site: kCallSpan bytes at kCallSiteRva. The call's target is the next instruction (kCallSiteRva + kCallSpan) plus rel32.
inline bool callSiteMatches(const uint8_t* site) {
    if (!site || std::memcmp(site, kCallSite, kCallSiteBytes) != 0) return false;
    int32_t rel = 0;
    std::memcpy(&rel, site + kCallSiteBytes, 4);
    const int64_t target = int64_t(kCallSiteRva) + int64_t(kCallSpan) + int64_t(rel);
    return target == int64_t(kCoordinatorRva);
}
// nullptr when every check passes; otherwise the reason, for the one log line.
inline const char* gateVerdict(uint32_t timestamp, uint32_t imageSize, const uint8_t* prologue, const uint8_t* site) {
    if (!identityMatches(timestamp, imageSize)) return "not build 332841 (PE timestamp or image size differs)";
    if (!prologueMatches(prologue)) return "the coordinator's first 30 bytes differ from build 332841's";
    if (!callSiteMatches(site)) return "the VR job's call to the coordinator at exe+0x2869068 is not where build 332841 has it";
    return nullptr;
}

// ---- the coordinator's nesting on this thread ----------------------------------------------------------------------------------------------------------------------
// The detour holds a DepthScope across the original call. A camera-block Unmap on this thread while the count is above zero is stamped as the headset's.
// The scope's destructor restores the count on every exit, a C++ throw included, so the count cannot drift.
inline thread_local uint32_t coordinatorDepth = 0;
class DepthScope {
public:
    explicit DepthScope(uint32_t& depth) noexcept : depth_(depth) { ++depth_; }
    ~DepthScope() { --depth_; }
    DepthScope(const DepthScope&) = delete;
    DepthScope& operator=(const DepthScope&) = delete;
private:
    uint32_t& depth_;
};
inline bool insideCoordinator() noexcept { return coordinatorDepth > 0; }

// ---- the chooser's pick ---------------------------------------------------------------------------------------------------------------------------------------------
// One write of the frame's ring as the pick sees it: its frame, its ring clock and whether it was stamped (and valid).
struct Cand {
    uint32_t frame = 0;
    uint32_t seq = 0;
    bool stamped = false;
};
// The index of the latest stamped write of `frame` among n writes (the highest clock), or -1. at(i) names write i.
template <class At>
int latestStamped(int n, uint32_t frame, At at) {
    int best = -1;
    uint32_t bestSeq = 0;
    for (int i = 0; i < n; ++i) {
        const Cand c = at(i);
        if (!c.stamped || c.frame != frame) continue;
        if (best < 0 || c.seq > bestSeq) { best = i; bestSeq = c.seq; }
    }
    return best;
}
// legacyPick is today's chooser's index (-1 for none); tagPick is latestStamped's (-1 for none). With the key on and a stamp, the pick is the tag's.
// With the key off the pick is always today's; disagree is reported either way, so the line says what the tag would have done.
struct Decision {
    int pick = -1;
    bool byTag = false;
    bool disagree = false;   // the tag named a write other than today's pick (a stamp against no pick counts)
    bool fellBack = false;   // no stamp this frame
};
inline Decision decide(int legacyPick, int tagPick, bool keyOn) {
    Decision d;
    d.pick = legacyPick;
    d.disagree = tagPick >= 0 && tagPick != legacyPick;
    d.fellBack = tagPick < 0;
    if (keyOn && tagPick >= 0) {
        d.pick = tagPick;
        d.byTag = true;
    }
    return d;
}

// ---- the 30 s line -----------------------------------------------------------------------------------------------------------------------------------------------------
struct Stats {
    uint64_t coordinatorCalls = 0;   // the detour's calls in the window
    uint64_t stampedWrites = 0;      // the headset block's rotation writes stamped in the window
    uint32_t frames = 0;             // the chooser's frames in the window
    uint32_t framesWithStamp = 0;
    uint32_t chosenByTag = 0;        // frames whose pick the tag made (key on only)
    uint32_t fallback = 0;           // frames with no stamp: today's pick
    uint32_t disagree = 0;           // frames whose tag named another write than today's pick
    uint64_t lastC = 0;              // the last detour call: the coordinator pointer, its thread, the frame
    uint32_t lastThread = 0;
    uint32_t lastFrame = 0;
};
// "temporal hmd tag: hook=live|inert:<why> coordinator_calls=.. per_frame=.. stamped_writes=.. frames_with_stamp=a/b chosen_by_tag=.. fallback=.. disagree=..
//  key=on|off last_frame=.. last_thread=.. last_c=..". Returns the length written, or 0 when the buffer is too small.
inline int formatReport(char* buf, size_t n, bool live, const char* why, bool keyOn, const Stats& s) {
    const double perFrame = s.frames ? double(s.coordinatorCalls) / double(s.frames) : 0.0;
    const int head = live ? std::snprintf(buf, n, "temporal hmd tag: hook=live ")
                          : std::snprintf(buf, n, "temporal hmd tag: hook=inert:%s ", why ? why : "not installed");
    if (head <= 0 || static_cast<size_t>(head) >= n) return 0;
    const int tail = std::snprintf(buf + head, n - static_cast<size_t>(head),
        "coordinator_calls=%llu per_frame=%.2f stamped_writes=%llu frames_with_stamp=%u/%u chosen_by_tag=%u fallback=%u disagree=%u key=%s "
        "last_frame=%u last_thread=%u last_c=%llX",
        static_cast<unsigned long long>(s.coordinatorCalls), perFrame, static_cast<unsigned long long>(s.stampedWrites),
        s.framesWithStamp, s.frames, s.chosenByTag, s.fallback, s.disagree, keyOn ? "on" : "off",
        s.lastFrame, s.lastThread, static_cast<unsigned long long>(s.lastC));
    if (tail <= 0 || static_cast<size_t>(head + tail) >= n) return 0;
    return head + tail;
}

}  // namespace edvr::hmdtag
