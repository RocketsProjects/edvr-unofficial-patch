#pragma once

#include "plugin_selection_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace edvr::plugin_selection::admission_v1 {

inline constexpr std::uint32_t kAbiVersion = 1;
inline constexpr std::uint32_t kCapabilityProfileWideAdmission = 1u << 0;
inline constexpr std::uint32_t kCapabilityGeneratedSelectionBits = 1u << 1;
inline constexpr std::uint32_t kCapabilitySnapshotValidation = 1u << 2;
// These bits cover only the pure rules in this module, not peer commitment.
inline constexpr std::uint32_t kCapabilities = kCapabilityProfileWideAdmission |
    kCapabilityGeneratedSelectionBits | kCapabilitySnapshotValidation;

enum class Mode : std::uint32_t { Legacy = 0, AdmittedSelection = 1 };
enum class Reason : std::uint32_t {
    None = 0,
    SnapshotUnavailable = 1,
    InvalidSnapshot = 2,
    ProfileNotReady = 3,
    ProviderRejected = 4,
};

// Fixed wire contract shared by peers that implement this admission ABI.
struct GraphicsContractV1 {
    std::uint32_t structSize, abiVersion, snapshotAbiVersion;
    std::uint32_t selectionSchemaVersion, catalogSchemaVersion;
    std::uint32_t supportedProfileMask, capabilities, reserved;
    std::uint8_t catalogFingerprint[32];
};
static_assert(sizeof(GraphicsContractV1) == 64 &&
              offsetof(GraphicsContractV1, catalogFingerprint) == 32,
              "graphics contract wire layout");

struct DecisionV1 {
    std::uint32_t structSize, abiVersion, mode, reason;
    std::uint32_t activeProfile, recordState, validationFlags, selectedPluginMask;
    std::int32_t captureStatus;
    std::uint32_t graphicsProviderBaseLow, graphicsProviderBaseHigh;
    std::uint32_t reserved[5];
};
static_assert(sizeof(DecisionV1) == 64 &&
              offsetof(DecisionV1, graphicsProviderBaseLow) == 36 &&
              offsetof(DecisionV1, reserved) == 44,
              "decision wire layout");
static_assert(std::is_trivially_copyable<GraphicsContractV1>::value &&
              std::is_standard_layout<GraphicsContractV1>::value &&
              std::is_trivially_copyable<DecisionV1>::value &&
              std::is_standard_layout<DecisionV1>::value,
              "wire structures must stay POD");

// The production functions always use the build-generated catalog internally.
// A returned decision is only a local candidate; it grants no cross-DLL authority.
GraphicsContractV1 contract() noexcept;
DecisionV1 decide(HRESULT captureStatus,
                  const PluginSelectionSnapshotV1* snapshot,
                  PluginSelectionProfileV1 activeProfile,
                  std::uint64_t graphicsProviderBase) noexcept;
bool selected(const DecisionV1& decision, std::uint32_t generatedGroupIndex) noexcept;
// Legacy keeps each applicable group enabled. Unknown profiles are fail-closed.
bool ownerEnabled(const DecisionV1& decision, std::uint32_t generatedGroupIndex) noexcept;

#ifdef EDVR_SELECTION_ADMISSION_TEST
// Synthetic catalogs exist only in the isolated fixture build.
namespace test_seam {
struct GroupV1 { std::uint32_t index, profileMask, readyMask; };
struct CatalogViewV1 {
    const GroupV1* groups;
    std::uint32_t count, schema;
    const std::uint8_t* fingerprint;
};
GraphicsContractV1 contractWithCatalog(CatalogViewV1 catalog) noexcept;
DecisionV1 decideWithCatalog(HRESULT captureStatus,
                             const PluginSelectionSnapshotV1* snapshot,
                             PluginSelectionProfileV1 activeProfile,
                             std::uint64_t graphicsProviderBase,
                             CatalogViewV1 catalog) noexcept;
bool selectedWithCatalog(const DecisionV1& decision, std::uint32_t groupIndex,
                         CatalogViewV1 catalog) noexcept;
bool ownerEnabledWithCatalog(const DecisionV1& decision, std::uint32_t groupIndex,
                             CatalogViewV1 catalog) noexcept;
}
#endif

} // namespace edvr::plugin_selection::admission_v1
