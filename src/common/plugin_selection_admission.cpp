#include "plugin_selection_admission.h"
#include "plugin_manifest.inc"

#include <array>
#include <cstring>

namespace edvr::plugin_selection::admission_v1 {
static_assert(plugins::kVrProfileBit == kPluginSelectionVrProfileMaskV1 &&
              plugins::kFlatProfileBit == kPluginSelectionFlatProfileMaskV1,
              "public profile-mask bits must match the generated catalog");
namespace {
#ifdef EDVR_SELECTION_ADMISSION_TEST
using GroupView = test_seam::GroupV1;
using CatalogView = test_seam::CatalogViewV1;
#else
struct GroupView { std::uint32_t index, profileMask, readyMask; };
struct CatalogView {
    const GroupView* groups;
    std::uint32_t count, schema;
    const std::uint8_t* fingerprint;
};
#endif

std::uint32_t profileBit(PluginSelectionProfileV1 profile) noexcept {
    if (profile == PluginSelectionProfileV1::Vr) return plugins::kVrProfileBit;
    if (profile == PluginSelectionProfileV1::Flat) return plugins::kFlatProfileBit;
    return 0;
}

bool validCatalog(CatalogView catalog) noexcept {
    if (!catalog.groups || !catalog.fingerprint || catalog.count == 0 || catalog.count >= 32)
        return false;
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        const auto& group = catalog.groups[i];
        if (group.index != i || (group.profileMask & ~3u) != 0 ||
            (group.readyMask & ~group.profileMask) != 0) return false;
    }
    return true;
}

bool validSupportedSnapshot(const PluginSelectionSnapshotV1& snapshot,
                            PluginSelectionProfileV1 activeProfile,
                            CatalogView catalog) noexcept {
    if (!validCatalog(catalog) || snapshot.structSize != sizeof(snapshot) ||
        snapshot.abiVersion != kPluginSelectionSnapshotVersionV1 ||
        snapshot.recordState != static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported) ||
        !profileBit(activeProfile) ||
        snapshot.installedProfile != static_cast<std::uint32_t>(activeProfile) ||
        snapshot.recordProfile != static_cast<std::uint32_t>(activeProfile) ||
        snapshot.recordSchemaVersion != 1 || snapshot.recordCatalogSchemaVersion != catalog.schema ||
        snapshot.compiledCatalogSchemaVersion != catalog.schema || snapshot.validationFlags != 0 ||
        snapshot.reserved[0] != 0 || snapshot.reserved[1] != 0 ||
        (snapshot.recordedPluginMask & ~((1u << catalog.count) - 1u)) != 0 ||
        std::memcmp(snapshot.catalogFingerprint, catalog.fingerprint, 32) != 0) return false;

    const std::uint32_t bit = profileBit(activeProfile);
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        if ((snapshot.recordedPluginMask & (1u << i)) &&
            (!(catalog.groups[i].profileMask & bit) || !(catalog.groups[i].readyMask & bit)))
            return false;
    }
    return true;
}

bool allProfileGroupsReady(PluginSelectionProfileV1 profile, CatalogView catalog) noexcept {
    if (!validCatalog(catalog)) return false;
    const std::uint32_t bit = profileBit(profile);
    if (!bit) return false;
    bool any = false;
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        const auto& group = catalog.groups[i];
        if (!(group.profileMask & bit)) continue;
        any = true;
        if (!(group.readyMask & bit)) return false;
    }
    return any;
}

CatalogView generatedCatalog() noexcept {
    static_assert(plugins::kPluginCount == plugins::kPluginIndexCount &&
                  plugins::kPluginCount < 32, "generated mask index bounds");
    static constexpr std::array<GroupView, plugins::kPluginCount> groups = []() constexpr {
        std::array<GroupView, plugins::kPluginCount> out{};
        for (std::uint32_t i = 0; i < plugins::kPluginCount; ++i)
            out[i] = {plugins::kManifest[i].index, plugins::kManifest[i].profileMask,
                      plugins::kManifest[i].selectionAvailableProfileMask};
        return out;
    }();
    return {groups.data(), plugins::kPluginCount, plugins::kSelectionCatalogSchemaVersion,
            plugins::kSelectionCatalogFingerprint};
}

std::uint32_t supportedProfiles(CatalogView catalog) noexcept {
    std::uint32_t result = 0;
    if (allProfileGroupsReady(PluginSelectionProfileV1::Vr, catalog)) result |= plugins::kVrProfileBit;
    if (allProfileGroupsReady(PluginSelectionProfileV1::Flat, catalog)) result |= plugins::kFlatProfileBit;
    return result;
}

GraphicsContractV1 makeContract(CatalogView catalog) noexcept {
    GraphicsContractV1 out{};
    out.structSize = sizeof(out);
    out.abiVersion = kAbiVersion;
    out.snapshotAbiVersion = kPluginSelectionSnapshotVersionV1;
    out.selectionSchemaVersion = 1;
    out.catalogSchemaVersion = catalog.schema;
    out.supportedProfileMask = supportedProfiles(catalog);
    out.capabilities = kCapabilities;
    if (catalog.fingerprint) std::memcpy(out.catalogFingerprint, catalog.fingerprint, 32);
    return out;
}

DecisionV1 makeDecision(HRESULT captureStatus, const PluginSelectionSnapshotV1* snapshot,
                        PluginSelectionProfileV1 activeProfile, std::uint64_t provider,
                        CatalogView catalog) noexcept {
    DecisionV1 out{};
    out.structSize = sizeof(out);
    out.abiVersion = kAbiVersion;
    out.mode = static_cast<std::uint32_t>(Mode::Legacy);
    out.activeProfile = static_cast<std::uint32_t>(activeProfile);
    out.captureStatus = captureStatus;
    out.graphicsProviderBaseLow = static_cast<std::uint32_t>(provider);
    out.graphicsProviderBaseHigh = static_cast<std::uint32_t>(provider >> 32);
    if (captureStatus != S_OK || !snapshot) {
        out.reason = static_cast<std::uint32_t>(Reason::SnapshotUnavailable);
        return out;
    }
    out.recordState = snapshot->recordState;
    out.validationFlags = snapshot->validationFlags;
    out.selectedPluginMask = snapshot->recordedPluginMask;
    if (!validSupportedSnapshot(*snapshot, activeProfile, catalog)) {
        out.reason = static_cast<std::uint32_t>(Reason::InvalidSnapshot);
        return out;
    }
    if (!allProfileGroupsReady(activeProfile, catalog)) {
        out.reason = static_cast<std::uint32_t>(Reason::ProfileNotReady);
        return out;
    }
    if (provider == 0) {
        out.reason = static_cast<std::uint32_t>(Reason::ProviderRejected);
        return out;
    }
    out.mode = static_cast<std::uint32_t>(Mode::AdmittedSelection);
    return out;
}

bool selectedForCatalog(const DecisionV1& decision, std::uint32_t groupIndex,
                        CatalogView catalog) noexcept {
    if (decision.structSize != sizeof(decision) || decision.abiVersion != kAbiVersion ||
        decision.mode != static_cast<std::uint32_t>(Mode::AdmittedSelection) ||
        decision.reason != static_cast<std::uint32_t>(Reason::None) ||
        decision.captureStatus != S_OK ||
        (decision.graphicsProviderBaseLow == 0 && decision.graphicsProviderBaseHigh == 0) ||
        !validCatalog(catalog) ||
        groupIndex >= catalog.count || groupIndex >= 32) return false;
    for (std::uint32_t i = 0; i < 5; ++i) if (decision.reserved[i] != 0) return false;
    if (decision.recordState != static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported) ||
        decision.validationFlags != 0 ||
        (decision.selectedPluginMask & ~((1u << catalog.count) - 1u)) != 0) return false;
    const std::uint32_t bit = profileBit(
        static_cast<PluginSelectionProfileV1>(decision.activeProfile));
    if (!bit || !allProfileGroupsReady(
            static_cast<PluginSelectionProfileV1>(decision.activeProfile), catalog)) return false;
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        if ((decision.selectedPluginMask & (1u << i)) &&
            (!(catalog.groups[i].profileMask & bit) || !(catalog.groups[i].readyMask & bit)))
            return false;
    }
    const auto& group = catalog.groups[groupIndex];
    return group.index == groupIndex && (group.profileMask & bit) &&
        (group.readyMask & bit) && (decision.selectedPluginMask & (1u << groupIndex));
}

bool ownerEnabledForCatalog(const DecisionV1& decision, std::uint32_t groupIndex,
                            CatalogView catalog) noexcept {
    if (decision.structSize != sizeof(decision) || decision.abiVersion != kAbiVersion ||
        (decision.mode != static_cast<std::uint32_t>(Mode::Legacy) &&
         decision.mode != static_cast<std::uint32_t>(Mode::AdmittedSelection)) ||
        decision.reason > static_cast<std::uint32_t>(Reason::ProviderRejected) ||
        !validCatalog(catalog) || groupIndex >= catalog.count || groupIndex >= 32) return false;
    for (std::uint32_t i = 0; i < 5; ++i) if (decision.reserved[i] != 0) return false;
    const std::uint32_t bit = profileBit(
        static_cast<PluginSelectionProfileV1>(decision.activeProfile));
    const auto& group = catalog.groups[groupIndex];
    if (!bit || group.index != groupIndex || !(group.profileMask & bit)) return false;
    if (decision.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection))
        return selectedForCatalog(decision, groupIndex, catalog);

    const auto maskIsBounded = [&]() noexcept {
        return (decision.selectedPluginMask & ~((1u << catalog.count) - 1u)) == 0;
    };
    if (decision.reason == static_cast<std::uint32_t>(Reason::SnapshotUnavailable))
        return decision.recordState == 0 && decision.validationFlags == 0 &&
            decision.selectedPluginMask == 0;
    // Invalid source data never controls Legacy owner behavior.
    if (decision.reason == static_cast<std::uint32_t>(Reason::InvalidSnapshot))
        return decision.captureStatus == S_OK;
    if (decision.reason == static_cast<std::uint32_t>(Reason::ProfileNotReady) ||
        decision.reason == static_cast<std::uint32_t>(Reason::ProviderRejected)) {
        if (decision.captureStatus != S_OK || !maskIsBounded() ||
            decision.recordState != static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported) ||
            decision.validationFlags != 0) return false;
        for (std::uint32_t i = 0; i < catalog.count; ++i) {
            if ((decision.selectedPluginMask & (1u << i)) &&
                (!(catalog.groups[i].profileMask & bit) || !(catalog.groups[i].readyMask & bit)))
                return false;
        }
        if (decision.reason == static_cast<std::uint32_t>(Reason::ProfileNotReady))
            return !allProfileGroupsReady(static_cast<PluginSelectionProfileV1>(decision.activeProfile), catalog);
        return allProfileGroupsReady(static_cast<PluginSelectionProfileV1>(decision.activeProfile), catalog) &&
            decision.graphicsProviderBaseLow == 0 && decision.graphicsProviderBaseHigh == 0;
    }
    return false;
}

bool ownerAllowedAtStartupForCatalog(const DecisionV1& decision,
                                    std::uint32_t groupIndex,
                                    CatalogView catalog) noexcept {
    if (decision.structSize != sizeof(decision) || decision.abiVersion != kAbiVersion ||
        (decision.mode != static_cast<std::uint32_t>(Mode::Legacy) &&
         decision.mode != static_cast<std::uint32_t>(Mode::AdmittedSelection)) ||
        decision.reason > static_cast<std::uint32_t>(Reason::ProviderRejected) ||
        !validCatalog(catalog) || groupIndex >= catalog.count || groupIndex >= 32)
        return false;
    for (std::uint32_t i = 0; i < 5; ++i)
        if (decision.reserved[i] != 0) return false;

    if (decision.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection))
        return ownerEnabledForCatalog(decision, groupIndex, catalog);

    // Legacy InvalidSnapshot deliberately ignores recordState,
    // validationFlags, and selectedPluginMask: those facts came from the
    // rejected source record and are not authority. The reason-specific
    // envelope still has to match a decision this producer can emit.
    switch (static_cast<Reason>(decision.reason)) {
    case Reason::SnapshotUnavailable:
        // The source pointer is not carried on the wire: decide() also emits
        // this reason for S_OK plus a null snapshot, so only the zeroed facts
        // distinguish a valid unavailable envelope from malformed data.
        return decision.recordState == 0 &&
            decision.validationFlags == 0 && decision.selectedPluginMask == 0;
    case Reason::InvalidSnapshot:
        return decision.captureStatus == S_OK;
    case Reason::ProfileNotReady:
    case Reason::ProviderRejected: {
        if (decision.captureStatus != S_OK ||
            decision.recordState != static_cast<std::uint32_t>(
                PluginSelectionSnapshotStateV1::Supported) || decision.validationFlags != 0 ||
            (decision.selectedPluginMask & ~((1u << catalog.count) - 1u)) != 0)
            return false;
        const auto profile = static_cast<PluginSelectionProfileV1>(decision.activeProfile);
        const std::uint32_t bit = profileBit(profile);
        if (!bit) return false;
        for (std::uint32_t i = 0; i < catalog.count; ++i) {
            if ((decision.selectedPluginMask & (1u << i)) &&
                (!(catalog.groups[i].profileMask & bit) ||
                 !(catalog.groups[i].readyMask & bit))) return false;
        }
        const bool profileReady = allProfileGroupsReady(profile, catalog);
        if (decision.reason == static_cast<std::uint32_t>(Reason::ProfileNotReady))
            return !profileReady;
        return profileReady && decision.graphicsProviderBaseLow == 0 &&
            decision.graphicsProviderBaseHigh == 0;
    }
    case Reason::None:
        return false;
    }
    return false;
}

} // namespace

GraphicsContractV1 contract() noexcept { return makeContract(generatedCatalog()); }

DecisionV1 decide(HRESULT captureStatus, const PluginSelectionSnapshotV1* snapshot,
                  PluginSelectionProfileV1 activeProfile,
                  std::uint64_t graphicsProviderBase) noexcept {
    return makeDecision(captureStatus, snapshot, activeProfile, graphicsProviderBase,
                        generatedCatalog());
}

bool selected(const DecisionV1& decision, std::uint32_t generatedGroupIndex) noexcept {
    return selectedForCatalog(decision, generatedGroupIndex, generatedCatalog());
}

bool ownerEnabled(const DecisionV1& decision, std::uint32_t generatedGroupIndex) noexcept {
    return ownerEnabledForCatalog(decision, generatedGroupIndex, generatedCatalog());
}

bool ownerAllowedForStartup(const DecisionV1& decision,
                            std::uint32_t generatedGroupIndex) noexcept {
    return ownerAllowedAtStartupForCatalog(decision, generatedGroupIndex,
                                           generatedCatalog());
}

#ifdef EDVR_SELECTION_ADMISSION_TEST
namespace test_seam {
GraphicsContractV1 contractWithCatalog(CatalogViewV1 catalog) noexcept {
    return makeContract(catalog);
}
DecisionV1 decideWithCatalog(HRESULT captureStatus,
                             const PluginSelectionSnapshotV1* snapshot,
                             PluginSelectionProfileV1 activeProfile,
                             std::uint64_t graphicsProviderBase,
                             CatalogViewV1 catalog) noexcept {
    return makeDecision(captureStatus, snapshot, activeProfile, graphicsProviderBase, catalog);
}
bool selectedWithCatalog(const DecisionV1& decision, std::uint32_t groupIndex,
                         CatalogViewV1 catalog) noexcept {
    return selectedForCatalog(decision, groupIndex, catalog);
}
bool ownerEnabledWithCatalog(const DecisionV1& decision, std::uint32_t groupIndex,
                             CatalogViewV1 catalog) noexcept {
    return ownerEnabledForCatalog(decision, groupIndex, catalog);
}
bool ownerAllowedForStartupWithCatalog(const DecisionV1& decision,
                                       std::uint32_t groupIndex,
                                       CatalogViewV1 catalog) noexcept {
    return ownerAllowedAtStartupForCatalog(decision, groupIndex, catalog);
}
} // namespace test_seam
#endif

} // namespace edvr::plugin_selection::admission_v1
