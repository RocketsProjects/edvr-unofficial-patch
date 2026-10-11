#include "plugin_registry.h"
#include "plugin_dispatch.h"
#include "binding_shadow.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <cstring>

namespace edvr {
namespace detail { std::atomic<uint64_t> g_pluginShaderCandidates{0}; }
namespace detail {
std::atomic<draw_interest::InterestMask> g_legacyDrawInterestMask{0};
}
namespace {
constexpr uint32_t kMaxPlugins = plugins::kPluginCount;
constexpr uint32_t kMaxLegacySubscribers = 64;
constexpr uint32_t kMaxShaderClaims = 64;
constexpr size_t kMaxLegacyShaderFilters = 32;
static_assert(plugins::kPluginCount == plugins::kPluginIndexCount,
              "plugin manifest count and index enum must agree");

struct LegacySubscriber {
    const char* name = nullptr;
    EdvrLegacyDrawGateFn predicate = nullptr;
    void* state = nullptr;
};

const EdvrPluginOps* g_plugins[kMaxPlugins]{};
const EdvrPluginOps* g_registeredPlugins[kMaxPlugins]{};
const EdvrPluginLifecycleOps* g_lifecyclePlugins[
    kMaxPlugins][kPluginLifecycleComponentCount]{};
uint32_t g_registeredPluginCount = 0;
LegacySubscriber g_legacy[kMaxLegacySubscribers]{};
uint32_t g_legacyCount = 0;
bool g_drawGateFailOpen = false;
LegacySubscriber g_testSavedLegacy[kMaxLegacySubscribers]{};
uint32_t g_testSavedLegacyCount = 0;
bool g_testSavedDrawGateFailOpen = false;
bool g_legacyDrawGateTestActive = false;
bool g_notedShaderCandidate = false;
bool g_notedCandidateDraw = false;
bool g_pendingShaderCandidateReport = false;
bool g_pendingCandidateDrawReport = false;
bool g_pluginsConfigured = false;
uint64_t g_activePluginMask = 0;
uint64_t g_vsHash = 0;
uint64_t g_psHash = 0;
constexpr char kNightVisionClaimId[] = "night-vision";
plugins::dispatch::ShaderClaimKey g_shaderClaims[kMaxShaderClaims]{};
uint32_t g_shaderClaimCount = 0;
draw_interest::InterestMask g_configuredLegacyInterests = 0;
draw_interest::ShaderFilter g_legacyShaderFilters[kMaxLegacyShaderFilters]{};
size_t g_legacyShaderFilterCount = 0;

void rebuildCandidates();

bool legacyInterestNeedsShaderObserver() {
    for (size_t i = 0; i < g_legacyShaderFilterCount; ++i) {
        if (draw_interest::contains(g_configuredLegacyInterests,
                                    g_legacyShaderFilters[i].id))
            return true;
    }
    return false;
}

template <typename T>
bool lifecycleFieldPresent(const EdvrPluginLifecycleOps* ops, size_t offset) {
    return ops && ops->structSize >= offset + sizeof(T);
}

const EdvrPluginLifecycleOps* lifecycleForComponent(uint32_t manifestIndex,
                                                    uint32_t componentIndex) {
    if (manifestIndex >= kMaxPlugins ||
        componentIndex >= kPluginLifecycleComponentCount)
        return nullptr;
    return g_lifecyclePlugins[manifestIndex][componentIndex];
}

bool lifecycleDeclaresConfigure(const plugins::PluginRecord& record) {
    for (uint32_t i = 0; i < record.hookPointCount; ++i)
        if (record.hookPoints[i] &&
            std::strcmp(record.hookPoints[i], "lifecycle.configure") == 0)
            return true;
    return false;
}

void observeCanonicalShaderPair() {
    pluginRegistryOnShaderBind(bindingShaderHash(BindSlot::Vs),
                               bindingShaderHash(BindSlot::Ps));
}

void updateBindingObserver() {
    const bool wantsObservation = g_pluginsConfigured &&
        (g_activePluginMask != 0 || legacyInterestNeedsShaderObserver());
    bindingShadowSetShaderObserver(wantsObservation ? observeCanonicalShaderPair : nullptr);
    if (wantsObservation) {
        // Configuration can change while setters are quiet. Seed from the
        // canonical owner-thread shadow so the next draw sees the current pair.
        observeCanonicalShaderPair();
        // The pair can be unchanged while a config toggle changes the active
        // module mask, so recompute even when the observer sees no hash delta.
        rebuildCandidates();
    } else {
        g_vsHash = g_psHash = 0;
        detail::g_pluginShaderCandidates.store(0, std::memory_order_relaxed);
        detail::g_legacyDrawInterestMask.store(
            draw_interest::buildCandidateMask(g_configuredLegacyInterests, 0, 0,
                g_legacyShaderFilters, g_legacyShaderFilterCount),
            std::memory_order_relaxed);
    }
}

uint64_t pluginBit(uint32_t index) {
    return index < 64 ? (uint64_t{1} << index) : 0;
}

void rebuildActivePluginMask() {
    uint64_t activePlugins = 0;
    for (uint32_t i = 0; i < g_registeredPluginCount; ++i) {
        const EdvrPluginOps* ops = g_registeredPlugins[i];
        if (ops && ops->claimDraw &&
            (!ops->wantsDraws || ops->wantsDraws(ops->state)))
            activePlugins |= pluginBit(ops->manifestIndex);
    }
    g_activePluginMask = activePlugins;
}

void rebuildCandidates() {
    const uint64_t before = detail::g_pluginShaderCandidates.load(std::memory_order_relaxed);
    const uint64_t candidates = plugins::dispatch::candidatePlugins(
        g_vsHash, g_psHash, g_activePluginMask, g_shaderClaims,
        g_shaderClaimCount);
    detail::g_pluginShaderCandidates.store(candidates, std::memory_order_relaxed);
    detail::g_legacyDrawInterestMask.store(
        draw_interest::buildCandidateMask(g_configuredLegacyInterests, g_vsHash,
            g_psHash, g_legacyShaderFilters, g_legacyShaderFilterCount),
        std::memory_order_relaxed);
    if (!g_notedShaderCandidate &&
        !(before & pluginBit(plugins::kPluginCockpitVisuals)) &&
        (candidates & pluginBit(plugins::kPluginCockpitVisuals))) {
        g_notedShaderCandidate = true;
        g_pendingShaderCandidateReport = true;
    }
}

const EdvrPluginOps* pluginForClaim(uint32_t claim) {
    if (claim == kPluginClaimNightVision)
        return g_plugins[plugins::kPluginCockpitVisuals];
    return nullptr;
}

bool pluginDeclaresClaim(const EdvrPluginOps* ops, const char* claimId) {
    if (!ops || !claimId || !ops->claimIds) return false;
    for (uint32_t i = 0; i < ops->claimCount; ++i)
        if (ops->claimIds[i] && std::strcmp(ops->claimIds[i], claimId) == 0) return true;
    return false;
}

bool validateAndAppendClaims(const EdvrPluginOps* ops) {
    if (!ops->claimCount || !ops->claimIds) return false;
    const plugins::PluginRecord& record = plugins::kManifest[ops->manifestIndex];
    for (uint32_t i = 0; i < ops->claimCount; ++i) {
        if (!ops->claimIds[i] || !*ops->claimIds[i]) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (std::strcmp(ops->claimIds[i], ops->claimIds[j]) == 0) return false;
        bool found = false;
        for (uint32_t j = 0; j < record.claimCount; ++j)
            if (record.claims[j].id && std::strcmp(record.claims[j].id, ops->claimIds[i]) == 0)
                found = true;
        if (!found) return false;
    }
    uint32_t needed = 0;
    for (uint32_t i = 0; i < record.claimCount; ++i)
        if (pluginDeclaresClaim(ops, record.claims[i].id))
            needed += record.claims[i].shaderPairCount;
    if (needed > kMaxShaderClaims - g_shaderClaimCount) return false;
    for (uint32_t i = 0; i < record.claimCount; ++i) {
        const plugins::Claim& claim = record.claims[i];
        if (!pluginDeclaresClaim(ops, claim.id)) continue;
        for (uint32_t j = 0; j < claim.shaderPairCount; ++j) {
            const plugins::ShaderPair& pair = claim.shaderPairs[j];
            g_shaderClaims[g_shaderClaimCount++] = {
                ops->manifestIndex, claim.id,
                pair.vertexShaderHash, pair.pixelShaderHash};
        }
    }
    return true;
}
}  // namespace

bool pluginRegistryRegister(const EdvrPluginOps* ops) {
    if (!ops || ops->structSize < sizeof(EdvrPluginOps) || !ops->manifestId ||
        !*ops->manifestId || !ops->drawGateName || !*ops->drawGateName ||
        !ops->wantsDraws || !ops->configure || !ops->claimDraw ||
        !ops->startupHooksWanted ||
        !ops->begin || !ops->end || !ops->shutdown ||
        ops->manifestIndex >= kMaxPlugins ||
        !pluginRegistryProfileSupports(ops->manifestIndex) ||
        g_registeredPluginCount >= kMaxPlugins || g_plugins[ops->manifestIndex]) return false;
    const plugins::PluginRecord& record = plugins::kManifest[ops->manifestIndex];
    if (!record.id || !record.implementationStatus || record.index != ops->manifestIndex ||
        std::strcmp(record.id, ops->manifestId) != 0 ||
        std::strcmp(record.implementationStatus, "phase1-pilot") != 0) return false;
    for (uint32_t i = 0; i < g_registeredPluginCount; ++i)
        if (std::strcmp(g_registeredPlugins[i]->drawGateName, ops->drawGateName) == 0)
            return false;
    if (!validateAndAppendClaims(ops)) return false;
    g_plugins[ops->manifestIndex] = ops;
    g_registeredPlugins[g_registeredPluginCount++] = ops;
    rebuildActivePluginMask();
    rebuildCandidates();
    updateBindingObserver();
    Log::get().note("plugin registry: %s registered; %u declared claim(s), draw gate '%s'",
                    record.id, ops->claimCount, ops->drawGateName);
    return true;
}

bool pluginRegistryRegisterLifecycle(const EdvrPluginLifecycleOps* ops) {
    constexpr size_t kLegacyLifecycleSize =
        offsetof(EdvrPluginLifecycleOps, configureStage);
    constexpr size_t kComponentIndexEnd =
        offsetof(EdvrPluginLifecycleOps, componentIndex) + sizeof(uint32_t);
    const bool hasStatefulConfigure = lifecycleFieldPresent<EdvrPluginConfigureStateFn>(
        ops, offsetof(EdvrPluginLifecycleOps, configureState)) &&
        ops->configureState;
    if (!ops || ops->structSize < kLegacyLifecycleSize ||
        (ops->structSize > offsetof(EdvrPluginLifecycleOps, componentIndex) &&
         ops->structSize < kComponentIndexEnd) ||
        !ops->manifestId || !*ops->manifestId ||
        !ops->frame || !ops->shutdown || ops->manifestIndex >= kMaxPlugins)
        return false;

    const bool hasComponentIndex = ops->structSize >= kComponentIndexEnd;
    const uint32_t componentIndex = hasComponentIndex ? ops->componentIndex : 0;
    if (componentIndex >= kPluginLifecycleComponentCount ||
        (componentIndex != 0 && ops->manifestIndex != plugins::kPluginComfort))
        return false;

    // Registration can be retried when a later install step fails. Preserve
    // the already-published slot only for the exact same immutable record.
    const EdvrPluginLifecycleOps* const existing =
        g_lifecyclePlugins[ops->manifestIndex][componentIndex];
    if (existing) return existing == ops;

    if (!pluginRegistryProfileSupports(ops->manifestIndex)) return false;
    const plugins::PluginRecord& record = plugins::kManifest[ops->manifestIndex];
    if (!record.id || !record.implementationStatus ||
        record.index != ops->manifestIndex ||
        std::strcmp(record.id, ops->manifestId) != 0 ||
        (std::strcmp(record.implementationStatus, "phase1-lifecycle") != 0 &&
         std::strcmp(record.implementationStatus, "phase1-pilot") != 0))
        return false;
    if (lifecycleDeclaresConfigure(record) &&
        !ops->configure && !hasStatefulConfigure)
        return false;

    g_lifecyclePlugins[ops->manifestIndex][componentIndex] = ops;
    return true;
}

bool pluginRegistryHasLifecycle(uint32_t manifestIndex) {
    return pluginRegistryHasLifecycleComponent(manifestIndex, 0);
}

bool pluginRegistryHasLifecycleComponent(uint32_t manifestIndex,
                                         uint32_t componentIndex) {
    return lifecycleForComponent(manifestIndex, componentIndex) != nullptr;
}

void pluginRegistryConfigureLifecycle(uint32_t manifestIndex, void* config) {
    pluginRegistryConfigureLifecycleComponent(manifestIndex, 0, config);
}

void pluginRegistryConfigureLifecycleComponent(uint32_t manifestIndex,
                                               uint32_t componentIndex,
                                               void* config) {
    const EdvrPluginLifecycleOps* const ops =
        lifecycleForComponent(manifestIndex, componentIndex);
    if (!ops) return;
    if (lifecycleFieldPresent<EdvrPluginConfigureStateFn>(
            ops, offsetof(EdvrPluginLifecycleOps, configureState)) &&
        ops->configureState) {
        ops->configureState(ops->state, config);
    } else if (ops->configure) {
        ops->configure(config);
    }
}

void pluginRegistryFrameLifecycle(uint32_t manifestIndex, uint32_t sceneFrame) {
    pluginRegistryFrameLifecycleComponent(manifestIndex, 0, sceneFrame);
}

void pluginRegistryFrameLifecycleComponent(uint32_t manifestIndex,
                                           uint32_t componentIndex,
                                           uint32_t sceneFrame) {
    const EdvrPluginLifecycleOps* const ops =
        lifecycleForComponent(manifestIndex, componentIndex);
    if (ops) ops->frame(ops->state, sceneFrame);
}

void pluginRegistryShutdownLifecycle(uint32_t manifestIndex) {
    pluginRegistryShutdownLifecycleComponent(manifestIndex, 0);
}

void pluginRegistryShutdownLifecycleComponent(uint32_t manifestIndex,
                                             uint32_t componentIndex) {
    if (manifestIndex >= kMaxPlugins ||
        componentIndex >= kPluginLifecycleComponentCount) return;
    const EdvrPluginLifecycleOps* const ops =
        g_lifecyclePlugins[manifestIndex][componentIndex];
    if (!ops) return;
    // Detach first so a callback that re-enters shutdown is harmless.
    g_lifecyclePlugins[manifestIndex][componentIndex] = nullptr;
    ops->shutdown(ops->state);
}

bool pluginRegistryConfigureLifecycleStage(uint32_t manifestIndex,
                                           uint32_t stage, void* config) {
    return pluginRegistryConfigureLifecycleComponentStage(manifestIndex, 0,
                                                           stage, config);
}

bool pluginRegistryConfigureLifecycleComponentStage(uint32_t manifestIndex,
                                                    uint32_t componentIndex,
                                                    uint32_t stage,
                                                    void* config) {
    const EdvrPluginLifecycleOps* const ops =
        lifecycleForComponent(manifestIndex, componentIndex);
    if (!lifecycleFieldPresent<EdvrPluginConfigureStageFn>(
            ops, offsetof(EdvrPluginLifecycleOps, configureStage)) ||
        !ops->configureStage)
        return false;
    ops->configureStage(ops->state, stage, config);
    return true;
}

bool pluginRegistryFrameLifecycleStage(uint32_t manifestIndex, uint32_t stage,
                                       ID3D11DeviceContext* context,
                                       uint32_t sceneFrame) {
    return pluginRegistryFrameLifecycleComponentStage(manifestIndex, 0, stage,
                                                       context, sceneFrame);
}

bool pluginRegistryFrameLifecycleComponentStage(
    uint32_t manifestIndex, uint32_t componentIndex, uint32_t stage,
    ID3D11DeviceContext* context, uint32_t sceneFrame) {
    const EdvrPluginLifecycleOps* const ops =
        lifecycleForComponent(manifestIndex, componentIndex);
    if (!lifecycleFieldPresent<EdvrPluginFrameStageFn>(
            ops, offsetof(EdvrPluginLifecycleOps, frameStage)) ||
        !ops->frameStage)
        return false;
    ops->frameStage(ops->state, stage, context, sceneFrame);
    return true;
}

bool pluginRegistryShutdownLifecycleStage(uint32_t manifestIndex,
                                          uint32_t stage) {
    return pluginRegistryShutdownLifecycleComponentStage(manifestIndex, 0,
                                                          stage);
}

bool pluginRegistryShutdownLifecycleComponentStage(uint32_t manifestIndex,
                                                   uint32_t componentIndex,
                                                   uint32_t stage) {
    const EdvrPluginLifecycleOps* const ops =
        lifecycleForComponent(manifestIndex, componentIndex);
    if (!lifecycleFieldPresent<EdvrPluginShutdownStageFn>(
            ops, offsetof(EdvrPluginLifecycleOps, shutdownStage)) ||
        !ops->shutdownStage)
        return false;
    ops->shutdownStage(ops->state, stage);
    return true;
}

bool pluginRegistryProfileSupports(uint32_t manifestIndex) {
    if (!runtimeFeaturesAllowed() || manifestIndex >= plugins::kPluginCount) return false;
    const uint32_t profile = runtimeFlatProfile() ? plugins::kProfileFlat
        : runtimeVrProfile() ? plugins::kProfileVr : 0u;
    return profile && (plugins::kManifest[manifestIndex].profileMask & profile) != 0;
}

bool pluginRegistryWantsStartupHooks(void* config) {
    for (uint32_t i = 0; i < g_registeredPluginCount; ++i) {
        const EdvrPluginOps* ops = g_registeredPlugins[i];
        if (ops && ops->startupHooksWanted && ops->startupHooksWanted(config)) return true;
    }
    return false;
}

bool pluginRegistryRegisterLegacyDrawGate(const char* stableName,
                                          EdvrLegacyDrawGateFn predicate,
                                          void* state) {
    if (!stableName || !*stableName || !predicate || g_legacyCount >= kMaxLegacySubscribers)
        return false;
    for (uint32_t i = 0; i < g_legacyCount; ++i)
        if (std::strcmp(g_legacy[i].name, stableName) == 0) return false;
    g_legacy[g_legacyCount++] = {stableName, predicate, state};
    return true;
}

bool pluginRegistryBeginLegacyDrawGateTest() noexcept {
    if (g_legacyDrawGateTestActive) return false;
    std::memcpy(g_testSavedLegacy, g_legacy, sizeof(g_legacy));
    g_testSavedLegacyCount = g_legacyCount;
    g_testSavedDrawGateFailOpen = g_drawGateFailOpen;
    g_legacyDrawGateTestActive = true;
    g_legacyCount = 0;
    g_drawGateFailOpen = false;
    return true;
}

void pluginRegistryEndLegacyDrawGateTest() noexcept {
    if (!g_legacyDrawGateTestActive) return;
    std::memcpy(g_legacy, g_testSavedLegacy, sizeof(g_legacy));
    g_legacyCount = g_testSavedLegacyCount;
    g_drawGateFailOpen = g_testSavedDrawGateFailOpen;
    g_legacyDrawGateTestActive = false;
}

uint32_t pluginRegistryVisitLegacyDrawGatesForTest(void* legacyState) noexcept {
    uint32_t visited = 0;
    for (uint32_t i = 0; i < g_legacyCount; ++i) {
        const LegacySubscriber& gate = g_legacy[i];
        if (gate.predicate) {
            (void)gate.predicate(gate.state ? gate.state : legacyState);
            ++visited;
        }
    }
    return visited;
}

void pluginRegistrySetDrawGateFailOpen() { g_drawGateFailOpen = true; }

void pluginRegistryConfigure(void* config) {
    // Pause observation while callbacks update module state. The binding
    // shadow remains canonical and is sampled again after the active mask is
    // rebuilt below.
    bindingShadowSetShaderObserver(nullptr);
    for (uint32_t i = 0; i < g_registeredPluginCount; ++i)
        if (g_registeredPlugins[i]->configure) g_registeredPlugins[i]->configure(config);
    g_pluginsConfigured = true;
    rebuildActivePluginMask();
    updateBindingObserver();
}

void pluginRegistryShutdown() {
    bindingShadowSetShaderObserver(nullptr);
    pluginRegistryReportActivity();
    for (uint32_t i = g_registeredPluginCount; i > 0; --i) {
        const EdvrPluginOps* ops = g_registeredPlugins[i - 1];
        if (ops && ops->shutdown) ops->shutdown(ops->state);
    }
    for (uint32_t i = 0; i < kMaxPlugins; ++i) {
        g_plugins[i] = nullptr;
        g_registeredPlugins[i] = nullptr;
    }
    g_registeredPluginCount = 0;
    g_shaderClaimCount = 0;
    g_legacyCount = 0;
    g_drawGateFailOpen = false;
    g_notedShaderCandidate = false;
    g_notedCandidateDraw = false;
    g_pendingShaderCandidateReport = false;
    g_pendingCandidateDrawReport = false;
    g_activePluginMask = 0;
    g_pluginsConfigured = false;
    g_vsHash = g_psHash = 0;
    detail::g_pluginShaderCandidates.store(0, std::memory_order_relaxed);
    g_configuredLegacyInterests = 0;
    g_legacyShaderFilterCount = 0;
    std::memset(g_legacyShaderFilters, 0, sizeof(g_legacyShaderFilters));
    detail::g_legacyDrawInterestMask.store(0, std::memory_order_relaxed);
}

void pluginRegistryOnShaderBind(uint64_t vsHash, uint64_t psHash) {
    if (vsHash == g_vsHash && psHash == g_psHash) return;
    g_vsHash = vsHash;
    g_psHash = psHash;
    rebuildCandidates();
}

void pluginRegistryRefreshShaderCandidates() {
    rebuildActivePluginMask();
    updateBindingObserver();
}

bool pluginRegistryConfigureDrawInterests(
    draw_interest::InterestMask configuredMask,
    const draw_interest::ShaderFilter* filters, size_t filterCount) noexcept {
    constexpr draw_interest::InterestMask kValidInterestBits =
        (draw_interest::InterestMask{1} << draw_interest::kInterestCount) - 1;
    bool accepted = true;
    if ((configuredMask & ~kValidInterestBits) != 0 ||
        filterCount > kMaxLegacyShaderFilters || (filterCount != 0 && !filters)) {
        // Malformed metadata must fail open to the legacy predicates: preserve
        // configured interest but disable only the optimization filters.
        accepted = false;
        filterCount = 0;
        filters = nullptr;
        configuredMask &= kValidInterestBits;
    }
    if (!runtimeVrProfile()) {
        configuredMask = 0;
        filterCount = 0;
    }

    g_configuredLegacyInterests = configuredMask;
    g_legacyShaderFilterCount = filterCount;
    if (filterCount) std::memcpy(g_legacyShaderFilters, filters,
                                 filterCount * sizeof(g_legacyShaderFilters[0]));
    if (filterCount < kMaxLegacyShaderFilters)
        std::memset(g_legacyShaderFilters + filterCount, 0,
                    (kMaxLegacyShaderFilters - filterCount) * sizeof(g_legacyShaderFilters[0]));
    updateBindingObserver();
    return accepted;
}

void pluginRegistryReportActivity() {
    if (g_pendingShaderCandidateReport) {
        g_pendingShaderCandidateReport = false;
        Log::get().note("plugin registry: night-vision shader pair observed; cached draw candidate ready");
    }
    if (g_pendingCandidateDrawReport) {
        g_pendingCandidateDrawReport = false;
        Log::get().note("plugin registry: night-vision candidate draw reached; evaluating live claim");
    }
}

uint32_t pluginRegistryResolveDraw(uint64_t candidates, uint8_t kind,
                                   uint32_t count, uint32_t instances) {
    if (!plugins::dispatch::hasCandidate(candidates, plugins::kPluginCockpitVisuals))
        return kPluginClaimNone;
    if (!g_notedCandidateDraw &&
        plugins::dispatch::hasCandidate(candidates, plugins::kPluginCockpitVisuals)) {
        g_notedCandidateDraw = true;
        g_pendingCandidateDrawReport = true;
    }
    const EdvrPluginOps* ops = g_plugins[plugins::kPluginCockpitVisuals];
    return ops && ops->claimDraw
        ? ops->claimDraw(ops->state, kNightVisionClaimId, kind, count, instances)
        : kPluginClaimNone;
}

uint32_t pluginRegistryResolveDrawObserved(
    uint64_t candidates, uint8_t kind, uint32_t count, uint32_t instances,
    EdvrPluginClaimObservation* observation) {
    if (!plugins::dispatch::hasCandidate(candidates, plugins::kPluginCockpitVisuals))
        return kPluginClaimNone;
    if (!g_notedCandidateDraw) {
        g_notedCandidateDraw = true;
        g_pendingCandidateDrawReport = true;
    }
    const EdvrPluginOps* ops = g_plugins[plugins::kPluginCockpitVisuals];
    if (!ops || !ops->claimDraw) return kPluginClaimNone;
    if (observation) *observation = {};
    if (ops->claimDrawObserved && observation) {
        const uint32_t claim = ops->claimDrawObserved(
            ops->state, kNightVisionClaimId, kind, count, instances, observation);
        observation->observed = 1;
        return claim;
    }
    return ops->claimDraw(ops->state, kNightVisionClaimId, kind, count, instances);
}

uint64_t pluginRegistryActivePluginMaskForTrace() noexcept {
    return g_activePluginMask;
}

bool pluginRegistryTraceMode(uint8_t* mode) noexcept {
    if (!mode) return false;
    const EdvrPluginOps* ops = g_plugins[plugins::kPluginCockpitVisuals];
    return ops && ops->traceMode && ops->traceMode(ops->state, mode) != 0;
}

void pluginRegistryBegin(uint32_t claim, ID3D11DeviceContext* context) {
    const EdvrPluginOps* ops = pluginForClaim(claim);
    if (ops && ops->begin) ops->begin(ops->state, kNightVisionClaimId, context);
}

void pluginRegistryEnd(uint32_t claim, ID3D11DeviceContext* context) {
    const EdvrPluginOps* ops = pluginForClaim(claim);
    if (ops && ops->end) ops->end(ops->state, kNightVisionClaimId, context);
}

bool pluginRegistryWantsDraws(void* legacyState) {
    if (g_drawGateFailOpen) return true;
    for (uint32_t i = 0; i < g_legacyCount; ++i)
        if (g_legacy[i].predicate(g_legacy[i].state ? g_legacy[i].state : legacyState))
            return true;
    for (uint32_t i = 0; i < g_registeredPluginCount; ++i) {
        const EdvrPluginOps* ops = g_registeredPlugins[i];
        if (ops && ops->wantsDraws && ops->wantsDraws(ops->state)) return true;
    }
    return false;
}

}  // namespace edvr
