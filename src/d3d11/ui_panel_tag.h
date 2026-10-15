// Which pixel-shader SRV slots hold a learned interface-panel surface, tagged when the view is BOUND -- and the observe-only census
// that asks, for every draw into an eye, whether it samples one. Pure and header-only: no device, no Config, no Log. The DLL
// (ui_depth.cpp tags and counts, vscreen.cpp reports each bind and each draw, binding_shadow.cpp keeps the bits truthful) and the
// build-gate rig (tools/ui_panel_tag_test) include this one file. docs/ui-layer-2026-09-23.md, "2026-10-14: the panel data-flow census".
//
// WHY. fix.ui_quality names the cockpit holo panels, the flight HUD and the sprite by VERTEX SHADER HASH on the lit HDR target
// (ui_layer_math.h uiLayerFamilyFor): every game recompile or mod swap can drop a panel (issue 90: vs 925ACEDA0153AA5B, 22-25 composite
// draws a frame, left in the scene and swimming). The post-tonemap target already names a panel by what it SAMPLES -- a learned surface,
// ui_depth's set -- but that test is a per-draw slot scan, too dear for the lit target (thousands of scene draws a frame). The way to ask
// the data-flow question there is to answer it where it is cheap: at the BIND. A game binds a few hundred views a frame and draws
// thousands of times; the answer belongs to the view, so it is computed once per bind (a pointer-hash probe, a resolve only for a view
// not seen lately) and the draw reads one byte.
//
// WHAT THIS IS NOT. Nothing is moved, redirected or decided from this. It is the instrument that says whether "a draw into the lit
// target that samples a learned panel surface" names the same draws the hash list does, before anything is built on it.
//
// THE BITS. g_panelTag.panel: bit i set when PS SRV slot i (0..3, the slots the binding shadow records) holds a view over a learned
// panel surface. .eye: bit i set when it holds a view over an eye-sized 2D texture (a post pass's input). The shadow's own rule is kept:
// the bits belong to the shadow's cell, so binding_shadow.cpp clears a slot's bits whenever the cell is written, invalidated or
// forgotten, and the tagger sets them after. A draw can therefore only see a bit the tagger set for the view that is bound now.
//
// RE-CREATION. A surface the game re-creates is learned a few frames after its first sampling draw; a view classified before that is
// "not a panel" and stays so until it is asked again. Every change to the learned set bumps g_panelLearnGen; the bits record the
// generation they were computed at; a bind (or the frame boundary) that finds them stale recomputes all four slots from the shadow.
// What the rule still misses in between is MEASURED, not guessed: a sampled verification (the slow, memo-free answer for the slots
// bound at that draw) counts the draws it disagreed on, and the frames those fell in.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

constexpr uint32_t kPanelTagSlots = 4;        // PS SRV slots 0..3: the binding shadow's PsSrv0..3
constexpr uint8_t kPanelFlagSurface = 1;      // the view is over a learned panel surface
constexpr uint8_t kPanelFlagEyeSized = 2;     // the view is over a 2D texture as large as an eye target

struct PanelTag {
    uint8_t panel = 0;   // bit i: slot i holds a learned panel surface
    uint8_t eye = 0;     // bit i: slot i holds an eye-sized texture
    uint32_t gen = 0;    // the learned-set generation the bits were computed at (0: never; always stale)

    void clearSlot(uint32_t slot) {   // the shadow's cell changed: the old answer is not about what is bound now
        if (slot >= kPanelTagSlots) return;
        const uint8_t keep = static_cast<uint8_t>(~(1u << slot));
        panel &= keep;
        eye &= keep;
    }
    void setSlot(uint32_t slot, uint8_t flags) {
        if (slot >= kPanelTagSlots) return;
        clearSlot(slot);
        if (flags & kPanelFlagSurface) panel = static_cast<uint8_t>(panel | (1u << slot));
        if (flags & kPanelFlagEyeSized) eye = static_cast<uint8_t>(eye | (1u << slot));
    }
    void forget() { panel = 0; eye = 0; }
};

inline PanelTag g_panelTag;
// Set by the layer's frame boundary: the UI layer is live (fix.ui_quality on and the layer armed). The bind hook tags only while it is, so a session with the
// key off or the layer stood down pays one load per bind.
inline bool g_panelTagLive = false;
inline uint32_t g_panelLearnGen = 1;   // bumped by every change to the learned set (add, evict, clear)

// The draw path's ONLY read of the tag: one load. tools/ui_panel_tag_test pins that nothing else of the tagger is reached from it.
inline uint8_t uiPanelTagMask() { return g_panelTag.panel; }
inline bool uiPanelTagStale() { return g_panelTag.gen != g_panelLearnGen; }
inline void uiPanelTagLearnChanged() { ++g_panelLearnGen; }

// The bind step, as the DLL runs it and the rig drives it. view(slot) is the shadow's pointer for PS SRV slot `slot`; classify(ptr) is the
// tagger's flags for it (kPanelFlag*). `tag.gen` != learnGen: the learned set changed since the bits were made, so all four slots are
// recomputed and the call returns true; otherwise only the slots [start, start + n) that fall in 0..3 are, and it returns false.
template <class View, class Classify>
inline bool panelTagBind(PanelTag& tag, uint32_t learnGen, uint32_t start, uint32_t n, View&& view, Classify&& classify) {
    if (tag.gen != learnGen) {
        for (uint32_t i = 0; i < kPanelTagSlots; ++i) tag.setSlot(i, classify(view(i)));
        tag.gen = learnGen;
        return true;
    }
    for (uint32_t slot = start; slot < start + n && slot < kPanelTagSlots; ++slot) tag.setSlot(slot, classify(view(slot)));
    return false;
}

// ---- the per-run census ---------------------------------------------------------------------------------------------------------
constexpr size_t kBoundaryPairs = 8;     // unlisted pairs a window names; more are counted, unnamed
constexpr size_t kBoundaryNamed = 3;     // ...and the line shows the busiest of them
constexpr size_t kBoundaryRuns = 8;      // targets that have had a tagged draw (or a composite) this frame: two eyes, and slack for a re-created pair

// Timing is sampled: one bind in kBindTimedEvery and one draw note in kNoteTimedEvery pay two QueryPerformanceCounter reads; the
// totals are scaled by calls / samples. The verification sample is separate (it is the census's own cost, not the tagging's).
constexpr uint32_t kBindTimedEvery = 16;
constexpr uint32_t kNoteTimedEvery = 64;
constexpr uint32_t kVerifyEvery = 256;       // one verification per this many draw notes ...
constexpr uint32_t kVerifyPerFrame = 4;      // ... and at most this many a frame

struct UiBoundaryPair {
    uint64_t vs = 0, ps = 0, draws = 0;
    bool caught = false;   // the lit-target rule (uiLayerFamilyFor, kPanelTag) took at least one of these draws as a cockpit panel
};

// The unlisted vertex shaders the rule has caught and the log has announced this session: "new panel VS seen", once each.
constexpr size_t kBoundaryAnnouncedMax = 16;
struct UiBoundaryAnnounced {
    uint64_t vs[kBoundaryAnnouncedMax] = {};
    uint32_t n = 0;
    bool knows(uint64_t v) const {
        for (uint32_t i = 0; i < n; ++i)
            if (vs[i] == v) return true;
        return false;
    }
};

// One eye target's run in the current frame: the draws into it, in order.
struct UiBoundaryRun {
    const void* target = nullptr;
    uint32_t since = 0;      // lit-target draws since the last one that sampled a learned surface
    uint32_t between = 0;    // ...summed over the gaps between such draws (the first and the last bound them)
    uint32_t tagged = 0;     // lit-target draws in this run that sampled one
    bool ldrStarted = false; // the 8-bit target's first composite has been seen
};

struct UiBoundaryWindow {
    // the lit HDR eye target
    uint64_t litDraws = 0;      // every owner draw into it
    uint64_t litTagged = 0;     // N: ... that sampled a learned panel surface (the bit)
    uint64_t litListed = 0;     // M: ... of those, named by the hash rule (a family)
    uint64_t litUnlisted = 0;   // K: ... and named by nothing
    uint32_t used = 0;
    uint64_t pastTable = 0;     // of K: draws of pairs the table had no room for
    UiBoundaryPair pairs[kBoundaryPairs];
    uint64_t litRuns = 0;       // eye-frames in which at least one draw sampled a learned surface
    uint64_t sceneBetween = 0;  // X summed: the other lit-target draws between a run's first and last such draw
    uint32_t maxBetween = 0;
    uint64_t runOverflow = 0;   // tagged draws (or composites) whose target found no free run slot: nonzero means "between" is incomplete
    // the 8-bit post-tonemap eye target, after its first composite
    uint64_t ldrComposites = 0; // runs that had a composite (the first one is the anchor, not counted below)
    uint64_t ldrLearned = 0, ldrReadsEye = 0, ldrOther = 0;
    // frames
    uint64_t framesLive = 0;    // frames in which the census saw an eye draw: the denominator
    // cost
    uint64_t bindCalls = 0, bindSamples = 0, bindTicks = 0;
    uint64_t noteCalls = 0, noteSamples = 0, noteTicks = 0;
    // the tagger
    uint64_t memoHits = 0, resolved = 0, unresolved = 0, retags = 0, learnChanges = 0;
    uint64_t rechecked = 0;       // views re-judged after a learned-set change from what was kept of them, with no resolve
    uint64_t otherDim = 0;        // views resolved once and found to be no 2D texture (a 3D or 1D texture, a buffer): never a panel, never asked again
    uint64_t unknownHits = 0;     // binds of a view that could not be resolved, answered from the short memo of that
    uint64_t verifyUnknown = 0;   // slots the verification could not resolve: what its "0 missed" does not cover
    uint64_t slotTagged[kPanelTagSlots] = {};   // tagged lit-target draws by the slot that held the panel surface (the lowest set bit)
    // the verification sample
    uint64_t checks = 0, missed = 0, falseHits = 0, missedFrames = 0;
    bool noted = false, missedThisFrame = false;
    UiBoundaryRun runs[kBoundaryRuns];

    // A target's run exists from its first tagged draw (lit) or first composite (8-bit) until the frame ends. Draws before that are of no interest
    // to either measure ("between the first and last" and "after the first composite"), so they only LOOK a run up; the table is never spent on the
    // frame's many other eye-sized targets (the first flight filled it with the G-buffer's and the lit target never found a slot).
    UiBoundaryRun* findRun(const void* target) {
        for (UiBoundaryRun& r : runs)
            if (r.target == target) return &r;
        return nullptr;
    }
    UiBoundaryRun* openRun(const void* target) {
        for (UiBoundaryRun& r : runs)
            if (!r.target) { r.target = target; return &r; }
        return nullptr;
    }

    // A draw into the lit HDR eye target. tagged: its bit; listed: the hash rule named it.
    void noteLit(const void* target, bool tagged, bool listed) {
        noted = true;
        ++litDraws;
        UiBoundaryRun* r = findRun(target);
        if (!tagged) {
            if (r) ++r->since;
            return;
        }
        ++litTagged;
        if (listed) ++litListed; else ++litUnlisted;
        if (!r) r = openRun(target);
        if (!r) { ++runOverflow; return; }
        if (r->tagged) r->between += r->since;   // the draws before the run's FIRST such draw are not between anything
        r->since = 0;
        ++r->tagged;
    }

    // The shaders of a tagged draw the hash rule did not name (the caller reads them for those alone).
    void nameUnlisted(uint64_t vs, uint64_t ps, bool caught = false) {
        for (uint32_t i = 0; i < used; ++i) {
            if (pairs[i].vs == vs && pairs[i].ps == ps) { ++pairs[i].draws; pairs[i].caught = pairs[i].caught || caught; return; }
        }
        if (used < kBoundaryPairs) {
            pairs[used].vs = vs; pairs[used].ps = ps; pairs[used].draws = 1; pairs[used].caught = caught;
            ++used;
            return;
        }
        ++pastTable;
    }

    // A draw into the 8-bit post-tonemap eye target. composite: it samples a learned surface or the hash rule named it.
    // The first composite of a run is the anchor; every later draw into that target is counted once: learned, else reads an eye-sized
    // texture, else other.
    void noteLdr(const void* target, bool composite, bool tagged, bool readsEye) {
        noted = true;
        UiBoundaryRun* r = findRun(target);
        if (!r || !r->ldrStarted) {
            if (!composite) return;
            if (!r) r = openRun(target);
            if (!r) { ++runOverflow; return; }
            r->ldrStarted = true;
            ++ldrComposites;
            return;
        }
        if (tagged) ++ldrLearned;
        else if (readsEye) ++ldrReadsEye;
        else ++ldrOther;
    }

    // The frame ended: close the runs, count the frame.
    void frameEnd() {
        for (UiBoundaryRun& r : runs) {
            if (r.target && r.tagged) {
                ++litRuns;
                sceneBetween += r.between;
                if (r.between > maxBetween) maxBetween = r.between;
            }
            r = UiBoundaryRun();
        }
        if (noted) ++framesLive;
        if (missedThisFrame) ++missedFrames;
        noted = false;
        missedThisFrame = false;
    }

    void reset() { *this = UiBoundaryWindow(); }
};

// What the line needs from the world.
struct UiBoundaryContext {
    bool detectorOn = true;          // ui depth is running: without it nothing is learned, so no bit is ever set
    const char* profile = "vr";
    uint64_t qpcFrequency = 1;
    std::string probe;               // the engine-node probe's one clause ("node probe: ...")
};

struct UiBoundaryText {
    std::string census, cost;
};

inline double boundaryPerFrame(uint64_t v, uint64_t frames) {
    return frames ? static_cast<double>(v) / static_cast<double>(frames) : 0.0;
}

// Calls a frame x the mean sampled cost of a call, in milliseconds a frame. 0 when nothing was sampled (the line says so).
inline double boundaryMsPerFrame(uint64_t calls, uint64_t samples, uint64_t ticks, uint64_t freq, uint64_t frames) {
    if (!samples || !freq || !frames) return 0.0;
    const double perCallMs = (static_cast<double>(ticks) / static_cast<double>(samples)) * 1000.0 / static_cast<double>(freq);
    return perCallMs * static_cast<double>(calls) / static_cast<double>(frames);
}

// Both lines, whole (the Log adds the time). Printed every window the layer prints at all, zeros included: a line that reads "0.00
// draws a frame" is the count having run and found nothing; no line at all is the code never having run; "NOT COUNTED" is the
// instrument unable to run, and says why.
inline UiBoundaryText uiBoundaryText(const UiBoundaryWindow& w, const UiBoundaryContext& c, uint64_t layerFrames) {
    UiBoundaryText out;
    char buf[640];
    if (!c.detectorOn) {
        std::snprintf(buf, sizeof(buf),
                      "ui boundary: NOT COUNTED (the interface depth pass is not running: fix.temporal_aa is off or it stood down, so no "
                      "surface is learned and no view can be tagged) in %llu frames [%s].",
                      static_cast<unsigned long long>(layerFrames), c.profile);
        out.census = buf;
        out.cost = "ui boundary cost: NOT COUNTED (nothing is tagged while the interface depth pass is off).";
        if (!c.probe.empty()) out.cost += " " + c.probe;
        return out;
    }
    const uint64_t f = w.framesLive;
    std::string line;
    std::snprintf(buf, sizeof(buf),
                  "ui boundary: lit target: %.2f draws a frame sample a learned surface (of %.1f lit draws a frame); %.2f by a listed VS, "
                  "%.2f unlisted",
                  boundaryPerFrame(w.litTagged, f), boundaryPerFrame(w.litDraws, f), boundaryPerFrame(w.litListed, f),
                  boundaryPerFrame(w.litUnlisted, f));
    line = buf;
    if (w.litUnlisted) {
        // the busiest first
        uint32_t order[kBoundaryPairs];
        for (uint32_t i = 0; i < w.used; ++i) order[i] = i;
        for (uint32_t i = 0; i < w.used; ++i)
            for (uint32_t j = i + 1; j < w.used; ++j)
                if (w.pairs[order[j]].draws > w.pairs[order[i]].draws) { const uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
        line += " (top unlisted:";
        for (uint32_t i = 0; i < w.used && i < kBoundaryNamed; ++i) {
            const UiBoundaryPair& p = w.pairs[order[i]];
            std::snprintf(buf, sizeof(buf), "%s vs %016llX ps %016llX %llu draws", i ? "," : "", static_cast<unsigned long long>(p.vs),
                          static_cast<unsigned long long>(p.ps), static_cast<unsigned long long>(p.draws));
            line += buf;
        }
        if (w.used > kBoundaryNamed || w.pastTable) {
            std::snprintf(buf, sizeof(buf), "; %u more pairs, %llu draws unnamed", w.used > kBoundaryNamed ? static_cast<unsigned>(w.used - kBoundaryNamed) : 0u,
                          static_cast<unsigned long long>(w.pastTable));
            line += buf;
        }
        line += ")";
    } else {
        line += " (none)";
    }
    const double x = w.litRuns ? static_cast<double>(w.sceneBetween) / static_cast<double>(w.litRuns) : 0.0;
    std::snprintf(buf, sizeof(buf),
                  "; scene draws between first and last such draw: %.1f an eye-frame (max %u, %llu eye-frames with one); "
                  "8-bit target after first composite: %.2f a learned, %.2f b reads-eye, %.2f c other a frame (%llu composites started "
                  "a run) in %llu frames (%llu live) [%s].",
                  x, w.maxBetween, static_cast<unsigned long long>(w.litRuns), boundaryPerFrame(w.ldrLearned, f),
                  boundaryPerFrame(w.ldrReadsEye, f), boundaryPerFrame(w.ldrOther, f), static_cast<unsigned long long>(w.ldrComposites),
                  static_cast<unsigned long long>(layerFrames), static_cast<unsigned long long>(f), c.profile);
    line += buf;
    if (w.runOverflow) {
        std::snprintf(buf, sizeof(buf), " %llu tagged draws found no run slot, so the between-draws figure is incomplete.", static_cast<unsigned long long>(w.runOverflow));
        line += buf;
    }
    out.census = line;

    // the cost line
    std::string cost = "ui boundary cost: tagging ";
    if (w.bindSamples) {
        std::snprintf(buf, sizeof(buf), "%.4f ms a frame on the bind hook (%.1f binds a frame, 1 in %u timed, %llu samples)",
                      boundaryMsPerFrame(w.bindCalls, w.bindSamples, w.bindTicks, c.qpcFrequency, f), boundaryPerFrame(w.bindCalls, f),
                      kBindTimedEvery, static_cast<unsigned long long>(w.bindSamples));
    } else {
        std::snprintf(buf, sizeof(buf), "no bind sampled (%.1f binds a frame)", boundaryPerFrame(w.bindCalls, f));
    }
    cost += buf;
    if (w.noteSamples) {
        std::snprintf(buf, sizeof(buf), " + %.4f ms a frame on the draw note, the read and the count (%.0f draws a frame, 1 in %u timed, %llu samples)",
                      boundaryMsPerFrame(w.noteCalls, w.noteSamples, w.noteTicks, c.qpcFrequency, f), boundaryPerFrame(w.noteCalls, f),
                      kNoteTimedEvery, static_cast<unsigned long long>(w.noteSamples));
    } else {
        std::snprintf(buf, sizeof(buf), " + no draw note sampled (%.0f draws a frame)", boundaryPerFrame(w.noteCalls, f));
    }
    cost += buf;
    std::snprintf(buf, sizeof(buf),
                  "; tags: %llu memo hits, %llu rejudged after a learned-set change without a resolve, %llu views resolved (%llu of them no 2D texture, "
                  "kept as such), %llu unresolvable (%llu binds answered from that short memo), %llu retags on %llu learned-set changes; tagged lit "
                  "draws by slot s0 %llu s1 %llu s2 %llu s3 %llu; "
                  "verify (1 in %u draws, at most %u a frame; the census's own cost, not in the figures above): %llu checks, %llu missed a "
                  "learned panel, %llu false, %llu frames with a miss, %llu slots it could not resolve (a view of that kind is outside its 0 missed).",
                  static_cast<unsigned long long>(w.memoHits), static_cast<unsigned long long>(w.rechecked), static_cast<unsigned long long>(w.resolved),
                  static_cast<unsigned long long>(w.otherDim), static_cast<unsigned long long>(w.unresolved), static_cast<unsigned long long>(w.unknownHits),
                  static_cast<unsigned long long>(w.retags), static_cast<unsigned long long>(w.learnChanges),
                  static_cast<unsigned long long>(w.slotTagged[0]), static_cast<unsigned long long>(w.slotTagged[1]),
                  static_cast<unsigned long long>(w.slotTagged[2]), static_cast<unsigned long long>(w.slotTagged[3]), kVerifyEvery,
                  kVerifyPerFrame, static_cast<unsigned long long>(w.checks), static_cast<unsigned long long>(w.missed),
                  static_cast<unsigned long long>(w.falseHits), static_cast<unsigned long long>(w.missedFrames),
                  static_cast<unsigned long long>(w.verifyUnknown));
    cost += buf;
    if (!c.probe.empty()) cost += " " + c.probe;
    out.cost = cost;
    return out;
}

// The once-per-session line for a vertex shader the hash list does not name that the lit-target rule took as a cockpit panel. Called with a window's pair; returns
// the line, and remembers the vertex shader, or returns empty for one already announced (or past the table, which is never announced twice by luck).
inline std::string boundaryAnnounce(const UiBoundaryPair& p, UiBoundaryAnnounced& seen) {
    if (!p.caught || !p.draws || seen.knows(p.vs) || seen.n >= kBoundaryAnnouncedMax) return std::string();
    seen.vs[seen.n++] = p.vs;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "ui quality: layer: new panel VS seen: vs %016llX ps %016llX, %llu draws in its first window -- not on the hash list; taken as a cockpit holo panel because "
                  "it samples a learned panel surface in the lit HDR target.",
                  static_cast<unsigned long long>(p.vs), static_cast<unsigned long long>(p.ps), static_cast<unsigned long long>(p.draws));
    return buf;
}

}  // namespace edvr
