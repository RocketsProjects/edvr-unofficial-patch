#pragma once
// The scene inspector's pure half (scene_inspector.cpp; docs/scene-inspector-2026-10-14.md): the box at the view centre and its mapping
// to eye pixels (canted displays included), which draws own what at the centre of the box, the camera-term motion cross-check, the
// verdict rules, every log and CSV line, the PNG writer and the image conversions. No Windows, no D3D11, no engine memory. The rig
// tools\scene_inspector_test compiles this header alone (and links scene_inspector.cpp for the device-side restore proof).
//
// A press captures ONE frame of the left eye. Every eye draw is duplicated once more, scissored to the box and then to its centre pixel,
// writing nothing, under an occlusion query; a draw whose box count is above zero is INVOLVED. Involved draws are classified against
// the draws after them in the same render target (an opaque depth-writing draw drawn later that also reaches the centre buries them),
// the surface at the centre is the stack of draws still visible there, and each involved draw gets a verdict from the rules below.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "blend_probe_core.h"   // the state names, the decision-flag decoder and the text helpers both instruments share

// windows.h defines near and far as empty macros; this header uses both as names (the far plane, an approximate comparison in the rig).
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

namespace edvr::inspect {

using blend::BlendDesc;
using blend::DecisionFlags;
using blend::DepthDesc;
using blend::Rect;

constexpr uint32_t kBox = 16;             // the box's side, in render pixels of the target a draw writes
constexpr uint32_t kCrop = 128;           // the PNG crops' side, in DLSS input pixels
constexpr uint32_t kMaxDraws = 6000;      // eye draws tested per capture (two queries each); the rest are counted
constexpr uint32_t kMaxListed = 300;      // involved draws listed in full; the rest are counted
constexpr uint32_t kReadTicksMax = 90;    // frame boundaries the answers are waited for before the report is written anyway
constexpr size_t kLineMax = 1100;         // the log keeps 1200 bytes a line; no inspector line reaches the truncation marker
constexpr size_t kLineBuf = 1200;

// ---- the box ------------------------------------------------------------------------------------------------------------------------

// A projection frustum as the native runtime advertises it: tangents, x right and y UP, so left < 0 < right and down < 0 < up. This is the
// shader's own tanNow order ({l, r, down, up}; temporal_shader_source.h builds the ray as d.y = up - v * (up - down)).
struct Frustum {
    float left = -1.0f, right = 1.0f, down = -1.0f, up = 1.0f;
};

struct Centre {
    bool ok = false;       // the direction is in front of the eye and the frustum is sane
    bool inside = false;   // and its point lies inside the image
    float u = 0.5f, v = 0.5f;   // the point, 0..1 across the image, v downwards
    float tanX = 0.0f, tanY = 0.0f;   // the ray's tangents in the eye's own frame (y up)
    float cantDeg = 0.0f;  // how far the eye looks from the head's forward direction (0 on a parallel display)
};

// The angle between an eye's forward axis and the head's, from the eye-to-head rotation (row-major 3x3; OpenVR/OpenXR: -z forward).
inline float cantDegrees(const float eyeToHead[9]) noexcept {
    if (!eyeToHead) return 0.0f;
    float c = eyeToHead[8];
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return std::acos(c) * 57.29578f;
}

// Where a direction given in HEAD space lands in an eye's image. eyeToHead is the eye's rotation to the head (row-major 3x3, nullptr = no
// canting); the direction in the eye's frame is its transpose times the head's. The head's forward direction is (0, 0, -1); a gaze
// direction goes through the same function. On a canted display the eye's axis is turned away from the head's, so the point is NOT the
// image centre, and on any display with an asymmetric frustum (all of them) it is not the centre either.
inline Centre headDirectionToImage(const Frustum& f, const float* eyeToHead, const float dirHead[3]) noexcept {
    Centre c;
    const float d[3] = {dirHead ? dirHead[0] : 0.0f, dirHead ? dirHead[1] : 0.0f, dirHead ? dirHead[2] : -1.0f};
    float e[3] = {d[0], d[1], d[2]};
    if (eyeToHead) {
        e[0] = eyeToHead[0] * d[0] + eyeToHead[3] * d[1] + eyeToHead[6] * d[2];
        e[1] = eyeToHead[1] * d[0] + eyeToHead[4] * d[1] + eyeToHead[7] * d[2];
        e[2] = eyeToHead[2] * d[0] + eyeToHead[5] * d[1] + eyeToHead[8] * d[2];
    }
    c.cantDeg = cantDegrees(eyeToHead);
    const float w = f.right - f.left, h = f.up - f.down;
    if (!(e[2] < -1e-6f) || !(w > 0.0f) || !(h > 0.0f) || !std::isfinite(w) || !std::isfinite(h)) return c;
    c.tanX = e[0] / -e[2];
    c.tanY = e[1] / -e[2];
    c.u = (c.tanX - f.left) / w;
    c.v = (f.up - c.tanY) / h;
    c.ok = std::isfinite(c.u) && std::isfinite(c.v);
    c.inside = c.ok && c.u >= 0.0f && c.u < 1.0f && c.v >= 0.0f && c.v < 1.0f;
    return c;
}

// The head's forward direction in an eye's image (the default centre).
inline Centre headForwardToImage(const Frustum& f, const float* eyeToHead) noexcept {
    const float fwd[3] = {0.0f, 0.0f, -1.0f};
    return headDirectionToImage(f, eyeToHead, fwd);
}

// The box in a target: `side` pixels square about the point (cx, cy), moved (not shrunk) to lie inside [0,w) x [0,h) when the target is big
// enough, and the whole target when it is not. False for an empty target.
inline bool boxAround(float cx, float cy, uint32_t side, uint32_t w, uint32_t h, Rect* out) noexcept {
    if (!out || w == 0 || h == 0 || side == 0 || !std::isfinite(cx) || !std::isfinite(cy)) return false;
    auto place = [side](float c, uint32_t extent, int32_t* lo, int32_t* hi) {
        const int32_t half = static_cast<int32_t>(side / 2);
        int32_t a = static_cast<int32_t>(std::floor(c)) - half;
        if (extent <= side) {
            a = 0;
            *lo = 0;
            *hi = static_cast<int32_t>(extent);
            return;
        }
        if (a < 0) a = 0;
        if (a + static_cast<int32_t>(side) > static_cast<int32_t>(extent)) a = static_cast<int32_t>(extent - side);
        *lo = a;
        *hi = a + static_cast<int32_t>(side);
    };
    Rect r;
    place(cx, w, &r.x0, &r.x1);
    place(cy, h, &r.y0, &r.y1);
    *out = r;
    return true;
}

// The one pixel the centre query measures: the pixel holding the point, kept inside the box.
inline void centrePixel(float cx, float cy, const Rect& box, int32_t* px, int32_t* py) noexcept {
    int32_t x = static_cast<int32_t>(std::floor(cx)), y = static_cast<int32_t>(std::floor(cy));
    if (x < box.x0) x = box.x0;
    if (x >= box.x1) x = box.x1 - 1;
    if (y < box.y0) y = box.y0;
    if (y >= box.y1) y = box.y1 - 1;
    if (px) *px = x;
    if (py) *py = y;
}

// A point of the image (u, v in 0..1) in a viewport of a target: the pixel position in that target.
inline void imagePointToTarget(float u, float v, float vpX, float vpY, float vpW, float vpH, float* x, float* y) noexcept {
    if (x) *x = vpX + u * vpW;
    if (y) *y = vpY + v * vpH;
}

// The crop of the DLSS input about the box centre: `side` pixels square, moved inside the input. False for an empty input.
inline bool cropAround(float cx, float cy, uint32_t side, uint32_t w, uint32_t h, Rect* out) noexcept {
    return boxAround(cx, cy, side, w, h, out);
}

// ---- state classes ----------------------------------------------------------------------------------------------------------------

enum class Cls : uint8_t { Opaque, Alpha, Additive, Premult, Multiply, OtherBlend, NoColour };
inline const char* clsName(Cls c) noexcept {
    switch (c) {
        case Cls::Opaque: return "opaque";
        case Cls::Alpha: return "alpha";
        case Cls::Additive: return "additive";
        case Cls::Premult: return "premult";
        case Cls::Multiply: return "multiply";
        case Cls::OtherBlend: return "blend-other";
        default: return "no-colour";
    }
}
inline bool clsTranslucent(Cls c) noexcept { return c != Cls::Opaque && c != Cls::NoColour; }

inline Cls classify(const BlendDesc& b) noexcept {
    if ((b.writeMask & 0xFu) == 0) return Cls::NoColour;
    if (!b.enable) return Cls::Opaque;
    // D3D11_BLEND: 1 ZERO, 2 ONE, 3 SRC_COLOR, 5 SRC_ALPHA, 6 INV_SRC_ALPHA, 9 DEST_COLOR; D3D11_BLEND_OP 1 ADD
    if (b.op == 1 && b.dst == 2 && (b.src == 2 || b.src == 5 || b.src == 3)) return Cls::Additive;
    if (b.op == 1 && b.src == 5 && b.dst == 6) return Cls::Alpha;
    if (b.op == 1 && b.src == 2 && b.dst == 6) return Cls::Premult;
    if (b.op == 1 && ((b.src == 9 && b.dst == 1) || (b.src == 1 && b.dst == 3))) return Cls::Multiply;
    // ONE, ZERO, ADD with blending "on" replaces the target like an opaque draw
    if (b.op == 1 && b.src == 2 && b.dst == 1) return Cls::Opaque;
    return Cls::OtherBlend;
}

// Can a later draw of this state hide an earlier one at the pixels it reaches: it replaces the colour outright (no blend, all of RGB
// written, no alpha-to-coverage) and takes the depth.
inline bool isOccluder(Cls c, bool depthWrite, const BlendDesc& b) noexcept {
    return c == Cls::Opaque && depthWrite && (b.writeMask & 7u) == 7u && !b.alphaToCoverage;
}

inline const char* topologyName(uint32_t t) noexcept {
    switch (t) {
        case 1: return "POINTLIST";
        case 2: return "LINELIST";
        case 3: return "LINESTRIP";
        case 4: return "TRIANGLELIST";
        case 5: return "TRIANGLESTRIP";
        case 10: return "LINELIST_ADJ";
        case 11: return "LINESTRIP_ADJ";
        case 12: return "TRIANGLELIST_ADJ";
        case 13: return "TRIANGLESTRIP_ADJ";
        case 0: return "UNDEFINED";
        default: return (t >= 33 && t <= 64) ? "PATCHLIST" : "OTHER";
    }
}
inline bool topologyThin(uint32_t t) noexcept { return t == 1 || t == 2 || t == 3 || t == 10 || t == 11; }

inline const char* stencilOpName(uint32_t v) noexcept {   // D3D11_STENCIL_OP
    switch (v) {
        case 1: return "KEEP";
        case 2: return "ZERO";
        case 3: return "REPLACE";
        case 4: return "INCR_SAT";
        case 5: return "DECR_SAT";
        case 6: return "INVERT";
        case 7: return "INCR";
        case 8: return "DECR";
        default: return "?";
    }
}
inline const char* cullName(uint32_t v) noexcept {   // D3D11_CULL_MODE
    switch (v) {
        case 1: return "NONE";
        case 2: return "FRONT";
        case 3: return "BACK";
        default: return "?";
    }
}

struct StencilDesc {
    bool enable = false;
    uint32_t ref = 0, readMask = 0xFF, writeMask = 0xFF;
    uint32_t func = 8, passOp = 1, failOp = 1, depthFailOp = 1;   // the front face's
};
inline int formatStencil(char* out, size_t cap, const StencilDesc& s) noexcept {
    if (!s.enable) return std::snprintf(out, cap, "off");
    return std::snprintf(out, cap, "ref=%u func=%s pass=%s fail=%s zfail=%s rd=%02X wr=%02X", s.ref, blend::compareName(s.func),
                         stencilOpName(s.passOp), stencilOpName(s.failOp), stencilOpName(s.depthFailOp), s.readMask & 0xFFu,
                         s.writeMask & 0xFFu);
}

// DXGI format names by number (the core has no dxgi header): the ones an eye pass meets, else "fmtN".
inline const char* formatName(uint32_t f, char* buf, size_t cap) noexcept {
    switch (f) {
        case 0: return "UNKNOWN";
        case 2: return "R32G32B32A32_FLOAT";
        case 10: return "R16G16B16A16_FLOAT";
        case 16: return "R32G32_FLOAT";
        case 23: return "R10G10B10A2_TYPELESS";
        case 24: return "R10G10B10A2_UNORM";
        case 26: return "R11G11B10_FLOAT";
        case 27: return "R8G8B8A8_TYPELESS";
        case 28: return "R8G8B8A8_UNORM";
        case 29: return "R8G8B8A8_UNORM_SRGB";
        case 34: return "R16G16_FLOAT";
        case 35: return "R16G16_UNORM";
        case 39: return "R32_TYPELESS";
        case 40: return "D32_FLOAT";
        case 41: return "R32_FLOAT";
        case 44: return "R24G8_TYPELESS";
        case 45: return "D24_UNORM_S8_UINT";
        case 49: return "R8G8_UNORM";
        case 54: return "R16_FLOAT";
        case 55: return "D16_UNORM";
        case 61: return "R8_UNORM";
        case 19: return "R32G8X24_TYPELESS";
        case 20: return "D32_FLOAT_S8X24_UINT";
        case 87: return "B8G8R8A8_UNORM";
        case 88: return "B8G8R8X8_UNORM";
        case 90: return "B8G8R8A8_TYPELESS";
        case 91: return "B8G8R8A8_UNORM_SRGB";
        default:
            std::snprintf(buf, cap, "fmt%u", f);
            return buf;
    }
}

// ---- who owns the centre ----------------------------------------------------------------------------------------------------------

// What the classifier needs of one tested draw, in issue order. box and ctr are the samples the draw wrote in the box and at the centre
// pixel (kUnknown when the query did not answer or could not be made).
constexpr int64_t kUnknown = -1;
struct DrawFact {
    uint32_t rt = 0;            // which render target (an index the capture numbers in the order it meets them)
    bool occluder = false;      // isOccluder()
    bool depthWrite = false;
    int64_t box = kUnknown, ctr = kUnknown;
};
enum class Status : uint8_t {
    Outside,      // wrote nothing in the box
    Top,          // visible where it matters: it wrote the centre pixel and no later occluder did, or it did not write the centre and no later occluder reached the box
    Buried,       // wrote the centre pixel and a later occluder did too: hidden at the centre
    Partial,      // did not write the centre pixel, and a later occluder writes somewhere in the box: whether it survives anywhere is unproven
    Unmeasured    // the count is unknown
};
inline const char* statusName(Status s) noexcept {
    switch (s) {
        case Status::Outside: return "outside";
        case Status::Top: return "top";
        case Status::Buried: return "buried";
        case Status::Partial: return "partial";
        default: return "unmeasured";
    }
}
struct StatusOut {
    Status status = Status::Outside;
    int32_t by = -1;   // Buried: the last (visible) occluder at the centre
};

// One backward pass: per render target, whether a later occluder reached the box and the last that reached the centre pixel. The centre pixel is
// the one place the query answers exactly, so a draw that wrote it is judged there (visible or buried) whatever happened elsewhere in the box.
inline void classifyStatus(const DrawFact* d, size_t n, StatusOut* out) noexcept {
    uint32_t maxRt = 0;
    for (size_t i = 0; i < n; ++i) maxRt = std::max(maxRt, d[i].rt);
    struct Later {
        bool anyBox = false;
        int32_t ctrIdx = -1;
    };
    std::vector<Later> later(static_cast<size_t>(maxRt) + 1);
    for (size_t k = n; k-- > 0;) {
        StatusOut& o = out[k];
        o = StatusOut{};
        const DrawFact& f = d[k];
        Later& l = later[f.rt];
        if (f.box == kUnknown) {
            o.status = Status::Unmeasured;
        } else if (f.box == 0) {
            o.status = Status::Outside;
        } else if (f.ctr > 0) {
            if (l.ctrIdx >= 0) {
                o.status = Status::Buried;
                o.by = l.ctrIdx;
            } else {
                o.status = Status::Top;
            }
        } else {
            o.status = l.anyBox ? Status::Partial : Status::Top;
        }
        if (f.occluder && f.box > 0) {
            l.anyBox = true;
            if (f.ctr > 0 && l.ctrIdx < 0) l.ctrIdx = static_cast<int32_t>(k);
        }
    }
}

// The draws visible at the centre pixel of one target, bottom to top: the last occluder that wrote it, then everything drawn after it
// that wrote it too. With no occluder at the centre the base is whatever the target held before (a clear or an earlier pass) and the
// stack is every draw that wrote it. depthOwner is the stack's last draw that wrote depth (the one DLSS's depth and motion come from), -1 if none.
inline size_t buildStack(const DrawFact* d, size_t n, uint32_t rt, int32_t* idx, size_t cap, int32_t* depthOwner) noexcept {
    int32_t last = -1;
    for (size_t i = 0; i < n; ++i)
        if (d[i].rt == rt && d[i].occluder && d[i].ctr > 0) last = static_cast<int32_t>(i);
    size_t m = 0;
    int32_t owner = -1;
    for (size_t i = last < 0 ? 0 : static_cast<size_t>(last); i < n; ++i) {
        if (d[i].rt != rt || !(d[i].ctr > 0)) continue;
        if (m < cap) idx[m++] = static_cast<int32_t>(i);
        if (d[i].depthWrite) owner = static_cast<int32_t>(i);
    }
    if (depthOwner) *depthOwner = owner;
    return m;
}

// ---- the camera term (the cross-check) ---------------------------------------------------------------------------------------------

// What the temporal pass's own shader would write for a pixel on the CAMERA path (temporal_shader_source.h fetchHistoryT): the ray of the
// pixel, rotated by the delta rows, scaled to the pixel's depth, plus the translation, projected through last frame's frustum. The
// supplied motion is compared against it; on a camera path the two must agree to a hair, on an engine path the difference IS the object's
// own motion relative to the world.
struct CamTerm {
    uint32_t w = 0, h = 0;
    float tanNow[4] = {};     // l r down up, jitter excluded
    float tanPrev[4] = {};
    float rows[3][3] = {};    // the delta rotation, one row each (xyz)
    float tv[3] = {};         // the translation term
    bool valid = false;
};
// The pixel's view depth in metres from the pass's raw reversed-Z depth copy: depth = x + z / metres (knobs.x, knobs.z). False for the far plane.
inline bool depthToMetres(float raw, float knobX, float knobZ, float* metres) noexcept {
    const float den = raw - knobX;
    if (!(raw > 0.0f) || !(den > 0.0f) || !std::isfinite(den)) return false;
    const float z = knobZ / den;
    if (!std::isfinite(z) || z <= 0.0f) return false;
    if (metres) *metres = z;
    return true;
}
inline bool expectedMotion(const CamTerm& c, float px, float py, bool far, float metres, float* mvx, float* mvy) noexcept {
    if (!c.valid || c.w == 0 || c.h == 0) return false;
    const float d[3] = {c.tanNow[0] + (px + 0.5f) / static_cast<float>(c.w) * (c.tanNow[1] - c.tanNow[0]),
                        c.tanNow[3] - (py + 0.5f) / static_cast<float>(c.h) * (c.tanNow[3] - c.tanNow[2]), -1.0f};
    float dp[3];
    for (int r = 0; r < 3; ++r) dp[r] = c.rows[r][0] * d[0] + c.rows[r][1] * d[1] + c.rows[r][2] * d[2];
    if (!far) {
        for (int r = 0; r < 3; ++r) dp[r] = dp[r] * metres + c.tv[r];
    }
    if (dp[2] >= -1e-6f) return false;
    const float xt = dp[0] / -dp[2], yt = dp[1] / -dp[2];
    const float ppx = (xt - c.tanPrev[0]) / (c.tanPrev[1] - c.tanPrev[0]) * static_cast<float>(c.w) - 0.5f;
    const float ppy = (c.tanPrev[3] - yt) / (c.tanPrev[3] - c.tanPrev[2]) * static_cast<float>(c.h) - 0.5f;
    if (!std::isfinite(ppx) || !std::isfinite(ppy)) return false;
    if (mvx) *mvx = ppx - px;
    if (mvy) *mvy = ppy - py;
    return true;
}
inline float dist2(float ax, float ay, float bx, float by) noexcept { return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }

// ---- the pixel ------------------------------------------------------------------------------------------------------------------------

struct PixelFacts {
    bool valid = false;           // the decision texture was read here
    float depth = 0.0f;           // the pass's raw depth copy (0 = far); NAN when not staged
    float metres = 0.0f;
    bool far = true;
    float mvx = 0.0f, mvy = 0.0f;     // the pass's physical motion, render pixels
    bool haveDlss = false;
    float dlx = 0.0f, dly = 0.0f;     // the motion vector DLSS was handed
    bool haveWorld = false, haveHead = false;
    float expWx = 0.0f, expWy = 0.0f;   // the camera term on the world (camera rows) and on the ship (the head's delta)
    float expHx = 0.0f, expHy = 0.0f;
    DecisionFlags flags;
    int coverage = -1, bias = -1;
};

// The motion the camera term predicts for the path the pixel took, else the nearer of the two. The decision texture's path 2 is the WORLD
// camera term (the camera rows, the pixel farther than the ship split or at the far plane) and path 1 the head's delta ("ship");
// temporal_shader_source.h sets decisionPath = count15 ? 2 : 1 and count15 only in the world branch. (blend_probe_core.h's legend says the
// opposite; it is wrong.)
inline bool expectedForPath(const PixelFacts& p, float* x, float* y) noexcept {
    if (p.flags.path == 2 && p.haveWorld) {
        *x = p.expWx;
        *y = p.expWy;
        return true;
    }
    if (p.flags.path == 1 && p.haveHead) {
        *x = p.expHx;
        *y = p.expHy;
        return true;
    }
    if (p.haveWorld && p.haveHead) {
        const bool w = dist2(p.mvx, p.mvy, p.expWx, p.expWy) <= dist2(p.mvx, p.mvy, p.expHx, p.expHy);
        *x = w ? p.expWx : p.expHx;
        *y = w ? p.expWy : p.expHy;
        return true;
    }
    if (p.haveWorld) {
        *x = p.expWx;
        *y = p.expWy;
        return true;
    }
    if (p.haveHead) {
        *x = p.expHx;
        *y = p.expHy;
        return true;
    }
    return false;
}
inline const char* pathName(uint32_t p) noexcept {
    switch (p) {
        case 1: return "camera-ship";
        case 2: return "camera-world";
        case 8: return "cockpit-hologram";
        case 10: return "screen";
        case 11: return "engine-record";
        case 12: return "planet-patch";
        default: return "other";
    }
}

// ---- which targets DLSS sees --------------------------------------------------------------------------------------------------------------

// A render target a draw wrote, as DLSS relates to it. The temporal pass says which texture the game submitted (DLSS's colour input) and which
// texture is the scene depth it reads; a target that is neither, and does not share that depth, is an intermediate pass (lighting, bloom, a
// shadow or a mask): its draws are listed but no DLSS rule is applied to them.
enum class Role : uint8_t { Unknown, Colour, Depth, ColourAndDepth, Intermediate };
inline Role roleOf(bool pass_known, bool isColourSource, bool sharesSceneDepth) noexcept {
    if (!pass_known) return Role::Unknown;
    if (isColourSource && sharesSceneDepth) return Role::ColourAndDepth;
    if (isColourSource) return Role::Colour;
    if (sharesSceneDepth) return Role::Depth;
    return Role::Intermediate;
}
inline bool roleIsDlssInput(Role r) noexcept { return r != Role::Intermediate; }
inline bool roleHasSceneDepth(Role r) noexcept { return r == Role::Depth || r == Role::ColourAndDepth; }
inline const char* roleName(Role r) noexcept {
    switch (r) {
        case Role::Colour: return "colour";
        case Role::Depth: return "depth";
        case Role::ColourAndDepth: return "colour+depth";
        case Role::Intermediate: return "intermediate";
        default: return "unknown";
    }
}
inline const char* roleText(Role r) noexcept {
    switch (r) {
        case Role::Colour: return "DLSS colour input (the texture the game submits)";
        case Role::Depth: return "DLSS depth target (shares the scene depth DLSS reads)";
        case Role::ColourAndDepth: return "DLSS colour input and depth target";
        case Role::Intermediate: return "intermediate pass, not a DLSS input";
        default: return "role unknown (the temporal pass offered no inputs, so the DLSS targets are not known)";
    }
}

// ---- the verdict rules ------------------------------------------------------------------------------------------------------------------

struct VerdictIn {
    Role role = Role::Unknown;    // Intermediate: no DLSS rule applies
    Cls cls = Cls::Opaque;
    Status status = Status::Top;
    int32_t buriedBy = -1;
    bool depthWrite = false, depthTest = false;
    int64_t box = 0, ctr = 0;
    uint32_t topology = 4;
    // what EDVR did with the draw
    bool routeNone = false;       // no verdict, no UI layer decision, no hologram, no coverage mask, no engine key
    bool uiTaken = false;
    bool holo = false;
    bool masked = false;          // the coverage family marks the reactive mask for it
    const char* maskSlot = "";
    bool keyed = false;           // its shader pair is in kFamilies (or self-marking)
    bool engineSub = false;       // the pool shaders were substituted for it
    // where it sits at the centre
    bool isDepthOwner = false;
    bool aboveOwner = false;      // in the centre stack, over the draw that owns the depth
    int32_t depthOwnerIdx = -1;
    bool havePixel = false;
    PixelFacts px;
    uint32_t boxArea = kBox * kBox;
};
struct VerdictOut {
    char code[28] = {};
    char text[920] = {};
};

constexpr float kMotionTolerancePx = 0.5f;
constexpr int64_t kThinSamples = 8;   // under this many box samples a draw is sub-pixel thin at the centre

// Composed from independent findings, joined by "; ". The code is the first finding that applies in the order below: what makes DLSS
// treat the pixel wrongly first, then the facts that explain it, then the cases where EDVR already handles the draw.
inline void verdictFor(const VerdictIn& in, VerdictOut* out) noexcept {
    if (!out) return;
    *out = VerdictOut{};
    const char* code = nullptr;
    std::string text;
    bool more = false;
    constexpr size_t kTextCap = 860;   // leaves room for the closing "and more" inside VerdictOut::text and the log line
    auto add = [&](const char* c, const std::string& t) {
        if (!code) code = c;
        if (text.size() + 2 + t.size() > kTextCap) {
            more = true;
            return;
        }
        if (!text.empty()) text += "; ";
        text += t;
    };
    char b[200];
    if (in.role == Role::Intermediate) {
        add("INTERMEDIATE", "intermediate pass, not a DLSS input: this target is neither the texture the game submits nor the scene depth DLSS reads, so "
                            "no DLSS rule is applied to the draw");
    } else if (in.status == Status::Unmeasured) {
        add("UNMEASURED", "coverage not measured (a UAV or stream-output target, an unanswered query or none to make): cannot say what it does here");
    } else if (in.status == Status::Buried) {
        std::snprintf(b, sizeof(b), "hidden at the centre pixel by the opaque draw #%d drawn after it: not what DLSS sees there", in.buriedBy);
        add("OCCLUDED", b);
    } else {
        const bool translucent = clsTranslucent(in.cls);
        if (in.uiTaken) {
            add("UI_LAYER", "taken by the UI layer: drawn after DLSS, outside its history");
        } else if (in.masked) {
            std::snprintf(b, sizeof(b), "marks the coverage mask (%s): EDVR's post-DLSS UI resolve covers it", in.maskSlot ? in.maskSlot : "");
            add("MASKED", b);
        } else if (in.holo) {
            add("HOLO_PASS", "taken by the generic hologram pass");
        }
        const bool handled = in.uiTaken || in.masked || in.holo;
        if (translucent && !in.depthWrite && !handled) {
            std::snprintf(b, sizeof(b),
                          "%s blend, writes no depth and has no reactive or coverage mask: DLSS has no hint that it is not the surface behind it "
                          "and keeps its history through it (the reactive mask is ignored by preset K): candidate for the coverage mask or the UI layer",
                          clsName(in.cls));
            add("TRANSLUCENT_NO_MASK", b);
        } else if (translucent && in.depthWrite && !handled) {
            std::snprintf(b, sizeof(b), "%s blend that writes depth: its depth and motion are its own, but no mask tells DLSS it is translucent",
                          clsName(in.cls));
            add("TRANSLUCENT_DEPTH", b);
        }
        if (in.aboveOwner && in.depthOwnerIdx >= 0) {
            if (in.havePixel && in.px.valid) {
                std::snprintf(b, sizeof(b),
                              "inherits depth and motion of #%d (borrowed motion: path %s, engine kind %s) while it may move on its own",
                              in.depthOwnerIdx, pathName(in.px.flags.path), blend::engineKindName(in.px.flags.engineKind));
            } else {
                std::snprintf(b, sizeof(b), "inherits depth and motion of #%d (borrowed motion) while it may move on its own", in.depthOwnerIdx);
            }
            add("BORROWS_MOTION", b);
        } else if (in.aboveOwner) {
            add("BORROWS_MOTION", "drawn over the target's earlier content with no draw of its own owning depth here: depth and motion are the background's");
        }
        if (in.havePixel && in.px.valid && (in.isDepthOwner || in.aboveOwner)) {
            float ex = 0.0f, ey = 0.0f;
            if (expectedForPath(in.px, &ex, &ey)) {
                const float delta = dist2(in.px.mvx, in.px.mvy, ex, ey);
                const bool camera = blend::isCameraPath(in.px.flags.path);
                if (camera && delta > kMotionTolerancePx) {
                    std::snprintf(b, sizeof(b), "motion vector mismatch: the pass wrote (%.2f,%.2f) px on the camera path, the camera term predicts (%.2f,%.2f), %.2f px apart",
                                  static_cast<double>(in.px.mvx), static_cast<double>(in.px.mvy), static_cast<double>(ex), static_cast<double>(ey),
                                  static_cast<double>(delta));
                    add("MV_MISMATCH", b);
                } else if (!camera && in.px.flags.path == 11 && delta > kMotionTolerancePx) {
                    std::snprintf(b, sizeof(b), "engine-record motion is %.2f px from the camera term (the object's own motion relative to the world; check it is right)",
                                  static_cast<double>(delta));
                    add("ENGINE_MOTION", b);
                }
            }
        }
        if (in.cls == Cls::Opaque && in.depthWrite && in.isDepthOwner && in.status == Status::Top && in.havePixel && in.px.valid &&
            blend::isCameraPath(in.px.flags.path) && in.px.flags.engineKind == 0 && !in.keyed && !in.engineSub) {
            std::snprintf(b, sizeof(b),
                          "opaque surface at the centre on the camera term alone (path %s): no per-object motion source; if it moves relative to the world its history smears",
                          pathName(in.px.flags.path));
            add("CAMERA_ONLY", b);
        }
        if (in.routeNone && !handled) add("NO_ROUTE", "no EDVR route: no verdict, no UI layer decision, no hologram or mask family, no engine-motion key");
        if (in.ctr > 0 && (in.box < kThinSamples || topologyThin(in.topology))) {
            std::snprintf(b, sizeof(b), "thin: %lld of %u box samples%s: at a 2x upscale a feature this fine is under one input pixel and DLSS leans on its history",
                          static_cast<long long>(in.box), in.boxArea, topologyThin(in.topology) ? " (line or point primitive)" : "");
            add("THIN", b);
        }
        if (in.havePixel && in.px.valid && in.px.flags.hidden) add("HISTORY_HIDDEN", "NVIDIA's history is hidden (restarted) at the centre pixel");
        if (in.status == Status::Partial) {
            add("PARTIAL", "a later opaque draw writes elsewhere in the box; whether this one survives at the centre is unproven");
        }
        if (!code) add("OK", "nothing wrong found from the state and the pixel read; if the element still smears the cause is not in these inputs");
    }
    if (more) text += "; and further findings that did not fit (the rules fire independently)";
    std::snprintf(out->code, sizeof(out->code), "%s", code ? code : "OK");
    std::snprintf(out->text, sizeof(out->text), "%s", text.c_str());
}

// ---- text ---------------------------------------------------------------------------------------------------------------------------------

// The pieces of a draw line that are strings, kept short enough that the worst case fits kLineMax (the rig pins that).
struct DrawRow {
    uint32_t idx = 0;           // the capture's own number for the draw (issue order among the tested)
    uint32_t rt = 0;
    int eye = -1;
    uint32_t order = 0;         // the game's eye-draw ordinal (both eyes counted)
    uint32_t gameFrame = 0;
    uint64_t vs = 0, ps = 0;
    char kind = '?';
    uint32_t count = 0, instances = 1, topology = 0;
    char cls[12] = {};
    char status[12] = {};
    int64_t box = kUnknown, ctr = kUnknown;
    char blend[96] = {};
    char depth[48] = {};
    char stencil[96] = {};
    char cull[8] = {};
    uint32_t rtCount = 1;
    char rt0[28] = {};
    uint32_t rt0W = 0, rt0H = 0;
    char ds[28] = {};
    uint32_t dsW = 0, dsH = 0;
    char srv[160] = {};
    // routes
    char verdict[16] = {};
    char uiFamily[24] = {};
    char uiDecision[24] = {};
    char holo[12] = {};
    char mask[40] = {};
    char keyed[28] = {};
    char engine[12] = {};
    char motion[24] = {};       // engine-record | self-marking | camera-row
    char role[16] = {};         // roleName(): the draw's render target as DLSS relates to it
};
inline void countText(char* out, size_t cap, int64_t v) noexcept {
    if (v < 0) std::snprintf(out, cap, "?");
    else std::snprintf(out, cap, "%lld", static_cast<long long>(v));
}

inline int formatDrawLine(char* out, size_t cap, const DrawRow& r) noexcept {
    char box[24], ctr[24];
    countText(box, sizeof(box), r.box);
    countText(ctr, sizeof(ctr), r.ctr);
    const int n = std::snprintf(out, cap,
        "scene inspect draw: #%u rt=#%u eye=%d order=%u gf=%u vs=%016llX ps=%016llX %c n=%u inst=%u topo=%s cls=%s status=%s box=%s ctr=%s "
        "blend=[%s] depth=[%s] stencil=[%s] cull=%s rts=%u rt0=%s %ux%u ds=%s %ux%u role=%s srv=[%s]",
        r.idx, r.rt, r.eye, r.order, r.gameFrame, static_cast<unsigned long long>(r.vs), static_cast<unsigned long long>(r.ps), r.kind,
        r.count, r.instances, topologyName(r.topology), r.cls, r.status, box, ctr, r.blend, r.depth, r.stencil, r.cull, r.rtCount, r.rt0,
        r.rt0W, r.rt0H, r.ds, r.dsW, r.dsH, r.role, r.srv);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    return n;
}
inline int formatRouteLine(char* out, size_t cap, const DrawRow& r) noexcept {
    const int n = std::snprintf(out, cap, "scene inspect route: #%u verdict=%s ui=%s/%s holo=%s mask=%s kf=%s eng=%s motion=%s", r.idx, r.verdict,
                                r.uiFamily, r.uiDecision, r.holo, r.mask, r.keyed, r.engine, r.motion);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    return n;
}
inline int formatVerdictLine(char* out, size_t cap, uint32_t idx, const VerdictOut& v) noexcept {
    const int n = std::snprintf(out, cap, "scene inspect verdict: #%u %s: %s", idx, v.code, v.text);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    return n;
}
inline int formatOccludedLine(char* out, size_t cap, const DrawRow& r, int32_t by) noexcept {
    char box[24], ctr[24];
    countText(box, sizeof(box), r.box);
    countText(ctr, sizeof(ctr), r.ctr);
    const int n = std::snprintf(out, cap, "scene inspect occluded: #%u rt=#%u vs=%016llX ps=%016llX cls=%s box=%s ctr=%s under #%d", r.idx, r.rt,
                                static_cast<unsigned long long>(r.vs), static_cast<unsigned long long>(r.ps), r.cls, box, ctr, by);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    return n;
}

// ---- refusals and the lines around a capture ---------------------------------------------------------------------------------------------

enum class Refusal : uint8_t {
    kNone = 0,
    kNotVr,             // the flat profile has no eye passes
    kNoHooks,           // the draw hooks are not live
    kBusy,              // a capture is already running
    kNoEyePass,         // no eye draw reached the inspector in the captured frame
    kQueriesUnavailable,
    kQueriesUnanswered,
    kNoInputs,          // the temporal pass offered no DLSS inputs
    kInputsLate,        // the inputs arrived after the draw frame
    kCount
};
inline const char* refusalText(Refusal r) noexcept {
    switch (r) {
        case Refusal::kNotVr:
            return "not VR: the inspector reads the VR eye passes and the flat profile has none (flat support would need the flat runtime's own capture; not built)";
        case Refusal::kNoHooks: return "the draw hooks are not live (the graphics half has not installed its fixes), so no draw can be offered";
        case Refusal::kBusy: return "a capture is already running; press again once it has reported";
        case Refusal::kNoEyePass:
            return "no eye pass seen: not one draw into an eye-sized target reached the inspector in the captured frame (a menu, a loading screen, "
                   "no VR session, or the draw path was off). This is NOT an empty box";
        case Refusal::kQueriesUnavailable:
            return "occlusion queries unavailable: the device refused to make one, so no coverage was measured and every draw shows box=?";
        case Refusal::kQueriesUnanswered: return "some coverage queries had not answered when the report was written; those draws show box=?";
        case Refusal::kNoInputs:
            return "no DLSS inputs: the temporal pass staged none for the captured frame (fix.temporal_aa is off, the upscaler is not NVIDIA's, or the pass never reached eye 0)";
        case Refusal::kInputsLate: return "the temporal pass's inputs came from a later frame than the draws";
        default: return "";
    }
}
inline int formatInstallLine(char* out, size_t cap, bool flat, bool bound, const char* key, const char* iniName) noexcept {
    if (flat) return std::snprintf(out, cap, "scene inspect: off; VR only, hotkey.scene_inspect does nothing in the flat profile");
    if (bound) return std::snprintf(out, cap, "scene inspect: off; hotkey.scene_inspect is bound to %s", key ? key : "");
    return std::snprintf(out, cap,
                         "scene inspect: off; hotkey.scene_inspect is NOT bound -- add \"scene_inspect = <a free key>\" under [hotkey] in %s to use it",
                         iniName ? iniName : "the settings file");
}
inline int formatArmedLine(char* out, size_t cap, uint32_t cap_, const char* trigger) noexcept {
    return std::snprintf(out, cap, "scene inspect: armed cap=%u (trigger %s): the next frame boundary starts a one-frame capture of the left eye.", cap_,
                         trigger ? trigger : "");
}
inline int formatCollectingLine(char* out, size_t cap, uint32_t cap_, uint32_t gameFrame) noexcept {
    return std::snprintf(out, cap, "scene inspect: collecting cap=%u from game frame %u.", cap_, gameFrame);
}

struct BoxInfo {
    bool fromGeometry = false;     // the head-forward point came from the eye's frustum and rotation
    bool gaze = false;             // a gaze source supplied the point
    bool inside = false;
    float u = 0.5f, v = 0.5f, cantDeg = 0.0f, centreOffsetPx = 0.0f;   // how far the point is from the viewport centre, in target pixels
    uint32_t side = kBox;
    float vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    uint32_t fixedAtOrdinal = 0;
};
inline int formatBoxLine(char* out, size_t cap, uint32_t capId, const BoxInfo& b) noexcept {
    return std::snprintf(out, cap,
        "scene inspect box: cap=%u centre=%s u=%.4f v=%.4f (%.1f px from the viewport centre) canting=%.2f deg inside_image=%d side=%upx "
        "viewport=%.0f,%.0f %.0fx%.0f fixed_at_draw=%u%s",
        capId, b.gaze ? "gaze" : (b.fromGeometry ? "head-forward (no gaze source in this build)" : "viewport-centre (the eye's frustum was not available)"),
        static_cast<double>(b.u), static_cast<double>(b.v), static_cast<double>(b.centreOffsetPx), static_cast<double>(b.cantDeg), b.inside ? 1 : 0,
        b.side, static_cast<double>(b.vpX), static_cast<double>(b.vpY), static_cast<double>(b.vpW), static_cast<double>(b.vpH), b.fixedAtOrdinal,
        b.inside || !b.fromGeometry ? "" : " -- the head-forward point is OUTSIDE this eye's image; the box was clamped to its edge");
}

struct FrameCounts {
    uint32_t gameFrame = 0;
    uint32_t eyeDraws = 0, eye0 = 0, eye1 = 0, eyeUnknown = 0;
    uint32_t tested = 0, untestedState = 0, noReplay = 0, overCap = 0, indirect = 0, unanswered = 0;
    uint32_t withheld = 0;   // draws EDVR's verdict skips: never drawn, so never on screen and never duplicated
    uint32_t involved = 0, top = 0, partial = 0, buried = 0, unmeasured = 0;
    uint32_t targets = 0, listed = 0, truncated = 0;
    uint32_t inDlssTargets = 0;   // of the involved draws, those in a DLSS input target (all of them when the roles are unknown)
};
inline int formatFrameLine(char* out, size_t cap, uint32_t capId, const FrameCounts& c) noexcept {
    return std::snprintf(out, cap,
        "scene inspect frame: cap=%u game frame %u: %u eye draws seen (eye0 %u, eye1 %u, eye unknown %u); %u tested; not tested: %u over the %u cap, "
        "%u on a UAV or stream-output target, %u with no replay, %u indirect, %u withheld by EDVR (verdict skip: never drawn); %u answers missing; "
        "%u draws wrote in the box (top %u, partial %u, buried %u, unmeasured %u; %u of them in DLSS input targets) across %u render targets; %u listed, %u beyond the listing cap",
        capId, c.gameFrame, c.eyeDraws, c.eye0, c.eye1, c.eyeUnknown, c.tested, c.overCap, kMaxDraws, c.untestedState, c.noReplay, c.indirect,
        c.withheld, c.unanswered, c.involved, c.top, c.partial, c.buried, c.unmeasured, c.inDlssTargets, c.targets, c.listed, c.truncated);
}
inline int formatEmptyBoxLine(char* out, size_t cap, uint32_t capId, uint32_t tested) noexcept {
    return std::snprintf(out, cap,
        "scene inspect empty: cap=%u nothing wrote a sample inside the box (%u eye draws were tested and none reached it). The capture RAN; this is the answer, not a failure.",
        capId, tested);
}
inline int formatTargetLine(char* out, size_t cap, uint32_t rt, const char* fmt, uint32_t w, uint32_t h, const char* ds, float vpX, float vpY, float vpW,
                            float vpH, const Rect& box, int32_t cx, int32_t cy, uint32_t draws, uint32_t covering, Role role = Role::Unknown) noexcept {
    return std::snprintf(out, cap,
        "scene inspect target: rt=#%u %s %ux%u ds=%s viewport=%.0f,%.0f %.0fx%.0f box=[%d,%d)x[%d,%d) centre_px=%d,%d draws=%u wrote_in_box=%u role=%s", rt, fmt, w, h,
        ds, static_cast<double>(vpX), static_cast<double>(vpY), static_cast<double>(vpW), static_cast<double>(vpH), box.x0, box.x1, box.y0, box.y1, cx, cy,
        draws, covering, roleText(role));
}

// The box in the DLSS input.
struct InputBox {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, cx = 0, cy = 0;
    uint32_t w = 0, h = 0;
};

struct SampleStats {
    uint32_t n = 0;
    float depthMin = 0, depthMax = 0, mvxMin = 0, mvxMax = 0, mvyMin = 0, mvyMax = 0, deltaMax = 0;
    uint32_t path[16] = {};
    uint32_t kind[8] = {};
    uint32_t hidden = 0, ui = 0, coverageSet = 0, biasSet = 0;
    int coverageMax = -1, biasMax = -1;
};

struct BoxSample {
    bool valid = false;
    int x = 0, y = 0;             // DLSS input pixel
    PixelFacts px;
};

inline void accumulate(SampleStats* s, const BoxSample& b) noexcept {
    if (!s || !b.valid || !b.px.valid) return;
    const PixelFacts& p = b.px;
    float ex = 0.0f, ey = 0.0f;
    const float delta = expectedForPath(p, &ex, &ey) ? dist2(p.mvx, p.mvy, ex, ey) : 0.0f;
    if (s->n == 0) {
        s->depthMin = s->depthMax = p.depth;
        s->mvxMin = s->mvxMax = p.mvx;
        s->mvyMin = s->mvyMax = p.mvy;
    } else {
        s->depthMin = std::min(s->depthMin, p.depth);
        s->depthMax = std::max(s->depthMax, p.depth);
        s->mvxMin = std::min(s->mvxMin, p.mvx);
        s->mvxMax = std::max(s->mvxMax, p.mvx);
        s->mvyMin = std::min(s->mvyMin, p.mvy);
        s->mvyMax = std::max(s->mvyMax, p.mvy);
    }
    s->deltaMax = std::max(s->deltaMax, delta);
    ++s->n;
    ++s->path[p.flags.path & 15u];
    ++s->kind[p.flags.engineKind & 7u];
    if (p.flags.hidden) ++s->hidden;
    if (p.flags.ui) ++s->ui;
    if (p.coverage > 0) ++s->coverageSet;
    if (p.bias > 0) ++s->biasSet;
    s->coverageMax = std::max(s->coverageMax, p.coverage);
    s->biasMax = std::max(s->biasMax, p.bias);
}

inline void depthText(char* out, size_t cap, const PixelFacts& p) noexcept {
    if (!std::isfinite(p.depth)) std::snprintf(out, cap, "not staged");
    else if (p.far) std::snprintf(out, cap, "%.6f (far plane)", static_cast<double>(p.depth));
    else std::snprintf(out, cap, "%.6f (%.3f m)", static_cast<double>(p.depth), static_cast<double>(p.metres));
}

// "scene inspect pixel centre: ..." : every DLSS input at the centre pixel.
inline int formatPixelLine(char* out, size_t cap, const char* which, int x, int y, const PixelFacts& p) noexcept {
    if (!p.valid) return std::snprintf(out, cap, "scene inspect pixel %s: input px %d,%d: not read (no decision texture)", which, x, y);
    char dep[64], dl[48], ew[48], eh[48], cov[12], bia[12];
    depthText(dep, sizeof(dep), p);
    if (p.haveDlss) std::snprintf(dl, sizeof(dl), "(%.3f,%.3f)", static_cast<double>(p.dlx), static_cast<double>(p.dly));
    else std::snprintf(dl, sizeof(dl), "not staged");
    if (p.haveWorld) std::snprintf(ew, sizeof(ew), "(%.3f,%.3f)", static_cast<double>(p.expWx), static_cast<double>(p.expWy));
    else std::snprintf(ew, sizeof(ew), "n/a");
    if (p.haveHead) std::snprintf(eh, sizeof(eh), "(%.3f,%.3f)", static_cast<double>(p.expHx), static_cast<double>(p.expHy));
    else std::snprintf(eh, sizeof(eh), "n/a");
    countText(cov, sizeof(cov), p.coverage);
    countText(bia, sizeof(bia), p.bias);
    float ex = 0.0f, ey = 0.0f;
    char mism[64];
    if (expectedForPath(p, &ex, &ey))
        std::snprintf(mism, sizeof(mism), "%.3f px vs the %s camera term", static_cast<double>(dist2(p.mvx, p.mvy, ex, ey)),
                      p.flags.path == 1 ? "ship" : (p.flags.path == 2 ? "world" : "nearer"));
    else std::snprintf(mism, sizeof(mism), "n/a");
    return std::snprintf(out, cap,
        "scene inspect pixel %s: input px %d,%d depth=%s mv_pass=(%.3f,%.3f) mv_dlss=%s expect_world=%s expect_ship=%s mismatch=%s path=%u(%s)%s%s "
        "engine_kind=%u(%s) coverage_mask=%s bias_mask=%s",
        which, x, y, dep, static_cast<double>(p.mvx), static_cast<double>(p.mvy), dl, ew, eh, mism, p.flags.path, pathName(p.flags.path),
        p.flags.hidden ? " HIDDEN" : "", p.flags.ui ? " UI" : "", p.flags.engineKind, blend::engineKindName(p.flags.engineKind), cov, bia);
}
inline int formatBoxStatsLine(char* out, size_t cap, const SampleStats& s) noexcept {
    return std::snprintf(out, cap,
        "scene inspect box stats: %u pixels read; depth min=%.6f max=%.6f; mv x %.3f..%.3f y %.3f..%.3f; max |mv - camera term| = %.3f px; paths "
        "world=%u ship=%u hologram=%u screen=%u engine=%u planet=%u other=%u; engine kinds none=%u exact=%u masked=%u stale-slot=%u bad-code=%u "
        "stale-pose=%u; hidden=%u ui=%u; coverage mask set=%u max=%d; bias mask set=%u max=%d",
        s.n, static_cast<double>(s.depthMin), static_cast<double>(s.depthMax), static_cast<double>(s.mvxMin), static_cast<double>(s.mvxMax),
        static_cast<double>(s.mvyMin), static_cast<double>(s.mvyMax), static_cast<double>(s.deltaMax), s.path[2], s.path[1], s.path[8], s.path[10],
        s.path[11], s.path[12], s.n - (s.path[1] + s.path[2] + s.path[8] + s.path[10] + s.path[11] + s.path[12]), s.kind[0], s.kind[1], s.kind[2],
        s.kind[4], s.kind[5], s.kind[6], s.hidden, s.ui, s.coverageSet, s.coverageMax, s.biasSet, s.biasMax);
}

struct DlssFacts {
    bool have = false;
    unsigned preset = 0;
    uint32_t w = 0, h = 0, outW = 0, outH = 0;
    float jx = 0.0f, jy = 0.0f;
    bool reset = false;
    const char* resetWhy = "";
    bool reactiveHanded = false;   // a bias-current-colour mask was handed to NVIDIA
    float preExposure = 1.0f, exposureScale = 1.0f;
    bool exposureTexture = false;
    bool hdr = false;
    bool transparencyMask = false;   // EDVR never passes one
    float frameMs = 0.0f;
    bool uiResolved = false;
    const char* sync = "";           // same-frame | late+N
    uint32_t sceneFrame = 0;
};
inline const char* presetName(unsigned p) noexcept {
    switch (p) {
        case 0: return "driver-default";
        case 6: return "F";
        case 10: return "J";
        case 11: return "K";
        case 12: return "L";
        case 13: return "M";
        default: return "?";
    }
}
inline int formatDlssLine(char* out, size_t cap, const DlssFacts& d) noexcept {
    if (!d.have) return std::snprintf(out, cap, "scene inspect dlss: not captured (the pass did not evaluate an upscaler for eye 0 in the captured frame)");
    return std::snprintf(out, cap,
        "scene inspect dlss: preset=%s(%u) input %ux%u -> output %ux%u jitter=(%.4f,%.4f) px history_reset=%d%s%s exposure=pre %.3f scale %.3f texture=%s "
        "hdr=%d; reactive(bias_current_colour) mask handed=%s%s; transparency/T&C mask=%s; frame_delta=%.2f ms; UI resolve ran=%d; inputs %s (temporal pass frame %u)",
        presetName(d.preset), d.preset, d.w, d.h, d.outW, d.outH, static_cast<double>(d.jx), static_cast<double>(d.jy), d.reset ? 1 : 0, d.reset ? " why=" : "",
        d.reset ? d.resetWhy : "", static_cast<double>(d.preExposure), static_cast<double>(d.exposureScale), d.exposureTexture ? "bound" : "none", d.hdr ? 1 : 0,
        d.reactiveHanded ? "yes" : "no", d.reactiveHanded && d.preset != 6 ? " (ignored: only preset F reads it)" : "",
        d.transparencyMask ? "supplied" : "none supplied (EDVR never passes one)", static_cast<double>(d.frameMs), d.uiResolved ? 1 : 0, d.sync, d.sceneFrame);
}

struct FilesInfo {
    char csv[160] = {};
    char boxCsv[160] = {};
    char pngs[600] = {};
};
inline int formatFilesLine(char* out, size_t cap, uint32_t capId, const FilesInfo& f) noexcept {
    return std::snprintf(out, cap, "scene inspect files: cap=%u draws_csv=%s box_csv=%s images=%s", capId, f.csv[0] ? f.csv : "(not written)",
                         f.boxCsv[0] ? f.boxCsv : "(not written)", f.pngs[0] ? f.pngs : "(none)");
}
inline int formatCompleteLine(char* out, size_t cap, uint32_t capId, uint32_t involved, uint32_t occluded, bool inputs, bool outputs) noexcept {
    return std::snprintf(out, cap, "scene inspect: complete cap=%u: %u draws wrote in the box (%u occluded), DLSS inputs %s, DLSS outputs %s.", capId, involved,
                         occluded, inputs ? "read" : "MISSING", outputs ? "read" : "MISSING");
}
inline const char* legendText() noexcept {
    return "scene inspect legend: draw = identity and state of a draw that wrote in the box (box = samples in the BOX, ctr = samples at the centre "
           "pixel, both counted by a duplicate of the draw under an occlusion query, measured BEFORE the game's own draw so they are what it adds "
           "over the earlier draws); status top = visible at the centre pixel (or, if it missed the centre, nothing opaque after it reached the box), "
           "buried = a later opaque draw wrote its centre pixel, partial = it missed the centre and a later opaque draw wrote elsewhere in the box; "
           "route = what EDVR decided for it; verdict = the rules that fired; stack = the draws visible at the centre pixel, bottom to top; "
           "pixel and box stats = the DLSS inputs read from the temporal pass";
}

// ---- CSV -----------------------------------------------------------------------------------------------------------------------------------

// RFC 4180: a field with a comma, quote, CR or LF is quoted, and its quotes doubled.
inline void csvField(std::string* line, const char* s) {
    if (!line) return;
    const char* p = s ? s : "";
    bool quote = false;
    for (const char* c = p; *c; ++c)
        if (*c == ',' || *c == '"' || *c == '\n' || *c == '\r') quote = true;
    if (!line->empty() && line->back() != '\n') *line += ',';
    if (!quote) {
        *line += p;
        return;
    }
    *line += '"';
    for (const char* c = p; *c; ++c) {
        if (*c == '"') *line += '"';
        *line += *c;
    }
    *line += '"';
}
inline void csvNumber(std::string* line, long long v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%lld", v);
    csvField(line, b);
}
inline void csvHex(std::string* line, uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llX", static_cast<unsigned long long>(v));
    csvField(line, b);
}

inline const char* drawsCsvHeader() noexcept {
    return "cap,idx,rt,eye,order,game_frame,vs,ps,kind,count,instances,topology,class,status,box,centre,blend,depth,stencil,cull,rt0_format,rt0_w,rt0_h,"
           "ds_format,ds_w,ds_h,target_role,srv,edvr_verdict,ui_family,ui_decision,holo,mask,keyed,engine,motion_source,occluded_by,verdict_code,verdict_text\n";
}
inline std::string drawsCsvRow(uint32_t capId, const DrawRow& r, int32_t by, const VerdictOut& v) {
    std::string l;
    csvNumber(&l, capId);
    csvNumber(&l, r.idx);
    csvNumber(&l, r.rt);
    csvNumber(&l, r.eye);
    csvNumber(&l, r.order);
    csvNumber(&l, r.gameFrame);
    csvHex(&l, r.vs);
    csvHex(&l, r.ps);
    char k[2] = {r.kind, 0};
    csvField(&l, k);
    csvNumber(&l, r.count);
    csvNumber(&l, r.instances);
    csvField(&l, topologyName(r.topology));
    csvField(&l, r.cls);
    csvField(&l, r.status);
    csvNumber(&l, r.box);
    csvNumber(&l, r.ctr);
    csvField(&l, r.blend);
    csvField(&l, r.depth);
    csvField(&l, r.stencil);
    csvField(&l, r.cull);
    csvField(&l, r.rt0);
    csvNumber(&l, r.rt0W);
    csvNumber(&l, r.rt0H);
    csvField(&l, r.ds);
    csvNumber(&l, r.dsW);
    csvNumber(&l, r.dsH);
    csvField(&l, r.role);
    csvField(&l, r.srv);
    csvField(&l, r.verdict);
    csvField(&l, r.uiFamily);
    csvField(&l, r.uiDecision);
    csvField(&l, r.holo);
    csvField(&l, r.mask);
    csvField(&l, r.keyed);
    csvField(&l, r.engine);
    csvField(&l, r.motion);
    csvNumber(&l, by);
    csvField(&l, v.code);
    csvField(&l, v.text);
    l += '\n';
    return l;
}
inline const char* boxCsvHeader() noexcept {
    return "cap,x,y,depth,metres,mv_pass_x,mv_pass_y,mv_dlss_x,mv_dlss_y,expect_world_x,expect_world_y,expect_ship_x,expect_ship_y,path,hidden,ui,"
           "engine_kind,coverage_mask,bias_mask\n";
}
inline void csvFloat(std::string* line, float v) {
    char b[40];
    if (!std::isfinite(v)) b[0] = 0;
    else std::snprintf(b, sizeof(b), "%.6g", static_cast<double>(v));
    csvField(line, b);
}
inline std::string boxCsvRow(uint32_t capId, const BoxSample& s) {
    std::string l;
    csvNumber(&l, capId);
    csvNumber(&l, s.x);
    csvNumber(&l, s.y);
    const PixelFacts& p = s.px;
    csvFloat(&l, p.depth);
    csvFloat(&l, p.far ? NAN : p.metres);
    csvFloat(&l, p.mvx);
    csvFloat(&l, p.mvy);
    csvFloat(&l, p.haveDlss ? p.dlx : NAN);
    csvFloat(&l, p.haveDlss ? p.dly : NAN);
    csvFloat(&l, p.haveWorld ? p.expWx : NAN);
    csvFloat(&l, p.haveWorld ? p.expWy : NAN);
    csvFloat(&l, p.haveHead ? p.expHx : NAN);
    csvFloat(&l, p.haveHead ? p.expHy : NAN);
    csvNumber(&l, p.flags.path);
    csvNumber(&l, p.flags.hidden ? 1 : 0);
    csvNumber(&l, p.flags.ui ? 1 : 0);
    csvNumber(&l, p.flags.engineKind);
    csvNumber(&l, p.coverage);
    csvNumber(&l, p.bias);
    l += '\n';
    return l;
}

// ---- images --------------------------------------------------------------------------------------------------------------------------------

inline uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) noexcept {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        built = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
inline uint32_t adler32(const uint8_t* p, size_t n) noexcept {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}
inline void putBe32(std::vector<uint8_t>* v, uint32_t x) {
    v->push_back(static_cast<uint8_t>(x >> 24));
    v->push_back(static_cast<uint8_t>(x >> 16));
    v->push_back(static_cast<uint8_t>(x >> 8));
    v->push_back(static_cast<uint8_t>(x));
}
inline void pngChunk(std::vector<uint8_t>* out, const char type[4], const uint8_t* data, size_t n) {
    putBe32(out, static_cast<uint32_t>(n));
    const size_t at = out->size();
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(type[i]));
    if (n) out->insert(out->end(), data, data + n);
    putBe32(out, crc32Update(0, out->data() + at, 4 + n));
}
// An 8-bit RGB PNG (colour type 2) with the pixel data in stored (uncompressed) zlib blocks: no compressor needed, any reader opens it.
// rgb is w*h*3 bytes, row-major. False for an empty or oversized image.
inline bool encodePngRgb(const uint8_t* rgb, uint32_t w, uint32_t h, std::vector<uint8_t>* out) {
    if (!rgb || !out || w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    out->clear();
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out->insert(out->end(), sig, sig + 8);
    uint8_t ihdr[13];
    ihdr[0] = static_cast<uint8_t>(w >> 24); ihdr[1] = static_cast<uint8_t>(w >> 16); ihdr[2] = static_cast<uint8_t>(w >> 8); ihdr[3] = static_cast<uint8_t>(w);
    ihdr[4] = static_cast<uint8_t>(h >> 24); ihdr[5] = static_cast<uint8_t>(h >> 16); ihdr[6] = static_cast<uint8_t>(h >> 8); ihdr[7] = static_cast<uint8_t>(h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    pngChunk(out, "IHDR", ihdr, 13);
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * 3 + 1));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), rgb + static_cast<size_t>(y) * w * 3, rgb + static_cast<size_t>(y + 1) * w * 3);
    }
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    putBe32(&z, adler32(raw.data(), raw.size()));
    pngChunk(out, "IDAT", z.data(), z.size());
    pngChunk(out, "IEND", nullptr, 0);
    return true;
}

// A 1-pixel magenta ring drawn just OUTSIDE the half-open box [x0,x1) x [y0,y1) of an RGB image, so every pixel of the box itself stays as read. Parts
// of the ring that fall off the image are left out. The crops carry it so the box is found on them without arithmetic.
inline void drawBoxOutline(uint8_t* rgb, uint32_t w, uint32_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1) noexcept {
    if (!rgb || w == 0 || h == 0) return;
    auto put = [&](int32_t x, int32_t y) {
        if (x < 0 || y < 0 || x >= static_cast<int32_t>(w) || y >= static_cast<int32_t>(h)) return;
        uint8_t* p = rgb + (static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 3;
        p[0] = 255;
        p[1] = 0;
        p[2] = 255;
    };
    for (int32_t x = x0 - 1; x <= x1; ++x) {
        put(x, y0 - 1);
        put(x, y1);
    }
    for (int32_t y = y0; y < y1; ++y) {
        put(x0 - 1, y);
        put(x1, y);
    }
}

inline float halfToFloat(uint16_t h) noexcept {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
    return s ? -v : v;
}

// One texel of a colour crop as 8-bit RGB. false for a format the inspector does not convert.
inline bool texelToRgb(uint32_t dxgi, const uint8_t* p, uint8_t rgb[3]) noexcept {
    auto q = [](float f) { return static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, f)) * 255.0f)); };
    switch (dxgi) {
        case 27: case 28: case 29:   // R8G8B8A8
            rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
            return true;
        case 87: case 88: case 90: case 91:   // B8G8R8A8 / X8
            rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
            return true;
        case 23: case 24: {   // R10G10B10A2
            uint32_t v;
            std::memcpy(&v, p, 4);
            rgb[0] = static_cast<uint8_t>(((v & 1023u) * 255u + 511u) / 1023u);
            rgb[1] = static_cast<uint8_t>((((v >> 10) & 1023u) * 255u + 511u) / 1023u);
            rgb[2] = static_cast<uint8_t>((((v >> 20) & 1023u) * 255u + 511u) / 1023u);
            return true;
        }
        case 10: {   // R16G16B16A16_FLOAT
            uint16_t h[3];
            std::memcpy(h, p, 6);
            rgb[0] = q(halfToFloat(h[0])); rgb[1] = q(halfToFloat(h[1])); rgb[2] = q(halfToFloat(h[2]));
            return true;
        }
        default: return false;
    }
}
inline uint32_t texelBytes(uint32_t dxgi) noexcept {
    switch (dxgi) {
        case 10: return 8;
        case 23: case 24: case 27: case 28: case 29: case 87: case 88: case 90: case 91: return 4;
        default: return 0;
    }
}

// Motion as an image: red = 128 + scale * x, green = 128 + scale * y (render pixels), blue 0; 128,128 is "no motion". The scale is chosen
// per crop so the largest component fills the range (at least 1 px full-scale), and is logged beside the file.
inline float motionScale(float maxAbs) noexcept {
    const float m = maxAbs < 1.0f ? 1.0f : maxAbs;
    return 127.0f / m;
}
inline void motionToRgb(float mvx, float mvy, float scale, uint8_t rgb[3]) noexcept {
    auto q = [](float f) { return static_cast<uint8_t>(std::lround(std::min(255.0f, std::max(0.0f, f)))); };
    rgb[0] = q(128.0f + scale * mvx);
    rgb[1] = q(128.0f + scale * mvy);
    rgb[2] = 0;
}
// Depth as grey, stretched between the crop's own nearest and farthest raw values (reversed Z: nearer is larger, shown brighter).
inline uint8_t depthToGrey(float raw, float lo, float hi) noexcept {
    if (!std::isfinite(raw) || !(hi > lo)) return 0;
    const float t = (raw - lo) / (hi - lo);
    return static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, t)) * 255.0f));
}
// The decision path as a flat colour, so a crop shows where one path ends and another begins.
inline void pathToRgb(const DecisionFlags& f, uint8_t rgb[3]) noexcept {
    static const uint8_t table[16][3] = {{0, 0, 0},      {60, 120, 255}, {60, 220, 120}, {0, 0, 0},     {0, 0, 0},      {0, 0, 0},     {0, 0, 0},     {0, 0, 0},
                                         {255, 200, 40}, {0, 0, 0},      {200, 80, 255}, {255, 70, 60}, {255, 140, 200}, {0, 0, 0},     {0, 0, 0},     {0, 0, 0}};
    const uint8_t* t = table[f.path & 15u];
    rgb[0] = t[0]; rgb[1] = t[1]; rgb[2] = t[2];
    if (f.hidden) { rgb[0] = 255; rgb[1] = 255; rgb[2] = 255; }
}
inline const char* motionImageLegend() noexcept {
    return "red = 128 + scale*mv_x, green = 128 + scale*mv_y (input pixels, current to previous), blue 0; 128,128 is no motion";
}
inline const char* pathImageLegend() noexcept {
    return "blue = camera on the ship (the head's delta, path 1), green = camera on the world (path 2), red = engine record, yellow = cockpit hologram, violet = screen, pink = planet patch, "
           "white = NVIDIA history hidden, black = no decision";
}

}  // namespace edvr::inspect
