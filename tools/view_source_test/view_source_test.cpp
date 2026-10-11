// Test rig for the view-source core (src\d3d11\view_source_core.h: which view the aim comes from, and the pose read from the game's view rows
// [R | c]) and the kept-records report core (src\d3d11\kept_report_core.h: why a record is kept, the bounded top lists, the counts and the
// CSV rows). No game, no DLL.
#include "../../src/d3d11/kept_report_core.h"
#include "../../src/d3d11/view_source_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

bool close(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// The text of field `index` (1-based) of a comma-separated row; empty when the field is empty or absent.
std::string fieldOf(const std::string& line, int index) {
    int at = 1;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            if (at == index) return line.substr(start, i - start);
            ++at;
            start = i + 1;
        }
    }
    return std::string();
}

int fieldCount(const std::string& line) {
    int n = 1;
    for (char c : line)
        if (c == ',') ++n;
    return n;
}

// The game's view-to-world rows for a camera at pos whose world axes are right, up and forward (view_source_core.h): row r is
// (right[r], up[r], forward[r], pos[r]), so column 0 is right, column 1 up and column 2 forward.
void rowsFor(const float pos[3], const float right[3], const float up[3], const float fwd[3], float rows[12]) {
    for (int r = 0; r < 3; ++r) {
        rows[4 * r + 0] = right[r];
        rows[4 * r + 1] = up[r];
        rows[4 * r + 2] = fwd[r];
        rows[4 * r + 3] = pos[r];
    }
}

void normalize3(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int c = 0; c < 3; ++c) v[c] /= l;
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    if (!selfTest) {
        std::printf("usage: view_source_test --self-test\n");
        return 2;
    }
    using namespace edvr::view;
    using namespace edvr::kept;

    // ---- The source choice: explorer cam first, then the vscreen on foot, then the eye rows; no rows, the origin.
    check(chooseSource(true, false, true) == Source::kExplorerCam, "explorer cam on, rows ok: explorer cam");
    check(chooseSource(true, true, true) == Source::kExplorerCam, "explorer cam wins over on foot");
    check(chooseSource(false, true, true) == Source::kVscreen, "on foot with rows: the vscreen label (reserved)");
    check(chooseSource(false, false, true) == Source::kEyeRows, "seated with rows: the eye rows");
    check(chooseSource(false, false, false) == Source::kOrigin, "seated with no rows: the origin fallback");
    check(chooseSource(false, true, false) == Source::kOrigin, "on foot with no rows: the origin fallback");
    check(chooseSource(true, false, false) == Source::kOrigin, "explorer cam with no rows: the origin fallback");
    check(std::strcmp(sourceName(Source::kExplorerCam), "explorer cam") == 0 && std::strcmp(sourceName(Source::kEyeRows), "eye rows") == 0 &&
              std::strcmp(sourceName(Source::kVscreen), "vscreen") == 0 && std::strcmp(sourceName(Source::kOrigin), "origin") == 0,
          "the four source names are the log's words");

    // ---- The pose from the view rows [R | c]: the position is the translation column, the forward is the third axis (column 2).
    {
        Pose p;
        const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        check(poseFromRows(id, &p) && close(p.pos[0], 0, 1e-5f) && close(p.pos[1], 0, 1e-5f) && close(p.pos[2], 0, 1e-5f) &&
                  close(p.fwd[2], 1, 1e-5f) && close(p.fwd[0], 0, 1e-5f),
              "identity rows: the camera at the origin looking down +Z");
        const float literal[12] = {1, 0, 0, 10, 0, 1, 0, 2, 0, 0, 1, -5};
        check(poseFromRows(literal, &p) && close(p.pos[0], 10, 1e-4f) && close(p.pos[1], 2, 1e-4f) && close(p.pos[2], -5, 1e-4f),
              "the translation column is the camera's place: (10, 2, -5)");
        const float o[3] = {10.0f, 2.0f, -5.0f}, ri[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fw[3] = {0, 0, 1};
        float rows[12];
        rowsFor(o, ri, up, fw, rows);
        check(poseFromRows(rows, &p) && close(p.pos[0], 10, 1e-4f) && close(p.pos[1], 2, 1e-4f) && close(p.pos[2], -5, 1e-4f),
              "rows built from a camera at (10, 2, -5) give that position back");
        // Turned to face +X: right = -Z, up = +Y, forward = +X; standing at (5, 0, 0).
        const float yawPos[3] = {5, 0, 0}, yawRight[3] = {0, 0, -1}, yawUp[3] = {0, 1, 0}, yawFwd[3] = {1, 0, 0};
        rowsFor(yawPos, yawRight, yawUp, yawFwd, rows);
        check(poseFromRows(rows, &p) && close(p.pos[0], 5, 1e-4f) && close(p.pos[2], 0, 1e-4f) && close(p.fwd[0], 1, 1e-5f) &&
                  close(p.fwd[2], 0, 1e-5f),
              "a camera at (5,0,0) turned to face +X: position and forward");
        // An arbitrary orientation and position: the forward must come back as the one given.
        float f[3] = {1.0f, 2.0f, 2.0f};
        normalize3(f);
        const float hint[3] = {0, 1, 0};
        float r[3];
        cross3(hint, f, r);
        normalize3(r);
        float u[3];
        cross3(f, r, u);
        normalize3(u);
        const float cam[3] = {-12.5f, 3.0f, 40.0f};
        rowsFor(cam, r, u, f, rows);
        check(poseFromRows(rows, &p) && close(p.pos[0], cam[0], 1e-3f) && close(p.pos[1], cam[1], 1e-3f) &&
                  close(p.pos[2], cam[2], 1e-3f),
              "an arbitrary orientation: the position is recovered");
        check(close(p.fwd[0], f[0], 1e-4f) && close(p.fwd[1], f[1], 1e-4f) && close(p.fwd[2], f[2], 1e-4f),
              "an arbitrary orientation: the forward (column 2) is recovered");
    }

    // ---- The refusals: a scaled axis, non-orthogonal axes, a NaN, a huge position; the output is left as it was.
    {
        Pose p;
        p.pos[0] = 777.0f;
        const float scaled[12] = {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        check(!poseFromRows(scaled, &p) && p.pos[0] == 777.0f, "an axis of length 2 is refused and the output untouched");
        // Column 1 = (0.8, 0.6, 0) and column 2 = (0, 0.6, 0.8): both unit, dot 0.36.
        const float skew[12] = {1, 0.8f, 0, 0, 0, 0.6f, 0.6f, 0, 0, 0, 0.8f, 0};
        check(!poseFromRows(skew, &p), "axes 1 and 2 with a dot product of 0.36 are not orthogonal and are refused");
        float nan[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        nan[7] = std::nanf("");
        check(!poseFromRows(nan, &p), "a NaN in the rows is refused");
        const float huge[12] = {1, 0, 0, 1.0e7f, 0, 1, 0, 0, 0, 0, 1, 0};
        check(!poseFromRows(huge, &p) && p.pos[0] == 777.0f, "a position beyond a million metres is refused");
    }

    // ---- Classification: own first, then the nearest ancestor, else unjudged.
    check(classify(true, 0) == Why::kOwn, "own sphere meets: own");
    check(classify(true, 3) == Why::kOwn, "own takes precedence over an ancestor");
    check(classify(false, 2) == Why::kBase, "no own, ancestor two levels up meets: base");
    check(classify(false, 0) == Why::kUnjudged, "no own and no ancestor: unjudged");
    check(std::strcmp(whyName(Why::kBase), "base") == 0 && std::strcmp(whyName(Why::kUnjudged), "unjudged") == 0,
          "the reason names are the CSV's words");

    // ---- Top by radius: sixty kept records of radius equal to their index, one not kept; the forty largest, best first.
    {
        Entry e[60];
        for (uint32_t i = 0; i < 60; ++i) {
            e[i].address = 0x1000 + i;
            e[i].radius = static_cast<float>(i);
            e[i].kept = i != 10;
            e[i].childCount = i % 3;
        }
        uint32_t out[kTop];
        const uint32_t n = topByRadius(e, 60, out);
        check(n == 40, "the radius list holds forty records");
        check(n == 40 && out[0] == 59 && out[39] == 20, "the forty largest kept radii, largest first (59 down to 20)");
        bool sorted = true;
        for (uint32_t k = 1; k < n; ++k)
            if (e[out[k - 1]].radius < e[out[k]].radius) sorted = false;
        check(sorted, "the radius list is in descending order");
    }

    // ---- Ties on radius list by address, ascending.
    {
        Entry e[3];
        e[0].address = 0x30;
        e[1].address = 0x10;
        e[2].address = 0x20;
        for (Entry& x : e) {
            x.radius = 5.0f;
            x.kept = true;
        }
        uint32_t out[kTop];
        const uint32_t n = topByRadius(e, 3, out);
        check(n == 3 && e[out[0]].address == 0x10 && e[out[1]].address == 0x20 && e[out[2]].address == 0x30,
              "equal radii list by address, smallest first");
    }

    // ---- Top bases by child count: only records with children and kept; children first, then radius, then address.
    {
        Entry e[5];
        e[0] = Entry{};
        e[0].address = 0xA;
        e[0].radius = 1.0f;
        e[0].childCount = 5;
        e[0].kept = true;
        e[1] = Entry{};
        e[1].address = 0xB;
        e[1].radius = 2.0f;
        e[1].childCount = 9;
        e[1].kept = true;
        e[2] = Entry{};
        e[2].address = 0xC;
        e[2].radius = 99.0f;
        e[2].childCount = 0;
        e[2].kept = true;
        e[3] = Entry{};
        e[3].address = 0xD;
        e[3].radius = 7.0f;
        e[3].childCount = 9;
        e[3].kept = true;
        e[4] = Entry{};
        e[4].address = 0x9;
        e[4].radius = 1.0f;
        e[4].childCount = 5;
        e[4].kept = true;
        uint32_t out[kTop];
        const uint32_t n = topBases(e, 5, out);
        check(n == 4, "a record with no children is not a base");
        check(n == 4 && e[out[0]].address == 0xD && e[out[1]].address == 0xB, "the most children first, then the larger radius");
        check(n == 4 && e[out[2]].address == 0x9 && e[out[3]].address == 0xA, "equal children and radius: the smaller address first");
    }

    // ---- Counts: own, base and unjudged kept records; the children kept only because their base was.
    {
        Entry e[6];
        e[0] = Entry{};
        e[0].kept = true;
        e[0].why = Why::kOwn;
        e[0].judged = true;   // a root kept by its own sphere
        e[1] = Entry{};
        e[1].kept = true;
        e[1].why = Why::kBase;
        e[1].parent = 0xA;
        e[1].levelsUp = 1;    // a child kept by its base
        e[2] = Entry{};
        e[2].kept = true;
        e[2].why = Why::kOwn;
        e[2].parent = 0xA;
        e[2].judged = true;   // a child kept by its own sphere
        e[3] = Entry{};
        e[3].kept = true;
        e[3].why = Why::kUnjudged;
        e[3].parent = 0xA;    // a child kept by nothing that qualifies
        e[4] = Entry{};
        e[4].kept = false;
        e[4].why = Why::kBase;
        e[4].parent = 0xA;    // a child not kept: not counted
        e[5] = Entry{};
        e[5].kept = true;
        e[5].why = Why::kBase;
        e[5].levelsUp = 2;    // a root kept by an ancestor's sphere
        const Stats s = countKept(e, 6);
        check(s.nodes == 6 && s.kept == 5, "five of the six records are kept");
        check(s.keptOwn == 2 && s.keptBase == 2 && s.keptUnjudged == 1, "kept by own two, by base two, unjudged one");
        check(s.judgedKept == 2, "two of the kept records were judged by the observer");
        check(s.children == 4 && s.keptChildren == 3, "four children, three of them kept");
        check(s.keptChildrenByBase == 1, "one kept child is there only because its base was");
    }

    // ---- The CSV: the header and every row have the same seventeen fields; the columns sit where the report says.
    {
        const std::string header = csvHeader();
        check(fieldCount(header) == kColumns, "the header has seventeen columns");
        Entry e;
        e.address = 0x1234;
        e.radius = 12.5f;
        e.rel[0] = 1.0f;
        e.rel[1] = -2.0f;
        e.rel[2] = 3.0f;
        e.dist = 3.74f;
        e.contains = false;
        e.why = Why::kBase;
        e.levelsUp = 1;
        e.judged = false;
        e.parent = 0x5678;
        e.base = 0x9ABC;
        e.childCount = 7;
        e.bucket = 0xDEF0;
        e.vtableRva = 0x2000;
        char buf[400];
        const int n = formatRow(buf, sizeof(buf), "base", 3, e);
        const std::string row(buf, n > 0 ? static_cast<size_t>(n) : 0);
        check(n > 0 && fieldCount(row) == kColumns, "a row has seventeen fields");
        check(fieldOf(row, 1) == "base" && fieldOf(row, 2) == "3" && fieldOf(row, 3) == "0x1234", "list, rank and address");
        check(fieldOf(row, 10) == "base" && fieldOf(row, 11) == "1" && fieldOf(row, 12) == "0", "why kept, levels up and judged");
        check(fieldOf(row, 13) == "0x5678" && fieldOf(row, 14) == "0x9ABC" && fieldOf(row, 15) == "7" &&
                  fieldOf(row, 16) == "0xDEF0" && fieldOf(row, 17) == "0x2000",
              "parent, base, child count, bucket and vtable RVA sit in the last five columns");
    }

    std::printf("view source test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
