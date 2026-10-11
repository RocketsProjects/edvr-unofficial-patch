#pragma once

#include "plugin_selection_admission.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <type_traits>

namespace edvr::plugin_selection::commit_v1 {

inline constexpr std::uint32_t kAbiVersion = 1;
inline constexpr std::uint32_t kCapabilityPeerCommit = 1u << 3;
inline constexpr std::uint32_t kRequiredCapabilities =
    admission_v1::kCapabilities | kCapabilityPeerCommit;

// The full source snapshot travels beside the derived candidate so the store
// can recompute it against its own generated catalog before peer callbacks.
struct AdmissionCommitRequestV1 {
    std::uint32_t structSize, abiVersion, requiredCapabilities;
    std::uint32_t activeProfile, supportedProfileMask, reserved;
    PluginSelectionSnapshotV1 snapshot;
    admission_v1::DecisionV1 decision;
};
static_assert(sizeof(AdmissionCommitRequestV1) == 168 &&
              offsetof(AdmissionCommitRequestV1, structSize) == 0 &&
              offsetof(AdmissionCommitRequestV1, abiVersion) == 4 &&
              offsetof(AdmissionCommitRequestV1, requiredCapabilities) == 8 &&
              offsetof(AdmissionCommitRequestV1, activeProfile) == 12 &&
              offsetof(AdmissionCommitRequestV1, supportedProfileMask) == 16 &&
              offsetof(AdmissionCommitRequestV1, reserved) == 20 &&
              offsetof(AdmissionCommitRequestV1, snapshot) == 24 &&
              offsetof(AdmissionCommitRequestV1, decision) == 104,
              "admission commit request wire layout");
static_assert(std::is_trivially_copyable<AdmissionCommitRequestV1>::value &&
              std::is_standard_layout<AdmissionCommitRequestV1>::value,
              "admission commit request must stay POD");

// Callbacks are supplied by the native caller after it has qualified the
// exact loaded graphics module. No lock is held while any callback runs.
struct PeerHooksV1 {
    void* context;
    bool (*queryExportOwnModule)(void*, std::uint64_t) noexcept;
    bool (*loadedGraphicsIsExpected)(void*, std::uint64_t) noexcept;
    HRESULT (*queryContract)(void*, std::uint64_t,
                             admission_v1::GraphicsContractV1*) noexcept;
    bool (*retainGraphics)(void*, std::uint64_t) noexcept;
    void (*releaseGraphics)(void*, std::uint64_t) noexcept;
};

// Internal catalog view used by the store implementation; it is never part
// of the cross-DLL request or production API.
struct CatalogGroupInternalV1 { std::uint32_t index, profileMask, readyMask; };
struct CatalogInternalV1 {
    const CatalogGroupInternalV1* groups;
    std::uint32_t count, schema;
    const std::uint8_t* fingerprint;
};

class CommitStoreV1 final {
public:
    CommitStoreV1() noexcept = default;
    ~CommitStoreV1() noexcept;
    CommitStoreV1(const CommitStoreV1&) = delete;
    CommitStoreV1& operator=(const CommitStoreV1&) = delete;

    // E_PENDING denotes another caller currently preparing. S_OK is returned
    // only after the peer contract is validated and an owned provider ref is
    // acquired. An identical committed request is idempotent and does no work.
    HRESULT commit(const AdmissionCommitRequestV1& request,
                   const PeerHooksV1& hooks) noexcept;
    bool read(AdmissionCommitRequestV1* out) const noexcept;

#ifdef EDVR_ADMISSION_COMMIT_TEST
    HRESULT commitWithCatalogForTest(
        const AdmissionCommitRequestV1& request,
        admission_v1::test_seam::CatalogViewV1 nativeCatalog,
        const PeerHooksV1& hooks) noexcept;
    void resetForTest() noexcept;
#endif

private:
    enum class State : std::uint32_t { Empty, Preparing, Committed };
    HRESULT commitImpl(const AdmissionCommitRequestV1& request,
                       CatalogInternalV1 catalog,
                       const PeerHooksV1& hooks) noexcept;
    std::atomic<State> state_{State::Empty};
    AdmissionCommitRequestV1 committed_{};
    std::uint64_t retainedBase_ = 0;
    PeerHooksV1 retainedHooks_{};
};

using NativeCommitExportV1 = HRESULT (WINAPI *)(const AdmissionCommitRequestV1*);

} // namespace edvr::plugin_selection::commit_v1
