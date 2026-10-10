#pragma once

#include <cstdint>

#include "../../../d3d11/fss_observation.h"

struct ID3D11DeviceContext;

namespace edvr::plugins::comfort::scanners {

// Borrowed host fields for one synchronous claim evaluation. The evaluator
// reads each field only when the legacy short-circuit path reaches it and
// never retains the references.
struct ScannerClaimHostView final {
    const std::uint32_t& bodyFrame;
    const std::uint32_t& frameNo;
    const std::uint32_t& jumpFrame;
    const std::uint32_t& jumpFrameNo;
};

bool scannerPanelClaim(ID3D11DeviceContext* context, char kind,
                       std::uint32_t count, std::uint32_t instances,
                       const ScannerClaimHostView& host,
                       FssPanelObservation* observation);

bool scannerRevealClaim(ID3D11DeviceContext* context, char kind,
                        std::uint32_t count, std::uint32_t instances,
                        const ScannerClaimHostView& host,
                        FssRevealObservation* observation);

}  // namespace edvr::plugins::comfort::scanners
