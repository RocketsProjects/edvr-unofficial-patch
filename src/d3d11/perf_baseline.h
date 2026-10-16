// The performance baseline probe (2026-10-15): counters and timers that PRICE the hypotheses in
// reviews/performance-flat-vr-2026-10-15.md (F1-F5) so one flight per runtime can say which optimisations are worth
// building. MEASUREMENT ONLY: nothing here changes what is drawn, copied, skipped or bound; no config key; every figure
// is read off the code as it already runs.
//
// WHAT IT HOLDS. Header only, so every rig that compiles a probed source links it with nothing else:
//   - the pure eligibility predicates (classifyDirectInput for F1, classifyInPlace for F2, classifyCopyThrough for F3,
//     classifyDrawFast for F5), which the rigs call with positive and negative cases;
//   - the plain counters those sites bump (owner thread only: a relaxed-nothing increment, no lock, no atomic);
//   - the formatters of each item's one summary line per 30 s window. gpu_census.cpp calls them from its window close
//     (the GPU cost report's own 30 s), handing in the GPU figures the census sections measured.
// F6 (native submit phases) and F7 (the XeSS bridge) live in their own modules and already print per window; see
// docs/performance.md "Baseline probe" for what each added.
//
// "NOT EXERCISED". A line whose code path never ran says so in words (the formatters take the counters, and the first
// test is "did anything happen"), so a window with no flat resolve, no UI layer, no trained eye or no VR draw never
// prints zeros that look like a measurement.
//
// COST. Counters are plain increments on the owner thread. QPC reads are sampled: F4 clocks one frame in
// kSelfFramePeriod (all of that frame's begin/end/poll calls), F5 clocks one draw in kDrawPeriod per kind. A scope on a
// frame that is not clocked is a load of the gate and a branch.
#pragma once

#include <windows.h>
#include <dxgiformat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef EDVR_PERFBASE_NOW
#define EDVR_PERFBASE_NOW() (::edvr::perfbase::qpcNow())
#endif

namespace edvr {
namespace perfbase {

inline int64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline int64_t qpcFrequency() noexcept {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? f.QuadPart : 1;
}
inline double ticksToUs(int64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1.0e6 / static_cast<double>(qpcFrequency());
}

// A window's samples, reservoir-sampled so the percentiles stay unbiased over the whole 30 s at any rate.
template <unsigned N>
struct Samples {
    float v[N];
    uint32_t n = 0;
    uint64_t seen = 0;
    uint32_t rng = 0x9E3779B9u;
    void clear() noexcept { n = 0; seen = 0; }
    void add(float x) noexcept {
        ++seen;
        if (n < N) { v[n++] = x; return; }
        rng = rng * 1664525u + 1013904223u;
        const uint64_t j = (static_cast<uint64_t>(rng) * seen) >> 32;
        if (j < N) v[j] = x;
    }
    // Nearest rank, as the rest of the logs; false with no sample.
    bool percentile(unsigned pct, float* out) const noexcept {
        if (!n) return false;
        float tmp[N];
        std::memcpy(tmp, v, n * sizeof(float));
        std::sort(tmp, tmp + n);
        const size_t idx = (static_cast<size_t>(n) * pct + 99) / 100;
        *out = tmp[(idx ? idx : 1) - 1];
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// GPU hooks. flat_mono_resolve.cpp links no census (its rig links nothing else), so it opens its copy's GPU interval
// through these pointers, which gpu_census.cpp installs at its first frame. Null in a rig: the counters still count.
enum class GpuProbe : int { FlatInputCopy = 0, Count };
using GpuProbeFn = void (*)(void* context, GpuProbe which) noexcept;
inline GpuProbeFn g_gpuBegin = nullptr;
inline GpuProbeFn g_gpuEnd = nullptr;
inline void setGpuHooks(GpuProbeFn begin, GpuProbeFn end) noexcept { g_gpuBegin = begin; g_gpuEnd = end; }
struct GpuProbeScope {
    void* context;
    GpuProbe which;
    GpuProbeScope(void* c, GpuProbe w) noexcept : context(c), which(w) { if (g_gpuBegin) g_gpuBegin(context, which); }
    ~GpuProbeScope() { if (g_gpuEnd) g_gpuEnd(context, which); }
    GpuProbeScope(const GpuProbeScope&) = delete;
    GpuProbeScope& operator=(const GpuProbeScope&) = delete;
};

// What a section's census snapshot says, handed to the formatters (gpu_census.cpp fills it).
struct GpuFigure {
    bool occurred = false;      // the section was entered this window
    unsigned samples = 0;       // timed occurrences that completed
    double msPerCall = 0.0;     // corrected mean per timed call (0 with no sample)
    double msPerFrame = 0.0;    // corrected mean x occurrences per frame
};

// ---------------------------------------------------------------------------------------------------------------
// F1. Flat LDR resolve: the input copy into g.color (flat_mono_resolve.cpp), and whether a direct-input path could
// have skipped it. The predicate is PURE and is not used to change behaviour.
enum class DirectInput : uint8_t {
    Eligible = 0,
    Hdr,               // the HDR route writes the result back into H; the copy is also the snapshot of H
    Overlay,           // overlay handling separates clean and raw colour; both snapshots are kept
    Aliased,           // the source is a texture the resolver itself owns or writes (read/write alias)
    Format,            // not a plain 8-bit RGBA family view of a compatible resource
    SrgbViewTypeless,  // an sRGB view: sampling it would differ from the UNORM interpretation; a UNORM view could be made
    SrgbViewTyped,     // an sRGB view over a fully typed resource: no UNORM view can be made over it
    Unstable,          // not the same resource at the same size on the previous frame: a cached view would not hold
    Count
};
struct DirectInputFacts {
    bool hdr = false;
    bool overlay = false;
    bool aliased = false;
    bool stable = false;
    uint32_t viewFormat = DXGI_FORMAT_UNKNOWN;      // the game's SRV format
    uint32_t resourceFormat = DXGI_FORMAT_UNKNOWN;  // the game's texture format
};
inline DirectInput classifyDirectInput(const DirectInputFacts& f) noexcept {
    if (f.hdr) return DirectInput::Hdr;
    if (f.overlay) return DirectInput::Overlay;
    if (f.aliased) return DirectInput::Aliased;
    if (f.viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        if (f.resourceFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS) return DirectInput::SrgbViewTypeless;
        if (f.resourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DirectInput::SrgbViewTyped;
        return DirectInput::Format;
    }
    if (f.viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM) return DirectInput::Format;
    if (f.resourceFormat != DXGI_FORMAT_R8G8B8A8_UNORM && f.resourceFormat != DXGI_FORMAT_R8G8B8A8_TYPELESS)
        return DirectInput::Format;
    if (!f.stable) return DirectInput::Unstable;
    return DirectInput::Eligible;
}
inline const char* directInputName(DirectInput d) noexcept {
    switch (d) {
    case DirectInput::Eligible: return "eligible";
    case DirectInput::Hdr: return "hdr";
    case DirectInput::Overlay: return "overlay";
    case DirectInput::Aliased: return "aliased";
    case DirectInput::Format: return "format";
    case DirectInput::SrgbViewTypeless: return "srgb view typeless";
    case DirectInput::SrgbViewTyped: return "srgb view typed";
    case DirectInput::Unstable: return "unstable lifetime";
    default: return "?";
    }
}
// Same resource, same size, on the very next frame. Raw pointer identity only: never dereferenced, never AddRef'd (the
// probe must not extend a game texture's life), so a recycled address can read as stable for a frame; the figure is a
// bound, not a proof, and the doc says so.
struct StabilityTracker {
    uintptr_t resource = 0;
    uint64_t frame = 0;
    uint32_t width = 0, height = 0;
    bool observe(uintptr_t res, uint64_t f, uint32_t w, uint32_t h) noexcept {
        const bool stable = resource == res && res != 0 && frame + 1 == f && width == w && height == h;
        resource = res; frame = f; width = w; height = h;
        return stable;
    }
};
enum class CopySite : uint8_t { Resolve = 0, Spatial, Count };
struct F1 {
    uint64_t byRoute[2] = {};   // slot 0 (the flat profile) / any other slot (the VR world route's)
    uint64_t calls[static_cast<size_t>(CopySite::Count)] = {};
    uint64_t verdict[static_cast<size_t>(CopySite::Count)][static_cast<size_t>(DirectInput::Count)] = {};
    uint64_t bytes = 0;
    StabilityTracker track[static_cast<size_t>(CopySite::Count)];
};

// ---------------------------------------------------------------------------------------------------------------
// F2. The UI layer composite (ui_layer.cpp compositeInner), flat and VR. In place needs: the caller says the frame is
// EDVR's own and rewritten in full (ownedFrame), the region is the whole texture, the texture is UAV-bindable, the GPU
// loads typed UAVs of the view format, and the in-place shader loaded. The blocker names the first of the hardware and
// shape conditions that fails, OWNERSHIP EXCLUDED, so "would run in place if owned" is blocker == None && !owned.
enum class InPlaceBlocker : uint8_t { None = 0, NoShader, PartialRegion, NoUavBind, NoTypedLoad, Count };
struct InPlaceFacts {
    bool shaderLoaded = false;
    bool wholeRegion = false;
    bool uavBindable = false;
    bool typedLoadOk = false;
};
inline InPlaceBlocker classifyInPlace(const InPlaceFacts& f) noexcept {
    if (!f.shaderLoaded) return InPlaceBlocker::NoShader;
    if (!f.wholeRegion) return InPlaceBlocker::PartialRegion;
    if (!f.uavBindable) return InPlaceBlocker::NoUavBind;
    if (!f.typedLoadOk) return InPlaceBlocker::NoTypedLoad;
    return InPlaceBlocker::None;
}
struct F2Side {
    uint64_t composites = 0;
    uint64_t inPlace = 0, outOfPlace = 0, viaCopy = 0;
    uint64_t ownedCalls = 0;                 // the caller said ownedFrame
    uint64_t wouldBeInPlaceIfOwned = 0;      // blocker None and not owned
    uint64_t blocker[static_cast<size_t>(InPlaceBlocker::Count)] = {};
    uint64_t outBytes = 0;                   // bytes the out-of-place composite wrote (the full frame)
};
struct F2 {
    F2Side side[2];                          // 0 flat profile, 1 VR
    // The flat adapter's evidence about the picture it composites over (flat_ui_layer.cpp): not an ownership proof.
    uint64_t flatTreated = 0, flatUntreated = 0, flatPictureUav = 0, flatRegionWhole = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// F3. VR trained output (temporal_pass.cpp): the copy of dlOut into dlSubmit when the UI resolve did not write it.
enum class FormatClass : uint8_t { Unorm = 0, Typeless, Srgb, Other, Count };
inline FormatClass formatClassOf(uint32_t format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: return FormatClass::Unorm;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return FormatClass::Typeless;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return FormatClass::Srgb;
    default: return FormatClass::Other;
    }
}
inline const char* formatClassName(FormatClass c) noexcept {
    switch (c) {
    case FormatClass::Unorm: return "UNORM";
    case FormatClass::Typeless: return "TYPELESS";
    case FormatClass::Srgb: return "SRGB";
    default: return "other";
    }
}
// A copy-through whose two formats are identical typed UNORM could be returned as dlOut instead. Typeless and sRGB
// inputs keep the game-format texture (the compositor must be told the same kind of texture on every path).
inline bool copyThroughReturnable(uint32_t outFormat, uint32_t submitFormat, bool uiResolved) noexcept {
    return !uiResolved && outFormat == submitFormat && formatClassOf(submitFormat) == FormatClass::Unorm;
}
struct F3Eye {
    uint64_t frames = 0;                      // trained eye-frames that reached the submit copy decision
    uint64_t uiResolved = 0;                  // dlSubmit written by the UI resolve (no copy)
    uint64_t copyThrough = 0;                 // dlOut copied into dlSubmit
    uint64_t byFormat[static_cast<size_t>(FormatClass::Count)] = {};   // copy-throughs by dlSubmit's format class
    uint64_t returnable = 0;                  // copy-throughs a return of dlOut could replace
    uint64_t bytes = 0;                       // bytes copied (D-sized, 4 bytes a pixel)
    uint32_t outFormat = 0, submitFormat = 0; // the last frame's actual formats
    uint32_t width = 0, height = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// F4. The UI machinery's own timing (ui_layer.cpp routeBegin / routeEnd / routePoll), CPU side.
constexpr unsigned kSelfFramePeriod = 8;      // one frame in this many clocks every begin/end/poll in it
enum class SelfKind : uint8_t { Begin = 0, End, Poll, Count };
struct F4 {
    uint64_t begun = 0, untimed = 0, ended = 0, polls = 0, pollsThatRead = 0;
    uint64_t readTimers = 0;                  // timers a poll consumed (ready or invalid)
    uint64_t pendingScanned = 0;              // entries walked by the polls' pending scans
    uint32_t pendingPeak = 0;                 // the largest head - tail seen this window
    uint32_t frameBegun = 0, framePolls = 0;  // this frame's, folded at the boundary
    uint32_t maxBegunPerFrame = 0, maxPollsPerFrame = 0;
    uint64_t frames = 0;                      // frames the boundary closed this window with the machinery in use
    uint64_t selfTicks[static_cast<size_t>(SelfKind::Count)] = {};   // clocked frames only
    uint64_t selfCalls[static_cast<size_t>(SelfKind::Count)] = {};
    uint64_t sampledFrames = 0;
    int64_t frameTicks = 0;                   // this frame's, clocked frames only
    Samples<1024> frameUs;                    // per clocked frame: begin + end + poll microseconds
    bool gate = false;                        // this frame is clocked
    uint64_t ordinal = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// F5. VR draw hooks (vscreen.cpp): AA-off unclaimed X draws that the D/I/N shortcut would take, and the sampled hook
// self-time by draw kind.
enum class DrawFast : uint8_t { Fast = 0, AaOn, Claimed, GateWanted, EngineVelocity, VrWorld, Count };
struct DrawFastInputs {
    bool aaOff = false;
    bool gateWanted = false;
    bool engineVelocityLive = false;
    bool vrWorld = false;
    bool claimed = false;
};
// The same decision as draw_fast_path.h's drawFastPathTaken, with the first reason that refused it named.
inline DrawFast classifyDrawFast(const DrawFastInputs& in) noexcept {
    if (!in.aaOff) return DrawFast::AaOn;
    if (in.claimed) return DrawFast::Claimed;
    if (in.gateWanted) return DrawFast::GateWanted;
    if (in.engineVelocityLive) return DrawFast::EngineVelocity;
    if (in.vrWorld) return DrawFast::VrWorld;
    return DrawFast::Fast;
}
enum class DrawKind : uint8_t { D = 0, I, N, X, Count };
constexpr unsigned kDrawPeriod = 64;          // one draw in this many, per kind, is clocked
struct F5 {
    uint64_t draws[static_cast<size_t>(DrawKind::Count)] = {};
    uint64_t fast[static_cast<size_t>(DrawKind::Count)] = {};
    uint64_t xReason[static_cast<size_t>(DrawFast::Count)] = {};
    Samples<512> hookUs[static_cast<size_t>(DrawKind::Count)];
};

struct State {
    F1 f1;
    F2 f2;
    F3Eye f3[2];
    F4 f4;
    F5 f5;
};
inline State& state() noexcept {
    static State* s = new State;   // never freed: no static destructor under the loader lock
    return *s;
}

// ---- the sites' notes ------------------------------------------------------------------------------------------
// `facts.stable` is filled here from the site's tracker: the resource's identity, the frame and the size.
inline void noteFlatInputCopy(CopySite site, uint32_t slot, DirectInputFacts facts, uintptr_t resource, uint64_t frame,
                              uint32_t w, uint32_t h, uint64_t bytes) noexcept {
    F1& f = state().f1;
    const size_t s = static_cast<size_t>(site);
    facts.stable = f.track[s].observe(resource, frame, w, h);
    ++f.byRoute[slot == 0 ? 0 : 1];
    ++f.calls[s];
    ++f.verdict[s][static_cast<size_t>(classifyDirectInput(facts))];
    f.bytes += bytes;
}

inline void noteComposite(bool flat, bool owned, bool inPlace, bool viaCopy, InPlaceBlocker blocker, uint64_t outBytes) noexcept {
    F2Side& s = state().f2.side[flat ? 0 : 1];
    ++s.composites;
    if (inPlace) ++s.inPlace; else ++s.outOfPlace;
    if (viaCopy) ++s.viaCopy;
    if (owned) ++s.ownedCalls;
    ++s.blocker[static_cast<size_t>(blocker)];
    if (!owned && blocker == InPlaceBlocker::None) ++s.wouldBeInPlaceIfOwned;
    if (!inPlace) s.outBytes += outBytes;
}
inline void noteFlatPicture(bool treated, bool uavBindable, bool wholeRegion) noexcept {
    F2& f = state().f2;
    if (treated) ++f.flatTreated; else ++f.flatUntreated;
    if (uavBindable) ++f.flatPictureUav;
    if (wholeRegion) ++f.flatRegionWhole;
}

inline void noteTrainedEye(uint32_t eye, bool uiResolved, uint32_t outFormat, uint32_t submitFormat, uint32_t w, uint32_t h) noexcept {
    if (eye > 1) return;
    F3Eye& e = state().f3[eye];
    ++e.frames;
    e.outFormat = outFormat; e.submitFormat = submitFormat; e.width = w; e.height = h;
    if (uiResolved) { ++e.uiResolved; return; }
    ++e.copyThrough;
    ++e.byFormat[static_cast<size_t>(formatClassOf(submitFormat))];
    if (copyThroughReturnable(outFormat, submitFormat, false)) ++e.returnable;
    e.bytes += static_cast<uint64_t>(w) * h * 4u;
}

// F4's scopes. One constructed at the top of routeBegin / routeEnd / routePoll.
inline void noteRouteBegin(bool ok) noexcept {
    F4& f = state().f4;
    if (ok) { ++f.begun; ++f.frameBegun; } else ++f.untimed;
}
inline void noteRouteEnd() noexcept { ++state().f4.ended; }
inline void noteRoutePending(uint32_t pending) noexcept {
    F4& f = state().f4;
    if (pending > f.pendingPeak) f.pendingPeak = pending;
}
inline void noteRoutePoll(uint64_t read, uint64_t scanned, uint32_t pendingAtEntry) noexcept {
    F4& f = state().f4;
    ++f.polls; ++f.framePolls;
    f.readTimers += read;
    f.pendingScanned += scanned;
    if (pendingAtEntry > f.pendingPeak) f.pendingPeak = pendingAtEntry;
}
class SelfScope {
public:
    explicit SelfScope(SelfKind k) noexcept : kind_(k), t0_(state().f4.gate ? EDVR_PERFBASE_NOW() : 0), on_(state().f4.gate) {}
    ~SelfScope() {
        if (!on_) return;
        F4& f = state().f4;
        const int64_t d = EDVR_PERFBASE_NOW() - t0_;
        f.selfTicks[static_cast<size_t>(kind_)] += static_cast<uint64_t>(d);
        ++f.selfCalls[static_cast<size_t>(kind_)];
        f.frameTicks += d;
    }
    SelfScope(const SelfScope&) = delete;
    SelfScope& operator=(const SelfScope&) = delete;
private:
    SelfKind kind_;
    int64_t t0_;
    bool on_;
};
// Once per frame, from gpuCensusFrame: folds this frame's machinery figures and decides whether the NEXT frame is clocked.
inline void frameBoundary() noexcept {
    F4& f = state().f4;
    if (f.frameBegun || f.framePolls) {
        ++f.frames;
        if (f.frameBegun > f.maxBegunPerFrame) f.maxBegunPerFrame = f.frameBegun;
        if (f.framePolls > f.maxPollsPerFrame) f.maxPollsPerFrame = f.framePolls;
        if (f.gate) {
            ++f.sampledFrames;
            f.frameUs.add(static_cast<float>(ticksToUs(f.frameTicks)));
        }
    }
    f.frameBegun = f.framePolls = 0;
    f.frameTicks = 0;
    f.gate = (++f.ordinal % kSelfFramePeriod) == 0;
}

// F5's notes. `owner` is false for a draw on another context: those are not counted (their thread is not the owner's).
inline bool noteDraw(DrawKind kind, bool owner) noexcept {
    if (!owner) return false;
    F5& f = state().f5;
    const uint64_t n = ++f.draws[static_cast<size_t>(kind)];
    return (n % kDrawPeriod) == 0;   // the caller clocks this one
}
inline void noteFast(DrawKind kind, bool owner) noexcept {
    if (owner) ++state().f5.fast[static_cast<size_t>(kind)];
}
inline void noteXReason(DrawFast why) noexcept { ++state().f5.xReason[static_cast<size_t>(why)]; }
inline void noteHookSelf(DrawKind kind, int64_t ticks) noexcept {
    state().f5.hookUs[static_cast<size_t>(kind)].add(static_cast<float>(ticksToUs(ticks)));
}

// ---- the window: formatting and reset -------------------------------------------------------------------------
constexpr size_t kLineMax = 1000;

inline double pct(uint64_t part, uint64_t whole) noexcept {
    return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}
inline void gpuText(char* out, size_t n, const GpuFigure& g, const char* unit, double perFrame) noexcept {
    if (!g.occurred) std::snprintf(out, n, "gpu not entered this window");
    else if (!g.samples) std::snprintf(out, n, "gpu entered but no %s was timed this window", unit);
    else std::snprintf(out, n, "gpu %.3f ms per %s (%u timed), %.3f ms/frame", g.msPerCall, unit, g.samples, perFrame);
}

// F1. Returns false (and writes the not-exercised sentence) when nothing ran.
inline bool formatF1(char* out, size_t n, const F1& f, const GpuFigure& gpu, uint64_t frames) noexcept {
    const uint64_t total = f.calls[0] + f.calls[1];
    if (!total) {
        std::snprintf(out, n, "perf baseline F1 flat input copy: not exercised (no flat resolve or spatial fallback reached "
                              "the input copy this window; flat inactive, AA off, or every frame refused earlier)");
        return false;
    }
    uint64_t v[static_cast<size_t>(DirectInput::Count)] = {};
    for (size_t s = 0; s < 2; ++s)
        for (size_t d = 0; d < static_cast<size_t>(DirectInput::Count); ++d) v[d] += f.verdict[s][d];
    char refused[400];
    size_t used = 0;
    refused[0] = 0;
    for (size_t d = 1; d < static_cast<size_t>(DirectInput::Count); ++d) {
        const int w = std::snprintf(refused + used, sizeof(refused) - used, "%s%s %llu", d > 1 ? ", " : "",
                                    directInputName(static_cast<DirectInput>(d)), static_cast<unsigned long long>(v[d]));
        if (w > 0) used += (std::min)(static_cast<size_t>(w), sizeof(refused) - used - 1);
    }
    char g[160];
    gpuText(g, sizeof(g), gpu, "copy", gpu.msPerFrame);
    std::snprintf(out, n,
                  "perf baseline F1 flat input copy: %llu copies (resolve %llu, spatial fallback %llu; slot 0 %llu, other slots %llu), "
                  "direct-input eligible %llu (%.1f%%), refused: %s; %.1f MB per copy over %llu frames; %s",
                  static_cast<unsigned long long>(total), static_cast<unsigned long long>(f.calls[0]),
                  static_cast<unsigned long long>(f.calls[1]), static_cast<unsigned long long>(f.byRoute[0]),
                  static_cast<unsigned long long>(f.byRoute[1]), static_cast<unsigned long long>(v[0]), pct(v[0], total), refused,
                  static_cast<double>(f.bytes) / static_cast<double>(total) / 1.0e6, static_cast<unsigned long long>(frames), g);
    return true;
}

// F2. One line for both profiles; a side that did not composite is named "none".
inline bool formatF2(char* out, size_t n, const F2& f, const GpuFigure& gpu, uint64_t frames) noexcept {
    const uint64_t total = f.side[0].composites + f.side[1].composites;
    if (!total) {
        std::snprintf(out, n, "perf baseline F2 ui composite: not exercised (the UI layer composited no frame this window; "
                              "fix.ui_quality off, no HUD taken, or no treated door)");
        return false;
    }
    char sides[640];
    size_t used = 0;
    sides[0] = 0;
    for (size_t i = 0; i < 2; ++i) {
        const F2Side& s = f.side[i];
        if (!s.composites) {
            const int w = std::snprintf(sides + used, sizeof(sides) - used, "%s%s none", i ? "; " : "", i ? "vr" : "flat");
            if (w > 0) used += (std::min)(static_cast<size_t>(w), sizeof(sides) - used - 1);
            continue;
        }
        const int w = std::snprintf(
            sides + used, sizeof(sides) - used,
            "%s%s %llu composites (in place %llu, out of place %llu, via copy %llu; owned-frame calls %llu; would run in place if owned %llu; "
            "blocked by shader %llu / region %llu / uav-bind %llu / typed-load %llu; out-of-place wrote %.1f MB per composite)",
            i ? "; " : "", i ? "vr" : "flat", static_cast<unsigned long long>(s.composites), static_cast<unsigned long long>(s.inPlace),
            static_cast<unsigned long long>(s.outOfPlace), static_cast<unsigned long long>(s.viaCopy),
            static_cast<unsigned long long>(s.ownedCalls), static_cast<unsigned long long>(s.wouldBeInPlaceIfOwned),
            static_cast<unsigned long long>(s.blocker[static_cast<size_t>(InPlaceBlocker::NoShader)]),
            static_cast<unsigned long long>(s.blocker[static_cast<size_t>(InPlaceBlocker::PartialRegion)]),
            static_cast<unsigned long long>(s.blocker[static_cast<size_t>(InPlaceBlocker::NoUavBind)]),
            static_cast<unsigned long long>(s.blocker[static_cast<size_t>(InPlaceBlocker::NoTypedLoad)]),
            s.outOfPlace ? static_cast<double>(s.outBytes) / static_cast<double>(s.outOfPlace) / 1.0e6 : 0.0);
        if (w > 0) used += (std::min)(static_cast<size_t>(w), sizeof(sides) - used - 1);
    }
    char flat[200];
    if (f.flatTreated + f.flatUntreated)
        std::snprintf(flat, sizeof(flat), "; flat picture: treated %llu, untreated %llu, uav-bindable %llu, whole region %llu (evidence, not an ownership proof)",
                      static_cast<unsigned long long>(f.flatTreated), static_cast<unsigned long long>(f.flatUntreated),
                      static_cast<unsigned long long>(f.flatPictureUav), static_cast<unsigned long long>(f.flatRegionWhole));
    else
        flat[0] = 0;
    char g[160];
    gpuText(g, sizeof(g), gpu, "composite", gpu.msPerFrame);
    std::snprintf(out, n, "perf baseline F2 ui composite over %llu frames: %s%s; %s", static_cast<unsigned long long>(frames), sides, flat, g);
    return true;
}

// F3.
inline bool formatF3(char* out, size_t n, const F3Eye (&e)[2], const GpuFigure& gpu, uint64_t frames) noexcept {
    const uint64_t total = e[0].frames + e[1].frames;
    if (!total) {
        std::snprintf(out, n, "perf baseline F3 vr trained output: not exercised (no trained-upscaler eye frame reached the submit copy "
                              "this window; the VR trained pass was not active)");
        return false;
    }
    char eyes[640];
    size_t used = 0;
    eyes[0] = 0;
    for (size_t i = 0; i < 2; ++i) {
        const F3Eye& x = e[i];
        if (!x.frames) {
            const int w = std::snprintf(eyes + used, sizeof(eyes) - used, "%s%s none", i ? "; " : "", i ? "right" : "left");
            if (w > 0) used += (std::min)(static_cast<size_t>(w), sizeof(eyes) - used - 1);
            continue;
        }
        const int w = std::snprintf(
            eyes + used, sizeof(eyes) - used,
            "%s%s %llu eye-frames at %ux%u (dlOut fmt %u -> dlSubmit fmt %u): ui resolve wrote %llu, copy-through %llu "
            "[UNORM %llu, TYPELESS %llu, SRGB %llu, other %llu], returnable as dlOut %llu, %.1f MB per copy",
            i ? "; " : "", i ? "right" : "left", static_cast<unsigned long long>(x.frames), x.width, x.height, x.outFormat,
            x.submitFormat, static_cast<unsigned long long>(x.uiResolved), static_cast<unsigned long long>(x.copyThrough),
            static_cast<unsigned long long>(x.byFormat[0]), static_cast<unsigned long long>(x.byFormat[1]),
            static_cast<unsigned long long>(x.byFormat[2]), static_cast<unsigned long long>(x.byFormat[3]),
            static_cast<unsigned long long>(x.returnable),
            x.copyThrough ? static_cast<double>(x.bytes) / static_cast<double>(x.copyThrough) / 1.0e6 : 0.0);
        if (w > 0) used += (std::min)(static_cast<size_t>(w), sizeof(eyes) - used - 1);
    }
    char g[160];
    gpuText(g, sizeof(g), gpu, "copy", gpu.msPerFrame);
    std::snprintf(out, n, "perf baseline F3 vr trained output over %llu frames: %s; %s", static_cast<unsigned long long>(frames), eyes, g);
    return true;
}

// F4.
inline bool formatF4(char* out, size_t n, F4& f, uint64_t frames) noexcept {
    if (!f.begun && !f.untimed && !f.polls && !f.ended) {
        std::snprintf(out, n, "perf baseline F4 ui timing machinery: not exercised (no UI route timer began, ended or was polled this window; "
                              "the UI layer was idle)");
        return false;
    }
    const double fr = frames ? static_cast<double>(frames) : 1.0;
    char self[300];
    if (f.sampledFrames && f.frameUs.n) {
        float p50 = 0, p95 = 0, p99 = 0;
        f.frameUs.percentile(50, &p50);
        f.frameUs.percentile(95, &p95);
        f.frameUs.percentile(99, &p99);
        const double sf = static_cast<double>(f.sampledFrames);
        std::snprintf(self, sizeof(self),
                      "self cpu over %llu clocked frames (1 in %u): begin %.1f us, end %.1f us, poll %.1f us per frame, "
                      "frame total p50/p95/p99 %.1f/%.1f/%.1f us",
                      static_cast<unsigned long long>(f.sampledFrames), kSelfFramePeriod,
                      ticksToUs(static_cast<int64_t>(f.selfTicks[0])) / sf, ticksToUs(static_cast<int64_t>(f.selfTicks[1])) / sf,
                      ticksToUs(static_cast<int64_t>(f.selfTicks[2])) / sf, p50, p95, p99);
    } else {
        std::snprintf(self, sizeof(self), "self cpu not measured (no clocked frame used the machinery this window)");
    }
    std::snprintf(out, n,
                  "perf baseline F4 ui timing machinery over %llu frames: timer pairs begun %llu (%.1f/frame, max %u/frame), untimed %llu, ended %llu; "
                  "polls %llu (%.1f/frame, max %u/frame) read %llu timers; pending entries scanned %llu (%.0f/frame), pending peak %u of 1024; %s",
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(f.begun), static_cast<double>(f.begun) / fr,
                  f.maxBegunPerFrame, static_cast<unsigned long long>(f.untimed), static_cast<unsigned long long>(f.ended),
                  static_cast<unsigned long long>(f.polls), static_cast<double>(f.polls) / fr, f.maxPollsPerFrame,
                  static_cast<unsigned long long>(f.readTimers), static_cast<unsigned long long>(f.pendingScanned),
                  static_cast<double>(f.pendingScanned) / fr, f.pendingPeak, self);
    return true;
}

// F5.
inline bool formatF5(char* out, size_t n, F5& f, uint64_t frames) noexcept {
    uint64_t all = 0;
    for (size_t k = 0; k < static_cast<size_t>(DrawKind::Count); ++k) all += f.draws[k];
    if (!all) {
        std::snprintf(out, n, "perf baseline F5 vr draw hooks: not exercised (no owner-context draw reached the VR draw hooks this window; "
                              "the flat profile, or no VR frame)");
        return false;
    }
    static const char kNames[] = {'D', 'I', 'N', 'X'};
    char kinds[420];
    size_t used = 0;
    kinds[0] = 0;
    const double fr = frames ? static_cast<double>(frames) : 1.0;
    for (size_t k = 0; k < static_cast<size_t>(DrawKind::Count); ++k) {
        float p50 = 0, p95 = 0;
        const bool have = f.hookUs[k].percentile(50, &p50) && f.hookUs[k].percentile(95, &p95);
        char t[64];
        if (have) std::snprintf(t, sizeof(t), "%.2f/%.2f us (%u clocked)", p50, p95, f.hookUs[k].n);
        else std::snprintf(t, sizeof(t), "not clocked");
        const int w = std::snprintf(kinds + used, sizeof(kinds) - used, "%s%c %llu draws (%.0f/frame, fast path %llu), hook self p50/p95 %s",
                                    k ? "; " : "", kNames[k], static_cast<unsigned long long>(f.draws[k]),
                                    static_cast<double>(f.draws[k]) / fr, static_cast<unsigned long long>(f.fast[k]), t);
        if (w > 0) used += (std::min)(static_cast<size_t>(w), sizeof(kinds) - used - 1);
    }
    const uint64_t x = f.draws[static_cast<size_t>(DrawKind::X)];
    const uint64_t xFast = f.xReason[static_cast<size_t>(DrawFast::Fast)];
    std::snprintf(out, n,
                  "perf baseline F5 vr draw hooks over %llu frames: %s; X draws with AA off and unclaimed (drawPathFast would take) %llu of %llu (%.1f%%), "
                  "not eligible: aa on %llu, claimed by a fix %llu, subscriber gate %llu, engine velocity %llu, vr world route %llu",
                  static_cast<unsigned long long>(frames), kinds, static_cast<unsigned long long>(xFast), static_cast<unsigned long long>(x),
                  pct(xFast, x), static_cast<unsigned long long>(f.xReason[static_cast<size_t>(DrawFast::AaOn)]),
                  static_cast<unsigned long long>(f.xReason[static_cast<size_t>(DrawFast::Claimed)]),
                  static_cast<unsigned long long>(f.xReason[static_cast<size_t>(DrawFast::GateWanted)]),
                  static_cast<unsigned long long>(f.xReason[static_cast<size_t>(DrawFast::EngineVelocity)]),
                  static_cast<unsigned long long>(f.xReason[static_cast<size_t>(DrawFast::VrWorld)]));
    return true;
}

// Clears the window's counters (the stability trackers and the last-seen formats are state, not window figures).
inline void resetWindow() noexcept {
    State& s = state();
    StabilityTracker keep[static_cast<size_t>(CopySite::Count)];
    for (size_t i = 0; i < static_cast<size_t>(CopySite::Count); ++i) keep[i] = s.f1.track[i];
    s.f1 = F1{};
    for (size_t i = 0; i < static_cast<size_t>(CopySite::Count); ++i) s.f1.track[i] = keep[i];
    s.f2 = F2{};
    for (auto& e : s.f3) {
        const uint32_t of = e.outFormat, sf = e.submitFormat, w = e.width, h = e.height;
        e = F3Eye{};
        e.outFormat = of; e.submitFormat = sf; e.width = w; e.height = h;
    }
    const bool gate = s.f4.gate;
    const uint64_t ordinal = s.f4.ordinal;
    s.f4 = F4{};
    s.f4.gate = gate;
    s.f4.ordinal = ordinal;
    for (auto& h : s.f5.hookUs) h.clear();
    s.f5 = F5{};
}

}  // namespace perfbase
}  // namespace edvr
