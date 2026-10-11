// Test rig for the shadow gate probe's pure half (src/d3d11/shadow_gate_core.h), v2: the pinned bytes (the true copies
// accepted, a one-byte change refused), the builder's centre against a hand-worked case, the view match, the candidate
// choice, the first failing gate in engine order, the 23-column CSV, the header, candidate, callee-check, cadence,
// sun-view and call lines. No DLL and no game.
#include "../../src/d3d11/shadow_gate_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace sg = edvr::shadowgate;

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

bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

int countFields(const std::string& s) {
    int n = 1;
    for (char c : s)
        if (c == ',') ++n;
    return n;
}

// Each pinned array: the true bytes accepted, every single-byte change refused.
template <size_t N>
void checkPin(const uint8_t (&pin)[N], const char* name) {
    uint8_t got[N] = {};
    std::memcpy(got, pin, N);
    check(sg::pinMatches(got, pin, N), name);
    for (size_t i = 0; i < N; ++i) {
        uint8_t changed[N] = {};
        std::memcpy(changed, pin, N);
        changed[i] = static_cast<uint8_t>(changed[i] ^ 0x01);
        check(!sg::pinMatches(changed, pin, N), name);
    }
    check(!sg::pinMatches(nullptr, pin, N), "a null read is refused");
}

}  // namespace

int main(int argc, char** argv) {
    const bool selfTest = argc > 1 && std::strcmp(argv[1], "--self-test") == 0;
    if (!selfTest) {
        std::printf("usage: shadow_gate_probe_test --self-test\n");
        return 2;
    }

    // --- The pins: the hook gate. A one-byte change of the exe's bytes is refused.
    checkPin(sg::kWindowPin, "window prologue pin");
    checkPin(sg::kCalleePin, "callee prologue pin");
    checkPin(sg::kDepthPin, "depth prologue pin");
    checkPin(sg::kMsmPin, "msm prologue pin");
    checkPin(sg::kPlanesPin, "planes prologue pin");
    checkPin(sg::kCalleeSitePin, "callee call-site pin");
    checkPin(sg::kCallbackSitePin, "callback call-site pin");
    check(sg::kExpectedTimestamp == 1788384820u && sg::kExpectedImageSize == 104894464u, "build 332841 identity");
    check(sg::kCallbackReturnRva == sg::kCallbackSiteRva + 5, "the builder's callback returns after its 5-byte call");
    check(sg::kCalleeReturnRva == sg::kCalleeSiteRva + 5, "the callback's callee returns after its 5-byte call");
    check(sg::kCadenceFrames == 600, "the cadence is 600 frames");

    // --- The builder's centre: the hand-worked case q=(0,0,0,1), t=(1,2,3,0), c=(4,5,6,7) gives (5,7,9,7).
    {
        const float q[4] = {0, 0, 0, 1}, t[4] = {1, 2, 3, 0}, c[4] = {4, 5, 6, 7};
        float out[4] = {};
        sg::builderCentre(q, t, c, out);
        check(near(out[0], 5) && near(out[1], 7) && near(out[2], 9) && near(out[3], 7), "builder centre, identity pose");
    }

    // --- The view match: views at ctx+0x40 + k*0x6A0.
    {
        const uintptr_t ctx = 0x100000;
        check(sg::viewIndexOf(ctx + 0x40 + 3 * 0x6A0, ctx, 8) == 3, "a view object matches its index");
        check(sg::viewIndexOf(ctx + 0x40, ctx, 8) == 0, "view 0 matches");
        check(sg::viewIndexOf(ctx + 0x40 + 3 * 0x6A0 + 8, ctx, 8) == -1, "an interior pointer matches no view");
        check(sg::viewIndexOf(ctx + 0x40 + 8 * 0x6A0, ctx, 8) == -1, "a view past the count matches none");
        check(sg::viewIndexOf(ctx + 0x40 + 64 * 0x6A0, ctx, 200) == -1, "the view count is capped at 64");
        check(sg::viewIndexOf(0, ctx, 8) == -1 && sg::viewIndexOf(ctx, 0, 8) == -1, "null inputs match nothing");
    }

    // --- Candidates: the camera test and the largest-radius lists.
    {
        const float cam[3] = {0, 0, 0};
        const float at[3] = {3, 4, 0};
        check(sg::makeCandidate(1, 2, at, 5.0f, cam).contains, "a sphere whose edge is at the camera contains it");
        check(!sg::makeCandidate(1, 2, at, 4.9f, cam).contains, "a sphere short of the camera does not contain it");
        check(near(sg::makeCandidate(1, 2, at, 4.9f, cam).dist, 5.0f), "distance from the camera");
        sg::LargestRadius<4> top;
        const float radii[9] = {1, 5, 3, 9, 2, 7, 8, 4, 6};
        for (int i = 0; i < 9; ++i) top.offer(sg::makeCandidate(uintptr_t(100 + i), 0, at, radii[i], cam));
        check(top.size() == 4, "the largest-radius list holds K");
        check(near(top[0].radius, 9) && near(top[1].radius, 8) && near(top[2].radius, 7) && near(top[3].radius, 6),
              "the largest-radius list is sorted largest first");
        sg::LargestRadius<2> ties;
        ties.offer(sg::makeCandidate(1, 0, at, 5.0f, cam));
        ties.offer(sg::makeCandidate(2, 0, at, 5.0f, cam));
        check(ties[0].record == 1, "a tie keeps the earlier candidate");
    }

    // --- The first failing gate, in engine order (G2 is the engine's window bit).
    {
        sg::GateInput in;
        in.g1 = false;
        check(sg::firstFailingGate(in) == sg::Gate::G1, "no collection bit: G1");
        in.g1 = true;
        check(sg::firstFailingGate(in) == sg::Gate::NoWindow, "no window observation: noWindow");
        in.windowKnown = true;
        check(sg::firstFailingGate(in) == sg::Gate::G2, "window bit clear: G2");
        in.window = true;
        check(sg::firstFailingGate(in) == sg::Gate::Lod, "window passed, rec+0x208 clear: LOD");
        in.recMask = true;
        check(sg::firstFailingGate(in) == sg::Gate::Unreached, "rec+0x208 set, builder not reached: unreached");
        in.builderReached = true;
        in.planes = -1;
        check(sg::firstFailingGate(in) == sg::Gate::G3, "planes outside: G3");
        in.planes = 1;
        in.visApplies = true;
        in.g4 = 0;
        check(sg::firstFailingGate(in) == sg::Gate::G4, "callback veto: G4");
        in.g4 = 1;
        check(sg::firstFailingGate(in) == sg::Gate::None, "callback passed: admitted");
        in.visApplies = false;
        in.g4 = -1;
        check(sg::firstFailingGate(in) == sg::Gate::None, "no callback applies: admitted");
        check(std::strcmp(sg::gateName(sg::Gate::G3), "G3") == 0 && std::strcmp(sg::gateName(sg::Gate::None), "admit") == 0,
              "gate names");
    }

    // --- The CSV: 23 columns, the header and each row agree; the replica columns are gone.
    {
        sg::CsvRow r;
        r.candidate = 3;
        r.kind = "contains";
        r.record = 0x1234;
        r.bucket = 0x5678;
        r.base = 0x1000;
        r.role = "child";
        r.radius = 1500.5f;
        r.dist = 1200.25f;
        r.contains = 1;
        r.view = 5;
        r.viewRole = "sun";
        r.viewBits = 0x20;
        r.g1 = 1;
        r.windowBit = 0;
        r.recBit = 0;
        r.builderReached = 1;
        r.builderRow = 42;
        r.builderMaskBit = 1;
        r.planes = -1;
        r.visApplies = 1;
        r.g4 = -1;
        r.gate = sg::Gate::G2;
        char line[512];
        const int n = sg::formatCsvRow(line, sizeof(line), 77, r);
        check(n > 0, "a CSV row formats");
        const std::string row = line;
        check(countFields(sg::csvHeader()) == 23, "the header has 23 columns");
        check(countFields(row) == 23, "the CSV row has 23 fields");
        check(row.rfind("77,3,contains,0x1234,0x5678,0x1000,child,1500.50,1200.25,1,5,sun,0x20,1,0,0,1,42,1,-1,1,-1,G2", 0) == 0,
              "the CSV row's fields are in header order");
        check(std::string(sg::csvHeader()).find("window_v") == std::string::npos &&
                  std::string(sg::csvHeader()).find("window_repl") == std::string::npos,
              "the replica columns are gone");
    }

    // --- The header, the candidate line, and the callee check.
    {
        sg::HeaderInfo h;
        h.frame = 77;
        h.trigger = "hotkey.shadow_gate_probe";
        h.build = "test";
        h.ctx = 0xABC000;
        h.views = 8;
        h.camera[0] = 1.5f;
        h.camera[1] = -2.0f;
        h.camera[2] = 3.25f;
        h.csvName = "shadow_gate_20261011_120000.csv";
        char line[640];
        sg::formatHeaderLine(line, sizeof(line), h);
        const std::string s = line;
        check(s.rfind("shadow gate probe: frame=77", 0) == 0, "the header starts with the frame");
        check(s.find("csv=shadow_gate_20261011_120000.csv") != std::string::npos, "the header names the CSV");
        check(s.find("window_mismatch") == std::string::npos && s.find("sun=") == std::string::npos,
              "the header carries no replica and no sun list (the cadence reports those)");

        sg::CandidateLine c;
        c.index = 0;
        c.kind = "contains";
        c.record = 0x2000;
        c.bucket = 0x3000;
        c.role = "base";
        c.radius = 1800.0f;
        c.dist = 900.0f;
        c.sun = "v3=G3 v5=admit";
        c.main = "v0=admit";
        sg::formatCandidateLine(line, sizeof(line), c);
        const std::string cs = line;
        check(cs.find("rec=0x2000") != std::string::npos && cs.find("sun[v3=G3 v5=admit]") != std::string::npos &&
                  cs.find("main[v0=admit]") != std::string::npos,
              "a candidate line carries the record and both summaries");

        char check1[200];
        sg::formatCalleeCheck(check1, sizeof(check1), 0, 0, 0, 0x7FF42B45D5u);
        check(std::string(check1).find("not applicable") != std::string::npos,
              "the callee check is not applicable when vis_applies is 0 in every row");
        sg::formatCalleeCheck(check1, sizeof(check1), 3, 0, 0, 0x7FF42B45D5u);
        check(std::string(check1).find("no callee call") != std::string::npos, "the callee check says when no call was seen");
        sg::formatCalleeCheck(check1, sizeof(check1), 3, 5, 0x7FF42B45D5u, 0x7FF42B45D5u);
        check(std::string(check1).find("match") != std::string::npos && std::string(check1).find("MISMATCH") == std::string::npos,
              "the callee check matches the builder site");
        sg::formatCalleeCheck(check1, sizeof(check1), 3, 5, 0x1u, 0x7FF42B45D5u);
        check(std::string(check1).find("MISMATCH") != std::string::npos, "the callee check reports a mismatch");
    }

    // --- The cadence: histogram, per-frame row, the sun view line, the call line.
    {
        uint32_t counts[5] = {};
        sg::addToHistogram(counts, 0);
        sg::addToHistogram(counts, 0);
        sg::addToHistogram(counts, 1);
        sg::addToHistogram(counts, 4);
        sg::addToHistogram(counts, 9);
        check(counts[0] == 2 && counts[1] == 1 && counts[2] == 0 && counts[4] == 2, "the histogram buckets 0, 1, 2, 3 and 4+");
        char h[200];
        sg::formatHistogram(h, sizeof(h), "shadow cadence depth calls per frame:", counts);
        check(std::string(h) == "shadow cadence depth calls per frame: 0=2 1=1 2=0 3=0 4+=2", "the histogram line");

        char row[160];
        sg::formatCadenceRow(row, sizeof(row), 4242, 1, -1, 3, 0);
        check(std::string(row) == "4242,1,-1,3,0" && countFields(sg::cadenceHeader()) == countFields(row),
              "the cadence row matches its header");

        const float origin[3] = {10, 20, 30};
        char v[400];
        sg::formatSunViewLine(v, sizeof(v), 3, 0x1000 + 3 * 0x6A0, 0x30ull, 0.5f, 2.0f, origin, 2, 0);
        const std::string vs = v;
        check(vs.find("shadow sun view 3:") == 0 && vs.find("bits=0x30") != std::string::npos &&
                  vs.find("depth_hits=2 msm_hits=0 shadow=yes") != std::string::npos,
              "the sun view line names the view, its bits and the shadow hit");
        sg::formatSunViewLine(v, sizeof(v), 4, 0x1000, 0, 0.0f, 0.0f, origin, 0, 0);
        check(std::string(v).find("shadow=no") != std::string::npos, "a view no shadow builder named says so");

        char cl[300];
        sg::formatCallLine(cl, sizeof(cl), 7, "depth", 88, 0x10, 0x20, 0x30, 0x40, 2);
        check(std::string(cl) == "shadow call 7: depth frame=88 rcx=0x10 rdx=0x20 r8=0x30 r9=0x40 view=2",
              "the call line carries the four arguments and the view");
    }

    std::printf("shadow_gate_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("shadow gate probe self-test passed\n");
    return 0;
}
