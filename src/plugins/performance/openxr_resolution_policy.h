#pragma once

#include "../../d3d11/native_render_policy.h"

namespace edvr::plugins::performance {

// Stateless query-time resolver registered by the Performance bootstrap.
BOOL WINAPI resolveOpenxrResolution(void* context,
                                   const EdvrNativeRenderSettings* request,
                                   edvr::native_render_policy::ResultV1* result);

}  // namespace edvr::plugins::performance
