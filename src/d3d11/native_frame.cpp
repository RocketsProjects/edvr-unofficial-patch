#include "../common/native_frame.h"
#include "native_frame_fov_trim_policy.h"

#include "../common/comfort_fade.h"
#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "journal_watch.h"
#include "native_render_labels.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace {
constexpr unsigned kPoolSize = 16;

struct State {
    ID3D11Device* device = nullptr; // borrowed; the host owns its lifetime
    uint64_t generation = 0;
    uint64_t sequenceFloor = 0;
    bool active = false;
    bool invalidated = false;
    bool began = false;
    bool latched = false;
    bool consumerAnnounced = false;
    bool held = false;
    bool transitionEnabled = true;
    bool configNoted = false;
    uint32_t beginCount = 0;
    uint32_t latchCount = 0;
    uint32_t invalidationCount = 0;
    uint32_t lastTransitionEnabled = 0;
    uint32_t lastResubmitEnabled = 0;
    uint32_t lastDeferredPacing = 0;
    bool pacingNoted = false;
    bool weaponStabilityNoted = false;
    bool lastWeaponStability = true;
    EdvrNativeFrameDecision cachedDecision{};
};

State g_pool[kPoolSize];
unsigned g_used = 0;
State* g_current = nullptr;
std::mutex g_mutex;

State* identify(void* context) {
    for (unsigned i = 0; i < g_used; ++i) {
        if (context == &g_pool[i]) return &g_pool[i];
    }
    return nullptr;
}

bool finiteValues(const float* values, size_t count) {
    if (!values) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

bool sameDevice(ID3D11Device* expected, void* candidate) {
    if (!expected || !candidate) return false;
    IUnknown* a = nullptr;
    IUnknown* b = nullptr;
    const bool aOk = SUCCEEDED(expected->QueryInterface(
        IID_IUnknown, reinterpret_cast<void**>(&a)));
    const bool bOk = SUCCEEDED(static_cast<IUnknown*>(candidate)->QueryInterface(
        IID_IUnknown, reinterpret_cast<void**>(&b)));
    const bool equal = aOk && bOk && a == b;
    if (a) a->Release();
    if (b) b->Release();
    return equal;
}

bool rigidPose(const float* m) {
    if (!finiteValues(m, 12)) return false;
    constexpr float kTolerance = 0.0025f;
    for (int r = 0; r < 3; ++r) {
        float norm = 0.0f;
        for (int c = 0; c < 3; ++c) norm += m[r * 4 + c] * m[r * 4 + c];
        if (std::fabs(norm - 1.0f) > kTolerance) return false;
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            float dot = 0.0f;
            for (int c = 0; c < 3; ++c) dot += m[a * 4 + c] * m[b * 4 + c];
            if (std::fabs(dot) > kTolerance) return false;
        }
    }
    const float determinant =
        m[0] * (m[5] * m[10] - m[6] * m[9]) -
        m[1] * (m[4] * m[10] - m[6] * m[8]) +
        m[2] * (m[4] * m[9] - m[5] * m[8]);
    return std::fabs(determinant - 1.0f) <= 0.004f;
}

// Guarded by g_mutex. This cached output preserves the same-DLL temporal-pass
// accessor without exporting the policy module's private state.
uint32_t g_trimDegrees[3] = {0, 0, 0};
bool g_trimResolved = false;
using edvr::plugins::performance::FovTrimSettingsPolicyOpsV1;
FovTrimSettingsPolicyOpsV1 g_trimPolicyOps{};
void* g_trimPolicyState = nullptr;
bool g_trimPolicyConfigured = false;
bool g_trimPolicyPresent = true;

// Constructing this guard does not enter the module. It releases a lazily
// created policy state at normal CRT teardown, as the former static TrimState
// did. The copied ops are declared above and outlive the guard. CRT teardown
// must not wait for g_mutex: another thread may have ended while holding it.
struct TrimPolicyCleanup {
    ~TrimPolicyCleanup() {
        if (g_trimPolicyState) {
            g_trimPolicyOps.destroy(g_trimPolicyState);
            g_trimPolicyState = nullptr;
        }
    }
};
TrimPolicyCleanup g_trimPolicyCleanup;

static_assert(sizeof(NativeRenderLabels::runtimeToken) ==
                  edvr::plugins::performance::kFovTrimSettingsTokenCapacity &&
              sizeof(NativeRenderLabels::systemToken) ==
                  edvr::plugins::performance::kFovTrimSettingsTokenCapacity,
              "FOV trim policy tokens must match the shared label provider");

bool validTrimOps(const FovTrimSettingsPolicyOpsV1& ops) {
    return ops.structSize == sizeof(ops) &&
           ops.version == edvr::plugins::performance::kFovTrimSettingsOpsVersion1 &&
           ops.create && ops.destroy && ops.resolve;
}

void noteTrimSettings(void*, edvr::plugins::performance::FovTrimSettingsNoteV1 kind,
                      uint32_t, const char* keyName, const char* token,
                      const char* headsetKey, uint32_t vertical, uint32_t outer,
                      uint32_t nasal, const char* entries) {
    switch (kind) {
        case edvr::plugins::performance::FovTrimSettingsNoteV1::IgnoredEntry:
            edvr::Log::get().note("fov trim: ignored \"%s\" in %s (entries look like "
                                  "pimax-openxr/pimax-crystal-super:5)",
                                  token ? token : "", keyName ? keyName : "");
            break;
        case edvr::plugins::performance::FovTrimSettingsNoteV1::ResolvedEntries:
            edvr::Log::get().note("fov trim: this headset is %s; vertical %u, outer %u, "
                                  "nasal %u degrees (entries: %s).",
                                  headsetKey ? headsetKey : "", vertical, outer,
                                  nasal, entries ? entries : "");
            break;
        case edvr::plugins::performance::FovTrimSettingsNoteV1::NoHeadset:
            edvr::Log::get().note("fov trim: no headset is known yet, so no trim applies.");
            break;
    }
}

HRESULT resolveTrim(uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    // Absent selection does not query labels or enter the policy module.
    if (!g_trimPolicyPresent) {
        g_trimDegrees[0] = g_trimDegrees[1] = g_trimDegrees[2] = 0;
        g_trimResolved = true;
        return S_OK;
    }

    // The factory is an immutable table, not a module state constructor.
    // The selected state is created only after beginFrame validates a caller.
    if (!g_trimPolicyConfigured) {
        const auto* ops = edvr::plugins::performance::fovTrimSettingsPolicyOpsV1();
        if (!ops || !validTrimOps(*ops)) return E_FAIL;
        g_trimPolicyOps = *ops;
        g_trimPolicyConfigured = true;
    }
    if (!validTrimOps(g_trimPolicyOps)) return E_FAIL;
    if (!g_trimPolicyState) {
        g_trimPolicyState = g_trimPolicyOps.create();
        if (!g_trimPolicyState) return E_OUTOFMEMORY;
    }

    NativeRenderLabels labels{};
    const bool haveHeadset = nativeRenderLabels(&labels);
    edvr::plugins::performance::FovTrimSettingsInputV1 input{};
    input.structSize = sizeof(input);
    input.version = edvr::plugins::performance::kFovTrimSettingsPolicyVersion1;
    input.haveHeadset = haveHeadset ? 1u : 0u;
    if (haveHeadset) {
        std::memcpy(input.runtimeToken, labels.runtimeToken, sizeof(input.runtimeToken));
        std::memcpy(input.systemToken, labels.systemToken, sizeof(input.systemToken));
    }
    edvr::plugins::performance::FovTrimSettingsOutputV1 output{};
    if (!g_trimPolicyOps.resolve(g_trimPolicyState, nullptr, &noteTrimSettings,
                                 &input, &output) ||
        output.structSize != sizeof(output) ||
        output.version != edvr::plugins::performance::kFovTrimSettingsPolicyVersion1 ||
        output.degrees[0] > edvr::native_render::kTrimDegreesMax ||
        output.degrees[1] > edvr::native_render::kTrimDegreesMax ||
        output.degrees[2] > edvr::native_render::kTrimDegreesMax) {
        g_trimDegrees[0] = g_trimDegrees[1] = g_trimDegrees[2] = 0;
        g_trimResolved = false;
        return E_FAIL;
    }
    for (size_t i = 0; i < 3; ++i) out[i] = output.degrees[i];
    for (size_t i = 0; i < 3; ++i) g_trimDegrees[i] = out[i];
    g_trimResolved = true;
    return S_OK;
}

HRESULT WINAPI beginFrame(void* context, const EdvrNativeFrameInput* input,
                          EdvrNativeFrameOutput* output) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    // A version 1, 2, 3 or 4 caller is an openvr_api.dll from before the
    // comfort fade (or, earlier, the channel probe, turbo pacing or the
    // field-of-view trim) existed. Each gets exactly the fields it knows
    // about, and whatever it cannot carry stays out of its struct entirely.
    const bool wantsFade = output &&
        output->version == EDVR_NATIVE_FRAME_VERSION_5 &&
        output->size == sizeof(*output);
    const bool wantsChannel = output && !wantsFade &&
        output->version == EDVR_NATIVE_FRAME_VERSION_4 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_4;
    const bool wantsPacing = output && !wantsFade && !wantsChannel &&
        output->version == EDVR_NATIVE_FRAME_VERSION_3 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_3;
    const bool wantsTrim = output && !wantsFade && !wantsChannel && !wantsPacing &&
        output->version == EDVR_NATIVE_FRAME_VERSION_2 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_2;
    const bool legacy = output && !wantsFade && !wantsChannel && !wantsPacing && !wantsTrim &&
        output->version == EDVR_NATIVE_FRAME_VERSION_1 &&
        output->size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    if (!state || state != g_current || !state->active || !input || !output ||
        input->size != sizeof(*input) || input->version != EDVR_NATIVE_FRAME_VERSION_1 ||
        (!wantsFade && !wantsChannel && !wantsPacing && !wantsTrim && !legacy) ||
        input->generation != state->generation || input->referenceGeneration == 0 ||
        input->sequence == 0 || input->sequence <= state->sequenceFloor ||
        input->valid > 1 || (input->valid && !rigidPose(input->physicalHead))) return E_INVALIDARG;

    EdvrNativeFrameOutput result{};
    result.size = wantsFade ? sizeof(result)
                 : wantsChannel ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_4
                 : wantsPacing ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_3
                 : wantsTrim  ? EDVR_NATIVE_FRAME_OUTPUT_SIZE_2
                              : EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    result.version = wantsFade ? EDVR_NATIVE_FRAME_VERSION_5
                    : wantsChannel ? EDVR_NATIVE_FRAME_VERSION_4
                    : wantsPacing ? EDVR_NATIVE_FRAME_VERSION_3
                    : wantsTrim  ? EDVR_NATIVE_FRAME_VERSION_2
                                 : EDVR_NATIVE_FRAME_VERSION_1;
    // Explorer Cam's comfort fade (comfort_fade.h): how black the view is
    // this frame, 0 unless the frame thread published a fresh level. Only a
    // version 5 caller has the slot; every older shape never learns of it.
    result.fadeAlpha = edvr::comfort::read(GetTickCount64());
    // The old Explorer Cam's headset offset (headOffset, yawRadians,
    // offsetEnabled, offsetGamePoses) is retired: those slots stay zero, so a
    // runtime built before 2026-10-07 reads "no offset" and applies none.
    const bool physicalValid = input->valid != 0;

    // The worn headset's entry in each of the three lists, resolved from the
    // last render-settings query's labels and cached between changes.
    uint32_t trim[edvr::plugins::performance::kFovTrimSettingsKeyCount] = {0, 0, 0};
    const HRESULT trimStatus = resolveTrim(trim);
    if (FAILED(trimStatus)) return trimStatus;
    result.trimVerticalDeg = static_cast<float>(trim[0]);
    result.trimOuterDeg = static_cast<float>(trim[1]);
    result.trimNasalDeg = static_cast<float>(trim[2]);
    result.sceneReady = edvr::sceneArrived() &&
                        sameDevice(state->device, edvr::gameDevice()) ? 1u : 0u;
    result.transitionEnabled = edvr::Config::get().getBool(
        "fix.transition_flash", true) ? 1u : 0u;
    // The key that switched the compositor's re-submit of a withheld frame
    // (advanced.transition_flash_resubmit) is gone; the field stays in the ABI
    // (native_frame.h) and is always on.
    result.resubmitEnabled = 1u;

    // fix.weapon_stability: while the journal watcher says Status.json
    // reports on foot, the frame wait moves from WaitGetPoses to the second
    // eye's Submit (deferred pacing, OpenXR Toolkit's "Turbo mode"), which
    // is what holds the weapon steady on mouse turns. Off, in a ship, or
    // with the watcher off, the wait stays in WaitGetPoses.
    const bool weaponStability = edvr::Config::get().getBool(
        "fix.weapon_stability", true);
    result.deferredPacing =
        (weaponStability && edvr::journalOnFootKnown() && edvr::journalOnFoot())
            ? 1u : 0u;
    if (!state->weaponStabilityNoted ||
        state->lastWeaponStability != weaponStability) {
        edvr::Log::get().note(
            "weapon stability: %s (live; the frame wait moves to the eye "
            "submit while on foot).", weaponStability ? "on" : "off");
        state->weaponStabilityNoted = true;
        state->lastWeaponStability = weaponStability;
    }
    if (weaponStability && !state->pacingNoted && !edvr::journalWatchActive()) {
        edvr::Log::get().note(
            "weapon stability: needs the journal watcher (d3d11.journal_watch) "
            "to say when you are on foot, and it is off, so it never engages.");
        state->pacingNoted = true;
    }

    if (physicalValid) {
        edvr::publishHeadPose(input->physicalHead);
        const float denominator = input->physicalHead[10] < 0.05f
                                      ? 0.05f : input->physicalHead[10];
        edvr::announceHeadForward(-input->physicalHead[8] / denominator,
                                  -input->physicalHead[9] / denominator);
    }
    edvr::clearGlitchFrame();
    // A frame may be prepared by the owner and then dropped before either eye
    // is submitted. Consume an explicit hold only when the first valid latch
    // makes the pair visible to the compositor.
    state->held = false;
    state->transitionEnabled = result.transitionEnabled != 0;
    state->sequenceFloor = input->sequence;
    state->invalidated = false;
    state->began = true;
    state->latched = false;
    state->cachedDecision = {};
    ++state->beginCount;
    if (!state->configNoted ||
        state->lastTransitionEnabled != result.transitionEnabled ||
        state->lastResubmitEnabled != result.resubmitEnabled ||
        state->lastDeferredPacing != result.deferredPacing) {
        edvr::Log::get().note(
            "native frame: begin #%u seq=%llu transition=%s, "
            "resubmit=%s, pacing=%s.", state->beginCount,
            static_cast<unsigned long long>(input->sequence),
            result.transitionEnabled ? "on" : "off",
            result.resubmitEnabled ? "on" : "off",
            result.deferredPacing ? "turbo" : "runtime");
        state->configNoted = true;
        state->lastTransitionEnabled = result.transitionEnabled;
        state->lastResubmitEnabled = result.resubmitEnabled;
        state->lastDeferredPacing = result.deferredPacing;
    }
    // Only as many bytes as the caller's own struct holds.
    std::memcpy(output, &result, result.size);
    return S_OK;
}

// RETIRED with the terrain guard (native_frame.h, EdvrNativeFrameRetiredCall): the slot stays in the
// table, so a runtime built before the removal still finds the provider's own entry there, and the
// entry does nothing.
HRESULT WINAPI retiredCall(void*, uint32_t, float, float) {
    return S_OK;
}

HRESULT WINAPI latchSubmit(void* context, uint64_t sequence,
                           EdvrNativeFrameDecision* decision) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state || state != g_current || !state->active || !state->began ||
        !decision || decision->size != sizeof(*decision) ||
        decision->version != EDVR_NATIVE_FRAME_VERSION_1 || sequence == 0 ||
        sequence != state->sequenceFloor || state->invalidated) return E_INVALIDARG;

    if (!state->latched) {
        const bool marked = edvr::glitchFrameMarked();
        state->held = edvr::takeSubmitHoldFrame();
        state->cachedDecision = {};
        state->cachedDecision.size = sizeof(state->cachedDecision);
        state->cachedDecision.version = EDVR_NATIVE_FRAME_VERSION_1;
        state->cachedDecision.withhold =
            ((state->transitionEnabled && marked) || state->held) ? 1u : 0u;
        state->cachedDecision.jumpOnly =
            (state->cachedDecision.withhold && marked && !state->held) ? 1u : 0u;
        state->cachedDecision.verdict = edvr::jumpVerdictPacked();
        edvr::announceGlitchConsumer();
        state->consumerAnnounced = true;
        state->latched = true;
    }
    ++state->latchCount;
    *decision = state->cachedDecision;
    return S_OK;
}

HRESULT WINAPI invalidate(void* context) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state || state != g_current || !state->active) return E_INVALIDARG;
    state->invalidated = true;
    state->began = false;
    state->latched = false;
    state->cachedDecision = {};
    state->held = false;
    ++state->invalidationCount;
    edvr::clearGlitchFrame();
    return S_OK;
}

HRESULT WINAPI close(void* context) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State* state = identify(context);
    if (!state) return E_INVALIDARG;
    if (!state->active) return S_FALSE;
    edvr::clearGlitchFrame();
    if (state->consumerAnnounced) {
        edvr::retireGlitchConsumer();
        state->consumerAnnounced = false;
    }
    state->active = false;
    state->device = nullptr;
    state->invalidated = true;
    state->began = false;
    state->latched = false;
    if (g_current == state) g_current = nullptr;
    edvr::Log::get().note(
        "native frame: close begins=%u latches=%u invalidations=%u.",
        state->beginCount, state->latchCount, state->invalidationCount);
    return S_OK;
}

} // namespace

bool setNativeFrameFovTrimSettingsPolicyOpsV1(
    const edvr::plugins::performance::FovTrimSettingsPolicyOpsV1* ops) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ops && !validTrimOps(*ops)) return false;
    if (g_trimPolicyState) g_trimPolicyOps.destroy(g_trimPolicyState);
    g_trimPolicyState = nullptr;
    g_trimPolicyOps = ops ? *ops : FovTrimSettingsPolicyOpsV1{};
    g_trimPolicyConfigured = ops != nullptr;
    g_trimPolicyPresent = ops != nullptr;
    g_trimDegrees[0] = g_trimDegrees[1] = g_trimDegrees[2] = 0;
    g_trimResolved = false;
    return true;
}

namespace edvr {

// For temporal_pass.cpp's edges-mode fovea (same DLL, no table needed):
// the cached triple the provider resolved above, vertical/outer/nasal order.
void nativeFrameFovTrimDegrees(uint32_t out[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out[0] = out[1] = out[2] = 0;
    if (g_trimResolved) {
        for (size_t i = 0; i < 3; ++i) out[i] = g_trimDegrees[i];
    }
}

}  // namespace edvr

extern "C" HRESULT WINAPI edvrAcquireNativeFrame(
    const EdvrNativeFrameRequest* request, EdvrNativeFrameTable* table) {
    if (!table || table->size != sizeof(*table) ||
        table->version != EDVR_NATIVE_FRAME_VERSION_1) return E_INVALIDARG;
    *table = {};
    table->size = sizeof(*table);
    table->version = EDVR_NATIVE_FRAME_VERSION_1;
    if (!request || request->size != sizeof(*request) ||
        request->version != EDVR_NATIVE_FRAME_VERSION_1 ||
        !request->gameDevice || request->generation == 0) return E_INVALIDARG;

    IUnknown* identity = nullptr;
    if (FAILED(request->gameDevice->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&identity)))) return E_INVALIDARG;
    identity->Release();
    if (FAILED(request->gameDevice->GetDeviceRemovedReason())) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_current || g_used == kPoolSize) return E_PENDING;
    State& state = g_pool[g_used++];
    state = {};
    state.device = request->gameDevice;
    state.generation = request->generation;
    state.active = true;
    g_current = &state;
    table->context = &state;
    table->beginFrame = beginFrame;
    table->reservedCall = retiredCall;
    table->latchSubmit = latchSubmit;
    table->invalidate = invalidate;
    table->close = close;
    return S_OK;
}
