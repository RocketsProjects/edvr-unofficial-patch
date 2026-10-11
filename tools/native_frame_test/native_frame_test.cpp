#include "../../src/common/native_frame.h"
#include "../../src/common/comfort_fade.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/config.h"
#include "../../src/common/native_render_settings.h"
#include "../../src/d3d11/native_render_policy.h"
#include "../../src/d3d11/native_frame_fov_trim_policy.h"
#include "../../src/plugins/performance/openxr_resolution_policy.h"
#include "../../src/plugins/performance/fov_trim_settings_policy.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/journal_watch.h"
#include "../../src/openxr/native_fov_trim.h"
#include "../../src/openxr/native_fov_startup.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>

// The journal watcher is stubbed so the on-foot signal is the rig's to set.
static bool g_journalActive = false, g_onFootKnown = false, g_onFoot = false;
static bool g_comfortOwnerAllowed = true;
namespace edvr {
bool journalWatchActive() { return g_journalActive; }
bool journalOnFootKnown() { return g_onFootKnown; }
bool journalOnFoot() { return g_onFoot; }
bool comfortOwnerAllowed() noexcept { return g_comfortOwnerAllowed; }
}

using Microsoft::WRL::ComPtr;

static EdvrNativeFrameInput input(uint64_t generation, uint64_t reference,
                                  uint64_t sequence, bool valid = true) {
    EdvrNativeFrameInput result{sizeof(result), EDVR_NATIVE_FRAME_VERSION_1};
    result.generation = generation;
    result.referenceGeneration = reference;
    result.sequence = sequence;
    result.valid = valid ? 1u : 0u;
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(result.physicalHead, identity, sizeof(identity));
    return result;
}

struct TrimPolicyCounts {
    const edvr::plugins::performance::FovTrimSettingsPolicyOpsV1* real = nullptr;
    unsigned creates = 0, destroys = 0, resolves = 0, notes = 0;
    bool failCreate = false, failResolve = false;
};

struct CountedTrimPolicyState {
    TrimPolicyCounts* counts = nullptr;
    void* realState = nullptr;
    void* noteHost = nullptr;
    edvr::plugins::performance::NoteFovTrimSettingsV1 realNote = nullptr;
};

static TrimPolicyCounts g_trimPolicyCounts;

static void* countedTrimCreate() {
    ++g_trimPolicyCounts.creates;
    if (g_trimPolicyCounts.failCreate || !g_trimPolicyCounts.real) return nullptr;
    auto* state = new (std::nothrow) CountedTrimPolicyState{};
    if (!state) return nullptr;
    state->counts = &g_trimPolicyCounts;
    state->realState = g_trimPolicyCounts.real->create();
    if (!state->realState) {
        delete state;
        return nullptr;
    }
    return state;
}

static void countedTrimDestroy(void* context) {
    auto* state = static_cast<CountedTrimPolicyState*>(context);
    if (!state) return;
    ++state->counts->destroys;
    state->counts->real->destroy(state->realState);
    delete state;
}

static void countedTrimNote(void* context,
                            edvr::plugins::performance::FovTrimSettingsNoteV1 kind,
                            uint32_t keyIndex, const char* keyName, const char* token,
                            const char* headsetKey, uint32_t vertical,
                            uint32_t outer, uint32_t nasal, const char* entries) {
    auto* state = static_cast<CountedTrimPolicyState*>(context);
    ++state->counts->notes;
    state->realNote(state->noteHost, kind, keyIndex, keyName, token, headsetKey,
                    vertical, outer, nasal, entries);
}

static bool countedTrimResolve(
    void* context, void* hostContext,
    edvr::plugins::performance::NoteFovTrimSettingsV1 note,
    const edvr::plugins::performance::FovTrimSettingsInputV1* input,
    edvr::plugins::performance::FovTrimSettingsOutputV1* output) {
    auto* state = static_cast<CountedTrimPolicyState*>(context);
    ++state->counts->resolves;
    if (state->counts->failResolve) return false;
    state->noteHost = hostContext;
    state->realNote = note;
    return state->counts->real->resolve(state->realState, state, &countedTrimNote,
                                        input, output);
}

static void* failedTrimCreate() {
    ++g_trimPolicyCounts.creates;
    return nullptr;
}

static void failedTrimDestroy(void*) { ++g_trimPolicyCounts.destroys; }

static bool failedTrimResolve(
    void*, void*, edvr::plugins::performance::NoteFovTrimSettingsV1,
    const edvr::plugins::performance::FovTrimSettingsInputV1*,
    edvr::plugins::performance::FovTrimSettingsOutputV1*) {
    ++g_trimPolicyCounts.resolves;
    return false;
}

static uint32_t countTrimNotes(const TrimPolicyCounts& counts) {
    return counts.notes;
}

struct NativeFovStartupCounts {
    unsigned allowedCalls = 0, factoryCalls = 0, creates = 0, destroys = 0;
    bool allowed = false;
};
constexpr uint32_t kStartupTestPerformanceGroupIndex = 6; // generated Performance index from plugin_manifest.json

static void* nativeFovStartupCreate(void* context) noexcept {
    ++static_cast<NativeFovStartupCounts*>(context)->creates;
    return context;
}

static void nativeFovStartupDestroy(void* context, void*) noexcept {
    ++static_cast<NativeFovStartupCounts*>(context)->destroys;
}

static uint32_t nativeFovStartupBeginFrame(
    void*, void*, const edvr::plugins::performance::NativeFovTrimInputV1*,
    edvr::plugins::performance::NativeFovTrimSnapshotV1*) noexcept {
    return 0;
}

static uint32_t nativeFovStartupSubmit(
    void*, void*, uint32_t, uint32_t, uint32_t,
    edvr::plugins::performance::NativeFovTrimSnapshotV1*) noexcept {
    return 0;
}

static void nativeFovStartupStandDown(void*, void*) noexcept {}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("native_frame_test: dry-run (no provider calls)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;

    unsigned checks = 0, failures = 0;
    const auto check = [&](bool condition, const char* name) {
        ++checks;
        if (!condition) {
            ++failures;
            std::printf("FAIL: %s\n", name);
        }
    };

    // Exercise the same selector used immediately before NativeRuntimeHost
    // construction. A disabled reservation must avoid both the Performance
    // factory and NativeFovTrim's create callback; legacy startup still asks
    // the factory for its historical default.
    NativeFovStartupCounts startupCounts{};
    const edvr::plugins::performance::NativeFovTrimOpsV1 startupOps = {
        sizeof(edvr::plugins::performance::NativeFovTrimOpsV1),
        edvr::plugins::performance::kNativeFovTrimApiVersion,
        &startupCounts, &nativeFovStartupCreate, &nativeFovStartupDestroy,
        &nativeFovStartupBeginFrame, &nativeFovStartupSubmit,
        &nativeFovStartupStandDown};
    edvr::plugin_selection::admission_v1::DecisionV1 startupDecision{};
    const auto startupOwnerAllowed = [&](const auto&, uint32_t groupIndex) noexcept {
        ++startupCounts.allowedCalls;
        return startupCounts.allowed && groupIndex == kStartupTestPerformanceGroupIndex;
    };
    const auto startupFactory = [&]() noexcept {
        ++startupCounts.factoryCalls;
        return &startupOps;
    };
    startupCounts.allowed = false;
    const auto* disabledStartupOps = edvr::openxr::native_fov_startup::selectOps(
        true, startupDecision, kStartupTestPerformanceGroupIndex,
        startupOwnerAllowed, startupFactory);
    edvr::openxr::NativeFovTrim disabledStartupTrim(disabledStartupOps);
    check(disabledStartupOps == nullptr && !disabledStartupTrim.requested() &&
          startupCounts.allowedCalls == 1 && startupCounts.factoryCalls == 0 &&
          startupCounts.creates == 0,
          "disabled native startup reservation skips FOV factory and policy callbacks");
    startupCounts.allowed = true;
    const auto* enabledStartupOps = edvr::openxr::native_fov_startup::selectOps(
        true, startupDecision, kStartupTestPerformanceGroupIndex,
        startupOwnerAllowed, startupFactory);
    {
        edvr::openxr::NativeFovTrim enabledStartupTrim(enabledStartupOps);
        check(enabledStartupOps == &startupOps && enabledStartupTrim.requested() &&
              startupCounts.factoryCalls == 1 && startupCounts.creates == 1,
              "enabled native startup reservation constructs Performance FOV policy");
    }
    const auto allowedCallsBeforeLegacy = startupCounts.allowedCalls;
    const auto* legacyStartupOps = edvr::openxr::native_fov_startup::selectOps(
        false, startupDecision, kStartupTestPerformanceGroupIndex,
        startupOwnerAllowed, startupFactory);
    {
        edvr::openxr::NativeFovTrim legacyStartupTrim(legacyStartupOps);
        check(legacyStartupOps == &startupOps && legacyStartupTrim.requested() &&
              startupCounts.allowedCalls == allowedCallsBeforeLegacy &&
              startupCounts.factoryCalls == 2 && startupCounts.creates == 2,
              "legacy native startup preserves default FOV policy");
    }

    // System32's d3d11 through common/system_d3d11.h, never an import: EDVR's proxy sits beside this exe.
    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "system D3D11 factory");
    std::puts("native_frame_test: factory ready");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ignoredContext;
    D3D_FEATURE_LEVEL featureLevel{};
    check(createDevice && SUCCEEDED(createDevice(
              nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
              D3D11_SDK_VERSION, &device, &featureLevel, &ignoredContext)),
          "WARP device");
    if (!device) return 1;
    std::puts("native_frame_test: device ready");

    const auto* realTrimOps = edvr::plugins::performance::fovTrimSettingsPolicyOpsV1();
    check(realTrimOps && realTrimOps->create && realTrimOps->destroy && realTrimOps->resolve,
          "real Performance trim settings policy is available to the provider fixture");
    if (!realTrimOps) return 1;
    g_trimPolicyCounts = {};
    g_trimPolicyCounts.real = realTrimOps;
    static const edvr::plugins::performance::FovTrimSettingsPolicyOpsV1 countedTrimOps = {
        sizeof(edvr::plugins::performance::FovTrimSettingsPolicyOpsV1),
        edvr::plugins::performance::kFovTrimSettingsOpsVersion1,
        &countedTrimCreate, &countedTrimDestroy, &countedTrimResolve};
    static const edvr::plugins::performance::FovTrimSettingsPolicyOpsV1 failedTrimOps = {
        sizeof(edvr::plugins::performance::FovTrimSettingsPolicyOpsV1),
        edvr::plugins::performance::kFovTrimSettingsOpsVersion1,
        &failedTrimCreate, &failedTrimDestroy, &failedTrimResolve};

    // Reset the process channel so every assertion below observes this
    // provider's session rather than an earlier native-frame run.
    edvr::clearGlitchFrame();
    edvr::requestSubmitHold(0);
    // Published after acquire; doing so also exercises the device identity
    // check without making setup depend on a pre-existing channel mapping.

    // The terrain guard's six keys (removed 2026-10-09), as an old ini still carries them: nothing reads them, whatever they say.
    edvr::Config::get().set("fix.cull_guard", "PeRcEnT");
    edvr::Config::get().set("fix.cull_guard_percent", "75");
    edvr::Config::get().set("fix.cull_guard_fraction_h", "-1");
    edvr::Config::get().set("fix.cull_guard_fraction_v", "nan");
    edvr::Config::get().set("fix.cull_guard_headsets",
                            "94x99, 120x130junk, 95X84, 10x20, 95x84 ");
    edvr::Config::get().set("fix.transition_flash", "1");
    edvr::Config::get().set("advanced.cull_guard_channel", "Matrix");
    // The three field-of-view trims are per-headset lists keyed like
    // fix.openxr_resolution, resolved against the headset the last v2
    // render-settings query saw. Drive that query first, exactly as the host
    // does (tools/native_render_settings_test drives the same export), so the
    // labels the provider matches on are valid: "Oculus" + "Meta Quest 3"
    // sanitise to oculus/meta-quest-3.
    EdvrNativeRenderViewBounds renderBounds[2];
    renderBounds[0] = {1824, 1968, 16384, 16384};
    renderBounds[1] = {1824, 1968, 16384, 16384};
    EdvrNativeRenderSettings renderSettings =
        edvr::native_render::buildRenderSettingsRequest(renderBounds, "Oculus", "Meta Quest 3");
    const EdvrNativeRenderSettings sentRenderSettings = renderSettings;
    edvr::Config::get().set("fix.openxr_resolution", "oculus/meta-quest-3:3283");
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,
                                        sizeof(renderSettings), &renderSettings) == TRUE,
          "the absent policy keeps the mandatory settings query available");
    check(renderSettings.openxrRenderScale == 1.f && renderSettings.matchedEntry == 0 &&
              renderSettings.entryCount == 0 &&
              edvr::native_render::validateRenderSettingsAnswer(sentRenderSettings, renderSettings),
          "the host receives the core-only 100% default before Performance selection");
    edvr::native_render_policy::OpsV1 renderPolicy{
        sizeof(edvr::native_render_policy::OpsV1), edvr::native_render_policy::kVersion,
        edvr::native_render_policy::kPerformanceManifestIndex, 0, nullptr,
        edvr::plugins::performance::resolveOpenxrResolution};
    check(edvr::native_render_policy::registerPolicy(&renderPolicy) == TRUE,
          "register the selected Performance resolution resolver");
    renderSettings = edvr::native_render::buildRenderSettingsRequest(
        renderBounds, "Oculus", "Meta Quest 3");
    const EdvrNativeRenderSettings selectedRequest = renderSettings;
    const float selectedScale = static_cast<float>(3283.0 / 1824.0);
    check(edvrQueryNativeRenderSettings(EDVR_NATIVE_RENDER_SETTINGS_VERSION_2,
                                        sizeof(renderSettings), &renderSettings) == TRUE &&
              edvr::native_render::validateRenderSettingsAnswer(selectedRequest, renderSettings) &&
              std::fabs(renderSettings.openxrRenderScale - selectedScale) < 0.000001f &&
              renderSettings.matchedEntry == 1 && renderSettings.entryCount == 1,
          "the selected resolver applies the saved per-headset setting");
    const float selectedEffective = edvr::native_render::effectiveScale(
        renderSettings.openxrRenderScale, renderBounds, 2);
    check(edvr::native_render::scaledDimension(renderBounds[0].originalWidth,
                                              renderBounds[0].maxWidth, selectedEffective) == 3283 &&
              edvr::native_render::scaledDimension(renderBounds[0].originalHeight,
                                                   renderBounds[0].maxHeight, selectedEffective) == 3542,
          "the selected ratio resolves the expected per-eye dimensions");
    // The keys live in [experimental] since 2026-09-29 (edvr.ini's
    // `# moved-from: fix.fov_trim_*`; tools/config_test proves an old line is
    // read as the new name and tools/installer_test that the merge carries it).
    edvr::Config::get().set("experimental.fov_trim_outer", "oculus/meta-quest-3:7");
    // 44 is outside 0..30, so the entry is refused rather than clamped, and
    // a bare 5 (the form this key used to take) names no headset at all.
    edvr::Config::get().set("experimental.fov_trim_nasal", "oculus/meta-quest-3:44");
    edvr::Config::get().set("experimental.fov_trim_vertical", "5");

    EdvrNativeFrameRequest request{
        sizeof(request), EDVR_NATIVE_FRAME_VERSION_1, device.Get(), 41};
    EdvrNativeFrameTable table{sizeof(table), EDVR_NATIVE_FRAME_VERSION_1};
    std::puts("native_frame_test: acquiring");
    check(edvrAcquireNativeFrame(&request, &table) == S_OK && table.context &&
              table.beginFrame && table.reservedCall && table.latchSubmit &&
              table.invalidate && table.close,
          "acquire exports complete table");
    EdvrNativeFrameTable blockedTable{sizeof(blockedTable),
                                      EDVR_NATIVE_FRAME_VERSION_1};
    check(edvrAcquireNativeFrame(&request, &blockedTable) == E_PENDING,
          "only one active provider session");
    edvr::publishGameDevice(device.Get());
    edvr::announceSceneArrived();

    auto copiedTrimOps = countedTrimOps;
    check(setNativeFrameFovTrimSettingsPolicyOpsV1(&copiedTrimOps),
          "install a counted valid FOV policy before startup-admission fixture");
    copiedTrimOps = {};
    check(applyNativeFrameFovTrimStartupAdmissionV1(true) &&
              g_trimPolicyCounts.creates == 0 && g_trimPolicyCounts.resolves == 0 &&
              g_trimPolicyCounts.notes == 0,
          "Legacy owner result preserves the selected policy lazily");
    check(applyNativeFrameFovTrimStartupAdmissionV1(true) &&
              g_trimPolicyCounts.creates == 0 && g_trimPolicyCounts.resolves == 0 &&
              g_trimPolicyCounts.notes == 0,
          "enabled owner result preserves the selected policy lazily");
    check(applyNativeFrameFovTrimStartupAdmissionV1(false),
          "disabled reservation installs absent FOV settings before provider frames");
    EdvrNativeFrameOutput absentTrimOutput{sizeof(absentTrimOutput),
                                           EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput absentTrimFrame = input(41, 7, 1);
    check(table.beginFrame(table.context, &absentTrimFrame, &absentTrimOutput) == S_OK &&
              absentTrimOutput.trimVerticalDeg == 0.0f &&
              absentTrimOutput.trimOuterDeg == 0.0f &&
              absentTrimOutput.trimNasalDeg == 0.0f,
          "absent selection writes zero trim asks through the real native-frame provider");
    uint32_t absentTrimAccessor[3] = {99, 99, 99};
    edvr::nativeFrameFovTrimDegrees(absentTrimAccessor);
    check(absentTrimAccessor[0] == 0 && absentTrimAccessor[1] == 0 &&
              absentTrimAccessor[2] == 0 && g_trimPolicyCounts.creates == 0 &&
              g_trimPolicyCounts.resolves == 0 && g_trimPolicyCounts.notes == 0,
          "absent selection skips Performance create/resolve/note callbacks and keeps the in-DLL accessor zero");
    copiedTrimOps = countedTrimOps;
    check(setNativeFrameFovTrimSettingsPolicyOpsV1(&copiedTrimOps),
          "copy a valid selected Performance policy table into the provider");
    copiedTrimOps = {};
    EdvrNativeFrameOutput invalidBeforeTrim{sizeof(invalidBeforeTrim),
                                            EDVR_NATIVE_FRAME_VERSION_5};
    invalidBeforeTrim.trimVerticalDeg = -51.0f;
    EdvrNativeFrameInput invalidBeforeTrimFrame = input(99, 7, 2);
    check(table.beginFrame(table.context, &invalidBeforeTrimFrame,
                           &invalidBeforeTrim) == E_INVALIDARG &&
              invalidBeforeTrim.trimVerticalDeg == -51.0f &&
              g_trimPolicyCounts.creates == 0 && g_trimPolicyCounts.resolves == 0,
          "invalid native-frame input is rejected before lazy policy allocation or resolution");

    // Acquire on this thread, then run the complete CPU callback sequence from
    // a different XR-owner thread.  The provider must not use a producer-ID
    // gate for callbacks.
    EdvrNativeFrameOutput firstOutput{sizeof(firstOutput),
                                      EDVR_NATIVE_FRAME_VERSION_5};
    HRESULT workerBegin = E_FAIL;
    HRESULT workerLatch = E_FAIL;
    EdvrNativeFrameDecision firstDecision{
        sizeof(firstDecision), EDVR_NATIVE_FRAME_VERSION_1};
    std::thread owner([&] {
        EdvrNativeFrameInput frame = input(41, 7, 2);
        workerBegin = table.beginFrame(table.context, &frame, &firstOutput);
        edvr::markGlitchFrame();
        workerLatch = table.latchSubmit(table.context, 2, &firstDecision);
    });
    owner.join();
    check(workerBegin == S_OK, "XR owner can begin after producer acquire");
    check(workerLatch == S_OK, "XR owner can latch after producer acquire");
    // The old Explorer Cam's headset offset is retired (2026-10-07): its slots stay in the
    // struct for layout and are always zero, so a runtime that still reads them applies nothing.
    check(firstOutput.reservedOffset[0] == 0.0f && firstOutput.reservedOffset[1] == 0.0f &&
              firstOutput.reservedOffset[2] == 0.0f && firstOutput.reservedYaw == 0.0f &&
              firstOutput.reservedOffsetEnabled == 0 && firstOutput.reservedOffsetGamePoses == 0,
          "the retired offset slots are zero");
    // The terrain guard's slots are retired (2026-10-09): always zero, whatever an old ini says.
    const auto retiredGuardSlotsZero = [](const EdvrNativeFrameOutput& o) {
        for (uint32_t word : o.reservedTerrainGuard) if (word != 0) return false;
        return o.reservedProjectionChannel == 0;
    };
    check(retiredGuardSlotsZero(firstOutput),
          "the terrain guard's retired slots are zero although the ini still carries all six of its old keys");
    check(firstOutput.sceneReady && firstOutput.transitionEnabled &&
              firstOutput.resubmitEnabled,
          "scene and transition outputs");
    check(firstOutput.version == EDVR_NATIVE_FRAME_VERSION_5 &&
              firstOutput.size == sizeof(firstOutput),
          "version 5 answered in kind");
    check(firstOutput.trimOuterDeg == 7.0f &&
              firstOutput.trimNasalDeg == 0.0f &&
              firstOutput.trimVerticalDeg == 0.0f,
          "the worn headset's trim entry applies; out of range and a bare number do not");
    uint32_t firstTrimAccessor[3] = {99, 99, 99};
    edvr::nativeFrameFovTrimDegrees(firstTrimAccessor);
    check(firstTrimAccessor[0] == 0 && firstTrimAccessor[1] == 7 &&
              firstTrimAccessor[2] == 0 && g_trimPolicyCounts.creates == 1 &&
              g_trimPolicyCounts.resolves == 1 && g_trimPolicyCounts.notes == 3,
          "selected Performance lazily creates once and provider output matches the temporal accessor");
    check(firstDecision.withhold && firstDecision.jumpOnly,
          "marked frame is withheld as jump-only");

    // The second latch for one sequence returns the first cached answer, even
    // when the shared mark has changed since the first eye latched.
    edvr::clearGlitchFrame();
    EdvrNativeFrameDecision cached{
        sizeof(cached), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 2, &cached) == S_OK &&
              std::memcmp(&cached, &firstDecision, sizeof(cached)) == 0,
          "second latch returns cached decision");
    check(edvr::glitchConsumerPresent(), "valid latch announces consumer");

    // Invalid physical tracking is a valid frame boundary but does not
    // publish a new pose.
    edvr::markGlitchFrame();
    EdvrNativeFrameInput lost = input(41, 7, 3, false);
    lost.physicalHead[0] = std::numeric_limits<float>::quiet_NaN();
    EdvrNativeFrameOutput lostOutput{sizeof(lostOutput),
                                     EDVR_NATIVE_FRAME_VERSION_5};
    check(table.beginFrame(table.context, &lost, &lostOutput) == S_OK,
          "lost pose succeeds");
    EdvrNativeFrameDecision lostDecision{
        sizeof(lostDecision), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 3, &lostDecision) == S_OK &&
              !lostDecision.withhold,
          "begin clears a mark from the previous frame");
    EdvrNativeFrameInput bad = input(41, 7, 4);
    bad.physicalHead[0] = 2.0f;
    EdvrNativeFrameOutput badOutput{sizeof(badOutput),
                                    EDVR_NATIVE_FRAME_VERSION_5};
    check(table.beginFrame(table.context, &bad, &badOutput) == E_INVALIDARG,
          "non-rigid physical pose rejected");

    // The table's retired call slot (the terrain guard's state announcement): a runtime built before the removal still calls it,
    // and it answers S_OK to anything and does nothing.
    check(table.reservedCall(table.context, 1, 1.25f, 1.5f) == S_OK &&
              table.reservedCall(table.context, 3, std::numeric_limits<float>::quiet_NaN(), 1.0f) == S_OK &&
              table.reservedCall(nullptr, 0, 0.0f, 0.0f) == S_OK,
          "the retired call slot answers S_OK and does nothing, whatever it is handed");

    // Invalidation preserves the sequence floor and clears pending shared
    // state.  The pose previously published remains available.
    float publishedPose[12]{};
    check(edvr::headPose(publishedPose) && publishedPose[0] == 1.0f,
          "physical pose published before invalidation");
    check(table.invalidate(table.context) == S_OK &&
              table.beginFrame(table.context, &lost, &lostOutput) == E_INVALIDARG,
          "invalidate refuses the old sequence");
    EdvrNativeFrameInput next = input(41, 7, 4);
    check(table.beginFrame(table.context, &next, &lostOutput) == S_OK,
          "greater sequence recovers after invalidation");

    // Explicit camera holds survive transition_flash being turned off, and a
    // prepared frame that never reaches Submit does not consume the hold.
    edvr::Config::get().set("fix.transition_flash", "0");
    edvr::requestSubmitHold(1);
    EdvrNativeFrameInput holdFrame = input(41, 7, 5);
    check(table.beginFrame(table.context, &holdFrame, &lostOutput) == S_OK,
          "begin prepared hold frame");
    EdvrNativeFrameInput visibleHoldFrame = input(41, 7, 6);
    check(table.beginFrame(table.context, &visibleHoldFrame, &lostOutput) == S_OK,
          "begin visible hold frame after dropped frame");
    EdvrNativeFrameDecision hold{
        sizeof(hold), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 6, &hold) == S_OK && hold.withhold &&
              !hold.jumpOnly,
          "explicit hold survives transition setting");
    EdvrNativeFrameInput afterHold = input(41, 7, 7);
    check(table.beginFrame(table.context, &afterHold, &lostOutput) == S_OK,
          "begin frame after consumed hold");
    EdvrNativeFrameDecision noHold{
        sizeof(noHold), EDVR_NATIVE_FRAME_VERSION_1};
    check(table.latchSubmit(table.context, 7, &noHold) == S_OK &&
              !noHold.withhold,
          "hold is consumed once by visible pair");

    // An openvr_api.dll from before the trim asks in version 1 and must be
    // answered in version 1, in its own smaller struct, with nothing written
    // past the end of it. A size that does not match its version is refused.
    struct Guarded { EdvrNativeFrameOutput output; uint32_t sentinel; } guarded{};
    guarded.output.size = EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    guarded.output.version = EDVR_NATIVE_FRAME_VERSION_1;
    guarded.output.trimOuterDeg = guarded.output.trimNasalDeg =
        guarded.output.trimVerticalDeg = -99.0f;
    guarded.sentinel = 0xA5A5A5A5u;
    EdvrNativeFrameInput legacyFrame = input(41, 7, 8);
    check(table.beginFrame(table.context, &legacyFrame, &guarded.output) == S_OK &&
              guarded.output.version == EDVR_NATIVE_FRAME_VERSION_1 &&
              guarded.output.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_1,
          "version 1 caller answered in version 1");
    check(guarded.output.sceneReady &&
              guarded.output.trimOuterDeg == -99.0f &&
              guarded.sentinel == 0xA5A5A5A5u,
          "version 1 answer writes no trim and nothing past its struct");
    EdvrNativeFrameOutput mismatched{EDVR_NATIVE_FRAME_OUTPUT_SIZE_1,
                                     EDVR_NATIVE_FRAME_VERSION_2};
    EdvrNativeFrameInput mismatchFrame = input(41, 7, 9);
    check(table.beginFrame(table.context, &mismatchFrame, &mismatched) ==
              E_INVALIDARG,
          "a size that contradicts the version is refused");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_1 + 3 * sizeof(float) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_2,
          "version 2 adds exactly the three trims");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_2 + sizeof(uint32_t) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_3,
          "version 3 adds exactly the pacing flag");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 + sizeof(uint32_t) ==
              EDVR_NATIVE_FRAME_OUTPUT_SIZE_4,
          "version 4 adds exactly the channel");
    check(EDVR_NATIVE_FRAME_OUTPUT_SIZE_4 + sizeof(float) ==
              sizeof(EdvrNativeFrameOutput),
          "version 5 adds exactly the comfort fade level");

    // A runtime-only entry applies to any headset on that runtime with no
    // entry of its own; a key with no entry for the worn headset is no trim,
    // including one keyed on a headset that is not being worn.
    edvr::Config::get().set("experimental.fov_trim_vertical", "oculus:3");
    edvr::Config::get().set("experimental.fov_trim_outer", "");
    edvr::Config::get().set("experimental.fov_trim_nasal", "virtualdesktopxr/meta-quest-3:9");
    EdvrNativeFrameOutput trimOutput{sizeof(trimOutput), EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput trimFrame = input(41, 7, 10);
    check(table.beginFrame(table.context, &trimFrame, &trimOutput) == S_OK &&
              trimOutput.trimVerticalDeg == 3.0f && trimOutput.trimOuterDeg == 0.0f &&
              trimOutput.trimNasalDeg == 0.0f,
          "a runtime-only trim entry applies; an empty list and another headset's entry do not");
    uint32_t changedTrimAccessor[3] = {99, 99, 99};
    edvr::nativeFrameFovTrimDegrees(changedTrimAccessor);
    check(changedTrimAccessor[0] == 3 && changedTrimAccessor[1] == 0 &&
              changedTrimAccessor[2] == 0,
          "the temporal-pass accessor agrees with changed v2+ frame trim fields");

    // fix.weapon_stability && the journal watcher's on-foot signal select
    // EdvrNativeFrameOutput::deferredPacing (version 3 and later). The journal
    // functions are stubbed above so this rig can drive that signal directly
    // instead of needing a real journal file. This first ask is a version 3
    // caller: answered in exactly its own shape, pacing and all, with the
    // channel field never written into its (absent) tail.
    EdvrNativeFrameOutput defaultOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_3, EDVR_NATIVE_FRAME_VERSION_3};
    defaultOutput.reservedProjectionChannel = 0xA5A5A5A5u;
    EdvrNativeFrameInput defaultFrame = input(41, 7, 11);
    check(table.beginFrame(table.context, &defaultFrame, &defaultOutput) == S_OK &&
              defaultOutput.version == EDVR_NATIVE_FRAME_VERSION_3 &&
              defaultOutput.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 &&
              defaultOutput.deferredPacing == 0 &&
              defaultOutput.reservedProjectionChannel == 0xA5A5A5A5u,
          "a version 3 caller is answered in kind, its absent tail untouched");

    g_journalActive = g_onFootKnown = g_onFoot = true;
    EdvrNativeFrameOutput onFootOutput{sizeof(onFootOutput), EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput onFootFrame = input(41, 7, 12);
    check(table.beginFrame(table.context, &onFootFrame, &onFootOutput) == S_OK &&
              onFootOutput.deferredPacing == 1,
          "weapon stability on foot defers pacing");

    g_onFoot = false;
    EdvrNativeFrameOutput inShipOutput{sizeof(inShipOutput), EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput inShipFrame = input(41, 7, 13);
    check(table.beginFrame(table.context, &inShipFrame, &inShipOutput) == S_OK &&
              inShipOutput.deferredPacing == 0,
          "weapon stability in a ship keeps the wait in WaitGetPoses");

    g_onFoot = true;
    edvr::Config::get().set("fix.weapon_stability", "0");
    EdvrNativeFrameOutput disabledOutput{sizeof(disabledOutput), EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput disabledFrame = input(41, 7, 14);
    check(table.beginFrame(table.context, &disabledFrame, &disabledOutput) == S_OK &&
              disabledOutput.deferredPacing == 0,
          "fix.weapon_stability = 0 never defers pacing");

    edvr::Config::get().set("fix.weapon_stability", "1");
    g_journalActive = g_onFootKnown = g_onFoot = false;

    // A version 2 caller is still answered in exactly its own shape, trims
    // and all, with the pacing field never written into its (absent) tail.
    EdvrNativeFrameOutput v2Output{EDVR_NATIVE_FRAME_OUTPUT_SIZE_2, EDVR_NATIVE_FRAME_VERSION_2};
    EdvrNativeFrameInput v2Frame = input(41, 7, 15);
    check(table.beginFrame(table.context, &v2Frame, &v2Output) == S_OK &&
              v2Output.version == EDVR_NATIVE_FRAME_VERSION_2 &&
              v2Output.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_2 &&
              v2Output.trimVerticalDeg == 3.0f && v2Output.trimOuterDeg == 0.0f &&
              v2Output.trimNasalDeg == 0.0f,
          "a version 2 caller is still answered in its own kind");

    // A struct sized for version 3 that claims version 2 names a shape that
    // does not exist and is refused, exactly like the version 1/2 mismatch
    // above, never silently downgraded to the version its label asks for.
    EdvrNativeFrameOutput v3SizeV2Version{sizeof(EdvrNativeFrameOutput), EDVR_NATIVE_FRAME_VERSION_2};
    EdvrNativeFrameInput v3MismatchFrame = input(41, 7, 16);
    check(table.beginFrame(table.context, &v3MismatchFrame, &v3SizeV2Version) ==
              E_INVALIDARG,
          "a version 3 size claiming version 2 is refused");

    // The trim reader asks for the [experimental] names and no others. First the
    // positive half, so the control below can fail: a value under the new name
    // reaches the output (vertical 3 is still set from the runtime-only case
    // above). Then the retired [fix] names carry a value for the worn headset
    // and the new ones carry none, and the game is told no trim: a reader that
    // had kept reading the old keys would tell it 9 degrees. (An old-layout LINE
    // in a file still works -- Config reads it as the new name through the
    // moved-from map, which this rig does not register; tools/config_test does,
    // with the shipped tables.)
    EdvrNativeFrameOutput newNames{sizeof(newNames), EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput newNamesFrame = input(41, 7, 18);
    check(table.beginFrame(table.context, &newNamesFrame, &newNames) == S_OK &&
              newNames.trimVerticalDeg == 3.0f,
          "the trim reader reads experimental.fov_trim_vertical");
    edvr::Config::get().set("experimental.fov_trim_vertical", "");
    edvr::Config::get().set("experimental.fov_trim_outer", "");
    edvr::Config::get().set("experimental.fov_trim_nasal", "");
    edvr::Config::get().set("fix.fov_trim_vertical", "oculus/meta-quest-3:9");
    edvr::Config::get().set("fix.fov_trim_outer", "oculus/meta-quest-3:9");
    edvr::Config::get().set("fix.fov_trim_nasal", "oculus/meta-quest-3:9");
    EdvrNativeFrameOutput retiredNames{sizeof(retiredNames), EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput retiredFrame = input(41, 7, 19);
    check(table.beginFrame(table.context, &retiredFrame, &retiredNames) == S_OK &&
              retiredNames.trimVerticalDeg == 0.0f && retiredNames.trimOuterDeg == 0.0f &&
              retiredNames.trimNasalDeg == 0.0f,
          "control: the retired fix.fov_trim_* names are not read by the trim reader");

    // ---- Explorer Cam's comfort fade (comfort_fade.h; EdvrNativeFrameOutput::fadeAlpha, version 5) --------------------------------------------------------------
    {
        uint64_t seq = 40;
        auto ask5 = [&](float* fade) {
            EdvrNativeFrameOutput o{sizeof(o), EDVR_NATIVE_FRAME_VERSION_5};
            o.fadeAlpha = -77.0f;   // a sentinel the answer must overwrite
            EdvrNativeFrameInput f = input(41, 7, ++seq);
            const HRESULT r = table.beginFrame(table.context, &f, &o);
            *fade = o.fadeAlpha;
            return r;
        };
        float fade = -1.0f;
        edvr::comfort::clear();
        check(ask5(&fade) == S_OK && fade == 0.0f, "NOTHING PUBLISHED: a version 5 caller reads fadeAlpha 0 (and the sentinel is overwritten)");
        edvr::comfort::publish(1.0f, GetTickCount64());
        check(ask5(&fade) == S_OK && fade == 1.0f, "A FRESH LEVEL of 1: a version 5 caller reads 1");
        edvr::comfort::publish(0.37f, GetTickCount64());
        check(ask5(&fade) == S_OK && std::fabs(fade - 0.37f) < 1e-6f, "...a level of 0.37 reads 0.37");
        edvr::comfort::publish(1.0f, GetTickCount64() - 250);
        check(ask5(&fade) == S_OK && fade > 0.3f && fade < 0.7f, "A STALE LEVEL (published 250 ms ago) has half decayed: the d3d11 half going quiet is not black for ever");
        edvr::comfort::publish(1.0f, GetTickCount64() - 5000);
        check(ask5(&fade) == S_OK && fade == 0.0f, "...5 s old reads 0");
        edvr::comfort::publish(std::numeric_limits<float>::quiet_NaN(), GetTickCount64());
        check(ask5(&fade) == S_OK && fade == 0.0f, "NaN reads 0, never black");
        edvr::comfort::publish(9.0f, GetTickCount64());
        check(ask5(&fade) == S_OK && fade == 1.0f, "...above 1 reads 1");
        // ---- the older shapes never learn of it ------------------------------------------------------------------------------------------------------------
        edvr::comfort::publish(1.0f, GetTickCount64());
        EdvrNativeFrameOutput v4{EDVR_NATIVE_FRAME_OUTPUT_SIZE_4, EDVR_NATIVE_FRAME_VERSION_4};
        v4.fadeAlpha = -123.0f;
        EdvrNativeFrameInput v4Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v4Frame, &v4) == S_OK && v4.version == EDVR_NATIVE_FRAME_VERSION_4 && v4.size == EDVR_NATIVE_FRAME_OUTPUT_SIZE_4 && v4.fadeAlpha == -123.0f,
              "A VERSION 4 CALLER is answered in its own shape and the fade slot past it is untouched (a runtime from before the fade cannot be blacked out)");
        EdvrNativeFrameOutput v3{EDVR_NATIVE_FRAME_OUTPUT_SIZE_3, EDVR_NATIVE_FRAME_VERSION_3};
        v3.reservedProjectionChannel = 0xA5A5A5A5u;
        v3.fadeAlpha = -123.0f;
        EdvrNativeFrameInput v3Frame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &v3Frame, &v3) == S_OK && v3.reservedProjectionChannel == 0xA5A5A5A5u && v3.fadeAlpha == -123.0f, "...and a version 3 caller likewise");
        EdvrNativeFrameOutput fullAsV4{sizeof(fullAsV4), EDVR_NATIVE_FRAME_VERSION_4};
        EdvrNativeFrameInput fullMismatchFrame = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &fullMismatchFrame, &fullAsV4) == E_INVALIDARG, "A FULL-SIZE STRUCT CLAIMING VERSION 4 is refused (the size contradicts the version)");
        EdvrNativeFrameOutput v4AsV5{EDVR_NATIVE_FRAME_OUTPUT_SIZE_4, EDVR_NATIVE_FRAME_VERSION_5};
        EdvrNativeFrameInput mismatchFrame2 = input(41, 7, ++seq);
        check(table.beginFrame(table.context, &mismatchFrame2, &v4AsV5) == E_INVALIDARG, "...and a version 4 sized struct claiming version 5 is refused: it is never half-answered");
        edvr::comfort::clear();
    }

    check(table.close(table.context) == S_OK && !edvr::glitchConsumerPresent(),
          "close retires announced consumer");
    check(g_trimPolicyCounts.creates == 1 && g_trimPolicyCounts.destroys == 0,
          "provider close retains the selected module state across provider generations");
    check(table.close(table.context) == S_FALSE, "close is idempotent");
    check(table.beginFrame(table.context, &next, &lostOutput) == E_INVALIDARG,
          "closed context rejects callbacks");

    EdvrNativeFrameRequest nextRequest{
        sizeof(nextRequest), EDVR_NATIVE_FRAME_VERSION_1, device.Get(), 42};
    EdvrNativeFrameTable nextTable{sizeof(nextTable), EDVR_NATIVE_FRAME_VERSION_1};
    check(edvrAcquireNativeFrame(&nextRequest, &nextTable) == S_OK &&
              nextTable.context && nextTable.beginFrame && nextTable.close,
          "a later provider generation can acquire while the Performance state remains selected");
    const uint32_t notesBeforeGeneration = countTrimNotes(g_trimPolicyCounts);
    EdvrNativeFrameOutput generationOutput{sizeof(generationOutput),
                                           EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput generationFrame = input(42, 8, 1);
    check(nextTable.beginFrame(nextTable.context, &generationFrame,
                               &generationOutput) == S_OK &&
              g_trimPolicyCounts.creates == 1 && g_trimPolicyCounts.resolves > 1 &&
              countTrimNotes(g_trimPolicyCounts) == notesBeforeGeneration,
          "the real policy cache and note cadence persist across provider close/reacquire");

    check(setNativeFrameFovTrimSettingsPolicyOpsV1(nullptr) &&
              g_trimPolicyCounts.destroys == 1,
          "explicit absence destroys the one selected opaque module state");
    EdvrNativeFrameOutput absentAgain{sizeof(absentAgain),
                                      EDVR_NATIVE_FRAME_VERSION_5};
    EdvrNativeFrameInput absentAgainFrame = input(42, 8, 2);
    const unsigned resolvesBeforeAbsent = g_trimPolicyCounts.resolves;
    check(nextTable.beginFrame(nextTable.context, &absentAgainFrame,
                               &absentAgain) == S_OK &&
              absentAgain.trimVerticalDeg == 0.0f && absentAgain.trimOuterDeg == 0.0f &&
              absentAgain.trimNasalDeg == 0.0f &&
              g_trimPolicyCounts.resolves == resolvesBeforeAbsent &&
              g_trimPolicyCounts.destroys == 1,
          "absent selection after replacement does no callback and writes the zero triple");
    uint32_t absentAgainAccessor[3] = {99, 99, 99};
    edvr::nativeFrameFovTrimDegrees(absentAgainAccessor);
    check(absentAgainAccessor[0] == 0 && absentAgainAccessor[1] == 0 &&
              absentAgainAccessor[2] == 0,
          "the in-DLL trim accessor clears when the selected policy is removed");
    check(setNativeFrameFovTrimSettingsPolicyOpsV1(nullptr) &&
              g_trimPolicyCounts.destroys == 1,
          "repeated absence does not destroy the same module state twice");

    check(setNativeFrameFovTrimSettingsPolicyOpsV1(&failedTrimOps),
          "install a valid test policy whose module-state allocator fails");
    EdvrNativeFrameOutput allocationFailure{sizeof(allocationFailure),
                                             EDVR_NATIVE_FRAME_VERSION_5};
    allocationFailure.trimVerticalDeg = -41.0f;
    allocationFailure.trimOuterDeg = -42.0f;
    allocationFailure.trimNasalDeg = -43.0f;
    EdvrNativeFrameInput allocationFailureFrame = input(42, 8, 3);
    const unsigned resolvesBeforeFailure = g_trimPolicyCounts.resolves;
    check(nextTable.beginFrame(nextTable.context, &allocationFailureFrame,
                               &allocationFailure) == E_OUTOFMEMORY &&
              allocationFailure.trimVerticalDeg == -41.0f &&
              allocationFailure.trimOuterDeg == -42.0f &&
              allocationFailure.trimNasalDeg == -43.0f &&
              g_trimPolicyCounts.resolves == resolvesBeforeFailure,
          "module-state allocation failure returns before callback and leaves caller output untouched");
    check(setNativeFrameFovTrimSettingsPolicyOpsV1(nullptr) &&
              g_trimPolicyCounts.destroys == 1,
          "failed allocation has no opaque state to destroy");
    check(nextTable.close(nextTable.context) == S_OK,
          "later provider generation closes cleanly after absence and allocation refusal");

    // The transition key remains enabled in memory while the cached Comfort
    // owner is absent. The real provider must still answer core frame data,
    // publish transition disabled, and leave Comfort's submit channels alone.
    auto nativeFrameComfortOwnerDenied = [&] {
    const std::string savedTransition =
        edvr::Config::get().getString("fix.transition_flash", "1");
    edvr::Config::get().set("fix.transition_flash", "1");
    g_comfortOwnerAllowed = false;
    EdvrNativeFrameRequest deniedComfortRequest{
        sizeof(deniedComfortRequest), EDVR_NATIVE_FRAME_VERSION_1,
        device.Get(), 43};
    EdvrNativeFrameTable deniedComfortTable{
        sizeof(deniedComfortTable), EDVR_NATIVE_FRAME_VERSION_1};
    const HRESULT deniedComfortAcquire =
        edvrAcquireNativeFrame(&deniedComfortRequest, &deniedComfortTable);
    check(deniedComfortAcquire == S_OK && deniedComfortTable.context &&
              deniedComfortTable.beginFrame && deniedComfortTable.latchSubmit &&
              deniedComfortTable.close,
          "Comfort absence keeps the native-frame ABI provider available for core data");
    if (deniedComfortAcquire == S_OK && deniedComfortTable.context) {
        edvr::publishGameDevice(device.Get());
        edvr::announceSceneArrived();
        EdvrNativeFrameInput deniedComfortInput = input(43, 8, 1);
        EdvrNativeFrameOutput deniedComfortOutput{
            sizeof(deniedComfortOutput), EDVR_NATIVE_FRAME_VERSION_5};
        const HRESULT beginDeniedComfort = deniedComfortTable.beginFrame(
            deniedComfortTable.context, &deniedComfortInput,
            &deniedComfortOutput);
        check(beginDeniedComfort == S_OK && deniedComfortOutput.version ==
                  EDVR_NATIVE_FRAME_VERSION_5 &&
                  deniedComfortOutput.size == sizeof(deniedComfortOutput) &&
                  deniedComfortOutput.sceneReady == 1 &&
                  deniedComfortOutput.resubmitEnabled == 1 &&
                  deniedComfortOutput.transitionEnabled == 0,
              "owner-denied native frame keeps scene/core outputs live and publishes transition disabled despite config on");
        edvr::clearGlitchFrame();
        edvr::requestSubmitHold(0);
        edvr::markGlitchFrame();
        edvr::requestSubmitHold(1);
        EdvrNativeFrameDecision deniedComfortDecision{
            sizeof(deniedComfortDecision), EDVR_NATIVE_FRAME_VERSION_1};
        check(deniedComfortTable.latchSubmit(
                  deniedComfortTable.context, 1, &deniedComfortDecision) == S_OK &&
                  deniedComfortDecision.withhold == 0 &&
                  !edvr::glitchConsumerPresent() &&
                  edvr::takeSubmitHoldFrame(),
              "owner-denied native frame does not consume or publish the Comfort submit channels");
        edvr::clearGlitchFrame();
        edvr::requestSubmitHold(0);
        check(deniedComfortTable.close(deniedComfortTable.context) == S_OK,
              "owner-denied native-frame session closes cleanly");
    }
    g_comfortOwnerAllowed = true;
    edvr::Config::get().set("fix.transition_flash", savedTransition.c_str());
    };
    nativeFrameComfortOwnerDenied();

    // advanced.slow_test_ms's end-frame hold (frame_flag.h, v36): d3d11 -> openvr, 0 is no hold, and the
    // runtime never holds a frame for more than five seconds whatever is asked.
    check(edvr::endFrameHoldMs() == 0, "no end-frame hold until the d3d11 half asks for one");
    edvr::requestEndFrameHold(80);
    check(edvr::endFrameHoldMs() == 80, "the end-frame hold carries its milliseconds");
    edvr::requestEndFrameHold(60000);
    check(edvr::endFrameHoldMs() == 5000, "a hold over five seconds is clamped to five");
    edvr::requestEndFrameHold(0);
    check(edvr::endFrameHoldMs() == 0, "and is withdrawn by asking for 0");

    // frame_flag's layout check (the roll-call, since v34). Last, because a refusal it provokes
    // is meant to outlast it. This process holds one half, so the roll-call
    // has one signature and there is nothing to name...
    check(edvr::kFrameFlagVersion == 37, "frame_flag layout is v37");
    check(edvr::frameFlagPeerMismatch() == 0, "one half alone is no mismatch");
    {
        wchar_t name[64];
        swprintf_s(name, L"Local\\edvr_frame_flag_rollcall_%lu", GetCurrentProcessId());
        HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        check(h != nullptr, "the roll-call is signed at the channel's first use");
        auto* roll = h ? static_cast<volatile LONG*>(
                             MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 3 * sizeof(LONG)))
                       : nullptr;
        // Relative to kFrameFlagVersion, so only the pin above moves when the
        // layout does; the roll-call's own behaviour does not change with it.
        const LONG ours = static_cast<LONG>(edvr::kFrameFlagVersion);
        check(roll && roll[0] == ours && roll[1] == 0 && roll[2] == 1,
              "one signature, this layout first, nobody else");
        if (roll) {
            // ...a half on another layout signing second is named at once,
            // and the channel refuses: a mark reads back as absent...
            edvr::clearGlitchFrame();
            InterlockedExchange(&roll[1], ours + 1);
            check(edvr::frameFlagPeerMismatch() == static_cast<uint32_t>(ours + 1),
                  "a half on the next layout signing second is named");
            edvr::markGlitchFrame();
            check(!edvr::glitchFrameMarked(), "the refused channel reads as absent");
            InterlockedExchange(&roll[1], 0);
            check(edvr::frameFlagPeerMismatch() == 0, "the roll-call refusal follows the roll-call");
            edvr::markGlitchFrame();
            check(edvr::glitchFrameMarked(), "and the channel carries again without it");
            edvr::clearGlitchFrame();
            UnmapViewOfFile(const_cast<LONG*>(roll));
        }
        if (h) CloseHandle(h);
    }
    {
        // ...and a half built before the roll-call (v33, which never signs)
        // is found by its block's name, and refused for good.
        wchar_t name[64];
        swprintf_s(name, L"Local\\edvr_glitch_frame_v33_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name);
        check(h != nullptr, "a v33 block in the process");
        check(edvr::frameFlagPeerMismatch() == 33, "an unsigned v33 half is named");
        edvr::markGlitchFrame();
        check(!edvr::glitchFrameMarked(), "and the channel stays refused");
        if (h) CloseHandle(h);
        check(edvr::frameFlagPeerMismatch() == 33, "a found v33 half is latched");
    }

    std::printf("native_frame_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
