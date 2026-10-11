#include "plugin_admission_commit.h"

#include "plugin_manifest.inc"

#include <array>
#include <cstring>

namespace edvr::plugin_selection::commit_v1 {
namespace {
using admission_v1::GraphicsContractV1;
using admission_v1::DecisionV1;

std::uint64_t providerBase(const DecisionV1& decision) noexcept {
    return (std::uint64_t(decision.graphicsProviderBaseHigh) << 32) |
        decision.graphicsProviderBaseLow;
}

CatalogInternalV1 generatedCatalog() noexcept {
    static_assert(plugins::kPluginCount == plugins::kPluginIndexCount &&
                  plugins::kPluginCount < 32, "generated mask index bounds");
    static constexpr std::array<CatalogGroupInternalV1, plugins::kPluginCount> groups = []() constexpr {
        std::array<CatalogGroupInternalV1, plugins::kPluginCount> out{};
        for (std::uint32_t i = 0; i < plugins::kPluginCount; ++i)
            out[i] = {plugins::kManifest[i].index, plugins::kManifest[i].profileMask,
                      plugins::kManifest[i].selectionAvailableProfileMask};
        return out;
    }();
    return {groups.data(), plugins::kPluginCount, plugins::kSelectionCatalogSchemaVersion,
            plugins::kSelectionCatalogFingerprint};
}

GraphicsContractV1 nativeContract(CatalogInternalV1 catalog) noexcept {
#ifdef EDVR_SELECTION_ADMISSION_TEST
    std::array<admission_v1::test_seam::GroupV1, 32> groups{};
    if (!catalog.groups || catalog.count > groups.size()) return {};
    for (std::uint32_t i = 0; i < catalog.count; ++i)
        groups[i] = {catalog.groups[i].index, catalog.groups[i].profileMask,
                     catalog.groups[i].readyMask};
    return admission_v1::test_seam::contractWithCatalog(
        {groups.data(), catalog.count, catalog.schema, catalog.fingerprint});
#else
    (void)catalog;
    return admission_v1::contract();
#endif
}

bool validRequest(const AdmissionCommitRequestV1& request, CatalogInternalV1 catalog) noexcept {
    if (request.structSize != sizeof(request) || request.abiVersion != kAbiVersion ||
        request.requiredCapabilities != kRequiredCapabilities || request.reserved != 0 ||
        request.supportedProfileMask != nativeContract(catalog).supportedProfileMask)
        return false;

    const auto profile = static_cast<PluginSelectionProfileV1>(request.activeProfile);
    const std::uint64_t provider = providerBase(request.decision);
    if (provider == 0) return false;

    DecisionV1 expected{};
#ifdef EDVR_SELECTION_ADMISSION_TEST
    std::array<admission_v1::test_seam::GroupV1, 32> groups{};
    if (!catalog.groups || catalog.count > groups.size()) return false;
    for (std::uint32_t i = 0; i < catalog.count; ++i)
        groups[i] = {catalog.groups[i].index, catalog.groups[i].profileMask,
                     catalog.groups[i].readyMask};
    expected = admission_v1::test_seam::decideWithCatalog(
        S_OK, &request.snapshot, profile, provider,
        {groups.data(), catalog.count, catalog.schema, catalog.fingerprint});
#else
    (void)catalog;
    expected = admission_v1::decide(S_OK, &request.snapshot, profile, provider);
#endif
    return expected.mode == static_cast<std::uint32_t>(admission_v1::Mode::AdmittedSelection) &&
        std::memcmp(&expected, &request.decision, sizeof(expected)) == 0;
}

bool validPeerContract(const GraphicsContractV1& actual, CatalogInternalV1 catalog) noexcept {
    const GraphicsContractV1 expected = nativeContract(catalog);
    return std::memcmp(&actual, &expected, sizeof(expected)) == 0;
}

} // namespace

CommitStoreV1::~CommitStoreV1() noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (state == State::Committed && retainedBase_ != 0 && retainedHooks_.releaseGraphics)
        retainedHooks_.releaseGraphics(retainedHooks_.context, retainedBase_);
}

HRESULT CommitStoreV1::commit(const AdmissionCommitRequestV1& request,
                              const PeerHooksV1& hooks) noexcept {
    return commitImpl(request, generatedCatalog(), hooks);
}

HRESULT CommitStoreV1::commitImpl(const AdmissionCommitRequestV1& request,
                                  CatalogInternalV1 catalog,
                                  const PeerHooksV1& hooks) noexcept {
    // Callbacks execute outside the store and may be supplied by another DLL.
    // Freeze both caller-owned values before reserving or invoking any callback.
    const AdmissionCommitRequestV1 frozenRequest = request;
    const PeerHooksV1 frozenHooks = hooks;
    State expectedState = State::Empty;
    if (!state_.compare_exchange_strong(expectedState, State::Preparing,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        if (expectedState == State::Preparing) return E_PENDING;
        if (expectedState == State::Committed)
            return std::memcmp(&committed_, &frozenRequest, sizeof(frozenRequest)) == 0 ?
                S_OK : E_ACCESSDENIED;
        return E_UNEXPECTED;
    }

    HRESULT result = E_ACCESSDENIED;
    std::uint64_t retainedBase = 0;
    const std::uint64_t provider = providerBase(frozenRequest.decision);
    if (!validRequest(frozenRequest, catalog)) {
        result = E_INVALIDARG;
    } else if (!frozenHooks.queryExportOwnModule || !frozenHooks.loadedGraphicsIsExpected ||
               !frozenHooks.queryContract || !frozenHooks.retainGraphics || !frozenHooks.releaseGraphics) {
        result = E_INVALIDARG;
    } else if (frozenHooks.retainGraphics(frozenHooks.context, provider)) {
        // Keep this image mapped across all identity and contract checks. The
        // individual peer queries take only temporary references of their own.
        retainedBase = provider;
        GraphicsContractV1 peerContract{};
        if (frozenHooks.loadedGraphicsIsExpected(frozenHooks.context, provider) &&
            frozenHooks.queryExportOwnModule(frozenHooks.context, provider) &&
            frozenHooks.queryContract(frozenHooks.context, provider, &peerContract) == S_OK &&
            validPeerContract(peerContract, catalog)) {
            result = S_OK;
        }
    }

    if (result == S_OK) {
        committed_ = frozenRequest;
        retainedBase_ = retainedBase;
        retainedHooks_ = frozenHooks;
        state_.store(State::Committed, std::memory_order_release);
        return S_OK;
    }

    if (retainedBase != 0)
        frozenHooks.releaseGraphics(frozenHooks.context, retainedBase);
    state_.store(State::Empty, std::memory_order_release);
    return result;
}

bool CommitStoreV1::read(AdmissionCommitRequestV1* out) const noexcept {
    if (!out || state_.load(std::memory_order_acquire) != State::Committed) return false;
    *out = committed_;
    return true;
}

#ifdef EDVR_ADMISSION_COMMIT_TEST
HRESULT CommitStoreV1::commitWithCatalogForTest(
    const AdmissionCommitRequestV1& request,
    admission_v1::test_seam::CatalogViewV1 catalog,
    const PeerHooksV1& hooks) noexcept {
    std::array<CatalogGroupInternalV1, 32> groups{};
    if (!catalog.groups || catalog.count > groups.size()) return E_INVALIDARG;
    for (std::uint32_t i = 0; i < catalog.count; ++i)
        groups[i] = {catalog.groups[i].index, catalog.groups[i].profileMask,
                     catalog.groups[i].readyMask};
    return commitImpl(request, {groups.data(), catalog.count, catalog.schema,
                                catalog.fingerprint}, hooks);
}

void CommitStoreV1::resetForTest() noexcept {
    const State state = state_.exchange(State::Empty, std::memory_order_acq_rel);
    if (state == State::Committed && retainedBase_ != 0 && retainedHooks_.releaseGraphics)
        retainedHooks_.releaseGraphics(retainedHooks_.context, retainedBase_);
    retainedBase_ = 0;
    retainedHooks_ = {};
    committed_ = {};
}
#endif

} // namespace edvr::plugin_selection::commit_v1
