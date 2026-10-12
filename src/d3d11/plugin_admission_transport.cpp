#include "plugin_admission_transport.h"

#include "plugin_selection_transport.h"
#include "../common/plugin_admission_commit.h"
#include "../common/plugin_admission_peer.h"
#include "../common/log.h"
#include "plugin_manifest.inc"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;
using namespace edvr::plugin_selection::commit_v1;

DecisionV1 g_publishedDecision{};
std::atomic<bool> g_decisionReady{false};

std::uint64_t moduleBase(HMODULE module) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(module));
}

std::wstring peerPath(const std::wstring& moduleDirectory) {
    return moduleDirectory + L"\\Openvr\\win64\\openvr_api.dll";
}

DecisionV1 candidateWithNativeCommit(const std::wstring& moduleDirectory,
                                     PluginSelectionProfileV1 profile,
                                     HRESULT captureStatus,
                                     const PluginSelectionSnapshotV1& snapshot,
                                     DecisionV1 candidate) noexcept {
    if (profile != PluginSelectionProfileV1::Vr ||
        candidate.mode != static_cast<std::uint32_t>(Mode::AdmittedSelection))
        return candidate;

    edvr::plugin_admission::AdmissionPeer peer;
    try {
        peer = edvr::plugin_admission::AdmissionPeer::acquire(
            peerPath(moduleDirectory), "edvrCommitPluginSelectionAdmissionV1");
    } catch (...) {
        return decide(captureStatus, &snapshot, profile, 0);
    }
    if (!peer) return decide(captureStatus, &snapshot, profile, 0);

    const auto graphicsContract = contract();
    AdmissionCommitRequestV1 request{};
    request.structSize = sizeof(request);
    request.abiVersion = commit_v1::kAbiVersion;
    request.requiredCapabilities = commit_v1::kRequiredCapabilities;
    request.activeProfile = static_cast<std::uint32_t>(profile);
    request.supportedProfileMask = graphicsContract.supportedProfileMask;
    request.snapshot = snapshot;
    request.decision = candidate;

    const auto commit = reinterpret_cast<NativeCommitExportV1>(peer.proc());
    const HRESULT status = commit(&request);
    if (status == S_OK) return candidate;
    return decide(captureStatus, &snapshot, profile, 0);
}

const char* modeName(const DecisionV1& decision) noexcept {
    return decision.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection)
        ? "selected" : "legacy";
}

const char* reasonName(const DecisionV1& decision) noexcept {
    switch (static_cast<Reason>(decision.reason)) {
    case Reason::None: return "none";
    case Reason::SnapshotUnavailable: return "snapshot-unavailable";
    case Reason::InvalidSnapshot: return "invalid-snapshot";
    case Reason::ProfileNotReady: return "profile-not-ready";
    case Reason::ProviderRejected: return "provider-rejected";
    }
    return "unknown";
}

const char* recordStateName(std::uint32_t state) noexcept {
    switch (static_cast<PluginSelectionSnapshotStateV1>(state)) {
    case PluginSelectionSnapshotStateV1::Absent: return "absent";
    case PluginSelectionSnapshotStateV1::Supported: return "supported";
    case PluginSelectionSnapshotStateV1::Opaque: return "opaque";
    }
    return "unknown";
}

void logEffectivePluginSet(const DecisionV1& decision,
                           PluginSelectionProfileV1 profile) noexcept {
    char ids[320] = {};
    std::size_t used = 0;
    for (std::uint32_t i = 0; i < edvr::plugins::kPluginCount; ++i) {
        const auto& plugin = edvr::plugins::kManifest[i];
        const std::uint32_t profileBit = profile == PluginSelectionProfileV1::Vr
            ? edvr::plugins::kProfileVr : profile == PluginSelectionProfileV1::Flat
                ? edvr::plugins::kProfileFlat : 0;
        if (!(plugin.profileMask & profileBit) ||
            !edvr::plugin_selection::admission_v1::ownerAllowedForStartup(decision, i)) continue;
        const int wrote = std::snprintf(ids + used, sizeof(ids) - used, "%s%s",
                                        used ? "," : "", plugin.id);
        if (wrote < 0 || static_cast<std::size_t>(wrote) >= sizeof(ids) - used) break;
        used += static_cast<std::size_t>(wrote);
    }
    const char* profileName = profile == PluginSelectionProfileV1::Vr ? "vr"
        : profile == PluginSelectionProfileV1::Flat ? "flat" : "unknown";
    edvr::Log::get().note(
        "plugin selection: mode=%s profile=%s plugins=%s reason=%s record=%s validation=0x%08X",
        modeName(decision), profileName, ids, reasonName(decision),
        recordStateName(decision.recordState), decision.validationFlags);
}
} // namespace

namespace edvr::plugin_admission_transport {

using plugin_selection::PluginSelectionProfileV1;
using plugin_selection::PluginSelectionSnapshotV1;
using plugin_selection::kPluginSelectionSnapshotVersionV1;
using plugin_selection::admission_v1::DecisionV1;

void publishCandidate(const std::wstring& moduleDirectory, HMODULE graphicsModule,
                      PluginSelectionProfileV1 activeProfile) noexcept {
    if (g_decisionReady.load(std::memory_order_acquire)) return;

    PluginSelectionSnapshotV1 snapshot{};
    HRESULT captureStatus = E_FAIL;
    if (graphicsModule) {
        captureStatus = edvrQueryPluginSelectionV1(
            kPluginSelectionSnapshotVersionV1, sizeof(snapshot), &snapshot);
    }
    const std::uint64_t base = moduleBase(graphicsModule);
    DecisionV1 decision = plugin_selection::admission_v1::decide(
        captureStatus, captureStatus == S_OK ? &snapshot : nullptr, activeProfile, base);
    if (captureStatus == S_OK) {
        decision = candidateWithNativeCommit(moduleDirectory, activeProfile,
                                             captureStatus, snapshot, decision);
    }

    // INIT_ONCE is the sole publisher. Decision queries remain a memcpy-only
    // read after this release store and never enter the initializer.
    g_publishedDecision = decision;
    g_decisionReady.store(true, std::memory_order_release);
    logEffectivePluginSet(decision, activeProfile);
}

bool ownerAllowedForStartup(std::uint32_t generatedGroupIndex) noexcept {
    DecisionV1 decision{};
    const HRESULT status = edvrQueryPluginAdmissionV1(
        kQueryVersionV1, static_cast<std::uint32_t>(QueryKindV1::Decision),
        sizeof(decision), &decision);
    if (status != S_OK) return false;
    return plugin_selection::admission_v1::ownerAllowedForStartup(
        decision, generatedGroupIndex);
}

bool readDecisionForReporting(DecisionV1* output) noexcept {
    if (!output) return false;
    return edvrQueryPluginAdmissionV1(
        kQueryVersionV1, static_cast<std::uint32_t>(QueryKindV1::Decision),
        sizeof(*output), output) == S_OK;
}

} // namespace edvr::plugin_admission_transport

extern "C" HRESULT WINAPI edvrQueryPluginAdmissionV1(
    std::uint32_t version, std::uint32_t kind, std::uint32_t size,
    void* output) noexcept {
    if (version != edvr::plugin_selection::admission_v1::kQueryVersionV1 || !output)
        return E_INVALIDARG;
    if (kind == static_cast<std::uint32_t>(
            edvr::plugin_selection::admission_v1::QueryKindV1::Contract)) {
        using Contract = edvr::plugin_selection::admission_v1::GraphicsContractV1;
        if (size != sizeof(Contract)) return E_INVALIDARG;
        const Contract value = edvr::plugin_selection::admission_v1::contract();
        std::memcpy(output, &value, sizeof(value));
        return S_OK;
    }
    if (kind == static_cast<std::uint32_t>(
            edvr::plugin_selection::admission_v1::QueryKindV1::Decision)) {
        using Decision = edvr::plugin_selection::admission_v1::DecisionV1;
        if (size != sizeof(Decision)) return E_INVALIDARG;
        if (!g_decisionReady.load(std::memory_order_acquire)) return E_PENDING;
        std::memcpy(output, &g_publishedDecision, sizeof(g_publishedDecision));
        return S_OK;
    }
    return E_INVALIDARG;
}
