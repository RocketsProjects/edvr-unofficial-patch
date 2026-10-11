#include "../../src/common/native_graphics.h"
#include "../../src/common/graphics_bridge.h"
#include "../../src/common/render_boundary.h"
#include "../../src/common/plugin_selection_admission.h"

#include <atomic>
#include <cstring>

namespace {
using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;

const std::uint8_t fingerprint[32] = {0x29, 0x10, 0x77};
test_seam::GroupV1 groups[] = {{0, 3, 3}, {1, 1, 1}, {2, 2, 2}};
test_seam::CatalogViewV1 catalog{groups, 3, 2, fingerprint};
std::atomic<unsigned> pairCalls{0};
DecisionV1 fixtureDecision{};
HRESULT fixtureDecisionStatus = E_PENDING;
}

extern "C" __declspec(dllexport) HRESULT WINAPI edvrQueryPluginAdmissionV1(
    std::uint32_t version, std::uint32_t kind, std::uint32_t size, void* output) noexcept {
    if (!output || version != 1) return E_INVALIDARG;
    if (kind == 1 && size == sizeof(GraphicsContractV1)) {
        const auto value = contractWithCatalog(catalog);
        std::memcpy(output, &value, sizeof(value));
        return S_OK;
    }
    if (kind == 2 && size == sizeof(DecisionV1)) {
        if (fixtureDecisionStatus == S_OK)
            std::memcpy(output, &fixtureDecision, sizeof(fixtureDecision));
        return fixtureDecisionStatus;
    }
    return E_INVALIDARG;
}

extern "C" HRESULT WINAPI edvrAcquireNativeGraphics(
    const EdvrNativeGraphicsRequest*, EdvrNativeGraphicsTable*) {
    ++pairCalls;
    return E_FAIL;
}
extern "C" HRESULT WINAPI edvrAcquireGraphicsBridge(
    const EdvrGraphicsBridgeRequest*, EdvrGraphicsBridgeTable*) {
    ++pairCalls;
    return E_FAIL;
}
extern "C" HRESULT WINAPI edvrAcquireRenderBoundary(
    const EdvrRenderBoundaryRequest*, EdvrRenderBoundaryTable*) {
    ++pairCalls;
    return E_FAIL;
}

extern "C" __declspec(dllexport) unsigned WINAPI FixturePairCalls() noexcept {
    return pairCalls.load();
}
extern "C" __declspec(dllexport) void WINAPI FixtureSetDecision(
    HRESULT status, const DecisionV1* decision) noexcept {
    fixtureDecisionStatus = status;
    fixtureDecision = decision ? *decision : DecisionV1{};
}
