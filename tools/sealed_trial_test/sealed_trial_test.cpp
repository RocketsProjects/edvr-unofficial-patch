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
        se::formatWindow(line, sizeof(line), w, 80.0f, "eye rows");
        const std::string s = line;
        check(s.find("window 5 s culled=12 kept=3 kept_unjudged=1 sun_pairs_skipped=40") != std::string::npos,
              "the window line carries the counts");
        check(s.find("eval_ms=2.500") != std::string::npos && s.find("radius_m=80.0") != std::string::npos,
              "the window line carries the time and the radius");
        check(s.find("faults=0 torn=0 radius_m=80.0") != std::string::npos,
              "the window line carries the fault count and the torn count, in that order, before the radius");
        w.faults = 3;
        w.torn = 1;
        se::formatWindow(line, sizeof(line), w, 80.0f, "eye rows");
        check(std::string(line).find("faults=3 torn=1 radius_m=80.0") != std::string::npos,
              "a torn cull is counted in the window line");
        se::formatWindow(line, sizeof(line), w, 60.0f, "explorer cam");
        check(std::string(line).find("radius_m=60.0") != std::string::npos, "a window at 60 m says so");
        check(std::string(line).find("view source = explorer cam") != std::string::npos, "a window names its view source");
    }

    // --- The toggle line, on and off, with and without a camera.
    {
        char line[400];
        const float c[3] = {1.0f, 2.0f, 3.0f};
        se::formatToggle(line, sizeof(line), true, 4242, 80.0f, c, true, "vscreen");
        check(std::string(line).find("on at frame 4242 (radius 80.0 m; view source = vscreen; camera (1.0,2.0,3.0)") !=
                  std::string::npos,
              "the on line names the frame, radius, view source and camera");
        se::formatToggle(line, sizeof(line), true, 7, 80.0f, c, false, "eye rows");
        check(std::string(line).find("camera unknown") != std::string::npos, "the on line says when the camera is unknown");
        se::formatToggle(line, sizeof(line), false, 9, 80.0f, c, true, "eye rows");
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

    // --- The cull with its fault handling, against a fake record: each failure point leaves the record as the store wrote it (kept),
    //     except a failed put-back, which is named as torn.
    {
        struct FakeRecord {
            uint64_t mask = 0x3F;
            uint8_t admitted = 1;
            bool failMaskRead = false, failAdmittedRead = false, failMaskWrite = false, failAdmittedWrite = false, failRestore = false;
            int maskWrites = 0;
            bool readMask(uint64_t* v) {
                if (failMaskRead) return false;
                *v = mask;
                return true;
            }
            bool readAdmitted(uint8_t* v) {
                if (failAdmittedRead) return false;
                *v = admitted;
                return true;
            }
            bool writeMask(uint64_t v) {
                ++maskWrites;
                if (maskWrites == 1 && failMaskWrite) return false;
                if (maskWrites == 2 && failRestore) return false;
                mask = v;
                return true;
            }
            bool writeAdmitted(uint8_t v) {
                if (failAdmittedWrite) return false;
                admitted = v;
                return true;
            }
        };
        se::CullField f = se::CullField::kMaskRead;
        uint64_t val = 0;
        {
            FakeRecord r;
            const se::Cull c = se::cullRecordFaultSafe(r, &f, &val);
            check(c == se::Cull::kCulled && r.mask == 0 && r.admitted == 0, "a clean cull clears the mask and the admitted flag");
        }
        {
            FakeRecord r;
            r.failMaskRead = true;
            const se::Cull c = se::cullRecordFaultSafe(r, &f, &val);
            check(c == se::Cull::kUntouched && f == se::CullField::kMaskRead && r.mask == 0x3F && r.admitted == 1,
                  "a failed mask read leaves the record as the store wrote it");
        }
        {
            FakeRecord r;
            r.failAdmittedRead = true;
            const se::Cull c = se::cullRecordFaultSafe(r, &f, &val);
            check(c == se::Cull::kUntouched && f == se::CullField::kAdmittedRead && r.mask == 0x3F && r.admitted == 1,
                  "a failed admitted read leaves the record untouched");
        }
        {
            FakeRecord r;
            r.failMaskWrite = true;
            const se::Cull c = se::cullRecordFaultSafe(r, &f, &val);
            check(c == se::Cull::kUntouched && f == se::CullField::kMaskWrite && r.mask == 0x3F && r.admitted == 1,
                  "a failed mask write leaves the record untouched");
        }
        {
            FakeRecord r;
            r.failAdmittedWrite = true;
            const se::Cull c = se::cullRecordFaultSafe(r, &f, &val);
            check(c == se::Cull::kRestored && f == se::CullField::kAdmittedWrite && r.mask == 0x3F && r.admitted == 1,
                  "a failed admitted write puts the mask back: the record is kept");
        }
        {
            FakeRecord r;
            r.failAdmittedWrite = true;
            r.failRestore = true;
            const se::Cull c = se::cullRecordFaultSafe(r, &f, &val);
            check(c == se::Cull::kTorn && f == se::CullField::kRestoreWrite && r.mask == 0 && r.admitted == 1,
                  "a failed put-back is reported as torn, naming the field");
        }
    }

    // --- Built v8: the trial's mode and builder policy parse, the name rule (named mode), the window line's new fields.
    {
        se::Mode m = se::Mode::kNamed;
        check(se::parseMode("", &m) && m == se::Mode::kSphere, "an empty diag.sealed_trial_mode is sphere");
        check(se::parseMode("sphere", &m) && m == se::Mode::kSphere, "sphere parses");
        check(se::parseMode("named", &m) && m == se::Mode::kNamed, "named parses");
        check(!se::parseMode("Named", &m) && m == se::Mode::kSphere, "an unknown mode word is refused and the mode is sphere");
        check(se::nextMode(se::Mode::kSphere) == se::Mode::kNamed && se::nextMode(se::Mode::kNamed) == se::Mode::kSphere,
              "hotkey.sealed_trial_mode cycles sphere and named");
        check(std::strcmp(se::modeName(se::Mode::kNamed), "named") == 0 && std::strcmp(se::modeName(se::Mode::kSphere), "sphere") == 0,
              "the mode names are sphere and named");

        se::BuilderPolicy p = se::BuilderPolicy::kSkip;
        check(se::parseBuilderPolicy("", &p) && p == se::BuilderPolicy::kCount, "an empty diag.sealed_trial_builder is count");
        check(se::parseBuilderPolicy("skip", &p) && p == se::BuilderPolicy::kSkip, "skip parses (the caller refuses it)");
        check(!se::parseBuilderPolicy("drop", &p) && p == se::BuilderPolicy::kCount, "an unknown builder word is refused and is count");

        check(se::containsNoCase("HangerSmall", "hanger") && se::containsNoCase("BlastShield_A", "BLASTSHIELD"),
              "the keyword test ignores case");
        check(!se::containsNoCase("DockingHub", "Hangar") && !se::containsNoCase("", "Lift"), "a name without the word does not match");
        check(se::nameKeptByWord("Agricultural_SmallHangar") && se::nameKeptByWord("LandingPadSmall") &&
                  se::nameKeptByWord("HangarLiftDoors__") && se::nameKeptByWord("SmallHangerSkirt"),
              "Hangar, Hanger, LandingPad, Lift and Skirt are keep words");
        check(!se::nameKeptByWord("DockingHub") && !se::nameKeptByWord("HabitatRingA") && !se::nameKeptByWord(nullptr),
              "DockingHub and HabitatRingA are not keep words, and a null name is none");

        const float near100[3] = {150.0f, 0.0f, 0.0f};    // 150 m away
        const float big[3] = {0.0f, 0.0f, 0.0f};          // centred on the camera
        check(se::keepRecordNamed(false, nullptr, big, 2000.0f, cam, 80.0f), "named: a record with no readable name is kept");
        check(!se::keepRecordNamed(true, "DockingHub", big, 2000.0f, cam, 80.0f),
              "named: a giant record that contains the camera is culled when its name is not a keep word");
        check(se::keepRecordNamed(true, "HangerSmall", near100, 2000.0f, cam, 80.0f),
              "named: a keep word keeps the record whatever its sphere");
        check(se::keepRecordNamed(true, "Panel", near100, 90.0f, cam, 80.0f),
              "named: a 90 m record meeting the trial sphere (150 <= 90 + 80) is kept");
        check(!se::keepRecordNamed(true, "Panel", near100, 120.0f, cam, 80.0f),
              "named: a record over 100 m across is culled even when its sphere meets the trial sphere");
        const float far200[3] = {200.0f, 0.0f, 0.0f};
        check(!se::keepRecordNamed(true, "Panel", far200, 50.0f, cam, 80.0f),
              "named: a 50 m record 200 m out, past 50 + 80, is culled");
        check(se::keepRecordForMode(se::Mode::kSphere, true, "DockingHub", big, 2000.0f, cam, 80.0f),
              "sphere mode ignores the name: the same giant record is kept");
        check(!se::keepRecordForMode(se::Mode::kNamed, true, "DockingHub", big, 2000.0f, cam, 80.0f),
              "named mode on the same record culls it");

        se::Window w;
        w.builderAfterCull = 3;
        w.builderUnvisited = 5;
        char line[640];
        se::formatWindow(line, sizeof(line), w, 80.0f, "eye rows", se::Mode::kNamed);
        check(contains(line, "radius_m=80.0 builder_after_cull=3 builder_unvisited=5 mode=named view source = eye rows"),
              "the window line appends builder_after_cull, builder_unvisited and the mode before the view source");
        se::formatWindow(line, sizeof(line), w, 80.0f, "eye rows");
        check(contains(line, "builder_unvisited=5 mode=sphere"), "the window line defaults to mode sphere");

        se::formatModeLine(line, sizeof(line), se::Mode::kNamed, 12, "hotkey.sealed_trial_mode");
        check(contains(line, "mode = named at frame 12 (trigger hotkey.sealed_trial_mode)") && contains(line, "Hangar"),
              "the named mode line names the rule and its keep words");
        se::formatModeLine(line, sizeof(line), se::Mode::kSphere, 13, "hotkey.sealed_trial_mode");
        check(contains(line, "mode = sphere at frame 13"), "the sphere mode line says sphere");
    }

    std::printf("sealed_trial_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("sealed trial self-test passed\n");
    return 0;
}
