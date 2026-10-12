#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace edvr::plugins {
struct PluginRecord;
}

namespace edvr::installer::plugin_components {

struct CatalogView {
    const edvr::plugins::PluginRecord* records = nullptr;
    std::uint32_t count = 0;
    std::uint32_t schemaVersion = 0;
};

struct Component {
    std::string id;
    std::string name;
    std::string description;
    std::string status;
    std::uint32_t profileMask = 0;
    std::uint32_t availableProfileMask = 0;
    std::uint32_t recommendedProfileMask = 0;
    std::uint32_t defaultProfileMask = 0;
};

struct Resolution {
    std::string profile;
    std::vector<std::string> requestedIds;
    std::vector<std::string> selectedIds;
    std::vector<std::string> autoSelectedIds;
    std::vector<std::string> recommendedIds;
    std::vector<std::string> defaultIds;
    std::vector<std::string> softNotices;
    std::string error;
};

CatalogView compiledCatalog() noexcept;
std::vector<Component> components(CatalogView catalog, const std::string& profile);
bool profileSelectionReady(CatalogView catalog, const std::string& profile) noexcept;
Resolution defaults(CatalogView catalog, const std::string& profile);
Resolution resolve(CatalogView catalog, const std::string& profile,
                   const std::vector<std::string>& requestedIds);
bool validPluginId(const std::string& id) noexcept;

}  // namespace edvr::installer::plugin_components
