// The asset census (asset_census.h). The chain reader, the grouping and the CSV row are in asset_census_core.h; this file holds the
// press, the one-frame collection in the store's observer, the view and the readers, and the write at the next Present.
#include "asset_census.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "asset_census_core.h"
#include "journal_watch.h"
#include "state_dump_probe.h"
#include "store_hook.h"
#include "view_source.h"

namespace edvr {
namespace {

using census::Observation;

enum Phase : int { kIdle = 0, kArmed = 1, kCollecting = 2 };
constexpr size_t kObservedCap = 32768;   // records kept for one frame; more are counted as dropped
constexpr int kViewTries = 64;           // resolutions while the view has no pose (the temporal pass chooses its rows part-way through a frame)
constexpr uintptr_t kView0 = 0x40;       // the render context's view 0 (the views are 0x6A0 apart)
constexpr uintptr_t kViewBit = 0x570;    // view+0x570: the view's bit in a record's mask
constexpr size_t kNameGroups = 400;      // groups that get a name scan at the write (the largest first)
// The docking object's fields the hatch line reads (docs Hangar door state; the same offsets as state_dump_probe.cpp's hatch probe).
constexpr uintptr_t kObjState = 0x3C8;   // this+0x3C8: the state byte (read as a dword)
constexpr uintptr_t kOwnerCount = 0x10;  // owner+0x10: the occupancy count
constexpr uintptr_t kOwnerB2C = 0x2C;    // owner+0x2C
constexpr uintptr_t kOwnerB2F = 0x2F;    // owner+0x2F

std::atomic<int> g_phase{kIdle};
// Guards the collection. The observers append under it; the write takes the observations under it.
std::mutex g_mutex;
std::vector<Observation> g_obs;
census::ChainCache g_cache;
size_t g_dropped = 0;
view::Resolved g_view;           // the frame's view: resolved at the first observation, and again while it has no pose
bool g_viewTried = false;
int g_viewAttempts = 0;
uint64_t g_mainBit = 0;          // view 0's bit in the masks (0 until a render context is read)
std::string g_trigger;           // render thread
uint32_t g_frame = 0;            // the frame collected (render thread)
StateReaderSnapshot g_readersAtStart;   // render thread
census::Image g_image;           // the module's sections, read once at the press (the vtable check uses them)

// The engine's memory, read under SEH. A function of its own, with no C++ objects to unwind.
__declspec(noinline) bool readSeh(void* ctx, uintptr_t address, void* dst, size_t n) noexcept {
    (void)ctx;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(address), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

census::Reader engineReader() noexcept {
    census::Reader r;
    r.read = &readSeh;
    r.ctx = nullptr;
    return r;
}

void loadImage() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    census::Image img;
    if (census::parseImage(engineReader(), base, &img)) g_image = img;
    else g_image = census::Image{};
}

// The hatch state: the four readers, the remembered docking objects (state_dump_probe.cpp) and the journal's Status words, read now. A reader's code is -1 when it was not
// called in the frame collected (the calls did not move).
// Appends one formatted piece (each piece is short; the buffer is local to the call).
void addf(std::string& out, const char* fmt, ...) {
    char piece[320];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(piece, sizeof(piece), fmt, args);
    va_end(args);
    out += piece;
}

std::string hatchStateLine(const StateReaderSnapshot& was, const StateReaderSnapshot& now) {
    std::string text;
    addf(text, "readers installed=%d", now.installed ? 1 : 0);
    for (int i = 0; i < 4; ++i) {
        const uint32_t delta = now.calls[i] >= was.calls[i] ? now.calls[i] - was.calls[i] : now.calls[i];
        const int code = delta > 0 ? static_cast<int>(now.codes[i]) : -1;
        addf(text, " r%d(calls=%u code=%d)", i, delta, code);
    }
    // Built v8: the objects the readers were called with (remembered, not the AAB8 suffix pin), four at most so the line fits the log.
    addf(text, " remembered=%u", now.rememberedCount);
    for (uint32_t i = 0; i < now.rememberedCount && i < 4 && i < 8; ++i) {
        const uintptr_t obj = now.remembered[i];
        if (!obj) continue;
        uint32_t state = 0;
        const bool okState = engineReader().u32(obj + kObjState, &state);
        uint64_t owner = 0;
        uint32_t count = 0;
        uint8_t b2C = 0, b2F = 0;
        const bool okOwner = obj >= 8 && engineReader().u64(obj - 8, &owner) && owner != 0 &&
                             engineReader().u32(static_cast<uintptr_t>(owner) + kOwnerCount, &count) &&
                             engineReader().bytes(static_cast<uintptr_t>(owner) + kOwnerB2C, &b2C, 1) &&
                             engineReader().bytes(static_cast<uintptr_t>(owner) + kOwnerB2F, &b2F, 1);
        addf(text, " obj%u=0x%llX state3C8=%s%u owner=%s0x%llX count=%s%u 2C=%u 2F=%u", i,
             static_cast<unsigned long long>(obj), okState ? "" : "?", okState ? state : 0u, okOwner ? "" : "?",
             static_cast<unsigned long long>(owner), okOwner ? "" : "?", okOwner ? count : 0u, okOwner ? b2C : 0u,
             okOwner ? b2F : 0u);
    }
    if (now.rememberedCount > 4) addf(text, " (+%u more)", now.rememberedCount - 4);
    const JournalRawStatus raw = journalRawStatus();
    bool locDocked = false;
    const bool locKnown = journalLocationDocked(&locDocked);
    addf(text, " status flags=%s0x%X flags2=%s0x%X status_docked=%d loc_docked=%d", raw.flagsKnown ? "" : "unknown ", raw.flags,
         raw.flags2Known ? "" : "unknown ", raw.flags2, raw.flagsKnown ? static_cast<int>(raw.flags & 1u) : -1,
         locKnown ? (locDocked ? 1 : 0) : -1);
    return text;
}

// The names of the groups' objects, scanned once per object address.
void appendNames(const census::Reader& r, const Observation& o, std::unordered_map<uintptr_t, std::vector<std::string>>& cache,
                 std::vector<std::string>& out) {
    struct Role {
        uintptr_t object;
        const char* label;
    };
    const Role roles[] = {{o.node, "node"}, {o.source, "source"}, {o.component, "component"}, {o.model, "model"}};
    for (const Role& role : roles) {
        if (out.size() >= census::kMaxNames || !role.object) continue;
        auto it = cache.find(role.object);
        if (it == cache.end()) {
            std::vector<std::string> hits;
            census::scanNames(r, role.object, role.label, census::kMaxNames, &hits);
            it = cache.emplace(role.object, hits).first;
        }
        for (const std::string& hit : it->second) {
            if (out.size() >= census::kMaxNames) break;
            out.push_back(hit);
        }
    }
}

// Writes the CSV for the frame collected, and its summary and hatch-state lines.
void writeCensus() {
    std::vector<Observation> obs;
    size_t dropped = 0;
    view::Resolved resolved;
    uint64_t mainBit = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        obs.swap(g_obs);
        dropped = g_dropped;
        g_dropped = 0;
        resolved = g_view;
        mainBit = g_mainBit;
        g_cache.clear();
    }
    float cam[3] = {};
    bool fromRows = false;
    const bool camKnown = view::sphereCentre(resolved, cam, &fromRows);
    const census::Reader r = engineReader();

    size_t ok = 0, noSource = 0, refused = 0, blockAgree = 0, hashAgree = 0, recordVtable = 0;
    for (const Observation& o : obs) {
        if (o.status == census::Status::kOk) ++ok;
        else if (o.status == census::Status::kNoSource) ++noSource;
        else ++refused;
        if (o.blockAgrees) ++blockAgree;
        if (o.hashAgrees) ++hashAgree;
        if (o.recordVtable) ++recordVtable;
    }
    const std::vector<census::Group> groups = census::groupObservations(obs);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[24] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    char csvName[64] = {};
    std::snprintf(csvName, sizeof(csvName), "asset_census_%s.csv", stamp);
    std::wstring path = Config::get().logDir() + L"\\asset_census_";
    for (const char* c = stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";

    StateReaderSnapshot nowState;
    stateDumpCensusSnapshot(&nowState);
    const std::string state = hatchStateLine(g_readersAtStart, nowState);

    char camText[96];
    if (camKnown) std::snprintf(camText, sizeof(camText), "camera=(%.2f,%.2f,%.2f)", static_cast<double>(cam[0]),
                                static_cast<double>(cam[1]), static_cast<double>(cam[2]));
    else std::snprintf(camText, sizeof(camText), "camera=unknown");
    char summary[640];
    std::snprintf(summary, sizeof(summary),
                  "asset census: frame %u view source = %s; centre = %s; %s; observations=%zu (dropped=%zu); groups=%zu "
                  "(identified=%zu, no-source=%zu, refused=%zu); block agrees %zu, hash agrees %zu of %zu identified; "
                  "rec+0 vtable-like %zu; main view bit 0x%llX%s; image %s",
                  g_frame, (resolved.poseOk || resolved.originOk) ? view::sourceName(resolved.source) : "unresolved",
                  camKnown ? (fromRows ? "rows" : "origin") : "unknown", camText,
                  obs.size(), dropped, groups.size(), ok, noSource, refused, blockAgree, hashAgree, ok, recordVtable,
                  static_cast<unsigned long long>(mainBit), mainBit ? "" : " (unknown: the main view bit is not read)",
                  g_image.ready() ? "read" : "NOT read (vtable checks refuse)");

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        std::fprintf(f, "# %s csv=%s\n", summary, csvName);
        std::fprintf(f, "# hatch state: %s\n", state.c_str());
        std::fprintf(f, "%s\n", census::csvHeader());
        std::unordered_map<uintptr_t, std::vector<std::string>> nameCache;
        char row[2048];
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const census::Group& g = groups[gi];
            const Observation& first = obs[g.members.front()];
            std::vector<std::string> names;
            if (g.status == census::Status::kOk && gi < kNameGroups) appendNames(r, first, nameCache, names);
            std::string joined;
            for (size_t k = 0; k < names.size(); ++k) joined += (k ? " | " : "") + names[k];
            const census::Stats s = census::computeStats(obs, g, camKnown ? cam : nullptr, mainBit);
            const int n = census::formatRow(row, sizeof(row), gi, g, s, joined, first, g_image);
            if (n > 0) std::fprintf(f, "%s\n", row);
        }
        std::fclose(f);
    } else {
        Log::get().note("asset census: could not open edvr_logs\\%s; the lines below are complete.", csvName);
    }
    Log::get().note("%s csv=%s", summary, csvName);
    Log::get().note("asset census: hatch state: %s", state.c_str());
}

}  // namespace

void assetCensusRequest(const char* trigger, uint32_t frame) {
    (void)frame;
    int expected = kIdle;
    if (!g_phase.compare_exchange_strong(expected, kArmed, std::memory_order_acq_rel)) {
        Log::get().note("asset census: refused (trigger %s): a census is already armed or collecting.", trigger);
        return;
    }
    loadImage();
    const std::string detours = stateDumpCensusArm(true);
    storeHookRecompute();
    if (std::strcmp(storeHookStatus(), "installed") != 0) {
        g_phase.store(kIdle, std::memory_order_release);
        stateDumpCensusArm(false);
        storeHookRecompute();
        Log::get().note("asset census: refused (trigger %s): the admission store is not detoured (%s).", trigger,
                        storeHookStatus());
        return;
    }
    g_trigger = trigger ? trigger : "";
    Log::get().note("asset census: armed (trigger %s): the next frame's records are collected and written at the Present after it. "
                    "Readers: %s. Image sections: %s.",
                    g_trigger.c_str(), detours.empty() ? "not requested" : detours.c_str(),
                    g_image.ready() ? "read" : "NOT read (vtable checks will refuse)");
}

void assetCensusFrame(uint32_t frame) {
    const int phase = g_phase.load(std::memory_order_acquire);
    if (phase == kArmed) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_obs.clear();
            g_obs.reserve(kObservedCap);
            g_cache.clear();
            g_dropped = 0;
            g_view = view::Resolved{};
            g_viewTried = false;
            g_viewAttempts = 0;
            g_mainBit = 0;
        }
        stateDumpCensusSnapshot(&g_readersAtStart);
        g_frame = frame + 1;
        g_phase.store(kCollecting, std::memory_order_release);
        Log::get().note("asset census: collecting frame %u (trigger %s).", g_frame, g_trigger.c_str());
    } else if (phase == kCollecting) {
        g_phase.store(kIdle, std::memory_order_release);
        writeCensus();
        stateDumpCensusArm(false);
        storeHookRecompute();
    }
}

bool assetCensusWanted() noexcept { return g_phase.load(std::memory_order_acquire) != kIdle; }

bool assetCensusCollecting() noexcept { return g_phase.load(std::memory_order_acquire) == kCollecting; }

void assetCensusAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    if (g_phase.load(std::memory_order_acquire) != kCollecting) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase.load(std::memory_order_acquire) != kCollecting) return;
    if (g_obs.size() >= kObservedCap) {
        ++g_dropped;
        return;
    }
    const census::Reader r = engineReader();
    uint64_t ctx64 = 0;
    const uintptr_t ctx = r.u64(bucket, &ctx64) ? static_cast<uintptr_t>(ctx64) : 0;
    if (ctx) {
        if (!g_mainBit) {
            uint64_t bit = 0;
            if (r.u64(ctx + kView0 + kViewBit, &bit)) g_mainBit = bit;
        }
        if (!g_viewTried || (!g_view.poseOk && g_viewAttempts < kViewTries)) {
            ++g_viewAttempts;
            view::resolveView(ctx, &g_view);
            g_viewTried = true;
        }
    }
    g_obs.push_back(census::observeRecord(r, g_image, bucket, record, &g_cache));
}

}  // namespace edvr
