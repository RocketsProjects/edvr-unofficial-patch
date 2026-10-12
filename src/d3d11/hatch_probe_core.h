#pragma once
// The hatch-state probe's pure half (state_dump_probe.cpp; TEMPORARY, hotkey.hatch_probe, docs\concourse-culling-2026-10-11.md, Built v8).
// No Windows and no engine memory. The docking readers' object O (the one the readers are called with, `this`) keeps its state at:
//   this+0x3C8  the state byte (u32 read here; the readers compare the byte)
//   this+0x340  the typed state-name value (16 bytes read; decoded offline against the table at 0x5E78C40)
//   owner = [this-8]; owner+0x10 the occupancy count (u32); owner+0x2C and owner+0x2F (bytes).
// The probe reads these every frame for each object it has remembered, and writes a line only when one of them changes.
// The rig tools\builder_probe_test compiles this header alone.
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr::hatch {

constexpr uint64_t kTableRva = 0x5E78C40;   // the eight state-name table qwords (PROVEN: docs Hangar door state, 0x5E78C40 to 0x5E78C98)
constexpr uint32_t kTableCount = 8;
constexpr uint32_t kRemember = 32;          // distinct objects the readers are remembered for (the cap; raised from 8 in Built v8 fix 1)
constexpr uint32_t kRefusedCap = 32;        // distinct refused objects named in the cap line (later refusals are counted, not named)
constexpr uint32_t kLineCap = 400;          // "hatch probe:" change lines per session

struct State {
    bool valid = false;        // every field below was read
    uint32_t state3C8 = 0;
    uint8_t name340[16] = {};
    uint64_t owner = 0;
    uint32_t ownerCount = 0;   // owner+0x10
    uint8_t ownerB2C = 0;      // owner+0x2C
    uint8_t ownerB2F = 0;      // owner+0x2F
};

// True when the two reads differ: a read that appeared or vanished counts, two unreadable reads do not.
inline bool differs(const State& a, const State& b) noexcept {
    if (a.valid != b.valid) return true;
    if (!a.valid) return false;
    if (a.state3C8 != b.state3C8 || a.owner != b.owner || a.ownerCount != b.ownerCount || a.ownerB2C != b.ownerB2C ||
        a.ownerB2F != b.ownerB2F)
        return true;
    for (int i = 0; i < 16; ++i)
        if (a.name340[i] != b.name340[i]) return true;
    return false;
}

// The 16 name bytes as lowercase hex, 32 characters.
inline void hexName(char* out, const uint8_t* bytes) noexcept {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[32] = '\0';
}

// One field's old -> new text pair. An unreadable side is written as "?".
inline int fieldChange(char* out, size_t cap, const char* label, const State& was, const State& now, int which) noexcept {
    char a[40] = "?", b[40] = "?";
    auto fmt = [which](char* dst, size_t n, const State& s) {
        if (!s.valid) return;
        switch (which) {
            case 0: std::snprintf(dst, n, "%u", s.state3C8); break;
            case 1: { char h[33]; hexName(h, s.name340); std::snprintf(dst, n, "%s", h); break; }
            case 2: std::snprintf(dst, n, "%llX", static_cast<unsigned long long>(s.owner)); break;
            case 3: std::snprintf(dst, n, "%u", s.ownerCount); break;
            case 4: std::snprintf(dst, n, "%u", static_cast<unsigned>(s.ownerB2C)); break;
            default: std::snprintf(dst, n, "%u", static_cast<unsigned>(s.ownerB2F)); break;
        }
    };
    fmt(a, sizeof(a), was);
    fmt(b, sizeof(b), now);
    return std::snprintf(out, cap, "%s %s->%s", label, a, b);
}

// The readers that supplied an object, as "r0,r2" (bit i = reader i, r0 to r3); "none" when no bit is set.
inline int formatReaders(char* out, size_t cap, uint8_t mask) noexcept {
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(mask & (1u << i))) continue;
        if (at >= cap) break;
        const int m = std::snprintf(out + at, cap - at, "%sr%d", at ? "," : "", i);
        if (m < 0) break;
        at += static_cast<size_t>(m);
    }
    if (at == 0) return std::snprintf(out, cap, "none");
    return static_cast<int>(at);
}

// "hatch probe: object O (readers r0,r2) frame F t=T ms: <each field old->new>; status flags=.. flags2=.." The status words are the journal
// watcher's Status.json words when they are known; otherwise "unknown". The readers are the reader detours that supplied the object (fix 3).
inline int formatChange(char* out, size_t cap, uint64_t object, uint32_t frame, uint64_t ms, const State& was, const State& now,
                        bool statusKnown, uint32_t flags, uint32_t flags2, uint8_t readers) noexcept {
    char f[6][120];
    fieldChange(f[0], sizeof(f[0]), "3C8", was, now, 0);
    fieldChange(f[1], sizeof(f[1]), "340", was, now, 1);
    fieldChange(f[2], sizeof(f[2]), "owner", was, now, 2);
    fieldChange(f[3], sizeof(f[3]), "owner_10", was, now, 3);
    fieldChange(f[4], sizeof(f[4]), "owner_2C", was, now, 4);
    fieldChange(f[5], sizeof(f[5]), "owner_2F", was, now, 5);
    char status[64];
    if (statusKnown) std::snprintf(status, sizeof(status), "flags=%u flags2=%u", flags, flags2);
    else std::snprintf(status, sizeof(status), "flags unknown");
    char who[32];
    formatReaders(who, sizeof(who), readers);
    return std::snprintf(out, cap, "hatch probe: object %llX (readers %s) frame %u t=%llu ms: %s; %s; %s; %s; %s; %s; status %s",
                         static_cast<unsigned long long>(object), who, frame, static_cast<unsigned long long>(ms), f[0], f[1], f[2],
                         f[3], f[4], f[5], status);
}

// The objects the readers were called with: up to kRemember, each with the readers that supplied it, plus the refusals past the cap.
// Plain data; the engine guards it with a mutex (state_dump_probe.cpp). Built v8 fix 1: cleared on every toggle-on; the first refusal past
// the cap is reported once (takeCapLine); refusals are counted and the first kRefusedCap distinct ones are named.
struct RememberTable {
    enum class Outcome : uint8_t { kIgnored, kNew, kKnown, kRefused };
    uintptr_t obj[kRemember] = {};
    uint8_t readers[kRemember] = {};   // bit i set when reader i supplied obj[k]
    uint32_t count = 0;
    uintptr_t refused[kRefusedCap] = {};
    uint32_t refusedDistinct = 0;      // distinct objects refused past the cap, up to kRefusedCap
    uint32_t refusedOverflow = 0;      // refusals past the named ones (counted per call, not de-duplicated)
    bool capHit = false;
    bool capLogged = false;

    // Forgets every remembered object and every refusal, and the cap line's state. Returns how many objects were remembered.
    uint32_t clear() noexcept {
        const uint32_t n = count;
        *this = RememberTable{};
        return n;
    }

    // Records one reader call's object. A known object gets the reader's bit; a new one is remembered while there is room; past the cap it is
    // refused (named while kRefusedCap allows).
    Outcome note(uintptr_t self, uint8_t readerBit) noexcept {
        if (!self) return Outcome::kIgnored;
        for (uint32_t i = 0; i < count; ++i) {
            if (obj[i] == self) {
                readers[i] = static_cast<uint8_t>(readers[i] | readerBit);
                return Outcome::kKnown;
            }
        }
        if (count < kRemember) {
            obj[count] = self;
            readers[count] = readerBit;
            ++count;
            return Outcome::kNew;
        }
        capHit = true;
        for (uint32_t i = 0; i < refusedDistinct; ++i)
            if (refused[i] == self) return Outcome::kRefused;
        if (refusedDistinct < kRefusedCap) refused[refusedDistinct++] = self;
        else ++refusedOverflow;
        return Outcome::kRefused;
    }

    // True once: the first time the cap has been hit. The engine writes the cap line then.
    bool takeCapLine() noexcept {
        if (!capHit || capLogged) return false;
        capLogged = true;
        return true;
    }
};

// "hatch probe: on, cleared N remembered objects (frame F, trigger T)." The toggle-on line.
inline int formatClearLine(char* out, size_t cap, uint32_t cleared, uint32_t frame, const char* trigger) noexcept {
    return std::snprintf(out, cap, "hatch probe: on, cleared %u remembered objects (frame %u, trigger %s)", cleared, frame,
                         trigger ? trigger : "");
}

// "hatch probe: remembered-object cap (32) reached at frame F: N distinct objects refused so far (M more refusals not named)." Written once.
inline int formatCapLine(char* out, size_t cap, uint32_t frame, const RememberTable& t) noexcept {
    return std::snprintf(out, cap,
                         "hatch probe: remembered-object cap (%u) reached at frame %u: %u distinct objects refused so far (%u more "
                         "refusals not named); the first %u remembered objects stay in the change lines.",
                         kRemember, frame, t.refusedDistinct, t.refusedOverflow, kRemember);
}

// Counts the change lines of one session against the cap. take() says whether this line may be written; a refused line is counted.
struct LineBudget {
    uint32_t cap = kLineCap;
    uint32_t used = 0;
    uint32_t suppressed = 0;
    bool take() noexcept {
        if (used < cap) {
            ++used;
            return true;
        }
        ++suppressed;
        return false;
    }
};

// "hatch probe: table at 0x5E78C40 (RVA 0x5E78C40; image base 0x...): [0]=0x.. [1]=0x.. ... [7]=0x..": the qwords, for offline comparison with
// the +0x340 bytes. Zero qwords are written as 0 (the table's null entry).
inline int formatTable(char* out, size_t cap, uintptr_t imageBase, const uint64_t* ptrs, uint32_t n) noexcept {
    int at = std::snprintf(out, cap, "hatch probe: state-name table at image+0x%llX (absolute 0x%llX):",
                           static_cast<unsigned long long>(kTableRva),
                           static_cast<unsigned long long>(static_cast<uint64_t>(imageBase) + kTableRva));
    if (at < 0) return at;
    for (uint32_t i = 0; i < n && static_cast<size_t>(at) < cap; ++i) {
        const int m = std::snprintf(out + at, cap - static_cast<size_t>(at), " [%u]=0x%llX", i,
                                    static_cast<unsigned long long>(ptrs[i]));
        if (m < 0) break;
        at += m;
    }
    return at;
}

}  // namespace edvr::hatch
