#include "plugin_components.h"

#include "plugin_manifest.inc"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

namespace edvr::installer::plugin_components {
namespace {

std::uint32_t profileBit(const std::string& profile) noexcept {
    if (profile == "vr") return plugins::kVrProfileBit;
    if (profile == "flat") return plugins::kFlatProfileBit;
    return 0;
}

bool validCatalog(CatalogView catalog) noexcept {
    if (!catalog.records || catalog.count == 0 || catalog.count >= 32 ||
        catalog.schemaVersion != plugins::kSelectionCatalogSchemaVersion)
        return false;
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        const auto& record = catalog.records[i];
        if (record.index != i || !record.id || !record.name || !record.description ||
            (record.profileMask & ~3u) ||
            (record.selectionAvailableProfileMask & ~record.profileMask) ||
            (record.recommendedProfileMask & ~record.profileMask) ||
            (record.defaultInstallProfileMask & ~record.profileMask) ||
            (record.dependencyCount && !record.dependencies) ||
            (record.conflictCount && !record.conflicts)) return false;
        for (std::uint32_t j = 0; j < i; ++j)
            if (std::strcmp(catalog.records[j].id, record.id) == 0) return false;
    }
    return true;
}

int find(CatalogView catalog, const std::string& id) noexcept {
    for (std::uint32_t i = 0; i < catalog.count; ++i)
        if (id == catalog.records[i].id) return static_cast<int>(i);
    return -1;
}

}  // namespace

CatalogView compiledCatalog() noexcept {
    static_assert(plugins::kPluginCount == plugins::kPluginIndexCount && plugins::kPluginCount < 32,
                  "installer selection uses a fixed-width plugin mask");
    return {plugins::kManifest, plugins::kPluginCount,
            plugins::kSelectionCatalogSchemaVersion};
}

bool validPluginId(const std::string& id) noexcept {
    if (id.empty() || id.front() == '-' || id.back() == '-') return false;
    bool hyphen = false;
    for (unsigned char c : id) {
        const bool isHyphen = c == '-';
        if (!(isHyphen || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ||
            (isHyphen && hyphen)) return false;
        hyphen = isHyphen;
    }
    return true;
}

bool profileSelectionReady(CatalogView catalog, const std::string& profile) noexcept {
    if (!validCatalog(catalog)) return false;
    const std::uint32_t bit = profileBit(profile);
    if (!bit) return false;
    bool found = false;
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        const auto& row = catalog.records[i];
        if (!(row.profileMask & bit)) continue;
        found = true;
        if (!(row.selectionAvailableProfileMask & bit) || !row.implementationStatus ||
            std::strcmp(row.implementationStatus, "selection-ready") != 0) return false;
    }
    return found;
}

std::vector<Component> components(CatalogView catalog, const std::string& profile) {
    std::vector<Component> out;
    const std::uint32_t bit = profileBit(profile);
    if (!validCatalog(catalog) || !bit) return out;
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        const auto& row = catalog.records[i];
        if (!(row.profileMask & bit)) continue;
        out.push_back({row.id, row.name, row.description,
                       row.implementationStatus ? row.implementationStatus : "unknown",
                       row.profileMask, row.selectionAvailableProfileMask,
                       row.recommendedProfileMask, row.defaultInstallProfileMask});
    }
    return out;
}

Resolution resolve(CatalogView catalog, const std::string& profile,
                   const std::vector<std::string>& requestedIds) {
    Resolution out;
    out.profile = profile;
    const std::uint32_t bit = profileBit(profile);
    if (!validCatalog(catalog) || !bit) {
        out.error = "The plugin catalog or install profile is invalid.";
        return out;
    }
    const bool ready = profileSelectionReady(catalog, profile);
    if (!ready) {
        out.error = "Plugin selection for the " + profile +
                    " profile is unavailable until every applicable plugin is selection-ready.";
        return out;
    }
    std::set<std::uint32_t> requested;
    std::set<std::uint32_t> selected;
    for (const auto& id : requestedIds) {
        if (!validPluginId(id)) {
            out.error = "Plugin IDs must be lowercase letters, digits, and single hyphens.";
            return out;
        }
        const int index = find(catalog, id);
        if (index < 0) {
            out.error = "Unknown plugin ID: " + id;
            return out;
        }
        const auto& row = catalog.records[index];
        if (!(row.profileMask & bit)) {
            out.error = id + " is not available for the " + profile + " profile.";
            return out;
        }
        if (!(row.selectionAvailableProfileMask & bit)) {
            out.error = id + " cannot be selected until every plugin for this profile is selection-ready.";
            return out;
        }
        requested.insert(static_cast<std::uint32_t>(index));
        selected.insert(static_cast<std::uint32_t>(index));
    }

    std::vector<std::uint32_t> visiting;
    std::set<std::uint32_t> visited;
    std::function<bool(std::uint32_t)> addHard = [&](std::uint32_t index) {
        if (std::find(visiting.begin(), visiting.end(), index) != visiting.end()) {
            out.error = "The plugin catalog contains a hard dependency cycle.";
            return false;
        }
        if (visited.count(index)) return true;
        visiting.push_back(index);
        const auto& row = catalog.records[index];
        for (std::uint32_t n = 0; n < row.dependencyCount; ++n) {
            const auto& dep = row.dependencies[n];
            if (dep.kind != 1) continue;
            const int depIndex = find(catalog, dep.id ? dep.id : "");
            if (depIndex < 0 || !(catalog.records[depIndex].profileMask & bit) ||
                !(catalog.records[depIndex].selectionAvailableProfileMask & bit)) {
                out.error = std::string(row.id) + " requires an unavailable plugin: " +
                            (dep.id ? dep.id : "unknown");
                visiting.pop_back();
                return false;
            }
            selected.insert(static_cast<std::uint32_t>(depIndex));
            if (!addHard(static_cast<std::uint32_t>(depIndex))) {
                visiting.pop_back();
                return false;
            }
        }
        visiting.pop_back();
        visited.insert(index);
        return true;
    };
    for (auto index : requested) if (!addHard(index)) return out;

    for (auto index : selected) {
        const auto& row = catalog.records[index];
        for (std::uint32_t n = 0; n < row.conflictCount; ++n) {
            const int other = find(catalog, row.conflicts[n].id ? row.conflicts[n].id : "");
            if (other >= 0 && selected.count(static_cast<std::uint32_t>(other))) {
                out.error = std::string("Selected plugins conflict: ") + row.id + " and " +
                            catalog.records[other].id;
                return out;
            }
        }
    }

    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        const auto& row = catalog.records[i];
        if (!(row.profileMask & bit)) continue;
        if (row.recommendedProfileMask & bit) out.recommendedIds.emplace_back(row.id);
        if (row.defaultInstallProfileMask & bit) out.defaultIds.emplace_back(row.id);
        if (requested.count(i)) out.requestedIds.emplace_back(row.id);
        if (selected.count(i)) {
            out.selectedIds.emplace_back(row.id);
            if (!requested.count(i)) out.autoSelectedIds.emplace_back(row.id);
            for (std::uint32_t n = 0; n < row.dependencyCount; ++n) {
                const auto& dep = row.dependencies[n];
                const int depIndex = find(catalog, dep.id ? dep.id : "");
                if (dep.kind != 2 || (depIndex >= 0 && selected.count(static_cast<std::uint32_t>(depIndex)))) continue;
                out.softNotices.emplace_back(std::string(row.id) + " optionally uses " +
                    (dep.id ? dep.id : "unknown") +
                    (dep.reason && dep.reason[0] ? ": " + std::string(dep.reason) : std::string()));
            }
        }
    }
    return out;
}

Resolution defaults(CatalogView catalog, const std::string& profile) {
    if (!profileSelectionReady(catalog, profile)) return resolve(catalog, profile, {});
    const std::uint32_t bit = profileBit(profile);
    std::vector<std::string> ids;
    for (std::uint32_t i = 0; i < catalog.count; ++i)
        if ((catalog.records[i].profileMask & bit) &&
            (catalog.records[i].defaultInstallProfileMask & bit)) ids.emplace_back(catalog.records[i].id);
    return resolve(catalog, profile, ids);
}

}  // namespace edvr::installer::plugin_components
