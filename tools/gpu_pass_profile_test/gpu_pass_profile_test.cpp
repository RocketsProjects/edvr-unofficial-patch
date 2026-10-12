// The GPU pass profiler's pure logic (src\d3d11\gpu_pass_profile_core.h): classification, cube runs, the stamp budget, the
// shader tally, the summary arithmetic and the CSV text. No D3D11: the device path is exercised in the DLL's own run.
// --self-test exits nonzero on the first failed check.
#include "../../src/d3d11/gpu_pass_profile_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace edvr::gpupass;

namespace {
unsigned g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

TargetShape shape(uint32_t w, uint32_t h, bool cube = false, uint32_t slices = 1) {
    TargetShape t;
    t.w = w;
    t.h = h;
    t.cube = cube;
    t.slices = slices;
    return t;
}

// Commas outside quotes: a CSV row must have exactly the header's field count.
size_t fieldCount(const std::string& line) {
    size_t n = 1;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') quoted = !quoted;
        else if (c == ',' && !quoted) ++n;
    }
    return n;
}

void testClassify() {
    const EyeSize eye{2016, 1949};
    PassFacts f;
    f.edvr = true;
    f.draws = 5;
    check(classifyPass(f, eye) == PassClass::Edvr, "an EDVR segment is edvr, whatever it drew");

    PassFacts c;
    c.dispatches = 3;
    check(classifyPass(c, eye) == PassClass::Compute, "dispatches without draws are compute");

    PassFacts k;
    k.clears = 1;
    check(classifyPass(k, eye) == PassClass::Clear, "a clear with no draws is clear");
    PassFacts e;
    check(classifyPass(e, eye) == PassClass::Other, "a segment with no work is other");

    PassFacts shadow;
    shadow.draws = 40;
    shadow.hasDepth = true;
    shadow.depth = shape(2048, 2048);
    check(classifyPass(shadow, eye) == PassClass::Shadow, "depth-only square 2048 is a shadow map");
    shadow.depth = shape(256, 256);
    check(classifyPass(shadow, eye) == PassClass::Shadow, "a small square depth-only map off the eye size is a shadow (v13.7: 256x256 D16)");
    shadow.depth = shape(2048, 2048, false, 4);
    check(classifyPass(shadow, eye) == PassClass::Shadow, "an array of depth slices is a shadow (cascades)");

    PassFacts prepass;
    prepass.draws = 300;
    prepass.hasDepth = true;
    prepass.depth = shape(2016, 1949);
    check(classifyPass(prepass, eye) == PassClass::SceneGBuffer, "a depth prepass at the eye size is scene geometry");

    PassFacts face;
    face.draws = 10;
    face.colours = 1;
    face.colour = shape(512, 512, true, 1);
    check(classifyPass(face, eye) == PassClass::ProbeCube, "a square target of a cube resource is a probe");

    PassFacts gbuf;
    gbuf.draws = 800;
    gbuf.colours = 3;
    gbuf.colour = shape(2016, 1949);
    check(classifyPass(gbuf, eye) == PassClass::SceneGBuffer, "three colour targets at the eye size are the g-buffer");

    PassFacts full;
    full.draws = 2;
    full.colours = 1;
    full.colour = shape(2016, 1949);
    check(classifyPass(full, eye) == PassClass::FullscreenAtEye, "one or two draws at the eye size are full-screen");

    PassFacts fwd;
    fwd.draws = 90;
    fwd.colours = 1;
    fwd.colour = shape(2016, 1949);
    check(classifyPass(fwd, eye) == PassClass::ForwardAtEye, "many draws into one eye-sized target are forward");

    PassFacts post;
    post.draws = 3;
    post.colours = 1;
    post.colour = shape(1008, 974);
    check(classifyPass(post, eye) == PassClass::PostSmaller, "draws into a smaller target are post");

    PassFacts big;
    big.draws = 3;
    big.colours = 1;
    big.colour = shape(4000, 4000);
    check(classifyPass(big, eye) == PassClass::Other, "a larger square target that is not a cube is other");

    PassFacts noEye;
    noEye.draws = 3;
    noEye.colours = 1;
    noEye.colour = shape(1008, 974);
    check(classifyPass(noEye, EyeSize{}) == PassClass::Other, "with no eye size known, nothing is post or eye");
}

void testCubeRuns() {
    auto square = [](uint32_t s) {
        PassFacts f;
        f.draws = 4;
        f.colours = 1;
        f.colour = shape(s, s);
        return f;
    };
    const EyeSize eye{2016, 1949};
    std::vector<PassFacts> six(6, square(256));
    std::vector<PassClass> cls;
    for (const PassFacts& f : six) cls.push_back(classifyPass(f, eye));
    markCubeRuns(six, cls);
    bool allCube = true;
    for (PassClass c : cls) allCube = allCube && c == PassClass::ProbeCube;
    check(allCube, "six consecutive same-size square passes read as one cube map");

    std::vector<PassFacts> five(5, square(256));
    cls.assign(5, PassClass::Other);
    markCubeRuns(five, cls);
    bool anyCube = false;
    for (PassClass c : cls) anyCube = anyCube || c == PassClass::ProbeCube;
    check(!anyCube, "five in a row is not a cube map");

    std::vector<PassFacts> broken;
    for (int i = 0; i < 3; ++i) broken.push_back(square(256));
    broken.push_back(square(128));   // a different size ends the run
    for (int i = 0; i < 6; ++i) broken.push_back(square(256));
    cls.assign(broken.size(), PassClass::Other);
    markCubeRuns(broken, cls);
    check(cls[0] == PassClass::Other && cls[3] == PassClass::Other, "a size change breaks the run");
    check(cls[4] == PassClass::ProbeCube && cls[9] == PassClass::ProbeCube, "a later run of six is found");

    PassFacts edvrFace = square(256);
    edvrFace.edvr = true;
    std::vector<PassFacts> withEdvr(6, square(256));
    withEdvr[2] = edvrFace;
    cls.assign(6, PassClass::Other);
    markCubeRuns(withEdvr, cls);
    check(cls[0] == PassClass::Other, "an EDVR segment breaks a cube run");
}

void testStampBudget() {
    StampBudget b;
    uint32_t boundaries = 0;
    while (b.take(1)) ++boundaries;
    check(boundaries == kMaxStampsPerFrame - 1, "a boundary leaves one stamp free for the frame's end");
    check(b.overflow == 1, "the first refused boundary is counted");
    check(b.take(0), "the frame's end stamp fits in the reserved slot");
    check(b.used == kMaxStampsPerFrame, "the frame holds exactly its cap");
    check(!b.take(0) && b.overflow == 2, "past the cap nothing is issued and everything is counted");
}

void testHashes() {
    PassHashes h;
    for (uint64_t i = 1; i <= 10; ++i) h.note(i, 100 + i);
    check(h.folded() == 2, "pairs past the eight slots fold into one count");
    for (int i = 0; i < 4; ++i) h.note(3, 103);
    const std::vector<HashTally> top = h.top();
    check(top.size() == kTopHashes, "the top list is capped at three");
    check(top[0].vs == 3 && top[0].draws == 5, "the most-drawn pair leads, with its count");
    check(top[1].draws == 1, "the rest follow in draw order");
    check(topText(std::vector<HashTally>{}) == "-", "a pass that drew nothing reads '-'");
    check(topText(std::vector<HashTally>{HashTally{1, 2, 5}}) ==
              "0000000000000001:0000000000000002:5",
          "a pair is written as vs:ps:draws in hex");
}

void testCsv() {
    check(csvField("plain") == "plain", "a plain field is not quoted");
    check(csvField("1,2") == "\"1,2\"", "a field with a comma is quoted");
    check(csvField("say \"hi\"") == "\"say \"\"hi\"\"\"", "quotes inside a field are doubled");

    Row r;
    r.frame = 2;
    r.index = 7;
    r.cls = PassClass::ForwardAtEye;
    r.ms = 1.23456;
    r.draws = 90;
    r.colours = 1;
    r.rt = "2016x1949";
    r.dsv = "2016x1949";
    r.top = "a:b:1;c:d:2";
    const std::string line = csvRow(r);
    check(fieldCount(line) == fieldCount(csvHeader()), "a row has as many fields as the header");
    check(line.rfind("2,7,forward_at_eye,0,1.2346,90,", 0) == 0, "the row starts with frame, pass, class, edvr, ms");
    Row odd;
    odd.rt = "a,b";
    check(fieldCount(csvRow(odd)) == fieldCount(csvHeader()), "a comma in a target name is quoted, not a new field");
}

void testSummary() {
    // Frame 0 and frame 1 read; frame 2 not read, and its rows must not count.
    std::vector<Row> rows;
    auto add = [&](uint32_t frame, PassClass c, bool edvr, double ms, const char* rt) {
        Row r;
        r.frame = frame;
        r.cls = c;
        r.edvr = edvr;
        r.ms = ms;
        r.draws = 10;
        r.rt = rt;
        rows.push_back(r);
    };
    add(0, PassClass::Shadow, false, 6.0, "2048x2048");
    add(0, PassClass::ForwardAtEye, false, 2.0, "2016x1949");
    add(0, PassClass::Edvr, true, 2.0, "-");
    add(1, PassClass::Shadow, false, 10.0, "2048x2048");
    add(1, PassClass::ForwardAtEye, false, 6.0, "2016x1949");
    add(1, PassClass::Edvr, true, 4.0, "-");
    add(2, PassClass::Shadow, false, 500.0, "2048x2048");

    const std::vector<double> frameMs{10.0, 20.0, 999.0};
    const std::vector<bool> ok{true, true, false};
    const Summary s = summarise(rows, frameMs, ok);
    check(s.frames == 2, "only the frames that were read are counted");
    check(s.frameMs == 15.0, "the frame mean is over the read frames");
    check(s.eliteMs == 12.0, "elite is the mean of the non-EDVR rows per frame");
    check(s.edvrMs == 3.0, "edvr is the mean of the EDVR rows per frame");
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].ms == 8.0, "a class mean is per frame");
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].passes == 1.0, "a class pass count is per frame");
    check(!s.top.empty() && s.top[0].cls == PassClass::Shadow && s.top[0].ms == 8.0,
          "the top pass is the largest group by mean ms per frame");
    check(s.top.size() == 3, "groups are the distinct class and target signatures");

    const Summary none = summarise(rows, frameMs, std::vector<bool>{false, false, false});
    check(none.frames == 0 && none.top.empty(), "with no frame read there are no means at all");
}

// Built v13.7: the HUD class, the depth-only shadow rule, the compute column and the medians.
void testV137() {
    const EyeSize eye{2016, 1949};
    const uint64_t kHudVs = 0xB7790CBFC6554097ull, kOtherVs = 0x1111111111111111ull;
    HudFamilies fam;
    fam.vs[0] = kHudVs;
    fam.n = 1;
    PassFacts hud;
    hud.colours = 1;
    hud.colour = shape(eye.w, eye.h);
    hud.draws = 2;
    hud.topVs = kHudVs;
    check(classifyPass(hud, eye, fam) == PassClass::HudOverlay, "two draws of a known HUD VS at the eye size are hud_overlay, not fullscreen_at_eye");
    check(classifyPass(hud, eye) == PassClass::FullscreenAtEye, "with no families given, the same pass is fullscreen_at_eye (the families are the caller's)");
    PassFacts many = hud;
    many.draws = kHudMaxDraws + 1;
    check(classifyPass(many, eye, fam) == PassClass::ForwardAtEye, "more draws than kHudMaxDraws is not a HUD overlay");
    PassFacts notHud = hud;
    notHud.topVs = kOtherVs;
    check(classifyPass(notHud, eye, fam) == PassClass::FullscreenAtEye, "a pass whose top VS is not a known family is not a HUD overlay");
    PassFacts unknownTop = hud;
    unknownTop.topVs = 0;
    check(classifyPass(unknownTop, eye, fam) == PassClass::FullscreenAtEye, "a pass with no drawn VS is not a HUD overlay");
    check(std::string(passClassName(PassClass::HudOverlay)) == "hud_overlay", "the class is named hud_overlay in the CSV and the summary");

    // The depth-only rule: a map off the eye size is a shadow (non-square 2048x1024 D16, small 256x256 D16); a map at the eye size is a G-buffer.
    PassFacts d;
    d.draws = 40;
    d.hasDepth = true;
    d.depth = shape(2048, 1024);
    check(classifyPass(d, eye) == PassClass::Shadow, "the non-square 2048x1024 depth-only pair is a shadow map");
    d.depth = shape(256, 256);
    check(classifyPass(d, eye) == PassClass::Shadow, "the 256x256 depth-only map is a shadow map");
    d.depth = shape(eye.w, eye.h);
    check(classifyPass(d, eye) == PassClass::SceneGBuffer, "a depth-only map at the eye size is still scene_gbuffer");
    PassFacts dn;
    dn.draws = 4;
    dn.hasDepth = true;
    dn.depth = shape(256, 256);
    check(classifyPass(dn, EyeSize{}) == PassClass::Other, "with no eye size known, a small square depth-only map is not claimed as a shadow");

    // The compute column: the bound compute shaders by hash, named from the catalogue when known.
    const std::vector<HashTally> cs = {HashTally{0x6FE04AF836BB1DBAull, 0, 3}, HashTally{0x0123456789ABCDEFull, 0, 1}};
    ComputeName names[] = {{0x6FE04AF836BB1DBAull, "APPLY_BIND_POSE_TRANSFORMS_CS"}};
    const std::string text = csText(cs, names, 1);
    check(text == "6fe04af836bb1dba(APPLY_BIND_POSE_TRANSFORMS_CS):3;0123456789abcdef:1",
          "compute shaders print as hash(NAME):count, most dispatches first, the unknown ones as hex only");
    check(csText({}, names, 1) == "-", "a pass with no dispatch prints a dash");
    check(std::string(csvHeader()).find("top_cs_dispatches") != std::string::npos,
          "the CSV header ends with the top_cs_dispatches column");
    Row rc;
    rc.topCs = "a(b):1";
    check(fieldCount(csvRow(rc)) == fieldCount(csvHeader()), "a row with a compute column still has the header's field count");

    // The medians: one spike moves the mean and not the median.
    std::vector<Row> rows;
    std::vector<double> frameMs = {10.0, 10.0, 10.0};
    std::vector<bool> ok = {true, true, true};
    for (uint32_t f = 0; f < 3; ++f) {
        Row r;
        r.frame = f;
        r.cls = PassClass::Shadow;
        r.ms = f == 2 ? 100.0 : 1.0;
        r.draws = 10;
        r.rt = "-";
        r.dsv = "2048x1024";
        rows.push_back(r);
    }
    const Summary s = summarise(rows, frameMs, ok);
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].median == 1.0, "the class median of one pass per frame is the middle value (1 ms), not the 34 ms mean");
    check(s.cls[static_cast<size_t>(PassClass::Shadow)].ms > 30.0, "the class mean still carries the spike");
    check(!s.top.empty() && s.top[0].median == 1.0, "the group median is printed beside its mean");
    check(medianOf({1.0, 2.0, 3.0, 4.0}) == 2.5 && medianOf({5.0}) == 5.0 && medianOf({}) == 0.0, "the median of an even, an odd and an empty list");
}

// Built v13.6: the yield follows the capture. Wanted from the press (armed), through every batch frame, and not after the batch's last frame is
// closed; never wanted idle. The application frame is refused while it is wanted, and admitted once it is not (modelled by the flag).
void testYield() {
    check(!yieldWanted(false, false), "idle: the yield is not wanted, so application frames and census timers are not paused");
    check(yieldWanted(true, false), "armed (the press, before the first boundary): the yield is wanted");
    check(yieldWanted(false, true), "a batch running: the yield is wanted through every frame of it");
    check(yieldWanted(true, true), "armed and running together: still wanted");
    // The capture's life, boundary by boundary: press, begin frames 1 to 4, close the last one; the flag is cleared at that boundary.
    bool requested = true, batch = false;
    check(yieldWanted(requested, batch), "press: wanted");
    requested = false;
    batch = true;   // boundary 1: the batch starts
    check(yieldWanted(requested, batch), "boundary 1: the batch has started, wanted");
    batch = true;   // boundaries 2 to 4: frames 2 to 4 begin
    check(yieldWanted(requested, batch), "boundaries 2 to 4: still wanted");
    batch = false;  // boundary 5: the last frame is closed at its top and the batch ends
    check(!yieldWanted(requested, batch), "after the last frame's close (boundary 5): not wanted, so the next application frame may open");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    if (!selfTest) {
        std::printf("usage: gpu_pass_profile_test --self-test\n");
        return 2;
    }
    testClassify();
    testCubeRuns();
    testStampBudget();
    testHashes();
    testCsv();
    testSummary();
    testYield();
    testV137();
    std::printf("gpu_pass_profile_test: %u checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
