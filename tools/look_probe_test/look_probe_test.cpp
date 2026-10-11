// The look probe's pure half (src\d3d11\look_probe_core.h, TEMPORARY): the ray test against hand-worked spheres, the forward
// direction recovered from a frustum under both plane conventions and at an arbitrary rotation, the refusal cases, and the
// CSV rows (every row has the header's 24 fields; a record row leaves the draw columns empty and a draw row the record
// columns). No game, no DLL.
#include "../../src/d3d11/look_probe_core.h"

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

void normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
}

void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A frustum with its eye at e looking along f (unit): the near plane at 0.1 m, the far plane at 1000 m, and four lateral
// planes through the eye. flipped negates every plane, which is the same frustum with the normals pointing out.
void buildFrustum(const float e[3], const float f[3], bool flipped, edvr::look::Plane out[6]) {
    float ref[3] = {0.0f, 0.0f, 1.0f};
    if (std::fabs(f[2]) > 0.9f) {
        ref[0] = 0.0f;
        ref[1] = 1.0f;
        ref[2] = 0.0f;
    }
    float r[3], u[3];
    cross(f, ref, r);
    normalize(r);
    cross(r, f, u);
    normalize(u);
    auto add = [&](int i, const float n0[3], const float p[3]) {
        float n[3] = {n0[0], n0[1], n0[2]};
        float d = -dot(n, p);
        if (flipped) {
            n[0] = -n[0];
            n[1] = -n[1];
            n[2] = -n[2];
            d = -d;
        }
        out[i].n[0] = n[0];
        out[i].n[1] = n[1];
        out[i].n[2] = n[2];
        out[i].d = d;
    };
    const float nearP[3] = {e[0] + 0.1f * f[0], e[1] + 0.1f * f[1], e[2] + 0.1f * f[2]};
    const float farP[3] = {e[0] + 1000.0f * f[0], e[1] + 1000.0f * f[1], e[2] + 1000.0f * f[2]};
    const float negF[3] = {-f[0], -f[1], -f[2]};
    add(0, f, nearP);
    add(1, negF, farP);
    const float sides[4][3] = {{0.7f * f[0] + r[0], 0.7f * f[1] + r[1], 0.7f * f[2] + r[2]},
                               {0.7f * f[0] - r[0], 0.7f * f[1] - r[1], 0.7f * f[2] - r[2]},
                               {0.7f * f[0] + u[0], 0.7f * f[1] + u[1], 0.7f * f[2] + u[2]},
                               {0.7f * f[0] - u[0], 0.7f * f[1] - u[1], 0.7f * f[2] - u[2]}};
    for (int k = 0; k < 4; ++k) {
        float n[3] = {sides[k][0], sides[k][1], sides[k][2]};
        normalize(n);
        add(2 + k, n, e);
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    if (!selfTest) {
        std::printf("usage: look_probe_test --self-test\n");
        return 2;
    }
    using namespace edvr::look;

    // The ray test.
    {
        const float o[3] = {0, 0, 0}, f[3] = {0, 0, 1};
        float t = -1.0f;
        const float c[3] = {0, 0, 10};
        check(rayHitsSphere(o, f, c, 1.0f, kRangeM, &t) && close(t, 9.0f, 1e-4f), "ray: sphere ahead, entry 9");
        const float off[3] = {5, 0, 10};
        check(!rayHitsSphere(o, f, off, 1.0f, kRangeM, nullptr), "ray: sphere 5 m off the axis is missed");
        const float behind[3] = {0, 0, -10};
        check(!rayHitsSphere(o, f, behind, 1.0f, kRangeM, nullptr), "ray: sphere behind the origin is not hit");
        const float inside[3] = {0, 0, 0.5f};
        check(rayHitsSphere(o, f, inside, 1.0f, kRangeM, &t) && t == 0.0f, "ray: origin inside the sphere, entry 0");
        const float far[3] = {0, 0, 3000};
        check(!rayHitsSphere(o, f, far, 10.0f, kRangeM, nullptr), "ray: sphere past the 2 km range is not listed");
        check(rayHitsSphere(o, f, far, 10.0f, 5000.0f, nullptr), "ray: the same sphere with a longer range is hit");
        const float tangent[3] = {1, 0, 10};
        check(rayHitsSphere(o, f, tangent, 1.0f, kRangeM, &t) && close(t, 10.0f, 1e-3f), "ray: tangent contact at 10 m");
    }

    // The forward direction, from a frustum with the normals in either convention.
    {
        const float e[3] = {0, 0, 0}, f[3] = {0, 0, 1};
        Plane pl[6];
        float out[3] = {};
        float nearM = 0, farM = 0;
        buildFrustum(e, f, false, pl);
        check(forwardFromPlanes(pl, 6, e, out, &nearM, &farM) && close(out[0], 0, 1e-4f) && close(out[1], 0, 1e-4f) &&
                  close(out[2], 1, 1e-4f),
              "frustum: forward +z with inward normals");
        check(close(nearM, 0.1f, 1e-3f) && close(farM, 1000.0f, 1e-2f), "frustum: near 0.1 m and far 1000 m from the eye");
        buildFrustum(e, f, true, pl);
        check(forwardFromPlanes(pl, 6, e, out) && close(out[2], 1, 1e-4f) && close(out[0], 0, 1e-4f),
              "frustum: forward +z with outward normals (the same answer)");
        const float e2[3] = {5, -3, 2};
        float g[3] = {0.3f, -0.5f, 0.8f};
        normalize(g);
        buildFrustum(e2, g, false, pl);
        check(forwardFromPlanes(pl, 6, e2, out, &nearM, &farM) && close(out[0], g[0], 1e-3f) &&
                  close(out[1], g[1], 1e-3f) && close(out[2], g[2], 1e-3f),
              "frustum: rotated forward recovered, inward normals");
        buildFrustum(e2, g, true, pl);
        check(forwardFromPlanes(pl, 6, e2, out) && close(out[0], g[0], 1e-3f) && close(out[1], g[1], 1e-3f) &&
                  close(out[2], g[2], 1e-3f),
              "frustum: rotated forward recovered, outward normals");
        const Plane sides[4] = {pl[2], pl[3], pl[4], pl[5]};
        check(!forwardFromPlanes(sides, 4, e2, out), "frustum: four lateral planes alone give no forward");
        Plane parallel[2];
        parallel[0] = {{0, 0, 1}, -1.0f};    // z > 1
        parallel[1] = {{0, 0, -1}, 20.0f};   // z < 20: the eye at z = 10 is on the inner side of both
        check(!forwardFromPlanes(parallel, 2, e2, out), "frustum: an eye on the inner side of both planes is refused");
    }

    // The CSV rows and the header: 24 fields each, empty where the other kind's columns sit.
    {
        const std::string header = csvHeader();
        check(fieldCount(header) == kColumns, "csv: the header has 24 columns");
        RecordRow rr;
        rr.index = 3;
        rr.address = 0x1234;
        rr.radius = 12.5f;
        rr.t = 99.25f;
        rr.dist = 150.0f;
        rr.bucket = 0xABC;
        rr.bucketBase = 0xDEF;
        rr.role = "child";
        rr.childCount = 4;
        rr.vtableRva = 0x2000;
        rr.nodeVtableRva = 0x3000;
        char buf[512];
        const int n = formatRecordRow(buf, sizeof(buf), rr);
        const std::string rec(buf, n > 0 ? static_cast<size_t>(n) : 0);
        check(n > 0 && fieldCount(rec) == kColumns, "csv: a record row has 24 fields");
        check(fieldOf(rec, 1) == "record" && fieldOf(rec, 2) == "3" && fieldOf(rec, 3) == "0x1234",
              "csv: record row kind, index and address");
        check(fieldOf(rec, 9) == "child" && fieldOf(rec, 10) == "4" && fieldOf(rec, 8) == "0xDEF",
              "csv: record row role, child count and bucket base");
        bool recordEmpty = true;
        for (int c = 13; c <= kColumns; ++c)
            if (!fieldOf(rec, c).empty()) recordEmpty = false;
        check(recordEmpty, "csv: a record row leaves the draw columns (13 to 24) empty");

        DrawRow dr;
        dr.index = 7;
        dr.vs = 0x1111;
        dr.ps = 0x2222;
        dr.drawKind = 'I';
        dr.count = 36;
        dr.instances = 1;
        dr.blend = "en1:src5:dst6:op1:wm15:a2c0";
        dr.depth = "dsen1:fn2:wm1:sten0";
        dr.rt = 0x9999;
        dr.rtW = 1920;
        dr.rtH = 2016;
        dr.centreSamples = 42;
        dr.covers = true;
        const int m = formatDrawRow(buf, sizeof(buf), dr);
        const std::string drw(buf, m > 0 ? static_cast<size_t>(m) : 0);
        check(m > 0 && fieldCount(drw) == kColumns, "csv: a draw row has 24 fields");
        check(fieldOf(drw, 1) == "draw" && fieldOf(drw, 2) == "7", "csv: draw row kind and issue order");
        bool drawGap = true;
        for (int c = 3; c <= 12; ++c)
            if (!fieldOf(drw, c).empty()) drawGap = false;
        check(drawGap, "csv: a draw row leaves the record columns (3 to 12) empty");
        check(fieldOf(drw, 13) == "0x0000000000001111" && fieldOf(drw, 14) == "0x0000000000002222",
              "csv: draw row vs and ps");
        check(fieldOf(drw, 15) == "I" && fieldOf(drw, 16) == "36" && fieldOf(drw, 17) == "1",
              "csv: draw row kind, count and instances");
        check(fieldOf(drw, 18) == "en1:src5:dst6:op1:wm15:a2c0" && fieldOf(drw, 19) == "dsen1:fn2:wm1:sten0",
              "csv: draw row blend and depth text");
        check(fieldOf(drw, 21) == "1920" && fieldOf(drw, 22) == "2016" && fieldOf(drw, 23) == "42" &&
                  fieldOf(drw, 24) == "1",
              "csv: draw row render target size, centre samples and covers flag");
    }

    // The summary line: a known forward prints the vector, an unknown one says so.
    {
        Summary s;
        s.frame = 812;
        s.trigger = "hotkey.look_probe";
        s.forwardKnown = true;
        s.forward[2] = 1.0f;
        s.eye[0] = 1.5f;
        s.recordsScanned = 900;
        s.recordsInRay = 12;
        s.recordsListed = 12;
        s.draws = 2100;
        s.drawsProbed = 2048;
        s.drawsCovering = 9;
        s.drawsListed = 9;
        s.csvName = "look_probe_x.csv";
        char line[640];
        const int n = formatSummary(line, sizeof(line), s);
        const std::string text(line, n > 0 ? static_cast<size_t>(n) : 0);
        check(text.find("forward=(0.0000,0.0000,1.0000)") != std::string::npos, "summary: known forward printed");
        check(text.find("covering the centre=9 listed=9 csv=look_probe_x.csv") != std::string::npos,
              "summary: draw counts and the csv name");
        s.forwardKnown = false;
        const int k = formatSummary(line, sizeof(line), s);
        const std::string unknown(line, k > 0 ? static_cast<size_t>(k) : 0);
        check(unknown.find("forward=unknown") != std::string::npos, "summary: unknown forward says so");
    }

    std::printf("look probe test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
