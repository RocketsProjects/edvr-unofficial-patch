#pragma once

#include "../../common/config.h"
#include "../../d3d11/target_sharp_observation.h"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace edvr {

struct TargetSharpFastStateV1 {
    uint32_t structSize;
    uint32_t version;
    uint32_t sharp;
    uint32_t failed;
    uint32_t engaged;
    uint32_t reserved;
    uint64_t vertexShaderHash;
};

struct TargetSharpPolicyOpsV1 {
    uint32_t structSize;
    uint32_t version;
    void* context;
    void (*configure)(void*, Config*, TargetSharpFastStateV1*);
    bool (*matches)(void*, ID3D11DeviceContext*, char, uint32_t, uint32_t,
                    TargetSharpObservation*);
    void (*begin)(void*, ID3D11DeviceContext*, TargetSharpFastStateV1*);
    void (*end)(void*, ID3D11DeviceContext*, TargetSharpFastStateV1*);
    void (*shutdown)(void*, TargetSharpFastStateV1*);
};

constexpr uint32_t kTargetSharpPolicyVersionV1 = 1;
constexpr uint64_t kTargetSharpVertexShaderHashV1 = 0xE508648660A352B2ull;
static_assert(sizeof(TargetSharpFastStateV1) == 32, "TargetSharp fast-state ABI");
static_assert(offsetof(TargetSharpFastStateV1, vertexShaderHash) == 24,
              "TargetSharp fast-state hash offset");
static_assert(std::is_standard_layout<TargetSharpFastStateV1>::value &&
              std::is_trivially_copyable<TargetSharpFastStateV1>::value,
              "TargetSharp fast-state must be POD-copyable");
static_assert(sizeof(TargetSharpPolicyOpsV1) == 56, "TargetSharp ops ABI");
static_assert(offsetof(TargetSharpPolicyOpsV1, context) == 8 &&
              offsetof(TargetSharpPolicyOpsV1, configure) == 16 &&
              offsetof(TargetSharpPolicyOpsV1, matches) == 24 &&
              offsetof(TargetSharpPolicyOpsV1, begin) == 32 &&
              offsetof(TargetSharpPolicyOpsV1, end) == 40 &&
              offsetof(TargetSharpPolicyOpsV1, shutdown) == 48,
              "TargetSharp ops V1 offsets");
static_assert(std::is_standard_layout<TargetSharpPolicyOpsV1>::value &&
              std::is_trivially_copyable<TargetSharpPolicyOpsV1>::value,
              "TargetSharp ops must be POD-copyable");

const TargetSharpPolicyOpsV1* targetSharpPolicyOpsV1() noexcept;
#if defined(EDVR_VSCREEN_PREDICATE_TEST)
void targetSharpPolicyPredicateSeedForTest(bool sharp, bool failed) noexcept;
#endif

}  // namespace edvr
