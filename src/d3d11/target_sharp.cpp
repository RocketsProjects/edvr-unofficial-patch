#include "target_sharp.h"

#include <cstdint>
#include <cstring>

#include "../plugins/cockpit_visuals/target_sharp_policy.h"
#include "target_sharp_policy_test.h"

namespace edvr {
namespace detail {
bool g_targetSharpSharp = false;
bool g_targetSharpFailed = false;
}  // namespace detail
namespace {
TargetSharpFastStateV1 g_fast{sizeof(TargetSharpFastStateV1),
                              kTargetSharpPolicyVersionV1, 0, 0, 0, 0,
                              kTargetSharpVertexShaderHashV1};
TargetSharpPolicyOpsV1 g_override{};
bool g_hasOverride = false;
bool g_overrideAbsent = false;
TargetSharpPolicyOpsV1 g_bracketOps{};
bool g_bracketPending = false;
ID3D11DeviceContext* g_bracketContext = nullptr;

const TargetSharpPolicyOpsV1* activeOps() noexcept {
    if (g_hasOverride) return g_overrideAbsent ? nullptr : &g_override;
    return targetSharpPolicyOpsV1();
}

bool validOps(const TargetSharpPolicyOpsV1* ops) noexcept {
    return ops && ops->structSize == sizeof(TargetSharpPolicyOpsV1) &&
           ops->version == kTargetSharpPolicyVersionV1 && ops->context &&
           ops->configure && ops->matches && ops->begin && ops->end &&
           ops->shutdown;
}

void clearFast() noexcept {
    g_fast = {sizeof(TargetSharpFastStateV1), kTargetSharpPolicyVersionV1,
              0, 0, 0, 0, kTargetSharpVertexShaderHashV1};
    detail::g_targetSharpSharp = false;
    detail::g_targetSharpFailed = false;
}

void clearBracket() noexcept {
    g_bracketPending = false;
    g_bracketContext = nullptr;
    g_bracketOps = {};
}

bool validFast(const TargetSharpFastStateV1& state) noexcept {
    return state.structSize == sizeof(TargetSharpFastStateV1) &&
           state.version == kTargetSharpPolicyVersionV1 && state.sharp <= 1 &&
           state.failed <= 1 && state.engaged <= 1 && state.reserved == 0 &&
           (!state.sharp || state.vertexShaderHash != 0);
}

void acceptFast(const TargetSharpFastStateV1& state) noexcept {
    if (!validFast(state)) {
        clearFast();
        return;
    }
    g_fast = state;
    detail::g_targetSharpSharp = state.sharp != 0;
    detail::g_targetSharpFailed = state.failed != 0;
}

template <bool Observe>
bool wantsDraws(TargetSharpObservation* observation, bool helper) noexcept {
    const bool sharp = detail::g_targetSharpSharp;
    if constexpr (Observe) {
        auto& read = helper ? observation->helperSharp : observation->outerSharp;
        read = {true, true, sharp};
    }
    if (!sharp) return false;
    const bool failed = detail::g_targetSharpFailed;
    if constexpr (Observe) {
        auto& read = helper ? observation->helperFailed : observation->outerFailed;
        read = {true, true, failed};
    }
    return !failed;
}
}  // namespace

void targetSharpConfigure(Config& cfg) {
    const TargetSharpPolicyOpsV1* ops = activeOps();
    if (!ops) { clearFast(); return; }
    TargetSharpFastStateV1 next = g_fast;
    ops->configure(ops->context, &cfg, &next);
    acceptFast(next);
}

bool targetSharpDrawInterestConfigured() noexcept {
    return detail::g_targetSharpSharp;
}

std::size_t targetSharpDrawInterestFilters(draw_interest::ShaderFilter* out,
                                           std::size_t capacity) noexcept {
    if (!detail::g_targetSharpSharp) return 0;
    if (out && capacity) {
        out[0] = {draw_interest::InterestId::TargetSharp,
                  draw_interest::HashFilter::Vertex,
                  g_fast.vertexShaderHash, 0};
    }
    return 1;
}

bool targetSharpWantsDrawsObserved(TargetSharpObservation& observation,
                                   bool helper) noexcept {
    return wantsDraws<true>(&observation, helper);
}

namespace {
template <bool Observe>
bool candidate(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                uint32_t instances, TargetSharpObservation* observation) {
    if (!wantsDraws<Observe>(observation, true)) return false;
    const TargetSharpPolicyOpsV1* ops = activeOps();
    if (!ops) return false;
    return ops->matches(ops->context, ctx, kind, count, instances, observation);
}
}  // namespace

bool targetSharpOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                          uint32_t instances) {
    return candidate<false>(ctx, kind, count, instances, nullptr);
}

bool targetSharpOnEyeDrawObserved(ID3D11DeviceContext* ctx, char kind,
                                  uint32_t count, uint32_t instances,
                                  TargetSharpObservation& observation) {
    return candidate<true>(ctx, kind, count, instances, &observation);
}

#if defined(EDVR_VSCREEN_PREDICATE_TEST)
void targetSharpPredicateTestSeed(bool sharp, bool failed) noexcept {
    g_fast.sharp = sharp ? 1u : 0u;
    g_fast.failed = failed ? 1u : 0u;
    g_fast.vertexShaderHash = kTargetSharpVertexShaderHashV1;
    if (!g_hasOverride) targetSharpPolicyPredicateSeedForTest(sharp, failed);
    detail::g_targetSharpSharp = sharp;
    detail::g_targetSharpFailed = failed;
}

std::uint64_t targetSharpPredicateTestConfiguredHash() noexcept {
    return g_fast.vertexShaderHash;
}
#endif

void targetSharpBegin(ID3D11DeviceContext* ctx) {
    if (g_bracketPending) return;  // Never overwrite a PS restoration still owed.
    if (!detail::g_targetSharpSharp || detail::g_targetSharpFailed) return;
    const TargetSharpPolicyOpsV1* ops = activeOps();
    if (!ops) return;
    g_bracketOps = *ops;
    g_bracketContext = ctx;
    g_bracketPending = true;
    TargetSharpFastStateV1 next = g_fast;
    try {
        ops->begin(ops->context, ctx, &next);
        acceptFast(next);
    } catch (...) {
        // Begin may have changed the game's PS before a later operation threw.
        // Keep the copied table/context until End has definitely restored it.
        try {
            TargetSharpFastStateV1 restored = g_fast;
            g_bracketOps.end(g_bracketOps.context, g_bracketContext, &restored);
            if (validFast(restored) && !restored.engaged) {
                acceptFast(restored);
                clearBracket();
            }
        } catch (...) {
            // An explicit End can retry; replacing the owner here could strand
            // the original PS or release its context while still engaged.
        }
        throw;
    }
}

void targetSharpEnd(ID3D11DeviceContext* ctx) {
    if (!g_bracketPending) return;
    TargetSharpFastStateV1 next = g_fast;
    g_bracketOps.end(g_bracketOps.context,
                     g_bracketContext ? g_bracketContext : ctx, &next);
    // A callback's valid engaged=0 snapshot promises the original PS is back.
    // Keep the copied owner/context for retry if that promise was not made.
    if (!validFast(next) || next.engaged) return;
    acceptFast(next);
    clearBracket();
}

void targetSharpShutdown() {
    if (g_bracketPending) return;
    const TargetSharpPolicyOpsV1* ops = activeOps();
    if (!ops) { clearFast(); return; }
    TargetSharpFastStateV1 next = g_fast;
    ops->shutdown(ops->context, &next);
    acceptFast(next);
}

bool targetSharpSetPolicyOpsV1ForTest(const TargetSharpPolicyOpsV1* ops) noexcept {
    if (g_bracketPending || g_fast.engaged) return false;
    if (ops && !validOps(ops)) return false;
    const TargetSharpPolicyOpsV1* old = activeOps();
    if (old) {
        TargetSharpFastStateV1 released = g_fast;
        try {
            old->shutdown(old->context, &released);
        } catch (...) {
            // The old table/context remains selected so teardown can retry.
            return false;
        }
    }
    // The factory pointer resets to the builtin selection. Keeping it as an
    // override would hide module-owned predicate seeding in the VScreen rig.
    const bool builtin = ops && ops == targetSharpPolicyOpsV1();
    g_hasOverride = !builtin;
    g_overrideAbsent = ops == nullptr;
    if (ops && !builtin) g_override = *ops;
    else std::memset(&g_override, 0, sizeof(g_override));
    clearFast();
    return true;
}

}  // namespace edvr
