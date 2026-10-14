// The HDR route's energy instrument and the exposure it measures for NVIDIA. TEMPORARY: the 2026-10-14 DLAA bloom entry in
// docs/design-flat-temporal-aa-2026-09-23.md, section 107 (listed in its Status block; removed when that arc closes).
//
// THE QUESTION. With DLAA or DLSS on the flat HDR route, the game's bloom looks too bright (AA off: normal; DLAA with bloom
// off: normal). The route resolves the scene image H before bloom and tone, and hands NVIDIA's network that radiance with an
// exposure of 1.0 (section 106), far above the range it expects. A tone curve that saturates hides excess energy in a white
// star; bloom is linear in it and shows it. The instrument measures whether the resolve changed the energy of H's bright
// pixels; the exposure measurement is the candidate fix.
//
// THE INSTRUMENT. Twice per second, on a frame the HDR route resolves, one compute pass (flat_hdr_energy_shader.h) reduces
// the scene image the backend is handed (H before the resolve) and again H after the resolve has written it back, each into
// its own small buffer, copied to a staging buffer read with DO_NOT_WAIT on later frames (two slots, so nothing stalls).
// Every 5 s one line, "flat hdr energy:", gives R (the bright-pixel luma sum out over in), M (the largest luma out over in)
// and the count of pixels over the threshold T. R near 1.00 and M at most about 1.05 mean the resolve conserved energy.
// It runs only inside the HDR route's treatment: a session that never treats a frame writes the armed line never, and one
// that treats frames but takes no sample writes a samples=0 line, so "never ran" and "ran, found nothing" read differently.
//
// THE EXPOSURE (advanced.flat_dlss_exposure = scene, the default). NVIDIA's network wants the picture at the range a
// tone-mapped game feeds it: input times exposure around 1 for the bright diffuse surfaces. EDVR measures it from H: the 90th
// percentile of luma over a 128x128 grid, W; the exposure is 1 / W, smoothed in log2 (flat_hdr_energy_shader.h states the
// exact procedure and the pure functions below mirror it for the rig). The result goes to a 1x1 R32_FLOAT texture the DLSS
// evaluation reads (dlaa.cpp). On a frame the pass did not run or failed, the evaluation falls back to 1.0 and the line counts it.
//
// The pure half (layout, decode, the CPU reference of both passes, the line) is here so tools can pin it.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace edvr {

// ---- the energy pass's layout and decode --------------------------------------------------------------------------------
constexpr uint32_t kFlatHdrEnergyOver = 0, kFlatHdrEnergyMax = 1, kFlatHdrEnergyBad = 2, kFlatHdrEnergySaturated = 3,
                   kFlatHdrEnergyFinite = 4, kFlatHdrEnergyHeader = 8, kFlatHdrEnergyGroups = 32 * 32;
constexpr uint32_t kFlatHdrEnergyBright = kFlatHdrEnergyHeader, kFlatHdrEnergyTotal = kFlatHdrEnergyHeader + kFlatHdrEnergyGroups,
                   kFlatHdrEnergyLog = kFlatHdrEnergyHeader + 2 * kFlatHdrEnergyGroups,
                   kFlatHdrEnergyWords = kFlatHdrEnergyHeader + 3 * kFlatHdrEnergyGroups;

struct FlatHdrEnergySample {
    double bright = 0, total = 0, geoMean = 0, max = 0;
    uint32_t over = 0, bad = 0, saturated = 0, finite = 0;
};

inline FlatHdrEnergySample flatHdrEnergyDecode(const uint32_t* words) {
    FlatHdrEnergySample s{};
    double logSum = 0;
    for (uint32_t g = 0; g < kFlatHdrEnergyGroups; ++g) {
        float b, t, l;
        std::memcpy(&b, &words[kFlatHdrEnergyBright + g], sizeof(b));
        std::memcpy(&t, &words[kFlatHdrEnergyTotal + g], sizeof(t));
        std::memcpy(&l, &words[kFlatHdrEnergyLog + g], sizeof(l));
        s.bright += b; s.total += t; logSum += l;
    }
    s.over = words[kFlatHdrEnergyOver]; s.bad = words[kFlatHdrEnergyBad];
    s.saturated = words[kFlatHdrEnergySaturated]; s.finite = words[kFlatHdrEnergyFinite];
    float mx; std::memcpy(&mx, &words[kFlatHdrEnergyMax], sizeof(mx));
    s.max = mx;
    s.geoMean = s.finite ? std::exp2(logSum / static_cast<double>(s.finite)) : 0.0;
    return s;
}

// The CPU reference of the energy pass over an RGBA float image (4 floats a pixel), the way the shader reads it: luma
// 0.2126 R + 0.7152 G + 0.0722 B, a pixel valid when 0 <= luma < 3e38, over the threshold when luma is above it.
inline FlatHdrEnergySample flatHdrEnergyReference(const float* rgba, uint32_t width, uint32_t height, float threshold) {
    FlatHdrEnergySample s{};
    double logSum = 0;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const float* c = rgba + (static_cast<size_t>(y) * width + x) * 4;
        const float l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
        if (!(l >= 0.0f && l < 3.0e38f)) { ++s.bad; continue; }
        ++s.finite; s.total += l; if (l > s.max) s.max = l;
        logSum += std::log2(l > 1.0e-5f ? static_cast<double>(l) : 1.0e-5);
        if (c[0] >= 60000.0f || c[1] >= 60000.0f || c[2] >= 60000.0f) ++s.saturated;
        if (l > threshold) { s.bright += l; ++s.over; }
    }
    s.geoMean = s.finite ? std::exp2(logSum / static_cast<double>(s.finite)) : 0.0;
    return s;
}

// The threshold T of a sample: 64 times the last input's geometric mean luma, between 1 and 4096; 8 before the first.
inline float flatHdrEnergyThreshold(double lastGeoMean) {
    if (!(lastGeoMean > 0.0)) return 8.0f;
    const double t = 64.0 * lastGeoMean;
    return static_cast<float>(t < 1.0 ? 1.0 : t > 4096.0 ? 4096.0 : t);
}

// ---- the exposure pass's procedure, in pure form ------------------------------------------------------------------------
constexpr uint32_t kFlatHdrExposureGrid = 128, kFlatHdrExposureBins = 64;
constexpr double kFlatHdrExposureAlpha = 0.08;   // per resolved frame, in log2: about a dozen frames

// The bin of a luma: half stops from 2^-16, bin 0 also takes luma 0 and everything below.
inline uint32_t flatHdrExposureBin(float luma) {
    if (!(luma > 0.0f)) return 0;
    const double b = std::floor((std::log2(static_cast<double>(luma)) + 16.0) * 2.0);
    return b < 0.0 ? 0u : b > 63.0 ? 63u : static_cast<uint32_t>(b);
}
// The grid's pixel for point (gx, gy), 0..127: ((2 g + 1) size) / 256, integer division, as the shader does it.
inline uint32_t flatHdrExposurePixel(uint32_t g, uint32_t size) {
    return static_cast<uint32_t>((static_cast<uint64_t>(2 * g + 1) * size) / 256);
}
// The 90th-percentile bin of a histogram of n samples: the first bin at which ceil(0.9 n) samples are at or below it.
inline uint32_t flatHdrExposurePercentileBin(const uint32_t* hist, uint32_t n) {
    const uint32_t target = (n * 9 + 9) / 10;
    uint32_t run = 0;
    for (uint32_t k = 0; k < kFlatHdrExposureBins; ++k) { run += hist[k]; if (run >= target) return k; }
    return kFlatHdrExposureBins - 1;
}
// log2 of the white W for a percentile bin: the bin's centre.
inline double flatHdrExposureWhiteLog2(uint32_t bin) { return (static_cast<double>(bin) + 0.5) * 0.5 - 16.0; }

struct FlatHdrExposureMeasure {
    uint32_t samples = 0;      // valid grid points
    uint32_t bin = 0;          // the 90th-percentile bin (0 when no sample)
    double whiteLog2 = 0;      // log2 W (0 when no sample)
};
// The CPU reference of the exposure pass's measurement over an RGBA float image.
inline FlatHdrExposureMeasure flatHdrExposureMeasure(const float* rgba, uint32_t width, uint32_t height) {
    FlatHdrExposureMeasure m{};
    uint32_t hist[kFlatHdrExposureBins] = {};
    for (uint32_t gy = 0; gy < kFlatHdrExposureGrid; ++gy) for (uint32_t gx = 0; gx < kFlatHdrExposureGrid; ++gx) {
        const uint32_t x = flatHdrExposurePixel(gx, width), y = flatHdrExposurePixel(gy, height);
        const float* c = rgba + (static_cast<size_t>(y) * width + x) * 4;
        const float l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
        if (!(l >= 0.0f && l < 3.0e38f)) continue;
        ++hist[flatHdrExposureBin(l)]; ++m.samples;
    }
    if (m.samples) { m.bin = flatHdrExposurePercentileBin(hist, m.samples); m.whiteLog2 = flatHdrExposureWhiteLog2(m.bin); }
    return m;
}
// The smoothing: at once on the first valid measurement and on a reset, else a lerp in log2.
inline double flatHdrExposureSmooth(double prevLog2, bool havePrev, double curLog2, bool reset) {
    return (!havePrev || reset) ? curLog2 : prevLog2 + (curLog2 - prevLog2) * kFlatHdrExposureAlpha;
}
// The exposure NVIDIA is handed: 1 / W.
inline double flatHdrExposureValue(double smoothedLog2) { return std::exp2(-smoothedLog2); }

// ---- the 5 s line -----------------------------------------------------------------------------------------------------
// One window's sums over its samples. `route` is "flat" or "vr-world"; `exposure` names the mode (scene, fixed, auto, or
// "world-auto" on the VR world route, which keeps AutoExposure).
struct FlatHdrEnergyWindow {
    char route[12] = {};
    char backend[16] = {};
    char exposure[12] = {};
    unsigned preset = 0;
    uint32_t width = 0, height = 0;
    unsigned frames = 0;       // frames the route offered a sample to (begin calls) in the window
    unsigned samples = 0;
    double brightIn = 0, brightOut = 0, totalIn = 0, totalOut = 0;
    double maxIn = 0, maxOut = 0;           // sums of the per-sample maxima
    double maxRatioPeak = 0;                // the largest per-sample max out/in
    double overIn = 0, overOut = 0, satIn = 0, satOut = 0, thresholdSum = 0, geoMeanIn = 0, exposureSum = 0;
    uint32_t badIn = 0, badOut = 0, exposureSamples = 0;
    uint32_t hazards = 0, dropped = 0, failures = 0, fallbacks = 0;
};
inline const char* flatHdrEnergyPresetName(unsigned p) {
    switch (p) { case 0: return "auto"; case 10: return "J"; case 11: return "K"; case 12: return "L"; case 13: return "M"; default: return "other"; }
}
// R: the window's bright-pixel luma sum out over in; M: the mean per-sample largest luma out over in; 0 when the divisor is 0.
inline double flatHdrEnergyR(const FlatHdrEnergyWindow& w) { return w.brightIn > 0 ? w.brightOut / w.brightIn : 0.0; }
inline double flatHdrEnergyM(const FlatHdrEnergyWindow& w) { return w.maxIn > 0 ? w.maxOut / w.maxIn : 0.0; }
inline int flatHdrEnergyFormatLine(char* out, size_t size, const FlatHdrEnergyWindow& w) {
    const double n = w.samples ? static_cast<double>(w.samples) : 1.0;
    return std::snprintf(out, size,
        "flat hdr energy: route=%s backend=%s preset=%s exposure=%s samples=%u frames=%u size=%ux%u "
        "R=%.4f M=%.4f Mpeak=%.4f T=%.4g N=%.1f/%.1f (in/out) bright-in=%.5g bright-out=%.5g max-in=%.5g max-out=%.5g "
        "total-out/in=%.4f saturated=%.1f/%.1f gm-in=%.5g applied-exposure=%.5g (%u) bad=%u/%u "
        "fallbacks=%u hazards=%u dropped=%u failures=%u",
        w.route, w.backend, flatHdrEnergyPresetName(w.preset), w.exposure, w.samples, w.frames, w.width, w.height,
        flatHdrEnergyR(w), flatHdrEnergyM(w), w.maxRatioPeak, w.samples ? w.thresholdSum / n : 0.0,
        w.overIn / n, w.overOut / n, w.brightIn / n, w.brightOut / n, w.maxIn / n, w.maxOut / n,
        w.totalIn > 0 ? w.totalOut / w.totalIn : 0.0, w.satIn / n, w.satOut / n, w.geoMeanIn / n,
        w.exposureSamples ? w.exposureSum / w.exposureSamples : 0.0, w.exposureSamples, w.badIn, w.badOut,
        w.fallbacks, w.hazards, w.dropped, w.failures);
}

// ---- the runtime half (flat_hdr_energy.cpp) -----------------------------------------------------------------------------
// Owner render thread only, inside FlatComputeInternalScope; nothing here runs unless the HDR route is treating a frame.
// Before the resolve: true when this frame samples (twice a second, a slot free); `h` is counted now. `route` is "flat" or
// "vr-world".
bool flatHdrEnergyBegin(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, uint32_t width, uint32_t height, const char* route);
// After a successful resolve on a sampling frame: H, now the backend's output, is counted into the same slot.
void flatHdrEnergyEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, const char* backend, unsigned preset,
                      const char* exposureName);
// The resolve refused or fell back after flatHdrEnergyBegin: the slot's input is dropped.
void flatHdrEnergyAbandon();
// Every treated frame: reads finished slots without waiting, and says the 5 s line.
void flatHdrEnergyPoll(ID3D11DeviceContext* ctx);
// Says the window's line now, whatever the clock (a rig's; the runtime lets the 5 s clock say it).
void flatHdrEnergyFlush();

// The measured exposure. Update runs the pass over `h` (a frame the flat route resolves with the scene exposure); `reset`
// restarts the smoothing. True when the 1x1 texture holds this frame's value.
bool flatHdrExposureUpdate(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, uint32_t width, uint32_t height, bool reset);
// The texture the evaluation hands NVIDIA, only on the frame Update succeeded (null otherwise: the evaluation uses 1.0 and
// calls flatHdrExposureCountFallback).
ID3D11Texture2D* flatHdrExposureTexture();
void flatHdrExposureCountFallback();

}  // namespace edvr
