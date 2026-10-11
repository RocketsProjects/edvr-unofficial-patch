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

// The chooser's glue for one frame (temporal_pass.cpp, chooseCameraRows), over
// the candidates' rows: the orientation test against last frame's rows, the
// chain's test (with the key on), the re-anchor when no candidate agrees with
// the chain, the pick (the latest continuous candidate, else the latest), its
// commit and the guard. gated false is the key off: orientation alone.
struct ContinuityFrame {
    int pick = -1;
    edvr::TemporalChainOutcome outcome = edvr::TemporalChainOutcome::Anchored;
    bool guard = false;
    bool refused = false;   // an orientation-continuous candidate the chain refused
};

ContinuityFrame continuityFrame(edvr::TemporalCameraChain& ch, const float prev[12], bool prevValid,
                                float (*cands)[12], int n, bool gated) {
    ContinuityFrame f;
    bool orient[4] = {}, chainOk[4] = {}, reOk[4] = {};
    bool anyChainOk = false;
    for (int i = 0; i < n && prevValid; ++i) {
        float rp[9], rn[9], rpT[9], d[9];
        edvr::temporalRot3Of34(prev, rp);
        edvr::temporalRot3Of34(cands[i], rn);
        edvr::temporalTranspose3(rp, rpT);
        edvr::temporalMul3(rpT, rn, d);
        orient[i] = edvr::temporalRotationAngleDeg(d) < 3.0f;
        if (!orient[i] || !gated) continue;
        chainOk[i] = edvr::temporalChainAgrees(ch, cands[i]);
        if (chainOk[i]) anyChainOk = true;
        else f.refused = true;
    }
    if (gated) {
        for (int i = 0; i < n; ++i) {
            if (orient[i] && !chainOk[i]) reOk[i] = edvr::temporalChainReanchors(ch, cands[i]);
        }
    }
    int cont = -1;
    for (int i = 0; i < n; ++i) {
        if (orient[i] && (!gated || chainOk[i] || (!anyChainOk && reOk[i]))) cont = i;
    }
    f.pick = cont >= 0 ? cont : n - 1;
    if (!gated) return f;
    f.outcome = edvr::temporalChainCommit(ch, cands[f.pick]);
    f.guard = edvr::temporalChainGuard(ch, f.outcome);
    return f;
}

// One frame through the gate, as temporal_pass.cpp runs it, with the head
// taken as still (the delta's angle from the head is the rows' own turn).
edvr::TemporalCameraStep gateFrame(edvr::TemporalCameraGate& g, const float prev[12],
                                   const float now[12], bool guard, float W[9], float tv[3]) {
    float mv[3];
    edvr::temporalWorldFromRows(prev, now, W, tv, mv);
    const double move = sqrt(static_cast<double>(mv[0]) * mv[0] +
                             static_cast<double>(mv[1]) * mv[1] +
                             static_cast<double>(mv[2]) * mv[2]);
    const float diff = edvr::temporalRotationAngleDeg(W);
    const edvr::TemporalCameraStep s = edvr::temporalCameraGateStep(g, prev, now, move, diff, W, tv, guard);
    edvr::temporalCameraGateAdvance(g, true);
    return s;
}

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
    // CAMERA CONTINUITY (temporal_math.h, fix.camera_continuity): the chain's
    // test, the re-anchor, the commit and the guard, through continuityFrame
    // (the chooser's glue) and gateFrame (the gate). The view's camera stands
    // at P; the excursions are other cameras' rows. Rows turn 0.04 deg a frame.
    {
        const float P[3] = {-3.0f, -1.8f, -13.0f};
        float cs[2][12];
        // A. A steady chain with one-frame excursions: 75 m / 2.9 deg, 20 m / 2.2 deg
        // and 0.14 m / 2 deg (frames 10, 20, 30). Each is refused, the chain
        // keeps the view's camera on every frame, and the delta carries no error.
        {
            edvr::TemporalCameraChain ch;
            edvr::TemporalCameraGate gate;
            float prev[12] = {};
            bool prevValid = false;
            int pickedView = 0, guards = 0, badMotion = 0, own = 0;
            int refusedAt[3] = {0, 0, 0};
            for (int k = 0; k < 60; ++k) {
                rows34(0.04f * k, P[0], P[1], P[2], cs[0]);
                int n = 1;
                if (k == 10 || k == 20 || k == 30) {
                    const float dist = k == 10 ? 75.0f : (k == 20 ? 20.0f : 0.14f);
                    const float deg = k == 10 ? 2.9f : (k == 20 ? 2.2f : 2.0f);
                    rows34(0.04f * k + deg, P[0] + dist, P[1], P[2], cs[1]);
                    n = 2;
                    if (!edvr::temporalChainAgrees(ch, cs[1])) ++refusedAt[(k - 10) / 10];
                }
                const ContinuityFrame f = continuityFrame(ch, prev, prevValid, cs, n, true);
                if (f.pick == 0) ++pickedView;
                if (f.guard) ++guards;
                if (prevValid) {
                    float W[9], tv[3];
                    const edvr::TemporalCameraStep s = gateFrame(gate, prev, cs[f.pick], f.guard, W, tv);
                    if (s.verdict == edvr::TemporalCameraVerdict::Own) ++own;
                    const float tvLen = sqrtf(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
                    if (edvr::temporalRotationAngleDeg(W) > 0.1f || tvLen > 1e-3f) ++badMotion;
                }
                memcpy(prev, cs[f.pick], sizeof(prev));
                prevValid = true;
            }
            check(refusedAt[0] && refusedAt[1] && refusedAt[2],
                  "each excursion (75 m / 2.9 deg, 20 m / 2.2 deg, 0.14 m / 2 deg) is refused by the chain's test");
            check(pickedView == 60 && guards == 0,
                  "a steady chain keeps the view's camera on all 60 frames, no guard");
            check(own == 59 && badMotion == 0,
                  "with the excursions refused, every delta is the view's own and carries no motion error");
        }
        // A2. The key off (today's chooser): the latest orientation-continuous
        // write is picked, so the excursion's 2.9 deg error enters the delta on
        // its frame and carries back on the next (the out-and-back).
        {
            edvr::TemporalCameraChain unused;
            edvr::TemporalCameraGate gate;
            float prev[12] = {};
            bool prevValid = false;
            float errOn = 0.0f, errBack = 0.0f;
            for (int k = 0; k <= 11; ++k) {
                rows34(0.04f * k, P[0], P[1], P[2], cs[0]);
                int n = 1;
                if (k == 10) {
                    rows34(0.04f * k + 2.9f, P[0] + 75.0f, P[1], P[2], cs[1]);
                    n = 2;
                }
                const ContinuityFrame f = continuityFrame(unused, prev, prevValid, cs, n, false);
                if (k == 10) check(f.pick == 1 && !f.guard, "key off: the excursion is picked, as before");
                if (prevValid) {
                    float W[9], tv[3];
                    gateFrame(gate, prev, cs[f.pick], false, W, tv);
                    if (k == 10) errOn = edvr::temporalRotationAngleDeg(W);
                    if (k == 11) errBack = edvr::temporalRotationAngleDeg(W);
                }
                memcpy(prev, cs[f.pick], sizeof(prev));
                prevValid = true;
            }
            check(errOn > 2.5f && errBack > 2.5f,
                  "key off: the 2.9 deg excursion error enters the delta on its frame and comes back on the next");
        }
        // B. A straight flight at 300 m a frame is accepted from its second frame.
        {
            edvr::TemporalCameraChain ch;
            float prev[12] = {};
            bool prevValid = false;
            bool allChain = true;
            int picked = 0;
            for (int k = 0; k < 40; ++k) {
                rows34(0.0f, P[0] + 300.0f * k, P[1], P[2], cs[0]);
                const ContinuityFrame f = continuityFrame(ch, prev, prevValid, cs, 1, true);
                if (k > 0 && f.outcome != edvr::TemporalChainOutcome::Chain) allChain = false;
                if (f.pick == 0) ++picked;
                memcpy(prev, cs[f.pick], sizeof(prev));
                prevValid = true;
            }
            check(allChain && picked == 40, "a straight flight at 300 m a frame is accepted by the chain");
        }
        // C. An accelerating flight (100 m a frame, gaining 5 m a frame) is accepted.
        {
            edvr::TemporalCameraChain ch;
            float prev[12] = {};
            bool prevValid = false;
            bool none = true;
            float x = 0.0f, step = 100.0f;
            for (int k = 0; k < 40; ++k) {
                rows34(0.0f, P[0] + x, P[1], P[2], cs[0]);
                const ContinuityFrame f = continuityFrame(ch, prev, prevValid, cs, 1, true);
                if (k > 0 && f.outcome == edvr::TemporalChainOutcome::Departed) none = false;
                memcpy(prev, cs[f.pick], sizeof(prev));
                prevValid = true;
                x += step;
                step += 5.0f;
            }
            check(none, "an accelerating flight (100 m a frame, gaining 5 m a frame) is accepted by the chain");
        }
        // D. A real cut to a new pose (500 m, 30 deg) held from frame 10: the first
        // frame is held out of the chain and its delta carried, the second frame
        // re-anchors the chain, and from the third the chain follows the new pose.
        {
            edvr::TemporalCameraChain ch;
            float prev[12] = {};
            bool prevValid = false;
            edvr::TemporalChainOutcome o[13] = {};
            bool g[13] = {};
            for (int k = 0; k < 13; ++k) {
                if (k < 10) rows34(0.0f, P[0], P[1], P[2], cs[0]);
                else rows34(30.0f, P[0] + 500.0f, P[1], P[2], cs[0]);
                const ContinuityFrame f = continuityFrame(ch, prev, prevValid, cs, 1, true);
                o[k] = f.outcome;
                g[k] = f.guard;
                memcpy(prev, cs[f.pick], sizeof(prev));
                prevValid = true;
            }
            check(o[10] == edvr::TemporalChainOutcome::Departed && g[10],
                  "a real cut's first frame is held out of the chain and its delta carried");
            check(o[11] == edvr::TemporalChainOutcome::Reanchored,
                  "a cut held for a second frame re-anchors the chain on that frame");
            check(o[12] == edvr::TemporalChainOutcome::Chain && !g[12],
                  "from the third frame the chain follows the new pose, with no guard");
        }
        // E. The floating origin moves 1000 m in one frame. The chain holds it
        // out, the second frame re-anchors, and the gate's own jump rule (the
        // translation dropped, the turn kept) is unchanged with the guard off.
        {
            edvr::TemporalCameraChain ch;
            float prev[12] = {};
            bool prevValid = false;
            edvr::TemporalChainOutcome o10 = edvr::TemporalChainOutcome::Anchored, o11 = o10;
            bool g10 = false;
            for (int k = 0; k < 12; ++k) {
                rows34(0.0f, P[0] + (k >= 10 ? 1000.0f : 0.0f), P[1], P[2], cs[0]);
                const ContinuityFrame f = continuityFrame(ch, prev, prevValid, cs, 1, true);
                if (k == 10) { o10 = f.outcome; g10 = f.guard; }
                if (k == 11) o11 = f.outcome;
                memcpy(prev, cs[f.pick], sizeof(prev));
                prevValid = true;
            }
            check(o10 == edvr::TemporalChainOutcome::Departed && g10,
                  "an origin move's first frame is held out of the chain and its delta carried");
            check(o11 == edvr::TemporalChainOutcome::Reanchored,
                  "after an origin move the chain re-anchors on the second frame");
            float a[12], b[12], W[9], tv[3];
            rows34(0.0f, P[0], P[1], P[2], a);
            rows34(0.0f, P[0] + 1000.0f, P[1], P[2], b);
            edvr::TemporalCameraGate jg;
            jg.refOwn = true;   // the rows before the jump are the view's own, as in a running session
            gateFrame(jg, a, a, false, W, tv);
            const edvr::TemporalCameraStep js = gateFrame(jg, a, b, false, W, tv);
            check(js.jump && js.verdict == edvr::TemporalCameraVerdict::Own &&
                  tv[0] == 0.0f && tv[1] == 0.0f && tv[2] == 0.0f,
                  "an origin move with the guard off keeps the gate's jump rule (translation dropped, turn kept)");
        }
        // F. The guard: a departed pick's delta (here a 75 m excursion, if it were
        // picked) is carried as the last good, whatever its own size.
        {
            float still[12], ex[12], W[9], tv[3];
            rows34(0.0f, P[0], P[1], P[2], still);
            rows34(2.9f, P[0] + 75.0f, P[1], P[2], ex);
            edvr::TemporalCameraGate gg;
            gg.refOwn = true;   // the rows before the excursion are the view's own
            gateFrame(gg, still, still, false, W, tv);
            const edvr::TemporalCameraStep s = gateFrame(gg, still, ex, true, W, tv);
            check(s.verdict == edvr::TemporalCameraVerdict::Departed && s.carried &&
                  memcmp(W, gg.lastGoodC, sizeof(W)) == 0 &&
                  memcmp(tv, gg.lastGoodTv, sizeof(tv)) == 0,
                  "the guard carries a departed delta as the last good, the turn and the translation both");
        }
    }
    if (g_fails) {
        printf("\nTEMPORAL TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nTEMPORAL TEST PASSED\n");
    return 0;
}
