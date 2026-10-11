#include "../../src/d3d11/plugin_dispatch.h"
#include "../../src/d3d11/plugin_registry.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/plugins/comfort/exposure/exposure_dispatch.h"
#include "../../src/plugins/comfort/exposure/exposure_lifecycle.h"
#include "../../src/plugins/comfort/scanners/scanner_lifecycle.h"
#include "../../src/plugins/comfort/comfort_lifecycle.h"
#include "../../src/d3d11/comfort_owner_gate.h"
#include "../../src/plugins/explorer_cam/explorer_cam_lifecycle.h"
#include "../../src/d3d11/fss_panel.h"
#include "../../src/d3d11/fss_reveal.h"
#include "../../src/d3d11/explorer_cam.h"
#include "../../src/d3d11/transition_flash_eye_base.h"
#include "../../src/common/config.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/log.h"

#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <windows.h>

namespace edvr {
uint32_t loggerNoteCalls = 0;

enum LifecycleModuleEvent : uint32_t {
    kScannerPanelConfigure = 1,
    kScannerRevealConfigure,
    kScannerRevealFrame,
    kScannerPanelShutdown,
    kScannerRectanglesShutdown,
    kScannerRevealShutdown,
    kComfortFlashConfigure,
    kComfortFlashFrame,
    kComfortExplorerFrame,
    kComfortExplorerShutdown,
    kComfortFlashShutdown,
};
struct LifecycleModuleTrace {
    LifecycleModuleEvent events[32]{};
    uint32_t count = 0;
    uint32_t frameNo = 0;
    Config* config = nullptr;
    bool scannerDetachedAtRevealShutdown = false;
    bool comfortDetachedAtFlashShutdown = false;
};
LifecycleModuleTrace lifecycleModuleTrace;

void noteLifecycleModule(LifecycleModuleEvent event) {
    if (lifecycleModuleTrace.count <
        sizeof(lifecycleModuleTrace.events) / sizeof(lifecycleModuleTrace.events[0]))
        lifecycleModuleTrace.events[lifecycleModuleTrace.count++] = event;
}

void fssPanelConfigure(Config& cfg) {
    lifecycleModuleTrace.config = &cfg;
    noteLifecycleModule(kScannerPanelConfigure);
}
void fssRevealConfigure(Config& cfg) {
    lifecycleModuleTrace.config = &cfg;
    noteLifecycleModule(kScannerRevealConfigure);
}
void fssRevealFrameBoundary() {
    noteLifecycleModule(kScannerRevealFrame);
}
void fssPanelShutdown() {
    noteLifecycleModule(kScannerPanelShutdown);
}
void fssRevealShutdown() {
    lifecycleModuleTrace.scannerDetachedAtRevealShutdown =
        !pluginRegistryHasLifecycleComponent(
            plugins::kPluginComfort, kComfortScannersLifecycleComponent);
    noteLifecycleModule(kScannerRevealShutdown);
}
void transitionFlashEyeBaseConfigure(Config& cfg) {
    lifecycleModuleTrace.config = &cfg;
    noteLifecycleModule(kComfortFlashConfigure);
}
void transitionFlashEyeBaseFrameBoundary(uint32_t frameNo) {
    lifecycleModuleTrace.frameNo = frameNo;
    noteLifecycleModule(kComfortFlashFrame);
}
void transitionFlashEyeBaseShutdown() {
    lifecycleModuleTrace.comfortDetachedAtFlashShutdown =
        !pluginRegistryHasLifecycleComponent(
            plugins::kPluginComfort, kComfortTransitionFlashLifecycleComponent);
    noteLifecycleModule(kComfortFlashShutdown);
}
void explorerCamFrameBoundary(uint32_t frameNo) {
    lifecycleModuleTrace.frameNo = frameNo;
    noteLifecycleModule(kComfortExplorerFrame);
}
void explorerCamShutdown() {
    noteLifecycleModule(kComfortExplorerShutdown);
}
}  // namespace edvr

namespace {
constexpr uint32_t kPlugin = edvr::plugins::kPluginCockpitVisuals;
constexpr uint32_t kNightVisionClaim = 0x1001u;
constexpr uint32_t kEarlierClaim = 0x2001u;
constexpr uint64_t kVs = 0xFCF7BD2896751D96ull;
constexpr uint64_t kPs = 0xF786D34B5E118D5Eull;
constexpr uint64_t kActive = uint64_t{1} << kPlugin;
constexpr uint64_t kLegacyVs = 0x1122334455667788ull;
constexpr auto kManifestNightVisionClaim = edvr::plugins::kManifest[kPlugin].claims[
    edvr::plugins::kClaimCockpitVisualsNightVision];
constexpr edvr::plugins::dispatch::ShaderClaimKey kClaims[] = {
    {kPlugin, kManifestNightVisionClaim.id,
     kManifestNightVisionClaim.shaderPairs[0].vertexShaderHash,
     kManifestNightVisionClaim.shaderPairs[0].pixelShaderHash},
};
uint32_t checks = 0;
float exposureDampingConfig = 0.0f;
float exposureTauConfig = 45.0f;
uint32_t fakeExposureReleases = 0;

struct FakeTexture final {
    const struct FakeTextureVtable* vtable;
};
ULONG STDMETHODCALLTYPE fakeTextureRelease(IUnknown*) {
    ++fakeExposureReleases;
    return 0;
}
struct FakeTextureVtable final {
    HRESULT (STDMETHODCALLTYPE* query)(IUnknown*, REFIID, void**);
    ULONG (STDMETHODCALLTYPE* addRef)(IUnknown*);
    ULONG (STDMETHODCALLTYPE* release)(IUnknown*);
};
const FakeTextureVtable kFakeTextureVtable{nullptr, nullptr,
                                           &fakeTextureRelease};

// Frozen public prefix from before trace-only callbacks were appended.
struct LegacyEdvrPluginOpsPrefix final {
    uint32_t structSize;
    uint32_t manifestIndex;
    const char* manifestId;
    const char* drawGateName;
    const char* const* claimIds;
    uint32_t claimCount;
    void* state;
    EdvrPluginConfigureFn configure;
    EdvrPluginWantsDrawsFn wantsDraws;
    EdvrPluginStartupHooksWantedFn startupHooksWanted;
    EdvrPluginClaimDrawFn claimDraw;
    EdvrPluginDrawFn begin;
    EdvrPluginDrawFn end;
    EdvrPluginShutdownFn shutdown;
};
static_assert(std::is_standard_layout<EdvrPluginOps>::value,
              "plugin ops ABI must remain standard-layout");
static_assert(sizeof(LegacyEdvrPluginOpsPrefix) ==
                  offsetof(EdvrPluginOps, claimDrawObserved),
              "trace-only callbacks must append after the exact legacy ops prefix");
static_assert(offsetof(EdvrPluginOps, shutdown) ==
                  offsetof(LegacyEdvrPluginOpsPrefix, shutdown),
              "legacy shutdown callback offset must remain stable");
static_assert(std::is_standard_layout<EdvrPluginLifecycleOps>::value,
              "lifecycle ops ABI must remain standard-layout");
struct LegacyEdvrPluginLifecycleOpsPrefix final {
    uint32_t structSize;
    uint32_t manifestIndex;
    const char* manifestId;
    void* state;
    EdvrPluginConfigureFn configure;
    EdvrPluginFrameFn frame;
    EdvrPluginShutdownFn shutdown;
};
struct ConfigureStageLifecycleOpsPrefix final {
    LegacyEdvrPluginLifecycleOpsPrefix legacy;
    EdvrPluginConfigureStageFn configureStage;
};
struct FrameStageLifecycleOpsPrefix final {
    LegacyEdvrPluginLifecycleOpsPrefix legacy;
    EdvrPluginConfigureStageFn configureStage;
    EdvrPluginFrameStageFn frameStage;
};
struct ShutdownStageLifecycleOpsPrefix final {
    LegacyEdvrPluginLifecycleOpsPrefix legacy;
    EdvrPluginConfigureStageFn configureStage;
    EdvrPluginFrameStageFn frameStage;
    EdvrPluginShutdownStageFn shutdownStage;
};
struct ConfigureStateLifecycleOpsPrefix final {
    ShutdownStageLifecycleOpsPrefix staged;
    EdvrPluginConfigureStateFn configureState;
};
static_assert(sizeof(LegacyEdvrPluginLifecycleOpsPrefix) ==
                  offsetof(EdvrPluginLifecycleOps, configureStage),
              "staged lifecycle fields must append after the exact legacy prefix");
static_assert(sizeof(ConfigureStageLifecycleOpsPrefix) ==
                  offsetof(EdvrPluginLifecycleOps, frameStage),
              "configure-only fixture must end at the frameStage boundary");
static_assert(sizeof(FrameStageLifecycleOpsPrefix) ==
                  offsetof(EdvrPluginLifecycleOps, shutdownStage),
              "frame-stage fixture must end at the shutdownStage boundary");
static_assert(sizeof(ShutdownStageLifecycleOpsPrefix) ==
                  offsetof(EdvrPluginLifecycleOps, configureState),
              "configureState must append after the staged lifecycle ABI");
static_assert(sizeof(ConfigureStateLifecycleOpsPrefix) ==
                  offsetof(EdvrPluginLifecycleOps, componentIndex),
              "legacy full lifecycle record must end before componentIndex");
static_assert(offsetof(EdvrPluginLifecycleOps, componentIndex) ==
                  sizeof(ConfigureStateLifecycleOpsPrefix),
              "componentIndex must append after the complete pre-component ABI");
static_assert(offsetof(EdvrPluginLifecycleOps, configure) ==
                  offsetof(LegacyEdvrPluginLifecycleOpsPrefix, configure) &&
              offsetof(EdvrPluginLifecycleOps, frame) ==
                  offsetof(LegacyEdvrPluginLifecycleOpsPrefix, frame) &&
              offsetof(EdvrPluginLifecycleOps, shutdown) ==
                  offsetof(LegacyEdvrPluginLifecycleOpsPrefix, shutdown),
              "legacy lifecycle callback offsets must remain stable");

struct RegistryState {
    bool wants = false;
    bool legacyWants = false;
    uint32_t configureCalls = 0;
    uint32_t claimCalls = 0;
    uint32_t beginCalls = 0;
    uint32_t endCalls = 0;
    uint32_t shutdownCalls = 0;
    uint32_t observedCalls = 0;
    uint8_t traceMode = 3;
    bool traceModeAvailable = true;
    bool failed = false;
};

struct LifecycleState {
    uint32_t configureCalls = 0;
    uint32_t frameCalls = 0;
    uint32_t shutdownCalls = 0;
    uint32_t lastFrame = 0;
    bool configured = false;
    bool reenterShutdown = false;
    uint32_t manifestIndex = 0;
    uint32_t configureStageCalls = 0;
    uint32_t configureStateCalls = 0;
    uint32_t frameStageCalls = 0;
    uint32_t shutdownStageCalls = 0;
    uint32_t lastStage = 0;
    void* lastState = nullptr;
    void* lastConfig = nullptr;
    void* lastStateConfig = nullptr;
    ID3D11DeviceContext* lastContext = nullptr;
    uint32_t lastStageSceneFrame = 0;
};
LifecycleState lifecycleState;
RegistryState registryState;
const char* const kRegistryClaimIds[] = {"night-vision"};
const char* const kNullRegistryClaimIds[] = {nullptr};
const char* const kDuplicateRegistryClaimIds[] = {"night-vision", "night-vision"};

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        std::exit(1);
    }
}

uint32_t registryWants(void* state) {
    return static_cast<RegistryState*>(state)->wants ? 1u : 0u;
}
uint32_t registryStartupHooksWanted(void* config) {
    return config && *static_cast<const bool*>(config) ? 1u : 0u;
}
void registryConfigure(void* config) {
    ++registryState.configureCalls;
    registryState.wants = config && *static_cast<const bool*>(config);
}
uint32_t registryClaim(void* state, const char* claimId, uint8_t kind,
                       uint32_t count, uint32_t instances) {
    auto* s = static_cast<RegistryState*>(state);
    ++s->claimCalls;
    return claimId && std::strcmp(claimId, "night-vision") == 0 &&
                   edvr::plugins::dispatch::matchesShape(
                       kManifestNightVisionClaim.drawShape, kind, count, instances)
               ? kNightVisionClaim : 0;
}
void registryBegin(void* state, const char* claimId, ID3D11DeviceContext*) {
    if (claimId && std::strcmp(claimId, "night-vision") == 0)
        ++static_cast<RegistryState*>(state)->beginCalls;
}
void registryEnd(void* state, const char* claimId, ID3D11DeviceContext*) {
    if (claimId && std::strcmp(claimId, "night-vision") == 0)
        ++static_cast<RegistryState*>(state)->endCalls;
}
void registryShutdown(void* state) {
    ++static_cast<RegistryState*>(state)->shutdownCalls;
}
uint32_t registryClaimObserved(void* state, const char* claimId, uint8_t kind,
                               uint32_t count, uint32_t instances,
                               EdvrPluginClaimObservation* observation) {
    auto* s = static_cast<RegistryState*>(state);
    ++s->observedCalls;
    if (observation) {
        observation->mode = s->traceMode;
        observation->failedKnown = s->traceMode ? 1u : 0u;
        observation->failed = s->traceMode && s->failed ? 1u : 0u;
    }
    return registryClaim(state, claimId, kind, count, instances);
}
uint32_t registryTraceMode(void* state, uint8_t* mode) {
    auto* s = static_cast<RegistryState*>(state);
    if (!mode || !s->traceModeAvailable) return 0;
    *mode = s->traceMode;
    return 1;
}
uint32_t legacyGate(void* state) {
    return static_cast<RegistryState*>(state)->legacyWants ? 1u : 0u;
}

EdvrPluginOps registryOps() {
    return {sizeof(EdvrPluginOps), edvr::plugins::kPluginCockpitVisuals,
            "cockpit-visuals", "test.cockpit-visuals", kRegistryClaimIds, 1,
            &registryState, &registryConfigure, &registryWants,
            &registryStartupHooksWanted, &registryClaim,
            &registryBegin, &registryEnd, &registryShutdown,
            &registryClaimObserved, &registryTraceMode};
}

void lifecycleConfigure(void* config) {
    ++lifecycleState.configureCalls;
    lifecycleState.configured = config && *static_cast<const bool*>(config);
}
void lifecycleFrame(void* state, uint32_t sceneFrame) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->frameCalls;
    s->lastFrame = sceneFrame;
}
void lifecycleShutdown(void* state) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->shutdownCalls;
    if (s->reenterShutdown)
        edvr::pluginRegistryShutdownLifecycle(s->manifestIndex);
}
void lifecycleConfigureStage(void* state, uint32_t stage, void* config) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->configureStageCalls;
    s->lastState = state;
    s->lastStage = stage;
    s->lastConfig = config;
}
void lifecycleConfigureState(void* state, void* config) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->configureStateCalls;
    s->lastState = state;
    s->lastStateConfig = config;
}
void lifecycleFrameStage(void* state, uint32_t stage,
                         ID3D11DeviceContext* context, uint32_t sceneFrame) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->frameStageCalls;
    s->lastState = state;
    s->lastStage = stage;
    s->lastContext = context;
    s->lastStageSceneFrame = sceneFrame;
}
void lifecycleShutdownStage(void* state, uint32_t stage) {
    auto* s = static_cast<LifecycleState*>(state);
    ++s->shutdownStageCalls;
    s->lastState = state;
    s->lastStage = stage;
}
EdvrPluginLifecycleOps introLifecycleOps() {
    return {sizeof(EdvrPluginLifecycleOps), edvr::plugins::kPluginIntro,
            "intro", &lifecycleState, &lifecycleConfigure, &lifecycleFrame,
            &lifecycleShutdown, &lifecycleConfigureStage,
            &lifecycleFrameStage, &lifecycleShutdownStage};
}

bool oldShape(char kind, uint32_t count, uint32_t instances) {
    return kind == 'X' && count == 240 && instances == 1;
}

// Frozen pre-registry behavior at the old night-vision rung. An earlier
// verdict preempts this rung because beginPanelOverride returns at its first
// claim; otherwise the old predicate checked enabled/failure/shape/hash.
uint32_t legacyDecision(uint32_t earlierClaim, char kind, uint32_t count,
                        uint32_t instances, uint64_t vs, uint64_t ps,
                        bool pulse, bool realistic, bool failed) {
    if (earlierClaim) return earlierClaim;
    return (pulse || realistic) && !failed && oldShape(kind, count, instances) &&
                   vs == kVs && ps == kPs
               ? kNightVisionClaim
               : 0;
}

uint32_t tableDecision(uint32_t earlierClaim, char kind, uint32_t count,
                       uint32_t instances, uint64_t candidates,
                       bool pulse, bool realistic, bool failed,
                       uint32_t* shapeCallbacks, uint32_t* claimCallbacks) {
    if (earlierClaim) return earlierClaim;
    return edvr::plugins::dispatch::resolveCandidate(
        candidates, kPlugin, [&]() {
            ++*shapeCallbacks;
            return edvr::plugins::dispatch::matchesShape(
                kManifestNightVisionClaim.drawShape, static_cast<uint8_t>(kind), count, instances);
        }, [&]() {
            ++*claimCallbacks;
            return (pulse || realistic) && !failed ? kNightVisionClaim : 0;
        });
}

void replayMatrix() {
    const char kinds[] = {'X', 'D'};
    const uint32_t counts[] = {240, 6};
    const uint32_t instances[] = {1, 2};
    const uint64_t shaders[][2] = {{kVs, kPs}, {kVs, 0}, {0, kPs}};
    for (bool pulse : {false, true})
        for (bool realistic : {false, true})
            for (bool failed : {false, true})
                for (size_t shape = 0; shape < 2; ++shape)
                    for (const auto& pair : shaders)
                        for (uint32_t earlier : {0u, kEarlierClaim}) {
                            size_t bindChecks = 0;
                            uint32_t shapeCallbacks = 0;
                            uint32_t callbacks = 0;
                            const uint64_t active = (pulse || realistic) ? kActive : 0;
                            const auto candidateMask = edvr::plugins::dispatch::candidatePlugins(
                                pair[0], pair[1], active, kClaims,
                                sizeof(kClaims) / sizeof(kClaims[0]), &bindChecks);
                            const auto old = legacyDecision(earlier, kinds[shape],
                                counts[shape], instances[shape], pair[0], pair[1],
                                pulse, realistic, failed);
                            const auto table = tableDecision(earlier, kinds[shape],
                                counts[shape], instances[shape], candidateMask,
                                pulse, realistic, failed, &shapeCallbacks, &callbacks);
                            check(old == table, "candidate-table replay matches frozen legacy verdict");
                            check(bindChecks == (active ? 1u : 0u),
                                  "shader-pair work occurs only for active plugins at bind/config");
                            const bool reachesCandidate = earlier == 0 && active &&
                                edvr::plugins::dispatch::matchesShape(
                                    kManifestNightVisionClaim.drawShape,
                                    static_cast<uint8_t>(kinds[shape]),
                                    counts[shape], instances[shape]) &&
                                pair[0] == kVs && pair[1] == kPs;
                            check(callbacks == (reachesCandidate ? 1u : 0u),
                                  "per-draw claim callback runs only for a candidate shape");
                            check(shapeCallbacks == (earlier == 0 && active &&
                                  pair[0] == kVs && pair[1] == kPs ? 1u : 0u),
                                  "uninterested/earlier-claimed draws skip shape work");
                        }
}

void costBudgetMetadata() {
    constexpr const char* kExpectedIds[] = {
        "temporal-aa", "cockpit-visuals", "intro", "on-foot-panel",
        "comfort", "explorer-cam", "performance", "diagnostics",
    };
    check(edvr::plugins::kPluginCount == 8 &&
              edvr::plugins::kPluginIndexCount == edvr::plugins::kPluginCount,
          "budget metadata remains attached to the complete eight-group catalog");
    for (uint32_t i = 0; i < edvr::plugins::kPluginCount; ++i) {
        const auto& plugin = edvr::plugins::kManifest[i];
        const auto* budget = plugin.costBudget;
        check(i < sizeof(kExpectedIds) / sizeof(kExpectedIds[0]) &&
                  plugin.id && std::strcmp(plugin.id, kExpectedIds[i]) == 0 &&
                  plugin.costNote && *plugin.costNote && budget &&
                  budget->state == edvr::plugins::kCostBudgetUnmeasured &&
                  budget->reference == nullptr && budget->metrics && budget->metricCount == 3,
              "all plugins retain costNote and an unmeasured, reference-free cost budget");
        if (!budget || !budget->metrics || budget->metricCount != 3) continue;
        constexpr const char* expectedMetrics[] = {"cpu", "issuedGpuWork", "directGpu"};
        bool metricsHonest = true;
        for (uint32_t j = 0; j < 3; ++j) {
            const auto& metric = budget->metrics[j];
            metricsHonest &= metric.id && std::strcmp(metric.id, expectedMetrics[j]) == 0 &&
                metric.state == edvr::plugins::kCostMetricUnmeasured &&
                metric.unit && *metric.unit && metric.coverage && *metric.coverage &&
                metric.referenceValue == nullptr && metric.maxRelativeIncrease == nullptr;
        }
        check(metricsHonest,
              "unmeasured CPU, issued-work, and direct-GPU metrics carry scope without numeric values");
    }
}

void disabledAndCacheCases() {
    size_t pairChecks = 0;
    uint32_t callbacks = 0;
    const uint64_t disabled = edvr::plugins::dispatch::candidatePlugins(
        kVs, kPs, 0, kClaims, sizeof(kClaims) / sizeof(kClaims[0]), &pairChecks);
    const uint32_t result = edvr::plugins::dispatch::resolveCandidate(
        disabled, kPlugin, [&]() { ++callbacks; return true; },
        [&]() { return kNightVisionClaim; });
    check(pairChecks == 0, "disabled plugin has no shader-pair comparison");
    check(disabled == 0 && result == 0 && callbacks == 0,
          "disabled plugin has no candidate and receives no draw callback");

    pairChecks = 0;
    const auto match = edvr::plugins::dispatch::candidatePlugins(
        kVs, kPs, kActive, kClaims, sizeof(kClaims) / sizeof(kClaims[0]), &pairChecks);
    check(edvr::plugins::dispatch::hasCandidate(match, kPlugin),
          "matching bound shader pair is cached as a candidate");
    check(!edvr::plugins::dispatch::hasCandidate(
              edvr::plugins::dispatch::candidatePlugins(kVs, 0, kActive, kClaims,
                  sizeof(kClaims) / sizeof(kClaims[0])), kPlugin),
          "a pixel-shader bind change invalidates the candidate pair");
    uint32_t shapeCalls = 0;
    uint32_t resolverCalls = 0;
    const uint32_t noShape = edvr::plugins::dispatch::resolveCandidate(
        match, kPlugin, [&]() { ++shapeCalls; return false; },
        [&]() { ++resolverCalls; return kNightVisionClaim; });
    check(noShape == 0 && shapeCalls == 1 && resolverCalls == 0,
          "inline shape guard evaluates the shape but skips the registry resolver when it fails");
    check(kManifestNightVisionClaim.drawShape.kind == static_cast<uint8_t>('X') &&
              kManifestNightVisionClaim.drawShape.count == 240 &&
              kManifestNightVisionClaim.drawShape.instances == 1 &&
              kClaims[0].vsHash == kVs && kClaims[0].psHash == kPs,
          "generated claim metadata matches the frozen legacy shader and draw contract");
}

void registryLifecycle() {
    edvr::pluginRegistryShutdown();
    registryState = {};
    uint8_t unregisteredMode = 0xff;
    check(!edvr::pluginRegistryTraceMode(&unregisteredMode) &&
              unregisteredMode == 0xff,
          "unregistered module leaves raw trace mode unavailable");
    EdvrPluginOps ops = registryOps();
    EdvrPluginOps invalid = ops;
    invalid.structSize = 0;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects malformed callback record");
    invalid = ops;
    invalid.claimIds = kNullRegistryClaimIds;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects null claim IDs");
    invalid = ops;
    invalid.claimIds = kDuplicateRegistryClaimIds;
    invalid.claimCount = 2;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects duplicate claim IDs");
    invalid = ops;
    invalid.drawGateName = "";
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects empty gate subscription names");
    invalid = ops;
    invalid.end = nullptr;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects a claimed draw without paired End");
    invalid = ops;
    invalid.startupHooksWanted = nullptr;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects modules without startup hook demand callback");
    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals) &&
              !edvr::pluginRegistryRegister(&ops),
          "VR-only module has no registration or shader interest in flat profile");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(1), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(2), kPs);
    bool unsupportedOn = true;
    check(edvr::pluginRegistryShaderCandidates() == 0 &&
              !edvr::pluginRegistryWantsStartupHooks(&unsupportedOn),
          "flat profile has no candidate mask or startup hook demand");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    check(!edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals),
          "invalid install descriptor disables profile-scoped plugin work");
    edvr::g_runtimeProfile = savedProfile;
    check(edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals),
          "legacy VR profile supports the pilot module");
    check(edvr::pluginRegistryRegister(&ops), "registry accepts only the pilot manifest module");
    bool startupOn = true;
    check(edvr::pluginRegistryWantsStartupHooks(&startupOn),
          "enabled pilot alone requests the core hook installation");
    startupOn = false;
    check(!edvr::pluginRegistryWantsStartupHooks(&startupOn),
          "disabled pilot does not keep otherwise-unused hooks installed");
    check(!edvr::pluginRegistryRegister(&ops), "registry rejects duplicate plugin registration");
    check(edvr::pluginRegistryRegisterLegacyDrawGate("legacy.test", &legacyGate, &registryState),
          "registry accepts a stable named legacy subscription");
    check(!edvr::pluginRegistryRegisterLegacyDrawGate("legacy.test", &legacyGate, &registryState),
          "registry rejects duplicate subscription names");

    bool off = false;
    const uint32_t noteBaseline = edvr::loggerNoteCalls;
    edvr::pluginRegistryConfigure(&off);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(1), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(2), kPs);
    check(edvr::loggerNoteCalls == noteBaseline,
          "off shader binds and configure do not take the logger path");
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "off-period canonical shader binds do not create plugin candidates");
    check(!edvr::pluginRegistryWantsDraws(&registryState),
          "named and plugin subscriptions both report off");
    uint8_t rawMode = 0xff;
    check(edvr::pluginRegistryTraceMode(&rawMode) && rawMode == registryState.traceMode,
          "raw trace mode remains available while the cached candidate mask is empty");
    EdvrPluginClaimObservation noCandidateObservation{};
    noCandidateObservation.mode = 0xff;
    const uint32_t noCandidateObserverCalls = registryState.observedCalls;
    check(edvr::pluginRegistryResolveDrawObserved(0, 'X', 240, 1,
                  &noCandidateObservation) == edvr::kPluginClaimNone &&
              registryState.observedCalls == noCandidateObserverCalls,
          "candidate miss skips the optional observer callback");

    using edvr::draw_interest::InterestId;
    using edvr::draw_interest::InterestMask;
    using edvr::draw_interest::HashFilter;
    constexpr InterestMask kTargetSharp = edvr::draw_interest::bit(InterestId::TargetSharp);
    constexpr InterestMask kIntroCurve = edvr::draw_interest::bit(InterestId::IntroCurveObserve);
    const edvr::draw_interest::ShaderFilter legacyFilters[] = {
        {InterestId::TargetSharp, HashFilter::Vertex, kLegacyVs, 0},
    };
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1),
          "configured VR legacy interest accepts bounded module-owned shader metadata");
    check(edvr::pluginRegistryShaderCandidates() == 0 &&
              edvr::pluginRegistryDrawInterestMask() == kIntroCurve,
          "legacy bind-interest is published separately and known shader mismatch is filtered");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(3), kLegacyVs);
    check(edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "legacy match refreshes on bind while config-only curve interest remains active");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(4), kLegacyVs + 1);
    check(edvr::pluginRegistryDrawInterestMask() == kIntroCurve,
          "known VS mismatch filters only its configured interest");
    edvr::bindingSetShader(edvr::BindSlot::Vs, nullptr, 0);
    check(edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "unknown VS hash conservatively keeps legacy fallback reachable");

    const auto vrProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1) &&
              edvr::pluginRegistryDrawInterestMask() == 0,
          "flat profile does not publish VR-only legacy shader interest");
    edvr::g_runtimeProfile = vrProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1) &&
              edvr::pluginRegistryDrawInterestMask() == 0,
          "invalid runtime profile clears legacy shader interest and observer demand");
    edvr::g_runtimeProfile = vrProfile;
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(5), kLegacyVs);
    check(edvr::pluginRegistryConfigureDrawInterests(
              kTargetSharp | kIntroCurve, legacyFilters, 1) &&
              edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "VR configure reseeds current shader pair without requiring a new bind");
    edvr::bindingForgetAll();
    check(edvr::pluginRegistryShaderCandidates() == 0 &&
              edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "ClearState removes plugin candidates while unknown legacy pair keeps fallback");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(6), kLegacyVs);
    check(edvr::pluginRegistryDrawInterestMask() == (kTargetSharp | kIntroCurve),
          "canonical shader repair republishes the matching legacy interest");
    check(edvr::pluginRegistryConfigureDrawInterests(0, nullptr, 0) &&
              edvr::pluginRegistryDrawInterestMask() == 0,
          "disabling legacy config interest clears the cached mask");

    // Exercise plugin seeding independently after the legacy observer has been
    // removed and the canonical shadow changes quietly.
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(7), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(8), kPs);
    registryState.legacyWants = true;
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "named legacy subscriber opens the gate independently of the plugin");
    registryState.legacyWants = false;
    bool on = true;
    edvr::pluginRegistryConfigure(&on);
    registryState.wants = false;
    edvr::pluginRegistryRefreshShaderCandidates();
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "refresh recomputes active plugin interest instead of using a stale mask");
    registryState.wants = true;
    edvr::pluginRegistryRefreshShaderCandidates();
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "plugin subscription opens the gate independently of legacy consumers");
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "config-on seeds a candidate from the already-bound canonical pair");
    const uint64_t candidates = edvr::pluginRegistryShaderCandidates();
    EdvrPluginClaimObservation observation{};
    const auto claim = edvr::plugins::dispatch::resolveCandidate(
        candidates, edvr::plugins::kPluginCockpitVisuals, [&] {
            return edvr::plugins::dispatch::matchesShape(
                kManifestNightVisionClaim.drawShape, 'X', 240, 1);
        }, [&] {
            return edvr::pluginRegistryResolveDrawObserved(
                candidates, 'X', 240, 1, &observation);
        });
    check(claim == kNightVisionClaim && registryState.claimCalls == 1 &&
              registryState.observedCalls == 1,
          "positive candidate invokes the observed claim callback exactly once");
    check(observation.observed == 1 && observation.mode == registryState.traceMode &&
              observation.failedKnown == 1 && observation.failed == 0,
          "registry marks a consumed successful observation as known source facts");
    check(edvr::loggerNoteCalls == noteBaseline,
          "candidate shader bind and draw resolution only queue breadcrumbs");
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "frame-boundary activity report drains both one-shot breadcrumbs");
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "drained activity breadcrumbs do not log twice");
    (void)edvr::plugins::dispatch::resolveCandidate(
        candidates, edvr::plugins::kPluginCockpitVisuals, [] { return true; },
        [&] { return edvr::pluginRegistryResolveDraw(candidates, 'X', 240, 1); });
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "later candidate draws do not re-enter the logger");
    edvr::pluginRegistryBegin(claim, nullptr);
    edvr::pluginRegistryEnd(claim, nullptr);
    check(registryState.beginCalls == 1 && registryState.endCalls == 1,
          "registry routes matched begin and end callbacks to the owning module");

    edvr::bindingForgetAll();
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "canonical ClearState/unbind observer drops the cached shader candidate");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(3), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(4), kPs);
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "active canonical shader repair republishes a matching candidate");
    edvr::pluginRegistrySetDrawGateFailOpen();
    off = false;
    edvr::pluginRegistryConfigure(&off);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(5), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(6), kPs);
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "configure-off removes the observer while shadow tracking continues");
    edvr::pluginRegistryConfigure(&on);
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "configure-on reseeds after silent off-period shader changes");
    off = false;
    edvr::pluginRegistryConfigure(&off);
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "registration errors can force the aggregate gate open");
    edvr::pluginRegistryShutdown();
    check(registryState.shutdownCalls == 1 &&
              edvr::pluginRegistryShaderCandidates() == 0 &&
              !edvr::pluginRegistryWantsDraws(&registryState),
          "shutdown releases modules and clears dispatch/subscription caches");
    check(edvr::pluginRegistryRegister(&ops), "registry can restart after shutdown");
    const uint32_t shutdownNoteBaseline = edvr::loggerNoteCalls;
    edvr::pluginRegistryConfigure(&on);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(7), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(8), kPs);
    const uint64_t shutdownCandidates = edvr::pluginRegistryShaderCandidates();
    (void)edvr::plugins::dispatch::resolveCandidate(
        shutdownCandidates, edvr::plugins::kPluginCockpitVisuals,
        [] { return true; }, [&] {
            return edvr::pluginRegistryResolveDraw(shutdownCandidates, 'X', 240, 1);
        });
    check(edvr::loggerNoteCalls == shutdownNoteBaseline,
          "pending draw activity remains deferred until a safe reporting point");
    edvr::pluginRegistryShutdown();
    check(edvr::loggerNoteCalls == shutdownNoteBaseline + 2,
          "shutdown drains pending bind and draw breadcrumbs away from draw dispatch");

    // A legacy-prefix ops record remains valid when the optional observation
    // tail is absent. The normal claim may still resolve, but its source facts
    // remain explicitly unsupported rather than being fabricated.
    registryState = {};
    EdvrPluginOps noObserver = registryOps();
    noObserver.claimDrawObserved = nullptr;
    noObserver.traceMode = nullptr;
    check(edvr::pluginRegistryRegister(&noObserver),
          "registry accepts ops with optional observation callbacks absent");
    bool enabled = true;
    edvr::pluginRegistryConfigure(&enabled);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(9), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(10), kPs);
    uint8_t unsupportedMode = 0xff;
    check(!edvr::pluginRegistryTraceMode(&unsupportedMode) && unsupportedMode == 0xff,
          "missing trace-mode callback reports unavailable without fabricating a mode");
    const uint64_t fallbackCandidates = edvr::pluginRegistryShaderCandidates();
    EdvrPluginClaimObservation unsupportedObservation{};
    const uint32_t fallbackClaim = edvr::pluginRegistryResolveDrawObserved(
        fallbackCandidates, 'X', 240, 1, &unsupportedObservation);
    check(fallbackClaim == kNightVisionClaim && registryState.claimCalls == 1 &&
              registryState.observedCalls == 0,
          "missing observer falls back to the ordinary claim callback");
    check(unsupportedObservation.observed == 0 && unsupportedObservation.mode == 0 &&
              unsupportedObservation.failedKnown == 0 && unsupportedObservation.failed == 0,
          "ordinary fallback leaves trace-only observation explicitly unknown");
    edvr::pluginRegistryShutdown();
}

void lifecycleRegistryDispatch() {
    constexpr uint32_t kIntro = edvr::plugins::kPluginIntro;
    constexpr uint32_t kExposure = edvr::plugins::kPluginComfort;
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;
    registryState.legacyWants = false;
    check(edvr::pluginRegistryRegisterLegacyDrawGate(
              "lifecycle.test", &legacyGate, &registryState),
          "draw subscription fixture is registered before lifecycle dispatch");
    const bool drawSubscriptionBefore = edvr::pluginRegistryWantsDraws(&registryState);
    const uint64_t candidatesBefore = edvr::pluginRegistryShaderCandidates();
    const auto interestsBefore = edvr::pluginRegistryDrawInterestMask();
    EdvrPluginLifecycleOps ops = introLifecycleOps();

    // Place each declared ABI prefix immediately before PAGE_NOACCESS. A read
    // of any callback beyond structSize then faults instead of finding stack
    // padding that happens to be readable.
    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;
    SYSTEM_INFO pageInfo{};
    GetSystemInfo(&pageInfo);
    auto* guardPages = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, static_cast<SIZE_T>(pageInfo.dwPageSize) * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(guardPages != nullptr, "guard-page lifecycle fixture allocates storage");
    DWORD previousProtect = 0;
    check(VirtualProtect(guardPages + pageInfo.dwPageSize, pageInfo.dwPageSize,
                         PAGE_NOACCESS, &previousProtect) != 0,
          "guard-page lifecycle fixture protects the following page");
    void* const pageEnd = guardPages + pageInfo.dwPageSize;
    auto* const oldPrefix = new (static_cast<uint8_t*>(pageEnd) -
                                 sizeof(LegacyEdvrPluginLifecycleOpsPrefix))
        LegacyEdvrPluginLifecycleOpsPrefix{
            sizeof(LegacyEdvrPluginLifecycleOpsPrefix), kIntro, "intro",
            &lifecycleState, &lifecycleConfigure, &lifecycleFrame,
            &lifecycleShutdown};
    const auto* oldOps = reinterpret_cast<const EdvrPluginLifecycleOps*>(oldPrefix);
    check(edvr::pluginRegistryRegisterLifecycle(oldOps),
          "lifecycle registry accepts the original callback-prefix size");
    check(!edvr::pluginRegistryConfigureLifecycleStage(kIntro, 91, nullptr) &&
              !edvr::pluginRegistryFrameLifecycleStage(kIntro, 92, nullptr, 1) &&
              !edvr::pluginRegistryShutdownLifecycleStage(kIntro, 93) &&
              lifecycleState.configureStageCalls == 0 &&
              lifecycleState.frameStageCalls == 0 &&
              lifecycleState.shutdownStageCalls == 0,
          "old prefix record reports absent staged callbacks without reading appended storage");
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    check(!edvr::pluginRegistryHasLifecycle(kIntro),
          "old prefix lifecycle record can be detached normally");

    auto* const configurePrefix = new (static_cast<uint8_t*>(pageEnd) -
                                       sizeof(ConfigureStageLifecycleOpsPrefix))
        ConfigureStageLifecycleOpsPrefix{
            {offsetof(EdvrPluginLifecycleOps, frameStage), kIntro, "intro",
             &lifecycleState, &lifecycleConfigure, &lifecycleFrame,
             &lifecycleShutdown},
            &lifecycleConfigureStage};
    const auto* configureOnlyOps = reinterpret_cast<
        const EdvrPluginLifecycleOps*>(configurePrefix);
    check(edvr::pluginRegistryRegisterLifecycle(configureOnlyOps),
          "configureStage-only boundary record registers at its declared size");
    check(edvr::pluginRegistryConfigureLifecycleStage(kIntro, 94, nullptr) &&
              !edvr::pluginRegistryFrameLifecycleStage(kIntro, 95, nullptr, 0) &&
              !edvr::pluginRegistryShutdownLifecycleStage(kIntro, 96) &&
              lifecycleState.configureStageCalls == 1 &&
              lifecycleState.frameStageCalls == 0 &&
              lifecycleState.shutdownStageCalls == 0,
          "configureStage-only page-boundary record never reads later callbacks");
    edvr::pluginRegistryShutdownLifecycle(kIntro);

    auto* const framePrefix = new (static_cast<uint8_t*>(pageEnd) -
                                   sizeof(FrameStageLifecycleOpsPrefix))
        FrameStageLifecycleOpsPrefix{
            {offsetof(EdvrPluginLifecycleOps, shutdownStage), kIntro, "intro",
             &lifecycleState, &lifecycleConfigure, &lifecycleFrame,
             &lifecycleShutdown},
            nullptr, &lifecycleFrameStage};
    const auto* frameOnlyOps = reinterpret_cast<const EdvrPluginLifecycleOps*>(
        framePrefix);
    check(edvr::pluginRegistryRegisterLifecycle(frameOnlyOps),
          "frameStage boundary record registers at its declared size");
    check(!edvr::pluginRegistryConfigureLifecycleStage(kIntro, 97, nullptr) &&
              edvr::pluginRegistryFrameLifecycleStage(kIntro, 98, nullptr, 99) &&
              !edvr::pluginRegistryShutdownLifecycleStage(kIntro, 100) &&
              lifecycleState.frameStageCalls == 1 &&
              lifecycleState.shutdownStageCalls == 0,
          "frameStage-only page-boundary record does not read shutdownStage");
    edvr::pluginRegistryShutdownLifecycle(kIntro);

    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;
    auto* const shutdownPrefix = new (static_cast<uint8_t*>(pageEnd) -
                                      sizeof(ShutdownStageLifecycleOpsPrefix))
        ShutdownStageLifecycleOpsPrefix{
            {offsetof(EdvrPluginLifecycleOps, configureState), kIntro, "intro",
             &lifecycleState, &lifecycleConfigure, &lifecycleFrame,
             &lifecycleShutdown},
            &lifecycleConfigureStage, &lifecycleFrameStage,
            &lifecycleShutdownStage};
    const auto* shutdownOnlyOps = reinterpret_cast<
        const EdvrPluginLifecycleOps*>(shutdownPrefix);
    check(edvr::pluginRegistryRegisterLifecycle(shutdownOnlyOps),
          "staged legacy record registers when configureState is outside structSize");
    bool legacyConfig = true;
    edvr::pluginRegistryConfigureLifecycle(kIntro, &legacyConfig);
    check(lifecycleState.configureCalls == 1 &&
              lifecycleState.configureStateCalls == 0,
          "normal configure never reads the appended callback past declared extent");
    edvr::pluginRegistryShutdownLifecycle(kIntro);

    auto* const truncatedNoConfigure = reinterpret_cast<EdvrPluginLifecycleOps*>(
        shutdownPrefix);
    truncatedNoConfigure->configure = nullptr;
    check(!edvr::pluginRegistryRegisterLifecycle(truncatedNoConfigure) &&
              !edvr::pluginRegistryHasLifecycle(kIntro),
          "truncated stateful-only record is rejected without publishing a slot");

    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;
    auto* const configureStatePrefix = new (
        static_cast<uint8_t*>(pageEnd) - sizeof(ConfigureStateLifecycleOpsPrefix))
        ConfigureStateLifecycleOpsPrefix{
            {{sizeof(ConfigureStateLifecycleOpsPrefix), kIntro, "intro", &lifecycleState,
              nullptr, &lifecycleFrame, &lifecycleShutdown},
             nullptr, nullptr, nullptr},
            &lifecycleConfigureState};
    const auto* configureStateOps = reinterpret_cast<
        const EdvrPluginLifecycleOps*>(configureStatePrefix);
    check(edvr::pluginRegistryRegisterLifecycle(configureStateOps),
          "pre-component full lifecycle record registers at its declared boundary");
    check(edvr::pluginRegistryHasLifecycleComponent(kIntro, 0) &&
              !edvr::pluginRegistryHasLifecycleComponent(kIntro, 1),
          "pre-component records default to component zero without reading beyond their extent");
    bool statefulConfig = true;
    edvr::pluginRegistryConfigureLifecycle(kIntro, &statefulConfig);
    check(lifecycleState.configureStateCalls == 1 &&
              lifecycleState.lastState == &lifecycleState &&
              lifecycleState.lastStateConfig == &statefulConfig &&
              lifecycleState.configureCalls == 0,
          "stateful configure forwards registered state and config through normal dispatch");
    edvr::pluginRegistryShutdownLifecycle(kIntro);

    // The new selector is optional as a whole, but its uint32 field cannot be
    // partially present at a host boundary.
    EdvrPluginLifecycleOps partialComponent = ops;
    partialComponent.structSize = offsetof(EdvrPluginLifecycleOps, componentIndex) + 1;
    check(!edvr::pluginRegistryRegisterLifecycle(&partialComponent),
          "lifecycle registry rejects a partial componentIndex extent");
    EdvrPluginLifecycleOps invalidComponent = ops;
    invalidComponent.structSize = sizeof(EdvrPluginLifecycleOps);
    invalidComponent.componentIndex = edvr::kPluginLifecycleComponentCount;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalidComponent),
          "lifecycle registry rejects an out-of-range component index");
    invalidComponent = ops;
    invalidComponent.structSize = sizeof(EdvrPluginLifecycleOps);
    invalidComponent.componentIndex = 1;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalidComponent),
          "lifecycle registry rejects nonzero components outside Comfort");

    EdvrPluginLifecycleOps statefulPreferred{};
    std::memcpy(&statefulPreferred, configureStatePrefix,
                sizeof(ConfigureStateLifecycleOpsPrefix));
    statefulPreferred.configure = &lifecycleConfigure;
    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;
    check(edvr::pluginRegistryRegisterLifecycle(&statefulPreferred),
          "lifecycle registry accepts stateful and legacy configure callbacks together");
    edvr::pluginRegistryConfigureLifecycle(kIntro, &statefulConfig);
    check(lifecycleState.configureStateCalls == 1 &&
              lifecycleState.configureCalls == 0 &&
              lifecycleState.lastState == &lifecycleState,
          "normal configure prefers the stateful callback when both are present");
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    check(VirtualFree(guardPages, 0, MEM_RELEASE) != 0,
          "guard-page lifecycle fixture releases its storage");

    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;

    EdvrPluginLifecycleOps invalid = ops;
    invalid.structSize = 0;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalid),
          "lifecycle registry rejects a malformed callback record");
    invalid = ops;
    invalid.manifestId = "exposure";
    check(!edvr::pluginRegistryRegisterLifecycle(&invalid),
          "lifecycle registry rejects a mismatched stable manifest ID");
    invalid = ops;
    invalid.frame = nullptr;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalid),
          "lifecycle registry requires configure, frame, and shutdown callbacks");
    invalid = ops;
    invalid.configure = nullptr;
    invalid.configureState = nullptr;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalid),
          "lifecycle registry rejects records with neither configure callback");
    EdvrPluginLifecycleOps catalogOnly = {
        sizeof(EdvrPluginLifecycleOps), edvr::plugins::kPluginPerformance, "performance", &lifecycleState,
        &lifecycleConfigure, &lifecycleFrame, &lifecycleShutdown};
    check(!edvr::pluginRegistryRegisterLifecycle(&catalogOnly),
          "lifecycle registry rejects catalog-only implementations");

    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryRegisterLifecycle(&ops),
          "VR-only lifecycle module is rejected under the flat profile");
    edvr::g_runtimeProfile = savedProfile;

    check(edvr::pluginRegistryRegisterLifecycle(&ops),
          "lifecycle registry accepts the manifest lifecycle implementation");
    check(edvr::pluginRegistryRegisterLifecycle(&ops),
          "same lifecycle ops pointer is idempotent for install retry");
    EdvrPluginLifecycleOps conflict = ops;
    check(!edvr::pluginRegistryRegisterLifecycle(&conflict),
          "lifecycle registry rejects a different record for an occupied index");
    check(edvr::pluginRegistryHasLifecycle(kIntro),
          "registered lifecycle slot is visible by direct manifest index");

    check(!edvr::pluginRegistryConfigureLifecycleStage(kExposure, 1, nullptr) &&
              !edvr::pluginRegistryFrameLifecycleStage(kExposure, 1, nullptr, 0) &&
              !edvr::pluginRegistryShutdownLifecycleStage(kExposure, 1) &&
              !edvr::pluginRegistryConfigureLifecycleStage(UINT32_MAX, 1, nullptr) &&
              !edvr::pluginRegistryFrameLifecycleStage(UINT32_MAX, 1, nullptr, 0) &&
              !edvr::pluginRegistryShutdownLifecycleStage(UINT32_MAX, 1),
          "staged lifecycle dispatch returns false for absent and out-of-range indices");

    EdvrPluginLifecycleOps noStageCallbacks = ops;
    noStageCallbacks.configureStage = nullptr;
    noStageCallbacks.frameStage = nullptr;
    noStageCallbacks.shutdownStage = nullptr;
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    check(edvr::pluginRegistryRegisterLifecycle(&noStageCallbacks),
          "lifecycle registry permits absent optional staged callbacks");
    check(!edvr::pluginRegistryConfigureLifecycleStage(kIntro, 3, nullptr) &&
              !edvr::pluginRegistryFrameLifecycleStage(kIntro, 3, nullptr, 0) &&
              !edvr::pluginRegistryShutdownLifecycleStage(kIntro, 3),
          "null optional staged callbacks report false");
    edvr::pluginRegistryShutdownLifecycle(kIntro);

    EdvrPluginLifecycleOps partialStageOps = ops;
    partialStageOps.structSize = offsetof(EdvrPluginLifecycleOps, frameStage);
    check(edvr::pluginRegistryRegisterLifecycle(&partialStageOps),
          "lifecycle registry accepts a record ending after configureStage");
    check(edvr::pluginRegistryConfigureLifecycleStage(kIntro, 4, nullptr) &&
              !edvr::pluginRegistryFrameLifecycleStage(kIntro, 5, nullptr, 0) &&
              !edvr::pluginRegistryShutdownLifecycleStage(kIntro, 6) &&
              lifecycleState.configureStageCalls == 1 &&
              lifecycleState.frameStageCalls == 0 &&
              lifecycleState.shutdownStageCalls == 0,
          "each staged callback is gated by its own structSize extent");
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    lifecycleState = {};
    lifecycleState.manifestIndex = kIntro;
    check(edvr::pluginRegistryRegisterLifecycle(&ops),
          "staged lifecycle record can be registered after optional-field fixture");

    bool stageConfig = true;
    auto* const expectedContext = reinterpret_cast<ID3D11DeviceContext*>(
        static_cast<uintptr_t>(0x12340));
    check(edvr::pluginRegistryConfigureLifecycleStage(kIntro, 0x10001u,
                                                       &stageConfig) &&
              lifecycleState.configureStageCalls == 1 &&
              lifecycleState.lastState == &lifecycleState &&
              lifecycleState.lastStage == 0x10001u &&
              lifecycleState.lastConfig == &stageConfig,
          "staged configure forwards opaque stage, state, and config exactly");
    check(edvr::pluginRegistryFrameLifecycleStage(kIntro, 0x20002u,
                                                   expectedContext, 0x30003u) &&
              lifecycleState.frameStageCalls == 1 &&
              lifecycleState.lastState == &lifecycleState &&
              lifecycleState.lastStage == 0x20002u &&
              lifecycleState.lastContext == expectedContext &&
              lifecycleState.lastStageSceneFrame == 0x30003u,
          "staged frame forwards stage, context, and scene frame exactly");
    check(edvr::pluginRegistryShutdownLifecycleStage(kIntro, 0x40004u) &&
              lifecycleState.shutdownStageCalls == 1 &&
              lifecycleState.lastState == &lifecycleState &&
              lifecycleState.lastStage == 0x40004u &&
              edvr::pluginRegistryHasLifecycle(kIntro),
          "staged shutdown forwards stage and keeps the lifecycle slot registered");

    bool enabled = true;
    edvr::pluginRegistryConfigureLifecycle(kIntro, &enabled);
    edvr::pluginRegistryFrameLifecycle(kIntro, 0x1234u);
    check(lifecycleState.configureCalls == 1 && lifecycleState.configured &&
              lifecycleState.frameCalls == 1 && lifecycleState.lastFrame == 0x1234u,
          "lifecycle dispatch delivers config and scene-frame value with module state");
    check(edvr::pluginRegistryShaderCandidates() == candidatesBefore &&
              edvr::pluginRegistryDrawInterestMask() == interestsBefore &&
              edvr::pluginRegistryWantsDraws(&registryState) == drawSubscriptionBefore,
          "lifecycle registration and dispatch do not change draw masks or subscriptions");

    edvr::pluginRegistryShutdown();
    check(edvr::pluginRegistryHasLifecycle(kIntro) &&
              lifecycleState.shutdownCalls == 0,
          "legacy draw shutdown leaves lifecycle slots and their lifetime alone");
    lifecycleState.reenterShutdown = true;
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    check(!edvr::pluginRegistryHasLifecycle(kIntro) &&
              lifecycleState.shutdownCalls == 1,
          "lifecycle shutdown detaches before callback and is reentrant/idempotent");

    const uint32_t configureBeforeAbsent = lifecycleState.configureCalls;
    const uint32_t frameBeforeAbsent = lifecycleState.frameCalls;
    edvr::pluginRegistryConfigureLifecycle(kIntro, nullptr);
    edvr::pluginRegistryFrameLifecycle(kIntro, 44);
    edvr::pluginRegistryShutdownLifecycle(kIntro);
    check(lifecycleState.configureCalls == configureBeforeAbsent &&
              lifecycleState.frameCalls == frameBeforeAbsent &&
              lifecycleState.shutdownCalls == 1,
          "absent lifecycle entries receive no callbacks");

    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    check(!edvr::pluginRegistryRegisterLifecycle(&ops),
          "invalid runtime profile rejects lifecycle registration");
    edvr::g_runtimeProfile = savedProfile;
}

void exposureLifecycleRegistryDispatch() {
    constexpr uint32_t kComfort = edvr::plugins::kPluginComfort;
    constexpr uint32_t kExposureComponent =
        edvr::kComfortExposureLifecycleComponent;
    edvr::pluginRegistryShutdownLifecycleComponent(kComfort, kExposureComponent);
    edvr::plugins::exposure::ExposureActionState state;
    state.dampPrevValid = true;
    state.dampHaveMean = true;
    EdvrPluginLifecycleOps ops{};
    edvr::plugins::exposure::initializeExposureLifecycleOps(&ops, &state);
    check(ops.state == &state && ops.manifestIndex == kComfort &&
              ops.componentIndex == kExposureComponent &&
              ops.configure == nullptr && ops.configureState != nullptr,
          "production Exposure lifecycle record binds its embedded stateful callback");

    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryRegisterLifecycle(&ops) &&
              !edvr::pluginRegistryHasLifecycleComponent(kComfort,
                                                         kExposureComponent),
          "Exposure lifecycle profile rejection publishes no registry state");
    edvr::g_runtimeProfile = savedProfile;
    check(edvr::pluginRegistryRegisterLifecycle(&ops) &&
              edvr::pluginRegistryRegisterLifecycle(&ops) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort,
                                                         kExposureComponent),
          "production Exposure lifecycle registers and retries idempotently");

    exposureDampingConfig = 4.0f;
    exposureTauConfig = 0.0f;
    edvr::pluginRegistryConfigureLifecycleComponent(kComfort,
                                                     kExposureComponent,
                                                     &edvr::Config::get());
    check(state.dampK == 1.0f && state.dampTau == 1.0f,
          "registered Exposure configure clamps damping high and tau low");

    exposureDampingConfig = -2.0f;
    exposureTauConfig = 900.0f;
    edvr::pluginRegistryConfigureLifecycleComponent(kComfort,
                                                     kExposureComponent,
                                                     &edvr::Config::get());
    check(state.dampK == 0.0f && state.dampTau == 600.0f &&
              !state.dampPrevValid && !state.dampHaveMean,
          "registered Exposure configure clamps values and resets damper state when disabled");

    state.detectStreak = 3;
    state.seenThisFrame = 2;
    state.shapeVerdict.emplace(0xE001, false);
    state.shapeVerdict.emplace(0xE002, true);
    state.everExamined.emplace(0xE001);
    state.firstEye[0] = reinterpret_cast<ID3D11UnorderedAccessView*>(
        static_cast<uintptr_t>(0xE100));
    edvr::pluginRegistryFrameLifecycleComponent(kComfort, kExposureComponent,
                                                123);
    check(state.detectStreak == 4 && state.seenThisFrame == 0 &&
              state.firstEye[0] == nullptr &&
              state.shapeVerdict.count(0xE001) == 0 &&
              state.shapeVerdict.count(0xE002) == 1 &&
              state.everExamined.count(0xE001) == 1,
          "production lifecycle frame resets pairing before expiring only negative verdicts");

    FakeTexture first{&kFakeTextureVtable};
    FakeTexture second{&kFakeTextureVtable};
    state.dampStaging[0] = reinterpret_cast<ID3D11Texture2D*>(&first);
    state.dampStaging[1] = reinterpret_cast<ID3D11Texture2D*>(&second);
    fakeExposureReleases = 0;
    edvr::pluginRegistryShutdownLifecycleComponent(kComfort,
                                                   kExposureComponent);
    check(!edvr::pluginRegistryHasLifecycleComponent(kComfort,
                                                      kExposureComponent) &&
              state.dampStaging[0] == nullptr && state.dampStaging[1] == nullptr &&
              fakeExposureReleases == 2,
          "production Exposure shutdown detaches and releases/nulls each staging texture");
    edvr::pluginRegistryShutdownLifecycleComponent(kComfort,
                                                   kExposureComponent);
    check(fakeExposureReleases == 2,
          "repeated Exposure shutdown does not release staging textures twice");
}

void comfortComponentRegistryDispatch() {
    constexpr uint32_t kComfort = edvr::plugins::kPluginComfort;
    const uint32_t legacyConfigureCallsBefore = lifecycleState.configureCalls;
    LifecycleState states[edvr::kPluginLifecycleComponentCount]{};
    EdvrPluginLifecycleOps ops[edvr::kPluginLifecycleComponentCount]{};
    for (uint32_t component = 0; component < edvr::kPluginLifecycleComponentCount;
         ++component) {
        edvr::pluginRegistryShutdownLifecycleComponent(kComfort, component);
        states[component].manifestIndex = kComfort;
        // The legacy configure fixture writes global lifecycleState. Use the
        // stateful callback to observe each independently registered component.
        ops[component] = {
            sizeof(EdvrPluginLifecycleOps), kComfort, "comfort", &states[component],
            &lifecycleConfigure, &lifecycleFrame,
            &lifecycleShutdown, &lifecycleConfigureStage,
            &lifecycleFrameStage, &lifecycleShutdownStage,
            &lifecycleConfigureState, component};
    }
    check(edvr::pluginRegistryRegisterLifecycle(&ops[0]) &&
              edvr::pluginRegistryRegisterLifecycle(&ops[1]) &&
              edvr::pluginRegistryRegisterLifecycle(&ops[2]),
          "Comfort accepts three independently registered lifecycle components");
    check(edvr::pluginRegistryRegisterLifecycle(&ops[1]) &&
              edvr::pluginRegistryRegisterLifecycle(&ops[2]),
          "exact lifecycle pointers retry idempotently within their own component slots");
    EdvrPluginLifecycleOps duplicate = ops[1];
    check(!edvr::pluginRegistryRegisterLifecycle(&duplicate) &&
              edvr::pluginRegistryHasLifecycle(kComfort) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 0) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 1) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 2),
          "component slots coexist and reject a different pointer without replacing any slot");
    edvr::pluginRegistryShutdown();
    check(edvr::pluginRegistryHasLifecycleComponent(kComfort, 0) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 1) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 2) &&
              states[0].shutdownCalls == 0 && states[1].shutdownCalls == 0 &&
              states[2].shutdownCalls == 0,
          "generic draw shutdown leaves every lifecycle component attached");

    bool enabled = true;
    edvr::pluginRegistryConfigureLifecycle(kComfort, &enabled);
    edvr::pluginRegistryFrameLifecycle(kComfort, 0x101u);
    check(states[0].configureStateCalls == 1 &&
              states[0].lastState == &states[0] &&
              states[0].lastStateConfig == &enabled &&
              states[0].frameCalls == 1 && states[0].lastFrame == 0x101u &&
              states[1].configureStateCalls == 0 && states[1].frameCalls == 0 &&
              states[2].configureStateCalls == 0 && states[2].frameCalls == 0 &&
              lifecycleState.configureCalls == legacyConfigureCallsBefore,
          "legacy lifecycle wrappers preserve component-zero dispatch");
    edvr::pluginRegistryConfigureLifecycleComponent(kComfort, 1, &enabled);
    edvr::pluginRegistryFrameLifecycleComponent(kComfort, 1, 0x202u);
    edvr::pluginRegistryConfigureLifecycleComponent(kComfort, 2, &enabled);
    edvr::pluginRegistryFrameLifecycleComponent(kComfort, 2, 0x303u);
    check(states[0].configureStateCalls == 1 && states[0].frameCalls == 1 &&
              states[0].lastFrame == 0x101u &&
              states[1].configureStateCalls == 1 &&
              states[1].lastState == &states[1] &&
              states[1].lastStateConfig == &enabled &&
              states[1].frameCalls == 1 && states[1].lastFrame == 0x202u &&
              states[2].configureStateCalls == 1 &&
              states[2].lastState == &states[2] &&
              states[2].lastStateConfig == &enabled &&
              states[2].frameCalls == 1 && states[2].lastFrame == 0x303u &&
              lifecycleState.configureCalls == legacyConfigureCallsBefore,
          "explicit component dispatch reaches only the selected Comfort module");

    auto* const expectedContext = reinterpret_cast<ID3D11DeviceContext*>(
        static_cast<uintptr_t>(0x77880));
    bool stagedConfig = true;
    check(edvr::pluginRegistryConfigureLifecycleComponentStage(
              kComfort, 1, 0x111u, &stagedConfig) &&
              states[1].lastStage == 0x111u && states[1].lastConfig == &stagedConfig &&
              edvr::pluginRegistryFrameLifecycleComponentStage(
                  kComfort, 2, 0x222u, expectedContext, 0x333u) &&
              states[2].lastStage == 0x222u &&
              states[2].lastContext == expectedContext &&
              states[2].lastStageSceneFrame == 0x333u &&
              edvr::pluginRegistryShutdownLifecycleComponentStage(kComfort, 1,
                                                                   0x444u) &&
              states[1].lastStage == 0x444u &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 1),
          "component staged APIs preserve group/component/stage and legacy argument order");
    check(!edvr::pluginRegistryHasLifecycleComponent(UINT32_MAX, 0) &&
              !edvr::pluginRegistryHasLifecycleComponent(kComfort,
                                                          edvr::kPluginLifecycleComponentCount) &&
              !edvr::pluginRegistryConfigureLifecycleComponentStage(
                  kComfort, edvr::kPluginLifecycleComponentCount, 1, nullptr) &&
              !edvr::pluginRegistryFrameLifecycleComponentStage(
                  kComfort, UINT32_MAX, 1, nullptr, 2) &&
              !edvr::pluginRegistryShutdownLifecycleComponentStage(
                  UINT32_MAX, 0, 1),
          "component lookups and staged dispatch are bounded for invalid indices");

    edvr::pluginRegistryShutdownLifecycleComponent(kComfort, 1);
    check(!edvr::pluginRegistryHasLifecycleComponent(kComfort, 1) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 0) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 2) &&
              states[1].shutdownCalls == 1,
          "component shutdown detaches and retires only its own slot");
    states[0].reenterShutdown = true;
    edvr::pluginRegistryShutdownLifecycle(kComfort);
    check(!edvr::pluginRegistryHasLifecycleComponent(kComfort, 0) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 2) &&
              states[0].shutdownCalls == 1,
          "default shutdown affects only component zero");
    edvr::pluginRegistryShutdownLifecycleComponent(kComfort, 2);
    check(!edvr::pluginRegistryHasLifecycleComponent(kComfort, 2) &&
              states[2].shutdownCalls == 1,
          "last component can be retired independently");
}

bool lifecycleEventsAre(const edvr::LifecycleModuleEvent* expected,
                        uint32_t count) {
    if (edvr::lifecycleModuleTrace.count != count) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (edvr::lifecycleModuleTrace.events[i] != expected[i]) return false;
    return true;
}

void scannerComfortLifecycleDispatch() {
    constexpr uint32_t kComfort = edvr::plugins::kPluginComfort;
    constexpr uint32_t kExplorerCam = edvr::plugins::kPluginExplorerCam;
    edvr::plugins::exposure::ExposureActionState exposureState;
    EdvrPluginLifecycleOps exposureOps{};
    edvr::plugins::exposure::initializeExposureLifecycleOps(&exposureOps,
                                                             &exposureState);
    const auto& scannerOps = edvr::plugins::scanners::scannerLifecycleOps();
    const auto& comfortOps = edvr::plugins::comfort::comfortLifecycleOps();
    const auto& explorerOps =
        edvr::plugins::explorer_cam::explorerCamLifecycleOps();
    check(exposureOps.manifestIndex == kComfort &&
              exposureOps.componentIndex ==
                  edvr::kComfortExposureLifecycleComponent &&
              exposureOps.manifestId && strcmp(exposureOps.manifestId, "comfort") == 0 &&
              scannerOps.manifestIndex == kComfort &&
              scannerOps.componentIndex ==
                  edvr::kComfortScannersLifecycleComponent &&
              scannerOps.manifestId && strcmp(scannerOps.manifestId, "comfort") == 0 &&
              comfortOps.manifestIndex == kComfort &&
              comfortOps.componentIndex ==
                  edvr::kComfortTransitionFlashLifecycleComponent &&
              comfortOps.manifestId && strcmp(comfortOps.manifestId, "comfort") == 0 &&
              explorerOps.manifestIndex == kExplorerCam &&
              explorerOps.manifestId &&
              strcmp(explorerOps.manifestId, "explorer-cam") == 0 &&
              explorerOps.configure == nullptr &&
              explorerOps.configureState == nullptr,
          "production records bind Comfort components and a configure-free Explorer group");

    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryRegisterLifecycle(&exposureOps) &&
              !edvr::pluginRegistryHasLifecycleComponent(
                  kComfort, edvr::kComfortExposureLifecycleComponent) &&
              !edvr::pluginRegistryRegisterLifecycle(&scannerOps) &&
              !edvr::pluginRegistryHasLifecycleComponent(
                  kComfort, edvr::kComfortScannersLifecycleComponent) &&
              !edvr::pluginRegistryRegisterLifecycle(&comfortOps) &&
              !edvr::pluginRegistryHasLifecycleComponent(
                  kComfort, edvr::kComfortTransitionFlashLifecycleComponent) &&
              !edvr::pluginRegistryRegisterLifecycle(&explorerOps) &&
              !edvr::pluginRegistryHasLifecycle(kExplorerCam),
          "VR-only Comfort components and Explorer registration publish no lifecycle state");
    edvr::g_runtimeProfile = savedProfile;
    check(edvr::pluginRegistryRegisterLifecycle(&exposureOps) &&
              edvr::pluginRegistryRegisterLifecycle(&scannerOps) &&
              edvr::pluginRegistryRegisterLifecycle(&scannerOps) &&
              edvr::pluginRegistryRegisterLifecycle(&comfortOps) &&
              edvr::pluginRegistryRegisterLifecycle(&comfortOps) &&
              edvr::pluginRegistryRegisterLifecycle(&explorerOps) &&
              edvr::pluginRegistryRegisterLifecycle(&explorerOps),
          "production Comfort and Explorer records register and retry idempotently");
    check(edvr::pluginRegistryHasLifecycleComponent(kComfort, 0) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 1) &&
              edvr::pluginRegistryHasLifecycleComponent(kComfort, 2),
          "all three production Comfort module records coexist under one manifest group");

    edvr::lifecycleModuleTrace = {};
    auto& config = edvr::Config::get();
    edvr::pluginRegistryConfigureLifecycleComponent(
        kComfort, edvr::kComfortScannersLifecycleComponent, &config);
    const edvr::LifecycleModuleEvent scannerConfigure[] = {
        edvr::kScannerPanelConfigure, edvr::kScannerRevealConfigure};
    check(lifecycleEventsAre(scannerConfigure, 2) &&
              edvr::lifecycleModuleTrace.config == &config,
          "normal Scanner configure forwards panel then reveal exactly once");

    edvr::lifecycleModuleTrace = {};
    check(edvr::pluginRegistryConfigureLifecycleComponentStage(
              kComfort, edvr::kComfortScannersLifecycleComponent,
              edvr::plugins::scanners::kScannerLifecycleConfigurePanel, &config) &&
              edvr::pluginRegistryConfigureLifecycleComponentStage(
                  kComfort, edvr::kComfortScannersLifecycleComponent,
                  edvr::plugins::scanners::kScannerLifecycleConfigureReveal,
                  &config),
          "staged Scanner configure accepts each host-owned original position");
    check(lifecycleEventsAre(scannerConfigure, 2) &&
              edvr::lifecycleModuleTrace.config == &config,
          "staged Scanner configure preserves panel-before-reveal order without duplicate module calls");

    edvr::lifecycleModuleTrace = {};
    edvr::pluginRegistryFrameLifecycleComponent(
        kComfort, edvr::kComfortScannersLifecycleComponent, 0x12345678u);
    check(edvr::lifecycleModuleTrace.count == 1 &&
              edvr::lifecycleModuleTrace.events[0] == edvr::kScannerRevealFrame,
          "normal Scanner frame forwards the real reveal boundary once");
    edvr::lifecycleModuleTrace = {};
    check(edvr::pluginRegistryFrameLifecycleComponentStage(
              kComfort, edvr::kComfortScannersLifecycleComponent,
              edvr::plugins::scanners::kScannerLifecycleRevealFrame, nullptr,
              0x23456789u) &&
              edvr::lifecycleModuleTrace.count == 1 &&
              edvr::lifecycleModuleTrace.events[0] == edvr::kScannerRevealFrame,
          "staged Scanner frame forwards the reveal boundary once");

    edvr::lifecycleModuleTrace = {};
    check(edvr::pluginRegistryShutdownLifecycleComponentStage(
              kComfort, edvr::kComfortScannersLifecycleComponent,
              edvr::plugins::scanners::kScannerLifecycleShutdownPanel),
          "Scanner panel teardown stage dispatches while the record is attached");
    edvr::noteLifecycleModule(edvr::kScannerRectanglesShutdown);
    edvr::pluginRegistryShutdownLifecycleComponent(
        kComfort, edvr::kComfortScannersLifecycleComponent);
    const edvr::LifecycleModuleEvent scannerShutdown[] = {
        edvr::kScannerPanelShutdown,
        edvr::kScannerRectanglesShutdown,
        edvr::kScannerRevealShutdown};
    check(lifecycleEventsAre(scannerShutdown, 3) &&
              edvr::lifecycleModuleTrace.scannerDetachedAtRevealShutdown &&
              !edvr::pluginRegistryHasLifecycleComponent(
                  kComfort, edvr::kComfortScannersLifecycleComponent),
          "Scanner shutdown preserves staged panel then host-gap then final detached reveal cleanup");

    edvr::lifecycleModuleTrace = {};
    edvr::pluginRegistryConfigureLifecycleComponent(
        kComfort, edvr::kComfortTransitionFlashLifecycleComponent, &config);
    check(edvr::lifecycleModuleTrace.count == 1 &&
              edvr::lifecycleModuleTrace.events[0] == edvr::kComfortFlashConfigure &&
              edvr::lifecycleModuleTrace.config == &config,
          "normal Comfort configure forwards Transition Flash exactly once");

    edvr::lifecycleModuleTrace = {};
    check(edvr::pluginRegistryFrameLifecycleComponentStage(
              kComfort, edvr::kComfortTransitionFlashLifecycleComponent,
              edvr::plugins::comfort::kComfortLifecycleTransitionFlashFrame,
              nullptr, 0x456789ABu) &&
              edvr::lifecycleModuleTrace.count == 1 &&
              edvr::lifecycleModuleTrace.events[0] == edvr::kComfortFlashFrame &&
              edvr::lifecycleModuleTrace.frameNo == 0x456789ABu,
          "staged Comfort frame forwards the exact Transition Flash frame number once");
    edvr::lifecycleModuleTrace = {};
    edvr::pluginRegistryShutdownLifecycleComponent(
        kComfort, edvr::kComfortTransitionFlashLifecycleComponent);
    check(edvr::lifecycleModuleTrace.count == 1 &&
              edvr::lifecycleModuleTrace.events[0] == edvr::kComfortFlashShutdown &&
              edvr::lifecycleModuleTrace.comfortDetachedAtFlashShutdown &&
              !edvr::pluginRegistryHasLifecycleComponent(
                  kComfort, edvr::kComfortTransitionFlashLifecycleComponent) &&
              edvr::pluginRegistryHasLifecycleComponent(
                  kComfort, edvr::kComfortExposureLifecycleComponent),
          "Transition Flash shutdown detaches only its component after independent dispatch");

    edvr::lifecycleModuleTrace = {};
    edvr::pluginRegistryFrameLifecycle(kExplorerCam, 0x3456789Au);
    check(edvr::lifecycleModuleTrace.count == 1 &&
              edvr::lifecycleModuleTrace.events[0] == edvr::kComfortExplorerFrame &&
              edvr::lifecycleModuleTrace.frameNo == 0x3456789Au,
          "standalone Explorer Cam frame dispatch forwards the exact frame number");
    edvr::pluginRegistryShutdownLifecycle(kExplorerCam);
    check(edvr::lifecycleModuleTrace.count == 2 &&
              edvr::lifecycleModuleTrace.events[1] == edvr::kComfortExplorerShutdown &&
              !edvr::pluginRegistryHasLifecycle(kExplorerCam),
          "standalone configure-free Explorer Cam lifecycle shuts down independently");
    edvr::pluginRegistryShutdownLifecycleComponent(
        kComfort, edvr::kComfortExposureLifecycleComponent);
    edvr::g_runtimeProfile = savedProfile;
}

// This exercises the production gate with the real lifecycle registry. Host
// source pins verify the startup/dispatch call sites use this same gate.
void comfortOwnerDeniedLifecycleFallbacks() {
    const auto& comfortOps = edvr::plugins::comfort::comfortLifecycleOps();
    auto& config = edvr::Config::get();
    edvr::pluginRegistryShutdownLifecycleComponent(
        edvr::plugins::kPluginComfort,
        edvr::kComfortTransitionFlashLifecycleComponent);
    edvr::lifecycleModuleTrace = {};
    unsigned registryAttempts = 0;
    unsigned directFallbackCalls = 0;
    const bool deniedRegistration = edvr::runComfortOwned(false, [&] {
        ++registryAttempts;
        edvr::pluginRegistryRegisterLifecycle(&comfortOps);
    });
    const bool deniedFallback = edvr::runComfortOwned(false, [&] {
        ++directFallbackCalls;
        edvr::transitionFlashEyeBaseConfigure(config);
    });
    check(!deniedRegistration && !deniedFallback && registryAttempts == 0 &&
              directFallbackCalls == 0 &&
              !edvr::pluginRegistryHasLifecycleComponent(
                  edvr::plugins::kPluginComfort,
                  edvr::kComfortTransitionFlashLifecycleComponent) &&
              edvr::lifecycleModuleTrace.count == 0,
          "denied Comfort owner leaves the real lifecycle registry empty and invokes neither module nor direct fallback callback");

    const bool allowedRegistration = edvr::runComfortOwned(true, [&] {
        ++registryAttempts;
        edvr::pluginRegistryRegisterLifecycle(&comfortOps);
    });
    edvr::lifecycleModuleTrace = {};
    edvr::pluginRegistryConfigureLifecycleComponent(
        edvr::plugins::kPluginComfort,
        edvr::kComfortTransitionFlashLifecycleComponent, &config);
    const bool allowedFallback = edvr::runComfortOwned(true, [&] {
        ++directFallbackCalls;
        edvr::transitionFlashEyeBaseConfigure(config);
    });
    check(allowedRegistration && allowedFallback && registryAttempts == 1 &&
              directFallbackCalls == 1 &&
              edvr::pluginRegistryHasLifecycleComponent(
                  edvr::plugins::kPluginComfort,
                  edvr::kComfortTransitionFlashLifecycleComponent) &&
              edvr::lifecycleModuleTrace.count == 2 &&
              edvr::lifecycleModuleTrace.events[0] ==
                  edvr::kComfortFlashConfigure &&
              edvr::lifecycleModuleTrace.events[1] ==
                  edvr::kComfortFlashConfigure,
          "allowed Comfort owner preserves real registry dispatch and Legacy fallback behavior");
    edvr::pluginRegistryShutdownLifecycleComponent(
        edvr::plugins::kPluginComfort,
        edvr::kComfortTransitionFlashLifecycleComponent);
}

void pilotAndDiagnosticsLifecycleAdmission() {
    constexpr uint32_t kCockpit = edvr::plugins::kPluginCockpitVisuals;
    constexpr uint32_t kDiagnostics = edvr::plugins::kPluginDiagnostics;
    edvr::pluginRegistryShutdownLifecycle(kCockpit);
    edvr::pluginRegistryShutdownLifecycle(kDiagnostics);
    edvr::pluginRegistryShutdown();
    registryState = {};

    LifecycleState cockpitState{};
    cockpitState.manifestIndex = kCockpit;
    EdvrPluginLifecycleOps cockpitLifecycle{
        sizeof(EdvrPluginLifecycleOps), kCockpit, "cockpit-visuals",
        &cockpitState, &lifecycleConfigure, &lifecycleFrame,
        &lifecycleShutdown, nullptr, nullptr, nullptr,
        &lifecycleConfigureState, 0};
    LifecycleState diagnosticsState{};
    diagnosticsState.manifestIndex = kDiagnostics;
    EdvrPluginLifecycleOps diagnosticsLifecycle{
        sizeof(EdvrPluginLifecycleOps), kDiagnostics, "diagnostics",
        &diagnosticsState, &lifecycleConfigure, &lifecycleFrame,
        &lifecycleShutdown, nullptr, nullptr, nullptr,
        &lifecycleConfigureState, 0};

    EdvrPluginLifecycleOps invalidDiagnosticsComponent = diagnosticsLifecycle;
    invalidDiagnosticsComponent.componentIndex = 1;
    check(!edvr::pluginRegistryRegisterLifecycle(&invalidDiagnosticsComponent) &&
              !edvr::pluginRegistryHasLifecycle(kDiagnostics),
          "nonzero lifecycle components remain restricted to Comfort");

    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryRegisterLifecycle(&cockpitLifecycle) &&
              !edvr::pluginRegistryHasLifecycle(kCockpit),
          "Cockpit RemLok lifecycle registration remains absent on the unsupported flat profile");
    edvr::g_runtimeProfile = savedProfile;

    EdvrPluginOps nightVision = registryOps();
    check(edvr::pluginRegistryRegister(&nightVision) &&
              edvr::pluginRegistryRegisterLifecycle(&cockpitLifecycle) &&
              edvr::pluginRegistryRegisterLifecycle(&diagnosticsLifecycle),
          "Cockpit pilot and Diagnostics lifecycle records register independently");
    check(edvr::pluginRegistryHasLifecycle(kCockpit) &&
              edvr::pluginRegistryHasLifecycle(kDiagnostics),
          "Cockpit component zero and Diagnostics component zero occupy separate lifecycle slots");

    bool enabled = true;
    edvr::pluginRegistryConfigure(&enabled);
    edvr::bindingSetShader(edvr::BindSlot::Vs,
                           reinterpret_cast<void*>(0x424), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps,
                           reinterpret_cast<void*>(0x515), kPs);
    const auto candidatesBeforeLifecycle = edvr::pluginRegistryShaderCandidates();
    check(edvr::plugins::dispatch::hasCandidate(candidatesBeforeLifecycle, kCockpit),
          "registered Night Vision draw module has a shader candidate before lifecycle dispatch");
    const auto legacyConfigureCallsBefore = lifecycleState.configureCalls;
    edvr::pluginRegistryConfigureLifecycle(kCockpit, &enabled);
    edvr::pluginRegistryFrameLifecycle(kCockpit, 0x4242u);
    const auto candidates = edvr::pluginRegistryShaderCandidates();
    check(candidates == candidatesBeforeLifecycle &&
              edvr::plugins::dispatch::hasCandidate(candidates, kCockpit),
          "Cockpit lifecycle configure/frame does not disturb the Night Vision draw candidate");
    check(cockpitState.configureStateCalls == 1 &&
              cockpitState.lastState == &cockpitState &&
              cockpitState.lastStateConfig == &enabled &&
              cockpitState.frameCalls == 1 && cockpitState.lastFrame == 0x4242u &&
              lifecycleState.configureCalls == legacyConfigureCallsBefore,
          "Cockpit lifecycle dispatch forwards stateful configure and frame to its own record");
    edvr::pluginRegistryShutdownLifecycle(kCockpit);
    check(!edvr::pluginRegistryHasLifecycle(kCockpit) &&
              edvr::pluginRegistryWantsDraws(&registryState) &&
              edvr::pluginRegistryShaderCandidates() == candidates,
          "retiring RemLok lifecycle leaves the independently registered Night Vision draw module active");

    edvr::pluginRegistryConfigureLifecycle(kDiagnostics, &enabled);
    edvr::pluginRegistryFrameLifecycle(kDiagnostics, 0x5151u);
    check(diagnosticsState.configureStateCalls == 1 &&
              diagnosticsState.lastState == &diagnosticsState &&
              diagnosticsState.lastStateConfig == &enabled &&
              diagnosticsState.frameCalls == 1 && diagnosticsState.lastFrame == 0x5151u &&
              lifecycleState.configureCalls == legacyConfigureCallsBefore,
          "Diagnostics lifecycle dispatch forwards its own configure and frame callbacks");
    edvr::pluginRegistryShutdownLifecycle(kDiagnostics);
    check(!edvr::pluginRegistryHasLifecycle(kDiagnostics) &&
              diagnosticsState.shutdownCalls == 1,
          "Diagnostics lifecycle shutdown retires only its lifecycle slot");
    edvr::pluginRegistryShutdown();
    edvr::g_runtimeProfile = savedProfile;
}
}

namespace edvr {
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) { ++loggerNoteCalls; }
void breadcrumb(const char*) {}
Config& Config::get() { static Config config; return config; }
float Config::getFloat(const char* key, float def) const {
    if (std::strcmp(key, "experimental.exposure_damping") == 0)
        return exposureDampingConfig;
    if (std::strcmp(key, "experimental.exposure_damping_tau") == 0)
        return exposureTauConfig;
    return def;
}
uint64_t lookupShaderHash(void*) { return 0; }
void exposureDispatchApplyPair(void*, ID3D11DeviceContext*,
                               ID3D11UnorderedAccessView**,
                               ID3D11UnorderedAccessView**) {}
uint64_t sunglareLastSeenMs() { return 0; }
}

namespace edvr::plugins::exposure {
bool shapeLooksLikeExposure() { return false; }
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("Usage: plugin_dispatch_test --self-test");
        return 2;
    }
    replayMatrix();
    costBudgetMetadata();
    disabledAndCacheCases();
    registryLifecycle();
    lifecycleRegistryDispatch();
    exposureLifecycleRegistryDispatch();
    comfortComponentRegistryDispatch();
    scannerComfortLifecycleDispatch();
    comfortOwnerDeniedLifecycleFallbacks();
    pilotAndDiagnosticsLifecycleAdmission();
    std::printf("PASS: plugin dispatch (%u checks)\n", checks);
    return 0;
}
