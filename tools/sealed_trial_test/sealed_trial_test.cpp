// Test rig for the sealed-hangar trial's pure half (src/d3d11/sealed_trial_core.h): the sphere rule (intersection with the
// camera's sphere of radius R), the keep-if-contains rule, the radius parse and clamp, the radius ladder (its step table, a step
// in each direction from every rung and from off-ladder radii, and the clamps at 80 m and 10 m), and the window, step and toggle
// lines. No DLL and no game.
#include "../../src/d3d11/sealed_trial_core.h"

#include <cmath>
#include <cstdio>
#include <limits>
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
        char line[1400];
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
        char line[1400];
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
        check(se::nextMode(se::Mode::kSphere) == se::Mode::kNamed && se::nextMode(se::Mode::kNamed) == se::Mode::kAll &&
                  se::nextMode(se::Mode::kAll) == se::Mode::kSphere,
              "hotkey.sealed_trial_mode cycles sphere, named, all, sphere");
        check(std::strcmp(se::modeName(se::Mode::kNamed), "named") == 0 && std::strcmp(se::modeName(se::Mode::kSphere), "sphere") == 0 &&
                  std::strcmp(se::modeName(se::Mode::kAll), "all") == 0,
              "the mode names are sphere, named and all");
        // Built v8.5: the all mode parses; the pointer kind at rec+0x290 classes null, image, other and unreadable.
        se::Mode am = se::Mode::kSphere;
        check(se::parseMode("all", &am) && am == se::Mode::kAll, "all parses (diag.sealed_trial_mode = all)");
        check(se::ptrKindOf(true, 0, 0x1000, 0x2000) == se::PtrKind::kNull, "a zero pointer is null");
        check(se::ptrKindOf(true, 0x1800, 0x1000, 0x2000) == se::PtrKind::kImage, "a pointer inside the image is image");
        check(se::ptrKindOf(true, 0x2000, 0x1000, 0x2000) == se::PtrKind::kOther, "the image end is exclusive: a pointer there is other");
        check(se::ptrKindOf(true, 0x9000, 0x1000, 0x2000) == se::PtrKind::kOther, "a pointer outside the image is other");
        check(se::ptrKindOf(false, 0x1800, 0x1000, 0x2000) == se::PtrKind::kUnread, "a failed read is unreadable, whatever the value");
        check(std::strcmp(se::ptrKindName(se::PtrKind::kOther), "other") == 0, "the pointer kind name is other");
        // Built v11: the capsule cell. The segment distance, the capsule's axis and its extension, the target pick (the BlastShieldBase first,
        // within the reach), the named keep through a capsule (a record half way up the axis is in the cell; with a sphere on the pad it is not).
        {
            const float p[3] = {0, 0, 0}, q[3] = {0, 100, 0}, far[3] = {0, 120, 0};
            const float beside[3] = {5, 5, 0}, pastEnd[3] = {-3, -4, 0};   // (5,5,0) is 5 m off the axis; (-3,-4,0) projects before the pad end
            check(std::fabs(se::distToSegment(beside, p, far) - 5.0f) < 1e-4f, "a point beside the middle of a segment is its distance");
            check(std::fabs(se::distToSegment(pastEnd, p, q) - 5.0f) < 1e-4f, "a point past an end is its distance to that end");
            float a[3] = {}, b[3] = {}, len = 0.0f;
            check(se::buildCapsule(p, q, 20.0f, a, b, &len) && std::fabs(len - 100.0f) < 1e-4f && std::fabs(b[1] - 120.0f) < 1e-4f && b[0] == 0.0f,
                  "the capsule runs from the pad to 20 m past a 100 m target");
            check(!se::buildCapsule(p, p, 20.0f, a, b, &len), "a target at the pad is no capsule");
            se::ExplicitObs obs[3];
            obs[0].centre[0] = 0; obs[0].centre[1] = 50; obs[0].centre[2] = 0;
            std::snprintf(obs[0].name, sizeof(obs[0].name), "HangerMedium");
            obs[1].centre[0] = 0; obs[1].centre[1] = 120; obs[1].centre[2] = 0;
            std::snprintf(obs[1].name, sizeof(obs[1].name), "BlastShieldBase");
            obs[2].centre[0] = 0; obs[2].centre[1] = 30; obs[2].centre[2] = 0;
            std::snprintf(obs[2].name, sizeof(obs[2].name), "SmallHangerSkirt");
            float tq[3] = {};
            char tn[48] = {};
            check(se::pickCapsuleTarget(obs, 3, p, tq, tn, sizeof(tn)) && std::strcmp(tn, "BlastShieldBase") == 0 && tq[1] == 120.0f,
                  "the nearest BlastShieldBase is the target, even past a nearer hanger");
            obs[1].centre[1] = 500.0f;   // beyond the reach: the own hanger is the target
            check(se::pickCapsuleTarget(obs, 3, p, tq, tn, sizeof(tn)) && std::strcmp(tn, "HangerMedium") == 0,
                  "beyond the reach a BlastShieldBase is not the target and the own hanger is");
            obs[0].centre[1] = 500.0f;
            check(!se::pickCapsuleTarget(obs, 3, p, tq, tn, sizeof(tn)), "no target within the reach is no capsule");
            check(se::isBlastBase("BlastShieldBase") && !se::isBlastBase("LandingPadSmallA") && se::isOwnHanger("HangerMedium") &&
                  !se::isOwnHanger("SmallHangerSkirt"), "the name rules: BlastShieldBase, own hangers, not the skirts");
            // The named keep: a capsule from the pad (0,0,0) to (0,120,0) with radius 10 keeps a record half way up it; a sphere of the same
            // radius on the pad does not.
            se::OwnCell cell;
            cell.has = true;
            cell.radius = 10.0f;
            cell.centre[0] = 0; cell.centre[1] = 0; cell.centre[2] = 0;
            const float cam[3] = {0, 0, 0};
            const float half[3] = {0, 60, 0}, pad[3] = {0, 0, 0};
            const uintptr_t noset[1] = {0};
            check(!se::keepNamedCell(half, 1.0f, cam, 5.0f, cell, noset, 0, 1).keep, "a sphere on the pad does not hold a record half way up");
            cell.capsule = true;
            cell.capA[0] = 0; cell.capA[1] = 0; cell.capA[2] = 0;
            cell.capB[0] = 0; cell.capB[1] = 120; cell.capB[2] = 0;
            const se::NamedKeep kc = se::keepNamedCell(half, 1.0f, cam, 5.0f, cell, noset, 0, 1);
            check(kc.keep && kc.why == se::KeepWhy::kCell, "the capsule keeps a record half way up the axis, by cell");
            check(se::keepNamedCell(pad, 1.0f, cam, 5.0f, cell, noset, 0, 1).keep, "the capsule keeps the pad's own centre");
            const float aside[3] = {30, 60, 0};
            check(!se::keepNamedCell(aside, 1.0f, cam, 5.0f, cell, noset, 0, 1).keep, "a record 30 m off the axis is culled");
            // The explicit picks with a capsule: within 15 m of the axis only (addendum). A part 10 m off the axis is picked; 30 m off is not.
            se::ExplicitObs parts[2];
            parts[0].addr = 11; parts[0].node = 1; parts[0].centre[0] = 10; parts[0].centre[1] = 60; parts[0].centre[2] = 0;
            parts[1].addr = 22; parts[1].node = 2; parts[1].centre[0] = 30; parts[1].centre[1] = 60; parts[1].centre[2] = 0;
            uintptr_t picked[4] = {};
            const size_t np = se::pickExplicitSegment(parts, 2, cell.capA, cell.capB, picked, 4);
            check(np == 1 && picked[0] == 11, "an explicit part 10 m off the axis is picked; one 30 m off is not (the 15 m limit)");
            // Built v11 (second addendum): the oriented box. Identity axes; the pad at the origin, the target 100 m up the shaft (y); the length
            // axis x; the half-extents 66 + 5 (length) and 27.5 + 5 (width); the box runs from 10 m under the pad to 20 m past the target.
            const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            float axI[3][3] = {};
            const float qId[4] = {0, 0, 0, 1};
            se::quatAxes(qId, axI);
            check(axI[0][0] == 1.0f && axI[1][1] == 1.0f && axI[2][2] == 1.0f && axI[0][1] == 0.0f && axI[2][0] == 0.0f,
                  "the identity quaternion's axes are the world axes");
            float axS[3][3] = {};
            const float aI[15] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0};
            se::storeAxes(aI, ident, axS);
            check(axS[0][0] == 1.0f && axS[1][1] == 1.0f && axS[2][2] == 1.0f && axS[1][0] == 0.0f, "the store-style identity pose has world axes");
            se::OwnCell box;
            const float padC[3] = {0, 0, 0}, tgt[3] = {0, 100, 0}, lenAx[3] = {1, 0, 0};
            check(se::buildBox(padC, tgt, lenAx, 66.0f, 27.5f, 5.0f, 10.0f, &box) && box.box, "a box builds along the shaft");
            check(box.boxHalf[0] == 71.0f && box.boxHalf[1] == 32.5f && box.boxHalf[2] == 65.0f, "the half-extents are 71 (length), 32.5 (width), 65 (height)");
            check(box.boxCentre[1] == 55.0f && box.boxCentre[0] == 0.0f, "the box centre is 55 m up the shaft from the pad");
            const float in1[3] = {60, 55, 0}, out1[3] = {75, 55, 0}, bottomIn[3] = {0, -10, 0}, bottomOut[3] = {0, -11, 0}, topIn[3] = {0, 120, 0},
                        topOut[3] = {0, 121, 0};
            check(se::inBox(in1, box) && !se::inBox(out1, box), "a point 60 m across the length is in, 75 m is out");
            check(se::inBox(bottomIn, box) && !se::inBox(bottomOut, box), "the box's floor is 10 m under the pad");
            check(se::inBox(topIn, box) && !se::inBox(topOut, box), "the box's top is 20 m past the target");
            float bc[3] = {};
            se::boxCoords(in1, box, bc);
            check(bc[0] == 60.0f && bc[1] == 0.0f && std::fabs(bc[2]) < 1e-4f, "the box coordinates of a point 60 m across are (60, 0, 0)");
            // The footprint: the up axis is the one along the shaft; the length axis is the footprint axis with the larger extent.
            const float up[3] = {0, 1, 0};
            int li = -1, wi = -1;
            const float extA[3] = {10, 5, 3}, extB[3] = {3, 5, 10};
            int ui = -1;
            check(se::footprintAxes(axI, up, extA, &li, &wi, &ui) && li == 0 && wi == 2 && ui == 1,
                  "the larger extent of the two footprint axes is the length; the up axis is the one along the shaft");
            check(se::footprintAxes(axI, up, extB, &li, &wi, &ui) && li == 2 && wi == 0 && ui == 1, "the other way round when the extents say so");
            check(!se::footprintAxes(axI, up, nullptr, &li, &wi, &ui), "no extent, no footprint: the box is not built");
            // Built v12 (flight fix a): the box stands on the up axis, centred laterally on the pad. A target 40 m off the up axis leaves the
            // box where it was and records the 40 m lateral offset.
            se::OwnCell lat;
            // v12.1: the axis is pad -> target. A target 80 m up and 60 m across: the axis is (0.6, 0.8, 0), the box's centre is 55 m along it
            // from the pad, the length axis is (0.8, -0.6, 0) (lenAx made orthogonal to the axis), and the half-height is 65 m.
            const float tgtDiag[3] = {60, 80, 0};
            check(se::buildBox(padC, tgtDiag, lenAx, 66.0f, 27.5f, 5.0f, 10.0f, &lat) && std::fabs(lat.boxW[0] - 0.6f) < 1e-5f &&
                  std::fabs(lat.boxW[1] - 0.8f) < 1e-5f && std::fabs(lat.boxCentre[0] - 33.0f) < 1e-3f && std::fabs(lat.boxCentre[1] - 44.0f) < 1e-3f &&
                  std::fabs(lat.boxU[0] - 0.8f) < 1e-5f && std::fabs(lat.boxU[1] + 0.6f) < 1e-5f && std::fabs(lat.boxHalf[2] - 65.0f) < 1e-3f,
                  "the box runs along the pad-to-target axis, centred on the pad's line, its length axis orthogonal to that axis");
            check(std::fabs(lat.boxU[0] * lat.boxW[0] + lat.boxU[1] * lat.boxW[1] + lat.boxU[2] * lat.boxW[2]) < 1e-5f,
                  "the length axis is orthogonal to the box axis");
            se::OwnCell down;
            const float tgtDown[3] = {0, -100, 0};
            check(se::buildBox(padC, tgtDown, lenAx, 66.0f, 27.5f, 5.0f, 10.0f, &down) && down.boxCentre[1] == -55.0f && down.boxHalf[2] == 65.0f,
                  "a target below the pad: the box runs down the pad-to-target axis");
            se::OwnCell tz;
            const float tgtZ[3] = {0, 0, 140};
            check(se::buildBox(padC, tgtZ, lenAx, 66.0f, 27.5f, 5.0f, 10.0f, &tz) && tz.boxCentre[2] == 75.0f && tz.boxHalf[2] == 85.0f,
                  "a target along z: centre 75 m out, half-height 85 m (from -10 to 140 + 20)");
            se::OwnCell flat;
            const float tgtFlat[3] = {0, 0.5f, 0};
            check(!se::buildBox(padC, tgtFlat, lenAx, 66.0f, 27.5f, 5.0f, 10.0f, &flat), "a target closer than 1 m builds no box");
            const float axW[3] = {0, 0, 1}, padE2[3] = {0, 0, -1};
            check(std::fabs(se::angleDeg(axW, padE2) - 180.0f) < 1e-3f && std::fabs(se::angleDeg(axW, lenAx) - 90.0f) < 1e-3f,
                  "the angle between the box axis and the pad's e2 is 180 deg for opposite axes and 90 deg across");
            check(std::fabs(se::kMarginDefaultM - 10.0f) < 1e-6f, "the margin default is 10 m (v12.1)");
            // Built v12.1 (d): the precomputed verdict's code (one byte per rig, read by the decision): 2 outside, 1 mixed, 0 unknown or refresh.
            check(se::verdictCode(se::RigVerdict::kOutside) == 2 && se::verdictCode(se::RigVerdict::kMixed) == 1 &&
                  se::verdictCode(se::RigVerdict::kUnknown) == 0 && se::verdictCode(se::RigVerdict::kRefresh) == 0,
                  "the verdict codes: outside 2, mixed 1, unknown and refresh 0");
            // Built v13 (fix 1): the own-ship prefix and exemption.
            char pfx[32] = {};
            se::ownPrefixOf("CobraMkV_02", pfx, sizeof(pfx));
            check(std::strcmp(pfx, "CobraMkV_") == 0, "the own ship's prefix is up to and including the first underscore");
            se::ownPrefixOf("NoUnderscore", pfx, sizeof(pfx));
            check(pfx[0] == '\0', "a name with no underscore has no prefix");
            check(se::ownShipExempt(true, 30.0f, "AnyRecord", "") && !se::ownShipExempt(true, 60.0f, "AnyRecord", "") &&
                  !se::ownShipExempt(false, 30.0f, "AnyRecord", ""), "a small sphere holding the camera is the own ship; a big one or none is not");
            check(se::ownShipExempt(false, 500.0f, "CobraMkV_02", "CobraMkV_") && !se::ownShipExempt(false, 500.0f, "Cobra_02", "CobraMkV_"),
                  "a record with the own ship's prefix is the own ship; another type is not");
            // Built v13 (fix 3): the shaft target is the BlastShieldBase nearest the LINE through the pad along its up axis e2 (perpendicular distance,
            // ties by along), above the pad only, within the reach. Deterministic: the same inputs give the same target.
            {
                se::ExplicitObs cand[3];
                const float padP[3] = {0, 0, 0}, e2z[3] = {0, 0, 1};
                cand[0].centre[0] = 50; cand[0].centre[1] = 0; cand[0].centre[2] = 60;     // perpendicular 50, along 60 (nearest to the pad)
                std::snprintf(cand[0].name, sizeof(cand[0].name), "BlastShieldBase");
                cand[1].centre[0] = 0; cand[1].centre[1] = 0; cand[1].centre[2] = 100;    // perpendicular 0, along 100 (on the line)
                std::snprintf(cand[1].name, sizeof(cand[1].name), "BlastShieldBase");
                cand[2].centre[0] = 0; cand[2].centre[1] = 0; cand[2].centre[2] = -20;    // below the pad: never a target
                std::snprintf(cand[2].name, sizeof(cand[2].name), "BlastShieldBase");
                float q[3] = {};
                char qn[48] = {};
                float perp = -1.0f, along = -1.0f;
                check(se::pickTargetOnLine(cand, 3, padP, e2z, 0, q, qn, sizeof(qn), &perp, &along) && q[2] == 100.0f && perp == 0.0f &&
                      along == 100.0f, "the BlastShieldBase on the pad's up line is the target, not the nearest to the pad");
                check(se::pickTargetOnLine(cand, 3, padP, e2z, 0, q, qn, sizeof(qn), &perp, &along) && q[2] == 100.0f,
                      "the same inputs give the same target (deterministic across engages)");
                cand[1].centre[0] = 0; cand[1].centre[1] = 0; cand[1].centre[2] = 30;     // a tie on the line: the smaller along wins
                check(se::pickTargetOnLine(cand, 3, padP, e2z, 0, q, qn, sizeof(qn), &perp, &along) && q[2] == 30.0f && along == 30.0f,
                      "a tie on the line goes to the instance nearer along the axis");
                cand[1].centre[2] = 100;
                float qb[3] = {};
                check(!se::pickTargetOnLine(cand + 2, 1, padP, e2z, 0, qb, qn, sizeof(qn), &perp, &along),
                      "an instance below the pad is not a target");
                check(se::padLowered(30.0f, 30.0f, 15.0f) && se::padLowered(40.0f, 30.0f, 15.0f) && !se::padLowered(46.0f, 30.0f, 15.0f),
                      "the pad is lowered within 15 m of its lowest height, and not beyond");
            }
            // The freeze: a fresh pick differs from the held cell when it is missing or its centre moved by more than 0.5 m.
            se::OwnCell held;
            held.has = true;
            held.centre[0] = 0; held.centre[1] = 0; held.centre[2] = 0;
            se::OwnCell moved = held, none;
            moved.centre[0] = 2.0f;
            check(se::cellDiffers(none, held), "a frame with no pad record differs from the held cell (counted, not used)");
            check(se::cellDiffers(moved, held), "a pad moved by 2 m differs from the held cell (counted, not used)");
            se::OwnCell same = held;
            same.centre[0] = 0.2f;
            check(!se::cellDiffers(same, held), "a pad within 0.5 m is the held cell's pad");
            // Built v12: the off-screen emulation's call order (decomp_431AFE0 lines 99 to 102): the byte store, the owner-count call, then the
            // pipe call only with a pipe. The pipe call adds 0x78 and the owner-count call writes +0x2A4: pipe+0x31C.
            {
                int kinds[3] = {};
                uintptr_t args[3] = {};
                const uintptr_t coll = 0x10000, pipe = 0x20000;
                check(se::offScreenSteps(coll, 0, kinds, args) == 2 && kinds[0] == 1 && args[0] == coll + 0x629 && kinds[1] == 2 &&
                          args[1] == coll + 0x300,
                      "without a pipe: the byte store to collection+0x629, then the owner-count call on collection+0x300");
                check(se::offScreenSteps(coll, pipe, kinds, args) == 3 && kinds[2] == 3 && args[2] == pipe,
                      "with a pipe: the same two steps, then the pipe call");
                check(args[2] + 0x78 + 0x2A4 == pipe + 0x31C, "the pipe call's owner count is pipe+0x31C (0x78 + 0x2A4)");
            }
            // The margin: a signed number; the ladder -10, -5, 0, 5, 10, 20, 40.
            float mg = 0.0f;
            check(se::parseMarginM("", 5.0f, &mg) && mg == 5.0f, "an empty margin is the default 5 m");
            check(se::parseMarginM("-10", 5.0f, &mg) && mg == -10.0f, "a negative margin parses");
            check(!se::parseMarginM("wide", 5.0f, &mg), "a margin that is not a number is refused");
            bool mv = false;
            check(se::stepMarginRung(5.0f, +1, &mv) == 10.0f && mv, "the margin steps up from 5 m to 10 m");
            check(se::stepMarginRung(5.0f, -1, &mv) == 0.0f && mv, "the margin steps down from 5 m to 0 m");
            check(se::stepMarginRung(40.0f, +1, &mv) == 40.0f && !mv, "the margin clamps at 40 m");
            check(se::stepMarginRung(-10.0f, -1, &mv) == -10.0f && !mv, "the margin clamps at -10 m");
            // Built v12: the early rig veto's rule. The cell is the box from above (centre (0,55,0), half-extents 71, 32.5, 65).
            const float farPt[3] = {500, 500, 0}, inside[3] = {60, 55, 0}, nearOut[3] = {90, 55, 0};
            using RV = se::RigVerdict;
            check(se::rigVerdict(false, farPt, 5.0f, true, box, false, 0) == RV::kUnknown, "a rig with no bound is unknown, not vetoed");
            check(se::rigVerdict(true, farPt, 5.0f, true, box, false, 0) == RV::kOutside, "a rig bound wholly outside the box is vetoed");
            check(se::rigVerdict(true, inside, 2.0f, true, box, false, 0) == RV::kMixed, "a rig bound inside the box is mixed (forwarded)");
            check(se::rigVerdict(true, nearOut, 2.0f, true, box, false, 0) == RV::kOutside, "19 m outside the box with a 2 m bound is outside");
            check(se::rigVerdict(true, nearOut, 25.0f, true, box, false, 0) == RV::kMixed, "19 m outside with a 25 m bound reaches the box: mixed");
            check(se::rigVerdict(true, farPt, 5.0f, false, box, false, 0) == RV::kUnknown, "named mode with no cell judges nothing");
            check(se::rigVerdict(true, farPt, 5.0f, false, box, true, 0) == RV::kOutside, "all mode needs no cell: a bounded rig is vetoed");
            check(se::rigVerdict(true, farPt, 5.0f, true, box, false, 29) == RV::kOutside, "the 29th veto in a row is still a veto");
            check(se::rigVerdict(true, farPt, 5.0f, true, box, false, se::kRigRefreshN) == RV::kRefresh,
                  "the 30th veto in a row is let through (a refresh)");
            check(std::fabs(se::distanceToCell(inside, box)) == 0.0f, "a point inside the box is at distance 0");
            check(std::fabs(se::distanceToCell(nearOut, box) - 19.0f) < 1e-4f, "a point 19 m beyond the box face is 19 m away");
            se::OwnCell capc;
            capc.has = true;
            capc.capsule = true;
            capc.radius = 10.0f;
            capc.capA[0] = 0; capc.capA[1] = 0; capc.capA[2] = 0;
            capc.capB[0] = 0; capc.capB[1] = 120; capc.capB[2] = 0;
            const float sideOfAxis[3] = {30, 60, 0};
            check(std::fabs(se::distanceToCell(sideOfAxis, capc) - 20.0f) < 1e-4f, "a capsule of radius 10 is 20 m from a point 30 m off the axis");
            se::OwnCell sph;
            sph.has = true;
            sph.radius = 10.0f;
            const float sphPt[3] = {30, 0, 0};
            check(std::fabs(se::distanceToCell(sphPt, sph) - 20.0f) < 1e-4f, "a sphere of radius 10 is 20 m from a point 30 m off");
            // The cadence: thirty vetoes, then the thirty-first decision is a refresh (the caller resets the run), then vetoes again.
            uint32_t run = 0;
            int outs = 0, refreshes = 0;
            for (int i = 0; i < 62; ++i) {
                const RV v = se::rigVerdict(true, farPt, 5.0f, true, box, false, run);
                if (v == RV::kRefresh) {
                    ++refreshes;
                    run = 0;
                } else if (v == RV::kOutside) {
                    ++outs;
                    ++run;
                }
            }
            check(outs == 60 && refreshes == 2, "over 62 decisions a bound rig is vetoed 60 times and refreshed twice (every 31st)");
        }
        // Built v10: producer B's veto mode parses (veto is the default), and the world sphere follows the store's formula.
        se::ProducerMode pm = se::ProducerMode::kCount;
        check(se::parseProducerMode("", &pm) && pm == se::ProducerMode::kVeto, "an empty diag.sealed_trial_producer is veto");
        check(se::parseProducerMode("count", &pm) && pm == se::ProducerMode::kCount, "count parses");
        check(!se::parseProducerMode("Veto", &pm) && pm == se::ProducerMode::kVeto, "an unknown producer word is refused and the mode is veto");
        {
            const float L[3] = {1.0f, 2.0f, 3.0f};
            float A[15] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0};   // identity 3x3 (A[4k+j]), no translation
            float B[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            float out[3] = {};
            check(se::storeWorldSphere(L, A, B, out) && out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f,
                  "identity poses leave the centre where the node's bounds put it");
            A[12] = 10.0f;                     // the pose's translation along x
            B[13] = 5.0f;                      // the base matrix's translation along y
            check(se::storeWorldSphere(L, A, B, out) && out[0] == 11.0f && out[1] == 7.0f && out[2] == 3.0f,
                  "the pose translation then the base translation: (11, 2+5, 3)");
            // A pose whose 3x3 swaps x and y: u0 = A[0]*L0 + A[4]*L1 = L1, u1 = A[1]*L0 + A[5]*L1 = L0 (its translation zero).
            float A2[15] = {0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};   // A2[4k+j]: A2[1] = A2[4] = A2[10] = 1
            float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            check(se::storeWorldSphere(L, A2, I, out) && out[0] == 2.0f && out[1] == 1.0f && out[2] == 3.0f,
                  "a swapped x and y in the pose swaps the centre's x and y");
            const float Nan[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
            check(!se::storeWorldSphere(Nan, A, B, out), "a non-finite input is not a world sphere");
        }
        {
            char buf[1400] = {};
            se::Window w;
            w.ptrNull = 3;
            w.ptrImage = 5;
            w.ptrOther = 7;
            w.ptrUnread = 11;
            se::formatWindow(buf, sizeof(buf), w, 80.0f, "view", se::Mode::kAll);
            check(std::strstr(buf, "mode=all ") != nullptr, "the window line names the all mode");
            check(std::strstr(buf, "ptr290 null=3 image=5 other=7 unread=11") != nullptr, "the window line carries the rec+0x290 kinds");
            char mode[400] = {};
            se::formatModeLine(mode, sizeof(mode), se::Mode::kAll, 42, "hotkey.sealed_trial_mode");
            check(std::strstr(mode, "mode = all at frame 42") != nullptr, "the mode line for all says every record loses its flags");
        }

        se::BuilderPolicy p = se::BuilderPolicy::kSkip;
        check(se::parseBuilderPolicy("", &p) && p == se::BuilderPolicy::kCount, "an empty diag.sealed_trial_builder is count");
        check(se::parseBuilderPolicy("skip", &p) && p == se::BuilderPolicy::kSkip, "skip parses (the caller refuses it)");
        check(!se::parseBuilderPolicy("drop", &p) && p == se::BuilderPolicy::kCount, "an unknown builder word is refused and is count");

        check(se::containsNoCase("HangerSmall", "hanger") && se::containsNoCase("BlastShield_A", "BLASTSHIELD"),
              "the keyword test ignores case");
        check(!se::containsNoCase("DockingHub", "Hangar") && !se::containsNoCase("", "Lift"), "a name without the word does not match");
        // Built v8.3b: the name classes, the pad's size from its name, the explicit part names.
        check(se::isPadName("LandingPadSmallA") && se::isPadName("LandingPadLargeB") && !se::isPadName("LandingPadSmallGuideMarkers1") &&
                  !se::isPadName("HangerSmall"),
              "a LandingPad* record is a pad; its GuideMarkers are not");
        check(se::padSizeOf("LandingPadSmallA") == se::PadSize::kSmall && se::padSizeOf("LandingPadMediumB") == se::PadSize::kMedium &&
                  se::padSizeOf("LandingPadLarge2") == se::PadSize::kLarge && se::padSizeOf("LandingPadXL") == se::PadSize::kNone &&
                  se::padSizeOf("LandingPadSmallGuideMarkers") == se::PadSize::kNone && se::padSizeOf(nullptr) == se::PadSize::kNone,
              "the size comes from the name: Small, Medium or Large; anything else, or GuideMarkers, has none");
        check(se::isExplicitName("HangerSmall") && se::isExplicitName("Hangar_x") && se::isExplicitName("BlastShield") &&
                  se::isExplicitName("SmallHangerSkirt") && se::isExplicitName("MediumHangerSkirt") && se::isExplicitName("LargeHangerSkirt") &&
                  !se::isExplicitName("DockingHub") && !se::isExplicitName("Agricultural_SmallHangar"),
              "the explicit parts are Hanger, Hangar, BlastShield and the three HangerSkirt sizes, by prefix");
        check(se::classifyName(true, "LandingPadSmallA") == se::NameClass::kPad && se::classifyName(true, "HangerSmall") == se::NameClass::kExplicit &&
                  se::classifyName(true, "DockingHub") == se::NameClass::kOther && se::classifyName(false, "x") == se::NameClass::kUnnamed,
              "the classes are pad, explicit, other named and unnamed");

        // The own pad: the containing pad nearest first; else the nearest pad centre within 150 m; else none. The radius is the size's.
        const float radii[3] = {60.0f, 90.0f, 130.0f};
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        se::PadObs pads[3];
        pads[0].addr = 0x100;
        pads[0].centre[0] = 10.0f;
        pads[0].radius = 5.0f;   // does not contain the camera (10 > 5)
        pads[0].size = se::PadSize::kSmall;
        std::snprintf(pads[0].name, sizeof(pads[0].name), "LandingPadSmallA");
        pads[1].addr = 0x200;
        pads[1].centre[0] = 3.0f;
        pads[1].radius = 5.0f;   // contains the camera (3 <= 5)
        pads[1].size = se::PadSize::kMedium;
        std::snprintf(pads[1].name, sizeof(pads[1].name), "LandingPadMediumB");
        pads[2].addr = 0x300;
        pads[2].centre[2] = 2.0f;
        pads[2].radius = 1.0f;   // does not contain the camera
        pads[2].size = se::PadSize::kLarge;
        std::snprintf(pads[2].name, sizeof(pads[2].name), "LandingPadLargeC");
        se::OwnCell cellPick;
        se::pickOwnPad(pads, 3, origin, radii, &cellPick);
        check(cellPick.has && cellPick.pad == 0x200 && cellPick.size == se::PadSize::kMedium && cellPick.radius == 90.0f && cellPick.padContains,
              "the own pad is the containing pad; its size gives the cell radius (medium, 90 m)");
        se::pickOwnPad(pads, 1, origin, radii, &cellPick);
        check(cellPick.has && cellPick.pad == 0x100 && !cellPick.padContains && cellPick.radius == 60.0f,
              "with no containing pad, the nearest within 150 m is the fallback (small, 60 m)");
        pads[0].centre[0] = 200.0f;
        se::pickOwnPad(pads, 1, origin, radii, &cellPick);
        check(!cellPick.has && std::strlen(cellPick.reason) > 0, "no pad within 150 m and none containing: no own pad, and the reason is said");
        se::pickOwnPad(nullptr, 0, origin, radii, &cellPick);
        check(!cellPick.has && std::strcmp(cellPick.reason, "no LandingPad record this frame") == 0, "no pad records: no own pad, said");

        // The centre test: the cell keeps a record whose CENTRE is within its radius of the cell centre (its sphere may overlap, and that is not
        // enough); the explicit set keeps the nearest instance outside it; everything else is culled, containing or not.
        se::OwnCell cell;
        cell.has = true;
        cell.radius = 60.0f;
        const uintptr_t noSet[1] = {0};
        const float insideC[3] = {30.0f, 0.0f, 0.0f};
        const float outsideC[3] = {100.0f, 0.0f, 0.0f};
        check(se::keepNamedCell(insideC, 500.0f, origin, 80.0f, cell, noSet, 0, 0x1).keep, "a centre within the cell radius is kept, even with a huge sphere");
        check(se::keepNamedCell(insideC, 500.0f, origin, 80.0f, cell, noSet, 0, 0x1).why == se::KeepWhy::kCell, "a centre-test keep is why cell");
        check(!se::keepNamedCell(outsideC, 500.0f, origin, 80.0f, cell, noSet, 0, 0x2).keep,
              "a centre outside the cell is culled although its sphere overlaps the cell (the giant station records)");
        const uintptr_t explicitSet[1] = {0x3};
        check(se::keepNamedCell(outsideC, 5.0f, origin, 80.0f, cell, explicitSet, 1, 0x3).why == se::KeepWhy::kExplicit,
              "an explicit part's nearest instance outside the cell is kept (why explicit)");
        check(!se::keepNamedCell(outsideC, 5.0f, origin, 80.0f, cell, explicitSet, 1, 0x4).keep,
              "a record outside the cell that is not in the explicit set is culled");
        const float containingC[3] = {0.0f, 0.0f, 1.0f};
        const se::NamedKeep cont = se::keepNamedCell(containingC, 10.0f, origin, 80.0f, cell, noSet, 0, 0x5);
        check(cont.containing && cont.keep, "a record containing the camera inside the cell is kept (the cell decides)");
        const se::NamedKeep far = se::keepNamedCell(outsideC, 500.0f, origin, 80.0f, cell, noSet, 0, 0x6);
        check(far.containing && !far.keep,
              "a far record whose sphere contains the camera (its centre 100 m away, outside the cell) is culled by the centre test");
        se::OwnCell noCell;
        check(se::keepNamedCell(insideC, 5.0f, origin, 80.0f, noCell, noSet, 0, 0x7).why == se::KeepWhy::kSphere,
              "with no own pad the frame is the sphere rule (why sphere)");
        check(!se::keepNamedCell(outsideC, 5.0f, origin, 80.0f, noCell, noSet, 0, 0x8).keep,
              "with no own pad, a record outside the trial sphere is culled as in sphere mode");

        // The explicit parts: per part node the nearest instance to the cell centre within 200 m.
        se::ExplicitObs parts[4];
        parts[0] = se::ExplicitObs{0xA, 1, {5.0f, 0.0f, 0.0f}};
        parts[1] = se::ExplicitObs{0xB, 1, {2.0f, 0.0f, 0.0f}};
        parts[2] = se::ExplicitObs{0xC, 2, {300.0f, 0.0f, 0.0f}};
        parts[3] = se::ExplicitObs{0xD, 2, {9.0f, 0.0f, 0.0f}};
        uintptr_t picked[8] = {};
        const size_t np = se::pickExplicit(parts, 4, origin, picked, 8);
        check(np == 2 && picked[0] == 0xB && picked[1] == 0xD,
              "per part node the nearest instance to the cell centre within 200 m is picked (node 2's 300 m instance is not)");

        // The ladder (Built v11, addendum): 15, 20, 25, 30, 40, 50, 60; off-ladder steps land on the next rung; the ends clamp.
        bool moved = false;
        check(se::stepCellRadius(30.0f, +1, &moved) == 40.0f && moved, "larger from 30 m is 40 m");
        check(se::stepCellRadius(30.0f, -1, &moved) == 25.0f && moved, "smaller from 30 m is 25 m");
        check(se::stepCellRadius(90.0f, +1, &moved) == 90.0f && !moved, "larger from 90 m (above the ladder) clamps and does not move");
        check(se::stepCellRadius(90.0f, -1, &moved) == 60.0f && moved, "smaller from 90 m (off the ladder) lands on 60 m");
        check(se::stepCellRadius(60.0f, +1, &moved) == 60.0f && !moved, "the top of the ladder clamps at 60 m");
        check(se::stepCellRadius(15.0f, -1, &moved) == 15.0f && !moved, "the bottom of the ladder clamps at 15 m");
        float rung = 15.0f;
        int steps = 0;
        for (;;) {
            const float next = se::stepCellRadius(rung, +1, &moved);
            if (!moved) break;
            rung = next;
            ++steps;
        }
        check(steps == 6 && rung == 60.0f, "six steps up from 15 m reach 60 m");
        check(se::kCellDefaultM[0] == 30.0f && se::kCellDefaultM[1] == 90.0f && se::kCellDefaultM[2] == 130.0f,
              "the cell radius defaults are 30 (small, a guess; was 60), 90 and 130 m (guesses)");
        float cellR = 0.0f;
        check(se::parseCellRadius("", 60.0f, &cellR) && cellR == 60.0f, "an empty cell key is the default");
        check(se::parseCellRadius("75", 60.0f, &cellR) && cellR == 75.0f, "a cell radius in metres is read");
        check(!se::parseCellRadius("big", 90.0f, &cellR) && cellR == 90.0f, "a word is the default, and says so");
        check(se::parseCellRadius("2", 60.0f, &cellR) && cellR == se::kCellMinM, "a cell radius below the floor is the floor");

        // The window line: the cell, its pad, the distances and the counts; with no pad, the size is none and the reason is said.
        se::Window w;
        w.builderAfterCull = 3;
        w.culledContaining = 7;
        w.keptExplicit = 2;
        w.keptByCell = 5;
        w.cellHas = true;
        w.cellSize = 'M';
        w.cellRadius = 90.0f;
        std::snprintf(w.padName, sizeof(w.padName), "LandingPadMediumB");
        w.padDist = 12.5f;
        char line[700];
        se::formatWindow(line, sizeof(line), w, 80.0f, "eye rows", se::Mode::kNamed);
        check(contains(line, "builder_after_cull=3 mode=named size=M cell_r=90.0 pad=LandingPadMediumB pad_dist=12.5 pad_reason=- "
                             "kept_explicit=2 kept_by_cell=5 culled_containing=7"),
              "the window line names the mode, the size, the cell radius, the pad, its distance and the counts");
        se::Window noPad;
        std::snprintf(noPad.padReason, sizeof(noPad.padReason), "no pad sphere contains the camera and none is within 150 m");
        se::formatWindow(line, sizeof(line), noPad, 80.0f, "eye rows", se::Mode::kNamed);
        check(contains(line, "mode=named size=none cell_r=0.0 pad=none pad_dist=-1.0 pad_reason=no pad sphere"),
              "with no own pad the window says size none and why");
        se::formatWindow(line, sizeof(line), w, 80.0f, "eye rows");
        check(contains(line, "builder_after_cull=3 mode=sphere"), "the window line defaults to mode sphere");

        se::formatModeLine(line, sizeof(line), se::Mode::kNamed, 12, "hotkey.sealed_trial_mode");
        check(contains(line, "mode = named at frame 12 (trigger hotkey.sealed_trial_mode)") && contains(line, "own pad's cell"),
              "the named mode line names the cell rule");
        se::formatModeLine(line, sizeof(line), se::Mode::kSphere, 13, "hotkey.sealed_trial_mode");
        check(contains(line, "mode = sphere at frame 13"), "the sphere mode line says sphere");
        se::formatCellStep(line, sizeof(line), 'M', true, 100.0f, +1, 7, "hotkey.sealed_cell_larger");
        check(std::string(line) == "named cell: medium radius -> 100.0 m at frame 7 (trigger hotkey.sealed_cell_larger)",
              "a cell step line names the size, the radius and the frame");
    }

    // --- Built v8.4: the builder's centre against two hand-worked cases, and the fallback paths of the record's centre.
    {
        // Identity quaternion: the centre is c + t. c = (1,2,3), t = (10,20,30) gives (11,22,33).
        const float qI[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float c[4] = {1.0f, 2.0f, 3.0f, 0.0f};
        const float t[4] = {10.0f, 20.0f, 30.0f, 0.0f};
        float out[4] = {};
        se::builderCentre(qI, t, c, out);
        check(std::fabs(out[0] - 11.0f) < 1e-4f && std::fabs(out[1] - 22.0f) < 1e-4f && std::fabs(out[2] - 33.0f) < 1e-4f,
              "the builder centre with the identity quaternion is c + t: (11,22,33)");
        // A 90 degree turn about z: q = (0, 0, s, s) with s = sqrt(1/2). The rotation of c = (1,2,3) is (-2,1,3); plus t gives (8,21,33).
        const float s = 0.70710678f;
        const float qZ[4] = {0.0f, 0.0f, s, s};
        se::builderCentre(qZ, t, c, out);
        check(std::fabs(out[0] - 8.0f) < 1e-3f && std::fabs(out[1] - 21.0f) < 1e-3f && std::fabs(out[2] - 33.0f) < 1e-3f,
              "a 90 degree turn about z rotates c to (-2,1,3) and adds t: (8,21,33)");

        // The record's centre: the pose when readable; else rec+0x240 (the counted fallback); else none.
        const float poseC[3] = {8.0f, 21.0f, 33.0f};
        const float rawC[3] = {0.0f, 0.0f, 0.0f};
        float world[3] = {};
        check(se::chooseCentre(true, poseC, true, rawC, world) == se::CentreSource::kPose && world[0] == 8.0f,
              "a readable pose gives the centre, even when rec+0x240 is zero");
        check(se::chooseCentre(false, poseC, true, rawC, world) == se::CentreSource::kRaw && world[0] == 0.0f,
              "an unreadable pose falls back to rec+0x240 (the counted fallback)");
        check(se::chooseCentre(false, poseC, false, rawC, world) == se::CentreSource::kNone, "neither readable: no centre");
        const float badPose[3] = {std::nanf(""), 0.0f, 0.0f};
        check(se::chooseCentre(true, badPose, true, poseC, world) == se::CentreSource::kRaw,
              "a non-finite pose is not used: the fallback is taken");
        check(se::isZeroCentre(rawC) && !se::isZeroCentre(poseC), "rec+0x240 exactly zero is detected; a real centre is not zero");

        // Percentiles (nearest rank): 1..100 gives p50 50 and p95 95; no values gives 0.
        std::vector<float> hundred;
        for (int i = 1; i <= 100; ++i) hundred.push_back(static_cast<float>(i));
        check(se::percentileOf(hundred, 0.50) == 50.0f && se::percentileOf(hundred, 0.95) == 95.0f,
              "the nearest-rank percentile of 1..100: p50 is 50 and p95 is 95");
        check(se::percentileOf(std::vector<float>{}, 0.5) == 0.0f, "no values: the percentile is 0");

        // Built v13.1 (b): the bay's name classes and volume.
        check(!se::bayPartName("CobraMkV_") && !se::bayPartName("SocialSpace") && !se::bayPartName(nullptr) && !se::bayPartName(""),
              "the bay keeps none of: a ship, the concourse, a null or empty name");
        check(se::bayPartName("Hangar_Small") && se::bayPartName("Hanger_Medium") && se::bayPartName("LandingPad_Large") &&
                  se::bayPartName("Lift_Dock") && se::bayPartName("Gate_X") && se::bayPartName("BlastShieldBase") && se::bayPartName("Skirt_Y") &&
                  se::bayPartName("Lobby_Z"),
              "the bay keeps Hangar, Hanger, LandingPad, Lift, Gate, BlastShield, Skirt and Lobby names at any size");
        const float bo[3] = {0.0f, 0.0f, 0.0f};
        const float bu[3] = {0.0f, 0.0f, 1.0f};
        const float lp[3] = {3.0f, 4.0f, 10.0f};
        float along = -1.0f;
        check(se::lateralToAxis(lp, bo, bu, &along) == 5.0f && along == 10.0f, "lateral of (3,4,10) to the z axis is 5; along is 10");
        const float p1[3] = {0.0f, 0.0f, 50.0f}, p2[3] = {61.0f, 0.0f, 50.0f}, p3[3] = {60.0f, 0.0f, 50.0f};
        const float p4[3] = {0.0f, 0.0f, -21.0f}, p5[3] = {0.0f, 0.0f, -20.0f}, p6[3] = {0.0f, 0.0f, 120.0f}, p7[3] = {0.0f, 0.0f, 121.0f};
        check(se::inBayVolume(p1, bo, bu, 100.0f, 20.0f) && !se::inBayVolume(p2, bo, bu, 100.0f, 20.0f) && se::inBayVolume(p3, bo, bu, 100.0f, 20.0f),
              "the bay: on the shaft is in; 61 m lateral is out; 60 m lateral is in (the limit is inclusive)");
        check(!se::inBayVolume(p4, bo, bu, 100.0f, 20.0f) && se::inBayVolume(p5, bo, bu, 100.0f, 20.0f),
              "the bay: 21 m below the pad is out; 20 m below is in");
        check(se::inBayVolume(p6, bo, bu, 100.0f, 20.0f) && !se::inBayVolume(p7, bo, bu, 100.0f, 20.0f),
              "the bay: 20 m past a target 100 m away is in; 21 m past is out");
        const float ua[3] = {0.0f, 0.0f, 0.0f}, ub[3] = {0.0f, 0.0f, 5.0f}, uc[3] = {0.0f, 0.0f, 0.5f};
        float uu[3] = {};
        check(se::unitBetween(ua, ub, uu) && uu[2] == 1.0f && !se::unitBetween(ua, uc, uu),
              "the unit vector from a to b is (0,0,1); a pair under a metre apart has none");

        // Built v13.1 (a): the box's coordinates and its footprint expanded by 20 m.
        se::OwnCell box;
        box.box = true;
        box.boxCentre[0] = box.boxCentre[1] = box.boxCentre[2] = 0.0f;
        box.boxU[0] = 1.0f; box.boxV[1] = 1.0f; box.boxW[2] = 1.0f;
        box.boxHalf[0] = 10.0f; box.boxHalf[1] = 5.0f; box.boxHalf[2] = 3.0f;
        const float pc[3] = {3.0f, 4.0f, 5.0f};
        float cc[3] = {};
        check(se::boxCoordsOf(box, pc, cc) && cc[0] == 3.0f && cc[1] == 4.0f && cc[2] == 5.0f, "the box coordinates of (3,4,5) are (3,4,5)");
        const float n1[3] = {29.0f, 0.0f, 0.0f}, n2[3] = {31.0f, 0.0f, 0.0f}, n3[3] = {0.0f, 0.0f, 22.0f}, n4[3] = {0.0f, 0.0f, 24.0f};
        check(se::nearBox(box, n1) && !se::nearBox(box, n2), "the box footprint: 29 m along u (half 10, margin 20) is near; 31 m is not");
        check(se::nearBox(box, n3) && !se::nearBox(box, n4), "the box footprint: 22 m along w (half 3, margin 20) is near; 24 m is not");
        se::OwnCell none;
        check(!se::nearBox(none, n1) && !se::boxCoordsOf(none, pc, cc), "no box: no footprint and no coordinates");

        // Built v13.2 (a): the below distance parses (empty is the default; a number in [0, 200]; other text refused).
        float bl = -1.0f;
        check(se::parseBelowM("", se::kBelowDefaultsM[0], &bl) && bl == 85.0f && se::kBelowDefaultsM[0] == 85.0f,
              "diag.sealed_cell_below_small_m: empty is the 85 m default (the small hangar's lift doors sit at -72 m)");

        // Built v13.4 (a): one floor per size (85, 120, 145 m); the override, when set, is every size's floor; the table is indexed by size.
        const float belowTable[3] = {85.0f, 120.0f, 145.0f};
        check(se::belowForSize(se::PadSize::kSmall, belowTable, false, 0.0f) == 85.0f &&
                  se::belowForSize(se::PadSize::kMedium, belowTable, false, 0.0f) == 120.0f &&
                  se::belowForSize(se::PadSize::kLarge, belowTable, false, 0.0f) == 145.0f,
              "the per-size floors: small 85, medium 120, large 145 m");
        check(se::belowForSize(se::PadSize::kMedium, belowTable, true, 60.0f) == 60.0f &&
                  se::belowForSize(se::PadSize::kLarge, belowTable, true, 60.0f) == 60.0f,
              "the override (diag.sealed_cell_below_m) is every size's floor when it is set");
        check(se::belowForSize(se::PadSize::kNone, belowTable, false, 0.0f) == 85.0f,
              "no size: the small floor (no pad is held, so none is used)");
        check(se::kBelowDefaultsM[1] == 120.0f && se::kBelowDefaultsM[2] == 145.0f, "the built-in defaults are 120 and 145 m for medium and large");

        // Built v13.3 (c): a raw centre is usable only when finite, not the origin, with a positive finite radius.
        const float rawOk[3] = {10.0f, 2.0f, -3.0f}, rawZero[3] = {0.0f, 0.0f, 0.0f}, rawNan[3] = {NAN, 0.0f, 0.0f};
        check(se::rawCentreUsable(rawOk, 5.0f) && !se::rawCentreUsable(rawZero, 5.0f) && !se::rawCentreUsable(rawNan, 5.0f) &&
                  !se::rawCentreUsable(rawOk, 0.0f) && !se::rawCentreUsable(rawOk, NAN),
              "a raw centre at the origin (rec+0x240 zero) is unreadable, as is a non-finite centre or radius");

        // Built v13.3 (b): the own pad candidates (containing, or within 40 m of the camera, any size), and the held cell's drop limit.
        check(se::ownPadCandidate(50.0f, 60.0f) && se::ownPadCandidate(39.0f, 0.0f) && !se::ownPadCandidate(41.0f, 0.0f),
              "a pad is a candidate when the camera is inside its sphere or its centre is within 40 m; not at 41 m with no sphere");
        check(!se::heldCellDrops(29, 29, se::kHeldLostFrames) && se::heldCellDrops(30, 0, se::kHeldLostFrames) &&
                  se::heldCellDrops(0, 30, se::kHeldLostFrames) && se::kHeldLostFrames == 30u,
              "the held cell drops at 30 consecutive frames of absence or of another own pad, not at 29");
        check(se::parseBelowM("45", 60.0f, &bl) && bl == 45.0f && se::parseBelowM(" 0 ", 60.0f, &bl) && bl == 0.0f,
              "diag.sealed_cell_below_m: 45 and 0 (with spaces) are read");
        check(!se::parseBelowM("sixty", 60.0f, &bl) && !se::parseBelowM("-1", 60.0f, &bl) && !se::parseBelowM("201", 60.0f, &bl),
              "diag.sealed_cell_below_m: text, a negative and 201 are refused");
        // The box's bottom at the below distance: pad at the origin, target 100 m up the z axis, below 60: the box runs -60 to 120 along z.
        const float zpad[3] = {0.0f, 0.0f, 0.0f}, ztgt[3] = {0.0f, 0.0f, 100.0f}, zlen[3] = {1.0f, 0.0f, 0.0f};
        se::OwnCell zb;
        check(se::buildBox(zpad, ztgt, zlen, 66.0f, 27.5f, 5.0f, 60.0f, &zb) && zb.boxHalf[2] == 90.0f && zb.boxCentre[2] == 30.0f &&
                  zb.boxW[2] == 1.0f,
              "the box with a 60 m below distance runs from -60 to 120 m along the shaft (half 90, centre 30)");

        // Built v13.2 (b): the stripped prefix, the own record name match, and the own sphere's keep (plus the margin).
        char stripped[48] = {};
        check(se::strippedPrefix("CobraMkV_", stripped, sizeof(stripped)) && std::strcmp(stripped, "CobraMkV") == 0 &&
                  !se::strippedPrefix("CobraMkV", stripped, sizeof(stripped)) && !se::strippedPrefix("_", stripped, sizeof(stripped)),
              "the prefix CobraMkV_ strips to CobraMkV; a name with no trailing underscore, or a lone underscore, does not");
        check(se::ownRecordMatch("CobraMkV", "CobraMkV") && !se::ownRecordMatch("CobraMkV_Door", "CobraMkV") &&
                  !se::ownRecordMatch("CobraMkV", "") && !se::ownRecordMatch(nullptr, "CobraMkV"),
              "the own record matches its exact name only: not CobraMkV_Door, not an empty name");
        const float oc[3] = {0.0f, 0.0f, 0.0f};
        const float in1[3] = {34.0f, 0.0f, 0.0f}, in2[3] = {36.0f, 0.0f, 0.0f};
        check(se::ownSphereKeeps(in1, oc, 30.0f, 5.0f) && !se::ownSphereKeeps(in2, oc, 30.0f, 5.0f) &&
                  !se::ownSphereKeeps(in1, oc, 0.0f, 5.0f),
              "a record 34 m from a 30 m own sphere is kept with a 5 m margin; 36 m is not; no radius, no keep");
    }

    {
        // Built v13.7 (a): the pad frame. A rotated pad at (10, 2, 3): a point taken to the pad's frame and back is itself; the cell placed back
        // after a frame shift moves by exactly the shift; a pad turned about up turns the box's directions with it, and a point at a fixed place on
        // the pad keeps that place on the pad.
        const float th = 0.5235988f;   // 30 degrees
        const float axesA[3][3] = {{std::cos(th), std::sin(th), 0.0f}, {-std::sin(th), std::cos(th), 0.0f}, {0.0f, 0.0f, 1.0f}};
        const float padC[3] = {10.0f, 2.0f, 3.0f};
        const float p0[3] = {14.0f, -5.0f, 20.0f};
        float loc0[3] = {}, back0[3] = {};
        se::pointToLocal(p0, padC, axesA, loc0);
        se::pointToWorld(loc0, padC, axesA, back0);
        check(std::fabs(back0[0] - p0[0]) < 1e-3f && std::fabs(back0[1] - p0[1]) < 1e-3f && std::fabs(back0[2] - p0[2]) < 1e-3f,
              "a point taken to the rotated pad's frame and back is the same point");
        check(std::fabs(loc0[2] - 17.0f) < 1e-3f, "a point 17 m above the pad's centre is 17 m along the pad's up axis");

        // A cell with a shaft, a box, frozen at engage; then the pad's live frame is shifted by T (the record frame moves).
        se::OwnCell cellW;
        cellW.has = true;
        cellW.centre[0] = padC[0]; cellW.centre[1] = padC[1]; cellW.centre[2] = padC[2];
        cellW.capA[0] = padC[0]; cellW.capA[1] = padC[1]; cellW.capA[2] = padC[2];
        cellW.capB[0] = 12.0f; cellW.capB[1] = 40.0f; cellW.capB[2] = 90.0f;
        cellW.capsule = true;
        cellW.box = true;
        cellW.boxCentre[0] = 9.0f; cellW.boxCentre[1] = 3.0f; cellW.boxCentre[2] = 60.0f;
        cellW.boxU[0] = std::cos(th); cellW.boxU[1] = std::sin(th);
        cellW.boxV[0] = -std::sin(th); cellW.boxV[1] = std::cos(th);
        cellW.boxW[2] = 1.0f;
        cellW.boxHalf[0] = 33.0f; cellW.boxHalf[1] = 14.0f; cellW.boxHalf[2] = 60.0f;
        se::OwnCell cellL;
        se::localizeCell(cellW, padC, axesA, &cellL);
        const float T[3] = {-5.18f, -2.99f, 34.0f};   // the v13.6 flight's shift
        const float padS[3] = {padC[0] + T[0], padC[1] + T[1], padC[2] + T[2]};
        se::OwnCell cellS = cellW;
        se::worldizeCell(cellL, padS, axesA, &cellS);
        check(std::fabs(cellS.centre[0] - padS[0]) < 1e-3f && std::fabs(cellS.centre[2] - padS[2]) < 1e-3f,
              "the pad-frame cell's centre follows the pad's live centre after a frame shift");
        check(std::fabs(cellS.capB[0] - (cellW.capB[0] + T[0])) < 1e-3f && std::fabs(cellS.capB[1] - (cellW.capB[1] + T[1])) < 1e-3f &&
                  std::fabs(cellS.capB[2] - (cellW.capB[2] + T[2])) < 1e-3f,
              "the shaft target moves with the frame shift, by the same offset as the pad");
        check(std::fabs(cellS.boxCentre[2] - (cellW.boxCentre[2] + T[2])) < 1e-3f && std::fabs(cellS.boxU[0] - cellW.boxU[0]) < 1e-5f &&
                  std::fabs(cellS.boxHalf[2] - 60.0f) < 1e-6f,
              "the box moves by the shift; its axes and half-extents do not change under a pure shift");

        // A pad turned 90 degrees about up: the box's length axis (world +x at 30 degrees, which is the pad's own axis 0) turns with the pad to
        // world +y; the shaft target keeps its height along the pad's up axis.
        const float axesR[3][3] = {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        se::OwnCell cellT = cellW;
        se::worldizeCell(cellL, padC, axesR, &cellT);
        check(std::fabs(cellT.boxU[0]) < 1e-3f && std::fabs(cellT.boxU[1] - 1.0f) < 1e-3f,
              "a pad turned 90 degrees turns the box's length axis with it (to world +y)");
        const float rotCap[3] = {cellT.capB[0] - padC[0], cellT.capB[1] - padC[1], cellT.capB[2] - padC[2]};
        check(std::fabs(rotCap[2] - cellL.capB[2]) < 1e-3f,
              "the shaft target keeps its height along the pad's up axis when the pad turns about up");

        // Built v13.7 (b): the release by the pad's own motion along the shaft, against the hangar shell. A shift of the whole frame (pad and shell
        // alike) changes nothing; the pad rising 6 m relative to the shell releases; rising 4 m or 5 m does not (the limit is more than 5 m).
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shellE[3] = {0.0f, 0.0f, 50.0f}, padE[3] = {0.0f, 0.0f, 10.0f};
        const float relE = se::shellRelative(padE, shellE, up);
        const float shellS[3] = {T[0], T[1], 50.0f + T[2]}, padSh[3] = {T[0], T[1], 10.0f + T[2]};
        check(!se::padMovedAway(relE, se::shellRelative(padSh, shellS, up)),
              "a frame shift that moves the pad and the shell alike does not release the cell");
        const float padUp6[3] = {0.0f, 0.0f, 16.0f}, padUp4[3] = {0.0f, 0.0f, 14.0f}, padUp5[3] = {0.0f, 0.0f, 15.0f};
        check(se::padMovedAway(relE, se::shellRelative(padUp6, shellE, up)) && !se::padMovedAway(relE, se::shellRelative(padUp4, shellE, up)) &&
                  !se::padMovedAway(relE, se::shellRelative(padUp5, shellE, up)),
              "the pad rising 6 m against the shell releases the cell; 4 m and exactly 5 m do not");
        check(!se::padMovedAway(NAN, 0.0f), "an unreadable reference never releases");
    }
    std::printf("sealed_trial_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("sealed trial self-test passed\n");
    return 0;
}
