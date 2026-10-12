// Test rig for the hatch geometry probe's pure half (src/d3d11/hatch_geometry_core.h; TEMPORARY, Built v8.2, revised v8.3). The name rules, the
// 400 m camera filter, the tracker's appear, change (one line a second while moving), settled (2 s still) and disappear (30 absent samples), the
// track cap, the nearest Hanger record and its vector, the first-sample names, the line text, and the sample box (one kept per record, drops past
// the reserve, the render context kept). No DLL and no game.
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

hg::Sample sampleAt(uintptr_t addr, const char* name, float x, float y, float z) {
    hg::Sample s;
    s.addr = addr;
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.centre[0] = x;
    s.centre[1] = y;
    s.centre[2] = z;
    s.poseOk = true;
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const bool selfTest = argc > 1 && std::strcmp(argv[1], "--self-test") == 0;
    if (!selfTest) {
        std::printf("usage: hatch_geometry_test --self-test\n");
        return 2;
    }

    // --- The names and the constants.
    check(hg::isGeometryName("BlastShield_A") && hg::isGeometryName("LandingPadSmall") && hg::isGeometryName("HangarLiftDoors__") &&
              hg::isGeometryName("SmallHangerSkirt") && hg::isGeometryName("door_lift_01"),
          "the five words select a record, in any case");
    check(!hg::isGeometryName("DockingHub") && !hg::isGeometryName("HabitatRingA") && !hg::isGeometryName("") && !hg::isGeometryName(nullptr),
          "other names are not followed");
    check(hg::isHangerName("HangerSmall") && hg::isHangerName("hangar_x") && !hg::isHangerName("Agricultural_SmallHangar") &&
              !hg::isHangerName("BlastShield"),
          "the Hanger and Hangar prefixes are the distance references; a word inside a name is not");
    check(hg::kSampleEvery == 6 && hg::kSampleCap == 64 && hg::kMissLimit == 30 && hg::kTrackCap == 256 && hg::kLineCap == 3000 &&
              hg::kRangeM == 400.0f && hg::kChangeIntervalMs == 1000 && hg::kSettleMs == 2000,
          "one frame in six, 64 records a sample, 30 absent samples, 256 tracks, 3000 lines, 400 m, 1 s between changes, 2 s to settle");

    // --- The 400 m filter: records beyond the range of the camera are dropped, the rest keep their order.
    {
        const float cam[3] = {0.0f, 0.0f, 0.0f};
        hg::Sample s[4] = {sampleAt(1, "Lift", 100, 0, 0), sampleAt(2, "Lift", 401, 0, 0), sampleAt(3, "Hanger", 0, 399, 0),
                           sampleAt(4, "Lift", 0, 0, -400.5f)};
        const size_t kept = hg::keepWithin(s, 4, cam, hg::kRangeM);
        check(kept == 2 && s[0].addr == 1 && s[1].addr == 3, "only records within 400 m of the camera are kept, in order");
    }

    // --- The tracker: time is the caller's clock. Walk the timeline.
    {
        hg::Tracker tr;
        std::vector<hg::Event> ev;
        hg::Sample a = sampleAt(0x1000, "BlastShield_A", 0, 0, 0);
        tr.apply(&a, 1, 0, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kAppear && ev[0].sample == 0, "a new record appears once");
        ev.clear();
        tr.apply(&a, 1, 50, &ev);
        check(ev.empty(), "the same record, unmoved, logs nothing");
        a.centre[0] = 0.1f;
        tr.apply(&a, 1, 100, &ev);
        check(ev.empty(), "a move of 0.1 m at 100 ms is not reported yet: the first change waits one second from the appearance");
        tr.apply(&a, 1, 500, &ev);
        check(ev.empty(), "still, at 500 ms: nothing (the record has not settled)");
        a.centre[0] = 0.2f;
        tr.apply(&a, 1, 1000, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kChange && std::fabs(ev[0].centreDelta - 0.2f) < 1e-4f &&
                  std::fabs(ev[0].totalMove - 0.2f) < 1e-4f,
              "at one second the moving record reports a change: 0.2 m from its appearance");
        ev.clear();
        a.centre[0] = 0.3f;
        tr.apply(&a, 1, 1100, &ev);
        check(ev.empty(), "moving again 100 ms later: no second change line within the one-second interval");
        tr.apply(&a, 1, 1200, &ev);
        check(ev.empty(), "still at 1200 ms: pending, not yet settled");
        tr.apply(&a, 1, 3000, &ev);
        check(ev.empty(), "still at 3000 ms: 1900 ms since it last moved, below the two-second settle");
        tr.apply(&a, 1, 3100, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kSettled && std::fabs(ev[0].centreDelta - 0.1f) < 1e-4f &&
                  std::fabs(ev[0].totalMove - 0.3f) < 1e-4f,
              "two seconds after it last moved, the settled line gives the move since the last line and the total since it appeared");
        ev.clear();
        tr.apply(&a, 1, 4000, &ev);
        check(ev.empty(), "once settled it logs nothing while still");
    }

    // --- A pose change alone (the centre still) is a change, at the interval.
    {
        hg::Tracker tr;
        std::vector<hg::Event> ev;
        hg::Sample a = sampleAt(0x2000, "LandingPadSmallA", 5, 5, 5);
        tr.apply(&a, 1, 0, &ev);
        ev.clear();
        a.pose[3] = 0.002f;
        tr.apply(&a, 1, 1500, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kChange && std::fabs(ev[0].poseDelta - 0.002f) < 1e-5f && ev[0].centreDelta < 1e-6f,
              "a pose float moving 0.002 (the centre still) is a change, with the pose delta named");
    }

    // --- Disappearance after 30 absent samples, showing the last centre; reappearance is an appearance.
    {
        hg::Tracker tr;
        std::vector<hg::Event> ev;
        hg::Sample a = sampleAt(0x3000, "Hanger", 7, 0, 0);
        tr.apply(&a, 1, 0, &ev);
        ev.clear();
        for (int i = 0; i < 29; ++i) tr.apply(nullptr, 0, 100u * static_cast<uint64_t>(i + 1), &ev);
        check(ev.empty(), "absent for 29 samples: nothing yet");
        tr.apply(nullptr, 0, 3000, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kDisappear && ev[0].missed == 30 && ev[0].addr == 0x3000 &&
                  ev[0].sample == hg::kNoSample && std::fabs(ev[0].centre[0] - 7.0f) < 1e-6f,
              "absent for 30 samples: disappeared, with the last centre and no sample index");
        ev.clear();
        tr.apply(&a, 1, 3100, &ev);
        check(ev.size() == 1 && ev[0].kind == hg::Kind::kAppear, "the record back again appears again");
    }

    // --- The track cap: kTrackCap records are followed; the next is refused and counted.
    {
        hg::Tracker full;
        std::vector<hg::Sample> many(hg::kTrackCap + 1);
        for (size_t i = 0; i < many.size(); ++i) many[i] = sampleAt(0x4000u + i * 0x10u, "Hanger", static_cast<float>(i), 0, 0);
        std::vector<hg::Event> fe;
        full.apply(many.data(), many.size(), 0, &fe);
        check(fe.size() == hg::kTrackCap && full.refused() == 1, "the tracker follows kTrackCap records; the next is refused and counted");
    }

    // --- The nearest Hanger record within the limit: its vector from the point, its distance and its name; the record itself is skipped.
    {
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        hg::Sample near3[3] = {sampleAt(1, "HangerSmall", 10, 0, 0), sampleAt(2, "Hangar_b", 3, 4, 0), sampleAt(3, "BlastShield", 0, 0, 1)};
        const hg::Nearest n1 = hg::nearestHanger(origin, near3, 3, 0, 400.0f);
        check(n1.has && std::fabs(n1.dist - 5.0f) < 1e-5f && std::strcmp(n1.name, "Hangar_b") == 0 && std::fabs(n1.vec[0] - 3.0f) < 1e-5f &&
                  std::fabs(n1.vec[1] - 4.0f) < 1e-5f,
              "the nearest Hanger record is 5 m away, along the vector (3,4,0); the BlastShield is not a reference");
        const hg::Nearest n2 = hg::nearestHanger(origin, near3, 3, 2, 400.0f);
        check(n2.has && std::fabs(n2.dist - 10.0f) < 1e-5f && std::strcmp(n2.name, "HangerSmall") == 0, "the record itself is skipped");
        const hg::Nearest n3 = hg::nearestHanger(origin, near3, 3, 0, 4.0f);
        check(!n3.has, "a Hanger record beyond the limit is not named");
        check(!hg::nearestHanger(origin, near3 + 2, 1, 0, 400.0f).has, "no Hanger record: none");
    }

    // --- The first-sample names: most first, with counts.
    {
        hg::Sample names3[3] = {sampleAt(1, "HangerSmall", 0, 0, 0), sampleAt(2, "BlastShield", 0, 0, 0), sampleAt(3, "HangerSmall", 0, 0, 0)};
        char nm[256];
        hg::formatNameCounts(nm, sizeof(nm), names3, 3);
        check(std::string(nm) == "HangerSmall x2, BlastShield x1", "the first-sample names: most first, with counts");
    }

    // --- The line: the camera, the distance to it, the move and the total, the nearest hangar's vector, the pose state and the status words.
    {
        hg::Event ce;
        ce.kind = hg::Kind::kChange;
        ce.addr = 0xABC;
        std::snprintf(ce.name, sizeof(ce.name), "BlastShield_A");
        ce.centre[0] = 1.0f;
        ce.centre[1] = 2.0f;
        ce.centre[2] = 3.0f;
        ce.centreDelta = 0.1f;
        ce.totalMove = 0.2f;
        ce.poseOk = false;
        hg::LineContext lc;
        lc.hasCamera = true;
        lc.camera[0] = 0.0f;
        lc.camera[1] = 0.0f;
        lc.camera[2] = 0.0f;
        lc.ms = 99;
        lc.statusKnown = true;
        lc.flags = 3;
        lc.flags2 = 4;
        hg::Nearest hangar;
        hangar.has = true;
        hangar.dist = 5.0f;
        hangar.vec[0] = 3.0f;
        hangar.vec[1] = 4.0f;
        hangar.vec[2] = 0.0f;
        std::snprintf(hangar.name, sizeof(hangar.name), "Hangar_b");
        char evLine[1400];
        hg::formatEvent(evLine, sizeof(evLine), ce, lc, 7, hangar);
        check(contains(evLine, "hatch geometry: changed frame 7 t=99 ms: rec 0xABC name=BlastShield_A centre (1.000,2.000,3.000) "
                               "camera (0.00,0.00,0.00) dist_cam 3.74 m"),
              "a line names the record, its centre, the camera and the distance to it");
        check(contains(evLine, "moved 0.100 m since its last line, total 0.200 m since it appeared"), "a change line gives the move and the total");
        check(contains(evLine, "nearest_hangar=Hangar_b vector (3.00,4.00,0.00) dist 5.00 m"), "the nearest hangar's name, vector and distance");
        check(contains(evLine, "pose=unreadable") && contains(evLine, "status flags=3 flags2=4"), "the pose state and the status words");
        hg::Event none = ce;
        none.kind = hg::Kind::kDisappear;
        none.missed = 30;
        hg::LineContext noCam;
        hg::Nearest noHangar;
        hg::formatEvent(evLine, sizeof(evLine), none, noCam, 9, noHangar);
        check(contains(evLine, "disappeared frame 9") && contains(evLine, "not seen for 30 sampled frames") &&
                  contains(evLine, "camera unknown") && contains(evLine, "nearest_hangar=none") && contains(evLine, "flags unknown"),
              "a disappearance line says so; with no camera, no hangar and no status it says unknown");
    }

    // --- The sample box: one kept per record; past the reserve a drop; the render context is kept with the sample.
    {
        hg::SampleBox<std::mutex> box;
        {
            hg::SampleStorage s;
            s.reserve(2);
            (void)box.exchange(std::move(s));
        }
        box.add(sampleAt(0x10, "Lift", 0, 0, 0), 0x7000);
        box.add(sampleAt(0x10, "Lift", 0, 0, 0));
        box.add(sampleAt(0x20, "Lift", 0, 0, 0));
        box.add(sampleAt(0x30, "Lift", 0, 0, 0));
        hg::SampleStorage got = box.exchange(hg::SampleStorage{});
        check(got.items.size() == 2 && got.drops == 1, "a record twice is kept once; past the reserve it is a drop");
        check(got.ctx == 0x7000, "the render context the store read is kept with the sample");
    }

    std::printf("hatch_geometry_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("hatch geometry self-test passed\n");
    return 0;
}
