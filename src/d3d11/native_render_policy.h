#pragma once

#include "../common/native_render_settings.h"

#include <stdint.h>

// Private bridge between the core OpenXR settings broker and the optional
// Performance plugin. This is deliberately separate from the exported ABI.
namespace edvr::native_render_policy {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kPerformanceManifestIndex = 6;

struct ResultV1 {
    uint32_t structSize;
    uint32_t version;
    float requestedScale;
    uint32_t matchedEntry;
    uint32_t entryCount;
    uint32_t reserved;
};

using Resolve = BOOL(WINAPI*)(void* context,
                              const EdvrNativeRenderSettings* request,
                              ResultV1* result);

struct OpsV1 {
    uint32_t structSize;
    uint32_t version;
    uint32_t manifestIndex;
    uint32_t reserved;
    void* context;
    Resolve resolve;
};

// Registration is immutable: an exact repeat succeeds, a different owner or
// callback cannot replace the selected policy. Unregister requires the full
// owner identity so teardown cannot remove another resolver accidentally.
BOOL registerPolicy(const OpsV1* ops) noexcept;
BOOL unregisterPolicy(uint32_t manifestIndex, void* context,
                      Resolve resolve) noexcept;

static_assert(sizeof(ResultV1) == 24, "native render policy result v1");
static_assert(sizeof(OpsV1) == 32, "native render policy ops v1 x64");

}  // namespace edvr::native_render_policy
