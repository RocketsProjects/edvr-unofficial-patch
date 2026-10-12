// The hatch geometry probe's engine side (hatch_geometry.h; TEMPORARY). The pure half (the name rules, the tracker, the bounded sample box, the range
// filter and the line text) is in hatch_geometry_core.h. The store's observer keeps the named records of a collected sample under the box's lock;
// the frame tick swaps the box out whole, filters it to the camera's 400 m, and processes it after the lock is released. Nothing is held across a
// call into the game.
#include "hatch_geometry.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "../common/log.h"
#include "hatch_geometry_core.h"
#include "journal_watch.h"
#include "sealed_trial.h"
#include "view_source.h"

namespace edvr {
namespace {

namespace hg = edvr::hatchgeo;

constexpr uintptr_t kRecCentre = 0x240;   // float3: the sphere centre (PROVEN, sealed_trial_core.h)
constexpr uintptr_t kRecPose = 0x190;     // 12 floats: the pose (layout NOT proven; logged raw)

std::atomic<bool> g_on{false};
std::atomic<bool> g_collecting{false};
hg::SampleBox<std::mutex> g_box;          // the collected sample (lock inside)
hg::Tracker g_tracker;                    // render thread only
uint32_t g_sampleFrame = 0;               // the frame the armed sample belongs to (render thread)
bool g_countPending = false;              // the next sample logs its names once (render thread)
uint32_t g_lines = 0;                     // lines written this session (render thread)
uint32_t g_suppressed = 0;                // lines past the cap this session (render thread)
bool g_capLogged = false;
bool g_noCameraLogged = false;

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

// One event, within the session's line cap. The camera and the nearest Hanger*/Hangar* record within 400 m of the event's centre are named.
void writeEvent(const hg::Event& e, const hg::Sample* samples, size_t n, const hg::LineContext& ctx, uint32_t frame) {
    if (g_lines >= hg::kLineCap) {
        ++g_suppressed;
        if (!g_capLogged) {
            g_capLogged = true;
            Log::get().note("hatch geometry: line cap (%u) reached at sampled frame %u; further lines are suppressed and counted "
                            "(hatch probe off prints the count).", hg::kLineCap, frame);
        }
        return;
    }
    const hg::Nearest hangar = hg::nearestHanger(e.centre, samples, n, e.addr, hg::kRangeM);
    char line[1400];
    hg::formatEvent(line, sizeof(line), e, ctx, frame, hangar);
    Log::get().note("%s", line);
    ++g_lines;
}

// Processes one collected sample (render thread): the records are kept within 400 m of the camera of the sampled frame; the first sample after
// a toggle-on names its records; the tracker reports the events, at the clock's time.
void processSample(std::vector<hg::Sample>& samples, uint64_t drops, uintptr_t ctx, uint32_t frame) {
    float cam[3] = {};
    bool camOk = false;
    if (ctx) {
        view::Resolved v;
        view::resolveView(ctx, &v);
        camOk = view::sphereCentre(v, cam);
    }
    if (!camOk) {
        if (!g_noCameraLogged) {
            g_noCameraLogged = true;
            Log::get().note("hatch geometry: sampled frame %u has no camera (the view is unread); the sample is skipped, and the 400 m "
                            "filter cannot run.", frame);
        }
        return;
    }
    const size_t n = hg::keepWithin(samples.data(), samples.size(), cam, hg::kRangeM);
    samples.resize(n);
    if (g_countPending) {
        g_countPending = false;
        char names[1000];
        hg::formatNameCounts(names, sizeof(names), samples.data(), samples.size());
        char line[1200];
        std::snprintf(line, sizeof(line), "hatch geometry: the first sampled frame (%u) saw %zu records within %.0f m of the camera "
                      "(%llu past the sample cap of %zu were dropped): %s", frame, samples.size(), static_cast<double>(hg::kRangeM),
                      static_cast<unsigned long long>(drops), hg::kSampleCap, names);
        Log::get().note("%s", line);
    }
    if (drops && g_lines < hg::kLineCap) {
        Log::get().note("hatch geometry: sampled frame %u: %llu records past the sample cap of %zu were not kept.", frame,
                        static_cast<unsigned long long>(drops), hg::kSampleCap);
        ++g_lines;
    }
    const uint64_t now = static_cast<uint64_t>(GetTickCount64());
    hg::LineContext lc;
    lc.hasCamera = true;
    for (int k = 0; k < 3; ++k) lc.camera[k] = cam[k];
    lc.ms = now;
    const JournalRawStatus raw = journalRawStatus();
    lc.statusKnown = raw.flagsKnown;
    lc.flags = static_cast<uint32_t>(raw.flags);
    lc.flags2 = static_cast<uint32_t>(raw.flags2);
    std::vector<hg::Event> events;
    events.reserve(samples.size() + 8);
    g_tracker.apply(samples.data(), samples.size(), now, &events);
    for (const hg::Event& e : events) writeEvent(e, samples.data(), samples.size(), lc, frame);
}

}  // namespace

void hatchGeometryOn(uint32_t frame) {
    g_tracker.clear();
    g_lines = 0;
    g_suppressed = 0;
    g_capLogged = false;
    g_noCameraLogged = false;
    g_countPending = true;
    {
        hg::SampleStorage prepared;
        prepared.reserve(hg::kSampleCap);
        (void)g_box.exchange(std::move(prepared));
    }
    g_collecting.store(false, std::memory_order_release);
    g_on.store(true, std::memory_order_release);
    Log::get().note("hatch geometry: on at frame %u: one frame in %u is sampled; a sample keeps the records named BlastShield, LandingPad, "
                    "Lift, Hanger or Hangar (cap %zu) within %.0f m of the camera; a moving record reports at most one line a second and a "
                    "settled line after 2 s still; %u lines a session. The pose at rec+0x190 is logged raw (its layout is not proven); the "
                    "centre is rec+0x240.", frame, hg::kSampleEvery, hg::kSampleCap, static_cast<double>(hg::kRangeM), hg::kLineCap);
}

void hatchGeometryOff(uint32_t frame) {
    g_on.store(false, std::memory_order_release);
    g_collecting.store(false, std::memory_order_release);
    (void)g_box.exchange(hg::SampleStorage{});
    Log::get().note("hatch geometry: off at frame %u: lines written %u, suppressed %u (cap %u); records present now %zu; refused "
                    "(track cap %zu) %u.", frame, g_lines, g_suppressed, hg::kLineCap, g_tracker.present(), hg::kTrackCap,
                    g_tracker.refused());
}

void hatchGeometryFrame(uint32_t frame) {
    if (g_collecting.load(std::memory_order_acquire)) {
        g_collecting.store(false, std::memory_order_release);
        hg::SampleStorage taken = g_box.exchange(hg::SampleStorage{});
        processSample(taken.items, taken.drops, taken.ctx, g_sampleFrame);
    }
    if (g_on.load(std::memory_order_acquire) && (frame + 1) % hg::kSampleEvery == 0) {
        hg::SampleStorage next;
        next.reserve(hg::kSampleCap);   // the allocation is here, before the lock
        (void)g_box.exchange(std::move(next));
        g_sampleFrame = frame + 1;
        g_collecting.store(true, std::memory_order_release);
    }
}

bool hatchGeometryWanted() noexcept { return g_on.load(std::memory_order_acquire); }

bool hatchGeometryCollecting() noexcept { return g_collecting.load(std::memory_order_acquire); }

void hatchGeometryAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    if (!record || !g_collecting.load(std::memory_order_acquire)) return;
    hg::Sample s;
    s.addr = record;
    if (!sealedTrialRecordNameInto(record, s.name, sizeof(s.name)) || !hg::isGeometryName(s.name)) return;
    // Built v8.4: the world centre (the builder's, from the pose; rec+0x240 only as the counted fallback). 2 is "none".
    bool rawZero = false;
    if (sealedTrialRecordCentre(record, s.centre, &rawZero) == 2) return;
    s.poseOk = readMem(s.pose, record + kRecPose, sizeof(s.pose));
    uintptr_t ctx = 0;
    if (bucket) readVal(&ctx, bucket);
    g_box.add(s, ctx);
}

}  // namespace edvr
