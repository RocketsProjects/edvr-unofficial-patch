#include "openxr_resolution_policy.h"

#include "../../d3d11/native_render_labels.h"
#include "../../common/config.h"
#include "../../common/log.h"

#include <cstring>
#include <string>
#include <vector>

namespace edvr::plugins::performance {

BOOL WINAPI resolveOpenxrResolution(
    void* context, const EdvrNativeRenderSettings* request,
    edvr::native_render_policy::ResultV1* result) {
    (void)context;
    if (!request || !result || request->size != sizeof(*request) ||
        request->version != EDVR_NATIVE_RENDER_SETTINGS_VERSION_2 ||
        request->reserved != 0) return FALSE;

    using namespace edvr::native_render;
    const std::string runtimeToken = headsetToken(request->runtimeName,
                                                   sizeof(request->runtimeName));
    const std::string systemToken = headsetToken(request->systemName,
                                                  sizeof(request->systemName));
    const std::string value = edvr::Config::get().getString("fix.openxr_resolution", "");
    ResolutionEntry entries[kResolutionEntryMax]{};
    std::vector<std::string> skipped;
    const size_t count = parseResolutionEntries(value.c_str(), entries, &skipped);
    uint32_t matched = 0;
    const uint32_t width = resolveResolutionWidth(entries, count, runtimeToken,
                                                   systemToken, &matched);
    const uint32_t requestedWidth = width ? width : request->eyes[0].originalWidth;

    OpenxrResolutionReport report;
    report.runtimeName = request->runtimeName;
    report.systemName = request->systemName;
    report.runtimeToken = runtimeToken;
    report.systemToken = systemToken;
    report.value = value;
    report.entries = entries;
    report.entryCount = count;
    report.matched = matched;
    report.width = width;
    report.eyes = request->eyes;
    report.settingsFile = edvr::Config::get().iniName();
    const std::string line = formatOpenxrResolutionReport(report);

    for (const std::string& token : skipped) {
        edvr::Log::get().note("openxr resolution: ignored \"%s\" in fix.openxr_resolution "
                              "(entries look like oculus/meta-quest-3:3283)", token.c_str());
    }
    edvr::Log::get().note("%s", line.c_str());

    result->structSize = sizeof(*result);
    result->version = edvr::native_render_policy::kVersion;
    result->requestedScale = clampScale(widthToScale(requestedWidth,
                                                      request->eyes[0].originalWidth));
    result->matchedEntry = matched;
    result->entryCount = static_cast<uint32_t>(count);
    result->reserved = 0;
    return TRUE;
}

}  // namespace edvr::plugins::performance
