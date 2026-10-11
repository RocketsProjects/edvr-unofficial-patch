// Test rig for the sealed-hangar trial's pure half (src/d3d11/sealed_trial_core.h): the sphere rule (intersection with the
// camera's sphere of radius R), the keep-if-contains rule, the radius parse and clamp, the statistics and toggle lines.
// No DLL and no game.
#include "../../src/d3d11/sealed_trial_core.h"

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

    // --- The radius: parsed, clamped, defaulted.
    {
        float r = 0;
        check(se::parseRadius("", &r) && r == se::kDefaultRadiusM, "an empty radius is the default, 80 m");
        check(se::parseRadius("120.5", &r) && std::fabs(r - 120.5f) < 1e-4f, "a radius in metres is read");
        check(se::parseRadius("0.2", &r) && r == se::kMinRadiusM, "a radius below the floor is the floor");
        check(se::parseRadius("99999", &r) && r == se::kMaxRadiusM, "a radius above the cap is the cap");
        check(!se::parseRadius("abc", &r) && r == se::kDefaultRadiusM, "a word is the default and says so");
        check(!se::parseRadius("12m", &r) && r == se::kDefaultRadiusM, "a number with a unit suffix is not read");
        check(!se::parseRadius("nan", &r) && r == se::kDefaultRadiusM, "NaN is refused to the default");
    }

    // --- The statistics line.
    {
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
        check(s.find("window 10 s culled=12 kept=3 kept_unjudged=1 sun_pairs_skipped=40") != std::string::npos,
              "the window line carries the counts");
        check(s.find("eval_ms=2.500") != std::string::npos && s.find("radius_m=80.0") != std::string::npos,
              "the window line carries the time and the radius");
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

    std::printf("sealed_trial_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("sealed trial self-test passed\n");
    return 0;
}
