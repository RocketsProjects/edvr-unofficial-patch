// The hatch geometry probe's engine side (hatch_geometry.h; TEMPORARY). The pure half (the name rules, the pad frame, the tracker, the lower-cycle watch
// and the line text) is in hatch_geometry_core.h. The store's observer keeps the named records and every pad of a collected sample under the box's
// lock; the frame tick swaps the box out whole and processes it after the lock is released. Nothing is held across a call into the game.
// Built v8.2 and v8.3; v13.8 expresses the records in the own pad's frame and watches the lower cycle (docs\concourse-culling-2026-10-11.md).
#include "hatch_geometry.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
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

std::atomic<bool> g_on{false};
std::atomic<bool> g_collecting{false};
hg::SampleBox<std::mutex> g_box;          // the collected sample (lock inside)
hg::Tracker g_tracker;                    // render thread only
hg::BottomWatch g_watch;                  // render thread only
uint32_t g_sampleFrame = 0;               // the frame the armed sample belongs to (render thread)
bool g_countPending = false;              // the next sample logs its names once (render thread)
uint32_t g_lines = 0;                     // record lines written this session (render thread)
uint32_t g_suppressed = 0;                // record lines past the cap this session (render thread)
uint32_t g_padLines = 0;                  // pad lines written this session (render thread)
uint32_t g_padSuppressed = 0;             // pad lines past their cap this session (render thread)
bool g_capLogged = false;
bool g_padCapLogged = false;
bool g_noCameraLogged = false;
bool g_keySet = false;                    // g_frameKey is valid (render thread)
uintptr_t g_frameKey = 0;                 // the own pad the samples are expressed in; 0 is the world (render thread)
bool g_firstHSet = false;                 // the first shell reading of this frame key (render thread)
float g_firstH = 0.0f;
uint64_t g_lastPadLineMs = 0;

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

// One record line, within the session's cap. False (and counted) past the cap.
bool takeRecordLine(uint32_t frame) {
    if (g_lines < hg::kLineCap) {
        ++g_lines;
        return true;
    }
    ++g_suppressed;
    if (!g_capLogged) {
        g_capLogged = true;
        Log::get().note("hatch geometry: record line cap (%u) reached at sampled frame %u; further record lines are suppressed and counted "
                        "(hatch probe off prints the count).", hg::kLineCap, frame);
    }
    return false;
}

void writeEvent(const hg::Event& e, const hg::LineContext& ctx, uint32_t frame) {
    if (!takeRecordLine(frame)) return;
    char line[1400];
    hg::formatEvent(line, sizeof(line), e, ctx, frame);
    Log::get().note("%s", line);
}

// One pad line, within its own cap (the pad line is the time base for the record lines).
void writePadLine(const hg::PadLine& p, uint32_t frame) {
    if (g_padLines >= hg::kPadLineCap) {
        ++g_padSuppressed;
        if (!g_padCapLogged) {
            g_padCapLogged = true;
            Log::get().note("hatch pad: line cap (%u) reached at sampled frame %u; further pad lines are suppressed and counted.", hg::kPadLineCap,
                            frame);
        }
        return;
    }
    ++g_padLines;
    char line[600];
    hg::formatPadLine(line, sizeof(line), p, frame);
    Log::get().note("%s", line);
}

// Writes the lower cycle's summary when one is ready: its header, then one line for each record that moved after the settle. Not counted against
// the record cap (one per lower cycle).
void flushSummary(uint32_t frame) {
    hg::Summary m;
    if (!g_watch.takeSummary(&m)) return;
    char line[1000];
    hg::formatSummary(line, sizeof(line), m, frame);
    Log::get().note("%s", line);
    for (size_t i = 0; i < m.movedN; ++i) {
        hg::formatMoved(line, sizeof(line), m.moved[i], m, frame);
        Log::get().note("%s", line);
    }
    if (m.movedCount > m.movedN)
        Log::get().note("hatch geometry: %u records moved after the settle; the first %zu are listed.", m.movedCount, m.movedN);
}

// Processes one collected sample (render thread). The own pad is the trial's (held cell, else the named pick) when the trial is on, else the pad the
// camera is on (the trial's pick rule); the records are expressed in its frame; the records kept are those on the shaft within 80 m (with a shaft)
// or within 300 m of the camera (without one), and the own pad. The shell is the nearest Hanger*/Hangar* record within 150 m of the pad.
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
            Log::get().note("hatch geometry: sampled frame %u has no camera (the view is unread); the sample is skipped, and the 300 m filter cannot "
                            "run.", frame);
        }
        return;
    }
    const uint64_t now = static_cast<uint64_t>(GetTickCount64());

    // The own pad and the shaft (world coordinates, from the trial; the trial's cell is stale while the trial is off, so the accessor says so).
    uintptr_t trialPad = 0;
    float capA[3] = {}, capB[3] = {};
    bool held = false;
    const bool trialShaft = sealedTrialShaft(&trialPad, capA, capB, &held);
    uintptr_t ownAddr = trialPad ? trialPad : hg::pickOwnPad(samples.data(), samples.size(), cam);
    hg::Frame fr;
    float padAx[3][3] = {};
    uint8_t padCs = 4, padAs = 4;
    float padRadius = 0.0f;
    if (ownAddr) {
        if (sealedTrialRecordWorld(ownAddr, fr.origin, &padRadius, padAx, &padCs, &padAs) && padAs != 4) {
            fr.padFrame = true;
            std::memcpy(fr.axes, padAx, sizeof(fr.axes));
        } else {
            ownAddr = 0;   // the pad's pose is unreadable this frame: the world is the frame
        }
    }
    char frameName[hg::kNameLen] = "";
    if (ownAddr && !sealedTrialRecordNameInto(ownAddr, frameName, sizeof(frameName))) std::snprintf(frameName, sizeof(frameName), "unnamed");

    // The shaft, unit, in the frame. Without a pad frame there is no origin for the shaft line, so it is not used.
    float shaft[3] = {0.0f, 0.0f, 1.0f};
    bool hasShaft = false;
    if (trialShaft && fr.padFrame) {
        const float d[3] = {capB[0] - capA[0], capB[1] - capA[1], capB[2] - capA[2]};
        const float len = hg::norm3(d);
        if (len > 1e-3f) {
            const float dn[3] = {d[0] / len, d[1] / len, d[2] / len};
            float sf[3] = {};
            hg::dirToFrame(fr, dn, sf);
            const float sl = hg::norm3(sf);
            if (sl > 1e-3f) {
                for (int k = 0; k < 3; ++k) shaft[k] = sf[k] / sl;
                hasShaft = true;
            }
        }
    }

    // The shell: the nearest Hanger*/Hangar* sample to the pad within 150 m, measured in world coordinates; its height above the shell is along the
    // pad's up axis (the frame's z). Read from this sample's world values, so a frame shift moves pad and shell alike and cancels.
    bool shellOk = false;
    float relH = 0.0f;
    char shellName[hg::kNameLen] = "";
    float shellDistM = 0.0f;
    if (fr.padFrame) {
        const int si = hg::nearestHangerIndex(fr.origin, samples.data(), samples.size(), ownAddr, hg::kShellM);
        if (si >= 0) {
            const hg::Sample& sh = samples[static_cast<size_t>(si)];
            relH = sealed::shellRelative(fr.origin, sh.world, padAx[2]);
            shellOk = std::isfinite(relH);
            std::snprintf(shellName, sizeof(shellName), "%s", sh.name);
            shellDistM = hg::distance3(fr.origin, sh.world);
        }
    }

    // Express every sample in the frame, and keep the ones the probe follows: the own pad, and the records on the shaft (or within 300 m of the
    // camera with no shaft). The other pads are kept only for the own-pad pick above, so they are dropped here.
    size_t kept = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        hg::Sample& s = samples[i];
        hg::localize(fr, shaft, &s);
        const bool own = ownAddr != 0 && s.addr == ownAddr;
        bool keep = own;
        if (!keep && !s.isPad) keep = hasShaft ? s.lateral <= hg::kLateralM : hg::distance3(s.world, cam) <= hg::kRangeM;
        if (keep) {
            if (kept != i) samples[kept] = s;
            ++kept;
        }
    }
    samples.resize(kept);

    if (g_countPending) {
        g_countPending = false;
        char names[1000];
        hg::formatNameCounts(names, sizeof(names), samples.data(), samples.size());
        char line[1200];
        std::snprintf(line, sizeof(line), "hatch geometry: the first sampled frame (%u) kept %zu records (the own pad, and the records on the shaft or "
                      "within %.0f m of the camera; %llu past the sample cap of %zu were dropped): %s", frame, samples.size(),
                      static_cast<double>(hg::kRangeM), static_cast<unsigned long long>(drops), hg::kSampleCap, names);
        Log::get().note("%s", line);
    }
    if (drops && g_lines < hg::kLineCap) {
        Log::get().note("hatch geometry: sampled frame %u: %llu records past the sample cap of %zu were not kept.", frame,
                        static_cast<unsigned long long>(drops), hg::kSampleCap);
        ++g_lines;
    }

    // A new own pad (or the world after the pad): the records are followed afresh, and the lower cycle restarts from it.
    const uintptr_t key = fr.padFrame ? ownAddr : 0;
    if (!g_keySet || key != g_frameKey) {
        if (g_keySet && takeRecordLine(frame)) {
            Log::get().note("hatch geometry: the frame changed at sampled frame %u (own pad 0x%llX -> 0x%llX): the records are followed afresh in the new "
                            "frame; an open lower cycle is closed.", frame, static_cast<unsigned long long>(g_frameKey),
                            static_cast<unsigned long long>(key));
        }
        g_tracker.clear();
        g_watch.reset(now, "the frame changed");
        flushSummary(frame);
        g_frameKey = key;
        g_keySet = true;
        g_firstHSet = false;
    }

    hg::LineContext lc;
    lc.hasCamera = true;
    for (int k = 0; k < 3; ++k) lc.camera[k] = cam[k];
    lc.ms = now;
    const JournalRawStatus raw = journalRawStatus();
    lc.statusKnown = raw.flagsKnown;
    lc.flags = static_cast<uint32_t>(raw.flags);
    lc.flags2 = static_cast<uint32_t>(raw.flags2);
    lc.padFrame = fr.padFrame;
    std::snprintf(lc.frameName, sizeof(lc.frameName), "%s", frameName);
    lc.frameAddr = ownAddr;
    lc.ownAddr = ownAddr;
    lc.hasShaft = hasShaft;
    lc.held = held;
    std::vector<hg::Event> events;
    events.reserve(samples.size() + 8);
    g_tracker.apply(samples.data(), samples.size(), now, &events);
    for (const hg::Event& e : events) writeEvent(e, lc, frame);

    // The lower cycle. The first shell reading of this frame key is the baseline for the shell change on the pad line.
    if (shellOk && !g_firstHSet) {
        g_firstHSet = true;
        g_firstH = relH;
    }
    const hg::PadNotice notice = g_watch.update(now, shellOk, relH, samples.data(), samples.size());
    switch (notice) {
        case hg::PadNotice::kCycleStart:
            Log::get().note("hatch pad: frame %u t=%llu ms: the pad started to move along the shaft (height above the shell %.3f m): a cycle starts.",
                            frame, static_cast<unsigned long long>(now), static_cast<double>(relH));
            break;
        case hg::PadNotice::kBottomSettled:
            Log::get().note("hatch pad: frame %u t=%llu ms: the pad settled at the bottom (height above the shell %.3f m): the records that move from "
                            "now are in the lower-cycle summary.", frame, static_cast<unsigned long long>(now), static_cast<double>(relH));
            break;
        case hg::PadNotice::kSettledNotLower:
            Log::get().note("hatch pad: frame %u t=%llu ms: the pad settled (height above the shell %.3f m), not a lower: no summary.", frame,
                            static_cast<unsigned long long>(now), static_cast<double>(relH));
            break;
        default:
            break;
    }
    flushSummary(frame);

    // The pad line: while the pad moves, every sampled frame; while it is still, every kPadHeartbeatMs; and on a notice.
    const bool moving = g_watch.padMoving();
    if (fr.padFrame && (moving || notice != hg::PadNotice::kNone || now - g_lastPadLineMs >= hg::kPadHeartbeatMs)) {
        g_lastPadLineMs = now;
        hg::PadLine p;
        p.ms = now;
        p.padFrame = true;
        std::snprintf(p.padName, sizeof(p.padName), "%s", frameName);
        p.padAddr = ownAddr;
        p.shellOk = shellOk;
        std::snprintf(p.shellName, sizeof(p.shellName), "%s", shellName);
        p.shellDistM = shellDistM;
        p.relH = relH;
        p.firstH = g_firstH;
        p.firstSet = g_firstHSet;
        p.phase = g_watch.phase();
        p.moving = moving;
        p.hasShaft = hasShaft;
        p.held = held;
        writePadLine(p, frame);
    } else if (!fr.padFrame && now - g_lastPadLineMs >= hg::kPadHeartbeatMs) {
        g_lastPadLineMs = now;
        hg::PadLine p;   // no own pad: the heartbeat says so, so a flight without a pad is not mistaken for one with no motion
        p.ms = now;
        p.padFrame = false;
        p.hasShaft = hasShaft;
        p.held = held;
        writePadLine(p, frame);
    }
}

}  // namespace

void hatchGeometryOn(uint32_t frame) {
    g_tracker.clear();
    g_watch.reset(static_cast<uint64_t>(GetTickCount64()), "the hatch probe is on");
    g_lines = 0;
    g_suppressed = 0;
    g_padLines = 0;
    g_padSuppressed = 0;
    g_capLogged = false;
    g_padCapLogged = false;
    g_noCameraLogged = false;
    g_keySet = false;
    g_firstHSet = false;
    g_lastPadLineMs = 0;
    g_countPending = true;
    {
        hg::SampleStorage prepared;
        prepared.reserve(hg::kSampleCap);
        (void)g_box.exchange(std::move(prepared));
    }
    g_collecting.store(false, std::memory_order_release);
    g_on.store(true, std::memory_order_release);
    Log::get().note("hatch geometry: on at frame %u: one frame in %u is sampled; a sample keeps the records named BlastShield, Hatch, Shutter, Door, "
                    "Collar, Lid, Cover, Gate, Hanger or Hangar, and every pad (cap %zu). Each is expressed in the own pad's frame (centre at the origin, "
                    "up = axis 2); records on the shaft within %.0f m lateral (or within %.0f m of the camera with no shaft) are followed. Change "
                    "lines: %.2f m or %.1f deg from the last line, at most one per %llu ms while moving, a settled line after %llu ms still; %u record "
                    "lines a session. The lower cycle: the pad's height above the nearest Hanger/Hangar within %.0f m, and a summary of the records "
                    "that move after a lower settles.", frame, hg::kSampleEvery, hg::kSampleCap, static_cast<double>(hg::kLateralM),
                    static_cast<double>(hg::kRangeM), static_cast<double>(hg::kMoveM), static_cast<double>(hg::kAxisDeg),
                    static_cast<unsigned long long>(hg::kChangeIntervalMs), static_cast<unsigned long long>(hg::kSettleMs), hg::kLineCap,
                    static_cast<double>(hg::kShellM));
}

void hatchGeometryOff(uint32_t frame) {
    g_on.store(false, std::memory_order_release);
    g_collecting.store(false, std::memory_order_release);
    (void)g_box.exchange(hg::SampleStorage{});
    g_watch.reset(static_cast<uint64_t>(GetTickCount64()), "the hatch probe is off");
    flushSummary(frame);
    Log::get().note("hatch geometry: off at frame %u: record lines written %u, suppressed %u (cap %u); pad lines written %u, suppressed %u (cap %u); "
                    "records present now %zu; refused (track cap %zu) %u.", frame, g_lines, g_suppressed, hg::kLineCap, g_padLines, g_padSuppressed,
                    hg::kPadLineCap, g_tracker.present(), hg::kTrackCap, g_tracker.refused());
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

// The store's observer for a collected sample: a record with a geometry name or a pad, read the way the trial reads it (its world centre, radius and
// axes). The records it reads are kept under the box's lock; nothing is read from the game under that lock.
void hatchGeometryAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    if (!record || !g_collecting.load(std::memory_order_acquire)) return;
    hg::Sample s;
    s.addr = record;
    if (!sealedTrialRecordNameInto(record, s.name, sizeof(s.name)) || !hg::keepsName(s.name)) return;
    if (!sealedTrialRecordWorld(record, s.world, &s.radius, s.worldAxes, &s.centreSrc, &s.axesSrc)) return;
    s.isPad = sealed::isPadName(s.name);
    uintptr_t ctx = 0;
    if (bucket) readVal(&ctx, bucket);
    g_box.add(s, ctx);
}

}  // namespace edvr
