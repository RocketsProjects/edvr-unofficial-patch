// The occlusion census's glue: the keys, the capture's phases, the bracket calls the draw wrappers make, and the log and CSV.
// The queries and the arithmetic are occlusion_census_core.cpp's. See occlusion_census.h.
#include "occlusion_census.h"

#include <windows.h>

#include <cstdio>
#include <new>
#include <string>
#include <vector>

#include "binding_shadow.h"
#include "draw_census.h"     // drawCensusIntern: the census's own ids for views, printed as rt= and dsv=
#include "flat_runtime.h"    // flatRuntimeSceneSizes: the flat scene's size, the flat profile's scene flag
#include "../common/config.h"
#include "../common/frame_flag.h"   // sceneArrived: the VR scene flag
#include "../common/log.h"
#include "../common/runtime_profile.h"

namespace edvr {
namespace {

enum class Phase {
    kIdle,         // nothing requested
    kOnWaitScene,  // diag.occlusion_census = on: waiting for the first in-scene frame
    kOnWaitDue,    // ... then 20 s
    kArmed,        // requested; starts at the next frame tick
    kRecording,    // the wrappers are installed; draws are bracketed
    kWaiting,      // recording is over; queries are read back each frame until all answer or the deadline passes
};

constexpr unsigned long long kOnDelayMs = 20000;
constexpr uint32_t kMaxWaitTicks = 600;   // frame ticks the read-back waits before the rest are called lost

struct Capture {
    Phase phase = Phase::kIdle;
    const char* trigger = "?";
    bool onMode = false;
    unsigned long long dueMs = 0;     // GetTickCount64 at which an `on` capture is requested
    uint32_t framesLeft = 0;
    uint32_t frame = 0;               // capture-relative frame the draws now issued belong to, 1-based
    uint32_t waitTicks = 0;
    bool recording = false;           // read by occlusionCensusBegin on every draw
    occlusion::Recorder* rec = nullptr;
    ID3D11DeviceContext* ctx = nullptr;   // held from the first immediate-context draw until the report
    char stamp[24] = {};              // YYYYMMDD_HHMMSS, local time
};
Capture g;

const char* phaseText() {
    switch (g.phase) {
        case Phase::kOnWaitScene: return "waiting for the first in-scene frame";
        case Phase::kOnWaitDue:   return "waiting for its 20 s delay";
        case Phase::kArmed:       return "armed";
        case Phase::kRecording:   return "recording";
        case Phase::kWaiting:     return "reading back its queries";
        case Phase::kIdle:        return "idle";
    }
    return "?";
}

void releaseContext() {
    if (g.ctx) g.ctx->Release();
    g.ctx = nullptr;
}

bool sceneSeen() {
    if (runtimeFlatProfile()) {
        uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
        return flatRuntimeSceneSizes(&rw, &rh, &ow, &oh) && rw != 0;
    }
    return sceneArrived();
}

void stampNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(g.stamp, sizeof(g.stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
}

void startRecording() {
    if (!g.rec) g.rec = new (std::nothrow) occlusion::Recorder();
    if (!g.rec) {
        Log::get().note("occlusion census: no memory for the query pool; nothing measured.");
        g.phase = Phase::kIdle;
        return;
    }
    g.rec->reset();
    g.rec->beginFrame();
    stampNow();
    g.frame = 1;
    g.framesLeft = occlusion::kCaptureFrames;
    g.recording = true;
    g.phase = Phase::kRecording;
    occlusionCensusInstallWrappers(true);
    Log::get().note("occlusion census: recording frames 1-%u of the scene's draws (%s target, from %s); stamp %s.",
                    occlusion::kCaptureFrames, runtimeFlatProfile() ? "flat" : "eye", g.trigger, g.stamp);
}

void finish() {
    const std::vector<std::string> lines = occlusion::formatReport(*g.rec);
    for (const std::string& line : lines) Log::get().note("%s", line.c_str());

    std::wstring path = Config::get().logDir() + L"\\occlusion_census_";
    for (const char* c = g.stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        const std::string csv = occlusion::formatCsv(*g.rec);
        std::fwrite(csv.data(), 1, csv.size(), f);
        std::fclose(f);
        Log::get().note("occlusion census: end rows=%u csv=occlusion_census_%s.csv",
                        static_cast<unsigned>(g.rec->records().size()), g.stamp);
    } else {
        Log::get().note("occlusion census: could not open edvr_logs\\occlusion_census_%s.csv; the report above is complete.",
                        g.stamp);
    }
    releaseContext();
    g.phase = Phase::kIdle;
    g.trigger = "?";
}

}  // namespace

void occlusionCensusConfigure() {
    const std::string v = Config::get().getString("diag.occlusion_census", "off");
    if (v == "on") {
        g.onMode = true;
        g.phase = Phase::kOnWaitScene;
        g.trigger = "diag.occlusion_census = on";
        Log::get().note(
            "occlusion census: diag.occlusion_census = on: one capture of %u frames, 20 s after the first in-scene frame. "
            "In flat that frame is the flat scene's first size report.", occlusion::kCaptureFrames);
    } else if (v == "off" || v.empty()) {
        Log::get().note("occlusion census: not armed (diag.occlusion_census = off). Nothing is measured until "
                        "hotkey.occlusion_census is pressed.");
    } else {
        Log::get().note("occlusion census: diag.occlusion_census = %s is not on or off; treated as off.", v.c_str());
    }
}

void occlusionCensusRequest(const char* trigger) {
    const char* who = trigger ? trigger : "?";
    if (g.phase != Phase::kIdle) {
        Log::get().note("occlusion census: %s pressed, but a capture is already %s; nothing started.", who, phaseText());
        return;
    }
    g.trigger = who;
    g.phase = Phase::kArmed;
    Log::get().note("occlusion census: capture requested by %s; it starts at the next frame tick and records %u frames.",
                    who, occlusion::kCaptureFrames);
}

void occlusionCensusFrameTick() {
    switch (g.phase) {
        case Phase::kIdle:
            return;   // one switch on a value that is almost always idle
        case Phase::kOnWaitScene:
            if (!sceneSeen()) return;
            g.dueMs = GetTickCount64() + kOnDelayMs;
            g.phase = Phase::kOnWaitDue;
            Log::get().note("occlusion census: the first in-scene frame; the capture is requested in 20 s.");
            return;
        case Phase::kOnWaitDue:
            if (GetTickCount64() < g.dueMs) return;
            g.phase = Phase::kArmed;
            [[fallthrough]];
        case Phase::kArmed:
            startRecording();
            return;
        case Phase::kRecording: {
            // The frame this tick ends is kept whole, or dropped whole when its draws overflowed the query pool. A dropped
            // frame ends the capture: the frames kept before it are the window.
            const bool kept = g.rec->endFrame();
            if (!kept) {
                Log::get().note("occlusion census: frame %u overflowed the query pool of %u and is dropped whole (%u draws); "
                                "%u whole frame(s) kept.", g.frame, g.rec->poolLimit(), g.rec->droppedDraws(),
                                g.rec->wholeFrames());
            } else if (g.framesLeft > 1) {
                --g.framesLeft;
                ++g.frame;
                g.rec->beginFrame();
                return;
            }
            g.recording = false;
            occlusionCensusInstallWrappers(false);
            g.phase = Phase::kWaiting;
            g.waitTicks = 0;
            Log::get().note("occlusion census: %u whole frame(s) recorded; reading the queries back over the following frames.",
                            g.rec->wholeFrames());
            return;
        }
        case Phase::kWaiting: {
            ++g.waitTicks;
            const uint32_t left = g.rec ? g.rec->poll(g.ctx) : 0;
            if (left == 0) {
                finish();
                return;
            }
            if (g.waitTicks >= kMaxWaitTicks) {
                const uint32_t lost = g.rec->loseOutstanding();
                Log::get().note("occlusion census: results lost: %u queries did not answer within %u frames; the report "
                                "counts the rest.", lost, kMaxWaitTicks);
                finish();
            }
            return;
        }
    }
}

void occlusionCensusBegin(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, bool scene,
                          occlusion::Ticket* t) {
    if (t) t->index = occlusion::kNoIndex;
    if (!g.recording || !g.rec || !ctx || !t) return;
    // The immediate context is held for the read-back; a deferred context's draw is counted by the recorder and skipped.
    if (!g.ctx && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        g.ctx = ctx;
        ctx->AddRef();
    }
    occlusion::DrawInput in;
    in.kind = kind;
    in.count = count;
    in.instances = instances;
    in.frame = g.frame;
    in.flat = runtimeFlatProfile();
    if (scene) {
        if (bindingGet(BindSlot::Vs)) in.vs = bindingShaderHash(BindSlot::Vs);
        if (bindingGet(BindSlot::Ps)) in.ps = bindingShaderHash(BindSlot::Ps);
        if (void* rtv = bindingGet(BindSlot::Rtv0)) {
            in.hasRtv = true;
            in.rt = static_cast<uint32_t>(drawCensusIntern(rtv));
        }
        if (void* dsv = bindingGet(BindSlot::Dsv0)) {
            in.hasDsv = true;
            in.dsv = static_cast<uint32_t>(drawCensusIntern(dsv));
        }
    }
    g.rec->begin(ctx, in, scene, t);
}

void occlusionCensusEnd(ID3D11DeviceContext* ctx, const occlusion::Ticket& t) {
    if (t.index != occlusion::kNoIndex && g.rec) g.rec->end(ctx, t);
}

}  // namespace edvr
