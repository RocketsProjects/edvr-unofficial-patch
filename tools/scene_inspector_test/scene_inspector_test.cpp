// The scene inspector (src\d3d11\scene_inspector.cpp and scene_inspector_core.h; docs/scene-inspector-2026-10-14.md).
//
// PURE HALF (no device): the box and its mapping to eye pixels (symmetric, asymmetric, canted and gazing eyes), the owner classification
// (top, buried, partial, outside), the centre stack, the camera-term motion cross-check, every verdict rule, the length of every log line
// at its worst, the CSV format, the PNG writer (decoded again here) and the image conversions.
//
// DEVICE HALF (a WARP device, the production module linked in as a source): a pipeline is set up as the game would, the module is
// offered a run of draws, and the rig proves
//   S1  every piece of state the duplicate touches is put back: the rasterizer state, the scissor rects, the blend state with its factor and
//       sample mask, the depth-stencil state with its reference -- and nothing else moved (shaders, layout, topology, targets, viewports,
//       constant buffers)
//   S2  the duplicate writes nothing: the render target after the inspected run is byte for byte the one after an un-inspected control run
//   S3  the counts are exact on pixel-aligned quads (box 256, centre 1, a half-covered box 128 and centre 0, a depth-failed quad 0)
//   S4  the classification of that run (buried, top, outside) and the stack, in the log the module wrote
//   S5  a clear of the target removes the draws before it
//   S6  with the temporal pass's inputs offered, the report carries the pixel, the camera-term cross-check (agreeing and not), the DLSS line,
//       and the CSVs and PNGs are written and readable
//   S7  the log never says "nothing in the box" for a capture that did not run, and never hides one that did
//   S8  the D3D11 debug layer, when installed, reports no error for any of it
// --self-test runs both halves; --dry-run runs neither and writes nothing. The device half writes only to a temp directory it removes.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/scene_inspector.h"
#include "../../src/d3d11/scene_inspector_core.h"

using Microsoft::WRL::ComPtr;
using namespace edvr::inspect;

// ---- what the DLL provides around the module: the log, the directory helper -------------------------------------------------
namespace edvr {
std::vector<std::string> g_lines;
bool g_verbose = false;
Log& Log::get() {
    static Log instance;
    return instance;
}
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lines.emplace_back(buf);
    if (g_verbose) std::printf("    log: %s\n", buf);
}
bool ensureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}
}  // namespace edvr

namespace {
unsigned g_checks = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) throw std::runtime_error(label);
}
void checkf(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (ok) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    throw std::runtime_error(buf);
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// =============================================================================================================================
// PURE HALF
// =============================================================================================================================

void testBox() {
    // A symmetric frustum with no canting: straight ahead is the centre.
    Frustum sym;
    sym.left = -1.0f; sym.right = 1.0f; sym.down = -1.0f; sym.up = 1.0f;
    Centre c = headForwardToImage(sym, nullptr);
    check(c.ok && c.inside && near(c.u, 0.5f) && near(c.v, 0.5f) && near(c.cantDeg, 0.0f), "a symmetric frustum, no canting: the head's forward is the image centre");
    // Asymmetric (every real headset): the optical axis is NOT the centre. l=-1.4 r=1.2 d=-1.3 u=1.5.
    Frustum asym;
    asym.left = -1.4f; asym.right = 1.2f; asym.down = -1.3f; asym.up = 1.5f;
    c = headForwardToImage(asym, nullptr);
    check(near(c.u, 1.4f / 2.6f) && near(c.v, 1.5f / 2.8f), "an asymmetric frustum puts straight ahead off-centre, by the tangents");
    // Canted: the eye turned by 10 degrees about the vertical axis (row-major R_y(theta), eye to head). The head's forward is then
    // theta off the eye's axis: tan x = tan(theta), so u moves by tan(theta) / (r - l).
    const float th = 10.0f * 3.14159265f / 180.0f;
    const float rotY[9] = {std::cos(th), 0.0f, std::sin(th), 0.0f, 1.0f, 0.0f, -std::sin(th), 0.0f, std::cos(th)};
    c = headForwardToImage(sym, rotY);
    check(near(c.u, 0.5f + 0.5f * std::tan(th), 1e-4f) && near(c.v, 0.5f), "a canted eye: the head's forward moves by tan(cant) across the image");
    check(near(c.cantDeg, 10.0f, 0.01f), "the cant is reported in degrees");
    const float rotYn[9] = {std::cos(-th), 0.0f, std::sin(-th), 0.0f, 1.0f, 0.0f, -std::sin(-th), 0.0f, std::cos(-th)};
    c = headForwardToImage(sym, rotYn);
    check(near(c.u, 0.5f - 0.5f * std::tan(th), 1e-4f), "the other eye, canted the other way, moves the other way");
    // Canted and asymmetric together: both terms add in tangent space.
    c = headForwardToImage(asym, rotY);
    check(near(c.u, (std::tan(th) + 1.4f) / 2.6f, 1e-4f), "canting and an asymmetric frustum compose in tangent space");
    // A very canted eye pushes the point out of its image.
    const float t2 = 60.0f * 3.14159265f / 180.0f;
    const float rot60[9] = {std::cos(t2), 0.0f, std::sin(t2), 0.0f, 1.0f, 0.0f, -std::sin(t2), 0.0f, std::cos(t2)};
    c = headForwardToImage(sym, rot60);
    check(c.ok && !c.inside && c.u > 1.0f, "a 60 degree cant puts the forward point outside the image: ok but not inside");
    // Pitch: the eye tilted up by 5 degrees sees the head's forward below its axis (v > 0.5).
    const float p5 = 5.0f * 3.14159265f / 180.0f;
    const float rotX[9] = {1.0f, 0.0f, 0.0f, 0.0f, std::cos(p5), -std::sin(p5), 0.0f, std::sin(p5), std::cos(p5)};
    c = headForwardToImage(sym, rotX);
    check(c.ok && c.v > 0.5f, "an eye tilted up sees the head's forward below its axis");
    // A gaze direction goes through the same function: 0.1 tangent right of forward.
    const float gaze[3] = {0.1f, 0.0f, -1.0f};
    c = headDirectionToImage(sym, nullptr, gaze);
    check(near(c.u, 0.55f) && near(c.v, 0.5f), "a gaze direction lands by its tangent");
    const float behind[3] = {0.0f, 0.0f, 1.0f};
    check(!headDirectionToImage(sym, nullptr, behind).ok, "a direction behind the eye has no point");
    Frustum bad;
    bad.left = 1.0f; bad.right = -1.0f;
    check(!headForwardToImage(bad, nullptr).ok, "an inverted frustum has no point");
    Frustum nan;
    nan.right = std::numeric_limits<float>::quiet_NaN();
    check(!headForwardToImage(nan, nullptr).ok, "a NaN frustum has no point");

    // The box in a target.
    Rect r;
    check(boxAround(1008.0f, 1008.0f, 16, 2016, 2016, &r) && r.x0 == 1000 && r.x1 == 1016 && r.y0 == 1000 && r.y1 == 1016, "the box is 16 square about the point");
    check(boxAround(2.0f, 3.0f, 16, 2016, 2016, &r) && r.x0 == 0 && r.x1 == 16 && r.y0 == 0 && r.y1 == 16, "a point near the corner moves the box inside, not smaller");
    check(boxAround(2015.0f, 2015.0f, 16, 2016, 2016, &r) && r.x1 == 2016 && r.x0 == 2000 && r.y1 == 2016, "and at the far corner");
    check(boxAround(-50.0f, 5000.0f, 16, 2016, 2016, &r) && r.x0 == 0 && r.y1 == 2016, "a point far outside clamps to the nearest edge");
    check(boxAround(5.0f, 5.0f, 16, 10, 10, &r) && r.width() == 10 && r.height() == 10, "a target smaller than the box is the box");
    check(!boxAround(5.0f, 5.0f, 16, 0, 10, &r), "an empty target has no box");
    check(!boxAround(NAN, 5.0f, 16, 100, 100, &r), "a NaN point has no box");
    check(!boxAround(5.0f, 5.0f, 16, 100, 100, nullptr), "a null out refuses");
    Rect b;
    boxAround(1008.4f, 1008.9f, 16, 2016, 2016, &b);
    int32_t px = 0, py = 0;
    centrePixel(1008.4f, 1008.9f, b, &px, &py);
    check(px == 1008 && py == 1008, "the centre pixel is the pixel that holds the point");
    centrePixel(-5.0f, 9999.0f, b, &px, &py);
    check(px == b.x0 && py == b.y1 - 1, "and stays inside the box");
    float tx = 0, ty = 0;
    imagePointToTarget(0.25f, 0.75f, 100.0f, 50.0f, 400.0f, 200.0f, &tx, &ty);
    check(near(tx, 200.0f) && near(ty, 200.0f), "an image point maps through the viewport's origin and size");
    // The same image point in the input and in the output of a 2x upscale lands at the same place, scaled.
    Rect in, out;
    cropAround(0.5f * 2016.0f, 0.5f * 2016.0f, 128, 2016, 2016, &in);
    cropAround(0.5f * 4032.0f, 0.5f * 4032.0f, 256, 4032, 4032, &out);
    check(in.width() == 128 && out.width() == 256 && out.x0 == in.x0 * 2, "the output crop is the input crop at the upscale ratio");
}

void testClasses() {
    BlendDesc b;
    check(classify(b) == Cls::Opaque, "no blend: opaque");
    b.enable = true; b.src = 5; b.dst = 6;
    check(classify(b) == Cls::Alpha, "SRC_ALPHA, INV_SRC_ALPHA: alpha");
    b.src = 5; b.dst = 2;
    check(classify(b) == Cls::Additive, "SRC_ALPHA, ONE: additive");
    b.src = 2; b.dst = 2;
    check(classify(b) == Cls::Additive, "ONE, ONE: additive");
    b.src = 2; b.dst = 6;
    check(classify(b) == Cls::Premult, "ONE, INV_SRC_ALPHA: premultiplied");
    b.src = 9; b.dst = 1;
    check(classify(b) == Cls::Multiply, "DEST_COLOR, ZERO: multiply");
    b.src = 2; b.dst = 1;
    check(classify(b) == Cls::Opaque, "ONE, ZERO with blending on replaces: opaque");
    b.src = 4; b.dst = 4;
    check(classify(b) == Cls::OtherBlend, "anything else is blend-other");
    b.writeMask = 0;
    check(classify(b) == Cls::NoColour, "a zero write mask writes no colour");
    BlendDesc o;
    check(isOccluder(Cls::Opaque, true, o), "opaque, depth write, all of RGB: an occluder");
    check(!isOccluder(Cls::Opaque, false, o), "no depth write: not an occluder");
    check(!isOccluder(Cls::Additive, true, o), "a blend never hides what is under it");
    o.writeMask = 3;
    check(!isOccluder(Cls::Opaque, true, o), "a draw that masks a colour channel does not replace the pixel");
    o.writeMask = 0xF;
    o.alphaToCoverage = true;
    check(!isOccluder(Cls::Opaque, true, o), "alpha to coverage covers partially");
    check(std::strcmp(topologyName(4), "TRIANGLELIST") == 0 && std::strcmp(topologyName(2), "LINELIST") == 0 && std::strcmp(topologyName(34), "PATCHLIST") == 0 &&
              std::strcmp(topologyName(99), "OTHER") == 0, "topology names");
    check(topologyThin(2) && topologyThin(1) && !topologyThin(4), "lines and points are thin");
    char buf[96];
    StencilDesc sd;
    formatStencil(buf, sizeof(buf), sd);
    check(std::strcmp(buf, "off") == 0, "stencil off");
    sd.enable = true; sd.ref = 1; sd.func = 3; sd.passOp = 3;
    formatStencil(buf, sizeof(buf), sd);
    check(has(buf, "ref=1") && has(buf, "func=EQUAL") && has(buf, "pass=REPLACE"), "stencil on: reference, function and pass op");
    char fb[16];
    check(std::strcmp(formatName(28, fb, sizeof(fb)), "R8G8B8A8_UNORM") == 0 && std::strcmp(formatName(26, fb, sizeof(fb)), "R11G11B10_FLOAT") == 0, "format names");
    check(std::strcmp(formatName(9999, fb, sizeof(fb)), "fmt9999") == 0, "an unknown format is named by number");
}

DrawFact fact(uint32_t rt, bool occ, bool dw, int64_t box, int64_t ctr) {
    DrawFact f;
    f.rt = rt; f.occluder = occ; f.depthWrite = dw; f.box = box; f.ctr = ctr;
    return f;
}

void testStatus() {
    // floor, object, closer object over both, additive overlay, a draw elsewhere, a draw that failed depth.
    DrawFact d[6] = {fact(0, true, true, 256, 1), fact(0, true, true, 256, 1), fact(0, true, true, 256, 1), fact(0, false, false, 256, 1),
                     fact(0, true, true, 0, 0), fact(0, true, true, 0, 0)};
    StatusOut s[6];
    classifyStatus(d, 6, s);
    check(s[0].status == Status::Buried && s[0].by == 2, "the floor is buried by the closer object, which is named");
    check(s[1].status == Status::Buried && s[1].by == 2, "so is the object under it");
    check(s[2].status == Status::Top, "the closer object has no later occluder: top");
    check(s[3].status == Status::Top, "an additive overlay drawn last is top");
    check(s[4].status == Status::Outside && s[5].status == Status::Outside, "no samples in the box: outside");
    // The overlay drawn BEFORE the opaque draw is buried by it.
    DrawFact e[3] = {fact(0, false, false, 256, 1), fact(0, true, true, 256, 1), fact(0, true, true, 0, 0)};
    StatusOut t[3];
    classifyStatus(e, 3, t);
    check(t[0].status == Status::Buried && t[0].by == 1, "a translucent draw drawn before an opaque one over its centre is buried");
    // Partial: the later occluder reaches the box but not the first draw's centre.
    DrawFact p[2] = {fact(0, true, true, 256, 1), fact(0, true, true, 100, 0)};
    StatusOut ps[2];
    classifyStatus(p, 2, ps);
    check(ps[0].status == Status::Top && ps[0].by == -1, "a later occluder in the box but not at the centre leaves the centre pixel's owner on top");
    DrawFact q[2] = {fact(0, true, true, 100, 0), fact(0, true, true, 256, 1)};
    classifyStatus(q, 2, ps);
    check(ps[0].status == Status::Partial && ps[0].by == -1, "a draw that missed the centre pixel is never proven buried, even by a draw that covers it: partial");
    // Different targets never hide each other.
    DrawFact r[2] = {fact(0, true, true, 256, 1), fact(1, true, true, 256, 1)};
    classifyStatus(r, 2, ps);
    check(ps[0].status == Status::Top && ps[1].status == Status::Top, "an opaque draw into another render target hides nothing");
    // Unknown counts.
    DrawFact u[2] = {fact(0, true, true, kUnknown, kUnknown), fact(0, true, true, 256, 1)};
    classifyStatus(u, 2, ps);
    check(ps[0].status == Status::Unmeasured, "an unanswered count is unmeasured, not outside");
    // An unmeasured occluder cannot bury anything (it is not known to reach the box).
    DrawFact w[2] = {fact(0, true, true, 256, 1), fact(0, true, true, kUnknown, kUnknown)};
    classifyStatus(w, 2, ps);
    check(ps[0].status == Status::Top, "an unmeasured later draw does not bury an earlier one");
    classifyStatus(nullptr, 0, nullptr);   // no crash on nothing
    check(true, "an empty list classifies");

    // The stack: the last occluder at the centre and everything visible over it.
    DrawFact k[6] = {fact(0, true, true, 256, 1), fact(0, true, true, 256, 1), fact(0, true, true, 256, 1), fact(0, false, false, 256, 1),
                     fact(0, false, true, 256, 1), fact(0, false, false, 100, 0)};
    int32_t idx[8];
    int32_t owner = -9;
    const size_t n = buildStack(k, 6, 0, idx, 8, &owner);
    check(n == 3 && idx[0] == 2 && idx[1] == 3 && idx[2] == 4, "the stack is the last opaque at the centre, then what is over it that wrote the centre");
    check(owner == 4, "the depth owner is the last draw in the stack that wrote depth (a blended draw may)");
    DrawFact z[2] = {fact(0, false, false, 256, 1), fact(0, false, false, 256, 1)};
    const size_t m = buildStack(z, 2, 0, idx, 8, &owner);
    check(m == 2 && owner == -1, "with no occluder the stack is every draw that wrote the centre and nothing owns depth");
    check(buildStack(z, 2, 3, idx, 8, &owner) == 0, "another target has an empty stack");
    check(buildStack(z, 2, 0, idx, 1, &owner) == 1, "the stack respects the caller's capacity");
}

CamTerm identityCam(uint32_t w, uint32_t h) {
    CamTerm c;
    c.w = w; c.h = h;
    const float t[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
    std::memcpy(c.tanNow, t, sizeof(t));
    std::memcpy(c.tanPrev, t, sizeof(t));
    c.rows[0][0] = c.rows[1][1] = c.rows[2][2] = 1.0f;
    c.valid = true;
    return c;
}

void testMotion() {
    float m = 0.0f;
    check(depthToMetres(0.25f, 0.0f, 0.5f, &m) && near(m, 2.0f), "depth = x + z/metres inverts to 2 m");
    check(!depthToMetres(0.0f, 0.0f, 0.5f, &m), "raw 0 is the far plane");
    check(!depthToMetres(0.1f, 0.2f, 0.5f, &m), "a raw depth below the encoding's floor has no metres");
    check(depthToMetres(0.35f, 0.1f, 0.5f, &m) && near(m, 2.0f), "with a floor term");
    CamTerm c = identityCam(100, 100);
    float mx = 9, my = 9;
    check(expectedMotion(c, 50.0f, 50.0f, false, 2.0f, &mx, &my) && near(mx, 0.0f, 1e-3f) && near(my, 0.0f, 1e-3f), "no camera movement: no motion");
    c.tv[0] = 0.1f;
    check(expectedMotion(c, 50.0f, 50.0f, false, 2.0f, &mx, &my) && near(mx, 0.1f * 100.0f / (2.0f * 2.0f), 1e-3f) && near(my, 0.0f, 1e-3f),
          "a sideways translation of 0.1 m moves a 2 m pixel by tv*w/(2z) px (2.5 px)");
    check(expectedMotion(c, 50.0f, 50.0f, false, 4.0f, &mx, &my) && near(mx, 1.25f, 1e-3f), "twice as far moves half as much (parallax)");
    check(expectedMotion(c, 50.0f, 50.0f, true, 2.0f, &mx, &my) && near(mx, 0.0f, 1e-3f), "the far plane takes no translation");
    // A rotation of the view: yaw by 1 degree moves a far pixel by about tan(1 deg) * w/2 = 0.873 px (the tangents span 2 across w pixels).
    CamTerm r = identityCam(100, 100);
    const float a = 1.0f * 3.14159265f / 180.0f;
    r.rows[0][0] = std::cos(a); r.rows[0][2] = std::sin(a);
    r.rows[2][0] = -std::sin(a); r.rows[2][2] = std::cos(a);
    check(expectedMotion(r, 50.0f, 50.0f, true, 0.0f, &mx, &my) && std::fabs(std::fabs(mx) - 0.873f) < 0.02f && near(my, 0.0f, 0.01f),
          "a one degree yaw moves a far pixel by about 0.87 px");
    CamTerm bad = identityCam(0, 0);
    check(!expectedMotion(bad, 0.0f, 0.0f, true, 0.0f, &mx, &my), "an empty term has no expectation");
    bad = identityCam(10, 10);
    bad.valid = false;
    check(!expectedMotion(bad, 0.0f, 0.0f, true, 0.0f, &mx, &my), "an invalid term has no expectation");
    PixelFacts p;
    p.haveWorld = p.haveHead = true;
    p.expWx = 1.0f; p.expWy = 0.0f; p.expHx = 5.0f; p.expHy = 0.0f;
    p.flags.path = 2;
    float ex = 0, ey = 0;
    check(expectedForPath(p, &ex, &ey) && ex == 1.0f, "path 2 (the shader's world branch) compares to the world term");
    p.flags.path = 1;
    check(expectedForPath(p, &ex, &ey) && ex == 5.0f, "path 1 (the head's delta) compares to the ship term");
    check(std::strcmp(pathName(2), "camera-world") == 0 && std::strcmp(pathName(1), "camera-ship") == 0, "path names: 2 is the world, 1 the ship");
    p.flags.path = 11;
    p.mvx = 4.5f;
    check(expectedForPath(p, &ex, &ey) && ex == 5.0f, "another path compares to the nearer term");
    p.haveHead = false;
    check(expectedForPath(p, &ex, &ey) && ex == 1.0f, "with only the world term, that one");
    p.haveWorld = false;
    check(!expectedForPath(p, &ex, &ey), "with neither, nothing");
}

VerdictIn baseIn() {
    VerdictIn v;
    v.status = Status::Top;
    v.box = 256; v.ctr = 1;
    v.routeNone = true;
    v.havePixel = true;
    v.px.valid = true;
    v.px.flags.path = 2;
    v.px.haveWorld = v.px.haveHead = true;
    return v;
}

void testVerdicts() {
    VerdictOut o;
    // The pad hologram's expected shape: additive, no depth write, no route, over the surface behind it.
    VerdictIn v = baseIn();
    v.cls = Cls::Additive; v.depthWrite = false; v.aboveOwner = true; v.depthOwnerIdx = 7;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "TRANSLUCENT_NO_MASK") == 0, "additive, no depth write, no mask: the translucent finding leads");
    check(has(o.text, "additive") && has(o.text, "no reactive or coverage mask") && has(o.text, "ignored by preset K"), "and says what is missing and that K ignores the reactive mask");
    check(has(o.text, "inherits depth and motion of #7") && has(o.text, "borrowed motion"), "it inherits the depth owner's depth and motion");
    check(has(o.text, "no EDVR route"), "and has no route at all");
    check(strlen(o.text) < sizeof(o.text) - 1, "the text fits its buffer");
    // Handled draws are not blamed.
    v = baseIn(); v.cls = Cls::Additive; v.uiTaken = true; v.routeNone = false;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "UI_LAYER") == 0 && !has(o.text, "TRANSLUCENT") && !has(o.text, "no reactive"), "taken by the UI layer: handled, not blamed");
    v = baseIn(); v.cls = Cls::Alpha; v.masked = true; v.maskSlot = "smoke"; v.routeNone = false;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "MASKED") == 0 && has(o.text, "smoke"), "marking the coverage mask: handled, and the slot is named");
    v = baseIn(); v.cls = Cls::Alpha; v.holo = true; v.routeNone = false;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "HOLO_PASS") == 0, "the hologram pass: handled");
    // Translucent that writes depth.
    v = baseIn(); v.cls = Cls::Alpha; v.depthWrite = true;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "TRANSLUCENT_DEPTH") == 0, "a blend that writes depth has its own depth but no mask");
    // Buried and unmeasured.
    v = baseIn(); v.status = Status::Buried; v.buriedBy = 12;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "OCCLUDED") == 0 && has(o.text, "#12"), "buried: occluded, by the named draw");
    v = baseIn(); v.status = Status::Unmeasured;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "UNMEASURED") == 0, "unmeasured");
    v = baseIn(); v.status = Status::Partial; v.cls = Cls::Opaque; v.depthWrite = true; v.isDepthOwner = true; v.px.flags.engineKind = 0;
    verdictFor(v, &o);
    check(has(o.text, "unproven"), "partial says it is unproven");
    // The railing: an opaque surface on the camera term alone.
    v = baseIn(); v.cls = Cls::Opaque; v.depthWrite = true; v.isDepthOwner = true;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "CAMERA_ONLY") == 0 && has(o.text, "no per-object motion source"), "opaque, top, owner, camera path, unkeyed: camera term alone");
    v.keyed = true; v.engineSub = true; v.routeNone = false;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "OK") == 0, "the same surface with a keyed pair and substituted shaders is fine");
    // Motion mismatch.
    v = baseIn(); v.cls = Cls::Opaque; v.depthWrite = true; v.isDepthOwner = true; v.keyed = true; v.routeNone = false;
    v.px.mvx = 3.0f; v.px.mvy = 0.0f; v.px.expWx = 1.0f; v.px.expWy = 0.0f; v.px.flags.path = 2;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "MV_MISMATCH") == 0 && has(o.text, "2.00 px"), "the pass wrote 3 and the camera term says 1: 2.00 px apart");
    v.px.mvx = 1.2f;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "OK") == 0, "within the tolerance it is fine");
    v.px.flags.path = 11; v.px.mvx = 4.0f; v.px.flags.engineKind = 1;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "ENGINE_MOTION") == 0 && has(o.text, "3.00 px"), "an engine-record pixel far from the camera term is information, not a fault");
    // Thin and hidden.
    v = baseIn(); v.cls = Cls::Additive; v.depthWrite = false; v.box = 3; v.ctr = 1; v.topology = 2; v.uiTaken = true; v.routeNone = false;
    verdictFor(v, &o);
    check(has(o.text, "thin: 3 of 256") && has(o.text, "line or point"), "three samples and a line primitive: thin");
    v = baseIn(); v.cls = Cls::Opaque; v.depthWrite = true; v.isDepthOwner = true; v.keyed = true; v.routeNone = false; v.px.flags.hidden = true;
    verdictFor(v, &o);
    check(has(o.text, "history is hidden"), "a hidden pixel is said");
    // No pixel: the state rules still run.
    v = baseIn(); v.havePixel = false; v.cls = Cls::Additive;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "TRANSLUCENT_NO_MASK") == 0, "without the pixel the state-only rules still fire");
    // Drawn over earlier content with no depth owner.
    v = baseIn(); v.cls = Cls::Alpha; v.aboveOwner = true; v.depthOwnerIdx = -1; v.uiTaken = true; v.routeNone = false;
    verdictFor(v, &o);
    check(has(o.text, "no draw of its own owning depth"), "no depth owner: the background's depth and motion");
    // Nothing found.
    v = baseIn(); v.cls = Cls::Opaque; v.depthWrite = true; v.isDepthOwner = true; v.keyed = true; v.routeNone = false;
    v.px.flags.engineKind = 1; v.px.flags.path = 11; v.px.haveWorld = v.px.haveHead = false;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "OK") == 0, "no finding is OK, in words");
    verdictFor(v, nullptr);
    check(true, "a null out is tolerated");
}

void testRolesAndOutline() {
    check(roleOf(false, true, true) == Role::Unknown, "without the pass's word every role is unknown");
    check(roleOf(true, true, true) == Role::ColourAndDepth && roleOf(true, true, false) == Role::Colour && roleOf(true, false, true) == Role::Depth &&
              roleOf(true, false, false) == Role::Intermediate, "colour source, shared scene depth, both, neither");
    check(roleIsDlssInput(Role::Unknown) && roleIsDlssInput(Role::Depth) && !roleIsDlssInput(Role::Intermediate), "only an intermediate pass is not a DLSS input");
    check(roleHasSceneDepth(Role::Depth) && roleHasSceneDepth(Role::ColourAndDepth) && !roleHasSceneDepth(Role::Colour), "scene depth roles");
    check(std::strcmp(roleText(Role::Intermediate), "intermediate pass, not a DLSS input") == 0, "the label the coordinator asked for, word for word");
    VerdictIn v = baseIn();
    v.role = Role::Intermediate; v.cls = Cls::Additive; v.depthWrite = false; v.aboveOwner = true; v.depthOwnerIdx = 3;
    VerdictOut o;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "INTERMEDIATE") == 0 && has(o.text, "not a DLSS input") && !has(o.text, "TRANSLUCENT") && !has(o.text, "no reactive"),
          "an additive draw in an intermediate pass gets no DLSS rule");
    v.role = Role::Depth;
    verdictFor(v, &o);
    check(std::strcmp(o.code, "TRANSLUCENT_NO_MASK") == 0, "the same draw in a DLSS depth target does");
    // The outline: a ring of magenta just outside [1,3)x[1,3) of a 6x5 image; the box's own pixels untouched; clipped at the image edge.
    std::vector<uint8_t> img(6 * 5 * 3, 7);
    drawBoxOutline(img.data(), 6, 5, 1, 1, 3, 3);
    auto px = [&](int x, int y, int c) { return img[(static_cast<size_t>(y) * 6 + static_cast<size_t>(x)) * 3 + static_cast<size_t>(c)]; };
    bool ring = true;
    for (int x = 0; x <= 3; ++x) ring = ring && px(x, 0, 0) == 255 && px(x, 0, 1) == 0 && px(x, 3, 0) == 255;
    for (int y = 1; y < 3; ++y) ring = ring && px(0, y, 0) == 255 && px(3, y, 2) == 255;
    check(ring, "the ring is the box grown by one pixel");
    check(px(1, 1, 0) == 7 && px(2, 2, 1) == 7 && px(1, 2, 2) == 7 && px(2, 1, 0) == 7, "the box's own pixels are untouched");
    check(px(4, 0, 0) == 7 && px(0, 4, 0) == 7 && px(5, 4, 1) == 7, "nothing outside the ring changed");
    std::vector<uint8_t> tiny(2 * 2 * 3, 7);
    drawBoxOutline(tiny.data(), 2, 2, 0, 0, 2, 2);
    check(tiny[0] == 7 && tiny[11] == 7, "a box that fills the image has its ring off the image: nothing is drawn, nothing overruns");
    drawBoxOutline(nullptr, 2, 2, 0, 0, 1, 1);
    check(true, "a null image is tolerated");
}

uint32_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void testEngineRecords() {
    check(slotFromCode(11.0f) == 5 && slotFromCode(1.0f) == 0 && slotFromCode(2049.0f) == 1024, "a marked pixel's x is 2*slot+1");
    check(slotFromCode(10.0f) == -1 && slotFromCode(0.0f) == -1 && slotFromCode(-1.0f) == -1 && slotFromCode(3.5f) == -1 && slotFromCode(NAN) == -1 &&
              slotFromCode(INFINITY) == -1, "an even code, none, a fraction, NaN and infinity are no slot");
    uint32_t w[kEngineRecordWords] = {};
    w[0] = 0;                       // base: rigid
    w[1] = fbits(1.0f);             // scale
    w[2] = 0x11111111u; w[3] = 0x22222222u;   // quaternion
    w[4] = fbits(10.0f); w[5] = fbits(20.0f); w[6] = fbits(30.0f);   // position
    w[72] = 0xAABBCCDDu;            // marker
    w[73] = fbits(10.0f); w[74] = fbits(20.0f); w[75] = fbits(30.0f);   // previous position
    w[77] = fbits(1.0f); w[78] = 0x11111111u; w[79] = 0x22222222u;     // previous scale, quaternion
    EngineRecord r = decodeEngineRecord(w);
    check(!r.moved && r.base == 0 && r.scale == 1.0f && r.pos[1] == 20.0f && r.prevPos[2] == 30.0f && r.marker == 0xAABBCCDDu, "equal pose blocks: a static rigid record, read word for word");
    w[74] = fbits(20.5f);
    check(decodeEngineRecord(w).moved, "a different previous position: moved");
    w[74] = fbits(20.0f);
    w[79] = 0x22222223u;
    check(decodeEngineRecord(w).moved, "a different previous quaternion: moved");
    w[79] = 0x22222222u;
    w[77] = fbits(0.5f);
    check(decodeEngineRecord(w).moved, "a different previous scale: moved");
    w[77] = fbits(1.0f);
    w[0] = 4;
    check(decodeEngineRecord(w).base == 4, "the base word is the palette base");
    check(!decodeEngineRecord(nullptr).moved, "a null record decodes to nothing");
    SlotInfo si;
    si.slot = 5; si.pixels = 140; si.kinds[3] = 140; si.kinds[1] = 12; si.haveRecord = true; si.rec = decodeEngineRecord(w); si.poolRecords = 9216;
    char b[kLineBuf];
    int n = formatEngineSlotLine(b, sizeof(b), si);
    check(n > 0 && static_cast<size_t>(n) < kLineMax && has(b, "slot=5 pixels=140") && has(b, "exact=12") && has(b, "other=140") && has(b, "SKINNED, palette base") &&
              has(b, "STATIC") && has(b, "marker=AABBCCDD"), "the slot line: pixels, kinds, record class, motion state, marker");
    w[0] = 0; w[74] = fbits(99.0f);
    si.rec = decodeEngineRecord(w);
    formatEngineSlotLine(b, sizeof(b), si);
    check(has(b, "rigid") && has(b, "MOVED"), "a rigid mover is named so");
    si.haveRecord = false;
    formatEngineSlotLine(b, sizeof(b), si);
    check(has(b, "record not read") && has(b, "9216"), "a slot beyond the pool says so");
    check(std::strlen(engineSlotsLegend()) < kLineMax, "the legend fits a line");
    ProducerTotals t, f;
    formatProducersLine(b, sizeof(b), t, f);
    check(has(b, "not available"), "no producer source: said");
    t.have = f.have = true; t.emitCalls = 9; t.emitItems = 10; t.secondCalls = 2; t.secondItems = 3; t.primaryCalls = 4; f.secondCalls = 1; f.secondItems = 6;
    n = formatProducersLine(b, sizeof(b), t, f);
    check(n > 0 && static_cast<size_t>(n) < kLineMax && has(b, "second producer FUN_14369C9C0 calls 1 items 6 (never stamped)") && has(b, "since launch: emit calls 9 items 10"),
          "the producers line: the captured frame and the totals");
}

void testText() {
    char b[kLineBuf];
    check(formatInstallLine(b, sizeof(b), false, true, "F4", "edvr.ini") > 0 && std::strcmp(b, "scene inspect: off; hotkey.scene_inspect is bound to F4") == 0, "install line, bound");
    formatInstallLine(b, sizeof(b), false, false, "", "edvr.ini");
    check(std::strcmp(b, "scene inspect: off; hotkey.scene_inspect is NOT bound -- add \"scene_inspect = <a free key>\" under [hotkey] in edvr.ini to use it") == 0,
          "install line, unbound: pinned text");
    formatInstallLine(b, sizeof(b), true, false, "", "edvr.ini");
    check(has(b, "VR only"), "install line, flat");
    formatInstallLine(b, sizeof(b), false, true, nullptr, nullptr);
    check(has(b, "bound to "), "install line tolerates nulls");
    formatArmedLine(b, sizeof(b), 3, "hotkey.scene_inspect");
    check(has(b, "armed cap=3") && has(b, "hotkey.scene_inspect"), "armed line");
    formatCollectingLine(b, sizeof(b), 3, 1234);
    check(has(b, "collecting cap=3 from game frame 1234"), "collecting line");
    check(std::strlen(legendText()) < kLineMax, "the legend fits a log line");
    for (int i = 1; i < static_cast<int>(Refusal::kCount); ++i) check(std::strlen(refusalText(static_cast<Refusal>(i))) > 10, "every refusal has words");
    check(has(refusalText(Refusal::kNoEyePass), "NOT an empty box"), "no eye pass says it is not an empty box");

    // Worst-case lines fit the log's buffer.
    DrawRow r;
    r.idx = 4294967295u; r.rt = 63; r.eye = -1; r.order = 4294967295u; r.gameFrame = 4294967295u; r.vs = ~0ull; r.ps = ~0ull; r.kind = 'X';
    r.count = 4294967295u; r.instances = 4294967295u; r.topology = 34;
    std::memset(r.cls, 'c', sizeof(r.cls) - 1);
    std::memset(r.status, 's', sizeof(r.status) - 1);
    r.box = 256; r.ctr = 1;
    std::memset(r.blend, 'b', sizeof(r.blend) - 1);
    std::memset(r.depth, 'd', sizeof(r.depth) - 1);
    std::memset(r.stencil, 't', sizeof(r.stencil) - 1);
    std::memset(r.cull, 'u', sizeof(r.cull) - 1);
    r.rtCount = 8;
    std::memset(r.rt0, 'f', sizeof(r.rt0) - 1);
    r.rt0W = r.rt0H = r.dsW = r.dsH = 99999;
    std::memset(r.ds, 'g', sizeof(r.ds) - 1);
    std::memset(r.srv, 'v', sizeof(r.srv) - 1);
    std::memset(r.role, 'o', sizeof(r.role) - 1);
    std::memset(r.verdict, 'x', sizeof(r.verdict) - 1);
    std::memset(r.uiFamily, 'x', sizeof(r.uiFamily) - 1);
    std::memset(r.uiDecision, 'x', sizeof(r.uiDecision) - 1);
    std::memset(r.holo, 'x', sizeof(r.holo) - 1);
    std::memset(r.mask, 'x', sizeof(r.mask) - 1);
    std::memset(r.keyed, 'x', sizeof(r.keyed) - 1);
    std::memset(r.engine, 'x', sizeof(r.engine) - 1);
    std::memset(r.motion, 'x', sizeof(r.motion) - 1);
    int n = formatDrawLine(b, sizeof(b), r);
    checkf(n > 0 && static_cast<size_t>(n) < kLineMax, "the worst-case draw line is %d bytes, over the %zu a log line keeps", n, kLineMax);
    n = formatRouteLine(b, sizeof(b), r);
    checkf(n > 0 && static_cast<size_t>(n) < kLineMax, "the worst-case route line is %d bytes", n);
    VerdictOut v;
    std::memset(v.code, 'C', sizeof(v.code) - 1);
    std::memset(v.text, 'T', sizeof(v.text) - 1);
    n = formatVerdictLine(b, sizeof(b), 4294967295u, v);
    checkf(n > 0 && static_cast<size_t>(n) < kLineMax, "the worst-case verdict line is %d bytes", n);
    // The real worst verdict: every finding at once.
    VerdictIn vin = baseIn();
    vin.cls = Cls::Additive; vin.depthWrite = false; vin.aboveOwner = true; vin.depthOwnerIdx = 2000000000; vin.box = 1; vin.ctr = 1; vin.topology = 2;
    vin.px.flags.path = 2; vin.px.mvx = 99.0f; vin.px.expWx = -99.0f; vin.px.flags.hidden = true; vin.status = Status::Partial;
    verdictFor(vin, &v);
    n = formatVerdictLine(b, sizeof(b), 4294967295u, v);
    checkf(n > 0 && static_cast<size_t>(n) < kLineMax && std::strlen(v.text) < sizeof(v.text) - 2, "the longest verdict (%zu text bytes) fits its buffer and the line", std::strlen(v.text));
    n = formatOccludedLine(b, sizeof(b), r, 2000000000);
    check(n > 0 && static_cast<size_t>(n) < kLineMax, "occluded line fits");

    BoxInfo bi;
    bi.fromGeometry = true; bi.u = 0.5385f; bi.v = 0.5357f; bi.cantDeg = 10.0f; bi.inside = false; bi.vpW = 2016; bi.vpH = 2016;
    n = formatBoxLine(b, sizeof(b), 4, bi);
    check(n > 0 && static_cast<size_t>(n) < kLineMax && has(b, "head-forward") && has(b, "OUTSIDE") && has(b, "canting=10.00"), "box line: geometry, canting and an outside point are said");
    bi.fromGeometry = false;
    formatBoxLine(b, sizeof(b), 4, bi);
    check(has(b, "viewport-centre"), "box line: the fallback is said");
    FrameCounts fc;
    fc.eyeDraws = 4294967295u;
    n = formatFrameLine(b, sizeof(b), 4, fc);
    check(n > 0 && static_cast<size_t>(n) < kLineMax, "frame line fits");
    formatEmptyBoxLine(b, sizeof(b), 4, 120);
    check(has(b, "nothing wrote a sample inside the box") && has(b, "120 eye draws") && has(b, "RAN"), "the empty-box line says the capture ran");
    Rect rc{10, 20, 26, 36};
    n = formatTargetLine(b, sizeof(b), 1, "R11G11B10_FLOAT", 2016, 2016, "D32_FLOAT", 0, 0, 2016, 2016, rc, 18, 28, 100, 3);
    check(n > 0 && has(b, "box=[10,26)x[20,36)") && has(b, "centre_px=18,28"), "target line");

    PixelFacts p;
    p.valid = true; p.depth = 0.25f; p.metres = 2.0f; p.far = false; p.mvx = 1.5f; p.mvy = -0.5f; p.haveDlss = true; p.dlx = 1.5f; p.dly = -0.5f;
    p.haveWorld = p.haveHead = true; p.expWx = 1.6f; p.expWy = 0.0f; p.expHx = 0.0f; p.expHy = 0.0f; p.flags.path = 2; p.coverage = 0; p.bias = 0;
    n = formatPixelLine(b, sizeof(b), "centre", 31, 32, p);
    check(n > 0 && static_cast<size_t>(n) < kLineMax && has(b, "(2.000 m)") && has(b, "mv_pass=(1.500,-0.500)") && has(b, "mv_dlss=(1.500,-0.500)") &&
              has(b, "mismatch=0.51") && has(b, "path=2(camera-world)") && has(b, "coverage_mask=0"), "the pixel line carries depth, both motions, the expectation and the masks");
    PixelFacts np;
    formatPixelLine(b, sizeof(b), "centre", 1, 2, np);
    check(has(b, "not read"), "an unread pixel says so");
    p.far = true;
    formatPixelLine(b, sizeof(b), "centre", 31, 32, p);
    check(has(b, "far plane"), "far plane is said");
    SampleStats ss;
    BoxSample bs;
    bs.valid = true; bs.px = p;
    accumulate(&ss, bs);
    bs.px.depth = 0.5f; bs.px.mvx = -2.0f; bs.px.flags.hidden = true; bs.px.flags.path = 11; bs.px.coverage = 5;
    accumulate(&ss, bs);
    check(ss.n == 2 && near(ss.depthMin, 0.25f) && near(ss.depthMax, 0.5f) && near(ss.mvxMin, -2.0f) && near(ss.mvxMax, 1.5f) && ss.path[2] == 1 && ss.path[11] == 1 &&
              ss.hidden == 1 && ss.coverageSet == 1 && ss.coverageMax == 5, "box stats: ranges, paths, hidden and mask counts");
    n = formatBoxStatsLine(b, sizeof(b), ss);
    check(n > 0 && static_cast<size_t>(n) < kLineMax && has(b, "2 pixels read"), "box stats line");
    DlssFacts df;
    n = formatDlssLine(b, sizeof(b), df);
    check(has(b, "not captured"), "no DLSS evaluation: said");
    df.have = true; df.preset = 11; df.w = 2016; df.h = 2016; df.outW = 4032; df.outH = 4032; df.jx = 0.25f; df.jy = -0.125f; df.reactiveHanded = true; df.reset = true;
    df.resetWhy = "first frame of this eye's history"; df.sync = "same frame as the draws"; df.sceneFrame = 77;
    n = formatDlssLine(b, sizeof(b), df);
    check(n > 0 && static_cast<size_t>(n) < kLineMax && has(b, "preset=K(11)") && has(b, "2016x2016 -> output 4032x4032") && has(b, "history_reset=1 why=first frame") &&
              has(b, "exposure=pre 1.000 scale 1.000 texture=none") && has(b, "ignored: only preset F reads it") && has(b, "transparency/T&C mask=none supplied"),
          "the DLSS line: preset, sizes, jitter, reset and why, exposure, the ignored reactive mask, no T&C mask");
    FilesInfo fi;
    formatFilesLine(b, sizeof(b), 4, fi);
    check(has(b, "(not written)") && has(b, "(none)"), "no files: said");
    formatCompleteLine(b, sizeof(b), 4, 3, 1, true, false);
    check(has(b, "complete cap=4") && has(b, "3 draws wrote in the box (1 occluded)") && has(b, "DLSS inputs read") && has(b, "DLSS outputs MISSING"), "the complete line");
}

// A tiny CSV reader for the test: one record, RFC 4180.
std::vector<std::string> csvParse(const std::string& s, size_t* pos) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    size_t i = *pos;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < s.size() && s[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c == '\n') {
            ++i;
            break;
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    *pos = i;
    return out;
}

void testCsv() {
    std::string l;
    csvField(&l, "plain");
    csvField(&l, "a,b");
    csvField(&l, "say \"hi\"");
    csvField(&l, "two\nlines");
    csvField(&l, "");
    csvNumber(&l, -7);
    l += '\n';
    size_t pos = 0;
    const std::vector<std::string> f = csvParse(l, &pos);
    check(f.size() == 6 && f[0] == "plain" && f[1] == "a,b" && f[2] == "say \"hi\"" && f[3] == "two\nlines" && f[4].empty() && f[5] == "-7", "quoting round-trips");
    // The draws CSV: header and row agree on the column count, even with hostile text in every string.
    DrawRow r;
    r.idx = 3; r.rt = 1; r.vs = 0xB75A6FF2CA9FA5D6ull; r.ps = 0xD56F859BE4781431ull; r.kind = 'I'; r.count = 36; r.topology = 4; r.box = 256; r.ctr = 1;
    std::snprintf(r.cls, sizeof(r.cls), "additive");
    std::snprintf(r.status, sizeof(r.status), "top");
    std::snprintf(r.blend, sizeof(r.blend), "SRC_ALPHA,ONE,ADD/ONE,ONE,ADD wm=F");   // contains commas
    std::snprintf(r.depth, sizeof(r.depth), "test LESS_EQUAL write 0");
    std::snprintf(r.stencil, sizeof(r.stencil), "off");
    std::snprintf(r.srv, sizeof(r.srv), "0:R8G8B8A8_UNORM 512x512,1:buffer");        // commas
    std::snprintf(r.verdict, sizeof(r.verdict), "none");
    VerdictOut v;
    std::snprintf(v.code, sizeof(v.code), "TRANSLUCENT_NO_MASK");
    std::snprintf(v.text, sizeof(v.text), "a \"quoted\", comma-ridden; text");
    const std::string header = drawsCsvHeader();
    const std::string row = drawsCsvRow(4, r, -1, v);
    size_t hp = 0, rp = 0;
    const std::vector<std::string> hf = csvParse(header, &hp), rf = csvParse(row, &rp);
    checkf(hf.size() == rf.size(), "draws csv: header has %zu columns, a row %zu", hf.size(), rf.size());
    check(rp == row.size() && row.back() == '\n', "one row is one line");
    check(rf[0] == "4" && rf[6] == "B75A6FF2CA9FA5D6" && rf[7] == "D56F859BE4781431", "the cap id and the full 16-digit hashes");
    bool foundBlend = false, foundText = false;
    for (size_t i = 0; i < hf.size(); ++i) {
        if (hf[i] == "blend" && rf[i] == "SRC_ALPHA,ONE,ADD/ONE,ONE,ADD wm=F") foundBlend = true;
        if (hf[i] == "verdict_text" && rf[i] == "a \"quoted\", comma-ridden; text") foundText = true;
    }
    check(foundBlend && foundText, "a blend with commas and a verdict with quotes survive the CSV");
    check(hf[0] == "cap" && hf.back() == "verdict_text", "the header's first and last columns");
    // The box CSV.
    BoxSample bs;
    bs.valid = true; bs.x = 12; bs.y = 34;
    bs.px.valid = true; bs.px.depth = 0.25f; bs.px.metres = 2.0f; bs.px.far = false; bs.px.mvx = 1.5f; bs.px.mvy = -0.5f; bs.px.coverage = 3; bs.px.bias = -1;
    bs.px.flags.path = 11; bs.px.flags.engineKind = 1; bs.px.slot = 17;
    const std::string bh = boxCsvHeader(), br = boxCsvRow(4, bs);
    hp = rp = 0;
    const std::vector<std::string> bhf = csvParse(bh, &hp), brf = csvParse(br, &rp);
    checkf(bhf.size() == brf.size(), "box csv: header %zu, row %zu columns", bhf.size(), brf.size());
    check(brf[1] == "12" && brf[2] == "34" && brf[3] == "0.25" && brf[4] == "2" && brf[5] == "1.5" && brf[6] == "-0.5", "box csv: position, depth, metres, motion");
    check(brf[7].empty() && brf[9].empty(), "box csv: values that were not staged are empty, not zero");
    check(brf[13] == "11" && brf[16] == "1" && brf[17] == "3" && brf[18] == "-1" && brf[19] == "17" && bhf[19] == "engine_slot", "box csv: path, engine kind, masks, slot");
    csvField(nullptr, "x");
    check(true, "a null line is tolerated");
}

// Reads a PNG back: signature, every chunk's CRC, IHDR, the zlib stream's stored blocks, the adler32; returns the pixels.
bool pngDecodeRgb(const std::vector<uint8_t>& png, uint32_t* w, uint32_t* h, std::vector<uint8_t>* rgb) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 8 || std::memcmp(png.data(), sig, 8) != 0) return false;
    size_t pos = 8;
    std::vector<uint8_t> idat;
    bool sawIhdr = false, sawIend = false;
    uint32_t bitDepth = 0, colour = 0;
    while (pos + 12 <= png.size()) {
        const uint32_t len = (png[pos] << 24) | (png[pos + 1] << 16) | (png[pos + 2] << 8) | png[pos + 3];
        if (pos + 12 + len > png.size()) return false;
        const uint8_t* type = &png[pos + 4];
        const uint8_t* data = &png[pos + 8];
        const uint32_t want = (png[pos + 8 + len] << 24) | (png[pos + 9 + len] << 16) | (png[pos + 10 + len] << 8) | png[pos + 11 + len];
        if (crc32Update(0, type, 4 + len) != want) return false;
        if (std::memcmp(type, "IHDR", 4) == 0) {
            *w = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
            *h = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
            bitDepth = data[8];
            colour = data[9];
            sawIhdr = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            sawIend = true;
        }
        pos += 12 + len;
    }
    if (!sawIhdr || !sawIend || bitDepth != 8 || colour != 2 || idat.size() < 6) return false;
    if ((idat[0] & 0x0F) != 8 || ((idat[0] << 8) | idat[1]) % 31 != 0) return false;
    std::vector<uint8_t> raw;
    size_t z = 2;
    for (;;) {
        if (z + 5 > idat.size()) return false;
        const bool last = (idat[z] & 1) != 0;
        if (((idat[z] >> 1) & 3) != 0) return false;   // stored only
        const uint32_t n = idat[z + 1] | (idat[z + 2] << 8), nn = idat[z + 3] | (idat[z + 4] << 8);
        if ((n ^ 0xFFFFu) != nn) return false;
        z += 5;
        if (z + n > idat.size()) return false;
        raw.insert(raw.end(), idat.begin() + static_cast<std::ptrdiff_t>(z), idat.begin() + static_cast<std::ptrdiff_t>(z + n));
        z += n;
        if (last) break;
    }
    if (z + 4 != idat.size()) return false;
    const uint32_t ad = (idat[z] << 24) | (idat[z + 1] << 16) | (idat[z + 2] << 8) | idat[z + 3];
    if (adler32(raw.data(), raw.size()) != ad) return false;
    if (raw.size() != static_cast<size_t>(*h) * (static_cast<size_t>(*w) * 3 + 1)) return false;
    rgb->clear();
    for (uint32_t y = 0; y < *h; ++y) {
        const uint8_t* row = &raw[static_cast<size_t>(y) * (static_cast<size_t>(*w) * 3 + 1)];
        if (row[0] != 0) return false;
        rgb->insert(rgb->end(), row + 1, row + 1 + static_cast<size_t>(*w) * 3);
    }
    return true;
}

void testImages() {
    const uint8_t nine[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(crc32Update(0, nine, 9) == 0xCBF43926u, "crc32 of 123456789");
    const uint8_t iend[4] = {'I', 'E', 'N', 'D'};
    check(crc32Update(0, iend, 4) == 0xAE426082u, "crc32 of IEND (the PNG trailer's well-known value)");
    const uint8_t wiki[9] = {'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a'};
    check(adler32(wiki, 9) == 0x11E60398u, "adler32 of Wikipedia");
    check(adler32(nullptr, 0) == 1u, "adler32 of nothing");
    // Small image round trip.
    std::vector<uint8_t> px = {255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30, 40, 50, 60, 70, 80, 90};
    std::vector<uint8_t> png;
    check(encodePngRgb(px.data(), 3, 2, &png), "a 3x2 image encodes");
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> back;
    check(pngDecodeRgb(png, &w, &h, &back) && w == 3 && h == 2 && back == px, "and decodes back to the same pixels, every chunk CRC valid");
    // A crop that needs several stored blocks (256x256x3 > 65535).
    std::vector<uint8_t> big(256 * 256 * 3);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>((i * 7 + i / 251) & 0xFF);
    check(encodePngRgb(big.data(), 256, 256, &png), "a 256x256 image encodes");
    check(pngDecodeRgb(png, &w, &h, &back) && w == 256 && h == 256 && back == big, "a multi-block stream round-trips");
    // A damaged file is refused by the reader (so the check above means something).
    std::vector<uint8_t> broken = png;
    broken[broken.size() / 2] ^= 0x55;
    check(!pngDecodeRgb(broken, &w, &h, &back), "a flipped byte breaks a CRC");
    check(!encodePngRgb(nullptr, 3, 2, &png) && !encodePngRgb(px.data(), 0, 2, &png) && !encodePngRgb(px.data(), 3, 0, &png) && !encodePngRgb(px.data(), 3, 2, nullptr),
          "bad arguments refuse");
    // Half floats.
    check(halfToFloat(0x3C00) == 1.0f && halfToFloat(0xC000) == -2.0f && halfToFloat(0x0000) == 0.0f, "half 1, -2 and 0");
    check(std::isinf(halfToFloat(0x7C00)) && std::isnan(halfToFloat(0x7E00)), "half infinity and NaN");
    check(near(halfToFloat(0x3555), 0.33325f, 1e-3f) && halfToFloat(0x0001) > 0.0f && halfToFloat(0x0001) < 1e-6f, "half 1/3 and the smallest subnormal");
    check(halfToFloat(0xB800) == -0.5f, "half -0.5");
    // Texels.
    uint8_t rgb[3];
    const uint8_t rgba[4] = {10, 20, 30, 255}, bgra[4] = {30, 20, 10, 255};
    check(texelToRgb(28, rgba, rgb) && rgb[0] == 10 && rgb[1] == 20 && rgb[2] == 30, "R8G8B8A8");
    check(texelToRgb(87, bgra, rgb) && rgb[0] == 10 && rgb[1] == 20 && rgb[2] == 30, "B8G8R8A8 swaps");
    const uint32_t r10 = 1023u | (512u << 10) | (0u << 20) | (3u << 30);
    uint8_t p4[4];
    std::memcpy(p4, &r10, 4);
    check(texelToRgb(24, p4, rgb) && rgb[0] == 255 && rgb[1] >= 127 && rgb[1] <= 129 && rgb[2] == 0, "R10G10B10A2");
    const uint16_t hh[4] = {0x3C00, 0x3800, 0x0000, 0x3C00};
    uint8_t p8[8];
    std::memcpy(p8, hh, 8);
    check(texelToRgb(10, p8, rgb) && rgb[0] == 255 && rgb[1] >= 127 && rgb[1] <= 128 && rgb[2] == 0, "R16G16B16A16_FLOAT, clamped");
    check(!texelToRgb(26, p8, rgb) && texelBytes(26) == 0 && texelBytes(28) == 4 && texelBytes(10) == 8, "an unconverted format is refused, byte sizes");
    // Motion, depth, path.
    check(near(motionScale(0.2f), 127.0f) && near(motionScale(254.0f), 0.5f), "motion scale: at least one pixel full-scale");
    motionToRgb(0.0f, 0.0f, 10.0f, rgb);
    check(rgb[0] == 128 && rgb[1] == 128 && rgb[2] == 0, "no motion is 128,128");
    motionToRgb(1.0f, -1.0f, 10.0f, rgb);
    check(rgb[0] == 138 && rgb[1] == 118, "motion offsets from 128 by the scale");
    motionToRgb(1000.0f, -1000.0f, 10.0f, rgb);
    check(rgb[0] == 255 && rgb[1] == 0, "motion saturates");
    check(depthToGrey(0.5f, 0.0f, 1.0f) == 128 || depthToGrey(0.5f, 0.0f, 1.0f) == 127, "depth stretches");
    check(depthToGrey(0.0f, 0.0f, 1.0f) == 0 && depthToGrey(1.0f, 0.0f, 1.0f) == 255 && depthToGrey(NAN, 0.0f, 1.0f) == 0 && depthToGrey(1.0f, 1.0f, 1.0f) == 0,
          "depth ends, NaN and an empty range");
    DecisionFlags f;
    f.path = 11;
    pathToRgb(f, rgb);
    check(rgb[0] == 255 && rgb[1] == 70, "engine record is red");
    f.hidden = true;
    pathToRgb(f, rgb);
    check(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255, "hidden history is white");
    check(std::strlen(motionImageLegend()) > 10 && std::strlen(pathImageLegend()) > 10, "the image legends exist");
}

void runPure() {
    testBox();
    testClasses();
    testStatus();
    testMotion();
    testVerdicts();
    testRolesAndOutline();
    testEngineRecords();
    testText();
    testCsv();
    testImages();
}

// =============================================================================================================================
// DEVICE HALF
// =============================================================================================================================

constexpr UINT kSize = 64;

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    bool debugLayer = false;
};
void hr(HRESULT h) { checkf(SUCCEEDED(h), "D3D operation failed, hr=0x%08lX", static_cast<unsigned long>(h)); }

Gpu makeGpu() {
    PFN_D3D11_CREATE_DEVICE create = edvr::systemD3D11CreateDevice();
    check(create != nullptr, "System32 D3D11CreateDevice is available");
    Gpu g;
    D3D_FEATURE_LEVEL level{};
    HRESULT made = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, g.dev.ReleaseAndGetAddressOf(), &level,
                          g.ctx.ReleaseAndGetAddressOf());
    g.debugLayer = SUCCEEDED(made);
    if (!g.debugLayer)
        made = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, g.dev.ReleaseAndGetAddressOf(), &level, g.ctx.ReleaseAndGetAddressOf());
    hr(made);
    if (g.debugLayer) g.dev.As(&g.info);
    return g;
}

ComPtr<ID3DBlob> compile(const char* src, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    const HRESULT h = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(h)) throw std::runtime_error(std::string("shader: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "no message"));
    return blob;
}

const char* kVs = R"HLSL(
cbuffer C : register(b0) { float4 rect; float4 misc; float4 colour; };
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 c[4] = { float2(rect.x, rect.y), float2(rect.z, rect.y), float2(rect.x, rect.w), float2(rect.z, rect.w) };
    return float4(c[id], misc.x, 1);
}
)HLSL";
const char* kPs = R"HLSL(
cbuffer C : register(b0) { float4 rect; float4 misc; float4 colour; };
float4 main(float4 p : SV_Position) : SV_Target { return colour; }
)HLSL";

struct Quad {
    float x0, y0, x1, y1;   // pixels of the 64x64 target
    float z;
    float colour[4];
};
struct DrawSpec {
    Quad q;
    int blend;      // 0 off, 1 additive (SRC_ALPHA, ONE)
    int depth;      // 0 test LESS + write, 1 test LESS_EQUAL no write
};

struct Rig {
    Gpu g;
    ComPtr<ID3D11Texture2D> rt, ds, cb;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cbuf;
    ComPtr<ID3D11RasterizerState> rsGame;
    ComPtr<ID3D11BlendState> bsOff, bsAdd;
    ComPtr<ID3D11DepthStencilState> dsWrite, dsTest;
    bool inspect = false;
};

void makeRig(Rig& r) {
    r.g = makeGpu();
    ID3D11Device* d = r.g.dev.Get();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr(d->CreateTexture2D(&td, nullptr, r.rt.GetAddressOf()));
    hr(d->CreateRenderTargetView(r.rt.Get(), nullptr, r.rtv.GetAddressOf()));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr(d->CreateTexture2D(&td, nullptr, r.ds.GetAddressOf()));
    hr(d->CreateDepthStencilView(r.ds.Get(), nullptr, r.dsv.GetAddressOf()));
    ComPtr<ID3DBlob> vb = compile(kVs, "vs_5_0"), pb = compile(kPs, "ps_5_0");
    hr(d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, r.vs.GetAddressOf()));
    hr(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, r.ps.GetAddressOf()));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr(d->CreateBuffer(&bd, nullptr, r.cbuf.GetAddressOf()));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr(d->CreateRasterizerState(&rd, r.rsGame.GetAddressOf()));
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    hr(d->CreateBlendState(&b, r.bsOff.GetAddressOf()));
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    hr(d->CreateBlendState(&b, r.bsAdd.GetAddressOf()));
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
    dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    dd.BackFace = dd.FrontFace;
    hr(d->CreateDepthStencilState(&dd, r.dsWrite.GetAddressOf()));
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr(d->CreateDepthStencilState(&dd, r.dsTest.GetAddressOf()));
}

// The frame start: the pipeline as the game leaves it for an eye draw.
void beginFrame(Rig& r, UINT scissorRects = 0) {
    ID3D11DeviceContext* c = r.g.ctx.Get();
    const float clear[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    c->ClearRenderTargetView(r.rtv.Get(), clear);
    c->ClearDepthStencilView(r.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* v = r.rtv.Get();
    c->OMSetRenderTargets(1, &v, r.dsv.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);
    c->RSSetState(r.rsGame.Get());
    if (scissorRects) {
        const D3D11_RECT sc = {3, 5, 50, 60};
        c->RSSetScissorRects(1, &sc);
    }
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(r.vs.Get(), nullptr, 0);
    c->PSSetShader(r.ps.Get(), nullptr, 0);
    ID3D11Buffer* b = r.cbuf.Get();
    c->VSSetConstantBuffers(0, 1, &b);
    c->PSSetConstantBuffers(0, 1, &b);
}

void setSpec(Rig& r, const DrawSpec& s) {
    ID3D11DeviceContext* c = r.g.ctx.Get();
    float cbv[12];
    cbv[0] = s.q.x0 / kSize * 2.0f - 1.0f;
    cbv[1] = 1.0f - s.q.y0 / kSize * 2.0f;
    cbv[2] = s.q.x1 / kSize * 2.0f - 1.0f;
    cbv[3] = 1.0f - s.q.y1 / kSize * 2.0f;
    cbv[4] = s.q.z; cbv[5] = cbv[6] = cbv[7] = 0.0f;
    std::memcpy(cbv + 8, s.q.colour, 16);
    c->UpdateSubresource(r.cbuf.Get(), 0, nullptr, cbv, 0, 0);
    const float bf[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    c->OMSetBlendState(s.blend ? r.bsAdd.Get() : r.bsOff.Get(), bf, 0xFFFFFFF7u);
    c->OMSetDepthStencilState(s.depth ? r.dsTest.Get() : r.dsWrite.Get(), 5);
}

// The state S1 compares: everything the duplicate may touch and everything it must not.
struct Snapshot {
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4] = {};
    UINT bmask = 0;
    ID3D11DepthStencilState* ds = nullptr;
    UINT dref = 0;
    UINT nScissor = 0;
    D3D11_RECT scissor[16] = {};
    UINT nVp = 0;
    D3D11_VIEWPORT vp[16] = {};
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vcb = nullptr;
    ID3D11Buffer* pcb = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Predicate* predicate = nullptr;
};
void takeSnapshot(ID3D11DeviceContext* c, Snapshot* s) {
    // Gets AddRef what they return; the pointers are compared and released at once (the identity is what is being proven).
    c->RSGetState(&s->rs);
    c->OMGetBlendState(&s->bs, s->bf, &s->bmask);
    c->OMGetDepthStencilState(&s->ds, &s->dref);
    s->nScissor = 16;
    c->RSGetScissorRects(&s->nScissor, s->scissor);
    s->nVp = 16;
    c->RSGetViewports(&s->nVp, s->vp);
    c->OMGetRenderTargets(1, &s->rtv, &s->dsv);
    c->VSGetShader(&s->vs, nullptr, nullptr);
    c->PSGetShader(&s->ps, nullptr, nullptr);
    c->VSGetConstantBuffers(0, 1, &s->vcb);
    c->PSGetConstantBuffers(0, 1, &s->pcb);
    c->IAGetPrimitiveTopology(&s->topo);
    c->IAGetInputLayout(&s->layout);
    BOOL pv = FALSE;
    c->GetPredication(&s->predicate, &pv);
}
void releaseSnapshot(Snapshot* s) {
    for (IUnknown* u : {static_cast<IUnknown*>(s->rs), static_cast<IUnknown*>(s->bs), static_cast<IUnknown*>(s->ds), static_cast<IUnknown*>(s->rtv),
                        static_cast<IUnknown*>(s->dsv), static_cast<IUnknown*>(s->vs), static_cast<IUnknown*>(s->ps), static_cast<IUnknown*>(s->vcb),
                        static_cast<IUnknown*>(s->pcb), static_cast<IUnknown*>(s->layout), static_cast<IUnknown*>(s->predicate)})
        if (u) u->Release();
}
std::string diffSnapshots(const Snapshot& a, const Snapshot& b) {
    std::string d;
    auto add = [&](const char* w) {
        if (!d.empty()) d += ", ";
        d += w;
    };
    if (a.rs != b.rs) add("rasterizer state");
    if (a.bs != b.bs) add("blend state");
    if (std::memcmp(a.bf, b.bf, sizeof(a.bf)) != 0) add("blend factor");
    if (a.bmask != b.bmask) add("sample mask");
    if (a.ds != b.ds) add("depth-stencil state");
    if (a.dref != b.dref) add("stencil reference");
    if (a.nScissor != b.nScissor || std::memcmp(a.scissor, b.scissor, sizeof(D3D11_RECT) * std::min(a.nScissor, 16u)) != 0) add("scissor rects");
    if (a.nVp != b.nVp || std::memcmp(a.vp, b.vp, sizeof(D3D11_VIEWPORT) * std::min(a.nVp, 16u)) != 0) add("viewports");
    if (a.rtv != b.rtv) add("render target");
    if (a.dsv != b.dsv) add("depth view");
    if (a.vs != b.vs) add("vertex shader");
    if (a.ps != b.ps) add("pixel shader");
    if (a.vcb != b.vcb || a.pcb != b.pcb) add("constant buffers");
    if (a.topo != b.topo) add("topology");
    if (a.layout != b.layout) add("input layout");
    if (a.predicate != b.predicate) add("predication");
    return d;
}

// ---- the module's environment ----------------------------------------------------------------------------------------------------------
ID3D11DeviceContext* g_envCtx = nullptr;
std::wstring g_logDir;
float g_frustum[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
int envEye(const void*) { return 0; }
bool envGeometry(uint32_t, float f[4], float rot[9]) {
    std::memcpy(f, g_frustum, sizeof(g_frustum));
    rot[0] = rot[4] = rot[8] = 1.0f;
    rot[1] = rot[2] = rot[3] = rot[5] = rot[6] = rot[7] = 0.0f;
    return true;
}
void envBlend(ID3D11DeviceContext* c, ID3D11BlendState* s, const float bf[4], uint32_t m) { c->OMSetBlendState(s, bf, m); }
std::wstring envLogDir() { return g_logDir; }
uint64_t g_fakeProd[5] = {100, 1000, 7, 70, 500};   // the kinematic producers' cumulative totals; drawRun bumps the second producer inside the captured frame
bool envProducers(uint64_t v[5]) {
    std::memcpy(v, g_fakeProd, sizeof(g_fakeProd));
    return true;
}
const char* envIni() { return "edvr.ini"; }
edvr::SceneInspectEnv makeEnv() {
    edvr::SceneInspectEnv e;
    e.eyeOfTarget = &envEye;
    e.eyeGeometry = &envGeometry;
    e.setBlendRaw = &envBlend;
    e.logDir = &envLogDir;
    e.producerTotals = &envProducers;
    e.iniName = &envIni;
    return e;
}

struct ReplayArgs {
    ID3D11DeviceContext* c;
    unsigned draws = 0;
};
void replay(void* p) {
    auto* a = static_cast<ReplayArgs*>(p);
    a->c->Draw(4, 0);
    ++a->draws;
}

edvr::SceneDrawInfo infoFor(unsigned ordinal, uint64_t vs, uint64_t ps) {
    edvr::SceneDrawInfo i;
    i.kind = 'D';
    i.count = 4;
    i.instances = 1;
    i.ordinal = ordinal;
    i.gameFrame = 100;
    i.vs = vs;
    i.ps = ps;
    i.verdict = "none";
    return i;
}

std::vector<uint8_t> readRt(Rig& r) {
    D3D11_TEXTURE2D_DESC d{};
    r.rt->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    hr(r.g.dev->CreateTexture2D(&d, nullptr, st.GetAddressOf()));
    r.g.ctx->CopyResource(st.Get(), r.rt.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(r.g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m));
    std::vector<uint8_t> out(kSize * kSize * 4);
    for (UINT y = 0; y < kSize; ++y) std::memcpy(&out[y * kSize * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, kSize * 4);
    r.g.ctx->Unmap(st.Get(), 0);
    return out;
}

// The run: six draws as the game would make them, each offered to the module before its own issue.
std::vector<DrawSpec> runSpecs() {
    // The box about the centre of a 64 target is [24,40) x [24,40), centre pixel (32,32).
    return {
        {{0, 0, 64, 64, 0.8f, {0.2f, 0.2f, 0.2f, 1}}, 0, 0},          // 0 the floor: the whole target
        {{16, 16, 48, 48, 0.5f, {0.9f, 0.1f, 0.1f, 1}}, 0, 0},       // 1 an object over the box
        {{20, 20, 44, 44, 0.3f, {0.1f, 0.9f, 0.1f, 1}}, 0, 0},       // 2 a closer object over the box and its centre: buries 0 and 1
        {{24, 24, 40, 40, 0.2f, {0.2f, 0.2f, 0.9f, 0.5f}}, 1, 1},    // 3 an additive overlay on the box, no depth write
        {{0, 0, 8, 8, 0.1f, {1, 1, 0, 1}}, 0, 0},                    // 4 a corner quad away from the box
        {{24, 24, 40, 40, 0.95f, {1, 0, 1, 1}}, 0, 0},               // 5 behind everything over the box: fails the depth test
        {{24, 24, 32, 40, 0.15f, {0, 1, 1, 1}}, 0, 0},               // 6 the box's left half, centre pixel (32,32) NOT included: partial for nothing later
    };
}

// Runs the six draws; with the module armed, offers each one first. Returns the draws' count and records S1 on the way.
std::string drawRun(Rig& r, bool offer, unsigned* snapshotsTaken, int scissor = 0) {
    std::string diffs;
    beginFrame(r, scissor ? 1 : 0);
    if (offer) {
        // Pass the real clear to the module, as the hook would.
        edvr::sceneInspectNoteClear(r.rt.Get());
    }
    const std::vector<DrawSpec> specs = runSpecs();
    for (size_t i = 0; i < specs.size(); ++i) {
        setSpec(r, specs[i]);
        if (offer) {
            Snapshot before, after;
            takeSnapshot(r.g.ctx.Get(), &before);
            ReplayArgs a{r.g.ctx.Get(), 0};
            if (i == 0) {
                g_fakeProd[2] += 3;   // the second producer appends inside the captured frame
                g_fakeProd[3] += 7;
            }
            edvr::sceneInspectDraw(r.g.ctx.Get(), infoFor(static_cast<unsigned>(i), 0x1000 + i, 0x2000 + i), &replay, &a);
            takeSnapshot(r.g.ctx.Get(), &after);
            const std::string d = diffSnapshots(before, after);
            if (!d.empty()) diffs += "draw " + std::to_string(i) + ": " + d + "; ";
            releaseSnapshot(&before);
            releaseSnapshot(&after);
            if (snapshotsTaken) ++*snapshotsTaken;
        }
        r.g.ctx->Draw(4, 0);
    }
    return diffs;
}

std::string logText(size_t from = 0) {
    std::string s;
    for (size_t i = from; i < edvr::g_lines.size(); ++i) {
        s += edvr::g_lines[i];
        s += '\n';
    }
    return s;
}
std::string lineWith(size_t from, const char* a, const char* b = nullptr) {
    for (size_t i = from; i < edvr::g_lines.size(); ++i)
        if (has(edvr::g_lines[i], a) && (!b || has(edvr::g_lines[i], b))) return edvr::g_lines[i];
    return std::string();
}

// Frame boundaries until the report with `needle` is in the log: a frame apart, as the game's are (WARP answers queries on its own thread, so a
// burst of boundaries with no time between them can outrun it).
bool pump(Rig& r, uint32_t* frame, size_t from, const char* needle, int maxTicks = 200) {
    for (int i = 0; i < maxTicks; ++i) {
        r.g.ctx->Flush();
        Sleep(4);
        edvr::sceneInspectFrameBoundary(++*frame);
        if (!lineWith(from, needle).empty()) return true;
    }
    return false;
}

ComPtr<ID3D11Texture2D> makeTex(Rig& r, UINT w, UINT h, DXGI_FORMAT f, const void* data, UINT pitch) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    ComPtr<ID3D11Texture2D> t;
    hr(r.g.dev->CreateTexture2D(&d, data ? &init : nullptr, t.GetAddressOf()));
    return t;
}
uint16_t floatToHalf(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const uint32_t s = (b >> 16) & 0x8000u;
    int e = static_cast<int>((b >> 23) & 0xFF) - 127 + 15;
    const uint32_t m = (b >> 13) & 0x3FFu;
    if (f == 0.0f) return static_cast<uint16_t>(s);
    if (e <= 0) return static_cast<uint16_t>(s);
    if (e >= 31) return static_cast<uint16_t>(s | 0x7C00u);
    return static_cast<uint16_t>(s | (static_cast<uint32_t>(e) << 10) | m);
}

// The temporal pass's inputs as synthetic textures: colour a gradient, the pass's mv (mvx, 0) on the camera-world path, depth 2 m, masks clear.
struct Synth {
    ComPtr<ID3D11Texture2D> colour, motion, depth, decision, coverage, bias, dlss, final, slots;
    ComPtr<ID3D11Buffer> pool;
    edvr::SceneInputsIn in;
    edvr::SceneOutputsIn out;
};
// roles: 1 the inputs name the rig's own target and depth as DLSS's (the real case), 2 name textures nothing drew into (every target is then an
// intermediate pass), 0 name nothing (the roles are unknown).
void makeSynth(Rig& r, Synth& s, float mvx, float dlx, int roles = 1) {
    const UINT n = kSize * kSize;
    std::vector<uint8_t> col(n * 4);
    for (UINT y = 0; y < kSize; ++y)
        for (UINT x = 0; x < kSize; ++x) {
            uint8_t* p = &col[(y * kSize + x) * 4];
            p[0] = static_cast<uint8_t>(x * 4); p[1] = static_cast<uint8_t>(y * 4); p[2] = 128; p[3] = 255;
        }
    std::vector<uint16_t> mv(n * 2);
    for (UINT i = 0; i < n; ++i) {
        mv[i * 2] = floatToHalf(dlx);
        mv[i * 2 + 1] = floatToHalf(0.0f);
    }
    std::vector<float> depth(n, 0.25f), dec(n * 4);
    for (UINT i = 0; i < n; ++i) {
        dec[i * 4] = mvx; dec[i * 4 + 1] = 0.0f; dec[i * 4 + 2] = 2.0f; dec[i * 4 + 3] = 2.0f;   // path 2 (camera on the world: the shader's count15 branch)
    }
    std::vector<uint8_t> zero(n, 0);
    s.colour = makeTex(r, kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM, col.data(), kSize * 4);
    s.motion = makeTex(r, kSize, kSize, DXGI_FORMAT_R16G16_FLOAT, mv.data(), kSize * 4);
    s.depth = makeTex(r, kSize, kSize, DXGI_FORMAT_R32_FLOAT, depth.data(), kSize * 4);
    s.decision = makeTex(r, kSize, kSize, DXGI_FORMAT_R32G32B32A32_FLOAT, dec.data(), kSize * 16);
    s.coverage = makeTex(r, kSize, kSize, DXGI_FORMAT_R8_UNORM, zero.data(), kSize);
    s.bias = makeTex(r, kSize, kSize, DXGI_FORMAT_R8_UNORM, zero.data(), kSize);
    std::vector<uint8_t> big(128 * 128 * 4, 90);
    s.dlss = makeTex(r, 128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, big.data(), 128 * 4);
    std::vector<uint8_t> big2(128 * 128 * 4, 200);
    s.final = makeTex(r, 128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, big2.data(), 128 * 4);
    s.in.colour = s.colour.Get(); s.in.motion = s.motion.Get(); s.in.depth = s.depth.Get(); s.in.decision = s.decision.Get();
    s.in.coverage = s.coverage.Get(); s.in.bias = s.bias.Get();
    // Engine-record velocity's inputs: every pixel marked with slot 5 (x = 2*5+1) and a pool of 16 records whose record 5 is a rigid MOVER with a marker no
    // frame ever stamped (the railing's suspected state).
    std::vector<float> slotTexels(static_cast<size_t>(kSize) * kSize * 2);
    for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i) {
        slotTexels[i * 2] = 11.0f;
        slotTexels[i * 2 + 1] = 0.25f;
    }
    s.slots = makeTex(r, kSize, kSize, DXGI_FORMAT_R32G32_FLOAT, slotTexels.data(), kSize * 8);
    std::vector<uint32_t> recs(16 * 84, 0);
    {
        uint32_t* q = &recs[5 * 84];
        auto fb = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        q[1] = fb(1.0f); q[4] = fb(10.0f); q[5] = fb(20.0f); q[6] = fb(30.0f);
        q[72] = 0xDEADBEEFu; q[73] = fb(10.0f); q[74] = fb(19.5f); q[75] = fb(30.0f); q[77] = fb(1.0f);
    }
    D3D11_BUFFER_DESC pbd{};
    pbd.ByteWidth = static_cast<UINT>(recs.size() * 4);
    pbd.Usage = D3D11_USAGE_DEFAULT;
    pbd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    pbd.StructureByteStride = 336;
    D3D11_SUBRESOURCE_DATA pinit{recs.data(), 0, 0};
    hr(r.g.dev->CreateBuffer(&pbd, &pinit, s.pool.GetAddressOf()));
    s.in.engineSlots = roles == 3 ? nullptr : s.slots.Get();
    s.in.enginePool = roles == 3 ? nullptr : s.pool.Get();
    s.in.poolFirst = 0; s.in.poolCount = 16; s.in.poolStride = 336;
    s.in.w = s.in.h = kSize;
    s.in.sceneFrame = 4242;
    s.in.colourSource = roles == 1 ? static_cast<const void*>(r.rt.Get()) : roles == 2 ? static_cast<const void*>(s.colour.Get()) : nullptr;
    s.in.sceneDepth = roles == 1 ? static_cast<const void*>(r.ds.Get()) : roles == 2 ? static_cast<const void*>(s.depth.Get()) : nullptr;
    const float t[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
    std::memcpy(s.in.tanNow, t, sizeof(t));
    std::memcpy(s.in.tanPrev, t, sizeof(t));
    for (int i = 0; i < 3; ++i) {
        s.in.worldRows[i][i] = 1.0f;
        s.in.headRows[i][i] = 1.0f;
    }
    s.in.tvCam[0] = 0.1f; s.in.tvCam[3] = 1.0f;   // the world path: a 0.1 m sideways step; at 2 m on a 64 px image that is 0.1*64/(2*2) = 1.6 px
    s.in.knobs[0] = 0.0f; s.in.knobs[2] = 0.5f;   // depth = 0.5 / metres: raw 0.25 is 2 m
    s.out.dlss = s.dlss.Get(); s.out.final = s.final.Get();
    s.out.w = s.out.h = kSize; s.out.outW = s.out.outH = 128;
    s.out.jx = 0.25f; s.out.jy = -0.125f; s.out.preset = 11; s.out.reactiveHanded = true; s.out.resetWhy = "";
}

struct Capture {
    size_t logFrom = 0;
    std::string diffs;
    unsigned snapshots = 0;
};

// One whole capture: press, boundary, the draws (offered), the pass's offers, boundaries until the report.
Capture capture(Rig& r, Synth* synth, uint32_t* frame, bool offerInputs = true) {
    Capture c;
    c.logFrom = edvr::g_lines.size();
    edvr::sceneInspectRequest("rig");
    edvr::sceneInspectFrameBoundary(++*frame);   // armed -> collecting
    check(edvr::sceneInspectWantsDraws(), "after the boundary the module is collecting");
    // The pass runs after the draws; an offer before any draw is the previous frame's and is not taken.
    check(!edvr::sceneInspectInputsWanted(), "no inputs are wanted before a draw of the captured frame has been seen");
    c.diffs = drawRun(r, true, &c.snapshots);
    if (synth && offerInputs) {
        check(edvr::sceneInspectInputsWanted(), "after the frame's draws the pass's inputs are wanted");
        edvr::sceneInspectInputsOffer(r.g.ctx.Get(), synth->in);
        check(!edvr::sceneInspectInputsWanted(), "and only once");
        edvr::sceneInspectOutputsOffer(r.g.ctx.Get(), synth->out);
    } else if (synth) {
        edvr::sceneInspectInputsRefuse("the rig declined to offer");
    }
    pump(r, frame, c.logFrom, "complete cap=");
    return c;
}

void runDevice() {
    Rig r;
    makeRig(r);
    check(edvr::reportSystemD3D11Only("scene_inspector_test"), "the rig runs on Windows' own d3d11, not EDVR's proxy beside it");
    // The directory the files go to.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_logDir = std::wstring(tmp) + L"edvr_scene_inspector_test_" + std::to_wstring(GetCurrentProcessId());
    edvr::ensureDirectory(g_logDir);
    edvr::g_lines.clear();
    edvr::sceneInspectInstall(false, false, "", makeEnv());
    check(!lineWith(0, "hotkey.scene_inspect is NOT bound").empty(), "S7: install says the key is unbound");
    uint32_t frame = 100;

    // The control: the same draws with the module idle.
    beginFrame(r);
    drawRun(r, false, nullptr);
    const std::vector<uint8_t> control = readRt(r);

    // S7: a capture that was never pressed is silent apart from the install line, and unarmed the module costs the gate's load.
    check(!edvr::sceneInspectWantsDraws() && !edvr::sceneInspectInputsWanted(), "idle: no draws wanted, no inputs wanted");
    edvr::sceneInspectFrameBoundary(++frame);
    check(edvr::g_lines.size() == 1, "S7: a boundary with no press writes nothing");
    edvr::sceneInspectDraw(r.g.ctx.Get(), infoFor(0, 1, 2), &replay, nullptr);   // not collecting: ignored (a null replay would otherwise be counted)
    check(edvr::g_lines.size() == 1, "S7: an idle offer writes nothing");

    // ---- capture 1: full inputs ----
    Synth syn1;
    makeSynth(r, syn1, 1.6f, 1.6f);
    Capture c1 = capture(r, &syn1, &frame);
    const std::string all1 = logText(c1.logFrom);
    checkf(c1.diffs.empty(), "S1: state changed across an offer: %s", c1.diffs.c_str());
    check(c1.snapshots == 7, "S1: every one of the seven draws was checked");
    const std::vector<uint8_t> inspected = readRt(r);
    check(inspected == control, "S2: the render target after the inspected run is byte for byte the control's");
    check(!lineWith(c1.logFrom, "armed cap=1").empty() && !lineWith(c1.logFrom, "collecting cap=1").empty(), "S7: armed and collecting are both said");
    check(!lineWith(c1.logFrom, "complete cap=1").empty(), "S7: the report completes");
    // S3/S4: counts and classes, from the lines the module wrote.
    auto drawLine = [&](unsigned idx) { return lineWith(c1.logFrom, ("scene inspect draw: #" + std::to_string(idx) + " ").c_str()); };
    const std::string d0 = drawLine(0), d1 = drawLine(1), d2 = drawLine(2), d3 = drawLine(3), d4 = drawLine(4), d5 = drawLine(5), d6 = drawLine(6);
    check(d0.empty() && d1.empty(), "S4: the buried draws are not in the main listing");
    check(!lineWith(c1.logFrom, "scene inspect occluded: #0 ").empty() && has(lineWith(c1.logFrom, "scene inspect occluded: #0 "), "under #2"), "S4: the floor is listed as occluded, under draw 2");
    check(!lineWith(c1.logFrom, "scene inspect occluded: #1 ").empty() && has(lineWith(c1.logFrom, "scene inspect occluded: #1 "), "under #2"), "S4: and so is the object under it");
    check(has(lineWith(c1.logFrom, "scene inspect occluded: #0 "), "box=256 ctr=1"), "S3: the floor wrote all 256 samples of the box and the centre");
    check(has(d2, "status=top") && has(d2, "box=256 ctr=1") && has(d2, "cls=opaque"), "S3/S4: the closer object is top with 256 and 1");
    check(has(d3, "status=top") && has(d3, "box=256 ctr=1") && has(d3, "cls=additive") && has(d3, "SRC_ALPHA,ONE,ADD"), "S3/S4: the additive overlay is top, classed additive, blend text intact");
    check(d4.empty() && d5.empty(), "S3: a quad away from the box and a quad that failed the depth test are not involved");
    check(has(d6, "box=128 ctr=0") && has(d6, "status=top"), "S3: the half-covered box counts 128 and misses the centre pixel");
    check(has(d2, "topo=TRIANGLESTRIP") && has(d2, "rt0=R8G8B8A8_UNORM 64x64") && has(d2, "ds=D32_FLOAT 64x64") && has(d2, "depth=[test LESS write 1]") && has(d3, "depth=[test LESS_EQUAL write 0]"),
          "the draw line carries topology, target and depth state");
    const std::string stackLine = lineWith(c1.logFrom, "scene inspect stack:");
    check(has(stackLine, "#2[opaque depth-write DEPTH-OWNER] < #3[additive no-depth-write]"), "S4: the stack is the closer object under the overlay; the object owns depth");
    const std::string v3 = lineWith(c1.logFrom, "scene inspect verdict: #3 ");
    check(has(v3, "TRANSLUCENT_NO_MASK") && has(v3, "inherits depth and motion of #2"), "the overlay's verdict: translucent with no mask, borrowing from draw 2");
    check(has(v3, "no EDVR route"), "and no route");
    const std::string v2 = lineWith(c1.logFrom, "scene inspect verdict: #2 ");
    check(has(v2, "CAMERA_ONLY"), "the opaque owner on the camera path with no key: camera term alone");
    check(!lineWith(c1.logFrom, "scene inspect box: cap=1 ").empty() && has(lineWith(c1.logFrom, "scene inspect box: cap=1 "), "u=0.5000 v=0.5000"), "the box is at the centre of a symmetric view");
    const std::string tline = lineWith(c1.logFrom, "scene inspect target: rt=#0");
    check(has(tline, "box=[24,40)x[24,40)") && has(tline, "centre_px=32,32"), "S3: the target's box and centre pixel");
    check(has(tline, "role=DLSS colour input and depth target"), "the target says it is DLSS's colour input and depth target");
    check(has(d2, "role=colour+depth") && has(d3, "role=colour+depth"), "and so do its draw lines");
    check(has(lineWith(c1.logFrom, "scene inspect frame:"), "7 eye draws seen") && has(lineWith(c1.logFrom, "scene inspect frame:"), "7 tested"), "the frame line counts the draws");
    // S6: pixels and DLSS.
    const std::string px = lineWith(c1.logFrom, "scene inspect pixel centre:");
    check(has(px, "input px 32,32") && has(px, "depth=0.250000 (2.000 m)") && has(px, "mv_pass=(1.600,0.000)") && has(px, "mv_dlss=(1.600,0.000)") && has(px, "path=2(camera-world)"),
          "S6: the centre pixel: depth and metres, both motions, the path");
    check(has(px, "expect_world=(1.600,0.000)"), "S6: the camera-term expectation at 2 m is 1.6 px, as computed by hand");
    check(has(px, "mismatch=0.000 px"), "S6: agreeing motion: no mismatch");
    check(has(lineWith(c1.logFrom, "scene inspect box stats:"), "256 pixels read"), "S6: all 256 box pixels read");
    const std::string dl = lineWith(c1.logFrom, "scene inspect dlss:");
    check(has(dl, "preset=K(11)") && has(dl, "jitter=(0.2500,-0.1250)") && has(dl, "64x64 -> output 128x128") && has(dl, "transparency/T&C mask=none supplied"), "S6: the DLSS line");
    check(!has(v2, "MV_MISMATCH"), "agreeing motion: no mismatch verdict");
    const std::string files = lineWith(c1.logFrom, "scene inspect files:");
    check(has(lineWith(c1.logFrom, "scene inspect engine:"), "1 distinct slots under the box") && has(lineWith(c1.logFrom, "scene inspect engine:"), "pool read (16 records of 336 bytes)"),
          "the engine section names one slot under the box and says the pool was read");
    const std::string slotLine = lineWith(c1.logFrom, "scene inspect engine slot:");
    check(has(slotLine, "slot=5 pixels=256") && has(slotLine, "none=256") && has(slotLine, "rigid (base word 0)") && has(slotLine, "MOVED") && has(slotLine, "marker=DEADBEEF") &&
              has(slotLine, "pos=(10.000,20.000,30.000)") && has(slotLine, "previous pos=(10.000,19.500,30.000)"),
          "the slot line reads the pool's record 5 back: a rigid mover with an unstamped marker, its pose and its previous pose");
    const std::string prodLine = lineWith(c1.logFrom, "scene inspect producers:");
    check(has(prodLine, "in the captured frame: emit FUN_144312E00 calls 0 items 0, second producer FUN_14369C9C0 calls 3 items 7") && has(prodLine, "since launch: emit calls 100 items 1000, second producer calls 10 items 77"),
          "the producers line: the second producer's 3 calls and 7 items inside the captured frame, and the totals since launch");
    check(has(files, "draws_csv=scene_inspect_") && has(files, "_cap1.csv") && has(files, "_box.csv") && has(files, "colour_in.png") && has(files, "depth.png") && has(files, "motion.png") &&
              has(files, "path.png") && has(files, "dlss_out.png") && has(files, "final.png"), "S6: the files are named");
    // Read the files back.
    auto readAll = [&](const std::string& name) {
        std::vector<uint8_t> v;
        FILE* f = _wfopen((g_logDir + L"\\" + std::wstring(name.begin(), name.end())).c_str(), L"rb");
        if (!f) return v;
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
        std::fclose(f);
        return v;
    };
    auto fileOf = [&](const std::string& list, const char* suffix) {
        size_t at = list.find(suffix);
        if (at == std::string::npos) return std::string();
        size_t s = list.rfind("scene_inspect_", at);
        size_t e = at + std::strlen(suffix);
        return list.substr(s, e - s);
    };
    const std::string csvName = fileOf(files, "_cap1.csv"), boxName = fileOf(files, "_cap1_box.csv");
    const std::vector<uint8_t> csvBytes = readAll(csvName);
    check(!csvBytes.empty(), "S6: the draws CSV was written");
    const std::string csvText(csvBytes.begin(), csvBytes.end());
    size_t cp = 0;
    const std::vector<std::string> head = csvParse(csvText, &cp);
    int rows = 0;
    while (cp < csvText.size()) {
        const std::vector<std::string> row = csvParse(csvText, &cp);
        checkf(row.size() == head.size(), "S6: a CSV row has %zu columns, the header %zu", row.size(), head.size());
        ++rows;
    }
    checkf(rows == 5, "S6: the CSV has the five involved draws (two buried, three listed): %d", rows);
    const std::vector<uint8_t> boxBytes = readAll(boxName);
    check(!boxBytes.empty(), "S6: the box CSV was written");
    int boxRows = 0;
    for (uint8_t ch : boxBytes) boxRows += ch == '\n';
    check(boxRows == 257, "S6: the box CSV has a header and 256 pixels");
    const char* pngs[] = {"colour_in", "depth", "motion", "path", "dlss_out", "final"};
    for (const char* p : pngs) {
        const std::string name = fileOf(files, (std::string("_cap1_") + p + ".png").c_str());
        const std::vector<uint8_t> bytes = readAll(name);
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgb;
        checkf(!bytes.empty() && pngDecodeRgb(bytes, &w, &h, &rgb), "S6: %s.png decodes (chunk CRCs, adler32)", p);
        if (std::strcmp(p, "dlss_out") == 0 || std::strcmp(p, "final") == 0) check(w == 128 && h == 128, "S6: the output crop covers the same area at the output's scale");
        else check(w == 64 && h == 64, "S6: an input crop is the 128 window clamped to the 64 test image");
        if (std::strcmp(p, "final") == 0) check(rgb[0] == 200, "S6: the final crop holds the final texture's pixels");
        if (std::strcmp(p, "dlss_out") == 0) check(rgb[0] == 90, "S6: the DLSS crop holds the DLSS texture's pixels");
        if (std::strcmp(p, "colour_in") == 0) {
            check(rgb[0] == 0 && rgb[2] == 128 && rgb[3] == 4, "S6: the colour crop is the colour input (gradient)");
            auto at = [&](int x, int y, int c) { return rgb[(static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 3 + static_cast<size_t>(c)]; };
            check(at(23, 30, 0) == 255 && at(23, 30, 1) == 0 && at(23, 30, 2) == 255 && at(40, 30, 0) == 255 && at(30, 23, 0) == 255 && at(30, 40, 2) == 255,
                  "the box [24,40)x[24,40) is outlined by a magenta ring just outside it");
            check(at(24, 30, 0) == 96 && at(24, 30, 1) == 120 && at(39, 39, 0) == 156 && at(39, 39, 1) == 156 && at(22, 30, 0) == 88,
                  "the box's own pixels, and those outside the ring, are the colour input unchanged");
        }
        if (std::strcmp(p, "final") == 0 || std::strcmp(p, "dlss_out") == 0) {
            auto at = [&](int x, int y, int c) { return rgb[(static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 3 + static_cast<size_t>(c)]; };
            check(at(47, 70, 0) == 255 && at(47, 70, 1) == 0 && at(80, 70, 2) == 255 && at(70, 47, 0) == 255 && at(70, 80, 0) == 255 && at(60, 60, 1) != 0,
                  "on the 2x output crop the ring surrounds the box scaled to [48,80)");
        }
        if (std::strcmp(p, "motion") == 0) check(rgb[0] == 255 || rgb[0] > 128, "S6: the motion crop encodes +x motion above 128");
    }

    // ---- capture 2: a motion vector that disagrees with the camera term ----
    Synth syn2;
    makeSynth(r, syn2, 4.0f, 4.0f);
    Capture c2 = capture(r, &syn2, &frame);
    check(!lineWith(c2.logFrom, "complete cap=2").empty(), "capture 2 completes");
    check(c2.diffs.empty(), "S1: capture 2 restores everything too");
    const std::string px2 = lineWith(c2.logFrom, "scene inspect pixel centre:");
    check(has(px2, "mismatch=2.400 px"), "S6: the pass wrote 4.0, the camera term says 1.6: 2.400 px apart");
    const std::string v2b = lineWith(c2.logFrom, "scene inspect verdict: #2 ");
    check(has(v2b, "MV_MISMATCH") && has(v2b, "2.40 px"), "S6: the mismatch is a verdict");

    // ---- capture 3: a scissor rect the game had set, and no inputs offered ----
    // (the module restores the game's own scissor list)
    {
        c2.logFrom = edvr::g_lines.size();
        edvr::sceneInspectRequest("rig");
        edvr::sceneInspectFrameBoundary(++frame);
        const std::string diffs = drawRun(r, true, nullptr, 1);
        check(diffs.empty(), "S1: with a game scissor rect set, it is restored");
        pump(r, &frame, c2.logFrom, "complete cap=3");
        check(!lineWith(c2.logFrom, "complete cap=3").empty(), "S7: with no inputs offered the report still completes after its wait");
        check(!lineWith(c2.logFrom, "refused (no DLSS inputs").empty(), "S7: and says the inputs were never offered");
        check(!lineWith(c2.logFrom, "scene inspect dlss: not captured").empty(), "S7: and the DLSS line says not captured");
        check(!lineWith(c2.logFrom, "inputs MISSING").empty() || !lineWith(c2.logFrom, "DLSS inputs MISSING").empty(), "S7: the complete line says the inputs are missing");
        check(lineWith(c2.logFrom, "scene inspect pixel").empty(), "S7: no pixel lines without inputs");
        check(!lineWith(c2.logFrom, "scene inspect verdict: #3 ").empty(), "the draw verdicts are still written without the pixel");
    }

    // ---- capture 4: a clear of the target after the first draws removes them ----
    {
        Capture c4;
        c4.logFrom = edvr::g_lines.size();
        edvr::sceneInspectRequest("rig");
        edvr::sceneInspectFrameBoundary(++frame);
        beginFrame(r);
        const std::vector<DrawSpec> specs = runSpecs();
        for (size_t i = 0; i < specs.size(); ++i) {
            if (i == 3) {
                const float clear[4] = {0, 0, 0, 1};
                r.g.ctx->ClearRenderTargetView(r.rtv.Get(), clear);
                edvr::sceneInspectNoteClear(r.rt.Get());
            }
            setSpec(r, specs[i]);
            ReplayArgs a{r.g.ctx.Get(), 0};
            edvr::sceneInspectDraw(r.g.ctx.Get(), infoFor(static_cast<unsigned>(i), 0x1000 + i, 0x2000 + i), &replay, &a);
            r.g.ctx->Draw(4, 0);
        }
        pump(r, &frame, c4.logFrom, "complete cap=4");
        check(!lineWith(c4.logFrom, "complete cap=4").empty(), "capture 4 completes");
        check(lineWith(c4.logFrom, "scene inspect occluded:").empty(), "S5: after a clear nothing before it is on screen, so nothing is buried by it");
        check(lineWith(c4.logFrom, "scene inspect draw: #0 ").empty() && lineWith(c4.logFrom, "scene inspect draw: #2 ").empty(), "S5: the draws before the clear are out of the listing");
        check(!lineWith(c4.logFrom, "scene inspect draw: #3 ").empty(), "S5: the draws after it stay");
        const std::string st4 = lineWith(c4.logFrom, "scene inspect stack:");
        check(has(st4, "#3[additive no-depth-write]") && !has(st4, "#2["), "S5: the stack begins after the clear (the base is the clear)");
    }

    // ---- capture 5: nothing in the box (the frustum puts the point where no draw reaches) ----
    {
        // The head-forward point at the far right of the image: a box at x ~ 56..64. Draws 0 (the floor) reaches it; to get an EMPTY box
        // the draws are limited to the left half.
        g_frustum[0] = -1.0f; g_frustum[1] = 0.05f;   // forward at u = 1 / 1.05 = 0.952
        const size_t from = edvr::g_lines.size();
        edvr::sceneInspectRequest("rig");
        edvr::sceneInspectFrameBoundary(++frame);
        beginFrame(r);
        DrawSpec s{{0, 0, 40, 64, 0.5f, {1, 1, 1, 1}}, 0, 0};
        setSpec(r, s);
        ReplayArgs a{r.g.ctx.Get(), 0};
        edvr::sceneInspectDraw(r.g.ctx.Get(), infoFor(0, 9, 9), &replay, &a);
        r.g.ctx->Draw(4, 0);
        pump(r, &frame, from, "complete cap=5");
        check(has(lineWith(from, "scene inspect box:"), "u=0.9524"), "the box follows the frustum: forward is at u = 0.9524");
        check(!lineWith(from, "scene inspect empty: cap=5").empty() && has(lineWith(from, "scene inspect empty: cap=5"), "RAN"), "S7: nothing in the box is SAID, and says the capture ran");
        check(lineWith(from, "scene inspect draw:").empty(), "and no draw is listed");
        check(lineWith(from, "refused (no eye pass seen").empty(), "S7: an empty box is not reported as no eye pass");
        g_frustum[1] = 1.0f;
    }

    // ---- capture 6: no draw reached the module at all ----
    {
        const size_t from = edvr::g_lines.size();
        edvr::sceneInspectRequest("rig");
        edvr::sceneInspectFrameBoundary(++frame);
        pump(r, &frame, from, "complete cap=6");
        check(!lineWith(from, "refused (no eye pass seen").empty(), "S7: a capture frame with no eye draw says no eye pass, not an empty box");
        check(lineWith(from, "scene inspect empty:").empty(), "S7: and does not claim the box was empty");
    }

    // ---- refusals ----
    {
        const size_t from = edvr::g_lines.size();
        edvr::sceneInspectRequest("one");
        edvr::sceneInspectRequest("two");
        check(!lineWith(from, "refused (a capture is already running").empty(), "a second press while armed is refused, with the phase");
        edvr::sceneInspectFrameBoundary(++frame);
        pump(r, &frame, from, "complete cap=7");
    }
    // ---- capture 9: inputs that name other textures: every target is an intermediate pass, and no DLSS rule is applied to its draws ----
    {
        Synth syn9;
        makeSynth(r, syn9, 1.6f, 1.6f, 2);
        Capture c9 = capture(r, &syn9, &frame);
        check(!lineWith(c9.logFrom, "complete cap=8").empty(), "capture 8 completes (wrong roles)");
        const std::string t9 = lineWith(c9.logFrom, "scene inspect target: rt=#0");
        check(has(t9, "role=intermediate pass, not a DLSS input"), "the target is labelled an intermediate pass, not a DLSS input");
        check(has(lineWith(c9.logFrom, "scene inspect draw: #3 "), "role=intermediate"), "its draw lines say so");
        const std::string v9 = lineWith(c9.logFrom, "scene inspect verdict: #3 ");
        check(has(v9, "INTERMEDIATE") && !has(v9, "TRANSLUCENT") && !has(v9, "NO_ROUTE"), "its verdicts are INTERMEDIATE: no translucent or route finding");
        check(lineWith(c9.logFrom, "scene inspect route: #3 ").empty(), "and carry no route line");
        check(lineWith(c9.logFrom, "scene inspect occluded:").empty() && lineWith(c9.logFrom, "scene inspect stack:").empty(),
              "an intermediate pass has no occluded or stack lines");
        check(has(lineWith(c9.logFrom, "scene inspect frame:"), "0 of them in DLSS input targets"), "the frame line counts none in DLSS input targets");
        check(!lineWith(c9.logFrom, "no draw wrote the texture the game submitted").empty(), "and notes that no draw wrote the submitted texture");
    }
    // ---- capture 10: inputs that name nothing: the roles are unknown and the rules apply as before ----
    {
        Synth syn10;
        makeSynth(r, syn10, 1.6f, 1.6f, 0);
        Capture c10 = capture(r, &syn10, &frame);
        check(!lineWith(c10.logFrom, "complete cap=9").empty(), "capture 9 completes (unknown roles)");
        check(has(lineWith(c10.logFrom, "scene inspect target: rt=#0"), "role unknown"), "unknown roles are said");
        check(has(lineWith(c10.logFrom, "scene inspect verdict: #3 "), "TRANSLUCENT_NO_MASK"), "with the roles unknown the draw rules still apply");
    }

    // ---- capture 8: a draw EDVR withholds (verdict skip) is never drawn, so it buries nothing ----
    {
        const size_t from = edvr::g_lines.size();
        edvr::sceneInspectRequest("rig");
        edvr::sceneInspectFrameBoundary(++frame);
        beginFrame(r);
        const std::vector<DrawSpec> specs = runSpecs();
        for (size_t i = 0; i < 2; ++i) {
            setSpec(r, specs[i == 0 ? 0 : 2]);
            edvr::SceneDrawInfo info = infoFor(static_cast<unsigned>(i), 0x1000 + i, 0x2000 + i);
            if (i == 1) info.verdict = "skip";
            ReplayArgs a{r.g.ctx.Get(), 0};
            edvr::sceneInspectDraw(r.g.ctx.Get(), info, &replay, &a);
            check(i == 0 || a.draws == 0, "a withheld draw is not duplicated");
            if (i == 0) r.g.ctx->Draw(4, 0);   // the skipped draw is not issued
        }
        pump(r, &frame, from, "complete cap=10");
        check(!lineWith(from, "complete cap=10").empty(), "capture 10 completes");
        check(lineWith(from, "scene inspect occluded:").empty(), "a withheld opaque draw buries nothing");
        check(has(lineWith(from, "scene inspect draw: #0 "), "status=top"), "the draw under it stays on top");
        check(has(lineWith(from, "scene inspect frame:"), "1 withheld by EDVR"), "and the frame line counts the withheld draw");
    }

    // Cost: what an idle session pays, per call. The figures are printed (docs\scene-inspector-2026-10-14.md quotes them); the bound is loose, so only an
    // accident (a lock, an allocation, a system call on the idle path) fails it.
    {
        check(!edvr::sceneInspectWantsDraws() && !edvr::sceneInspectInputsWanted(), "after its captures the module is idle again");
        const size_t linesBefore = edvr::g_lines.size();
        LARGE_INTEGER freq{}, t0{}, t1{}, t2{};
        QueryPerformanceFrequency(&freq);
        volatile int sink = 0;
        const int n = 20000000;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < n; ++i) sink += edvr::sceneInspectWantsDraws() ? 1 : 0;
        QueryPerformanceCounter(&t1);
        for (int i = 0; i < n; ++i) sink += edvr::sceneInspectInputsWanted() ? 1 : 0;
        QueryPerformanceCounter(&t2);
        for (int i = 0; i < n / 4; ++i) edvr::sceneInspectFrameBoundary(1);
        LARGE_INTEGER t3{};
        QueryPerformanceCounter(&t3);
        const double ns1 = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart) / n;
        const double ns2 = static_cast<double>(t2.QuadPart - t1.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart) / n;
        const double ns3 = static_cast<double>(t3.QuadPart - t2.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart) / (n / 4);
        std::printf("scene_inspector_test: idle gate: sceneInspectWantsDraws %.2f ns, sceneInspectInputsWanted %.2f ns, sceneInspectFrameBoundary %.2f ns per call (sink %d)\n",
                    ns1, ns2, ns3, static_cast<int>(sink));
        checkf(ns1 < 100.0 && ns2 < 100.0 && ns3 < 100.0, "the idle entry points cost under 100 ns a call (%.1f, %.1f, %.1f)", ns1, ns2, ns3);
        check(edvr::g_lines.size() == linesBefore, "and write nothing");
    }
    edvr::sceneInspectInstall(true, false, "", makeEnv());
    {
        const size_t from = edvr::g_lines.size();
        edvr::sceneInspectRequest("flat");
        check(!lineWith(from, "refused (not VR").empty(), "the flat profile refuses, and says why");
    }

    // S8: the debug layer.
    if (r.g.info) {
        const UINT64 n = r.g.info->GetNumStoredMessages();
        unsigned errors = 0;
        std::string first;
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            r.g.info->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
            if (SUCCEEDED(r.g.info->GetMessage(i, m, &len)) && (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                ++errors;
                if (first.empty()) first = m->pDescription ? m->pDescription : "";
            }
        }
        checkf(errors == 0, "S8: the D3D11 debug layer reported %u error(s); first: %s", errors, first.c_str());
    }
    std::printf("scene_inspector_test: device half on WARP, debug layer %s\n", r.g.debugLayer ? "on" : "off");
    // Clean the temp directory.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_logDir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((g_logDir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(g_logDir.c_str());
}

DWORD WINAPI watchdog(LPVOID) {
    Sleep(120000);
    std::printf("scene_inspector_test: watchdog -- the device half hung\n");
    std::fflush(stdout);
    ExitProcess(3);
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        else if (std::strcmp(argv[i], "--dry-run") == 0) dry = true;
        else if (std::strcmp(argv[i], "--verbose") == 0) edvr::g_verbose = true;
    }
    if (dry) {
        std::printf("scene_inspector_test: dry run -- no device, no files\n");
        return 0;
    }
    if (!selfTest) {
        std::printf("usage: scene_inspector_test --self-test [--verbose] | --dry-run\n");
        return 2;
    }
    CreateThread(nullptr, 0, &watchdog, nullptr, 0, nullptr);
    try {
        runPure();
        std::printf("scene_inspector_test: pure half ok (%u checks)\n", g_checks);
        runDevice();
    } catch (const std::exception& e) {
        std::printf("scene_inspector_test: FAILED: %s\n", e.what());
        const size_t n = edvr::g_lines.size();
        for (size_t i = n > 12 ? n - 12 : 0; i < n; ++i) std::printf("    log: %s\n", edvr::g_lines[i].c_str());
        return 1;
    }
    std::printf("scene_inspector_test: %u checks, 0 failed\n", g_checks);
    return 0;
}
