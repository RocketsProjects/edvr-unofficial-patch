#pragma once

#include "../plugins/performance/fov_trim_policy.h"

#include <cmath>
#include <cstdint>

namespace edvr::openxr {

using NativeTrimStage = edvr::plugins::performance::NativeTrimStage;
using NativeTrimSettings = edvr::plugins::performance::NativeTrimSettings;
using NativeTrimFrustum = edvr::plugins::performance::NativeTrimFrustum;
using NativeTrimDimensions = edvr::plugins::performance::NativeTrimDimensions;
using NativeTrimBounds = edvr::plugins::performance::NativeTrimBounds;
using NativeTrimNudge = edvr::plugins::performance::NativeTrimNudge;
using NativeTrimRoute = edvr::plugins::performance::NativeTrimRoute;

constexpr float kNativeTrimMinEdgeDeg = edvr::plugins::performance::kNativeTrimMinEdgeDeg;
constexpr float kNativeTrimMinSpanDeg = edvr::plugins::performance::kNativeTrimMinSpanDeg;
constexpr uint32_t kNativeTrimAdoptGraceFrames = edvr::plugins::performance::kNativeTrimAdoptGraceFrames;
constexpr uint32_t kNativeTrimNudgePx = edvr::plugins::performance::kNativeTrimNudgePx;
constexpr uint32_t kNativeTrimNudgeMaxPx = edvr::plugins::performance::kNativeTrimNudgeMaxPx;
constexpr float kNativeTrimPi = edvr::plugins::performance::kNativeTrimPi;

// These types live in Performance. Re-export the same functions so unqualified
// runtime calls and argument-dependent lookup name one declaration.
using edvr::plugins::performance::nativeTrimNudgeName;
using edvr::plugins::performance::nativeTrimRouteName;
using edvr::plugins::performance::gameFacingDimensions;

inline float nativeTrimDegrees(float tangent) noexcept {
  return edvr::plugins::performance::nativeTrimDegrees(tangent);
}
inline float nativeTrimTangent(float degrees) noexcept {
  return edvr::plugins::performance::nativeTrimTangent(degrees);
}

// Runtime-side owner-thread facade. The state and every policy decision live
// in the Performance implementation; this object owns only the opaque handle
// and the latest fixed-size result record. Passing nullptr models an absent
// Performance implementation and keeps the runtime's located geometry.
class NativeFovTrim final {
 public:
  explicit NativeFovTrim(const edvr::plugins::performance::NativeFovTrimOpsV1*
                             ops = edvr::plugins::performance::nativeFovTrimOpsV1()) noexcept
      : requested_(ops != nullptr) {
    if (validOps(ops)) {
      opsStorage_ = *ops;
      ops_ = &opsStorage_;
    }
    if (ops_) {
      state_ = ops_->create(ops_->context);
      if (!state_) ops_ = nullptr;
    }
    resetSnapshot();
  }
  ~NativeFovTrim() { release(); }
  NativeFovTrim(const NativeFovTrim&) = delete;
  NativeFovTrim& operator=(const NativeFovTrim&) = delete;
  NativeFovTrim(NativeFovTrim&&) = delete;
  NativeFovTrim& operator=(NativeFovTrim&&) = delete;

  bool requested() const noexcept { return requested_; }
  bool ready() const noexcept { return !requested_ || (ops_ && state_); }

  NativeTrimStage beginFrame(const NativeTrimSettings& settings,
      const NativeTrimFrustum (&trueFrusta)[2],
      const NativeTrimDimensions (&runtimeDims)[2],
      uint64_t referenceGeneration, bool sizeAsked = true) noexcept {
    if (!ops_) {
      resetIdentity(settings, trueFrusta, runtimeDims);
      return stage();
    }
    fallbackSettings_ = settings;
    fallbackFrusta_[0] = trueFrusta[0]; fallbackFrusta_[1] = trueFrusta[1];
    fallbackDimensions_[0] = runtimeDims[0]; fallbackDimensions_[1] = runtimeDims[1];
    edvr::plugins::performance::NativeFovTrimInputV1 input{};
    input.settings = settings;
    input.trueFrusta[0] = trueFrusta[0]; input.trueFrusta[1] = trueFrusta[1];
    input.runtimeDimensions[0] = runtimeDims[0]; input.runtimeDimensions[1] = runtimeDims[1];
    input.referenceGeneration = referenceGeneration;
    input.sizeAsked = sizeAsked ? 1u : 0u;
    resetSnapshot();
    if (!ops_->beginFrame(ops_->context, state_, &input, &snapshot_) ||
        !validSnapshot(snapshot_)) {
      disablePolicy();
      resetIdentity(settings, trueFrusta, runtimeDims);
    }
    return stage();
  }

  void noteSubmittedSize(unsigned eye, uint32_t width, uint32_t height) noexcept {
    if (!ops_) return;
    if (!ops_->noteSubmittedSize(ops_->context, state_, eye, width, height,
                                 &snapshot_) || !validSnapshot(snapshot_)) {
      disablePolicy();
      resetIdentity(fallbackSettings_, fallbackFrusta_, fallbackDimensions_);
    }
  }
  void standDown() noexcept {
    if (ops_) ops_->standDown(ops_->context, state_);
  }

  NativeTrimStage stage() const noexcept { return static_cast<NativeTrimStage>(snapshot_.stage); }
  NativeTrimRoute route() const noexcept { return static_cast<NativeTrimRoute>(snapshot_.route); }
  bool pending() const noexcept { return stage() == NativeTrimStage::Adopting; }
  float factorWidth() const noexcept { return snapshot_.factorWidth; }
  float factorHeight() const noexcept { return snapshot_.factorHeight; }
  bool changed() const noexcept { return snapshot_.changed != 0; }
  bool baselineReady() const noexcept { return snapshot_.baselineReady != 0; }
  NativeTrimDimensions baseline(unsigned eye) const noexcept {
    return eye < 2 && baselineReady() ? snapshot_.eyes[eye].baseline : NativeTrimDimensions{};
  }
  NativeTrimDimensions baselineAsk(unsigned eye) const noexcept {
    return eye < 2 && baselineReady() ? snapshot_.eyes[eye].baselineAsk : NativeTrimDimensions{};
  }
  NativeTrimDimensions lastSubmitted(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].lastSubmitted : NativeTrimDimensions{};
  }
  uint32_t adoptingFrames() const noexcept { return snapshot_.adoptingFrames; }
  float appliedOuterDeg(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].appliedOuterDeg : 0.0f;
  }
  float appliedNasalDeg(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].appliedNasalDeg : 0.0f;
  }
  float appliedVerticalDeg(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].appliedVerticalDeg : 0.0f;
  }
  NativeTrimNudge nudge() const noexcept { return static_cast<NativeTrimNudge>(snapshot_.nudge); }
  uint32_t nudgeFloor() const noexcept { return snapshot_.nudgeFloor; }
  bool trimmed() const noexcept { return snapshot_.trimmed != 0; }
  NativeTrimFrustum runtimeFrustum(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].runtimeFrustum : NativeTrimFrustum{};
  }
  NativeTrimDimensions trueRecommended(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].trueRecommended : NativeTrimDimensions{};
  }
  NativeTrimDimensions recommended(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].recommended : NativeTrimDimensions{};
  }
  NativeTrimDimensions treatedFor(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].treatedFor : NativeTrimDimensions{};
  }
  NativeTrimFrustum gameFrustum(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].gameFrustum : NativeTrimFrustum{};
  }
  NativeTrimBounds placementBounds(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].placementBounds : NativeTrimBounds{};
  }
  NativeTrimFrustum contentFrustum(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].contentFrustum : NativeTrimFrustum{};
  }
  NativeTrimDimensions canonical(unsigned eye) const noexcept {
    return eye < 2 ? snapshot_.eyes[eye].canonical : NativeTrimDimensions{};
  }

 private:
  using Ops = edvr::plugins::performance::NativeFovTrimOpsV1;
  using Snapshot = edvr::plugins::performance::NativeFovTrimSnapshotV1;

  static bool validOps(const Ops* ops) noexcept {
    return ops && ops->structSize == sizeof(*ops) &&
        ops->version == edvr::plugins::performance::kNativeFovTrimApiVersion &&
        ops->create && ops->destroy && ops->beginFrame &&
        ops->noteSubmittedSize && ops->standDown;
  }
  static bool validSnapshot(const Snapshot& snapshot) noexcept {
    return snapshot.structSize == sizeof(snapshot) &&
        snapshot.version == edvr::plugins::performance::kNativeFovTrimApiVersion &&
        snapshot.reserved == 0;
  }
  void release() noexcept {
    if (ops_ && state_) ops_->destroy(ops_->context, state_);
    state_ = nullptr; ops_ = nullptr;
  }
  void disablePolicy() noexcept { release(); }
  void resetSnapshot() noexcept { snapshot_ = Snapshot{}; }
  void resetIdentity(const NativeTrimSettings& settings,
      const NativeTrimFrustum (&frusta)[2],
      const NativeTrimDimensions (&dims)[2]) noexcept {
    resetSnapshot();
    (void)settings;
    fallbackSettings_ = settings;
    fallbackFrusta_[0] = frusta[0]; fallbackFrusta_[1] = frusta[1];
    fallbackDimensions_[0] = dims[0]; fallbackDimensions_[1] = dims[1];
    for (unsigned eye = 0; eye < 2; ++eye) {
      // Elite's game-facing size still needs the mandatory even/valid guard
      // when the optional trim policy is absent or has failed.
      const NativeTrimDimensions gameDims{
          gameFacingDimension(dims[eye].width),
          gameFacingDimension(dims[eye].height)};
      auto& out = snapshot_.eyes[eye];
      out.runtimeFrustum = frusta[eye];
      out.gameFrustum = frusta[eye];
      out.contentFrustum = frusta[eye];
      out.trueRecommended = gameDims;
      out.recommended = gameDims;
      out.treatedFor = gameDims;
    }
  }

  // Copy the immutable table so its record itself need not outlive this host.
  // The opaque context, if any, remains caller-owned and must outlive it.
  Ops opsStorage_{};
  const Ops* ops_ = nullptr;
  void* state_ = nullptr;
  Snapshot snapshot_{};
  NativeTrimSettings fallbackSettings_{};
  NativeTrimFrustum fallbackFrusta_[2]{};
  NativeTrimDimensions fallbackDimensions_[2]{};
  bool requested_ = false;
};

}  // namespace edvr::openxr
