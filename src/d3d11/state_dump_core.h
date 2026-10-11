#pragma once
// The state dump's pure half (state_dump_probe.cpp; TEMPORARY, the M2 measurement of
// docs/concourse-culling-2026-10-11.md). No Windows, no engine memory: the four
// docking-state readers and their pinned prologue, the region layout behind each
// reader's object, the capped byte diff, the journal counter deltas, and the text
// of the CSV rows and the log lines. The rig tools\state_dump_test compiles this
// header alone.
//
// The readers (build 332841): each takes the object in rcx, calls its virtual
// bool getter at [obj+0x88] (slot 17), and returns a small code in AL. Their
// first six bytes are the same in all four (push rbx; sub rsp,20h). The probe
// observes each call's object and returned code; it never calls the getter.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::statedump {

// Build 332841, the hash-verified exe (cpu_sampler.h pins the same stamp and size).
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;

constexpr int kReaders = 4;
constexpr uintptr_t kReaderRva[kReaders] = {0x12C3C20u, 0x12CAF80u, 0x12CADE0u, 0x12CB320u};
constexpr uint8_t kReaderPin[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};   // push rbx; sub rsp,20h (all four)

// What is read behind each reader's object (the object is the reader's rcx).
constexpr uintptr_t kObjBytes = 0x400;      // the object's first bytes
constexpr uintptr_t kSubOffset = 0x340;     // the sub-object the reader hands to its helper: this+0x340
constexpr uintptr_t kSubBytes = 0x40;
constexpr uintptr_t kPtrOffset = 0x240;     // the pointer at this+0x240, dereferenced (the lighting object's home)
constexpr uintptr_t kPtrBytes = 0x100;

constexpr uint32_t kFrames = 600;           // the capture length, in owned frames
constexpr int kChangesPerRegion = 16;       // the cap on listed bytes per region per frame
constexpr int kTracks = 4;                  // distinct objects followed at once

struct Change {
    uint32_t offset = 0;     // from the region's start (the region's base is added by the caller)
    uint8_t was = 0, now = 0;
};

inline bool pinMatches(const uint8_t* got, const uint8_t* want, size_t n) noexcept {
    return got && want && std::memcmp(got, want, n) == 0;
}

// Diffs `n` bytes. Keeps the first `cap` changes in out and counts them all in *total.
// Returns how many were kept.
inline int diffBytes(const uint8_t* was, const uint8_t* now, size_t n, uint32_t base, Change* out, int cap,
                     int* total) noexcept {
    int kept = 0, all = 0;
    for (size_t i = 0; i < n; ++i) {
        if (was[i] == now[i]) continue;
        ++all;
        if (kept < cap) {
            out[kept].offset = base + static_cast<uint32_t>(i);
            out[kept].was = was[i];
            out[kept].now = now[i];
            ++kept;
        }
    }
    if (total) *total = all;
    return kept;
}

// Writes "name:+0xOFF:WW>NN" items separated by spaces, starting at `at`, never past cap-1. Returns the new length.
inline size_t appendChanges(char* out, size_t cap, size_t at, const char* name, const Change* list, int n,
                            int total) noexcept {
    if (at >= cap) return cap - 1;
    for (int i = 0; i < n; ++i) {
        const int w = std::snprintf(out + at, cap - at, "%s%s+0x%X:%02X>%02X", at ? " " : "", name,
                                    static_cast<unsigned>(list[i].offset), list[i].was, list[i].now);
        if (w < 0 || static_cast<size_t>(w) >= cap - at) return cap - 1;
        at += static_cast<size_t>(w);
    }
    if (total > n) {
        const int w = std::snprintf(out + at, cap - at, " %s+%d_more", name, total - n);
        if (w > 0 && static_cast<size_t>(w) < cap - at) at += static_cast<size_t>(w);
    }
    return at;
}

// The journal's counters at one frame. Each is cumulative for the journal session.
struct Counters {
    uint32_t docks = 0, undocks = 0, disembarks = 0, embarks = 0;
};

// The increase of one counter between two frames: a count that went down (a new
// journal session, or a reset) is no event.
inline uint32_t deltaOf(uint32_t was, uint32_t now) noexcept { return now >= was ? now - was : 0u; }

// One captured frame, as the CSV row and the log both see it.
struct FrameRow {
    uint32_t index = 0;             // 0-based within the capture
    uint32_t frame = 0;             // the Present frame clock at the end of the work
    Counters counters{};
    uint32_t flags = 0, flags2 = 0;
    bool flagsKnown = false, flags2Known = false;
    int code[kReaders] = {};        // the reader's AL this frame; -1 when it was not called
    uint32_t calls[kReaders] = {};  // calls this frame
    uintptr_t object[kReaders] = {};// the reader's object this frame (0 when not called)
    const char* changes = "";       // the compact byte changes of every tracked region
};

inline const char* csvHeader() noexcept {
    return "index,frame,docks,undocks,disembarks,embarks,flags_hex,flags2_hex,flags_known,flags2_known,"
           "code0,code1,code2,code3,calls0,calls1,calls2,calls3,obj0,obj1,obj2,obj3,changes";
}

inline int formatFrameRow(char* out, size_t cap, const FrameRow& r) noexcept {
    return std::snprintf(out, cap,
                         "%u,%u,%u,%u,%u,%u,0x%08X,0x%08X,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,0x%llX,0x%llX,0x%llX,0x%llX,%s",
                         r.index, r.frame, r.counters.docks, r.counters.undocks, r.counters.disembarks,
                         r.counters.embarks, r.flags, r.flags2, r.flagsKnown ? 1 : 0, r.flags2Known ? 1 : 0,
                         r.code[0], r.code[1], r.code[2], r.code[3], r.calls[0], r.calls[1], r.calls[2], r.calls[3],
                         static_cast<unsigned long long>(r.object[0]), static_cast<unsigned long long>(r.object[1]),
                         static_cast<unsigned long long>(r.object[2]), static_cast<unsigned long long>(r.object[3]),
                         r.changes[0] ? r.changes : "-");
}

// The start line: what is armed and for how long.
struct StartInfo {
    uint32_t firstFrame = 0;
    const char* trigger = "";
    const char* detours = "";
    const char* csvName = "";
};

inline int formatStartLine(char* out, size_t cap, const StartInfo& s) noexcept {
    return std::snprintf(out, cap,
                         "state dump: armed for frames %u..%u (trigger %s; detours: %s); csv=%s. Journal and Status "
                         "fields are zero in flat, as the journal's own accessors are.",
                         s.firstFrame, s.firstFrame + kFrames - 1, s.trigger, s.detours, s.csvName);
}

// An event line: a counter moved, a reader's code changed, or an object was seen for the first time.
inline int formatEventLine(char* out, size_t cap, uint32_t frame, const char* what, uint32_t n) noexcept {
    if (n == 0) return std::snprintf(out, cap, "state dump: f=%u %s", frame, what);
    return std::snprintf(out, cap, "state dump: f=%u %s +%u", frame, what, n);
}

}  // namespace edvr::statedump
