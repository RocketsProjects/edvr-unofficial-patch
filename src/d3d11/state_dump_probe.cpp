// The state dump (state_dump_probe.h): the engine side. The pure half (the reader
// table, the regions, the capped diff, the CSV and the lines) is in state_dump_core.h.
//
// Four detours on the docking-state readers (0x12C3C20, 0x12CAF80, 0x12CADE0, 0x12CB320).
// Each reader takes its object in rcx; the detour forwards rcx and returns RAX, and records
// the object and AL after the forward. The probe never calls the getters the readers call.
// While the gate is closed (no capture) the relays go straight to the originals.
//
// Per owned frame, at the Present tick: the readers' last observation of the frame, the
// journal counters and the Status words, and for each distinct object the readers have named
// (up to four): its first 0x400 bytes, the 0x40 bytes at +0x340, and the 0x100 bytes behind
// the pointer at +0x240, each diffed against the previous frame. Every read is SEH-guarded
// and a fault is recorded, not raised.
#include "state_dump_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "journal_watch.h"
#include "state_dump_core.h"

namespace sd = edvr::statedump;

namespace edvr {
namespace {

constexpr uint64_t kDiagDelayMs = 20000;
constexpr int kLogLineCap = 60;

// Engine memory, read under SEH. POD only.
bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept { return readMem(dst, src, sizeof(T)); }

// ---------------------------------------------------------------------------
// Relay machinery, mirrored from shadow_gate_probe.cpp and kinematic_eval_hook.cpp (kept as copies).
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline].
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

struct Detour {
    const char* name;
    uintptr_t rva;
    void* callback;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
};

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& d = *static_cast<Detour*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(d.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(d.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), d.relay, kRelayBytes))
        return false;
    d.forward.store(address, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// The reader observers. Each reader returns its code in AL; the observer returns RAX unchanged.
using ReaderFn = uintptr_t(__fastcall*)(uintptr_t self);

uintptr_t observeReader(int i, uintptr_t self) noexcept;
__declspec(noinline) uintptr_t __fastcall stateReader0(uintptr_t self) noexcept { return observeReader(0, self); }
__declspec(noinline) uintptr_t __fastcall stateReader1(uintptr_t self) noexcept { return observeReader(1, self); }
__declspec(noinline) uintptr_t __fastcall stateReader2(uintptr_t self) noexcept { return observeReader(2, self); }
__declspec(noinline) uintptr_t __fastcall stateReader3(uintptr_t self) noexcept { return observeReader(3, self); }

alignas(8) std::atomic<uintptr_t> g_stateGate{0};
static_assert(decltype(g_stateGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

Detour g_readers[sd::kReaders] = {
    {"state-reader-FUN_12C3C20", sd::kReaderRva[0], reinterpret_cast<void*>(&stateReader0)},
    {"state-reader-FUN_12CAF80", sd::kReaderRva[1], reinterpret_cast<void*>(&stateReader1)},
    {"state-reader-FUN_12CADE0", sd::kReaderRva[2], reinterpret_cast<void*>(&stateReader2)},
    {"state-reader-FUN_12CB320", sd::kReaderRva[3], reinterpret_cast<void*>(&stateReader3)},
};

struct Slot {
    std::atomic<uintptr_t> object{0};
    std::atomic<uint32_t> code{0};
    std::atomic<uint32_t> stamp{0};    // the frame stamp of the last call
    std::atomic<uint32_t> calls{0};    // cumulative while armed
};
Slot g_slot[sd::kReaders];
std::atomic<bool> g_armed{false};
std::atomic<uint32_t> g_stamp{0};      // the frame the work now running belongs to

uintptr_t observeReader(int i, uintptr_t self) noexcept {
    const auto forward = reinterpret_cast<ReaderFn>(g_readers[i].forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    const uintptr_t result = forward(self);
    if (g_armed.load(std::memory_order_acquire)) {
        Slot& s = g_slot[i];
        s.object.store(self, std::memory_order_relaxed);
        s.code.store(static_cast<uint32_t>(result & 0xFFu), std::memory_order_relaxed);
        s.stamp.store(g_stamp.load(std::memory_order_relaxed), std::memory_order_relaxed);
        s.calls.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Installing the detours (the first start only; each stands down alone).
const char* installDetour(Detour& d, uintptr_t base) noexcept {
    if (d.ready.load(std::memory_order_acquire)) return "installed";
    uint8_t got[sizeof(sd::kReaderPin)] = {};
    if (!readMem(got, base + d.rva, sizeof(got)) || !sd::pinMatches(got, sd::kReaderPin, sizeof(got)))
        return "prologue mismatch";
    d.relay = allocateRelay(base + d.rva);
    if (!d.relay) return "no relay within range";
    buildRelay(d.relay, &g_stateGate, d.callback);
    if (!d.hook.install(reinterpret_cast<void*>(base + d.rva), d.relay, nullptr, d.name, &prepareRelay, &d)) {
        VirtualFree(d.relay, 0, MEM_RELEASE);
        d.relay = nullptr;
        return "CodeHook refused (its own line says why)";
    }
    d.ready.store(true, std::memory_order_release);
    return "installed";
}

std::string ensureDetours(uintptr_t base) {
    std::string status;
    for (int i = 0; i < sd::kReaders; ++i) {
        if (i) status += " ";
        status += std::string("r") + std::to_string(i) + "=" + installDetour(g_readers[i], base);
    }
    return status;
}

// ---------------------------------------------------------------------------
// Capture state (render thread only, except the slots above).
struct Track {
    uintptr_t addr = 0;
    bool havePrev = false;
    uint8_t obj[sd::kObjBytes] = {};
    uint8_t sub[sd::kSubBytes] = {};
    uint8_t ptr[sd::kPtrBytes] = {};
    uintptr_t ptrValue = 0;
};

struct Row {
    sd::FrameRow fields;
    std::string changes;
    uint32_t changedBytes = 0;
};

bool g_diagOn = false;
bool g_waitingForScene = false;
uint64_t g_requestAtMs = 0;
bool g_pending = false;
const char* g_pendingTrigger = "";
bool g_flat = false;
bool g_capturing = false;
uint32_t g_firstFrame = 0;             // the first work frame of the capture
uint32_t g_lastRowFrame = 0;
const char* g_trigger = "";
uint32_t g_callsLast[sd::kReaders] = {};
uint32_t g_codeLast[sd::kReaders] = {};
bool g_codeSeen[sd::kReaders] = {};
uintptr_t g_objectLast[sd::kReaders] = {};
sd::Counters g_countersLast{};
bool g_countersSeen = false;
Track g_tracks[sd::kTracks];
std::vector<Row> g_rows;
int g_linesLeft = kLogLineCap;
uint32_t g_faults = 0;
std::string g_detourStatus;

// Snapshots one track's three regions and diffs them against the previous frame.
void trackObject(Track& t, uintptr_t addr, std::string& changes, uint32_t& changed, uint32_t& faults) {
    uint8_t obj[sd::kObjBytes] = {};
    uint8_t sub[sd::kSubBytes] = {};
    uint8_t ptr[sd::kPtrBytes] = {};
    uintptr_t pointer = 0;
    const bool objOk = readMem(obj, addr, sizeof(obj));
    const bool subOk = readMem(sub, addr + sd::kSubOffset, sizeof(sub));
    if (!objOk || !subOk) ++faults;
    if (readVal(&pointer, addr + sd::kPtrOffset) && pointer) {
        if (!readMem(ptr, pointer, sizeof(ptr))) {
            std::memset(ptr, 0, sizeof(ptr));
            pointer = 0;
            ++faults;
        }
    } else {
        pointer = 0;
    }
    char name[24];
    if (t.havePrev) {
        sd::Change list[sd::kChangesPerRegion];
        int total = 0;
        int n = sd::diffBytes(t.obj, obj, sizeof(obj), 0, list, sd::kChangesPerRegion, &total);
        if (n || total) {
            std::snprintf(name, sizeof(name), "A%llX.obj", static_cast<unsigned long long>(addr & 0xFFFFu));
            std::string s(512, '\0');
            size_t at = 0;
            at = sd::appendChanges(&s[0], s.size(), at, name, list, n, total);
            s.resize(at);
            changes += (changes.empty() ? "" : " ") + s;
        }
        changed += static_cast<uint32_t>(total);
        n = sd::diffBytes(t.sub, sub, sizeof(sub), static_cast<uint32_t>(sd::kSubOffset), list,
                          sd::kChangesPerRegion, &total);
        if (n || total) {
            std::snprintf(name, sizeof(name), "A%llX.sub", static_cast<unsigned long long>(addr & 0xFFFFu));
            std::string s(512, '\0');
            size_t at = sd::appendChanges(&s[0], s.size(), 0, name, list, n, total);
            s.resize(at);
            changes += (changes.empty() ? "" : " ") + s;
        }
        changed += static_cast<uint32_t>(total);
        n = sd::diffBytes(t.ptr, ptr, sizeof(ptr), static_cast<uint32_t>(sd::kPtrOffset), list,
                          sd::kChangesPerRegion, &total);
        if (n || total) {
            std::snprintf(name, sizeof(name), "A%llX.ptr", static_cast<unsigned long long>(addr & 0xFFFFu));
            std::string s(512, '\0');
            size_t at = sd::appendChanges(&s[0], s.size(), 0, name, list, n, total);
            s.resize(at);
            changes += (changes.empty() ? "" : " ") + s;
        }
        changed += static_cast<uint32_t>(total);
        if (pointer != t.ptrValue) {
            char p[64];
            std::snprintf(p, sizeof(p), "%sA%llX.pointer:%llX>%llX", changes.empty() ? "" : " ",
                          static_cast<unsigned long long>(addr & 0xFFFFu),
                          static_cast<unsigned long long>(t.ptrValue), static_cast<unsigned long long>(pointer));
            changes += p;
        }
    }
    std::memcpy(t.obj, obj, sizeof(obj));
    std::memcpy(t.sub, sub, sizeof(sub));
    std::memcpy(t.ptr, ptr, sizeof(ptr));
    t.ptrValue = pointer;
    t.addr = addr;
    t.havePrev = true;
}

// Collects the row of one finished frame (stamp == frame): counters, Status words, the readers, the tracks.
void collectRow(uint32_t frame) {
    Row row;
    sd::FrameRow& f = row.fields;
    f.index = static_cast<uint32_t>(g_rows.size());
    f.frame = frame;
    f.counters.docks = journalDocks();
    f.counters.undocks = journalUndocks();
    f.counters.disembarks = journalDisembarks();
    f.counters.embarks = journalEmbarks();
    const JournalRawStatus raw = journalRawStatus();
    f.flags = raw.flags;
    f.flags2 = raw.flags2;
    f.flagsKnown = raw.flagsKnown;
    f.flags2Known = raw.flags2Known;

    for (int i = 0; i < sd::kReaders; ++i) {
        const Slot& s = g_slot[i];
        const uint32_t now = s.calls.load(std::memory_order_relaxed);
        const bool called = s.stamp.load(std::memory_order_relaxed) == frame && now != g_callsLast[i];
        f.calls[i] = called ? now - g_callsLast[i] : 0;
        g_callsLast[i] = now;
        if (called) {
            f.code[i] = static_cast<int>(s.code.load(std::memory_order_relaxed));
            f.object[i] = s.object.load(std::memory_order_relaxed);
        } else {
            f.code[i] = -1;
            f.object[i] = s.object.load(std::memory_order_relaxed);   // the last object it named, for tracking
        }
    }

    // The distinct objects the readers have named: each gets a track (a fresh one when none is free).
    for (int i = 0; i < sd::kReaders; ++i) {
        const uintptr_t obj = f.object[i];
        if (!obj) continue;
        Track* t = nullptr;
        for (Track& k : g_tracks)
            if (k.addr == obj) t = &k;
        if (!t) {
            for (Track& k : g_tracks)
                if (!k.addr) { t = &k; break; }
        }
        if (!t) continue;
        if (t->addr != obj) *t = Track{};
        trackObject(*t, obj, row.changes, row.changedBytes, g_faults);
    }
    if (row.changes.empty()) row.changes = "-";
    g_rows.push_back(std::move(row));
}

// The events between the previous row and this one, as log lines (capped).
void logEvents(const Row& r) {
    if (g_linesLeft <= 0) return;
    const sd::Counters& c = r.fields.counters;
    if (g_countersSeen) {
        const struct { const char* name; uint32_t d; } deltas[] = {
            {"docks", sd::deltaOf(g_countersLast.docks, c.docks)},
            {"undocks", sd::deltaOf(g_countersLast.undocks, c.undocks)},
            {"disembarks", sd::deltaOf(g_countersLast.disembarks, c.disembarks)},
            {"embarks", sd::deltaOf(g_countersLast.embarks, c.embarks)},
        };
        for (const auto& d : deltas) {
            if (!d.d || g_linesLeft <= 0) continue;
            char line[160];
            sd::formatEventLine(line, sizeof(line), r.fields.frame, d.name, d.d);
            Log::get().note("%s", line);
            --g_linesLeft;
        }
    }
    g_countersLast = c;
    g_countersSeen = true;
    for (int i = 0; i < sd::kReaders && g_linesLeft > 0; ++i) {
        if (r.fields.code[i] < 0) continue;
        if (!g_codeSeen[i] || g_codeLast[i] != static_cast<uint32_t>(r.fields.code[i])) {
            char what[48];
            std::snprintf(what, sizeof(what), "reader%d code %d", i, r.fields.code[i]);
            char line[160];
            sd::formatEventLine(line, sizeof(line), r.fields.frame, what, 0);
            Log::get().note("%s", line);
            --g_linesLeft;
            g_codeLast[i] = static_cast<uint32_t>(r.fields.code[i]);
            g_codeSeen[i] = true;
        }
        if (r.fields.object[i] && r.fields.object[i] != g_objectLast[i] && g_linesLeft > 0) {
            char what[64];
            std::snprintf(what, sizeof(what), "reader%d object %llX", i,
                          static_cast<unsigned long long>(r.fields.object[i] & 0xFFFFFFFFull));
            char line[160];
            sd::formatEventLine(line, sizeof(line), r.fields.frame, what, 0);
            Log::get().note("%s", line);
            --g_linesLeft;
            g_objectLast[i] = r.fields.object[i];
        }
    }
    if (r.changedBytes && g_linesLeft > 0) {
        char line[200];
        sd::formatEventLine(line, sizeof(line), r.fields.frame, "bytes changed", r.changedBytes);
        Log::get().note("%s", line);
        --g_linesLeft;
    }
}

void finishCapture(uint32_t frame) {
    g_stateGate.store(0, std::memory_order_release);
    g_armed.store(false, std::memory_order_release);
    g_capturing = false;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[24] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    char csvName[64];
    std::snprintf(csvName, sizeof(csvName), "state_dump_%s.csv", stamp);
    std::wstring path = Config::get().logDir() + L"\\state_dump_";
    for (const char* c = stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        std::fprintf(f, "%s\n", sd::csvHeader());
        for (const Row& r : g_rows) {
            char line[1024];
            sd::FrameRow fr = r.fields;
            fr.changes = r.changes.c_str();
            sd::formatFrameRow(line, sizeof(line), fr);
            std::fprintf(f, "%s\n", line);
        }
        std::fclose(f);
    } else {
        Log::get().note("state dump: could not open edvr_logs\\%s; the lines above are complete.", csvName);
    }
    const sd::Counters& c = g_countersLast;
    Log::get().note("state dump: done frames=%u from=%u docks=%u undocks=%u disembarks=%u embarks=%u "
                    "faults=%u csv=%s (the readers' codes and objects are in the CSV; the journal counts are cumulative).",
                    static_cast<unsigned>(g_rows.size()), g_firstFrame, c.docks, c.undocks, c.disembarks, c.embarks,
                    g_faults, csvName);
    g_rows.clear();
    g_rows.shrink_to_fit();
    for (Track& t : g_tracks) t = Track{};
    (void)frame;
}

void startCapture(uint32_t firstFrame, const char* trigger) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        Log::get().note("state dump: refused (trigger %s): the module base is unknown.", trigger);
        return;
    }
    g_detourStatus = ensureDetours(base);
    g_rows.clear();
    g_rows.reserve(sd::kFrames);
    for (Slot& s : g_slot) {
        s.object.store(0);
        s.code.store(0);
        s.stamp.store(0);
        s.calls.store(0);
    }
    for (int i = 0; i < sd::kReaders; ++i) {
        g_callsLast[i] = 0;
        g_codeSeen[i] = false;
        g_objectLast[i] = 0;
        g_codeLast[i] = 0;
    }
    g_countersSeen = false;
    g_countersLast = {};
    g_faults = 0;
    g_linesLeft = kLogLineCap;
    for (Track& t : g_tracks) t = Track{};
    g_firstFrame = firstFrame;
    g_trigger = trigger;
    g_capturing = true;
    g_stamp.store(firstFrame, std::memory_order_release);
    g_stateGate.store(1, std::memory_order_release);
    g_armed.store(true, std::memory_order_release);

    sd::StartInfo info;
    info.firstFrame = firstFrame;
    info.trigger = trigger;
    info.detours = g_detourStatus.c_str();
    info.csvName = "state_dump_<stamp>.csv (written at the end)";
    char line[400];
    sd::formatStartLine(line, sizeof(line), info);
    Log::get().note("%s", line);
}

}  // namespace

void stateDumpInstall(bool flatProfile) {
    g_flat = flatProfile;
    const std::string v = Config::get().getString("diag.state_dump", "off");
    if (v == "on") {
        g_diagOn = true;
        g_waitingForScene = !flatProfile;
        if (flatProfile)
            Log::get().note("state dump: diag.state_dump = on, but flat has no scene flag, so it waits and never fires: "
                            "bind hotkey.state_dump there.");
        else
            Log::get().note("state dump: diag.state_dump = on: one %u-frame capture, 20 s after the first in-scene frame.",
                            sd::kFrames);
    } else if (v != "off" && !v.empty()) {
        Log::get().note("state dump: diag.state_dump = '%s' is not on or off; treated as off.", v.c_str());
    } else {
        Log::get().note("state dump: diag.state_dump = off: nothing is hooked until hotkey.state_dump is pressed.");
    }
}

void stateDumpRequest(const char* trigger) {
    if (g_capturing || g_pending) {
        Log::get().note("state dump: a capture is already %s; the request (%s) is refused.",
                        g_pending ? "pending" : "running", trigger);
        return;
    }
    g_pending = true;
    g_pendingTrigger = trigger;
    Log::get().note("state dump: requested by %s; %u frames are captured from the next frame.", trigger, sd::kFrames);
}

void stateDumpFrame(uint32_t frame) {
    // The work that just finished is stamped `frame`: collect its row first.
    if (g_capturing) {
        collectRow(frame);
        logEvents(g_rows.back());
        if (g_rows.size() >= sd::kFrames) {
            finishCapture(frame);
        }
    }
    // The work after this tick is stamped frame + 1.
    g_stamp.store(frame + 1, std::memory_order_release);
    if (g_waitingForScene) {
        if (!sceneArrived()) return;
        g_waitingForScene = false;
        g_requestAtMs = GetTickCount64() + kDiagDelayMs;
        Log::get().note("state dump: the first in-scene frame; the capture is requested in 20 s.");
    }
    if (g_requestAtMs && GetTickCount64() >= g_requestAtMs) {
        g_requestAtMs = 0;
        if (!g_capturing && !g_pending) {
            g_pending = true;
            g_pendingTrigger = "diag.state_dump = on";
        }
    }
    if (g_pending && !g_capturing) {
        g_pending = false;
        startCapture(frame + 1, g_pendingTrigger);
    }
}

}  // namespace edvr
