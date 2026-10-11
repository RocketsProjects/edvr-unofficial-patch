// Test rig for the sealed-hangar trial's pure half (src/d3d11/sealed_trial_core.h): the sphere rule (intersection with the
// camera's sphere of radius R), the keep-if-contains rule, the radius parse and clamp, the radius ladder (its step table, a step
// in each direction from every rung and from off-ladder radii, and the clamps at 80 m and 10 m), and the window, step and toggle
// lines. No DLL and no game.
#include "../../src/d3d11/sealed_trial_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace se = edvr::sealed;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool contains(const char* line, const char* part) { return std::strstr(line, part) != nullptr; }

}  // namespace

int main(int argc, char** argv) {
    const bool selfTest = argc > 1 && std::strcmp(argv[1], "--self-test") == 0;
    if (!selfTest) {
        std::printf("usage: sealed_trial_test --self-test\n");
        return 2;
    }

    const float cam[3] = {0.0f, 0.0f, 0.0f};

    // --- The sphere rule: a record outside R + its radius is culled; one inside is kept.
    {
        const float far[3] = {100.0f, 0.0f, 0.0f};
        check(!se::intersectsCameraSphere(far, 10.0f, cam, 80.0f), "a record 100 m away, radius 10, R 80: no intersection");
        check(!se::keepRecord(far, 10.0f, cam, 80.0f), "...so it is culled");
        const float near[3] = {85.0f, 0.0f, 0.0f};
        check(se::intersectsCameraSphere(near, 10.0f, cam, 80.0f), "a record 85 m away, radius 10, R 80: intersects (85 <= 90)");
        check(se::keepRecord(near, 10.0f, cam, 80.0f), "...so it is kept");
        const float edge[3] = {90.0f, 0.0f, 0.0f};
        check(se::intersectsCameraSphere(edge, 10.0f, cam, 80.0f), "touching spheres intersect (90 <= 90)");
        const float justOut[3] = {90.5f, 0.0f, 0.0f};
        check(!se::intersectsCameraSphere(justOut, 10.0f, cam, 80.0f), "a gap of 0.5 m does not intersect");
    }

    // --- Keep-if-contains: a sphere that contains the camera is kept whatever R is, and whatever its centre distance.
    {
        const float hullCentre[3] = {300.0f, 0.0f, 0.0f};
        check(!se::containsCamera(hullCentre, 200.0f, cam), "a sphere of radius 200 at 300 m does not contain the camera");
        check(se::containsCamera(hullCentre, 400.0f, cam), "a sphere of radius 400 at 300 m contains the camera");
        check(se::keepRecord(hullCentre, 400.0f, cam, 1.0f), "a camera-containing hull is kept even with R = 1 m");
        const float hangarCentre[3] = {0.0f, 0.0f, 0.0f};
        check(se::keepRecord(hangarCentre, 5.0f, cam, 1.0f), "a record centred on the camera is kept");
        check(!se::keepRecord(hullCentre, 200.0f, cam, 1.0f), "a record 300 m away, radius 200, R 1: culled");
    }

    // --- The radius: parsed, clamped at both ends, defaulted.
    {
        float r = 0;
        check(se::parseRadius("", &r) && r == se::kDefaultRadiusM, "an empty radius is the default, 80 m");
        check(se::parseRadius("120.5", &r) && std::fabs(r - 120.5f) < 1e-4f, "a radius in metres is read");
        check(se::parseRadius("0.2", &r) && r == se::kMinRadiusM, "a radius below the floor is the floor");
        check(se::parseRadius("1", &r) && r == 1.0f, "a radius of 1 m is the floor itself");
        check(se::parseRadius("99999", &r) && r == se::kMaxRadiusM, "a radius above the cap is the cap");
        check(se::parseRadius("5000", &r) && r == 5000.0f, "a radius of 5000 m is the cap itself");
        check(!se::parseRadius("abc", &r) && r == se::kDefaultRadiusM, "a word is the default and says so");
        check(!se::parseRadius("12m", &r) && r == se::kDefaultRadiusM, "a number with a unit suffix is not read");
        check(!se::parseRadius("nan", &r) && r == se::kDefaultRadiusM, "NaN is refused to the default");
    }

    // --- The window line: every five seconds, the counts, the time and the radius it ran at.
    {
        check(se::kWindowMs == 5000, "the window is 5 s");
        se::Window w;
        w.culled = 12;
        w.kept = 3;
        w.keptUnknown = 1;
        w.sunSkipped = 40;
        w.evalNs = 2500000;   // 2.5 ms
        w.faults = 0;
        char line[400];
        se::formatWindow(line, sizeof(line), w, 80.0f);
        const std::string s = line;
        check(s.find("window 5 s culled=12 kept=3 kept_unjudged=1 sun_pairs_skipped=40") != std::string::npos,
              "the window line carries the counts");
        check(s.find("eval_ms=2.500") != std::string::npos && s.find("radius_m=80.0") != std::string::npos,
              "the window line carries the time and the radius");
        se::formatWindow(line, sizeof(line), w, 60.0f);
        check(std::string(line).find("radius_m=60.0") != std::string::npos, "a window at 60 m says so");
    }

    // --- The toggle line, on and off, with and without a camera.
    {
        char line[400];
        const float c[3] = {1.0f, 2.0f, 3.0f};
        se::formatToggle(line, sizeof(line), true, 4242, 80.0f, c, true);
        check(std::string(line).find("on at frame 4242 (radius 80.0 m; camera (1.0,2.0,3.0)") != std::string::npos,
              "the on line names the frame, radius and camera");
        se::formatToggle(line, sizeof(line), true, 7, 80.0f, c, false);
        check(std::string(line).find("camera unknown") != std::string::npos, "the on line says when the camera is unknown");
        se::formatToggle(line, sizeof(line), false, 9, 80.0f, c, true);
        check(std::string(line) == "sealed trial: off at frame 9", "the off line");
    }

    // --- The ladder: seven rungs, largest first, 80 down to 10 m.
    {
        const float expect[7] = {80.0f, 60.0f, 40.0f, 30.0f, 20.0f, 15.0f, 10.0f};
        check(se::kStepCount == 7, "the ladder has seven rungs");
        bool table = se::kStepCount == 7;
        for (uint32_t i = 0; table && i < se::kStepCount; ++i)
            if (se::kSteps[i] != expect[i]) table = false;
        check(table, "the ladder is 80, 60, 40, 30, 20, 15, 10 m, largest first");
    }

    // --- A step down from every rung moves to the next rung; a step up from every rung moves back to the previous one.
    {
        bool downOk = true, upOk = true;
        for (uint32_t i = 0; i + 1 < se::kStepCount; ++i) {
            bool moved = false;
            const float next = se::stepRadius(se::kSteps[i], -1, &moved);
            if (!moved || next != se::kSteps[i + 1]) downOk = false;
        }
        for (uint32_t i = 1; i < se::kStepCount; ++i) {
            bool moved = false;
            const float prev = se::stepRadius(se::kSteps[i], +1, &moved);
            if (!moved || prev != se::kSteps[i - 1]) upOk = false;
        }
        check(downOk, "smaller moves down one rung from every rung but the last");
        check(upOk, "larger moves up one rung from every rung but the first");
    }

    // --- Clamps at both ends: no rung that way, so the radius stays and nothing moves.
    {
        bool moved = true;
        float r = se::stepRadius(10.0f, -1, &moved);
        check(r == 10.0f && !moved, "smaller at 10 m is clamped: no move");
        moved = true;
        r = se::stepRadius(80.0f, +1, &moved);
        check(r == 80.0f && !moved, "larger at 80 m is clamped: no move");
        float down = 80.0f;
        int moves = 0;
        for (int k = 0; k < 9; ++k) {
            bool m = false;
            down = se::stepRadius(down, -1, &m);
            if (m) ++moves;
        }
        check(down == 10.0f && moves == 6, "nine presses down from 80 m stop at 10 m after six moves");
        float up = 10.0f;
        moves = 0;
        for (int k = 0; k < 9; ++k) {
            bool m = false;
            up = se::stepRadius(up, +1, &m);
            if (m) ++moves;
        }
        check(up == 80.0f && moves == 6, "nine presses up from 10 m stop at 80 m after six moves");
    }

    // --- Off the ladder: a step lands on the nearest rung in its direction; with no rung that way, the radius stays.
    {
        bool m = false;
        check(se::stepRadius(50.0f, -1, &m) == 40.0f && m, "50 m smaller lands on 40 m");
        check(se::stepRadius(50.0f, +1, &m) == 60.0f && m, "50 m larger lands on 60 m");
        m = true;
        check(se::stepRadius(200.0f, -1, &m) == 80.0f && m, "200 m smaller lands on 80 m, the largest rung");
        check(se::stepRadius(200.0f, +1, &m) == 200.0f && !m, "200 m larger has no rung above: no move");
        m = true;
        check(se::stepRadius(5.0f, +1, &m) == 10.0f && m, "5 m larger lands on 10 m, the smallest rung");
        check(se::stepRadius(5.0f, -1, &m) == 5.0f && !m, "5 m smaller has no rung below: no move");
    }

    // --- The step lines: a move names the new radius and the frame; a clamp names the end of the ladder; off, a refusal.
    {
        char line[320];
        se::formatStep(line, sizeof(line), -1, true, 60.0f, 7);
        check(std::string(line) == "sealed trial: radius -> 60.0 m at frame 7", "a step line names the radius and the frame");
        se::formatStep(line, sizeof(line), +1, true, 80.0f, 8);
        check(contains(line, "radius -> 80.0 m at frame 8"), "a step up line names its radius and frame");
        se::formatStep(line, sizeof(line), -1, false, 10.0f, 9);
        check(contains(line, "radius stays 10.0 m at frame 9") && contains(line, "smallest step is 10.0 m"),
              "the clamp at the bottom names the smallest step");
        se::formatStep(line, sizeof(line), +1, false, 80.0f, 10);
        check(contains(line, "radius stays 80.0 m at frame 10") && contains(line, "largest step is 80.0 m"),
              "the clamp at the top names the largest step");
        se::formatStepRefused(line, sizeof(line), 40.0f, 11);
        check(contains(line, "act only while the trial is on") && contains(line, "radius 40.0 m at frame 11") &&
                  contains(line, "nothing changed"),
              "a step key with the trial off is refused and says nothing changed");
    }

    std::printf("sealed_trial_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("sealed trial self-test passed\n");
    return 0;
}
