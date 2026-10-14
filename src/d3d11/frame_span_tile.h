#pragma once
// The overlay's GPU figures (perf_monitor.cpp, 2026-10-14): which numbers the GPU BUSY tile and the fps line show. Pure, so
// tools/aa_off_test runs the decision the overlay makes. TWO figures, labelled:
//   - GPU busy, from the OS (gpu_busy_os.h): the time the GPU ran this process's work per frame. The primary figure.
//   - the second figure: the application-render mean (advanced.app_gpu_timing on) when it has samples; otherwise the frame GPU
//     work span from gpuCensusFrame (the census's own frame scope, on with the application clock off) when one has completed;
//     otherwise "--". Both of these are elapsed spans that include GPU idle inside the frame, so they are never the busy figure.
#include <cstddef>
#include <cstdio>

#include "../common/gpu_busy_os.h"

namespace edvr {

// The frame span's timer slots (gpu_census.cpp): a pair is held until it is read back four frames later, so five are
// in flight at most; six leaves one spare. Two timestamps a pair.
constexpr unsigned kFrameSpanSlots = 6;

// WHICH number the GPU figure shows, shared by the menu's GPU TIME tile and the in-headset fps overlay line
// (perf_monitor.cpp, 2026-10-14): the two surfaces once chose separately and only the tile learned the work span, so the
// overlay read "gpu -- ms" for a flight with the application clock off. One choice, both callers.
//   clockOn       advanced.app_gpu_timing is on (the application-render clock is live)
//   appReady      the caller's own test that the application-render figure is valid right now (the tile passes clockOn; the
//                 overlay passes nativeApplicationGpuReady, which also tests the snapshot's age and source)
//   historyHas    the application-render series has samples; with the clock off that series holds the work span (noteFrameWork)
//   spanDone/spanLastMs  the census's last completed work-span pair, the fallback before the series has a sample
enum class GpuTimeSource { None, ApplicationRender, WorkSpan };
struct GpuTimeChoice {
    GpuTimeSource source = GpuTimeSource::None;
    double ms = 0.0;
};
inline GpuTimeChoice chooseGpuTime(bool clockOn, bool appReady, bool historyHas, double historyMeanMs, bool spanDone,
                                   double spanLastMs) {
    GpuTimeChoice c;
    if (appReady && historyHas) {
        c.source = GpuTimeSource::ApplicationRender;
        c.ms = historyMeanMs;
    } else if (!clockOn && (historyHas || spanDone)) {
        c.source = GpuTimeSource::WorkSpan;
        c.ms = historyHas ? historyMeanMs : spanLastMs;
    }
    return c;
}

// Both GPU figures at once: what the tile and the fps line print. One function builds it, so they cannot choose apart.
//   busy       the OS figure (GpuBusyBoard::read), have=false while there is none
//   span       chooseGpuTime's pick for the second figure
//   clockOn    advanced.app_gpu_timing is on: the second figure's label is then "app render", also while it has no sample
struct GpuFigures {
    GpuBusyReading busy;
    GpuTimeChoice span;
    bool clockOn = false;
};
inline GpuFigures chooseGpuFigures(const GpuBusyReading& busy, bool clockOn, bool appReady, bool historyHas, double historyMeanMs,
                                   bool spanDone, double spanLastMs) {
    GpuFigures f;
    f.busy = busy;
    f.clockOn = clockOn;
    f.span = chooseGpuTime(clockOn, appReady, historyHas, historyMeanMs, spanDone, spanLastMs);
    return f;
}
inline const char* gpuSecondFigureLabel(const GpuFigures& f) {
    if (f.span.source == GpuTimeSource::ApplicationRender) return "app render";
    if (f.span.source == GpuTimeSource::WorkSpan) return "span";
    return f.clockOn ? "app render" : "span";
}

// The overlay's one line, "NN fps   gpu busy A ms   span B ms   cpu C ms": "--" only where a figure is absent. The second
// label says what it is: "span" (the frame GPU work span) or "app render" (the application-render clock).
inline void overlayLineText(char* buf, size_t len, double fps, const GpuFigures& gpu, const char* cpuLabel, bool haveCpu,
                            double cpuMs) {
    if (!buf || !len) return;
    char busyValue[24] = "--", spanValue[24] = "--", cpuValue[24] = "--";
    if (gpu.busy.have) std::snprintf(busyValue, sizeof(busyValue), "%.1f", gpu.busy.busyMs);
    if (gpu.span.source != GpuTimeSource::None) std::snprintf(spanValue, sizeof(spanValue), "%.1f", gpu.span.ms);
    if (haveCpu) std::snprintf(cpuValue, sizeof(cpuValue), "%.1f", cpuMs);
    std::snprintf(buf, len, "%.0f fps   gpu busy %s ms   %s %s ms   %s %s ms", fps, busyValue, gpuSecondFigureLabel(gpu), spanValue,
                  cpuLabel ? cpuLabel : "cpu", cpuValue);
}

// The Monitor page's GPU BUSY tile: the busy figure as the value, the second figure in the sub-line (a tile's sub-line holds
// 39 characters). `value` is "--" only when the OS has no figure. Returns whether `value` is a number.
inline const char* gpuBusyShortReason(GpuBusyFail f) {
    return f == GpuBusyFail::NotStarted || f == GpuBusyFail::None ? "OS GPU stats: no sample yet" : "OS GPU stats unavailable";
}
inline bool gpuBusyTileText(const GpuFigures& gpu, char* value, size_t valueLen, char* sub, size_t subLen) {
    if (!value || !sub || !valueLen || !subLen) return false;
    char second[40] = "";
    if (gpu.span.source != GpuTimeSource::None)
        std::snprintf(second, sizeof(second), "%s %.1f ms", gpuSecondFigureLabel(gpu), gpu.span.ms);
    if (gpu.busy.have) {
        std::snprintf(value, valueLen, "%.1f", gpu.busy.busyMs);
        if (second[0]) std::snprintf(sub, subLen, "%s (elapsed)", second);
        else std::snprintf(sub, subLen, "%s", gpu.clockOn ? "app render: no sample yet" : "span: no sample yet");
        return true;
    }
    std::snprintf(value, valueLen, "--");
    if (second[0]) std::snprintf(sub, subLen, "%s; no OS GPU figure", second);
    else std::snprintf(sub, subLen, "%s", gpuBusyShortReason(gpu.busy.failure));
    return false;
}

// The census window line's frame work span field (gpu_census.cpp; tools\edvr_log.py --gpu-work-span parses exactly this text).
// All-time counts of the span pair: completed = pairs read back, skipped = pairs not begun, invalid = pairs the GPU or the
// disjoint scope voided. "last" is "-" until one has completed.
inline void formatFrameWorkSpanField(char* buf, size_t len, unsigned long long completed, unsigned long long skipped,
                                     unsigned long long invalid, double lastMs) {
    if (!buf || !len) return;
    char last[32] = "-";
    if (completed) std::snprintf(last, sizeof(last), "%.3f", lastMs);
    std::snprintf(buf, len, "frame work span: completed %llu, skipped %llu, invalid %llu, last %s ms", completed, skipped, invalid,
                  last);
}

// The work span's arithmetic (gpu_census.cpp's pair): the GPU time from the first command's timestamp to the door end's.
// Clamped at zero: a pair the GPU reports out of order is not a negative frame.
inline double frameWorkSpanMs(double firstCommandMs, double doorEndMs) {
    const double span = doorEndMs - firstCommandMs;
    return span > 0.0 ? span : 0.0;
}

}  // namespace edvr
