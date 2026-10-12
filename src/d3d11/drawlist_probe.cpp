// The draw-list probe's engine half (TEMPORARY, Built v9). See drawlist_probe.h for the taps and drawlist_probe_core.h for the origin classes,
// the carried-candidate sets, and the text. The probe holds its own lock only while it updates its counters and rows; the names and keys are
// read before that lock is taken, and nothing is called into the game or the trial while it is held.
#include "drawlist_probe.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "builder_probe.h"
#include "drawlist_probe_core.h"
#include "engine_velocity.h"
#include "engine_velocity_families.h"
#include "sealed_trial.h"
#include "kinematic_eval_hook.h"
#include "store_hook.h"

namespace edvr {

namespace {

namespace dl = edvr::drawlist;

constexpr size_t kRowCap = 6000;        // CSV rows per window; past it, rows are counted, not written
constexpr size_t kNameCap = 512;        // distinct keys named per class per window
constexpr uintptr_t kRecKey = 0x250;    // the rig record's pool key (FUN_143696FA0's argument; engine_velocity_emit.h kRecordKey)
constexpr size_t kTopNames = 15;

enum class Phase : uint8_t { kIdle = 0, kArmed = 1, kCollecting = 2 };

struct NameRec {
    uint64_t key = 0;
    char name[48] = {};
    uint32_t last = 0;          // the game frame it was last appended in
};

std::mutex g_mutex;                    // the phase, the counters and the rows; never held across a call into the game or the trial
Phase g_phase = Phase::kIdle;          // under g_mutex
std::atomic<bool> g_wanted{false};     // armed or collecting: drawlistWanted()
uint32_t g_windowFrames = 0;           // completed frames of the window (under g_mutex)
uint32_t g_frameNo = 0;                // the game frame of the open window frame (under g_mutex)
const char* g_modeText = "off";        // the trial's rule at the open frame (under g_mutex)
dl::KeySet g_keysA, g_keysB;           // the keys each path appended (under g_mutex)
std::vector<NameRec> g_namesA, g_namesB;
dl::FrameCounts g_counts;
struct IdentOp {
    uint64_t id;
    const char* kind;
};
std::vector<IdentOp> g_idents;         // the collections a copy, merge or clear touched this frame
std::vector<std::string> g_rows;
uint64_t g_rowsDropped = 0;
uint64_t g_namesDropped = 0;
std::atomic<uint64_t> g_famDraws[dl::kFamilySlots] = {};

// A 64-bit read from game memory, guarded: a failed read is false, not a fault.
bool readU64(uintptr_t src, uint64_t* out) noexcept {
    if (!src) return false;
    __try {
        *out = *reinterpret_cast<const volatile uint64_t*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void addRow(const dl::CsvRow& r) {   // under g_mutex
    if (g_rows.size() >= kRowCap) {
        ++g_rowsDropped;
        return;
    }
    char buf[512];
    dl::formatRow(buf, sizeof(buf), r);
    g_rows.emplace_back(buf);
}

// Records the key's name and the frame it was appended in. Under g_mutex.
void touchName(std::vector<NameRec>& v, uint64_t key, const char* name, uint32_t frame) {
    for (NameRec& n : v) {
        if (n.key == key) {
            n.last = frame;
            return;
        }
    }
    if (v.size() >= kNameCap) {
        ++g_namesDropped;
        return;
    }
    NameRec n;
    n.key = key;
    n.last = frame;
    std::snprintf(n.name, sizeof(n.name), "%s", name ? name : "(none)");
    v.push_back(n);
}

void addIdent(uint64_t id, const char* kind) {   // under g_mutex
    for (IdentOp& op : g_idents) {
        if (op.id == id) {
            op.kind = kind;
            return;
        }
    }
    g_idents.push_back(IdentOp{id, kind});
}

// Up to kTopNames distinct names. carried false: appended in the open frame; true: appended in an earlier frame of the window only.
std::string namesOf(const std::vector<NameRec>& v, bool carried, uint32_t frame) {
    std::string out;
    std::vector<std::string> seen;
    for (const NameRec& r : v) {
        if (carried ? (r.last >= frame) : (r.last != frame)) continue;
        const std::string nm = r.name;
        if (std::find(seen.begin(), seen.end(), nm) != seen.end()) continue;
        if (seen.size() >= kTopNames) break;
        seen.push_back(nm);
        if (!out.empty()) out += ", ";
        out += nm;
    }
    return out.empty() ? std::string("none") : out;
}

void resetWindow() {   // under g_mutex, at the window's start
    g_rows.clear();
    g_rows.reserve(kRowCap);
    g_rowsDropped = 0;
    g_namesDropped = 0;
    g_windowFrames = 0;
    g_keysA.reset();
    g_keysB.reset();
    g_namesA.clear();
    g_namesB.clear();
    g_idents.clear();
}

void openFrame(uint32_t frame) {   // under g_mutex: the next window frame starts at this tick
    g_frameNo = frame;
    g_modeText = sealedTrialModeText();
    g_counts = dl::FrameCounts{};
    g_idents.clear();
    for (auto& d : g_famDraws) d.store(0, std::memory_order_relaxed);
    g_keysA.beginFrame();
    g_keysB.beginFrame();
}

// Closes the open window frame: the counts, the summary line, the family, carried and frame rows. Under g_mutex.
void closeFrame() {
    dl::FrameCounts& c = g_counts;
    c.frame = g_frameNo;
    c.window = g_windowFrames + 1;
    c.mode = g_modeText;
    c.engineLive = engineVelocityActive();
    c.builderPathBracket = builderProbeOnBracket();
    c.appendedA = g_keysA.distinctThisFrame();
    c.appendedB = g_keysB.distinctThisFrame();
    c.carried = g_keysA.carried() + g_keysB.carried();
    c.keysOverflow = g_keysA.overflow() + g_keysB.overflow();
    c.rebuilt = g_idents.size();
    uint64_t fam = 0;
    for (auto& d : g_famDraws) fam += d.load(std::memory_order_relaxed);
    c.familyDraws = fam;
    // Built v12: the early rig veto's decisions since the last frame's take (sealedTrialRigTake; its counts are the window's own, not reset here).
    uint64_t rig[8] = {};
    sealedTrialRigTake(rig);
    c.rigSeen = rig[0];
    c.rigVetoed = rig[1];
    c.rigMixed = rig[2];
    c.rigUnknown = rig[3];
    c.rigRefresh = rig[4];
    c.rigMatched = rig[5];
    c.rigMs = static_cast<double>(rig[7]) / 1.0e6;

    std::string names = "Top " + std::to_string(kTopNames) + " names. appended_A: " + namesOf(g_namesA, false, g_frameNo) +
                        "; appended_B: " + namesOf(g_namesB, false, g_frameNo) + "; carried (appended earlier in the window, not this frame): " +
                        namesOf(g_namesA, true, g_frameNo) + " | " + namesOf(g_namesB, true, g_frameNo) +
                        "; unattributed scene draws: no per-draw join (see the window's join line).";
    char line[1600];
    dl::formatFrameLine(line, sizeof(line), c, names.c_str());
    Log::get().note("%s", line);

    for (uint32_t f = 0; f < dl::kFamilySlots; ++f) {
        const uint64_t draws = g_famDraws[f].load(std::memory_order_relaxed);
        if (draws == 0) continue;
        const char* fname = f < static_cast<uint32_t>(engine_velocity_family::kFamilyCount) ? engine_velocity_family::kFamilies[f].name : "?";
        dl::CsvRow r;
        r.table = "family";
        r.frame = g_frameNo;
        r.kind = "family";
        r.family = fname;
        r.draws = static_cast<int64_t>(draws);
        r.origin = "unattributed";
        r.mode = g_modeText;
        addRow(r);
    }
    for (int side = 0; side < 2; ++side) {
        const std::vector<NameRec>& v = side == 0 ? g_namesA : g_namesB;
        for (const NameRec& n : v) {
            if (n.last >= g_frameNo) continue;
            dl::CsvRow r;
            r.table = "carried";
            r.frame = g_frameNo;
            r.kind = "-";
            r.key = n.key;
            r.name = n.name;
            r.origin = "carried";
            r.mode = g_modeText;
            addRow(r);
        }
    }
    dl::CsvRow fr;
    fr.table = "frame";
    fr.frame = g_frameNo;
    fr.appended = static_cast<int64_t>(c.appendedA + c.appendedB);
    fr.draws = static_cast<int64_t>(c.familyDraws);
    fr.origin = "unattributed";
    fr.mode = g_modeText;
    fr.rebuilt = c.rebuilt ? "yes" : "no";
    addRow(fr);
    ++g_windowFrames;
}

// Writes the window's CSV and says so. Under g_mutex.
void finishWindow() {
    SYSTEMTIME sysTime{};
    GetLocalTime(&sysTime);
    char stamp[24] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(sysTime.wYear),
                  static_cast<unsigned>(sysTime.wMonth), static_cast<unsigned>(sysTime.wDay), static_cast<unsigned>(sysTime.wHour),
                  static_cast<unsigned>(sysTime.wMinute), static_cast<unsigned>(sysTime.wSecond));
    char csvName[64] = {};
    std::snprintf(csvName, sizeof(csvName), "drawlist_%s.csv", stamp);
    std::wstring path = Config::get().logDir() + L"\\drawlist_";
    for (const char* c = stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        Log::get().note("drawlist probe: could not open edvr_logs\\%s; the frame lines above are complete.", csvName);
        return;
    }
    std::fprintf(f, "%s\n", dl::csvHeader());
    for (const std::string& row : g_rows) std::fprintf(f, "%s\n", row.c_str());
    std::fclose(f);
    Log::get().note("drawlist probe: window written: csv=%s, %zu rows (%llu dropped past %zu), %llu keys not named (cap %zu). %s",
                    csvName, g_rows.size(), static_cast<unsigned long long>(g_rowsDropped), kRowCap,
                    static_cast<unsigned long long>(g_namesDropped), kNameCap, dl::joinStatement());
}

}  // namespace

// The taps, defined below: the function table the engine's tap points reach once a window is armed.
void tapBuilder(uintptr_t rec, uintptr_t instance) noexcept;
void tapEmit(uintptr_t record, uintptr_t owner, int32_t before, int32_t after) noexcept;
void tapCopy(uintptr_t mapped, uint32_t count) noexcept;
void tapMerge(uintptr_t destination) noexcept;
void tapClear(uintptr_t dictionary) noexcept;
void tapFamily(int family) noexcept;
void tapProducerC(uint64_t items, uint32_t fault) noexcept;
const DrawlistTapFns kTapFns{&tapBuilder, &tapEmit, &tapCopy, &tapMerge, &tapClear, &tapFamily, &tapProducerC};

void drawlistRequest(const char* trigger, uint32_t frame) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase != Phase::kIdle) {
            Log::get().note("drawlist probe: refused (trigger %s): a window is already armed or collecting.", trigger);
            return;
        }
        g_phase = Phase::kArmed;
        g_wanted.store(true, std::memory_order_release);
        g_drawlistTaps.store(&kTapFns, std::memory_order_release);
        kinematicEvalSetProducerCObserver(&tapProducerC);   // path C (producer 1's bracket) counts through the probe's observer
    }
    storeHookRecompute();   // the builder observer attaches with the want (builder_probe.cpp)
    Log::get().note("drawlist probe: armed (trigger %s) at frame %u. Three consecutive frames are collected from the next frame tick and "
                    "written at the tick after the third. The taps are live where their functions run: path A on the builder bracket, "
                    "path B and the pool copy, merge and clear on engine motion's observers, the family draws on the substituted draw path. "
                    "A window with no 'collecting' line means the tick never came; 'engine motion NOT live' in a frame line means "
                    "those taps are silent for that reason.", trigger, frame);
}

void drawlistFrame(uint32_t frame) {
    if (!g_wanted.load(std::memory_order_acquire)) return;
    bool recompute = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase == Phase::kCollecting) {
            closeFrame();
            if (g_windowFrames >= dl::kFrames) {
                finishWindow();
                g_phase = Phase::kIdle;
                g_drawlistCollecting.store(false, std::memory_order_release);
                g_wanted.store(false, std::memory_order_release);
                recompute = true;
            } else {
                openFrame(frame);
            }
        } else if (g_phase == Phase::kArmed) {
            g_phase = Phase::kCollecting;
            resetWindow();
            openFrame(frame);
            g_drawlistCollecting.store(true, std::memory_order_release);
            Log::get().note("drawlist probe: collecting %u consecutive frames from frame %u (mode %s).", dl::kFrames, frame, g_modeText);
        }
    }
    if (recompute) storeHookRecompute();
}

bool drawlistWanted() noexcept { return g_wanted.load(std::memory_order_acquire); }

void tapBuilder(uintptr_t rec, uintptr_t instance) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    char name[48] = "(none)";
    sealedTrialRecordNameInto(rec, name, sizeof(name));   // the trial's lock, taken before ours
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase != Phase::kCollecting) return;
    ++g_counts.builderCalls;
    const bool fresh = g_keysA.noteAppended(instance);
    touchName(g_namesA, instance, name, g_frameNo);
    if (fresh) {
        dl::CsvRow r;
        r.table = "event";
        r.frame = g_frameNo;
        r.kind = "builder";
        r.record = rec;
        r.key = instance;
        r.name = name;
        r.origin = "appended_A";
        r.mode = g_modeText;
        addRow(r);
    }
}

void tapEmit(uintptr_t record, uintptr_t owner, int32_t before, int32_t after) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    uint64_t key = 0;
    if (!readU64(record + kRecKey, &key)) key = 0;
    char name[48] = "(none)";
    sealedTrialRecordNameInto(record, name, sizeof(name));
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase != Phase::kCollecting) return;
    ++g_counts.emitCalls;
    const int64_t n = static_cast<int64_t>(after) - static_cast<int64_t>(before);
    g_counts.emitItems += n;
    if (n <= 0) return;
    const bool fresh = g_keysB.noteAppended(key);
    touchName(g_namesB, key, name, g_frameNo);
    if (fresh) {
        dl::CsvRow r;
        r.table = "event";
        r.frame = g_frameNo;
        r.kind = "emit";
        r.record = record;
        r.owner = owner;
        r.key = key;
        r.before = before;
        r.after = after;
        r.appended = n;
        r.name = name;
        r.origin = "appended_B";
        r.mode = g_modeText;
        addRow(r);
    }
}

void tapCopy(uintptr_t mapped, uint32_t count) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase != Phase::kCollecting) return;
    ++g_counts.copyCalls;
    g_counts.copyRecords += count;
    addIdent(mapped, "copy");
    dl::CsvRow r;
    r.table = "collection";
    r.frame = g_frameNo;
    r.kind = "copy";
    r.owner = mapped;
    r.appended = count;
    r.origin = "-";
    r.mode = g_modeText;
    r.rebuilt = "yes";
    addRow(r);
}

void tapMerge(uintptr_t destination) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase != Phase::kCollecting) return;
    ++g_counts.mergeCalls;
    addIdent(destination, "merge");
    dl::CsvRow r;
    r.table = "collection";
    r.frame = g_frameNo;
    r.kind = "merge";
    r.owner = destination;
    r.origin = "-";
    r.mode = g_modeText;
    r.rebuilt = "yes";
    addRow(r);
}

void tapClear(uintptr_t dictionary) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase != Phase::kCollecting) return;
    ++g_counts.clearCalls;
    addIdent(dictionary, "clear");
    dl::CsvRow r;
    r.table = "collection";
    r.frame = g_frameNo;
    r.kind = "clear";
    r.owner = dictionary;
    r.origin = "-";
    r.mode = g_modeText;
    r.rebuilt = "yes";
    addRow(r);
}

void tapFamily(int family) noexcept {
    if (family < 0 || family >= static_cast<int>(dl::kFamilySlots)) return;
    g_famDraws[family].fetch_add(1, std::memory_order_relaxed);
}

// Built v11: path C, from the producer-1 bracket after its forward: the owner's count delta is the items the call appended (the bracket's own
// reading). Under g_mutex, like the other producer taps.
void tapProducerC(uint64_t items, uint32_t fault) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase != Phase::kCollecting) return;
    ++g_counts.cCalls;
    if (fault) {
        ++g_counts.cFaults;
        return;
    }
    g_counts.cItems += static_cast<int64_t>(items);
}

}  // namespace edvr
