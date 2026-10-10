#pragma once
#include "../../openxr/game_render_size.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace edvr::plugins::performance {

// The field-of-view trim's stage machine. It tells the game a narrower
// projection and a smaller render size, then waits for the game's own
// render-target rebuild to land before the narrower image is placed.
//
// This was the terrain guard's engine as well (a widened frustum, with a scene
// hold, a crop at submit and two projection channels); the guard was removed
// 2026-10-09 and the trim, which had always run through the same machine, kept
// it. The stage numbers are the trace's stage= and stay as they were.
enum class NativeTrimStage : uint32_t { Off = 0, Adopting = 2, Live = 3, Inert = 4 };

struct NativeTrimSettings {
  // Degrees taken off each eye's own edges before anything else happens, so
  // the game is told a narrower projection AND a smaller render size: every
  // pass it runs, upscaler included, is priced by that rectangle. Outer is
  // the temple side of each eye, nasal the nose side (which costs only the
  // stereo overlap), vertical both the top and the bottom edge. Zero leaves
  // the edge exactly where the runtime put it.
  float trimOuterDeg = 0.0f;
  float trimNasalDeg = 0.0f;
  float trimVerticalDeg = 0.0f;
};

struct NativeTrimFrustum { float left=0, right=0, down=0, up=0; };
struct NativeTrimDimensions { uint32_t width=0, height=0; };
struct NativeTrimBounds { float left=0, top=0, right=1, bottom=1; };

// The trim's own limits, in degrees. No edge is brought closer than
// kNativeTrimMinEdgeDeg to straight ahead and no axis is left narrower than
// kNativeTrimMinSpanDeg, whatever is asked for: a trim that closed an eye
// would look like a broken runtime rather than a setting.
constexpr float kNativeTrimMinEdgeDeg = 5.0f;
constexpr float kNativeTrimMinSpanDeg = 20.0f;
// Frames of adoption after which a target that has moved at all is taken as
// the rebuild although no ratio matched. The game's own quality multiplier
// can change underneath the ask (0.65 to 0.5 and back within one adoption in
// the flight of 2026-09-16), and then no ratio ever matches; the narrowed
// projection is correct at any target size. Three seconds at 90 Hz.
constexpr uint32_t kNativeTrimAdoptGraceFrames = 270;
// The nudge (NativeFovTrim::startNudge): rows taken off the lowest height
// the game has been told since its last rebuild, so that it rebuilds for a
// change it would otherwise ignore, and how many rows under the true ask
// the told height may fall before the ask is told as it is.
constexpr uint32_t kNativeTrimNudgePx = 2;
constexpr uint32_t kNativeTrimNudgeMaxPx = 40;
// What startNudge decided as a frame entered Adopting. First: no rebuild has
// been seen since the scene began, nothing to measure against. Same: the
// size did not move, only the angles, and the pair in hand already fits.
// Shrink: the height fell under the floor, the game rebuilds by itself.
// Taller: more rows than were told before, never nudged, the next apply
// builds them. Capped: the dip would cost more than kNativeTrimNudgeMaxPx
// rows. Started: the game is told the dipped height from this frame on.
enum class NativeTrimNudge : uint32_t { None, First, Same, Shrink, Taller, Capped, Started };
inline const char* nativeTrimNudgeName(NativeTrimNudge n) noexcept {
  switch (n) {
    case NativeTrimNudge::First: return "first";
    case NativeTrimNudge::Same: return "same";
    case NativeTrimNudge::Shrink: return "shrink";
    case NativeTrimNudge::Taller: return "taller";
    case NativeTrimNudge::Capped: return "capped";
    case NativeTrimNudge::Started: return "started";
    default: return "none";
  }
}
// How the adoption in hand, or the one that went live, was entered: the
// trace's route=. A trim tells the game exactly the frustum the headset shows,
// a function of the runtime's frustum and the settings only, so it is told
// from the first frame both are known. FirstAsk: told before the game had read
// any size, so every pair it renders is rendered for this ask and the first
// complete pair promotes, with no baseline. Rebuild: told after the game has
// read an earlier size: a baseline pair, then its rebuild, as a change while
// live has always been landed.
enum class NativeTrimRoute : uint32_t { None, FirstAsk, Rebuild };
inline const char* nativeTrimRouteName(NativeTrimRoute r) noexcept {
  switch (r) {
    case NativeTrimRoute::FirstAsk: return "first_ask";
    case NativeTrimRoute::Rebuild: return "rebuild";
    default: return "none";
  }
}
constexpr float kNativeTrimPi = 3.1415926535f;

inline float nativeTrimDegrees(float tangent) noexcept {
  return std::atan(tangent) * 180.0f / kNativeTrimPi;
}
inline float nativeTrimTangent(float degrees) noexcept {
  return std::tan(degrees * kNativeTrimPi / 180.0f);
}

// Elite applies its HMD quality multiplier with integer truncation. Keep the
// game-facing recommendation even on both axes so quality 0.5 cannot lose a
// pixel from an odd runtime or trimmed size. The XR swapchain dimensions stay
// in the host's raw `sizes` records; this helper is only for game geometry.
inline NativeTrimDimensions gameFacingDimensions(NativeTrimDimensions v) noexcept {
  return {edvr::openxr::gameFacingDimension(v.width),
          edvr::openxr::gameFacingDimension(v.height)};
}

constexpr uint32_t kNativeFovTrimApiVersion = 1;

struct NativeFovTrimInputV1 {
  uint32_t structSize = sizeof(NativeFovTrimInputV1);
  uint32_t version = kNativeFovTrimApiVersion;
  NativeTrimSettings settings{};
  NativeTrimFrustum trueFrusta[2]{};
  NativeTrimDimensions runtimeDimensions[2]{};
  uint64_t referenceGeneration = 0;
  uint32_t sizeAsked = 1;
  uint32_t reserved = 0;
};

struct NativeFovTrimEyeV1 {
  NativeTrimFrustum runtimeFrustum{};
  NativeTrimFrustum gameFrustum{};
  NativeTrimFrustum contentFrustum{};
  NativeTrimBounds placementBounds{};
  NativeTrimDimensions trueRecommended{};
  NativeTrimDimensions recommended{};
  NativeTrimDimensions treatedFor{};
  NativeTrimDimensions canonical{};
  NativeTrimDimensions baseline{};
  NativeTrimDimensions baselineAsk{};
  NativeTrimDimensions lastSubmitted{};
  float appliedOuterDeg = 0;
  float appliedNasalDeg = 0;
  float appliedVerticalDeg = 0;
};

struct NativeFovTrimSnapshotV1 {
  uint32_t structSize = sizeof(NativeFovTrimSnapshotV1);
  uint32_t version = kNativeFovTrimApiVersion;
  uint32_t stage = 0;
  uint32_t route = 0;
  uint32_t nudge = 0;
  uint32_t changed = 0;
  uint32_t baselineReady = 0;
  uint32_t adoptingFrames = 0;
  uint32_t nudgeFloor = 0;
  uint32_t trimmed = 0;
  uint32_t reserved = 0;
  float factorWidth = 1;
  float factorHeight = 1;
  NativeFovTrimEyeV1 eyes[2]{};
};

using NativeFovTrimCreateV1 = void* (*)(void* context) noexcept;
using NativeFovTrimDestroyV1 = void (*)(void* context, void* state) noexcept;
using NativeFovTrimBeginFrameV1 = uint32_t (*)(void* context, void* state,
    const NativeFovTrimInputV1* input, NativeFovTrimSnapshotV1* output) noexcept;
using NativeFovTrimSubmitV1 = uint32_t (*)(void* context, void* state,
    uint32_t eye, uint32_t width, uint32_t height,
    NativeFovTrimSnapshotV1* output) noexcept;
using NativeFovTrimStandDownV1 = void (*)(void* context, void* state) noexcept;

struct NativeFovTrimOpsV1 {
  uint32_t structSize = sizeof(NativeFovTrimOpsV1);
  uint32_t version = kNativeFovTrimApiVersion;
  void* context = nullptr;
  NativeFovTrimCreateV1 create = nullptr;
  NativeFovTrimDestroyV1 destroy = nullptr;
  NativeFovTrimBeginFrameV1 beginFrame = nullptr;
  NativeFovTrimSubmitV1 noteSubmittedSize = nullptr;
  NativeFovTrimStandDownV1 standDown = nullptr;
};

const NativeFovTrimOpsV1* nativeFovTrimOpsV1() noexcept;

static_assert(sizeof(NativeTrimSettings) == 12, "FOV trim settings POD");
static_assert(sizeof(NativeTrimFrustum) == 16, "FOV trim frustum POD");
static_assert(sizeof(NativeTrimDimensions) == 8, "FOV trim dimensions POD");
static_assert(sizeof(NativeTrimBounds) == 16, "FOV trim bounds POD");
static_assert(sizeof(NativeFovTrimInputV1) == 88, "FOV trim input v1 x64");
static_assert(sizeof(NativeFovTrimEyeV1) == 132, "FOV trim eye v1");
static_assert(sizeof(NativeFovTrimSnapshotV1) == 316, "FOV trim snapshot v1");
static_assert(sizeof(NativeFovTrimOpsV1) == 56, "FOV trim ops v1 x64");
static_assert(std::is_standard_layout<NativeTrimSettings>::value &&
              std::is_trivially_copyable<NativeTrimSettings>::value &&
              std::is_standard_layout<NativeTrimFrustum>::value &&
              std::is_trivially_copyable<NativeTrimFrustum>::value &&
              std::is_standard_layout<NativeTrimDimensions>::value &&
              std::is_trivially_copyable<NativeTrimDimensions>::value &&
              std::is_standard_layout<NativeTrimBounds>::value &&
              std::is_trivially_copyable<NativeTrimBounds>::value,
              "FOV trim geometry/settings are fixed-record ABI data");
static_assert(offsetof(NativeFovTrimInputV1, settings) == 8 &&
              offsetof(NativeFovTrimInputV1, trueFrusta) == 20 &&
              offsetof(NativeFovTrimInputV1, runtimeDimensions) == 52 &&
              offsetof(NativeFovTrimInputV1, referenceGeneration) == 72 &&
              offsetof(NativeFovTrimInputV1, reserved) == 84,
              "FOV trim input v1 offsets");
static_assert(offsetof(NativeFovTrimSnapshotV1, eyes) == 52 &&
              offsetof(NativeFovTrimSnapshotV1, reserved) == 40 &&
              offsetof(NativeFovTrimSnapshotV1, factorWidth) == 44 &&
              offsetof(NativeFovTrimEyeV1, recommended) == 72 &&
              offsetof(NativeFovTrimEyeV1, lastSubmitted) == 112 &&
              offsetof(NativeFovTrimEyeV1, appliedOuterDeg) == 120,
              "FOV trim snapshot v1 offsets");
static_assert(std::is_standard_layout<NativeFovTrimEyeV1>::value &&
              std::is_trivially_copyable<NativeFovTrimEyeV1>::value,
              "FOV trim eye is fixed-record ABI data");
static_assert(offsetof(NativeFovTrimOpsV1, context) == 8 &&
              offsetof(NativeFovTrimOpsV1, beginFrame) == 32 &&
              offsetof(NativeFovTrimOpsV1, standDown) == 48,
              "FOV trim ops v1 offsets");
static_assert(std::is_standard_layout<NativeFovTrimInputV1>::value &&
              std::is_trivially_copyable<NativeFovTrimInputV1>::value,
              "FOV trim input is fixed-record ABI data");
static_assert(std::is_standard_layout<NativeFovTrimSnapshotV1>::value &&
              std::is_trivially_copyable<NativeFovTrimSnapshotV1>::value,
              "FOV trim snapshot is fixed-record ABI data");
static_assert(std::is_standard_layout<NativeFovTrimOpsV1>::value &&
              std::is_trivially_copyable<NativeFovTrimOpsV1>::value,
              "FOV trim ops are fixed-record ABI data");

}  // namespace edvr::plugins::performance
