#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <windows.h>

namespace edvr {
struct IniDoc;
}

namespace edvr::plugin_selection {

// Shared installer/runtime representation. Opaque bytes are retained by the
// installer codec and are deliberately absent from the fixed runtime snapshot.
enum class RecordState : std::uint32_t { Absent = 0, Supported = 1, Opaque = 2 };
struct PluginSelectionRecord {
    RecordState state = RecordState::Absent;
    std::uint32_t schemaVersion = 1;
    std::uint32_t catalogSchemaVersion = 2;
    std::string profile;
    std::vector<std::string> selectedIds;
    std::string opaqueText;
};

PluginSelectionRecord decodePluginSelectionRecord(const edvr::IniDoc& doc);
bool isSupportedPluginSelectionRecord(const PluginSelectionRecord& record);
bool isPluginSectionName(const std::string& section);

enum class PluginSelectionProfileV1 : std::uint32_t { Unknown = 0, Vr = 1, Flat = 2 };
inline constexpr std::uint32_t kPluginSelectionSnapshotVersionV1 = 1;
static_assert(static_cast<std::uint32_t>(RecordState::Absent) == 0 &&
              static_cast<std::uint32_t>(RecordState::Supported) == 1 &&
              static_cast<std::uint32_t>(RecordState::Opaque) == 2,
              "installer record state values are persisted semantics");
static_assert(static_cast<std::uint32_t>(PluginSelectionProfileV1::Unknown) == 0 &&
              static_cast<std::uint32_t>(PluginSelectionProfileV1::Vr) == 1 &&
              static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat) == 2,
              "selection profile values are transport semantics");
enum class PluginSelectionSnapshotStateV1 : std::uint32_t {
    Absent = 1, Supported = 2, Opaque = 3,
};
enum PluginSelectionValidationV1 : std::uint32_t {
    kSelectionProfileMismatch = 1u << 0,
    kSelectionUnsupportedInstalledProfile = 1u << 1,
    kSelectionUnknownPluginId = 1u << 2,
    kSelectionUnavailablePlugin = 1u << 3,
    kSelectionMissingHardDependency = 1u << 4,
    kSelectionConflict = 1u << 5,
    kSelectionBuildMismatch = 1u << 6,
};

// Cross-DLL transport only. The mask is a recorded summary, never authority.
struct PluginSelectionSnapshotV1 {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint32_t recordState;
    std::uint32_t installedProfile;
    std::uint32_t recordProfile;
    std::uint32_t recordSchemaVersion;
    std::uint32_t recordCatalogSchemaVersion;
    std::uint32_t compiledCatalogSchemaVersion;
    std::uint32_t recordedPluginMask;
    std::uint32_t validationFlags;
    std::uint8_t catalogFingerprint[32];
    std::uint32_t reserved[2];
};
static_assert(sizeof(PluginSelectionSnapshotV1) == 80, "selection snapshot ABI size");
static_assert(std::is_trivially_copyable<PluginSelectionSnapshotV1>::value,
              "selection snapshot must remain trivially copyable");
static_assert(std::is_standard_layout<PluginSelectionSnapshotV1>::value,
              "selection snapshot must remain standard layout");
static_assert(offsetof(PluginSelectionSnapshotV1, catalogFingerprint) == 40,
              "selection fingerprint ABI offset");
static_assert(offsetof(PluginSelectionSnapshotV1, reserved) == 72,
              "selection reserved ABI offset");

// Input is bounded in bytes, including any UTF-8 BOM. This pure codec does no
// file access. A missing installer file is represented by the caller as an
// empty string; read errors and runtime read-size enforcement belong to the
// capture wrapper and are not installer codec limits.
HRESULT decodePluginSelectionSnapshotV1(
    const char* stateText, std::size_t textBytes,
    PluginSelectionProfileV1 installedProfile,
    PluginSelectionSnapshotV1* out) noexcept;

HRESULT copyPluginSelectionSnapshotV1(
    std::uint32_t version, std::uint32_t size,
    const PluginSelectionSnapshotV1* source, void* output) noexcept;

bool pluginSelectionSnapshotMatchesBuildV1(
    const PluginSelectionSnapshotV1& snapshot) noexcept;

}  // namespace edvr::plugin_selection
