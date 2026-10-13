// Test rig for the hatch geometry probe's pure half (hatch_geometry_core.h; TEMPORARY, Built v8.2, v8.3, v13.8). The name rules and constants; the
// pad-local frame (a rigid motion of the station leaves a record's pad-local centre and axes unchanged); the axis angles; the tracker's change, settle,
// interval, disappearance and cap rules on a caller's clock, in the frame; the nearest hangar and the own-pad pick; the lower-cycle watch (the pad's
// bottom, the records that move after the settle, the close reasons); the line text; and the sample box. No DLL and no game.
#include "../../src/d3d11/hatch_geometry_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hg = edvr::hatchgeo;

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

bool contains(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// A sample in the frame (the tracker's and the watch's input): centre and identity axes unless given.
hg::Sample frameSample(uintptr_t addr, const char* name, float x, float y, float z) {
    hg::Sample s;
    s.addr = addr;
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.centre[0] = x;
    s.centre[1] = y;
    s.centre[2] = z;
    s.world[0] = x;
    s.world[1] = y;
    s.world[2] = z;
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) s.axes[k][j] = k == j ? 1.0f : 0.0f;
    s.axesOk = true;
    s.along = z;
    s.worldAxes[0][0] = s.worldAxes[1][1] = s.worldAxes[2][2] = 1.0f;
    s.axesSrc = 1;
    s.centreSrc = 1;
    return s;
}

// Rotates a sample's frame axes about z by deg (the record turns in its pad's frame).
void turnAboutZ(hg::Sample* s, float deg) {
    const float r = deg * 3.14159265f / 180.0f;
    const float c = std::cos(r), si = std::sin(r);
    // axes[0] and axes[1] turn about z; axes[2] (up) does not.
    const float a0[3] = {s->axes[0][0], s->axes[0][1], s->axes[0][2]};
    const float a1[3] = {s->axes[1][0], s->axes[1][1], s->axes[1][2]};
    for (int j = 0; j < 3; ++j) {
        s->axes[0][j] = c * a0[j] + si * a1[j];
        s->axes[1][j] = -si * a0[j] + c * a1[j];
    }
}

using Mat = float[3][3];

void rotZ(float rad, Mat m) {
    const float c = std::cos(rad), s = std::sin(rad);
    const float r[3][3] = {{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
    std::memcpy(m, r, sizeof(r));
}

void rotX(float rad, Mat m) {
    const float c = std::cos(rad), s = std::sin(rad);
    const float r[3][3] = {{1, 0, 0}, {0, c, -s}, {0, s, c}};
    std::memcpy(m, r, sizeof(r));
}

void mul(const Mat a, const Mat b, Mat out) {
    Mat t{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            t[i][j] = 0;
            for (int k = 0; k < 3; ++k) t[i][j] += a[i][k] * b[k][j];
        }
    std::memcpy(out, t, sizeof(t));
}

void apply(const Mat m, const float v[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // --- The constants and the name rules.
    check(hg::kSampleEvery == 6 && hg::kSampleCap == 128 && hg::kMissLimit == 30 && hg::kTrackCap == 256 && hg::kLineCap == 4000 &&
              hg::kPadLineCap == 3000,
          "the sampling, track and line caps");
    check(near(hg::kRangeM, 300.0f, 1e-6f) && near(hg::kLateralM, 80.0f, 1e-6f) && near(hg::kShellM, 150.0f, 1e-6f),
          "the range (300 m without a shaft), the lateral (80 m with one) and the shell (150 m)");
    check(near(hg::kMoveM, 0.05f, 1e-6f) && near(hg::kAxisDeg, 0.5f, 1e-6f) && hg::kChangeIntervalMs == 500 && hg::kSettleMs == 2000,
          "the change thresholds (0.05 m, 0.5 deg), the change interval (500 ms) and the settle (2 s)");
    check(hg::isGeometryName("BlastShield_A") && hg::isGeometryName("BlastShieldBase") && hg::isGeometryName("Hatch_01") &&
              hg::isGeometryName("ShutterLeft") && hg::isGeometryName("HangarLiftDoors__") && hg::isGeometryName("TurboLiftDoor") &&
              hg::isGeometryName("CollarTop") && hg::isGeometryName("LidA") && hg::isGeometryName("CoverPlate") && hg::isGeometryName("GateNorth") &&
              hg::isGeometryName("HangerSmall") && hg::isGeometryName("Hangar_b"),
          "the named records: BlastShield, Hatch, Shutter, Door, Collar, Lid, Cover, Gate, Hanger, Hangar");
    check(!hg::isGeometryName("DockingHub") && !hg::isGeometryName("HabitatRingA") && !hg::isGeometryName("TurboLift") &&
              !hg::isGeometryName("") && !hg::isGeometryName(nullptr),
          "other records (and a turbolift with no door in its name) are not followed");
    check(hg::keepsName("LandingPadSmall_A") && hg::keepsName("Door") && !hg::keepsName("LandingPadGuideMarkers") && !hg::keepsName("DockingHub"),
          "the store keeps the named records and every pad (not the guide markers)");
    check(hg::isHangerName("HangerSmall") && hg::isHangerName("hangar_x") && !hg::isHangerName("Agricultural_SmallHangar") &&
              hg::isBaseName("BlastShieldBase_A") && !hg::isBaseName("BlastShield_A"),
          "the shell names begin Hanger or Hangar; the base begins BlastShieldBase");

    // --- The pad-local frame: a rigid motion of the station (a turn and an origin rebase) leaves a record's pad-local centre and axes unchanged.
    {
        Mat qPad{};
        Mat rz{}, rx{};
        rotZ(0.7f, rz);
        rotX(0.3f, rx);
        mul(rz, rx, qPad);   // the pad's axes, in the station: axes[k] = column k
        const float origin[3] = {12.0f, -4.0f, 2.5f};
        const float rec[3] = {14.0f, -1.0f, 6.0f};
        Mat recRot{};
        rotZ(0.1f, recRot);
        // Pad-local: the record's centre, and its axes, measured from the pad's frame.
        hg::Frame f;
        f.padFrame = true;
        for (int j = 0; j < 3; ++j) f.origin[j] = origin[j];
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) f.axes[k][j] = qPad[j][k];
        float local[3] = {};
        hg::toFrame(f, rec, local);
        // The same world, moved by a rigid motion M and a translation T: the pad turns and moves with the station.
        Mat motion{};
        Mat mz{}, mx{};
        rotZ(1.9f, mz);
        rotX(-0.4f, mx);
        mul(mz, mx, motion);
        const float shift[3] = {-300.0f, 45.0f, 7.0f};
        float rec2[3] = {}, origin2[3] = {};
        apply(motion, rec, rec2);
        apply(motion, origin, origin2);
        for (int k = 0; k < 3; ++k) {
            rec2[k] += shift[k];
            origin2[k] += shift[k];
        }
        hg::Frame f2;
        f2.padFrame = true;
        std::memcpy(f2.origin, origin2, sizeof(origin2));
        for (int k = 0; k < 3; ++k) {
            float ax[3] = {qPad[0][k], qPad[1][k], qPad[2][k]};
            float turned[3] = {};
            apply(motion, ax, turned);
            for (int j = 0; j < 3; ++j) f2.axes[k][j] = turned[j];
        }
        float local2[3] = {};
        hg::toFrame(f2, rec2, local2);
        check(near(local[0], local2[0], 1e-3f) && near(local[1], local2[1], 1e-3f) && near(local[2], local2[2], 1e-3f),
              "a rigid motion of the station (turn and rebase) leaves a record's pad-local centre unchanged");
        // A record whose axes are the pad's axes turned 0.1 rad about the PAD's z (world axes = Q * Rz(0.1), columns): in the pad's frame its axes
        // are Rz(0.1), so the axis angles against the pad's own axes are 0.1 rad (5.73 deg) for u and v, and 0 for w.
        Mat rz01{};
        rotZ(0.1f, rz01);
        hg::Sample rs;
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) {
                float acc = 0.0f;
                for (int m = 0; m < 3; ++m) acc += qPad[j][m] * rz01[m][k];
                rs.worldAxes[k][j] = acc;
            }
        rs.axesSrc = 1;
        rs.axesOk = true;
        hg::axesToFrame(f, rs.worldAxes, rs.axes);
        const float idAxes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        check(near(hg::angleDeg(rs.axes[0], rs.axes[0]), 0.0f, 1e-6f), "an axis compared with itself is 0 deg");
        check(near(hg::axesDeg(rs.axes, idAxes), 0.1f * 180.0f / 3.14159265f, 1e-3f),
              "a record turned 0.1 rad about its pad's z reads 0.1 rad (5.73 deg) against the pad's axes");
    }

    // --- Point and direction in a pad frame: the pad at (10,0,0) with identity axes; and turned 90 degrees about z.
    {
        hg::Frame f;
        f.padFrame = true;
        f.origin[0] = 10.0f;
        const float p[3] = {10.0f, 3.0f, 4.0f};
        float out[3] = {};
        hg::toFrame(f, p, out);
        check(near(out[0], 0.0f, 1e-6f) && near(out[1], 3.0f, 1e-6f) && near(out[2], 4.0f, 1e-6f), "identity axes: the local point is the offset");
        hg::Frame t;
        t.padFrame = true;
        t.origin[0] = 10.0f;
        const float ax[3][3] = {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};
        std::memcpy(t.axes, ax, sizeof(ax));
        const float q[3] = {12.0f, 0.0f, 0.0f};
        float lq[3] = {};
        hg::toFrame(t, q, lq);
        check(near(lq[0], 0.0f, 1e-6f) && near(lq[1], -2.0f, 1e-6f) && near(lq[2], 0.0f, 1e-6f),
              "a pad turned 90 degrees: the world's +x offset is the pad's -y");
        float dl[3] = {};
        const float dv[3] = {0.0f, 0.0f, 1.0f};
        hg::dirToFrame(t, dv, dl);
        check(near(dl[2], 1.0f, 1e-6f) && near(dl[0], 0.0f, 1e-6f), "an up direction stays up in the pad's frame");
        hg::Frame none;
        float ident[3] = {};
        hg::toFrame(none, q, ident);
        check(near(ident[0], 12.0f, 1e-6f), "no own pad: the frame is the world");
    }

    // --- Angles.
    {
        const float x[3] = {1.0f, 0.0f, 0.0f};
        const float y[3] = {std::cos(0.5f * 3.14159265f / 180.0f), std::sin(0.5f * 3.14159265f / 180.0f), 0.0f};
        check(near(hg::angleDeg(x, y), 0.5f, 1e-3f), "a half-degree turn reads 0.5 deg");
        const float z[3] = {0.0f, 1.0f, 0.0f};
        check(near(hg::angleDeg(x, z), 90.0f, 1e-4f), "a right angle reads 90 deg");
    }

    // --- Localize: the along-shaft and lateral offsets from the shaft through the origin.
    {
        hg::Sample s = frameSample(1, "BlastShield_A", 1.0f, 2.0f, 3.0f);
        hg::Frame none;
        const float up[3] = {0.0f, 0.0f, 1.0f};
        s.world[0] = 1.0f;
        s.world[1] = 2.0f;
        s.world[2] = 3.0f;
        s.axesSrc = 4;
        hg::localize(none, up, &s);
        check(near(s.along, 3.0f, 1e-5f) && near(s.lateral, std::sqrt(5.0f), 1e-5f), "along and lateral from a shaft through the origin");
        check(!s.axesOk, "axes with no source are not read");
    }

    // --- The tracker: appear, change (one per 500 ms while moving), settle (2 s still), rotation, disappearance and the cap.
    {
        hg::Tracker tr;
        std::vector<hg::Event> ev;
        hg::Sample a = frameSample(0x10, "BlastShield_A", 0.0f, 0.0f, 0.0f);
        tr.apply(&a, 1, 0, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kAppear, "a new record appears once");
        ev.clear();
        tr.apply(&a, 1, 100, &ev);
        check(ev.empty(), "the same record, unmoved, logs nothing");

        // A move of 0.2 m at 100 ms: no change line yet (the first change waits 500 ms from the appearance); nothing while it is pending; settled
        // 2 s after the move, with the 0.2 m from the last line (the appearance).
        hg::Sample b = frameSample(0x10, "BlastShield_A", 0.2f, 0.0f, 0.0f);
        tr.apply(&b, 1, 100, &ev);
        check(ev.empty(), "a move of 0.2 m at 100 ms is not reported before the change interval");
        for (uint64_t t = 200; t <= 2000; t += 100) {
            tr.apply(&b, 1, t, &ev);
        }
        check(ev.empty(), "still for under 2 s after the move: nothing");
        tr.apply(&b, 1, 2100, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kSettled && near(ev[0].centreDelta, 0.2f, 1e-4f),
              "2 s after the move: one settled line with the 0.2 m since its last line");
        ev.clear();
        tr.apply(&b, 1, 2200, &ev);
        check(ev.empty(), "once settled and unchanged, nothing");

        // Continuous motion: 0.1 m every 100 ms from 2300 ms: change lines at most one per 500 ms (the first at 2800, then 3300).
        hg::Tracker tc;
        std::vector<hg::Event> ec;
        hg::Sample c0 = frameSample(0x20, "Door_B", 0.0f, 0.0f, 0.0f);
        tc.apply(&c0, 1, 0, &ec);
        ec.clear();
        int changes = 0;
        float lastDelta = 0.0f;
        for (int k = 1; k <= 10; ++k) {
            hg::Sample cur = frameSample(0x20, "Door_B", 0.1f * k, 0.0f, 0.0f);
            tc.apply(&cur, 1, static_cast<uint64_t>(k * 100), &ec);
        }
        for (const hg::Event& e : ec)
            if (e.kind == hg::Kind::kChange) {
                ++changes;
                lastDelta = e.centreDelta;
            }
        check(changes == 2 && near(lastDelta, 0.5f, 1e-3f), "moving 0.1 m every 100 ms: two change lines in a second, each 0.5 m since the last");

        // A rotation of 0.6 deg about the pad's z (the centre still): a change at 600 ms (after the 500 ms interval), and none after it settles.
        hg::Tracker tt;
        std::vector<hg::Event> et;
        hg::Sample r0 = frameSample(0x30, "Hatch_C", 1.0f, 1.0f, 1.0f);
        tt.apply(&r0, 1, 0, &et);
        et.clear();
        hg::Sample r1 = r0;
        turnAboutZ(&r1, 0.6f);
        tt.apply(&r1, 1, 600, &et);
        check(et.size() == 1 && et[0].kind == hg::Kind::kChange && near(et[0].axesDelta, 0.6f, 1e-3f) && near(et[0].centreDelta, 0.0f, 1e-6f),
              "a 0.6 deg turn at 600 ms, centre still: a change line with the 0.6 deg");
        et.clear();
        tt.apply(&r1, 1, 2700, &et);
        check(et.empty(), "after the change line, still: no settled line (the line already has the turn)");

        // A disappearance: 30 sampled frames absent, reported on the 30th with the last values.
        hg::Tracker td;
        std::vector<hg::Event> ed;
        hg::Sample d0 = frameSample(0x40, "Gate_D", 2.0f, 2.0f, 2.0f);
        td.apply(&d0, 1, 0, &ed);
        ed.clear();
        for (int k = 1; k <= 29; ++k) td.apply(nullptr, 0, static_cast<uint64_t>(k * 100), &ed);
        check(ed.empty(), "absent for 29 sampled frames: nothing yet");
        td.apply(nullptr, 0, 3000, &ed);
        check(ed.size() == 1 && ed[0].kind == hg::Kind::kDisappear && ed[0].missed == 30 && ed[0].s.addr == 0x40 &&
                  near(ed[0].s.centre[0], 2.0f, 1e-6f),
              "absent for 30: one disappearance with the last values");

        // The cap: kTrackCap records follow at once; the next is refused and counted.
        hg::Tracker full;
        std::vector<hg::Sample> many;
        for (uintptr_t i = 1; i <= hg::kTrackCap + 1; ++i) many.push_back(frameSample(i, "Hanger_X", 0.0f, 0.0f, 0.0f));
        std::vector<hg::Event> ef;
        full.apply(many.data(), many.size(), 0, &ef);
        check(full.present() == hg::kTrackCap && full.refused() == 1, "the tracker follows kTrackCap records; the next is refused and counted");
    }

    // --- The nearest hangar, and the own pad by the trial's rule.
    {
        std::vector<hg::Sample> s;
        s.push_back(frameSample(0x1, "Hangar_b", 5.0f, 0.0f, 0.0f));
        s.push_back(frameSample(0x2, "HangerSmall", 10.0f, 0.0f, 0.0f));
        s.push_back(frameSample(0x3, "BlastShield", 1.0f, 0.0f, 0.0f));
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        check(hg::nearestHangerIndex(origin, s.data(), s.size(), 0, 150.0f) == 0, "the nearest Hanger*/Hangar* is Hangar_b");
        check(hg::nearestHangerIndex(origin, s.data(), s.size(), 0x1, 150.0f) == 1, "the record itself is skipped");
        check(hg::nearestHangerIndex(origin, s.data(), s.size(), 0, 4.0f) == -1, "a Hanger beyond the limit is not named");

        std::vector<hg::Sample> pads;
        hg::Sample p1 = frameSample(0x100, "LandingPadSmall_A", 0.0f, 0.0f, 0.0f);
        p1.isPad = true;
        p1.radius = 40.0f;
        hg::Sample p2 = frameSample(0x200, "LandingPadLarge", 100.0f, 0.0f, 0.0f);
        p2.isPad = true;
        p2.radius = 150.0f;
        pads.push_back(p1);
        pads.push_back(p2);
        const float cam[3] = {10.0f, 0.0f, 0.0f};
        check(hg::pickOwnPad(pads.data(), pads.size(), cam) == 0x100, "the pad whose sphere holds the camera, nearest first");
        pads[0].radius = 1.0f;
        pads[1].radius = 1.0f;
        check(hg::pickOwnPad(pads.data(), pads.size(), cam) == 0x100, "no sphere holds the camera: the nearest pad within 150 m");
        std::vector<hg::Sample> none;
        check(hg::pickOwnPad(none.data(), 0, cam) == 0, "no pad: none");
    }

    // --- The lower-cycle watch.
    {
        // A lower: the pad at 5.0 m above the shell; from 1000 ms it falls to 4.0 by 1100 ms and holds. Settled 2 s after the last move: 3100 ms.
        hg::BottomWatch w;
        std::vector<hg::Sample> recs;
        recs.push_back(frameSample(0x1, "BlastShield_A", 0.0f, 0.0f, -3.0f));
        recs.push_back(frameSample(0x2, "Door_B", 2.0f, 0.0f, -1.0f));
        recs.push_back(frameSample(0x3, "BlastShieldBase", 0.0f, 0.0f, -1.0f));
        hg::PadNotice n = w.update(0, true, 5.0f, recs.data(), recs.size());
        check(n == hg::PadNotice::kNone && w.phase() == hg::PadPhase::kStill, "a still pad: no notice");
        n = w.update(100, true, 5.0f, recs.data(), recs.size());
        check(n == hg::PadNotice::kNone, "still at 100 ms: no notice");
        n = w.update(1000, true, 4.5f, recs.data(), recs.size());
        check(n == hg::PadNotice::kCycleStart && w.phase() == hg::PadPhase::kMoving && w.padMoving(), "the pad moves: a cycle starts");
        n = w.update(1100, true, 4.0f, recs.data(), recs.size());
        check(n == hg::PadNotice::kNone, "still falling: no notice");
        for (uint64_t t = 1200; t <= 3000; t += 100) n = w.update(t, true, 4.0f, recs.data(), recs.size());
        check(n == hg::PadNotice::kNone && w.phase() == hg::PadPhase::kMoving, "still for under 2 s: not yet the bottom");
        n = w.update(3100, true, 4.0f, recs.data(), recs.size());
        check(n == hg::PadNotice::kBottomSettled && w.phase() == hg::PadPhase::kBottom, "still 2 s after the last move, 1 m below the start: the bottom");

        // After the settle: A moves 0.5 m at 3200 ms and 0.5 m more at 3300 ms, then holds. B stays.
        recs[0].centre[2] = -2.5f;
        recs[0].along = -2.5f;
        n = w.update(3200, true, 4.0f, recs.data(), recs.size());
        recs[0].centre[2] = -2.0f;
        recs[0].along = -2.0f;
        n = w.update(3300, true, 4.0f, recs.data(), recs.size());
        for (uint64_t t = 3400; t <= 5200; t += 100) n = w.update(t, true, 4.0f, recs.data(), recs.size());
        check(w.phase() == hg::PadPhase::kBottom, "the records still moved: the summary is not closed before they settle");
        n = w.update(5300, true, 4.0f, recs.data(), recs.size());
        hg::Summary sm;
        const bool got = w.takeSummary(&sm);
        check(got && std::strcmp(sm.reason, "the moved records settled") == 0 && w.phase() == hg::PadPhase::kStill,
              "2 s after the last move of the records: the summary closes");
        check(got && sm.movedCount == 1 && sm.movedN == 1 && std::strcmp(sm.moved[0].name, "BlastShield_A") == 0,
              "only the record that moved is listed");
        if (got && sm.movedN == 1) {
            const hg::MovedRecord& r = sm.moved[0];
            check(near(sm.startH, 5.0f, 1e-5f) && near(sm.settleH, 4.0f, 1e-5f) && near(sm.endH, 4.0f, 1e-5f),
                  "the pad's heights: 5 m at the start, 4 m at the settle and at the close");
            check(near(r.startC[2], -3.0f, 1e-5f) && near(r.endC[2], -2.0f, 1e-5f), "the record's start and end pad-local centre");
            check(near(r.startS, 0.1f, 1e-4f) && near(r.stopS, 0.2f, 1e-4f), "the record's first and last moves, in seconds after the settle");
            check(near(r.startAlong, -3.0f, 1e-5f) && near(r.endAlong, -2.0f, 1e-5f) && near(r.rotationDeg, 0.0f, 1e-5f),
                  "the along heights and no rotation");
            check(sm.baseSeen && near(sm.baseAlong, -1.0f, 1e-5f), "the BlastShieldBase height at the close, for the reference");
        }
    }
    {
        // A record that turns after the settle (2 deg about z, its centre still) is listed with its rotation.
        hg::BottomWatch w;
        std::vector<hg::Sample> recs;
        recs.push_back(frameSample(0x5, "CollarTop", 1.0f, 1.0f, -2.0f));
        w.update(0, true, 6.0f, recs.data(), recs.size());
        w.update(1000, true, 5.0f, recs.data(), recs.size());
        for (uint64_t t = 1100; t <= 3100; t += 100) w.update(t, true, 5.0f, recs.data(), recs.size());
        check(w.phase() == hg::PadPhase::kBottom, "a 1 m fall settles at the bottom");
        turnAboutZ(&recs[0], 2.0f);
        w.update(3200, true, 5.0f, recs.data(), recs.size());
        for (uint64_t t = 3300; t <= 5400; t += 100) w.update(t, true, 5.0f, recs.data(), recs.size());
        hg::Summary sm;
        check(w.takeSummary(&sm) && sm.movedN == 1 && near(sm.moved[0].rotationDeg, 2.0f, 1e-2f) && sm.moved[0].axesOk,
              "a record that turns 2 deg after the settle is listed with the rotation");
    }
    {
        // The pad moves again before the records settle: the summary closes with that reason and the next cycle starts.
        hg::BottomWatch w;
        std::vector<hg::Sample> recs;
        recs.push_back(frameSample(0x6, "Shutter_E", 0.0f, 0.0f, -4.0f));
        w.update(0, true, 6.0f, recs.data(), recs.size());
        w.update(1000, true, 5.0f, recs.data(), recs.size());
        for (uint64_t t = 1100; t <= 3100; t += 100) w.update(t, true, 5.0f, recs.data(), recs.size());
        check(w.phase() == hg::PadPhase::kBottom, "the bottom settles");
        hg::PadNotice n = w.update(3200, true, 4.9f, recs.data(), recs.size());
        hg::Summary sm;
        check(n == hg::PadNotice::kCycleStart && w.takeSummary(&sm) && std::strcmp(sm.reason, "the pad moved again") == 0,
              "a move of the pad at the bottom closes the summary (reason: the pad moved again) and starts a cycle");
    }
    {
        // A rise (or a small fall) that settles is not a lower: no bottom, no summary.
        hg::BottomWatch w;
        std::vector<hg::Sample> recs;
        recs.push_back(frameSample(0x7, "Hatch_F", 0.0f, 0.0f, 0.0f));
        w.update(0, true, 4.0f, recs.data(), recs.size());
        w.update(1000, true, 5.0f, recs.data(), recs.size());
        bool sawNotLower = false;
        for (uint64_t t = 1100; t <= 3100; t += 100)
            sawNotLower = sawNotLower || w.update(t, true, 5.0f, recs.data(), recs.size()) == hg::PadNotice::kSettledNotLower;
        hg::Summary sm;
        check(sawNotLower && w.phase() == hg::PadPhase::kStill && !w.takeSummary(&sm),
              "a rise that settles is not a lower: no bottom, no summary");
    }
    {
        // A bottom where no record moves closes 30 s after the settle with none listed.
        hg::BottomWatch w;
        std::vector<hg::Sample> recs;
        recs.push_back(frameSample(0x8, "Hangar_G", 0.0f, 0.0f, -6.0f));
        w.update(0, true, 6.0f, recs.data(), recs.size());
        w.update(1000, true, 5.0f, recs.data(), recs.size());
        for (uint64_t t = 1100; t <= 3100; t += 100) w.update(t, true, 5.0f, recs.data(), recs.size());
        hg::Summary sm;
        for (uint64_t t = 3200; t <= 33500; t += 1000) w.update(t, true, 5.0f, recs.data(), recs.size());   // closes at 33200: 30 s after 3000
        check(w.takeSummary(&sm) && sm.movedCount == 0 && std::strcmp(sm.reason, "no record moved") == 0,
              "no record moves: the summary closes 30 s after the settle with none listed");
    }
    {
        // The probe turns off during a bottom: reset closes the summary with the reason.
        hg::BottomWatch w;
        std::vector<hg::Sample> recs;
        recs.push_back(frameSample(0x9, "Lid_H", 0.0f, 0.0f, -6.0f));
        w.update(0, true, 6.0f, recs.data(), recs.size());
        w.update(1000, true, 5.0f, recs.data(), recs.size());
        for (uint64_t t = 1100; t <= 3100; t += 100) w.update(t, true, 5.0f, recs.data(), recs.size());
        w.reset(3200, "the hatch probe is off");
        hg::Summary sm;
        check(w.takeSummary(&sm) && std::strcmp(sm.reason, "the hatch probe is off") == 0 && w.phase() == hg::PadPhase::kStill,
              "turning the probe off closes an open bottom with its reason");
    }

    // --- The lines: the record's line names the own pad, the frame, the local values, the world values with their source, and the status words.
    {
        hg::Event ce;
        ce.kind = hg::Kind::kChange;
        ce.s = frameSample(0xABC, "BlastShield_A", 1.0f, 2.0f, 3.0f);
        ce.s.world[0] = 100.0f;
        ce.s.world[1] = 200.0f;
        ce.s.world[2] = 300.0f;
        ce.s.centreSrc = 0;
        ce.s.lateral = std::sqrt(5.0f);
        ce.centreDelta = 0.1f;
        ce.axesDelta = 0.6f;
        ce.totalMove = 0.2f;
        hg::LineContext lc;
        lc.hasCamera = true;
        lc.ms = 99;
        lc.statusKnown = true;
        lc.flags = 3;
        lc.flags2 = 4;
        lc.padFrame = true;
        std::snprintf(lc.frameName, sizeof(lc.frameName), "LandingPadMedium_A");
        lc.frameAddr = 0x500;
        lc.ownAddr = 0x500;
        lc.hasShaft = true;
        lc.held = true;
        char evLine[1400];
        hg::formatEvent(evLine, sizeof(evLine), ce, lc, 7);
        check(contains(evLine, "hatch geometry: changed frame 7 t=99 ms: rec 0xABC name=BlastShield_A") &&
                  contains(evLine, "local centre (1.000,2.000,3.000) along 3.000 m lateral 2.236 m") &&
                  contains(evLine, "world centre (100.000,200.000,300.000) from producer-B"),
              "a change line: the record, its pad-local centre with the along and lateral, and the world centre with its source");
        check(contains(evLine, "frame=own pad LandingPadMedium_A rec 0x500") && contains(evLine, "[own pad]") == false,
              "the frame names the own pad (the record is not the own pad here)");
        check(contains(evLine, "shaft held cell") && contains(evLine, "moved 0.100 m since its last line, rotated 0.60 deg since its last line") &&
                  contains(evLine, "status flags=3 flags2=4"),
              "the shaft source, the move and the rotation since the last line, and the status words");
        lc.ownAddr = 0xABC;
        hg::formatEvent(evLine, sizeof(evLine), ce, lc, 7);
        check(contains(evLine, "name=BlastShield_A [own pad]"), "the own pad's own line is tagged");
        hg::Event none = ce;
        none.kind = hg::Kind::kDisappear;
        none.missed = 30;
        hg::LineContext noCam;
        hg::formatEvent(evLine, sizeof(evLine), none, noCam, 9);
        check(contains(evLine, "disappeared frame 9") && contains(evLine, "not seen for 30 sampled frames") && contains(evLine, "camera unknown") &&
                  contains(evLine, "frame=world") && contains(evLine, "no shaft") && contains(evLine, "flags unknown"),
              "a disappearance with no camera, no pad frame and no status says so");
    }
    {
        hg::PadLine p;
        p.ms = 4242;
        p.padFrame = true;
        std::snprintf(p.padName, sizeof(p.padName), "LandingPadSmall_A");
        p.padAddr = 0x77;
        p.shellOk = true;
        std::snprintf(p.shellName, sizeof(p.shellName), "HangerSmall");
        p.shellDistM = 12.0f;
        p.relH = 4.25f;
        p.firstH = 4.5f;
        p.firstSet = true;
        p.phase = hg::PadPhase::kMoving;
        p.moving = true;
        p.hasShaft = false;
        char line[600];
        hg::formatPadLine(line, sizeof(line), p, 11);
        check(contains(line, "hatch pad: frame 11 t=4242 ms") && contains(line, "own pad LandingPadSmall_A rec 0x77") &&
                  contains(line, "shell HangerSmall at 12.00 m") && contains(line, "height above the shell 4.250 m (change since the first reading -0.250 m)") &&
                  contains(line, "phase moving (moving)") && contains(line, "shaft none"),
              "a pad line: the own pad, the shell, the height and its change, the phase and the shaft");
    }
    {
        hg::Summary sm;
        std::snprintf(sm.reason, sizeof(sm.reason), "%s", "the moved records settled");
        sm.startH = 5.0f;
        sm.settleH = 4.0f;
        sm.endH = 4.0f;
        sm.settleMs = 3100;
        sm.watched = 3;
        sm.movedCount = 1;
        sm.baseSeen = true;
        std::snprintf(sm.baseName, sizeof(sm.baseName), "BlastShieldBase");
        sm.baseAlong = -1.0f;
        char line[900];
        hg::formatSummary(line, sizeof(line), sm, 17);
        check(contains(line, "lower cycle summary (frame 17)") && contains(line, "a fall of 1.000 m") && contains(line, "closed: the moved records settled"),
              "the summary header: the heights, the fall, and the close reason");
        hg::MovedRecord r;
        r.addr = 0x1;
        std::snprintf(r.name, sizeof(r.name), "BlastShield_A");
        r.startAlong = -3.0f;
        r.endAlong = -2.0f;
        r.rotationDeg = 0.0f;
        r.axesOk = false;
        hg::formatMoved(line, sizeof(line), r, sm, 17);
        check(contains(line, "name=BlastShield_A") && contains(line, "along start -3.000 m end -2.000 m") && contains(line, "BlastShieldBase at along -1.000 m: the end is -1.000 m from it"),
              "a moved record line: its along heights and its place against the base");
    }

    // --- The sample box: one kept per record; past the reserve a drop; the render context is kept with the sample.
    {
        hg::SampleBox<std::mutex> box;
        {
            hg::SampleStorage s;
            s.reserve(2);
            (void)box.exchange(std::move(s));
        }
        hg::Sample k = frameSample(0x10, "Door", 0, 0, 0);
        box.add(k, 0x7000);
        box.add(k);
        box.add(frameSample(0x20, "Door", 0, 0, 0));
        box.add(frameSample(0x30, "Door", 0, 0, 0));
        hg::SampleStorage got = box.exchange(hg::SampleStorage{});
        check(got.items.size() == 2 && got.drops == 1, "a record twice is kept once; past the reserve it is a drop");
        check(got.ctx == 0x7000, "the render context the store read is kept with the sample");
    }

    std::printf("hatch_geometry_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("hatch geometry self-test passed\n");
    return 0;
}
