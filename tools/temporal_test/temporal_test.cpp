// temporal_test -- table tests for the temporal pass's arithmetic
// (src/common/temporal_math.h), without a headset, a game, a device, or
// either DLL: everything under test is header-only.
//
// What a build can pin that a flight cannot cheaply: the jitter sequence
// and the sign of its tangent shift (a jitter told to the game as the wrong
// sign un-jitters the wrong way and wobbles the image by a pixel every
// frame); the pixel-to-direction mapping and its inverse, on a real
// headset's lopsided frustum; the rotation deltas from the runtime's pose
// and from the game's view rows; the whole reprojection walked by hand
// against a known head turn; and the world path's gate on the rows' delta,
// replayed over a dumped event (eye run 050423: a parked camera's zero
// taken as the view's and carried). The shader in
// src/d3d11/temporal_shader_source.h transcribes the same functions.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../src/common/temporal_math.h"
#include "../../src/common/temporal_census.h"
#include "../../src/common/hmd_camera_tag.h"
#include "../../src/common/camera_writes.h"
#include "../../src/common/camera_ipd_pair.h"
#include "../../src/common/camera_jump_guard.h"
#include <chrono>
#include <stdexcept>
#include "../../src/common/temporal_mode.h"
#include "../../src/d3d11/temporal_history.h"

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }

void check(bool got, const char* what) {
    if (got) {
        ok(what);
        return;
    }
    printf("  FAIL  %s\n", what);
    ++g_fails;
}

void checkNear(float got, float want, float tol, const char* what) {
    if (fabsf(got - want) <= tol) {
        ok(what);
        return;
    }
    printf("  FAIL  %s -- got %g, wanted %g (tolerance %g)\n", what, got, want, tol);
    ++g_fails;
}

// A rotation about +Y by theta, as the 3x3 of a row-major 3x4.
void yaw34(float theta, float m34[12]) {
    memset(m34, 0, sizeof(float) * 12);
    const float c = cosf(theta), s = sinf(theta);
    m34[0] = c;  m34[2] = s;
    m34[5] = 1.0f;
    m34[8] = -s; m34[10] = c;
}

// View rows turned `deg` about +Y and standing at (x, y, z).
void rows34(float deg, float x, float y, float z, float m34[12]) {
    yaw34(deg * 0.0174532925f, m34);
    m34[3] = x; m34[7] = y; m34[11] = z;
}

// One eye's pass through the world path's gate, as temporal_pass.cpp runs
// it: the rows' delta, its angle from the head's delta, then the verdict.
// W and tv come back as the delta the pass would use.
edvr::TemporalCameraStep gateEye(edvr::TemporalCameraGate& g, const float prev[12],
                                 const float now[12], const float head[9],
                                 float W[9], float tv[3], float* diffDeg = nullptr) {
    float mv[3], hT[9], diff[9];
    edvr::temporalWorldFromRows(prev, now, W, tv, mv);
    edvr::temporalTranspose3(head, hT);
    edvr::temporalMul3(W, hT, diff);
    const float d = edvr::temporalRotationAngleDeg(diff);
    if (diffDeg) *diffDeg = d;
    const double move = sqrt(static_cast<double>(mv[0]) * mv[0] +
                             static_cast<double>(mv[1]) * mv[1] +
                             static_cast<double>(mv[2]) * mv[2]);
    return edvr::temporalCameraGateStep(g, prev, now, move, d, W, tv);
}

bool same9(const float a[9], const float b[9]) { return memcmp(a, b, sizeof(float) * 9) == 0; }

}  // namespace

int main() {
    for (float scale : {0.5f, 0.65f, 0.999f}) {
        if (strcmp(edvr::temporalNvidiaLabel(scale), "DLSS")) { ++g_fails; puts("FAIL: upscaling label"); }
    }
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        if (strcmp(edvr::temporalNvidiaLabel(scale), "DLAA")) { ++g_fails; puts("FAIL: native or supersampled label"); }
    }
    for (float scale : {0.0f, -1.0f, NAN, INFINITY}) {
        if (strcmp(edvr::temporalNvidiaLabel(scale), "DLSS / DLAA")) { ++g_fails; puts("FAIL: unknown scale label"); }
    }
    for (const char* mode : {"on", "ON", "dlaa", "DLAA", "dlss", "DLSS"}) {
        if (!edvr::temporalModeEnabled(mode)) { ++g_fails; printf("FAIL: bundled temporal mode %s\n", mode); }
    }
    for (const char* mode : {"off", "OFF", "", "bogus"}) {
        if (edvr::temporalModeEnabled(mode)) { ++g_fails; printf("FAIL: inactive temporal mode %s\n", mode); }
    }
    {
        using edvr::temporalCameraFollowScore;
        int score = -30;
        // Recorded yaw/head cancellation: the scene camera barely turns,
        // but its rows predict the stars correctly. Recover on a still
        // head too, rather than waiting indefinitely for another head turn.
        score = temporalCameraFollowScore(score, 1100, true, .25f, .03f);
        check(score == 30, "bound scene camera survives opposing ship/head yaw");
        check(temporalCameraFollowScore(-30, 1100, true, 0, .25f) == 30,
              "bound scene camera recovers with the head stationary");
        for (int i=0;i<16;++i) score = temporalCameraFollowScore(score, 1100, true, .25f, 0);
        check(score == 30, "sustained cancellation cannot disable scene motion");
        check(temporalCameraFollowScore(0, 2, true, .25f, 0) == -4,
              "sparse menu backdrop still uses head-follow detector");
        // The scanner's initial screen in a sparse system: 49 draws (eye
        // dump 182049, 2026-09-16), a scene, not a menu.
        check(temporalCameraFollowScore(-30, 49, true, 0, .25f) == 30,
              "the scanner's sparse screen is a populated scene");
        check(edvr::kTemporalSceneDrawFloor <= 49u && edvr::kTemporalSceneDrawFloor > 2u,
              "the scene floor sits between the menu's draws and the scanner's");
        check(temporalCameraFollowScore(0, 1100, false, .25f, 0) == -4,
              "auxiliary camera chain still triggers resynchronization");
        check(temporalCameraFollowScore(-30, 1100, false, .25f, .25f) == -29,
              "auxiliary camera can regain confidence");
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("edvr temporal test\n\n");

    // ---- The Halton sequence and the jitter drawn from it. ----------------
    {
        using edvr::temporalHalton;
        const float b2[8] = {0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f};
        const float b3[4] = {1.0f / 3, 2.0f / 3, 1.0f / 9, 4.0f / 9};
        bool h2 = true, h3 = true;
        for (uint32_t i = 0; i < 8; ++i) {
            if (fabsf(temporalHalton(i + 1, 2) - b2[i]) > 1e-6f) h2 = false;
        }
        for (uint32_t i = 0; i < 4; ++i) {
            if (fabsf(temporalHalton(i + 1, 3) - b3[i]) > 1e-6f) h3 = false;
        }
        check(h2, "halton: base 2 is 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8, 1/16");
        check(h3, "halton: base 3 is 1/3, 2/3, 1/9, 4/9");

        float jx[edvr::kTemporalJitterCount], jy[edvr::kTemporalJitterCount];
        bool inRange = true, distinct = true, periodic = true;
        for (uint32_t n = 0; n < edvr::kTemporalJitterCount; ++n) {
            edvr::temporalJitter(n, &jx[n], &jy[n]);
            if (jx[n] < -0.5f || jx[n] >= 0.5f || jy[n] < -0.5f || jy[n] >= 0.5f) inRange = false;
            for (uint32_t m = 0; m < n; ++m) {
                if (jx[m] == jx[n] && jy[m] == jy[n]) distinct = false;
            }
            float px, py;
            edvr::temporalJitter(n + edvr::kTemporalJitterCount, &px, &py);
            if (px != jx[n] || py != jy[n]) periodic = false;
        }
        check(inRange, "jitter: every offset lies inside the pixel");
        check(distinct, "jitter: the eight offsets are all different");
        check(periodic, "jitter: the sequence repeats after eight frames");
    }

    // ---- The jitter cycle's length (advanced.temporal_aa_jitter_phases, the flat profile's key). ----
    // Eight is what every path ran before a count could be named; the key's default must change nothing, so the eight offsets are held
    // twice: as a literal table (the Halton (2,3) points at index 1..8, minus half a pixel), and as the formula the code had.
    {
        const float lx[8] = {0.0f, -0.25f, 0.25f, -0.375f, 0.125f, -0.125f, 0.375f, -0.4375f};
        const float ly[8] = {1.0f / 3 - 0.5f, 2.0f / 3 - 0.5f, 1.0f / 9 - 0.5f, 4.0f / 9 - 0.5f,
                             7.0f / 9 - 0.5f, 2.0f / 9 - 0.5f, 5.0f / 9 - 0.5f, 8.0f / 9 - 0.5f};
        bool literal = true, noCountIsOld = true, eightIsOld = true;
        for (uint32_t n = 0; n < 200; ++n) {
            const uint32_t i = (n % 8) + 1;
            const float ex = edvr::temporalHalton(i, 2) - 0.5f, ey = edvr::temporalHalton(i, 3) - 0.5f;
            float nx, ny, ax, ay;
            edvr::temporalJitter(n, &nx, &ny);
            edvr::temporalJitter(n, &ax, &ay, 8);
            if (memcmp(&nx, &ex, sizeof(float)) || memcmp(&ny, &ey, sizeof(float))) noCountIsOld = false;
            if (memcmp(&ax, &ex, sizeof(float)) || memcmp(&ay, &ey, sizeof(float))) eightIsOld = false;
            if (fabsf(nx - lx[n % 8]) > 1e-6f || fabsf(ny - ly[n % 8]) > 1e-6f) literal = false;
        }
        check(literal, "jitter count: with no count the offsets are the shipped eight, as a literal table, for 200 frames");
        check(noCountIsOld, "jitter count: no count draws the formula's bits (index (n mod 8) + 1), so the VR paths are unchanged");
        check(eightIsOld, "jitter count: a count of 8 draws the same bits as no count (the key's default changes nothing)");
        float zx, zy, ex, ey;
        edvr::temporalJitter(3, &zx, &zy, 0);
        edvr::temporalJitter(3, &ex, &ey);
        check(zx == ex && zy == ey, "jitter count: a count of zero reads as the default eight, not a division by zero");
    }
    {
        const uint32_t counts[] = {8, 9, 12, 16, 24, 32, 48, 63, 64};
        for (const uint32_t count : counts) {
            float x[64], y[64];
            bool inRange = true, distinct = true, periodic = true, extendsEight = true;
            for (uint32_t n = 0; n < count; ++n) {
                edvr::temporalJitter(n, &x[n], &y[n], count);
                if (x[n] < -0.5f || x[n] >= 0.5f || y[n] < -0.5f || y[n] >= 0.5f) inRange = false;
                for (uint32_t m = 0; m < n; ++m)
                    if (x[m] == x[n] && y[m] == y[n]) distinct = false;
                for (uint32_t round = 1; round <= 3; ++round) {
                    float px, py;
                    edvr::temporalJitter(n + round * count, &px, &py, count);
                    if (px != x[n] || py != y[n]) periodic = false;
                }
                if (n < 8) {
                    float fx, fy;
                    edvr::temporalJitter(n, &fx, &fy);
                    if (fx != x[n] || fy != y[n]) extendsEight = false;
                }
            }
            char what[160];
            snprintf(what, sizeof(what), "jitter count %u: every offset lies inside the pixel, [-0.5, 0.5)", count);
            check(inRange, what);
            snprintf(what, sizeof(what), "jitter count %u: the %u offsets of one cycle are all different", count, count);
            check(distinct, what);
            snprintf(what, sizeof(what), "jitter count %u: the sequence repeats with period %u (three more cycles, bit for bit)", count, count);
            check(periodic, what);
            snprintf(what, sizeof(what), "jitter count %u: its first eight offsets are the shipped eight's", count);
            check(extendsEight, what);
        }
        // Where a longer cycle departs from the eight: its ninth frame (n = 8) is the ninth Halton point, (1/16, 1/27 - 1/2) = (0.0625,
        // -0.463), where the eight starts over at its first (0, 1/3 - 1/2). That y is past 7/16 of a pixel, which is why the legacy route's
        // lighting patch cannot take a longer cycle (flat_lighting_tests.h; flatCameraPhaseCount).
        float nine[2], again[2];
        edvr::temporalJitter(8, &nine[0], &nine[1], 16);
        edvr::temporalJitter(8, &again[0], &again[1], 8);
        checkNear(nine[0], 0.0625f, 1e-6f, "jitter count 16: frame 8 is the ninth Halton point, x");
        checkNear(nine[1], 1.0f / 27 - 0.5f, 1e-6f, "jitter count 16: frame 8 is the ninth Halton point, y (-0.463)");
        check(again[0] == 0.0f && fabsf(again[1] - (1.0f / 3 - 0.5f)) < 1e-6f, "jitter count 8: frame 8 starts the cycle over at its first point");
    }
    {
        // What the key's value asks for: a whole number from 8 to 64 as written; anything else is the default, not the nearest edge.
        const long accepted[] = {8, 9, 16, 32, 48, 63, 64};
        bool acceptedOk = true, flagged = true;
        for (const long value : accepted) {
            bool usable = false;
            if (edvr::temporalJitterPhases(value, &usable) != static_cast<uint32_t>(value) || !usable) acceptedOk = false;
        }
        check(acceptedOk, "jitter phases: 8, 9, 16, 32, 48, 63 and 64 are read as written, both edges included");
        const long refused[] = {7, 6, 1, 0, -1, -8, -64, 65, 100, 128, 1000000, 2147483647L, -2147483647L - 1};
        bool fallsBack = true;
        for (const long value : refused) {
            bool usable = true;
            if (edvr::temporalJitterPhases(value, &usable) != 8u || usable) { fallsBack = false; flagged = false; }
        }
        check(fallsBack, "jitter phases: 7, 0, a negative, 65, 100 and the extremes of an int all read as 8 (the default, not the nearest edge)");
        check(flagged && edvr::temporalJitterPhases(100) == 8u, "jitter phases: the caller is told a value was not usable, and the pointer is optional");
        check(edvr::kTemporalJitterPhasesMin == 8u && edvr::kTemporalJitterPhasesMax == 64u && edvr::kTemporalJitterCount == 8u,
              "jitter phases: the range is 8 to 64 and the default is the fixed eight");
    }

    // ---- The jitter as a tangent shift: the sign, pinned. -----------------
    {
        const float tan[4] = {-1.0f, 1.0f, -1.0f, 1.0f};   // a square 90-degree frustum
        const uint32_t w = 1000, h = 1000;
        float dx = 0.0f, dy = 0.0f;
        edvr::temporalJitterToTangents(0.5f, 0.5f, tan, w, h, &dx, &dy);
        checkNear(dx, -0.001f, 1e-7f, "jitter shift: +0.5 px right is l and r moved by -(r-l)/2w");
        checkNear(dy, 0.001f, 1e-7f, "jitter shift: +0.5 px down is t and b moved by +(b-t)/2h");
        // A fixed direction straight ahead lands on the centre pixel
        // through the unshifted frustum and half a pixel right and down
        // through the shifted one: the content moved by the jitter.
        const float ahead[3] = {0.0f, 0.0f, -1.0f};
        float px0 = 0.0f, py0 = 0.0f, px1 = 0.0f, py1 = 0.0f;
        const float shifted[4] = {tan[0] + dx, tan[1] + dx, tan[2] + dy, tan[3] + dy};
        check(edvr::temporalDirToPixel(ahead, tan, w, h, &px0, &py0) &&
                  edvr::temporalDirToPixel(ahead, shifted, w, h, &px1, &py1),
              "jitter shift: straight ahead projects through both frusta");
        checkNear(px0, 499.5f, 1e-3f, "jitter shift: unshifted, straight ahead is the centre pixel");
        checkNear(px1 - px0, 0.5f, 1e-3f, "jitter shift: the content moved right by the jitter");
        checkNear(py1 - py0, 0.5f, 1e-3f, "jitter shift: ...and down by the jitter");
    }

    // ---- Pixel <-> direction, on the Quest 3's lopsided frustum. ----------
    {
        const float tan[4] = {-1.3764f, 0.8391f, -1.4281f, 0.9657f};
        const uint32_t w = 3096, h = 3312;
        bool roundTrip = true;
        const float probes[5][2] = {{0, 0}, {10.25f, 3300.5f}, {1547.5f, 1655.5f}, {3095, 3311}, {700, 40}};
        for (const float* pr : probes) {
            float d[3], px, py;
            edvr::temporalPixelToDir(pr[0], pr[1], tan, w, h, d);
            if (!edvr::temporalDirToPixel(d, tan, w, h, &px, &py)) roundTrip = false;
            if (fabsf(px - pr[0]) > 1e-2f || fabsf(py - pr[1]) > 1e-2f) roundTrip = false;
        }
        check(roundTrip, "mapping: pixel -> direction -> pixel is the identity across the image");
        // Row 0 looks along the b tangent (up, +0.9657) and the last row
        // along t (down, -1.4281): the guard's field-verified orientation.
        float top[3], bottom[3];
        edvr::temporalPixelToDir(1547.5f, -0.5f, tan, w, h, top);
        edvr::temporalPixelToDir(1547.5f, 3311.5f, tan, w, h, bottom);
        checkNear(top[1], 0.9657f, 1e-4f, "mapping: the top edge looks along b, upward");
        checkNear(bottom[1], -1.4281f, 1e-4f, "mapping: the bottom edge looks along t, downward");
        float d[3];
        d[0] = 0.0f; d[1] = 0.0f; d[2] = 1.0f;   // behind the eye
        float px, py;
        check(!edvr::temporalDirToPixel(d, tan, w, h, &px, &py),
              "mapping: a direction behind the eye has no pixel");
    }

    // ---- The rotation deltas. ---------------------------------------------
    {
        float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        float turned[12];
        const float theta = 1.0f * 3.14159265f / 180.0f;   // one degree
        yaw34(theta, turned);
        check(edvr::temporalRowsAreRotation(ident) && edvr::temporalRowsAreRotation(turned),
              "rotation test: a rotation is one");
        float scaled[12];
        memcpy(scaled, ident, sizeof(scaled));
        scaled[0] = 2.0f;
        float zeros[12] = {};
        check(!edvr::temporalRowsAreRotation(scaled) && !edvr::temporalRowsAreRotation(zeros),
              "rotation test: a scaled or empty matrix is not");

        // The head: identity last frame, one degree of yaw now. The delta
        // takes this frame's directions to last frame's, so straight ahead
        // now is one degree of yaw away from straight ahead then.
        float delta[9];
        edvr::temporalHeadDelta(ident, turned, delta);
        const float ahead[3] = {0.0f, 0.0f, -1.0f};
        float then[3];
        edvr::temporalApply3(delta, ahead, then);
        checkNear(then[0], -sinf(theta), 1e-6f, "head delta: straight ahead now, seen last frame, is turned by the yaw");
        checkNear(then[2], -cosf(theta), 1e-6f, "head delta: ...and still nearly straight ahead");
        // The same turn, undone: last frame's directions through the
        // inverse delta come back.
        float back[9], again[3];
        edvr::temporalHeadDelta(turned, ident, back);
        edvr::temporalApply3(back, then, again);
        checkNear(again[0], 0.0f, 1e-6f, "head delta: the reverse delta undoes it");
        checkNear(again[2], -1.0f, 1e-6f, "head delta: ...exactly");
        // No motion, no delta.
        float none[9];
        edvr::temporalHeadDelta(turned, turned, none);
        bool identity = true;
        for (int i = 0; i < 9; ++i) {
            if (fabsf(none[i] - ((i % 4 == 0) ? 1.0f : 0.0f)) > 1e-6f) identity = false;
        }
        check(identity, "head delta: the same pose twice is the identity");
        // The turn's size, for the registration instrument's speed
        // buckets: one degree reads as one degree, no turn as none.
        checkNear(edvr::temporalRotationAngleDeg(delta), 1.0f, 1e-3f,
                  "rotation angle: a one-degree yaw measures one degree");
        checkNear(edvr::temporalRotationAngleDeg(none), 0.0f, 1e-3f,
                  "rotation angle: no turn measures zero");

        // The translation term. A head that steps 10 cm to the right, no
        // turn: a point 1 m ahead was, last frame, 10 cm further LEFT in
        // eye space (the world moved the other way), whichever eye.
        {
            float stepped[12];
            memcpy(stepped, ident, sizeof(stepped));
            stepped[3] = 0.10f;   // +x translation, row-major 3x4
            const float eyeOff[3] = {-0.032f, 0.0f, 0.0f};
            float tv[3];
            edvr::temporalHeadTranslation(ident, stepped, eyeOff, tv);
            checkNear(tv[0], 0.10f, 1e-6f, "translation: a 10 cm step right moves last frame's view 10 cm along x");
            checkNear(tv[1], 0.0f, 1e-6f, "translation: ...and nothing along y");
            checkNear(tv[2], 0.0f, 1e-6f, "translation: ...or z");
            // The same step seen through a one-degree yaw: the eye's own
            // offset enters through the turn, by (delta - I) e.
            float tv2[3];
            edvr::temporalHeadTranslation(ident, turned, eyeOff, tv2);
            float de[3];
            edvr::temporalApply3(delta, eyeOff, de);
            checkNear(tv2[0], de[0] - eyeOff[0], 1e-6f, "translation: a pure turn moves the eye by (delta - I) e, x");
            checkNear(tv2[2], de[2] - eyeOff[2], 1e-6f, "translation: ...z");
        }
        // Reversed-Z to metres, with the game's planes: the values the
        // cockpit census read (2026-09-03) land where a cockpit is.
        checkNear(edvr::temporalDepthToMetres(0.02007425f, 0.025f, 50000.0f), 1.245f, 0.01f,
                  "depth: 0.0201 reads as 1.25 m with near 0.025 m");
        checkNear(edvr::temporalDepthToMetres(0.00184340f, 0.025f, 50000.0f), 13.56f, 0.05f,
                  "depth: 0.00184 reads as 13.6 m");
        check(edvr::temporalDepthToMetres(0.0f, 0.025f, 50000.0f) > 40000.0f,
              "depth: the far plane reads as the far distance");

        // The game's camera: the two readings of the rows differ by a
        // transpose, and the transposed reading is the other's inverse.
        float dv[9], dvT[9], prod[9];
        edvr::temporalViewDelta(turned, ident, false, dv);
        edvr::temporalViewDelta(turned, ident, true, dvT);
        edvr::temporalMul3(dv, dvT, prod);
        bool inverse = true;
        for (int i = 0; i < 9; ++i) {
            if (fabsf(prod[i] - ((i % 4 == 0) ? 1.0f : 0.0f)) > 1e-5f) inverse = false;
        }
        check(inverse, "view delta: the transposed reading is the inverse of the plain one");
    }

    // ---- The reprojection as a whole, by hand. ----------------------------
    {
        const float tan[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
        const uint32_t w = 1000, h = 1000;
        float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        float turned[12];
        const float theta = 1.0f * 3.14159265f / 180.0f;
        yaw34(theta, turned);
        float delta[9];
        edvr::temporalHeadDelta(ident, turned, delta);
        // The centre pixel now was 500 tan(1 deg) = 8.73 pixels to the
        // left last frame (the head turned toward -X, so what is ahead now
        // was to the left of ahead then... and the image records that as a
        // smaller column). The number is the frustum's pixels per unit
        // tangent times the tangent of the turn.
        float ppx, ppy;
        check(edvr::temporalReproject(499.5f, 499.5f, tan, tan, delta, w, h, &ppx, &ppy),
              "reproject: the centre pixel lands on the image after a one-degree turn");
        checkNear(ppx, 499.5f - 500.0f * tanf(theta), 1e-2f,
                  "reproject: ...8.73 pixels along the row, the frustum's scale times tan(1 deg)");
        checkNear(ppy, 499.5f, 1e-3f, "reproject: ...and on the same row");
        // The identity delta is the identity map.
        float none[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        check(edvr::temporalReproject(123.0f, 456.0f, tan, tan, none, w, h, &ppx, &ppy) &&
                  fabsf(ppx - 123.0f) < 1e-3f && fabsf(ppy - 456.0f) < 1e-3f,
              "reproject: no motion maps every pixel to itself");
        // A turn large enough to carry the edge off the image says so.
        float big[12];
        yaw34(60.0f * 3.14159265f / 180.0f, big);
        edvr::temporalHeadDelta(ident, big, delta);
        check(!edvr::temporalReproject(0.0f, 499.5f, tan, tan, delta, w, h, &ppx, &ppy),
              "reproject: a pixel that lands off the image is refused");
        // Under the guard the two frames' frusta can differ (a re-stage);
        // the same direction through a wider previous frustum lands
        // nearer the centre.
        const float wide[4] = {-1.2f, 1.2f, -1.0f, 1.0f};
        check(edvr::temporalReproject(999.0f, 499.5f, tan, wide, none, w, h, &ppx, &ppy) &&
                  ppx < 999.0f && ppx > 900.0f,
              "reproject: a wider previous frustum pulls the same direction inward");
    }

    {
        float a=0, b=0;
        check(!edvr::temporalSceneProjection(0,0,.025f,&a,&b), "missing scene row uses the scene fallback");
        const float distances[] = {30.0f,3000.0f,15000.0f,25000.0f};
        for (float metres : distances) {
            const float captured=.025f/metres;
            checkNear(b/(captured-a),metres,.005f,"missing projection still reconstructs actual station distance");
        }
        check(edvr::temporalSceneProjection(0,.025f,.025f,&a,&b),"measured infinite scene row accepted");
        check(!edvr::temporalSceneProjection(1,2,.025f,&a,&b),"unrelated projection cannot create near-plane HUD depth");
        checkNear(a,0,0,"invalid projection leaves infinite scene offset");
        checkNear(b,.025f,0,"invalid projection retains scene near scale");
        const float fa=.025f/(.025f-50000.0f),fb=.025f*50000.0f/(50000.0f-.025f);
        check(edvr::temporalSceneProjection(fa,fb,.025f,&a,&b),"explicit measured finite row is respected");
        checkNear(a,fa,0,"measured finite offset retained");
    }
    {
        edvr::TemporalHistory<3> history;
        check(history.size()==0,"temporal history starts explicitly empty");
        edvr::TemporalHistoryEntry entry{};
        for(uint32_t frame=1;frame<=5;++frame) {
            entry.frame=frame;entry.flags=frame==4?33u:2u;entry.output=frame==5?0u:2u;
            entry.selectedSeq=100u+frame;entry.drawSeq=200u+frame;
            entry.cameraChoiceFlags=frame;entry.cameraDrawFlags=frame<<4;
            entry.selectedRows[3]=float(frame)+0.25f;entry.drawRows[11]=-float(frame);
            entry.events=frame==4?17u:8u;history.record(entry);
        }
        check(history.size()==3 && history.oldest(0).frame==3 && history.oldest(2).frame==5,
              "bounded temporal history retains chronological entries after wrap");
        check(history.oldest(1).flags==33 && history.oldest(1).events==17 && history.oldest(2).output==0,
              "reset requests and failed output survive alongside successful calls");
        check(history.oldest(0).selectedSeq==103 && history.oldest(2).drawSeq==205 &&
              history.oldest(1).cameraChoiceFlags==4 && history.oldest(1).cameraDrawFlags==64 &&
              history.oldest(0).selectedRows[3]==3.25f && history.oldest(2).drawRows[11]==-5.0f,
              "camera provenance and full selected/draw rows survive history wrap");
        history.clear();check(history.size()==0,"cleared temporal history cannot report stale success");
        entry.frame=9;history.record(entry);
        check(history.size()==1 && history.oldest(0).frame==9,"history restarts at its first new entry");
    }

    // ---- The world path's gate over a parked camera (eye run 050423). -----
    // Eye 0's rows and head deltas from eye_050423_motion.csv (build
    // c9cab91e, 2026-09-25 05:04, the ship rolling): frames 19994..19999 are
    // real, real, an auxiliary pass parked 148 deg from the view, the same
    // pass's identical write again, real, real. The gate before 2026-09-25
    // accepted 19997's zero as the view's (a still head, well inside the 3
    // degrees) and 19998 carried it: two frames of the world standing still
    // under the roll. docs/camera-rows-carry-2026-09-25.md.
    {
        using edvr::TemporalCameraVerdict;
        using edvr::temporalRotationAngleDeg;
        const float r93[12] = {0.780769169f, 0.194406167f, -0.593806207f, -198.458389f,
                               0.423516452f, 0.534070432f, 0.731712043f, 1471.88684f,
                               0.459383726f, -0.822784841f, 0.33465144f, 767.823914f};
        const float r94[12] = {0.778171837f, 0.206665993f, -0.593074799f, -198.482376f,
                               0.418339074f, 0.53377068f, 0.734902203f, 1472.50903f,
                               0.468445241f, -0.819986522f, 0.32890889f, 768.194946f};
        const float r95[12] = {0.773163915f, 0.220272169f, -0.594724894f, -198.508804f,
                               0.41296041f, 0.536850452f, 0.735700428f, 1473.16528f,
                               0.48133266f, -0.814414859f, 0.324109703f, 768.586121f};
        const float aux[12] = {-0.950436831f, 0.297400296f, 0.0906804204f, -199.47821f,
                               0.0727622211f, 0.496309847f, -0.865090966f, 1491.63611f,
                               -0.302283823f, -0.815616131f, -0.493350685f, 779.845398f};
        const float r98[12] = {0.598011613f, 0.622053683f, -0.505402088f, -199.778397f,
                               0.234719515f, 0.467010468f, 0.852530301f, 1498.0531f,
                               0.766347706f, -0.628450692f, 0.133269534f, 783.657043f};
        const float r99[12] = {0.582894862f, 0.642890394f, -0.496915996f, -199.863831f,
                               0.225240201f, 0.459744751f, 0.859011889f, 1499.52124f,
                               0.780705154f, -0.61263907f, 0.12317808f, 784.564819f};
        const float h94[9] = {0.999999762f, 0.000184280798f, 0.00026550889f,
                              -0.000184312463f, 0.99999994f, 4.96953726e-05f,
                              -0.000265479088f, -4.97214496e-05f, 0.999999881f};
        const float h95[9] = {0.999989986f, 0.00352385081f, 0.00276330113f,
                              -0.00353198126f, 0.99998939f, 0.00295353308f,
                              -0.00275287032f, -0.00296327844f, 0.999991775f};
        const float h96[9] = {0.999999285f, 0.000695446506f, 0.000980585814f,
                              -0.000695226714f, 0.999999762f, -0.000203188509f,
                              -0.000980764627f, 0.00020249933f, 0.999999523f};
        const float h97[9] = {0.999999881f, 9.8310411e-05f, 0.00028476119f,
                              -9.83420759e-05f, 1.0f, 1.80006027e-05f,
                              -0.000284701586f, -1.80006027e-05f, 0.999999881f};
        const float h98[9] = {0.999999762f, -0.000324182212f, 0.00046429038f,
                              0.000324141234f, 0.99999994f, -5.66542149e-05f,
                              -0.000464260578f, 5.68702817e-05f, 0.999999762f};
        const float h99[9] = {0.999999881f, -0.00010949932f, 0.000200748444f,
                              0.000109475106f, 0.99999994f, -3.79234552e-05f,
                              -0.000200778246f, 3.79942358e-05f, 1.0f};
        // What the DLL wrote for 19995 and 19999 (cameraR, cameraTv): the
        // header's arithmetic must be the pass's to the dump's precision.
        const float cam95[9] = {0.99988991f, 0.0144863427f, 0.00319825113f,
                                -0.0144734383f, 0.99988699f, -0.00402030349f,
                                -0.00325605273f, 0.00397354364f, 0.999986768f};
        const float tv95[3] = {0.437213093f, 0.0240675509f, -0.626614213f};
        const float cam99[9] = {0.99973774f, 0.0228724182f, 0.00113745779f,
                                -0.0228532553f, 0.999631405f, -0.0146477595f,
                                -0.00147202611f, 0.0146179274f, 0.999891937f};
        struct Frame { const float* prev; const float* now; const float* head; };
        const Frame run[6] = {{r93, r94, h94}, {r94, r95, h95}, {r95, aux, h96},
                              {aux, aux, h97}, {aux, r98, h98}, {r98, r99, h99}};
        edvr::TemporalCameraGate gate;
        float ownW[6][9], ownTv[6][3], usedW[6][2][9], usedTv[6][2][3], diffDeg[6];
        edvr::TemporalCameraStep step[6][2];
        uint32_t stay[6];
        for (int f = 0; f < 6; ++f) {
            float mv[3];
            edvr::temporalWorldFromRows(run[f].prev, run[f].now, ownW[f], ownTv[f], mv);
            for (int eye = 0; eye < 2; ++eye) {
                step[f][eye] = gateEye(gate, run[f].prev, run[f].now, run[f].head,
                                       usedW[f][eye], usedTv[f][eye], &diffDeg[f]);
            }
            edvr::temporalCameraGateAdvance(gate, true);
            stay[f] = gate.parkedRun;
        }
        float worst = 0.0f;
        for (int i = 0; i < 9; ++i) {
            worst = fmaxf(worst, fabsf(ownW[1][i] - cam95[i]));
            worst = fmaxf(worst, fabsf(ownW[5][i] - cam99[i]));
        }
        for (int i = 0; i < 3; ++i) worst = fmaxf(worst, fabsf(ownTv[1][i] - tv95[i]));
        checkNear(worst, 0.0f, 1e-5f, "050423: the rows' delta is the pass's own (19995 and 19999 as dumped)");
        check(step[1][0].verdict == TemporalCameraVerdict::Own &&
              step[1][1].verdict == TemporalCameraVerdict::Own,
              "050423 19995: the view's delta is accepted");
        check(step[2][0].verdict == TemporalCameraVerdict::Another &&
              step[2][0].carried && same9(usedW[2][0], ownW[1]),
              "050423 19996: the parked pass, 148 deg away, is dropped and 19995's delta carried");
        check(temporalRotationAngleDeg(ownW[3]) < 0.001f && diffDeg[3] <= 3.0f,
              "050423 19997: the parked pass's own delta is a zero the 3-degree test alone passes");
        check(step[3][0].verdict == TemporalCameraVerdict::Parked &&
              step[3][1].verdict == TemporalCameraVerdict::Parked,
              "050423 19997: a zero from rows a drop left behind is refused, by both eyes");
        check(step[3][0].carried && same9(usedW[3][0], ownW[1]) && same9(usedW[3][1], ownW[1]) &&
              memcmp(usedTv[3][0], ownTv[1], sizeof(float) * 3) == 0,
              "050423 19997: the world delta is the carried real one, not zero");
        check(step[4][0].verdict == TemporalCameraVerdict::Another && step[4][0].carried &&
              same9(usedW[4][0], ownW[1]) && temporalRotationAngleDeg(usedW[4][0]) > 0.5f,
              "050423 19998: back on the view, the carried delta is 19995's roll, not the zero");
        check(step[5][0].verdict == TemporalCameraVerdict::Own && !step[5][0].carried &&
              same9(usedW[5][0], ownW[5]) && same9(gate.lastGoodC, ownW[5]),
              "050423 19999: the view's turning delta is accepted at once, as before");
        check(gate.refOwn, "050423: the rows are the view's own again after 19999");
        check(stay[2] == 0 && stay[3] == 1 && stay[4] == 0,
              "050423: the stay is one parked frame, counted once for both eyes");
    }
    // ---- The same gate on the cases the fix must not disturb. -------------
    {
        using edvr::TemporalCameraVerdict;
        using edvr::temporalRotationAngleDeg;
        const float still[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float a0[12], a1[12], a2[12], a3[12], p[12], pMoved[12], b5[12], b6[12];
        rows34(0.0f, 0, 0, 0, a0);
        rows34(1.0f, 0, 0, 1, a1);
        rows34(15.0f, 0, 0, 10, a2);     // a hitch: a real jump of 14 degrees
        rows34(16.5f, 0, 0, 11, a3);
        rows34(150.0f, 5, 0, 5, p);      // a parked pass
        rows34(150.0f, 5, 0, 6, pMoved); // ...re-parked a metre on, the same turn
        rows34(4.0f, 0, 0, 3, b5);
        rows34(5.5f, 0, 0, 4, b6);
        float W[9], tv[3], d1[9], d1tv[3];
        // A hitch: the view jumps 14 degrees (dropped), then turns on; the
        // turning frame is accepted at once -- recovery costs no frame.
        edvr::TemporalCameraGate g;
        gateEye(g, a0, a1, still, d1, d1tv); edvr::temporalCameraGateAdvance(g, true);
        const auto hitch = gateEye(g, a1, a2, still, W, tv); edvr::temporalCameraGateAdvance(g, true);
        const auto after = gateEye(g, a2, a3, still, W, tv); edvr::temporalCameraGateAdvance(g, true);
        check(hitch.verdict == TemporalCameraVerdict::Another && hitch.carried,
              "a real 14-degree hitch is dropped and the last delta carried");
        check(after.verdict == TemporalCameraVerdict::Own && !after.carried &&
              fabsf(temporalRotationAngleDeg(W) - 1.5f) < 0.01f,
              "the frame after a hitch is accepted on its own turning delta");
        // The view that truly stood still (the chooser's twin fallback) is
        // still the view's own when the rows before it were.
        edvr::TemporalCameraGate s;
        gateEye(s, a0, a1, still, W, tv); edvr::temporalCameraGateAdvance(s, true);
        const auto twin = gateEye(s, a1, a1, still, W, tv);
        check(twin.verdict == TemporalCameraVerdict::Own && temporalRotationAngleDeg(W) < 0.001f,
              "a still twin after the view's own rows is accepted as the identity");
        // A longer stay: every frame on the parked pass, and the exit, carry
        // the view's last delta; a re-parked pass (moved, not turned) too.
        edvr::TemporalCameraGate c;
        gateEye(c, a0, a1, still, d1, d1tv); edvr::temporalCameraGateAdvance(c, true);
        bool allCarried = true;
        uint32_t longest = 0;
        const float* chain[6][2] = {{a1, p}, {p, p}, {p, p}, {p, pMoved}, {pMoved, pMoved}, {pMoved, b5}};
        for (auto& link : chain) {
            const auto r = gateEye(c, link[0], link[1], still, W, tv);
            allCarried = allCarried && r.verdict != TemporalCameraVerdict::Own && r.carried &&
                         same9(W, d1) && memcmp(tv, d1tv, sizeof(tv)) == 0;
            edvr::temporalCameraGateAdvance(c, true);
            if (c.parkedRun > longest) longest = c.parkedRun;
        }
        check(allCarried, "a parked stay of five frames, re-parked once, carries the view's delta throughout");
        check(longest == 4 && c.parkedRun == 0,
              "the stay's length is counted (four parked frames) and ends with it");
        const auto back = gateEye(c, b5, b6, still, W, tv);
        check(back.verdict == TemporalCameraVerdict::Own && same9(c.lastGoodC, W),
              "the view's first turning delta after the stay is accepted");
        // The standing moves on once a frame and needs both eyes: within a
        // frame the second eye judges against the same standing as the
        // first, one eye's drop leaves the rows suspect for the next frame,
        // and rows no delta reached (a frame with no write before them) are
        // not the view's.
        edvr::TemporalCameraGate e;
        float mvs[3];
        gateEye(e, a0, a1, still, W, tv); edvr::temporalCameraGateAdvance(e, true);
        edvr::temporalWorldFromRows(a1, a1, W, tv, mvs);
        const auto eye0 = edvr::temporalCameraGateStep(e, a1, a1, 0.0, 3.5f, W, tv);
        edvr::temporalWorldFromRows(a1, a1, W, tv, mvs);
        const auto eye1 = edvr::temporalCameraGateStep(e, a1, a1, 0.0, 2.5f, W, tv);
        check(eye0.verdict == TemporalCameraVerdict::Another &&
              eye1.verdict == TemporalCameraVerdict::Own,
              "the second eye judges a frame against the same standing as the first");
        edvr::temporalCameraGateAdvance(e, true);
        edvr::temporalWorldFromRows(a1, a1, W, tv, mvs);
        const auto next = edvr::temporalCameraGateStep(e, a1, a1, 0.0, 0.0f, W, tv);
        check(!e.refOwn && next.verdict == TemporalCameraVerdict::Parked,
              "one eye's drop leaves the rows suspect for the next frame");
        edvr::TemporalCameraGate n;
        n.refOwn = true;
        edvr::temporalCameraGateAdvance(n, true);
        check(!n.refOwn, "rows no delta was measured to are not known to be the view's");
        n.refOwn = true;
        n.measured = true;
        edvr::temporalCameraGateAdvance(n, false);
        check(!n.refOwn, "a frame with no rows leaves no reference");
        // The floating origin: a jump's translation is dropped before the
        // last-good store, so a later drop never carries it.
        edvr::TemporalCameraGate j;
        float far1[12];
        rows34(2.0f, 0, 0, 5001, far1);
        gateEye(j, a0, a1, still, d1, d1tv); edvr::temporalCameraGateAdvance(j, true);
        const auto jump = gateEye(j, a1, far1, still, W, tv); edvr::temporalCameraGateAdvance(j, true);
        check(jump.jump && jump.verdict == TemporalCameraVerdict::Own &&
              tv[0] == 0.0f && tv[1] == 0.0f && tv[2] == 0.0f &&
              memcmp(j.lastGoodTv, d1tv, sizeof(d1tv)) == 0 && same9(j.lastGoodC, W),
              "a jump keeps its turn as the last good and the translation before it");
        const auto dropped = gateEye(j, far1, p, still, W, tv);
        check(dropped.carried && !dropped.carriedJump && memcmp(tv, d1tv, sizeof(tv)) == 0,
              "a drop after a jump carries the translation before it (a jump carried: zero)");
    }
    // THE PER-DRAW CENSUS (diag.temporal_draws, temporal_census.h): N recognised draws give N records with the rows and
    // the projection floats of each; the per-eye-frame cap holds; a frame slot is reused by its frame number; and with the
    // key off a draw does no work at all.
    {
        static edvr::census::Ring ring, offRing;
        float rows[12], blk[16];
        rows34(90.0f, 1.0f, 2.0f, 3.0f, rows);
        for (int j = 0; j < 16; ++j) blk[j] = static_cast<float>(j) + 0.5f;
        // A. Key off: nothing is stored or counted.
        {
            edvr::census::Draw d;
            edvr::census::fillDraw(d, 0x1234u, 6u, rows, true, 9u, blk);
            uint64_t calls = 0;
            const bool stored = edvr::census::noteDraw(offRing, false, 7u, 0, d, &calls);
            check(!stored && calls == 0 && offRing.frames[7].frame == ~0u && offRing.frames[7].drawsSeen[0] == 0,
                  "census key off: a recognised draw stores nothing and counts nothing");
        }
        // B. Five draws on eye 0 and three on eye 1 in frame 7: N records, each with its own vs and index count, and the
        // rows and projection floats of the object's write at the draw.
        {
            uint64_t calls = 0;
            for (uint32_t k = 0; k < 5; ++k) {
                edvr::census::Draw d;
                edvr::census::fillDraw(d, 1000u + k, 100u + k, rows, true, 42u, blk);
                edvr::census::noteDraw(ring, true, 7u, 0, d, &calls);
            }
            for (uint32_t k = 0; k < 3; ++k) {
                edvr::census::Draw d;
                edvr::census::fillDraw(d, 2000u + k, 200u + k, rows, true, 42u, blk);
                edvr::census::noteDraw(ring, true, 7u, 1, d, &calls);
            }
            const edvr::census::Frame& f = ring.frames[7];
            check(f.frame == 7u && f.drawsSeen[0] == 5u && f.drawsSeen[1] == 3u && calls == 8u,
                  "census: 5 draws on eye 0 and 3 on eye 1 are counted per eye in their frame, 8 in all");
            bool idsOk = true;
            for (uint32_t k = 0; k < 5; ++k) idsOk = idsOk && f.draws[0][k].vs == 1000u + k && f.draws[0][k].indexCount == 100u + k;
            for (uint32_t k = 0; k < 3; ++k) idsOk = idsOk && f.draws[1][k].vs == 2000u + k && f.draws[1][k].indexCount == 200u + k;
            check(idsOk, "census: each record keeps its own VS hash and index count, in draw order, per eye");
            const edvr::census::Draw& d0 = f.draws[0][0];
            checkNear(d0.T[0], 1.0f, 1e-6f, "census: the record's translation x is the rows' (1)");
            checkNear(d0.T[1], 2.0f, 1e-6f, "census: the record's translation y is the rows' (2)");
            checkNear(d0.T[2], 3.0f, 1e-6f, "census: the record's translation z is the rows' (3)");
            // Yaw 90 deg about +Y: the quaternion (w, x, y, z) is (sqrt(1/2), 0, sqrt(1/2), 0).
            checkNear(d0.R[0], 0.70710678f, 1e-5f, "census: the rotation quaternion w of a 90 deg yaw");
            checkNear(d0.R[2], 0.70710678f, 1e-5f, "census: the rotation quaternion y of a 90 deg yaw");
            checkNear(d0.R[1] + d0.R[3], 0.0f, 1e-5f, "census: the quaternion x and z vanish for a yaw");
            bool pOk = true;
            for (int j = 0; j < 16; ++j) pOk = pOk && d0.P[j] == static_cast<float>(j) + 0.5f;
            check(pOk && d0.hasRows && d0.seq == 42u, "census: the record keeps the projection floats 784-799 and the write clock");
            // A draw with no write this frame: no rows, no translation.
            edvr::census::Draw none;
            edvr::census::fillDraw(none, 9u, 1u, nullptr, false, 0u, nullptr);
            check(!none.hasRows && none.T[0] == 0.0f && none.seq == 0u && none.P[0] == 0.0f,
                  "census: a draw whose object had no write this frame keeps no rows");
            // The line: one TDRW record in the dump's format.
            char line[640];
            const int len = edvr::census::formatDraw(line, sizeof(line), 7u, 1, 2u, f.draws[1][2]);
            int commas = 0;
            const char* pp = std::strstr(line, "P=(");
            if (pp) for (const char* c = pp; *c; ++c) if (*c == ',') ++commas;
            check(len > 0 && std::strncmp(line, "TDRW f7 eye=1 i=2 vs=00000000000007D2 n=202 seq=42 rows=1 ", 58) == 0 &&
                  pp && commas == 15 && line[len - 1] == ')',
                  "census: a TDRW line carries frame, eye, index, vs, n, seq, rows, T, R and 16 P floats");
            check(edvr::census::formatDraw(line, 40, 7u, 1, 2u, f.draws[1][2]) == 0,
                  "census: a record that does not fit its buffer is refused, not truncated");
        }
        // C. The cap: 40 draws on eye 0 in frame 9 keep the first 32 and count all 40.
        {
            uint64_t calls = 0;
            for (uint32_t k = 0; k < 40; ++k) {
                edvr::census::Draw d;
                edvr::census::fillDraw(d, 3000u + k, 1u, rows, true, 1u, blk);
                edvr::census::noteDraw(ring, true, 9u, 0, d, &calls);
            }
            const edvr::census::Frame& f = ring.frames[9];
            check(f.drawsSeen[0] == 40u && calls == 40u && f.draws[0][31].vs == 3031u &&
                  f.draws[0][0].vs == 3000u,
                  "census cap: 40 draws are counted, the first 32 kept in order, the rest counted only");
        }
        // D. The slot is reused by its frame number: frame 7 + 256 lands in frame 7's slot and resets its counts.
        {
            edvr::census::Draw d;
            edvr::census::fillDraw(d, 77u, 1u, rows, true, 1u, blk);
            edvr::census::noteDraw(ring, true, 7u + static_cast<uint32_t>(edvr::census::kFrames), 0, d);
            const edvr::census::Frame& f = ring.frames[7];
            check(f.frame == 7u + static_cast<uint32_t>(edvr::census::kFrames) && f.drawsSeen[0] == 1u &&
                  f.drawsSeen[1] == 0u && f.draws[0][0].vs == 77u,
                  "census: a frame's slot is reset when the ring comes round to it, not added to");
        }
        // E. The rows of a write that was not a rotation: no rotation, no translation, but the P floats still come.
        {
            float bad[12] = {2, 0, 0, 5, 0, 2, 0, 6, 0, 0, 2, 7};
            edvr::census::Draw d;
            edvr::census::fillDraw(d, 5u, 1u, bad, false, 3u, blk);
            check(!d.hasRows && d.T[0] == 0.0f && d.P[15] == 15.5f && d.seq == 3u,
                  "census: a non-rotation write keeps its P floats and clock, and no translation or rotation");
        }
    }
    // THE CAMERA WRITES (diag.camera_writes, camera_writes.h, 2026-10-11): the key off does no work; the ring keeps 256 writes a frame and counts the
    // overflow; the pick and drawn marks land on the write with that clock; a record's translation, quaternion and projection floats are the rows'
    // and the block's; the exe's image range keeps the RVAs of its own frames; a TCWR line parses back; the expected projection and the cost line
    // are as written. The write's cost in this rig is measured (noteWrite alone; the stack capture is Windows and is measured by the flight).
    {
        namespace cw = edvr::camwrites;
        static cw::Ring ring, offRing, bench;
        float rows[12], blk[16];
        rows34(90.0f, 1.0f, 2.0f, 3.0f, rows);
        for (int j = 0; j < 16; ++j) blk[j] = static_cast<float>(j) + 0.5f;
        auto mk = [&](uint32_t seq) {
            cw::In in;
            in.seq = seq;
            in.obj = 0x1000u + seq;
            in.off = 0;
            in.len = 944u * 4u;
            in.tid = 77;
            in.co = 1;
            in.rot = true;
            in.rows = rows;
            in.blk = blk;
            return in;
        };
        // A. Key off: nothing is stored or counted.
        {
            uint64_t calls = 0;
            const bool stored = cw::noteWrite(offRing, false, 7u, mk(1u), &calls);
            check(!stored && calls == 0 && offRing.frames[7].frame == ~0u && offRing.frames[7].n == 0,
                  "camera writes key off: a write stores nothing and counts nothing");
        }
        // B. The cap: 300 writes in frame 9 keep the first 256 in clock order and count 44 as overflow.
        {
            uint64_t calls = 0;
            for (uint32_t s = 0; s < 300u; ++s) cw::noteWrite(ring, true, 9u, mk(100u + s), &calls);
            const cw::Frame& f = ring.frames[9];
            check(f.frame == 9u && f.n == 256u && f.overflow == 44u && calls == 300u,
                  "camera writes cap: 300 writes give 256 kept, 44 overflowed, all 300 counted");
            bool ordered = true;
            for (uint32_t k = 0; k < f.n; ++k) ordered = ordered && f.w[k].seq == 100u + k;
            check(ordered && f.w[0].obj == 0x1000u + 100u && f.w[0].tid == 77u && f.w[0].co == 1u && f.w[0].len == 944u * 4u && f.w[0].off == 0u,
                  "camera writes: each record keeps its clock, object, byte range, thread and depth, in write order");
            // The slot is reused by its frame number, on a ring of its own: frame 9 + 64 lands in frame 9's slot and resets its counts.
            static cw::Ring reuse;
            cw::noteWrite(reuse, true, 9u, mk(1u));
            cw::noteWrite(reuse, true, 9u, mk(2u));
            cw::noteWrite(reuse, true, 9u + static_cast<uint32_t>(cw::kFrames), mk(3u));
            check(reuse.frames[9].frame == 9u + static_cast<uint32_t>(cw::kFrames) && reuse.frames[9].n == 1u &&
                  reuse.frames[9].overflow == 0u && reuse.frames[9].w[0].seq == 3u,
                  "camera writes: a frame's slot is reset when the ring comes round to it, not added to");
        }
        // C. The marks: a write named by both pickers takes both bits; an unknown clock or frame marks nothing; a draw marks the write it saw,
        // and an overflowed write (clock 399) cannot be marked.
        {
            check(cw::markPick(ring, 9u, 100u, cw::kPickLegacy) && cw::markPick(ring, 9u, 100u, cw::kPickTag) &&
                  ring.frames[9].w[0].pick == 3,
                  "camera writes pick: a write named by both pickers takes both bits (pick=both)");
            check(cw::markPick(ring, 9u, 105u, cw::kPickLegacy) && ring.frames[9].w[5].pick == cw::kPickLegacy,
                  "camera writes pick: the chooser's write is found by its clock");
            check(!cw::markPick(ring, 9u, 9999u, cw::kPickLegacy) && !cw::markPick(ring, 8u, 105u, cw::kPickLegacy),
                  "camera writes pick: an unknown clock, or a frame not in the ring, marks nothing");
            check(cw::markDrawn(ring, 9u, 110u) && ring.frames[9].w[10].drawn && !ring.frames[9].w[11].drawn,
                  "camera writes drawn: the draw's write is marked and its neighbour is not");
            check(!cw::markDrawn(ring, 9u, 399u), "camera writes drawn: an overflowed write has no record to mark");
        }
        // D. The record's fields: the rows' translation and quaternion, the projection floats (block floats 0, 2, 5, 6, 10, 11, 14 of 784..799).
        {
            const cw::Write& w = ring.frames[9].w[0];
            checkNear(w.T[0], 1.0f, 1e-6f, "camera writes: the record's translation x is the rows' (1)");
            checkNear(w.T[2], 3.0f, 1e-6f, "camera writes: the record's translation z is the rows' (3)");
            checkNear(w.R[0], 0.70710678f, 1e-5f, "camera writes: the record's quaternion w of a 90 deg yaw");
            checkNear(w.R[2], 0.70710678f, 1e-5f, "camera writes: the record's quaternion y of a 90 deg yaw");
            const float want[7] = {blk[0], blk[2], blk[5], blk[6], blk[10], blk[11], blk[14]};
            bool pOk = true;
            for (int k = 0; k < 7; ++k) pOk = pOk && w.P[k] == want[k];
            check(pOk && w.P[0] == 0.5f && w.P[4] == 10.5f && w.P[6] == 14.5f,
                  "camera writes: P0,P2,P5,P6,P10,P11,P14 are block floats 784 + 0,2,5,6,10,11,14");
        }
        // E. A write whose rows are not a rotation keeps its clock and projection, with no translation or rotation (rot=0).
        {
            cw::In in = mk(500u);
            in.rot = false;
            cw::noteWrite(ring, true, 10u, in);
            const cw::Write& w = ring.frames[10].w[0];
            check(!w.rot && w.T[0] == 0.0f && w.R[0] == 0.0f && w.P[0] == 0.5f && w.seq == 500u,
                  "camera writes: a non-rotation write keeps its clock and projection, and no translation or rotation");
        }
        // F. The stack: the return addresses inside the exe's image are kept as RVAs, innermost first; EDVR's and the driver's are skipped; the
        // cap holds. A stub image: base 0x140000000, 1 MiB.
        {
            const uintptr_t base = 0x140000000ull;
            const size_t size = 0x100000u;
            const void* frames[8] = {
                reinterpret_cast<const void*>(base + 0x1234u),
                reinterpret_cast<const void*>(0x7FFD00001000ull),
                reinterpret_cast<const void*>(base + 0x5678u),
                reinterpret_cast<const void*>(base + size),
                reinterpret_cast<const void*>(base),
                reinterpret_cast<const void*>(0x7FFD00002000ull),
                reinterpret_cast<const void*>(base + 0x9abcu),
                reinterpret_cast<const void*>(0x7FFD00003000ull),
            };
            uint32_t out[6] = {};
            const int n = cw::rvasInRange(frames, 8, base, size, out, 6);
            check(n == 4 && out[0] == 0x1234u && out[1] == 0x5678u && out[2] == 0u && out[3] == 0x9abcu,
                  "stack: the frames inside the exe's image are kept as RVAs, innermost first; frames outside it are skipped");
            const void* many[10];
            for (int i = 0; i < 10; ++i) many[i] = reinterpret_cast<const void*>(base + 0x100u * static_cast<uintptr_t>(i + 1));
            const int capped = cw::rvasInRange(many, 10, base, size, out, 6);
            check(capped == 6 && out[0] == 0x100u && out[5] == 0x600u,
                  "stack: the cap holds (6 of 10 inside the image)");
        }
        // G. The TCWR line: a record formats and parses back to its fields; a buffer that is too small is refused; a malformed line is refused.
        {
            cw::Write w;
            w.seq = 4242u;
            w.obj = 0xABCDEF0u;
            w.off = 0u;
            w.len = 3775u;
            w.tid = 1234u;
            w.co = 2u;
            w.rot = true;
            w.pick = cw::kPickLegacy | cw::kPickTag;
            w.drawn = true;
            w.pair = true;
            w.stkN = 3u;
            w.stk[0] = 0x2869068u;
            w.stk[1] = 0x594B60u;
            w.stk[2] = 0x1u;
            w.T[0] = 1.5f; w.T[1] = -2.0f; w.T[2] = 3.25f;
            w.R[0] = 1.0f; w.R[1] = 0.0f; w.R[2] = 0.0f; w.R[3] = 0.0f;
            for (int k = 0; k < 7; ++k) w.P[k] = 0.5f + static_cast<float>(k) * 2.0f;
            char line[640] = {};
            const int len = cw::formatWrite(line, sizeof(line), 9u, w);
            cw::Parsed p;
            const bool parsed = cw::parseWrite(line, p);
            check(len > 0 && parsed && p.frame == 9u && p.seq == 4242u && p.obj == 0xABCDEF0u && p.off == 0u && p.len == 3775u &&
                  p.tid == 1234u && p.co == 2u && p.rot && p.drawn && p.pair && std::strcmp(p.pick, "both") == 0 && p.stkN == 3u &&
                  p.stk[0] == 0x2869068u && p.stk[1] == 0x594B60u && p.stk[2] == 1u && p.T[1] == -2.0f && p.T[2] == 3.25f &&
                  p.R[0] == 1.0f && p.P[0] == 0.5f && p.P[6] == 12.5f,
                  "camera writes format: a TCWR line parses back to its clock, object, bytes, thread, depth, rows, stack, picks and projection");
            const char* pp = std::strstr(line, "P=(");
            int commas = 0;
            if (pp) for (const char* c = pp; *c && *c != ')'; ++c) if (*c == ',') ++commas;
            check(pp && commas == 6 && std::strstr(line, " stk=2869068,594B60,1 pick=both drawn=1 rot=1 pair=1") != nullptr,
                  "camera writes format: seven P floats, then the stack in hex, then pick, drawn, rot and pair");
            cw::Write ipdW;
            ipdW.pick = cw::kPickIpd | cw::kPickLegacy;
            char ipdLine[640] = {};
            check(cw::formatWrite(ipdLine, sizeof(ipdLine), 9u, ipdW) > 0 && std::strstr(ipdLine, " pick=ipd drawn=0 rot=0 pair=0") != nullptr &&
                  std::strcmp(cw::pickName(cw::kPickIpd), "ipd") == 0 && std::strcmp(cw::pickName(cw::kPickTag), "tag") == 0,
                  "camera writes format: a write the IPD pair chose prints pick=ipd ahead of the older pickers");
            check(cw::formatWrite(line, 60, 9u, w) == 0, "camera writes format: a line that does not fit its buffer is refused, not truncated");
            cw::Write e;
            e.stkN = 0;
            e.pick = 0;
            char empty[640] = {};
            check(cw::formatWrite(empty, sizeof(empty), 9u, e) > 0 && cw::parseWrite(empty, p) && p.stkN == 0u &&
                  std::strcmp(p.pick, "-") == 0 && !p.drawn && !p.rot && !p.pair && std::strstr(empty, " stk=- pick=- drawn=0 rot=0 pair=0") != nullptr,
                  "camera writes format: no stack and no pick print as - and parse back");
            check(!cw::parseWrite("TCWR f9 s4 nonsense", p), "camera writes parse: a malformed line is refused");
            // The jump guard's marks (2026-10-11): pick=guard and rejected=1 format and parse back; a line from before the
            // rejected field has four fields and reads as not rejected.
            cw::Write g;
            g.stkN = 0;
            g.pick = cw::kPickGuard;
            g.rejected = true;
            char guardLine[640] = {};
            check(cw::formatWrite(guardLine, sizeof(guardLine), 9u, g) > 0 &&
                  std::strstr(guardLine, " pick=guard drawn=0 rot=0 pair=0 rejected=1") != nullptr &&
                  cw::parseWrite(guardLine, p) && p.rejected && std::strcmp(p.pick, "guard") == 0,
                  "camera writes format: the guard's pick and rejected mark format and parse back");
            check(std::strcmp(cw::pickName(cw::kPickGuard), "guard") == 0, "camera writes pick name: guard");
            cw::Write old;
            old.stkN = 0;
            char oldLine[640] = {};
            check(cw::formatWrite(oldLine, sizeof(oldLine), 9u, old) > 0, "camera writes format: an unmarked write formats");
            char* cut = std::strstr(oldLine, " rejected=");
            if (cut) *cut = '\0';
            check(cut != nullptr && cw::parseWrite(oldLine, p) && !p.rejected,
                  "camera writes parse: a line from before the rejected field reads as not rejected");
            char head[96] = {};
            check(cw::formatFrame(head, sizeof(head), 9u, 256u, 44u) > 0 && std::strcmp(head, "TCWRF f9 writes=256 overflow=44") == 0,
                  "camera writes format: the frame header line");
        }
        // H. The expected projection: a symmetric frustum has no centre term; an offset one has the textbook values. The TCWRE line carries the
        // tangents in the pass's order (left, right, bottom, top).
        {
            float P[4] = {};
            cw::expectedProjection(-1.0f, 1.0f, 0.5f, -0.5f, P);
            check(P[0] == 1.0f && P[1] == 0.0f && P[2] == 2.0f && P[3] == 0.0f,
                  "camera writes expected: a symmetric frustum has P0 = 2/(r-l), P5 = 2/(t-b) and no centre term");
            cw::expectedProjection(-0.5f, 1.0f, 0.8f, -0.4f, P);
            checkNear(P[0], 4.0f / 3.0f, 1e-5f, "camera writes expected: P0 = 2/(r-l) of an offset frustum");
            checkNear(P[1], 1.0f / 3.0f, 1e-5f, "camera writes expected: P2 = (r+l)/(r-l) of an offset frustum");
            checkNear(P[2], 5.0f / 3.0f, 1e-5f, "camera writes expected: P5 = 2/(t-b) of an offset frustum");
            checkNear(P[3], 1.0f / 3.0f, 1e-5f, "camera writes expected: P6 = (t+b)/(t-b) of an offset frustum");
            const float tan[4] = {-0.5f, 1.0f, -0.4f, 0.8f};
            char line[320] = {};
            const int len = cw::formatExpect(line, sizeof(line), 1, tan, 0.25f, -0.5f);
            check(len > 0 && std::strstr(line, "TCWRE eye=1 tan=(-0.5,+1,-0.4,+0.8)") != nullptr && std::strstr(line, "P0=+1.333333") != nullptr,
                  "camera writes expected: the TCWRE line carries the tangents and the expected P0");
        }
        // I. The 20 s cost line: writes, frames and the cost a frame in microseconds; zero frames print as zeros, not as a division by zero.
        {
            char line[240] = {};
            const int len = cw::formatCost(line, sizeof(line), 12000u, 60u, 2400u, 1.0e6);
            check(len > 0 && std::strcmp(line, "temporal camera writes: 12000 writes over 60 frames (200.0 a frame), cost 40.0 us a frame") == 0,
                  "camera writes cost line: writes, frames, and the cost a frame in microseconds");
            const int zero = cw::formatCost(line, sizeof(line), 0u, 0u, 0u, 0.0);
            check(zero > 0 && std::strcmp(line, "temporal camera writes: 0 writes over 0 frames (0.0 a frame), cost 0.0 us a frame") == 0,
                  "camera writes cost line: zero frames and zero writes print as zeros");
        }
        // J. The rig's measured cost: noteWrite alone, 200 000 writes over 782 frames of 256 (the overflow path included), with a whole write each.
        {
            const int kWrites = 200000;
            uint64_t calls = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kWrites; ++i) cw::noteWrite(bench, true, static_cast<uint32_t>(i / 256), mk(static_cast<uint32_t>(i)), &calls);
            const auto t1 = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(kWrites);
            printf("  cost  camera writes: noteWrite %.1f ns a write (rig, %d writes, no stack capture)\n", ns, kWrites);
            check(calls == static_cast<uint64_t>(kWrites), "camera writes cost: the rig's writes are all counted");
        }
    }
    // THE HEADSET CAMERA TAG (hmd_camera_tag.h, 2026-10-11): the gate, the chooser's pick, the depth scope, the 30 s line, the census's cost line and
    // the TDRW hmd field. The hook itself (src\d3d11\hmd_camera_tag_hook.cpp) is Windows and is exercised by the flight, not here.
    {
        using namespace edvr::hmdtag;
        // A. The gate: build 332841's bytes pass; a one-byte change in any read is refused.
        uint8_t prologue[kPrologueBytes];
        std::memcpy(prologue, kPrologue, sizeof(prologue));
        uint8_t site[kCallSpan];
        std::memcpy(site, kCallSite, kCallSiteBytes);
        const int64_t rel = int64_t(kCoordinatorRva) - (int64_t(kCallSiteRva) + int64_t(kCallSpan));
        const int32_t rel32 = int32_t(rel);
        std::memcpy(site + kCallSiteBytes, &rel32, 4);
        check(gateVerdict(kExpectedTimestamp, kExpectedImageSize, prologue, site) == nullptr,
              "hmd tag gate: the build's identity, the coordinator's prologue and the call site's displacement all pass");
        uint8_t p2[kPrologueBytes];
        std::memcpy(p2, prologue, sizeof(p2));
        p2[17] ^= 0x01;
        check(gateVerdict(kExpectedTimestamp, kExpectedImageSize, p2, site) != nullptr,
              "hmd tag gate: a one-byte change in the prologue is refused");
        uint8_t s2[kCallSpan];
        std::memcpy(s2, site, sizeof(s2));
        s2[7] ^= 0x01;   // the rel32's low byte
        check(gateVerdict(kExpectedTimestamp, kExpectedImageSize, prologue, s2) != nullptr,
              "hmd tag gate: a one-byte change in the call's displacement is refused");
        uint8_t s3[kCallSpan];
        std::memcpy(s3, site, sizeof(s3));
        s3[0] ^= 0x01;   // the call site's first opcode byte
        check(gateVerdict(kExpectedTimestamp, kExpectedImageSize, prologue, s3) != nullptr,
              "hmd tag gate: a one-byte change in the call site's opcode bytes is refused");
        check(gateVerdict(kExpectedTimestamp + 1u, kExpectedImageSize, prologue, site) != nullptr,
              "hmd tag gate: another PE timestamp is refused");
        check(gateVerdict(kExpectedTimestamp, kExpectedImageSize, prologue, nullptr) != nullptr,
              "hmd tag gate: a missing call-site read is refused");
        // B. The pick: a stamped write of this frame is chosen over a newer unstamped write of another camera's pose, and another frame's stamp is ignored.
        const Cand ring[3] = {{5u, 10u, true}, {5u, 14u, false}, {4u, 20u, true}};
        const int tagB = latestStamped(3, 5u, [&](int i) { return ring[i]; });
        check(tagB == 0, "hmd tag: the frame's stamp is found; the newer unstamped write and the other frame's stamp are not");
        const Decision dB = decide(/*legacy pick*/ 1, tagB, true);
        check(dB.pick == 0 && dB.byTag && dB.disagree && !dB.fellBack,
              "hmd tag: with the key on, the stamped write is chosen over the newer unstamped pose, and the disagreement is counted");
        // C. The latest of several stamps of the frame.
        const Cand ringC[3] = {{5u, 10u, true}, {5u, 12u, true}, {5u, 11u, false}};
        check(latestStamped(3, 5u, [&](int i) { return ringC[i]; }) == 1,
              "hmd tag: of several stamps of a frame the latest (highest clock) is taken");
        // D. No stamp: today's pick, counted as a fallback.
        const Cand ringD[2] = {{5u, 3u, false}, {5u, 4u, false}};
        const int tagD = latestStamped(2, 5u, [&](int i) { return ringD[i]; });
        const Decision dD = decide(/*legacy pick*/ 1, tagD, true);
        check(tagD == -1 && dD.pick == 1 && !dD.byTag && !dD.disagree && dD.fellBack,
              "hmd tag: with no stamp this frame the pick is today's, and the frame counts as a fallback");
        // E. Key off: the pick is today's whatever the stamp says; the disagreement is still reported.
        const Decision dE = decide(/*legacy pick*/ 1, 0, false);
        check(dE.pick == 1 && !dE.byTag && dE.disagree,
              "hmd tag: with the key off the pick is today's, and the tag's disagreement is still counted");
        // F. The depth scope: balanced across nested scopes, and across a throw out of nested calls.
        uint32_t depth = 0;
        {
            DepthScope a(depth);
            {
                DepthScope b(depth);
                check(depth == 2, "hmd tag depth: a nested scope counts two");
            }
            check(depth == 1, "hmd tag depth: leaving the nested scope counts one");
        }
        check(depth == 0, "hmd tag depth: the nested scopes balance to zero");
        auto thrower = [](auto& self, uint32_t& dep, int k) -> void {
            DepthScope s(dep);
            if (k == 0) throw std::runtime_error("the game threw");
            self(self, dep, k - 1);
        };
        bool caught = false;
        try {
            thrower(thrower, depth, 3);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        check(caught && depth == 0, "hmd tag depth: a throw out of four nested scopes restores the count to zero");
        check(!insideCoordinator(), "hmd tag: outside the coordinator's scope the thread is not inside it");
        {
            DepthScope s(coordinatorDepth);
            check(insideCoordinator(), "hmd tag: inside the scope the thread is inside the coordinator");
        }
        try {
            thrower(thrower, coordinatorDepth, 2);
        } catch (const std::runtime_error&) {
        }
        check(!insideCoordinator() && coordinatorDepth == 0,
              "hmd tag: a throw inside the thread's coordinator scope leaves the thread outside it");
        // G. The 30 s line: live, inert with its reason, zeros included, and a buffer too small is refused.
        Stats st;
        st.coordinatorCalls = 12;
        st.frames = 12;
        st.framesWithStamp = 12;
        st.stampedWrites = 12;
        st.chosenByTag = 12;
        st.disagree = 2;
        st.lastFrame = 99;
        st.lastThread = 7;
        char line[640];
        const int gLen = formatReport(line, sizeof(line), true, nullptr, true, st);
        check(gLen > 0 && std::strstr(line, "temporal hmd tag: hook=live ") && std::strstr(line, "per_frame=1.00") &&
                  std::strstr(line, "frames_with_stamp=12/12") && std::strstr(line, "disagree=2 key=on") &&
                  std::strstr(line, "last_frame=99 last_thread=7"),
              "hmd tag 30 s line, live: per frame, stamp coverage, disagreement, the key and the last call");
        const int gLen2 = formatReport(line, sizeof(line), false, "not build 332841 (PE timestamp or image size differs)", false, Stats{});
        check(gLen2 > 0 && std::strstr(line, "hook=inert:not build 332841") && std::strstr(line, "per_frame=0.00") &&
                  std::strstr(line, "key=off"),
              "hmd tag 30 s line, inert: names the reason, and an idle window prints its zeros");
        check(formatReport(line, 20, true, nullptr, true, st) == 0, "hmd tag 30 s line: a buffer too small is refused, not truncated");
        // H. The census's cost line: zero draws over zero frames print zeros; the per-frame and per-draw costs are the interval's.
        char cost[400];
        const int cl0 = edvr::census::formatCost(cost, sizeof(cost), 0u, 0u, 0u, 1.0e7);
        check(cl0 > 0 && std::strstr(cost, "temporal draw census: 0 recognised eye-target draws over 0 frames") && !std::strstr(cost, "nan") &&
                  !std::strstr(cost, "inf"),
              "census cost line: no draws over no frames prints zeros, never nan or inf");
        const int cl1 = edvr::census::formatCost(cost, sizeof(cost), 10u, 4u, 5000u, 1.0e7);
        check(cl1 > 0 && std::strstr(cost, "10 recognised eye-target draws over 4 frames (2.5 a frame)") &&
                  std::strstr(cost, "the census cost 125.0 us a frame (50.00 us a draw)"),
              "census cost line: 10 draws over 4 frames, 500 us of census work, prints 2.5 a frame and 125 us a frame");
        // I. The TDRW line's hmd field: a draw whose rows are the frame's headset-stamped rows is marked hmd=1, the rest hmd=0.
        const float rowsI[12] = {1, 0, 0, 2, 0, 1, 0, 3, 0, 0, 1, 4};
        edvr::census::Draw dI;
        edvr::census::fillDraw(dI, 9u, 1u, rowsI, true, 1u, nullptr);
        dI.hmd = true;
        char tl[640];
        const int tlen = edvr::census::formatDraw(tl, sizeof(tl), 3u, 0, 0u, dI);
        check(tlen > 0 && std::strstr(tl, "rows=1 hmd=1 T=(+2,+3,+4)") && !std::strstr(tl, "hmd=0"),
              "census: a TDRW line marks the draw whose rows are the headset-stamped write's: rows=1 hmd=1");
        dI.hmd = false;
        const int tlen0 = edvr::census::formatDraw(tl, sizeof(tl), 3u, 0, 0u, dI);
        check(tlen0 > 0 && std::strstr(tl, "rows=1 hmd=0 T=("), "census: a TDRW line without the stamp is hmd=0");
    }
    // J. THE CAMERA IPD PAIR (camera_ipd_pair.h, 2026-10-11): the runtime offset, the partner test, the nearest reference, the hold, the legacy
    // fallback, the backstop and the 30 s line. The rows are the physical model: the head's pose composed with each eye's game-convention
    // eye-to-head (the logged pair's relation, P = D - 0.0606 m * col0, falls out of it). The chooser itself (temporal_pass.cpp) is exercised by the flight.
    {
        namespace ip = edvr::ipdpair;
        const float kPi = 3.14159265f;
        // rigid [R(yaw theta) | t], row-major 3x4
        auto mk = [](float theta, float tx, float ty, float tz, float out[12]) {
            const float c = std::cos(theta), s = std::sin(theta);
            const float r[12] = {c, 0, s, tx, 0, 1, 0, ty, -s, 0, c, tz};
            std::memcpy(out, r, sizeof(r));
        };
        // out = a * b for rigid 3x4s
        auto compose = [](const float a[12], const float b[12], float out[12]) {
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    float s = 0.0f;
                    for (int k = 0; k < 3; ++k) s += a[i * 4 + k] * b[k * 4 + j];
                    out[i * 4 + j] = s;
                }
                float t = a[i * 4 + 3];
                for (int k = 0; k < 3; ++k) t += a[i * 4 + k] * b[k * 4 + 3];
                out[i * 4 + 3] = t;
            }
        };
        float head[12];
        mk(0.4f, 0.10f, 1.60f, -0.30f, head);
        // The runtime's located eye-to-head: OpenVR's left eye at -IPD/2 on x, right at +IPD/2, no cant. IPD 60.6 mm.
        const float locL[12] = {1, 0, 0, -0.0303f, 0, 1, 0, 0, 0, 0, 1, 0};
        const float locR[12] = {1, 0, 0, 0.0303f, 0, 1, 0, 0, 0, 0, 1, 0};
        float eyes[2][12];
        std::memcpy(eyes[0], locL, sizeof(locL));
        std::memcpy(eyes[1], locR, sizeof(locR));
        const ip::Offset off = ip::offsetFromEyes(eyes);
        check(off.valid, "ipd pair offset: two eye-to-head transforms at 60.6 mm give a valid runtime offset");
        checkNear(off.o[0], -0.0606f, 1e-5f, "ipd pair offset: the left eye sits -60.6 mm on the reference's own x (the logged relation)");
        checkNear(off.o[1], 0.0f, 1e-6f, "ipd pair offset: no y component without cant");
        checkNear(off.o[2], 0.0f, 1e-6f, "ipd pair offset: no z component without cant");
        checkNear(off.ipdM * 1000.0f, 60.6f, 1e-3f, "ipd pair offset: |ipd| = 60.6 mm");
        checkNear(off.cantDeg, 0.0f, 1e-3f, "ipd pair offset: no canting on a parallel pair");
        float gl[12], gr[12], D[12], P[12];
        ip::gameEyeToHead(locL, gl);
        ip::gameEyeToHead(locR, gr);
        compose(head, gr, D);   // the right eye: the reference
        compose(head, gl, P);   // the left eye: its partner
        checkNear(P[3], D[3] - 0.0606f * D[0], 1e-5f, "ipd pair physics: P = D - 0.0606 m * col0 (x)");
        checkNear(P[7], D[7] - 0.0606f * D[4], 1e-5f, "ipd pair physics: P = D - 0.0606 m * col0 (y)");
        checkNear(P[11], D[11] - 0.0606f * D[8], 1e-5f, "ipd pair physics: P = D - 0.0606 m * col0 (z)");

        // The steady pair is chosen, the reference being the right eye (index 1 here), whatever the order of the writes.
        const float* rows2[2] = {P, D};
        const uint32_t seq2[2] = {1u, 2u};
        uint8_t inP[8] = {};
        const ip::Scan s1 = ip::scanPairs(rows2, seq2, 2, off.o, off.rel, nullptr, inP);
        check(s1.ref == 1 && s1.partner == 0 && s1.families == 1 && inP[0] && inP[1],
              "ipd pair: the steady pair is chosen, the right eye as the reference, its partner marked");
        checkNear(s1.transErrM, 0.0f, 1e-5f, "ipd pair: the steady pair's translation residual is zero");

        // A probe face: the same translation, 90 degrees about the same axis, no partner. Rejected.
        float probe[12];
        mk(0.4f + kPi / 2.0f, D[3], D[7], D[11], probe);
        const float* rows3[3] = {P, D, probe};
        const uint32_t seq3[3] = {1u, 2u, 3u};
        uint8_t inP3[8] = {};
        const ip::Scan s2 = ip::scanPairs(rows3, seq3, 3, off.o, off.rel, nullptr, inP3);
        check(s2.ref == 1 && inP3[2] == 0, "ipd pair: a probe-face write with the same translation and no partner is rejected");
        const float* rows6[1] = {probe};
        const ip::Scan s6 = ip::scanPairs(rows6, seq3, 1, off.o, off.rel, nullptr, nullptr);
        check(s6.ref == -1 && s6.families == 0, "ipd pair: a probe face alone forms no pair family");

        // The 0.19 m / 2.99 deg excursion: no partner. Rejected beside the steady pair; alone it leaves the frame with no pair (held below).
        float exc[12];
        mk(0.4f + 2.99f * kPi / 180.0f, D[3] + 0.19f, D[7], D[11], exc);
        const float* rows4[3] = {P, D, exc};
        uint8_t inP4[8] = {};
        const ip::Scan s3 = ip::scanPairs(rows4, seq3, 3, off.o, off.rel, nullptr, inP4);
        check(s3.ref == 1 && inP4[2] == 0, "ipd pair: the 0.19 m / 2.99 deg excursion, with no partner, is rejected");
        const float* rowsE[1] = {exc};
        const ip::Scan s4 = ip::scanPairs(rowsE, seq3, 1, off.o, off.rel, nullptr, nullptr);
        check(s4.ref == -1, "ipd pair: the excursion alone leaves no pair");
        {
            ip::Chain c;
            (void)ip::decideFrame(c, true, D, D);
            const ip::Verdict hv = ip::decideFrame(c, true, nullptr, exc);
            check(hv.pick == ip::Pick::Hold && !hv.backstop && std::memcmp(c.pose, D, sizeof(D)) == 0,
                  "ipd pair: a frame whose only candidate is the excursion is held: the previous rows stand");
        }

        // A camera 10 m away, facing about 178 degrees: no partner. Rejected.
        float far[12];
        mk(0.4f + 178.0f * kPi / 180.0f, D[3], D[7], D[11] + 10.1f, far);
        const float* rows5[3] = {P, D, far};
        uint8_t inP5[8] = {};
        const ip::Scan s5 = ip::scanPairs(rows5, seq3, 3, off.o, off.rel, nullptr, inP5);
        check(s5.ref == 1 && inP5[2] == 0, "ipd pair: a camera 10 m away is rejected");

        // Two pairs in one frame: the one nearest the previous pose wins; with no previous pose, the latest clock.
        float headB[12], DB[12], PB[12];
        mk(0.4f, 5.10f, 1.60f, -0.30f, headB);
        compose(headB, gr, DB);
        compose(headB, gl, PB);
        const float* rows7[4] = {P, D, PB, DB};
        const uint32_t seq7[4] = {1u, 2u, 3u, 4u};
        const ip::Scan s7near = ip::scanPairs(rows7, seq7, 4, off.o, off.rel, D, nullptr);
        check(s7near.families == 2 && s7near.ref == 1, "ipd pair: two pairs, the one nearest the previous pose (pair A) is the reference");
        const ip::Scan s7late = ip::scanPairs(rows7, seq7, 4, off.o, off.rel, nullptr, nullptr);
        check(s7late.ref == 3, "ipd pair: two pairs with no previous pose, the latest clock (pair B) is the reference");

        // The hold, the legacy fallback after three holds, and the episode's start and end.
        {
            ip::Chain c;
            const ip::Verdict first = ip::decideFrame(c, true, D, D);
            check(first.pick == ip::Pick::Rule && c.havePose, "ipd pair chain: the first pair is taken");
            ip::Verdict h1 = ip::decideFrame(c, true, nullptr, D), h2 = ip::decideFrame(c, true, nullptr, D), h3 = ip::decideFrame(c, true, nullptr, D);
            check(h1.pick == ip::Pick::Hold && h2.pick == ip::Pick::Hold && h3.pick == ip::Pick::Hold && c.holdRun == 3,
                  "ipd pair chain: three frames without a pair are held");
            const ip::Verdict fb = ip::decideFrame(c, true, nullptr, D);
            check(fb.pick == ip::Pick::Legacy && fb.episodeStart && fb.episode == 1u,
                  "ipd pair chain: the fourth frame without a pair falls back to the legacy chooser, starting episode 1 once");
            const ip::Verdict fb2 = ip::decideFrame(c, true, nullptr, D);
            check(fb2.pick == ip::Pick::Legacy && !fb2.episodeStart, "ipd pair chain: the episode is logged once, not each frame");
            const ip::Verdict back = ip::decideFrame(c, true, D, D);
            check(back.pick == ip::Pick::Rule && back.episodeEnd && c.holdRun == 0 && !c.inEpisode,
                  "ipd pair chain: a pair returning ends the episode and the rule's pick resumes");
        }

        // The backstop with no runtime offset (the rule off): the 0.19 m excursion is rejected and held; a jump that persists is taken on
        // the fourth frame; a jump that continues the previous step is taken at once.
        {
            ip::Chain c;
            c.havePose = true;
            std::memcpy(c.pose, D, sizeof(D));
            const ip::Verdict b1 = ip::decideFrame(c, false, nullptr, exc);
            check(b1.pick == ip::Pick::Hold && b1.backstop && std::memcmp(c.pose, D, sizeof(D)) == 0,
                  "backstop, rule off: the 0.19 m / 2.99 deg excursion is rejected and the previous rows stand");
            (void)ip::decideFrame(c, false, nullptr, exc);
            (void)ip::decideFrame(c, false, nullptr, exc);
            const ip::Verdict b4 = ip::decideFrame(c, false, nullptr, exc);
            check(b4.pick == ip::Pick::Legacy && !b4.backstop,
                  "backstop: a jump that persists for three frames is taken on the fourth (a real camera change is not held forever)");
        }
        {
            float before[12], cand[12];
            std::memcpy(before, D, sizeof(D));
            before[3] -= 0.5f;
            std::memcpy(cand, D, sizeof(D));
            cand[3] += 0.5f;   // the step 0.5 m: a jump over 3 cm, continuing the previous step by constant velocity
            ip::Chain c;
            c.havePose = true;
            c.havePrev = true;
            std::memcpy(c.before, before, sizeof(before));
            std::memcpy(c.pose, D, sizeof(D));
            const ip::Verdict cv = ip::decideFrame(c, false, nullptr, cand);
            check(cv.pick == ip::Pick::Legacy && !cv.backstop, "backstop: a jump that continues the previous step by constant velocity is taken");
            float off2[12];
            std::memcpy(off2, D, sizeof(D));
            off2[3] += 0.2f;   // a jump with no step behind it
            ip::Chain c2;
            c2.havePose = true;
            c2.havePrev = true;
            std::memcpy(c2.before, D, sizeof(D));
            std::memcpy(c2.pose, D, sizeof(D));
            const ip::Verdict nv = ip::decideFrame(c2, false, nullptr, off2);
            check(nv.pick == ip::Pick::Hold && nv.backstop, "backstop: a jump with no step behind it is rejected");
        }

        // A canted headset, 68 mm: the runtime's offset finds the pair, its cant included. The rows are the physical model; the partner's
        // rotation differs from the reference's by the cant, so the relative-rotation branch is the one that passes.
        {
            const float cant = 2.5f * kPi / 180.0f;
            float cL[12], cR[12];
            mk(cant, -0.034f, 0.0f, 0.0f, cL);
            mk(-cant, 0.034f, 0.0f, 0.0f, cR);
            float ce[2][12];
            std::memcpy(ce[0], cL, sizeof(cL));
            std::memcpy(ce[1], cR, sizeof(cR));
            const ip::Offset co = ip::offsetFromEyes(ce);
            check(co.valid, "ipd pair, canted 68 mm: the runtime offset is valid");
            checkNear(co.ipdM * 1000.0f, 68.0f, 1e-2f, "ipd pair, canted: |ipd| = 68 mm, from the runtime offset");
            checkNear(co.cantDeg, 5.0f, 0.02f, "ipd pair, canted: the cant is 5 deg between the two eyes (2 x 2.5)");
            check(std::fabs(co.o[2]) > 0.002f, "ipd pair, canted: the offset has a z component (the cant rotates it into the reference's axes)");
            float cgl[12], cgr[12], cD[12], cP[12], ch[12];
            mk(0.25f, 0.10f, 1.60f, -0.30f, ch);
            ip::gameEyeToHead(cL, cgl);
            ip::gameEyeToHead(cR, cgr);
            compose(ch, cgr, cD);
            compose(ch, cgl, cP);
            const float* rowsC[2] = {cP, cD};
            const uint32_t seqC[2] = {1u, 2u};
            const ip::Scan sc = ip::scanPairs(rowsC, seqC, 2, co.o, co.rel, nullptr, nullptr);
            check(sc.ref == 1 && sc.partner == 0, "ipd pair, canted: the pair is found from the runtime offset, cant and all");
            check(ip::isPartner(cD, cP, co.o, co.rel) && !ip::isPartner(cD, cP, co.o, ip::kIdentity3),
                  "ipd pair, canted: the partner passes on the relative cant and would fail on identity alone");
        }
        // A zero transform is no offset.
        {
            float z2[2][12] = {};
            const ip::Offset zo = ip::offsetFromEyes(z2);
            check(!zo.valid, "ipd pair offset: two zero transforms are no offset (the rule cannot run)");
        }
        // The 30 s line: the counts, the IPD, the residual's p50 and max, the key, and the backstop count. Too small a buffer is refused.
        {
            ip::Stats st;
            st.frames = 10;
            st.paired = 7;
            st.held = 2;
            st.legacyFallback = 1;
            st.episodes = 1;
            st.disagree = 3;
            st.backstop = 1;
            st.ipdKnown = true;
            st.ipdM = 0.0606f;
            st.errN = 3;
            st.errs[0] = 0.001f;
            st.errs[1] = 0.002f;
            st.errs[2] = 0.003f;
            st.errMax = 0.003f;
            char rep[480];
            const int rl = ip::formatReport(rep, sizeof(rep), st, true);
            check(rl > 0 && std::strcmp(rep,
                  "temporal ipd pair: frames=10 paired=7 held=2 legacy_fallback=1 episodes=1 disagree_with_legacy=3 ipd_mm=60.60 "
                  "pair_err_mm p50/max=2.000/3.000 key=on backstop=1") == 0,
                  "ipd pair 30 s line: the counts, ipd_mm, the residual's p50 and max, key and backstop, in the logged format");
            check(ip::formatReport(rep, 20, st, true) == 0, "ipd pair 30 s line: a buffer too small is refused, not truncated");
            ip::resetWindow(st);
            check(st.frames == 0 && st.errN == 0 && st.ipdKnown && st.ipdM > 0.06f,
                  "ipd pair 30 s line: the window resets its counts and keeps the runtime's IPD");
        }
        // The key: applied picks are the legacy's with the key off, whatever the chain decided. The chain still counts.
        check(ip::applyPick(false, ip::Pick::Rule) == ip::Pick::Legacy && ip::applyPick(false, ip::Pick::Hold) == ip::Pick::Legacy &&
              ip::applyPick(true, ip::Pick::Rule) == ip::Pick::Rule && ip::applyPick(true, ip::Pick::Hold) == ip::Pick::Hold,
              "ipd pair key: off applies the legacy chooser's pick for every chain pick; on applies the chain's");
        check(ip::samePose(D, D) && !ip::samePose(D, P), "ipd pair: samePose is true for one pose and false for the partner");
    }
    // K. THE CAMERA JUMP GUARD (camera_jump_guard.h, 2026-10-11): the one-frame excursion is replaced by the view candidate; steady motion and a
    // constant-velocity flight pass; a jump with nothing passing is taken as a real change; a persisting jump is taken after kReplaceLimit replaced
    // frames; the alternates in their own order; the applied pose after a replacement is the candidate's; key off leaves the legacy track; the line.
    {
        namespace jg = edvr::jumpguard;
        const float kDeg = 3.14159265f / 180.0f;
        // rigid [R(yaw) | t], row-major 3x4
        auto mkPose = [](float yaw, float x, float y, float z, float out[12]) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            const float r[12] = {c, 0, s, x, 0, 1, 0, y, -s, 0, c, z};
            std::memcpy(out, r, sizeof(r));
        };
        // the applied history: before, then prev (the last applied pose)
        auto seed = [](jg::Guard& g, const float before[12], const float prev[12]) {
            jg::commit(g, before, false);
            jg::commit(g, prev, false);
        };
        float V0[12];
        mkPose(0.4f, 0.10f, 1.60f, -0.30f, V0);
        // A. The excursion: 0.19 m and 2.99 deg from a still view. The view's own write (0.5 mm off) is the replacement.
        {
            jg::Guard g;
            seed(g, V0, V0);
            float exc[12], view[12];
            mkPose(0.4f + 2.99f * kDeg, 0.29f, 1.60f, -0.30f, exc);
            mkPose(0.4f, 0.1005f, 1.60f, -0.30f, view);
            const float* alts[1] = {view};
            const jg::Decision d = jg::decide(g, exc, alts, 1);
            check(d.rejected && d.alt == 0, "jump guard: the 0.19 m / 2.99 deg excursion is rejected and replaced by the view candidate");
            checkNear(d.jumpM, 0.19f, 1e-4f, "jump guard: the rejected jump is the 0.19 m translation");
            check(jg::applied(true, d) == 0 && jg::applied(false, d) == -1,
                  "jump guard key: on applies the view candidate; off applies the legacy pick");
            jg::commit(g, view, true);
            check(std::memcmp(g.chain.pose, view, sizeof(view)) == 0 && g.replaceRun == 1,
                  "jump guard: the applied pose after a replacement is the candidate's, and the replaced run counts it");
            float view2[12];
            mkPose(0.4f, 0.1006f, 1.60f, -0.30f, view2);
            const jg::Decision next = jg::decide(g, view2, nullptr, 0);
            check(!next.rejected && next.alt == -1, "jump guard: the view's next frame passes and the run ends");
            jg::commit(g, view2, false);
            check(g.replaceRun == 0, "jump guard: a frame applied without a replacement resets the run");
        }
        // B. Steady head motion passes: 5 mm and 0.2 deg a frame, and a constant-velocity 4 cm a frame.
        {
            jg::Guard g;
            float b[12], p[12], l[12];
            mkPose(0.4f, 0.095f, 1.60f, -0.30f, b);
            mkPose(0.4f + 0.2f * kDeg, 0.10f, 1.60f, -0.30f, p);
            mkPose(0.4f + 0.4f * kDeg, 0.105f, 1.60f, -0.30f, l);
            seed(g, b, p);
            const jg::Decision d = jg::decide(g, l, nullptr, 0);
            check(!d.rejected && d.alt == -1, "jump guard: steady head motion (5 mm, 0.2 deg a frame) passes untouched");
            float b2[12], p2[12], l2[12];
            mkPose(0.4f, 0.02f, 1.60f, -0.30f, b2);
            mkPose(0.4f, 0.06f, 1.60f, -0.30f, p2);
            mkPose(0.4f, 0.10f, 1.60f, -0.30f, l2);
            jg::Guard g2;
            seed(g2, b2, p2);
            const jg::Decision d2 = jg::decide(g2, l2, nullptr, 0);
            check(!d2.rejected, "jump guard: a 4 cm a frame step at constant velocity passes (it continues the step)");
        }
        // C. A 300 m-a-frame flight passes at constant velocity; the same flight with the camera turned 10 deg in a frame is rejected, and the
        //    view (300 m, still on the flight's track) replaces it.
        {
            float x0[12], x1[12], x2[12], x2turn[12], xv[12];
            mkPose(0.4f, 0.0f, 1.60f, -0.30f, x0);
            mkPose(0.4f, 300.0f, 1.60f, -0.30f, x1);
            mkPose(0.4f, 600.0f, 1.60f, -0.30f, x2);
            mkPose(0.4f + 10.0f * kDeg, 600.0f, 1.60f, -0.30f, x2turn);
            mkPose(0.4f, 300.0005f, 1.60f, -0.30f, xv);
            jg::Guard g;
            seed(g, x0, x1);
            const jg::Decision flight = jg::decide(g, x2, nullptr, 0);
            check(!flight.rejected, "jump guard: a 300 m-a-frame flight passes (constant velocity, no replacement)");
            const float* alts[1] = {xv};
            const jg::Decision turn = jg::decide(g, x2turn, alts, 1);
            check(turn.rejected && turn.alt == 0, "jump guard: the same flight with a 10 deg turn in the frame is rejected and the on-track view replaces it");
        }
        // D. A real camera change: 10 m away, no candidate passes (none offered, or only one that also jumps). It is taken, and the next frame
        //    measures from it.
        {
            float far[12], farAlt[12];
            mkPose(0.4f, 10.10f, 1.60f, -0.30f, far);
            mkPose(0.4f, 10.1005f, 1.60f, -0.30f, farAlt);
            jg::Guard g;
            seed(g, V0, V0);
            const jg::Decision none = jg::decide(g, far, nullptr, 0);
            check(none.rejected && none.alt == -1, "jump guard: a real change with no candidate is rejected and the legacy pick is taken");
            checkNear(none.jumpM, 10.0f, 1e-3f, "jump guard: the real change's jump is its 10 m translation");
            const float* jumpy[1] = {farAlt};
            const jg::Decision still = jg::decide(g, far, jumpy, 1);
            check(still.rejected && still.alt == -1 && !jg::passes(farAlt, g.chain),
                  "jump guard: a candidate that also jumps and does not continue is not a replacement");
            jg::commit(g, far, false);
            const jg::Decision after = jg::decide(g, far, nullptr, 0);
            check(!after.rejected, "jump guard: the next frame measures from the new camera and passes");
        }
        // E. A persisting jump: after kReplaceLimit replaced frames the same excursion is taken as a real change; one short of it is still replaced.
        {
            float exc[12], view[12];
            mkPose(0.4f + 2.99f * kDeg, 0.29f, 1.60f, -0.30f, exc);
            mkPose(0.4f, 0.1005f, 1.60f, -0.30f, view);
            const float* alts[1] = {view};
            jg::Guard g;
            seed(g, V0, V0);
            for (uint32_t i = 0; i + 1 < jg::kReplaceLimit; ++i) jg::commit(g, V0, true);
            const jg::Decision short1 = jg::decide(g, exc, alts, 1);
            check(short1.rejected && short1.alt == 0, "jump guard: a jump one frame short of the limit is still replaced");
            jg::commit(g, V0, true);
            const jg::Decision taken = jg::decide(g, exc, alts, 1);
            check(taken.rejected && taken.alt == -1, "jump guard: a jump that persists kReplaceLimit replaced frames is taken as a real change");
        }
        // F. The alternates in the chooser's order: the first that passes; none passing gives the legacy pick.
        {
            float exc[12], bad[12], good1[12], good2[12];
            mkPose(0.4f + 2.99f * kDeg, 0.29f, 1.60f, -0.30f, exc);
            mkPose(0.4f, 0.60f, 1.60f, -0.30f, bad);
            mkPose(0.4f, 0.1002f, 1.60f, -0.30f, good1);
            mkPose(0.4f, 0.1001f, 1.60f, -0.30f, good2);
            jg::Guard g;
            seed(g, V0, V0);
            const float* order1[3] = {bad, good1, good2};
            check(jg::decide(g, exc, order1, 3).alt == 1, "jump guard: the first passing alternate in the chooser's order is taken");
            const float* order2[2] = {bad, bad};
            check(jg::decide(g, exc, order2, 2).alt == -1, "jump guard: no passing alternate leaves the legacy pick");
        }
        // G. The 30 s line: the counts, passed_through as frames minus replaced, max_jump_m over the rejected jumps, the key, and a small buffer refused.
        {
            jg::Stats s;
            jg::Decision a;
            a.rejected = true;
            a.alt = 0;
            a.jumpM = 0.19f;
            jg::Decision b;
            jg::Decision c;
            c.rejected = true;
            c.jumpM = 10.0f;
            jg::note(s, a);
            jg::note(s, b);
            jg::note(s, c);
            check(s.frames == 3 && s.rejected == 2 && s.replaced == 1, "jump guard 30 s line: frames, rejected and replaced count the verdicts");
            char rep[240];
            const int rl = jg::formatReport(rep, sizeof(rep), s, true);
            check(rl > 0 && std::strcmp(rep, "temporal camera jump guard: frames=3 rejected=2 replaced=1 passed_through=2 key=on "
                                             "max_jump_m=10.000") == 0,
                  "jump guard 30 s line: the counts, passed_through, key and max_jump_m, in the logged format");
            check(jg::formatReport(rep, sizeof(rep), s, false) > 0 && std::strstr(rep, "key=off") != nullptr,
                  "jump guard 30 s line: key off is printed as key=off");
            check(jg::formatReport(rep, 30, s, true) == 0, "jump guard 30 s line: a buffer too small is refused, not truncated");
            jg::resetWindow(s);
            check(s.frames == 0 && s.rejected == 0 && s.replaced == 0 && s.maxJumpM == 0.0f, "jump guard 30 s line: the window resets");
        }
    }
    if (g_fails) {
        printf("\nTEMPORAL TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nTEMPORAL TEST PASSED\n");
    return 0;
}
