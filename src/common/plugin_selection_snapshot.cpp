#include "plugin_selection_snapshot.h"

#include <cstring>
#include <new>

#include "iniedit.h"
#include "plugin_manifest.inc"

namespace edvr::plugin_selection {
namespace {

std::string trimToken(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowerToken(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool isPluginSectionNameImpl(const std::string& section) {
    return lowerToken(section) == "plugins";
}

// A broken candidate remains opaque through installer rewrites, rather than
// being silently discarded as an ordinary comment.
bool isPluginSectionCandidate(const IniLine& line) {
    const std::string body = trimToken(line.text);
    if (body.empty() || body[0] != '[') return false;
    const std::string inside = trimToken(body.substr(1));
    if (inside.size() < 7 || lowerToken(inside.substr(0, 7)) != "plugins") return false;
    return inside.size() == 7 || inside[7] == ']' || inside[7] == ' ' || inside[7] == '\t';
}

bool validPluginId(const std::string& id) {
    if (id.empty() || id.front() == '-' || id.back() == '-') return false;
    bool previousDash = false;
    for (char c : id) {
        const bool dash = c == '-';
        const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!dash && !alphanumeric) return false;
        if (dash && previousDash) return false;
        previousDash = dash;
    }
    return true;
}

bool parseSelectedIds(const std::string& value, std::vector<std::string>* ids) {
    ids->clear();
    if (value.empty()) return true;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t comma = value.find(',', begin);
        const std::size_t end = comma == std::string::npos ? value.size() : comma;
        const std::string id = value.substr(begin, end - begin);
        if (!validPluginId(id)) return false;
        for (const std::string& prior : *ids) if (prior == id) return false;
        ids->push_back(id);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return true;
}

std::uint32_t profileMask(PluginSelectionProfileV1 profile) {
    switch (profile) {
    case PluginSelectionProfileV1::Vr: return edvr::plugins::kVrProfileBit;
    case PluginSelectionProfileV1::Flat: return edvr::plugins::kFlatProfileBit;
    default: return 0;
    }
}

std::uint32_t findPluginIndex(const std::string& id) {
    for (std::uint32_t i = 0; i < edvr::plugins::kPluginCount; ++i) {
        if (id == edvr::plugins::kManifest[i].id) return i;
    }
    return edvr::plugins::kPluginIndexCount;
}

bool selected(std::uint32_t mask, std::uint32_t index) {
    return index < 32 && (mask & (1u << index)) != 0;
}

bool validSnapshotShape(const PluginSelectionSnapshotV1& value) {
    constexpr std::uint32_t knownFlags = kSelectionProfileMismatch |
        kSelectionUnsupportedInstalledProfile | kSelectionUnknownPluginId |
        kSelectionUnavailablePlugin | kSelectionMissingHardDependency |
        kSelectionConflict | kSelectionBuildMismatch;
    if (value.structSize != sizeof(value) || value.abiVersion != 1 ||
        value.recordState < static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Absent) ||
        value.recordState > static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Opaque) ||
        value.installedProfile > static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat) ||
        value.recordProfile > static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat) ||
        (value.validationFlags & ~knownFlags) != 0 || value.reserved[0] != 0 || value.reserved[1] != 0)
        return false;
    static_assert(edvr::plugins::kPluginCount < 32, "selection mask must fit uint32");
    const std::uint32_t allowedMask = (1u << edvr::plugins::kPluginCount) - 1u;
    if ((value.recordedPluginMask & ~allowedMask) != 0) return false;
    const bool unsupportedInstalled = value.installedProfile ==
        static_cast<std::uint32_t>(PluginSelectionProfileV1::Unknown);
    if (((value.validationFlags & kSelectionUnsupportedInstalledProfile) != 0) != unsupportedInstalled)
        return false;
    if (value.recordState != static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported)) {
        return value.recordProfile == 0 && value.recordSchemaVersion == 0 &&
            value.recordCatalogSchemaVersion == 0 && value.recordedPluginMask == 0 &&
            (value.validationFlags & kSelectionProfileMismatch) == 0;
    }
    if (value.recordProfile != static_cast<std::uint32_t>(PluginSelectionProfileV1::Vr) &&
        value.recordProfile != static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat)) return false;
    if (value.recordSchemaVersion != 1 || value.recordCatalogSchemaVersion != 2) return false;
    const bool expectedMismatch = !unsupportedInstalled && value.recordProfile != value.installedProfile;
    return (((value.validationFlags & kSelectionProfileMismatch) != 0) == expectedMismatch);
}

void validateSelection(const PluginSelectionRecord& record,
                       PluginSelectionProfileV1 installed,
                       PluginSelectionSnapshotV1* snapshot) {
    const std::uint32_t installedMask = profileMask(installed);
    if (installedMask == 0) snapshot->validationFlags |= kSelectionUnsupportedInstalledProfile;
    if (record.profile == "vr") snapshot->recordProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Vr);
    else if (record.profile == "flat") snapshot->recordProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat);
    if (installedMask != 0 && snapshot->recordProfile != static_cast<std::uint32_t>(installed))
        snapshot->validationFlags |= kSelectionProfileMismatch;

    for (const std::string& id : record.selectedIds) {
        const std::uint32_t index = findPluginIndex(id);
        if (index == edvr::plugins::kPluginIndexCount || index >= 32) {
            snapshot->validationFlags |= kSelectionUnknownPluginId;
            continue;
        }
        snapshot->recordedPluginMask |= 1u << index;
        const auto& plugin = edvr::plugins::kManifest[index];
        if (installedMask == 0 || (plugin.profileMask & installedMask) == 0 ||
            (plugin.selectionAvailableProfileMask & installedMask) == 0)
            snapshot->validationFlags |= kSelectionUnavailablePlugin;
    }

    for (std::uint32_t i = 0; i < edvr::plugins::kPluginCount; ++i) {
        if (!selected(snapshot->recordedPluginMask, i)) continue;
        const auto& plugin = edvr::plugins::kManifest[i];
        for (std::uint32_t j = 0; j < plugin.dependencyCount; ++j) {
            const auto& dep = plugin.dependencies[j];
            if (dep.kind != 1) continue;  // hard dependency
            const std::uint32_t depIndex = findPluginIndex(dep.id);
            if (depIndex == edvr::plugins::kPluginIndexCount || !selected(snapshot->recordedPluginMask, depIndex))
                snapshot->validationFlags |= kSelectionMissingHardDependency;
        }
        for (std::uint32_t j = 0; j < plugin.conflictCount; ++j) {
            const std::uint32_t conflictIndex = findPluginIndex(plugin.conflicts[j].id);
            if (conflictIndex != edvr::plugins::kPluginIndexCount && selected(snapshot->recordedPluginMask, conflictIndex))
                snapshot->validationFlags |= kSelectionConflict;
        }
    }
}

void initializeSnapshot(PluginSelectionSnapshotV1* value,
                        PluginSelectionProfileV1 installed) {
    std::memset(value, 0, sizeof(*value));
    value->structSize = sizeof(*value);
    value->abiVersion = 1;
    value->installedProfile = static_cast<std::uint32_t>(installed);
    value->compiledCatalogSchemaVersion = edvr::plugins::kSelectionCatalogSchemaVersion;
    std::memcpy(value->catalogFingerprint, edvr::plugins::kSelectionCatalogFingerprint,
                sizeof(value->catalogFingerprint));
}

}  // namespace

bool isPluginSectionName(const std::string& section) {
    return isPluginSectionNameImpl(section);
}

PluginSelectionRecord decodePluginSelectionRecord(const IniDoc& doc) {
    PluginSelectionRecord record;
    std::vector<const IniLine*> pluginLines;
    std::size_t sectionCount = 0;
    bool inPluginSection = false;
    for (const IniLine& line : doc.lines) {
        const bool candidate = isPluginSectionCandidate(line);
        if (candidate) {
            ++sectionCount;
            inPluginSection = true;
        } else if (line.kind == LineKind::Section) {
            inPluginSection = false;
        }
        if (inPluginSection) {
            record.opaqueText += line.text;
            record.opaqueText += line.eol;
            pluginLines.push_back(&line);
        }
    }
    if (sectionCount == 0) {
        record.opaqueText.clear();
        return record;
    }

    auto opaque = [&]() {
        record.state = RecordState::Opaque;
        record.selectedIds.clear();
        return record;
    };
    if (sectionCount != 1 || pluginLines.empty() ||
        pluginLines.front()->kind != LineKind::Section ||
        lowerToken(trimToken(pluginLines.front()->text)) != "[plugins]") return opaque();

    bool haveSelectionSchema = false, haveCatalogSchema = false;
    bool haveProfile = false, haveSelectedIds = false;
    std::string selectedValue;
    for (std::size_t i = 1; i < pluginLines.size(); ++i) {
        const IniLine& line = *pluginLines[i];
        if (line.kind != LineKind::Key || !isPluginSectionNameImpl(line.section)) return opaque();
        const std::size_t eq = line.text.find('=');
        if (eq == std::string::npos || trimToken(line.text.substr(eq + 1)) != line.value) return opaque();
        if (line.key == "selection_schema_version") {
            if (haveSelectionSchema || line.value != "1") return opaque();
            haveSelectionSchema = true;
        } else if (line.key == "catalog_schema_version") {
            if (haveCatalogSchema || line.value != "2") return opaque();
            haveCatalogSchema = true;
        } else if (line.key == "profile") {
            if (haveProfile || (line.value != "vr" && line.value != "flat")) return opaque();
            record.profile = line.value;
            haveProfile = true;
        } else if (line.key == "selected_ids") {
            if (haveSelectedIds) return opaque();
            selectedValue = line.value;
            haveSelectedIds = true;
        } else return opaque();
    }
    if (!haveSelectionSchema || !haveCatalogSchema || !haveProfile || !haveSelectedIds ||
        !parseSelectedIds(selectedValue, &record.selectedIds)) return opaque();
    record.state = RecordState::Supported;
    record.schemaVersion = 1;
    record.catalogSchemaVersion = 2;
    record.opaqueText.clear();
    return record;
}

bool isSupportedPluginSelectionRecord(const PluginSelectionRecord& record) {
    if (record.state != RecordState::Supported || record.schemaVersion != 1 ||
        record.catalogSchemaVersion != 2 || (record.profile != "vr" && record.profile != "flat")) return false;
    std::vector<std::string> checked;
    for (const std::string& id : record.selectedIds) {
        if (!validPluginId(id)) return false;
        for (const std::string& prior : checked) if (prior == id) return false;
        checked.push_back(id);
    }
    return true;
}

HRESULT decodePluginSelectionSnapshotV1(const char* stateText, std::size_t textBytes,
                                         PluginSelectionProfileV1 installedProfile,
                                         PluginSelectionSnapshotV1* out) noexcept {
    if (!out || (!stateText && textBytes != 0) ||
        static_cast<std::uint32_t>(installedProfile) >
            static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat)) return E_INVALIDARG;
    if (textBytes > 1024u * 1024u) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    try {
        const std::string text = textBytes == 0 ? std::string() : std::string(stateText, textBytes);
        const IniDoc doc = iniParse(text);
        const PluginSelectionRecord record = decodePluginSelectionRecord(doc);
        PluginSelectionSnapshotV1 value{};
        initializeSnapshot(&value, installedProfile);
        if (profileMask(installedProfile) == 0)
            value.validationFlags |= kSelectionUnsupportedInstalledProfile;
        switch (record.state) {
        case RecordState::Absent:
            value.recordState = static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Absent);
            break;
        case RecordState::Opaque:
            value.recordState = static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Opaque);
            break;
        case RecordState::Supported:
            value.recordState = static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported);
            value.recordSchemaVersion = record.schemaVersion;
            value.recordCatalogSchemaVersion = record.catalogSchemaVersion;
            validateSelection(record, installedProfile, &value);
            break;
        }
        *out = value;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT copyPluginSelectionSnapshotV1(std::uint32_t version, std::uint32_t size,
                                       const PluginSelectionSnapshotV1* source,
                                       void* output) noexcept {
    if (version != 1 || size != sizeof(PluginSelectionSnapshotV1) || !source || !output)
        return E_INVALIDARG;
    PluginSelectionSnapshotV1 value{};
    __try { std::memcpy(&value, source, sizeof(value)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_POINTER; }
    if (!validSnapshotShape(value)) return E_INVALIDARG;
    __try { std::memcpy(output, &value, sizeof(value)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_POINTER; }
    return S_OK;
}

bool pluginSelectionSnapshotMatchesBuildV1(const PluginSelectionSnapshotV1& snapshot) noexcept {
    return validSnapshotShape(snapshot) &&
        snapshot.compiledCatalogSchemaVersion == edvr::plugins::kSelectionCatalogSchemaVersion &&
        std::memcmp(snapshot.catalogFingerprint, edvr::plugins::kSelectionCatalogFingerprint,
                    sizeof(snapshot.catalogFingerprint)) == 0;
}

}  // namespace edvr::plugin_selection
