// The rig for the panel tag and the boundary census (src\d3d11\ui_panel_tag.h; docs/ui-layer-2026-09-23.md, "2026-10-14: the panel data-flow
// census"), and for the observe-only probe on the engine's UI node (src\d3d11\ui_node_probe.*). OBSERVE ONLY: nothing here moves a draw.
//
//   B1..B4  the bitmask: bind, unbind (a null view), overwrite, slot ranges (a call covering slots 2..5 touches 2 and 3 and no other; a start
//           past 3 touches nothing), the eye bit beside the panel bit, and a bind with n = 0.
//   B5      re-creation: a view classified "not a panel" before its surface was learned is re-asked when the learned set changes -- all four
//           slots, including the ones the call did not cover -- exactly once; a second bind at the same generation recomputes only its own.
//   B6      the shadow's side (the production binding_shadow.cpp, linked): bindingSet on a PS SRV cell clears that slot's bits, bindingInvalidate
//           too, bindingForgetAll clears all four, and a write to any other cell leaves them alone.
//   C1..C4  the census window: scene draws between the first and last draw that samples a learned surface (per target, two eyes blocked or
//           interleaved), listed against unlisted and the unlisted pairs by (vs, ps) with a cap, the 8-bit target's a/b/c after its first
//           composite, frameEnd resetting the runs and counting live frames, the run table's overflow.
//   T1..T5  the lines: the zero line (a count that ran and found nothing), the NOT COUNTED line (the detector off), the lit-target line with
//           unlisted pairs busiest first, the cost line with and without samples (the scaling arithmetic), the verification counters.
//   N1..N3  the node probe on a synthetic module image: pinned by stamp, slot value and prologue; inert on any mismatch (one attempt, ever);
//           live it forwards to the original, counts, and its clause says where the last call fell.
//   M1      the measured cost of the tagging half and of the draw note (printed; the bounds are generous guards, not the figure).
//   R1..R3  the lit-target rule (ui_layer_math.h kPanelTag): a draw no hash names that samples a learned panel surface is a cockpit holo panel, whatever its
//           vertex shader; the hash list is still the first answer; ui_depth's exclusion wins; nothing else about the rule moved.
//   A1..A2  the once-a-session "new panel VS seen" line, and the runs table spent only on targets that have a tagged draw (the first flight filled it with the G-buffer's).
//   P1..P10 source pins from the repo root, each with a control (the same predicate over a copy with one edit must fail): the bind hook, the
//           draw hook, the draw path only reading the bit, the shadow clearing the bits, the learned set's generation, the 30 s report, the probe's
//           install, the build list, and that no decision reads the tag.
//
// Usage: --self-test [<repo root>] [--only B1,C2,...]   |   --dry-run
#include "ui_panel_tag.h"
#include "ui_layer_math.h"
#include "ui_node_probe.h"
#include "binding_shadow.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// The stub the probe's glue links against (production: ui_depth.cpp counts the draws this fills in).
namespace edvr {
uint32_t g_stubLit = 0, g_stubLdr = 0;
unsigned long g_stubThread = 0;
void uiDepthBoundaryDrawPosition(uint32_t* lit, uint32_t* ldr, unsigned long* thread) {
    if (lit) *lit = g_stubLit;
    if (ldr) *ldr = g_stubLdr;
    if (thread) *thread = g_stubThread;
}
}  // namespace edvr

namespace {
using namespace edvr;

unsigned g_checks = 0;
std::string g_root;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) throw std::runtime_error(label);
}

// ---- a model of the shadow and the tagger: views are small integers cast to pointers ---------------------------------------------
// A "view" is an id; ids >= 100 are panel surfaces once `learned` says so; ids >= 200 are eye-sized textures.
struct World {
    std::vector<uint32_t> learned;        // ids the learned set holds
    void* shadow[4] = {};
    uint32_t gen = 1;
    PanelTag tag;
    int resolves = 0;
    uint8_t classify(void* v) {
        if (!v) return 0;
        ++resolves;
        const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v));
        uint8_t f = 0;
        if (std::find(learned.begin(), learned.end(), id) != learned.end()) f |= kPanelFlagSurface;
        if (id >= 200) f |= kPanelFlagEyeSized;
        return f;
    }
    // The hook: the shadow's cells are written (clearing their bits, as binding_shadow.cpp does), then the tagger runs.
    bool bind(uint32_t start, uint32_t n, std::initializer_list<uint32_t> ids) {
        uint32_t i = 0;
        for (uint32_t id : ids) {
            const uint32_t slot = start + i++;
            if (slot < 4) {
                shadow[slot] = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                tag.clearSlot(slot);
            }
            if (i >= n) break;
        }
        return panelTagBind(tag, gen, start, n, [&](uint32_t s) { return s < 4 ? shadow[s] : nullptr; },
                            [&](void* v) { return classify(v); });
    }
    void learn(uint32_t id) { learned.push_back(id); ++gen; }
    void forgetSurface(uint32_t id) { learned.erase(std::remove(learned.begin(), learned.end(), id), learned.end()); ++gen; }
};

World fresh() {
    World w;
    w.learn(100); w.learn(101); w.learn(102);
    w.tag.gen = w.gen;   // as after a frame boundary's recompute
    return w;
}

// ---- B: the bitmask ---------------------------------------------------------------------------------------------------------------
void caseB1() {
    World w = fresh();
    w.bind(0, 1, {100});
    check(w.tag.panel == 0x1 && w.tag.eye == 0, "B1a: binding a learned panel surface at slot 0 sets bit 0 and no other");
    w.bind(2, 1, {101});
    check(w.tag.panel == 0x5, "B1b: slot 2 adds bit 2");
    w.bind(1, 1, {7});
    check(w.tag.panel == 0x5, "B1c: a scene texture at slot 1 sets nothing");
    w.bind(0, 1, {0});
    check(w.tag.panel == 0x4 && w.shadow[0] == nullptr, "B1d: unbinding slot 0 (a null view) clears bit 0 and only it");
    w.bind(2, 1, {7});
    check(w.tag.panel == 0, "B1e: overwriting the panel view at slot 2 with a scene texture clears bit 2");
    w.bind(3, 1, {100});
    w.bind(3, 1, {101});
    check(w.tag.panel == 0x8, "B1f: overwriting a panel with another panel keeps the bit");
    check(uiPanelTagMask() == g_panelTag.panel, "B1g: the draw path's one read is the tag's panel byte");
}

void caseB2() {
    World w = fresh();
    w.bind(0, 4, {100, 7, 101, 9});
    check(w.tag.panel == 0x5, "B2a: a four-slot bind tags slots 0 and 2");
    w.bind(2, 4, {7, 102, 100, 100});   // slots 2,3 only fall in range; the ids for slots 4,5 are ignored
    check(w.tag.panel == 0x9 && w.shadow[2] == reinterpret_cast<void*>(7) && w.shadow[3] == reinterpret_cast<void*>(102),
          "B2b: a call covering slots 2..5 touches slots 2 and 3 and no other: slot 2 cleared, slot 3 set, slot 0 untouched");
    const int before = w.resolves;
    w.bind(4, 2, {100, 100});
    check(w.tag.panel == 0x9 && w.resolves == before, "B2c: a call that starts past slot 3 touches nothing and resolves nothing");
    w.bind(1, 0, {});
    check(w.tag.panel == 0x9, "B2d: a call with n = 0 touches nothing");
}

void caseB3() {
    World w = fresh();
    w.bind(0, 3, {100, 250, 7});
    check(w.tag.panel == 0x1 && w.tag.eye == 0x2, "B3a: the eye bit is its own: an eye-sized texture at slot 1 sets eye bit 1 and not the panel bit");
    w.bind(1, 1, {101});
    check(w.tag.panel == 0x3 && w.tag.eye == 0, "B3b: overwriting it with a panel surface moves slot 1 from the eye bit to the panel bit");
    w.learn(250);
    w.bind(0, 4, {250, 250, 250, 250});
    check(w.tag.panel == 0xF && w.tag.eye == 0xF, "B3c: a view can be both (an eye-sized learned surface): both bits, and the census reads the panel one first");
}

void caseB4() {
    World w = fresh();
    w.bind(0, 4, {100, 100, 100, 100});
    w.tag.forget();
    check(w.tag.panel == 0 && w.tag.eye == 0, "B4a: forget clears every bit");
    w.tag.setSlot(9, kPanelFlagSurface);
    w.tag.clearSlot(9);
    check(w.tag.panel == 0, "B4b: a slot past 3 is ignored by set and clear");
}

void caseB5() {
    World w = fresh();
    w.bind(0, 2, {300, 100});   // 300 is the panel surface the game has just re-created: not learned yet
    check(w.tag.panel == 0x2, "B5a: a view over a surface not yet learned is not a panel");
    const bool again = w.bind(3, 1, {7});
    check(!again && w.tag.panel == 0x2, "B5b: a bind at the same generation recomputes its own slot only; slot 0 stays 'not a panel'");
    w.learn(300);
    const int before = w.resolves;
    const bool retag = w.bind(3, 1, {7});
    check(retag && w.tag.panel == 0x3, "B5c: the learned set changed: the next bind recomputes ALL four slots -- slot 0, which the call did not cover, becomes a panel");
    check(w.resolves - before == 3, "B5d: ...asking each bound view once (slots 0, 1 and 3; slot 2 is empty)");
    check(!w.bind(3, 1, {7}) && w.tag.gen == w.gen, "B5e: and exactly once: the generation is recorded");
    w.forgetSurface(300);
    check(w.bind(2, 1, {7}) && w.tag.panel == 0x2, "B5f: a surface dropped from the learned set (a recycled address) un-tags its view at the next bind");
}

void caseB6() {
    int a = 0, b = 0;
    bindingForgetAll();
    g_panelTag = PanelTag();
    g_panelTag.setSlot(0, kPanelFlagSurface);
    g_panelTag.setSlot(1, kPanelFlagSurface | kPanelFlagEyeSized);
    g_panelTag.setSlot(3, kPanelFlagSurface);
    check(g_panelTag.panel == 0xB, "B6a: (setup) three slots tagged");
    bindingSet(BindSlot::Rtv0, &a);
    bindingSet(BindSlot::Vs, &b);
    bindingSet(BindSlot::VsCb0, &b);
    check(g_panelTag.panel == 0xB, "B6b: a write to any cell that is not a PS SRV cell leaves the bits alone");
    bindingSet(BindSlot::PsSrv1, &a);
    check(g_panelTag.panel == 0x9 && g_panelTag.eye == 0, "B6c: bindingSet on PS SRV slot 1 clears that slot's panel and eye bits and no other");
    bindingInvalidate(BindSlot::PsSrv3);
    check(g_panelTag.panel == 0x1, "B6d: bindingInvalidate on PS SRV slot 3 clears its bit");
    bindingSet(BindSlot::PsSrv0, nullptr);
    check(g_panelTag.panel == 0, "B6e: an unbind recorded in the shadow clears the bit");
    g_panelTag.setSlot(2, kPanelFlagSurface);
    bindingForgetAll();
    check(g_panelTag.panel == 0 && g_panelTag.eye == 0, "B6f: bindingForgetAll (ClearState, a command list replayed) clears every bit");
    check(static_cast<uint32_t>(BindSlot::PsSrv3) - static_cast<uint32_t>(BindSlot::PsSrv0) == 3,
          "B6g: PsSrv0..3 are contiguous in the shadow's enum, which the slot arithmetic depends on");
}

// ---- C: the census window -----------------------------------------------------------------------------------------------------------
void caseC1() {
    UiBoundaryWindow w;
    int eyeA = 0, eyeB = 0;
    // scene scene TAGGED scene scene scene TAGGED scene   -> 3 between, 1 after the last (not counted)
    for (bool tagged : {false, false, true, false, false, false, true, false}) w.noteLit(&eyeA, tagged, false);
    w.frameEnd();
    check(w.litDraws == 8 && w.litTagged == 2 && w.litRuns == 1 && w.sceneBetween == 3 && w.maxBetween == 3 && w.framesLive == 1,
          "C1a: scene draws between the first and last draw that samples a learned surface are counted: 3; those before the first and after the last are not");
    // two eyes BLOCKED: A tagged, then B untagged scene, tagged -> per target
    for (bool tagged : {true, false, true}) w.noteLit(&eyeA, tagged, true);
    for (bool tagged : {false, false, false, true, false}) w.noteLit(&eyeB, tagged, true);
    w.frameEnd();
    check(w.litRuns == 3 && w.sceneBetween == 3 + 1 + 0 && w.maxBetween == 3, "C1b: each eye target keeps its own run: left 1 between, right 0 (its tagged draw is alone)");
    // INTERLEAVED: A and B draws alternate; runs stay per target
    w.noteLit(&eyeA, true, true); w.noteLit(&eyeB, false, false); w.noteLit(&eyeA, false, false); w.noteLit(&eyeB, true, true); w.noteLit(&eyeA, true, true);
    w.frameEnd();
    check(w.sceneBetween == 3 + 1 + 0 + 1 + 0, "C1c: interleaved eyes do not count each other's draws: A has 1 between, B none");
    // a frame with no tagged draw adds no run
    const uint64_t runs = w.litRuns;
    w.noteLit(&eyeA, false, false); w.noteLit(&eyeA, false, false);
    w.frameEnd();
    check(w.litRuns == runs && w.framesLive == 4, "C1d: a frame whose lit draws sampled nothing adds no run; every frame with a draw is live");
    w.frameEnd();
    check(w.framesLive == 4, "C1e: a frame with no eye draw at all is not a live frame");
}

void caseC2() {
    UiBoundaryWindow w;
    int eye = 0;
    w.noteLit(&eye, true, true);
    w.noteLit(&eye, true, false); w.nameUnlisted(0x925ACEDA0153AA5Bull, 0x1111ull);
    w.noteLit(&eye, true, false); w.nameUnlisted(0x925ACEDA0153AA5Bull, 0x1111ull);
    w.noteLit(&eye, true, false); w.nameUnlisted(0x925ACEDA0153AA5Bull, 0x2222ull);
    check(w.litTagged == 4 && w.litListed == 1 && w.litUnlisted == 3, "C2a: N = 4 tagged, M = 1 listed, K = 3 unlisted");
    check(w.used == 2 && w.pairs[0].draws == 2 && w.pairs[1].draws == 1, "C2b: unlisted draws are named by (vs, ps): the same pair is one entry");
    for (uint64_t i = 0; i < 10; ++i) { w.noteLit(&eye, true, false); w.nameUnlisted(0x9000 + i, 0x1); }
    check(w.used == kBoundaryPairs && w.pastTable == 10 - (kBoundaryPairs - 2), "C2c: past the table's room the draws are counted, unnamed");
}

void caseC3() {
    UiBoundaryWindow w;
    int eye = 0, other = 0;
    // before the first composite: nothing counted
    w.noteLdr(&eye, false, false, false);
    w.noteLdr(&eye, false, false, true);
    check(w.ldrLearned + w.ldrReadsEye + w.ldrOther == 0 && w.ldrComposites == 0, "C3a: draws before the 8-bit target's first composite are not counted");
    w.noteLdr(&eye, true, true, false);   // the anchor
    check(w.ldrComposites == 1 && w.ldrLearned == 0, "C3b: the first composite is the anchor, not counted as a learned draw");
    w.noteLdr(&eye, true, true, false);   // learned
    w.noteLdr(&eye, true, false, true);   // named by the hash rule, not tagged, reads an eye-sized input -> b
    w.noteLdr(&eye, false, false, true);  // b
    w.noteLdr(&eye, false, false, false); // c
    w.noteLdr(&other, false, false, false);   // another target, no composite yet: not counted
    check(w.ldrLearned == 1 && w.ldrReadsEye == 2 && w.ldrOther == 1, "C3c: after the anchor each draw is exactly one of: learned (a), reads an eye-sized texture (b), other (c)");
    w.frameEnd();
    w.noteLdr(&eye, false, false, false);
    check(w.ldrOther == 1, "C3d: frameEnd re-arms the anchor: the next frame's draws before its first composite are not counted");
}

void caseC4() {
    UiBoundaryWindow w;
    int t[12];
    for (int i = 0; i < 12; ++i) w.noteLit(&t[i], false, false);
    check(w.runOverflow == 0 && w.litDraws == 12, "C4a: untagged draws into many targets spend no run slot (the G-buffer's targets no longer fill the table)");
    for (int i = 0; i < 10; ++i) w.noteLit(&t[i], true, true);
    check(w.runOverflow == 2 && w.litTagged == 10, "C4b: only targets with a tagged draw take a slot: a ninth and tenth find none, counted as overflow, still counted as tagged");
    w.frameEnd();
    w.noteLit(&t[9], true, true); w.frameEnd();
    check(w.litRuns == 9, "C4c: the table is cleared by frameEnd (a target that overflowed last frame has a slot now)");
    // The failure the first flight showed: other targets drawn first, THEN the lit target: its tagged draws must still be measured.
    UiBoundaryWindow v;
    int g[6], lit = 0;
    for (int i = 0; i < 6; ++i) for (int k = 0; k < 20; ++k) v.noteLit(&g[i], false, false);
    for (bool tg : {false, true, false, false, true}) v.noteLit(&lit, tg, true);
    v.frameEnd();
    check(v.runOverflow == 0 && v.litRuns == 1 && v.sceneBetween == 2 && v.maxBetween == 2, "C4d: scene draws into six other targets first, then the lit target: its run is found and X = 2");
}

// ---- T: the lines ---------------------------------------------------------------------------------------------------------------------
bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

void caseT1() {
    UiBoundaryWindow w;
    UiBoundaryContext c;
    const UiBoundaryText t = uiBoundaryText(w, c, 0);
    check(has(t.census, "ui boundary: lit target: 0.00 draws a frame sample a learned surface (of 0.0 lit draws a frame); 0.00 by a listed VS, 0.00 unlisted (none);"),
          "T1a: an empty window still prints the line, with zeros: a count that ran and found nothing");
    check(has(t.census, "8-bit target after first composite: 0.00 a learned, 0.00 b reads-eye, 0.00 c other a frame") && has(t.census, "[vr].") &&
              has(t.census, "scene draws between first and last such draw: 0.0 an eye-frame (max 0, 0 eye-frames with one)"),
          "T1b: ...with the 8-bit clause, the between-draws clause, the profile");
    check(has(t.cost, "ui boundary cost: tagging no bind sampled (0.0 binds a frame) + no draw note sampled (0 draws a frame);") && has(t.cost, "0 checks, 0 missed a learned panel, 0 false, 0 frames with a miss, 0 slots it could not resolve"),
          "T1c: the cost line says when nothing was sampled, and prints the verification counters at zero");
}

void caseT2() {
    UiBoundaryWindow w;
    UiBoundaryContext c;
    c.detectorOn = false;
    c.probe = "node probe: inert (x).";
    const UiBoundaryText t = uiBoundaryText(w, c, 1800);
    check(has(t.census, "ui boundary: NOT COUNTED (the interface depth pass is not running") && has(t.census, "in 1800 frames [vr].") && !has(t.census, "draws a frame"),
          "T2a: with the detector off the line says NOT COUNTED and why, never a zero that would pass for a finding");
    check(has(t.cost, "ui boundary cost: NOT COUNTED") && has(t.cost, "node probe: inert (x)."), "T2b: the cost line says so too, and still carries the probe's clause");
}

void caseT3() {
    UiBoundaryWindow w;
    int eye = 0;
    for (int f = 0; f < 4; ++f) {
        for (int i = 0; i < 22; ++i) { w.noteLit(&eye, true, false); w.nameUnlisted(0x925ACEDA0153AA5Bull, 0x738A98ED038E58B7ull); }
        for (int i = 0; i < 3; ++i) { w.noteLit(&eye, true, false); w.nameUnlisted(0xAAAAull, 0xBBBBull); }
        for (int i = 0; i < 5; ++i) w.noteLit(&eye, true, true);
        for (int i = 0; i < 100; ++i) w.noteLit(&eye, false, false);
        w.frameEnd();
    }
    UiBoundaryContext c;
    c.profile = "flat";
    const UiBoundaryText t = uiBoundaryText(w, c, 4);
    check(has(t.census, "30.00 draws a frame sample a learned surface (of 130.0 lit draws a frame); 5.00 by a listed VS, 25.00 unlisted (top unlisted: vs 925ACEDA0153AA5B ps 738A98ED038E58B7 88 draws, vs 000000000000AAAA ps 000000000000BBBB 12 draws)"),
          "T3a: N, M, K per frame and the unlisted pairs by vs/ps with window counts, busiest first");
    check(has(t.census, "[flat].") && has(t.census, "in 4 frames (4 live)"), "T3b: the profile and the frame counts");
}

void caseT4() {
    UiBoundaryWindow w;
    w.bindCalls = 3200; w.bindSamples = 200; w.bindTicks = 200 * 500;      // 500 ticks a call
    w.noteCalls = 6400; w.noteSamples = 100; w.noteTicks = 100 * 250;      // 250 ticks a call
    w.framesLive = 10;
    UiBoundaryContext c;
    c.qpcFrequency = 10000000;   // 100 ns a tick
    const UiBoundaryText t = uiBoundaryText(w, c, 10);
    // bind: 500 ticks = 50 us a call?? 500/1e7 s = 50 us -> 0.05 ms; x 3200 / 10 frames = 16 ms a frame (an absurd input, to make the arithmetic visible)
    check(std::fabs(boundaryMsPerFrame(3200, 200, 200 * 500, 10000000, 10) - 16.0) < 1e-9, "T4a: calls x mean sampled cost / frames: 3200 x 0.05 ms / 10 = 16 ms a frame");
    check(has(t.cost, "tagging 16.0000 ms a frame on the bind hook (320.0 binds a frame, 1 in 16 timed, 200 samples)"), "T4b: the bind figure and its basis are printed");
    check(has(t.cost, "+ 16.0000 ms a frame on the draw note, the read and the count (640 draws a frame, 1 in 64 timed, 100 samples)"), "T4c: ...and the draw note's");
    check(boundaryMsPerFrame(10, 0, 0, 1, 10) == 0.0 && boundaryMsPerFrame(10, 5, 5, 0, 10) == 0.0 && boundaryMsPerFrame(10, 5, 5, 1, 0) == 0.0,
          "T4d: no samples, no frequency or no frames is zero, never a division by zero");
}

void caseT5() {
    UiBoundaryWindow w;
    w.checks = 12; w.missed = 3; w.falseHits = 1; w.missedFrames = 2; w.retags = 5; w.learnChanges = 7; w.memoHits = 900; w.resolved = 40; w.unresolved = 2;
    w.rechecked = 30; w.otherDim = 25; w.unknownHits = 6; w.verifyUnknown = 4; w.slotTagged[1] = 11; w.slotTagged[2] = 13;
    w.framesLive = 1;
    UiBoundaryContext c;
    const UiBoundaryText t = uiBoundaryText(w, c, 1);
    check(has(t.cost, "900 memo hits, 30 rejudged after a learned-set change without a resolve, 40 views resolved (25 of them no 2D texture, kept as such), "
                        "2 unresolvable (6 binds answered from that short memo), 5 retags on 7 learned-set changes;") &&
              has(t.cost, "tagged lit draws by slot s0 0 s1 11 s2 13 s3 0;") &&
              has(t.cost, "12 checks, 3 missed a learned panel, 1 false, 2 frames with a miss, 4 slots it could not resolve"),
          "T5a: the tagger's counters and the verification's (re-creation frames: the frames with a miss) are printed");
    // frameEnd turns missedThisFrame into a counted frame
    UiBoundaryWindow v;
    int e = 0;
    v.noteLit(&e, false, false);
    v.missedThisFrame = true;
    v.frameEnd();
    v.noteLit(&e, false, false);
    v.frameEnd();
    check(v.missedFrames == 1 && !v.missedThisFrame, "T5b: a frame with a missed panel is counted once, and the flag does not carry to the next frame");
}

// ---- R: the lit-target rule ------------------------------------------------------------------------------------------------------------
void caseR1() {
    UiFamilyFacts f;
    f.targetKind = 1;
    f.vs = 0x1234567890ABCDEFull;   // a vertex shader no hash names
    UiFamilyWhy why = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap, "R1a: without the tag an unnamed vertex shader on the lit target is no family");
    f.panelTagged = true;
    why = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kHolo && why == UiFamilyWhy::kPanelTag, "R1b: with the tag, whatever its vertex shader, it is the cockpit holo panels, by the tag");
    f.excluded = true;
    why = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone, "R1c: ui_depth's exclusion wins (the null-output quad keeps a leftover surface bound)");
    // A hash-named draw is decided exactly as before, tag or not.
    for (uint64_t vs : {kUiVsHolo, kUiVsHoloGuiFxOff, kUiVsHoloAlt, kUiVsFlightHud, kUiVsSprite}) {
        UiFamilyFacts a, b;
        a.targetKind = b.targetKind = 1;
        a.vs = b.vs = vs;
        b.panelTagged = true;
        UiFamilyWhy wa = UiFamilyWhy::kOther, wb = UiFamilyWhy::kOther;
        check(uiLayerFamilyFor(a, &wa) == uiLayerFamilyFor(b, &wb) && wa == wb && wa == UiFamilyWhy::kDirect, "R1d: a hash-named draw has the same family and the same reason with the tag set (the list is the first answer)");
    }
    // The 8-bit target and non-eye targets ignore the lit-target fact.
    UiFamilyFacts g;
    g.targetKind = 2; g.vs = 0x1234567890ABCDEFull; g.panelTagged = true;
    why = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(g, &why) == UiLayerFamily::kNone, "R1e: on the 8-bit target the fact is not read (that tier already names by what it samples)");
    g.targetKind = 0;
    check(uiLayerFamilyFor(g) == UiLayerFamily::kNone, "R1f: not an eye target: no family");
    check(std::strcmp(uiFamilyWhyName(UiFamilyWhy::kPanelTag), "a learned panel surface sampled in the lit target") == 0, "R1g: the reason has its name");
}

void caseR2() {
    // Every combination of the old facts with panelTagged false is the rule as it was: the other 3 shaders and families answer the same.
    for (int kind = 0; kind <= 2; ++kind) for (int bits = 0; bits < 8; ++bits) {
        UiFamilyFacts a, b;
        a.targetKind = b.targetKind = kind;
        a.vs = b.vs = bits & 1 ? kUiVsHolo : 0x42;
        a.excluded = b.excluded = (bits & 2) != 0;
        a.learnedSurface = b.learnedSurface = (bits & 4) != 0;
        b.panelTagged = false;
        UiFamilyWhy wa, wb;
        check(uiLayerFamilyFor(a, &wa) == uiLayerFamilyFor(b, &wb) && wa == wb, "R2a: the new fact defaults to false and changes nothing when false");
    }
    UiFamilyFacts h;
    h.targetKind = 1; h.vs = kUiVsOrbitLines; h.ps = kUiPsOrbitLines; h.panelTagged = true;
    UiFamilyWhy why = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(h, &why) == UiLayerFamily::kOrbitLines && why == UiFamilyWhy::kDirect, "R2b: the supercruise families named by pair keep their family when the tag is set");
}

void caseA1() {
    UiBoundaryAnnounced seen;
    UiBoundaryPair p;
    p.vs = 0x925ACEDA0153AA5Bull; p.ps = 0x738A98ED038E58B7ull; p.draws = 22; p.caught = true;
    const std::string first = boundaryAnnounce(p, seen);
    check(first == "ui quality: layer: new panel VS seen: vs 925ACEDA0153AA5B ps 738A98ED038E58B7, 22 draws in its first window -- not on the hash list; taken as a cockpit holo panel because it samples a learned panel surface in the lit HDR target.",
          "A1a: the line names the vertex and pixel shader and the draws");
    p.draws = 30;
    check(boundaryAnnounce(p, seen).empty(), "A1b: once a session for a vertex shader: the same one in a later window is silent");
    p.ps = 0x1;
    check(boundaryAnnounce(p, seen).empty(), "A1c: ...whatever its pixel shader (the key is the vertex shader)");
    UiBoundaryPair q;
    q.vs = 0x1111; q.ps = 0x2; q.draws = 5; q.caught = false;
    check(boundaryAnnounce(q, seen).empty(), "A1d: a pair the rule did not take (excluded) is not announced as a panel");
    q.caught = true; q.draws = 0;
    check(boundaryAnnounce(q, seen).empty(), "A1e: a pair with no draws is not announced");
    q.draws = 1;
    check(!boundaryAnnounce(q, seen).empty(), "A1f: another vertex shader is");
    // the window carries 'caught' through nameUnlisted
    UiBoundaryWindow w; int e = 0;
    w.noteLit(&e, true, false); w.nameUnlisted(0x77, 0x1, false);
    w.noteLit(&e, true, false); w.nameUnlisted(0x77, 0x1, true);
    check(w.used == 1 && w.pairs[0].draws == 2 && w.pairs[0].caught, "A1g: a pair is 'caught' once any of its draws was");
}

// ---- N: the node probe on a synthetic image -------------------------------------------------------------------------------------------
struct Image {
    uint8_t* base = nullptr;
    Image() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x5400000, MEM_RESERVE, PAGE_NOACCESS));
        if (!base) throw std::runtime_error("N: no address space for the synthetic image");
        commit(0, PAGE_READWRITE);
        commit(unode::kFunctionRva & ~0xFFFu, PAGE_EXECUTE_READWRITE);
        commit(unode::kSlotRva & ~0xFFFu, PAGE_READWRITE);
        std::memset(base, 0, 0x1000);
        const uint32_t peOff = 0x80;
        std::memcpy(base + 0x3C, &peOff, 4);
        std::memcpy(base + peOff, "PE\0\0", 4);
        const uint16_t magic = 0x20B;
        std::memcpy(base + peOff + 24, &magic, 2);
        const uint32_t stamp = unode::kExpectedTimestamp, size = unode::kExpectedImageSize;
        std::memcpy(base + peOff + 8, &stamp, 4);
        std::memcpy(base + peOff + 0x50, &size, 4);
        // The routine: the real head, then an epilogue that undoes it (the head saves RSI at [rsp+80h], loads [rcx+0B0h]).
        uint8_t* fn = base + unode::kFunctionRva;
        std::memcpy(fn, unode::kHead, unode::kHeadBytes);
        const uint8_t tail[] = {0x48, 0x8B, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00,   // mov rsi,[rsp+80h]
                                0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,         // add rsp,88h
                                0x41, 0x5F, 0x5D, 0xC3};                          // pop r15 / pop rbp / ret
        std::memcpy(fn + unode::kHeadBytes, tail, sizeof(tail));
        const uint64_t value = reinterpret_cast<uint64_t>(base) + unode::kFunctionRva;
        std::memcpy(base + unode::kSlotRva, &value, 8);
    }
    void commit(uint32_t rva, DWORD prot) {
        if (!VirtualAlloc(base + rva, 0x1000, MEM_COMMIT, prot)) throw std::runtime_error("N: commit failed");
    }
    uint64_t slot() const { uint64_t v; std::memcpy(&v, base + unode::kSlotRva, 8); return v; }
    ~Image() { VirtualFree(base, 0, MEM_RELEASE); }
};

void caseN1() {
    uiNodeProbeTestReset();
    char clause[480];
    uiNodeProbeClause(clause, sizeof(clause));
    check(has(clause, "node probe: not attempted."), "N1a: before any attempt the clause says so (a log line without it is a build that never carried the probe)");
    Image img;
    uiNodeProbeTestSetImage(img.base);
    unode::Located loc;
    check(unode::locate(img.base, &loc), "N1b: the synthetic image passes the pin");
    {   // wrong stamp
        Image bad;
        const uint32_t stamp = unode::kExpectedTimestamp + 1;
        std::memcpy(bad.base + 0x80 + 8, &stamp, 4);
        check(!unode::locate(bad.base, &loc) && has(loc.why, "not build 332841's"), "N1c: another build's PE stamp is refused, with the reason");
    }
    {   // wrong slot value
        Image bad;
        const uint64_t v = 0x1234;
        std::memcpy(bad.base + unode::kSlotRva, &v, 8);
        check(!unode::locate(bad.base, &loc) && has(loc.why, "slot 5"), "N1d: a slot that does not hold the routine is refused");
    }
    {   // wrong prologue
        Image bad;
        bad.base[unode::kFunctionRva + 3] ^= 1;
        check(!unode::locate(bad.base, &loc) && has(loc.why, "does not start"), "N1e: a routine whose first bytes differ is refused");
    }
    check(!unode::locate(nullptr, &loc), "N1f: no module is a refusal, not a crash");
}

void caseN2() {
    uiNodeProbeTestReset();
    Image img;
    uiNodeProbeTestSetImage(img.base);
    const uint64_t original = img.slot();
    uiNodeProbeInstall();
    check(uiNodeProbeTestAttempted() && uiNodeProbeTestInstalled() && img.slot() != original, "N2a: installed: the slot now holds the forwarder, not the routine");
    const uint64_t forwarder = img.slot();
    uiNodeProbeInstall();
    check(img.slot() == forwarder, "N2b: a second call does nothing (once, ever)");
    uint8_t node[0x200] = {};
    using Fn = void(__fastcall*)(void*, void*);
    g_stubLit = 10; g_stubLdr = 2; g_stubThread = GetCurrentThreadId();
    reinterpret_cast<Fn>(forwarder)(node, nullptr);
    g_stubLit = 4000; g_stubLdr = 7;
    reinterpret_cast<Fn>(forwarder)(node, nullptr);
    check(uiNodeProbeTestCalls() == 2, "N2c: calling the slot runs through the counter to the original and returns (two calls counted)");
    char clause[480];
    uiNodeProbeClause(clause, sizeof(clause));
    check(has(clause, "node probe: live (exe+0x52EA060 -> exe+0x285A540), 2 calls in 1 frames, 0 of them on another thread than the eye draws;") &&
              has(clause, "after 4000 lit-target and 7 8-bit-target eye draws of its frame."),
          "N2d: the clause: live, the slot and routine, calls, frames, threads, and where the last call fell among the eye draws");
    uiNodeProbeClause(clause, sizeof(clause));
    check(has(clause, "but not called in this window."), "N2e: a window with no call says so (the counters are per window)");
    g_stubLit = 3; g_stubThread = GetCurrentThreadId() + 1;
    reinterpret_cast<Fn>(forwarder)(node, nullptr);
    uiNodeProbeClause(clause, sizeof(clause));
    check(has(clause, "1 calls in 1 frames, 1 of them on another thread than the eye draws"), "N2f: a call from a different thread than the eye draws is counted as such");
    uiNodeProbeTestReset();
    check(img.slot() == original, "N2g: (the rig's reset restores the slot)");
}

void caseN3() {
    uiNodeProbeTestReset();
    Image img;
    img.base[unode::kFunctionRva + 5] ^= 0xFF;
    uiNodeProbeTestSetImage(img.base);
    const uint64_t original = img.slot();
    uiNodeProbeInstall();
    char clause[480];
    uiNodeProbeClause(clause, sizeof(clause));
    check(uiNodeProbeTestAttempted() && !uiNodeProbeTestInstalled() && img.slot() == original && has(clause, "node probe: inert ("),
          "N3a: a mismatch patches nothing and the clause says inert, with the reason");
    uiNodeProbeInstall();
    check(!uiNodeProbeTestInstalled(), "N3b: a refusal is final");
    uiNodeProbeTestReset();
}

// ---- M: measured cost -----------------------------------------------------------------------------------------------------------------
// The memo here is the same structure the DLL's tagger probes (ui_depth.cpp PtrMemo: 1024 slots, 4-way linear probe, frame-stamped), so the
// figure is the tagger's hit path; a miss adds one bindingResolve (a guarded GetResource + GetDesc), counted by the DLL's own "resolved" figure.
struct RigMemo {
    struct Slot { const void* key = nullptr; uint32_t frame = 0; uint64_t value = 0; };
    Slot slots[1024];
    static uint32_t home(const void* p) { return static_cast<uint32_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 2654435761u) & 1023; }
    bool get(const void* p, uint32_t now, uint32_t life, uint64_t* v) const {
        const uint32_t h = home(p);
        for (uint32_t i = 0; i < 4; ++i) {
            const Slot& s = slots[(h + i) & 1023];
            if (s.key != p) continue;
            if (now - s.frame >= life) return false;
            *v = s.value;
            return true;
        }
        return false;
    }
    void put(const void* p, uint32_t now, uint64_t v) {
        const uint32_t h = home(p);
        uint32_t victim = h;
        for (uint32_t i = 0; i < 4; ++i) { const uint32_t idx = (h + i) & 1023; if (slots[idx].key == p || !slots[idx].key) { victim = idx; break; } }
        slots[victim] = Slot{p, now, v};
    }
};

void caseM1() {
    RigMemo memo;
    std::vector<uint8_t> views(512);   // 512 distinct "views", like a busy frame's
    for (size_t i = 0; i < views.size(); ++i) memo.put(&views[i], 1, (i % 7 == 0 ? 1u : 0u) | (1ull << 8));
    PanelTag tag;
    volatile uint64_t sink = 0;
    const uint32_t gen = 1;
    tag.gen = gen;
    const int kBinds = 2000000;
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kBinds; ++i) {
        const uint32_t start = static_cast<uint32_t>(i & 1), n = 1u + static_cast<uint32_t>((i >> 1) & 1);
        panelTagBind(tag, gen, start, n, [&](uint32_t s) { return static_cast<void*>(&views[(i * 7 + s * 13) & 511]); },
                     [&](void* v) -> uint8_t { uint64_t m = 0; return memo.get(v, 50, 120, &m) ? static_cast<uint8_t>(m & 3) : uint8_t(0); });
        sink += tag.panel;
    }
    QueryPerformanceCounter(&b);
    const double nsBind = 1e9 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / kBinds;
    // The draw note's counting half on the real window (the read is one load).
    UiBoundaryWindow w;
    int eye = 0;
    const int kDraws = 4000000;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kDraws; ++i) {
        const bool tagged = (uiPanelTagMask() & 1) != 0 || (i % 200 == 0);
        w.noteLit(&eye, tagged, i % 400 != 0);
        if (i % 4096 == 4095) w.frameEnd();
    }
    QueryPerformanceCounter(&b);
    const double nsDraw = 1e9 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / kDraws;
    std::printf("ui panel tag test: M1 measured on this machine: tag bind (memo hit path, 1-2 slots a call) %.1f ns a bind; census draw count %.1f ns a draw. "
                "At the DLL's census (about 300 binds and 5000 lit draws a frame): %.4f ms + %.4f ms a frame.\n",
                nsBind, nsDraw, nsBind * 300 / 1e6, nsDraw * 5000 / 1e6);
    check(sink != 12345 && nsBind < 400.0, "M1a: the memo-hit bind step is well under 0.4 us (a guard against an accidental O(n) or a resolve on the hit path)");
    check(nsDraw < 100.0, "M1b: the draw-side counting is well under 0.1 us a draw");
}

// ---- P: the pins -------------------------------------------------------------------------------------------------------------------------
std::string readFile(const std::string& rel) {
    std::ifstream in(g_root + "\\" + rel, std::ios::binary);
    if (!in) throw std::runtime_error("P: cannot read " + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
std::string between(const std::string& s, const std::string& from, const std::string& to) {
    const size_t a = s.find(from);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find(to, a + from.size());
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}
// predicate over a text, and the same predicate over a copy with one edit must fail (the control).
void pin(const char* label, const std::string& text, const std::function<bool(const std::string&)>& pred,
         const std::function<std::string(std::string)>& breakIt) {
    check(pred(text), label);
    const std::string broken = breakIt(text);
    check(broken != text && !pred(broken), (std::string("control: ") + label).c_str());
}
std::string eraseAll(std::string s, const std::string& what) {
    for (size_t at; (at = s.find(what)) != std::string::npos;) s.erase(at, what.size());
    return s;
}

void casePins() {
    const std::string vscreen = readFile("src\\d3d11\\vscreen.cpp");
    const std::string depth = readFile("src\\d3d11\\ui_depth.cpp");
    const std::string shadow = readFile("src\\d3d11\\binding_shadow.cpp");
    const std::string layer = readFile("src\\d3d11\\ui_layer.cpp");
    const std::string device = readFile("src\\d3d11\\device_hook.cpp");
    const std::string build = readFile("build.bat");

    // P1: the bind hook tags after the shadow's write and before the real call, gated.
    pin("P1: the PSSetShaderResources hook tags the slots right after the shadow's writes and before the game's call, gated on the layer and the pass",
        between(vscreen, "void STDMETHODCALLTYPE hookedPSSetShaderResources", "void STDMETHODCALLTYPE hookedVSSetConstantBuffers"),
        [](const std::string& h) {
            const size_t set = h.find("bindingSet(static_cast<BindSlot>(");
            const size_t tagAt = h.find("if (g_panelTagLive) uiDepthTagBind(start, n);");
            const size_t real = h.rfind("g_state->realPSSetShaderResources(self, start, n, srvs);");
            return set != std::string::npos && tagAt != std::string::npos && real != std::string::npos && set < tagAt && tagAt < real;
        },
        [](std::string h) { return eraseAll(h, "uiDepthTagBind(start, n)"); });
    pin("P1b: the tag is made only while the UI layer is live: the layer's frame boundary publishes the flag the bind hook tests, and the frame boundary stops re-tagging when it is off",
        layer + "\n" + depth,
        [](const std::string& t) {
            return t.find("g_panelTagLive = detail::g_uiLayerLive;") != std::string::npos && t.find("if (!uiDepthWantsDraws() || !g_panelTagLive) g_panelTag.gen = 0;") != std::string::npos;
        },
        [](std::string t) { return eraseAll(t, "g_panelTagLive = detail::g_uiLayerLive;"); });

    // P2: the draw hook counts after the family rule, from its result, and passes what it needs.
    pin("P2: the draw hook notes the eye draw right after the family rule, from the layer's target kind and the rule's answer",
        vscreen,
        [](const std::string& v) {
            const size_t fam = v.find("const UiLayerFamily uiFamily = uiLayerFamilyOf(g_state, kind, count);");
            const size_t note = v.find("uiDepthBoundaryNoteDraw(uiLayerTargetKind(), uiLayerTargetResource(), uiFamily != UiLayerFamily::kNone && !t_familyByTag,");
            const size_t decide = v.find("uiLayer = uiLayerDecide(self, static_cast<int>(uiFamily)");
            return fam != std::string::npos && note != std::string::npos && decide != std::string::npos && fam < note && note < decide;
        },
        [](std::string v) { return eraseAll(v, "uiDepthBoundaryNoteDraw(uiLayerTargetKind(), uiLayerTargetResource(), uiFamily != UiLayerFamily::kNone && !t_familyByTag,"); });

    // P3: the draw path only reads the bit.
    const std::string note = between(depth, "void uiDepthBoundaryNoteDraw(", "// --- the verification sample");
    pin("P3: between the draw note's read and its verification marker, the draw path reads uiPanelTagMask() and scans no slot, resolves no view and asks no learned set",
        note,
        [](const std::string& n) {
            if (n.empty() || n.find("uiPanelTagMask()") == std::string::npos) return false;
            for (const char* bad : {"bindingGet(", "bindingResolve(", "viewIsSurface(", "surfaceMatches(", "tagClassify(", "tagShadowView(", "uiDepthSampledSurfaceSlot(",
                                    "surfaceIndex(", "panelTagBind(", "tagRetagAll("})
                if (n.find(bad) != std::string::npos) return false;
            return true;
        },
        [](std::string n) { return n + " bindingGet(BindSlot::PsSrv0); "; });
    check(depth.find("if ((w.noteCalls % kVerifyEvery) == 0 && g_tagVerifyThisFrame < kVerifyPerFrame) tagVerify();") != std::string::npos,
          "P3b: the slow verification is reached only on the sampled draw, after the marker, capped per frame");

    // P4: the shadow clears the bits wherever a cell changes.
    pin("P4: bindingSet, bindingInvalidate and bindingForgetAll clear the panel bits",
        shadow,
        [](const std::string& s) {
            const std::string set = between(s, "void bindingSet(BindSlot slot, void* ptr) {", "void bindingInvalidate");
            const std::string inv = between(s, "void bindingInvalidate(BindSlot slot) {", "void bindingSetShader");
            const std::string all = between(s, "void bindingForgetAll() {", "void bindingFrameBoundary");
            return set.find("panelBitsOf(slot);") != std::string::npos && inv.find("panelBitsOf(slot);") != std::string::npos &&
                   all.find("g_panelTag.forget();") != std::string::npos && s.find("g_panelTag.clearSlot(i - first)") != std::string::npos;
        },
        [](std::string s) { return eraseAll(s, "g_panelTag.forget();"); });

    // P5: the learned set bumps the generation when it changes.
    pin("P5: adding, evicting and clearing the learned surfaces move the generation the bits are checked against",
        depth,
        [](const std::string& d) {
            const std::string add = between(d, "void addSurface(", "bool inList(");
            const std::string ev = between(d, "bool surfaceMatches(", "void addSurface(");
            const std::string clr = between(d, "    g_surfaceCount = 0;\n    g_surfaceNext = 0;\n    uiPanelTagLearnChanged();", "g_viewMemo.clear();");
            return add.find("uiPanelTagLearnChanged();") != std::string::npos && ev.find("uiPanelTagLearnChanged();") != std::string::npos && !clr.empty();
        },
        [](std::string d) { return eraseAll(d, "uiPanelTagLearnChanged();"); });

    // P6: the report: two lines every window, and the reset.
    pin("P6: the 30 s window prints the census and the cost line, whatever the counts, then starts over",
        depth,
        [](const std::string& d) {
            const std::string r = between(d, "void uiDepthBoundaryReport(", "int uiDepthEyeOfTarget(");
            return r.find("Log::get().note(\"%s\", text.census.c_str());") != std::string::npos && r.find("Log::get().note(\"%s\", text.cost.c_str());") != std::string::npos &&
                   r.find("g_boundary.reset();") != std::string::npos && r.find("c.detectorOn = uiDepthWantsDraws();") != std::string::npos;
        },
        [](std::string d) { return eraseAll(d, "Log::get().note(\"%s\", text.cost.c_str());"); });
    pin("P6b: the layer's window calls the report beside the composites line",
        layer,
        [](const std::string& l) {
            const size_t comp = l.find("uiSceneCompositeFormat(composites, sizeof(composites), g_win.scene, g_win.frames, uiDepthWantsDraws());");
            const size_t rep = l.find("uiDepthBoundaryReport(g_win.frames, runtimeFlatProfile() ? \"flat\" : \"vr\");");
            return comp != std::string::npos && rep != std::string::npos && comp < rep;
        },
        [](std::string l) { return eraseAll(l, "uiDepthBoundaryReport(g_win.frames"); });

    // P7: the frame boundary closes the census's runs and re-tags a stale tag.
    pin("P7: uiDepthFrameBoundary closes the census frame and recomputes a stale tag (or marks the tag stale while the pass is off)",
        between(depth, "void uiDepthFrameBoundary(ID3D11DeviceContext* ctx) {", "releaseChromeHeld();"),
        [](const std::string& f) {
            return f.find("g_boundary.frameEnd();") != std::string::npos && f.find("g_tagVerifyThisFrame = 0;") != std::string::npos &&
                   f.find("g_panelTag.gen = 0;") != std::string::npos && f.find("tagRetagAll();") != std::string::npos;
        },
        [](std::string f) { return eraseAll(f, "g_boundary.frameEnd();"); });

    // P8: the probe is installed at startup and built.
    pin("P8: device_hook installs the node probe beside the other engine installers, and build.bat compiles it into the DLL",
        device + "\n" + build,
        [](const std::string& t) {
            return t.find("uiNodeProbeInstall();") != std::string::npos && t.find("\"src\\d3d11\\ui_node_probe.cpp\"") != std::string::npos;
        },
        [](std::string t) { return eraseAll(t, "uiNodeProbeInstall();"); });

    // P9: who reads the tag. The census (ui_depth.cpp) and the one place that turns it into a fact for the rule (vscreen.cpp's uiLayerFamilyOf); the layer's decision and
    // the family rule know it only as the fact panelTagged.
    {
        const char* files[] = {"src\\d3d11\\ui_layer.cpp", "src\\d3d11\\ui_layer_math.h", "src\\d3d11\\flat_ui_layer.cpp", "src\\d3d11\\ui_layer_seed.h"};
        bool clean = true;
        for (const char* rel : files) {
            std::ifstream in(g_root + "\\" + rel, std::ios::binary);
            if (!in) continue;
            std::stringstream ss;
            ss << in.rdbuf();
            if (ss.str().find("uiPanelTagMask") != std::string::npos || ss.str().find("g_panelTag.") != std::string::npos) clean = false;
        }
        check(clean, "P9: the layer's decision, the family rule and the flat adapter read no tag byte: uiPanelTagMask appears only in the census and the fact-gatherer");
    }
    pin("P10: uiLayerFamilyOf sets the lit-target fact from the one byte, for the lit target and the VR profile only, with ui_depth's exclusion carried; the family is told apart from a hash's",
        between(vscreen, "thread_local bool t_familyByTag", "// The verdicts that forward the game's own draw"),
        [](const std::string& f) {
            return f.find("if (f.targetKind == 1 && uiPanelTagMask() != 0 && !runtimeFlatProfile()) {") != std::string::npos &&
                   f.find("f.panelTagged = true;") != std::string::npos && f.find("f.excluded = uiDepthIsExcluded(f.vs);") != std::string::npos &&
                   f.find("t_familyByTag = why == UiFamilyWhy::kPanelTag;") != std::string::npos;
        },
        [](std::string f) { return eraseAll(f, "&& !runtimeFlatProfile()"); });
    {
        const std::string math = readFile("src\\d3d11\\ui_layer_math.h");
        const std::string rule = between(math, "inline UiLayerFamily uiLayerFamilyFor(", "// THE ON-FOOT GATE.");
        pin("P10b: in the rule the tag is the LAST answer on the lit target (after every hash), is skipped when excluded, and takes the family holo",
            rule,
            [](const std::string& r) {
                const size_t direct = r.find("if (out != UiLayerFamily::kNone) w = UiFamilyWhy::kDirect;");
                const size_t tag = r.find("else if (f.panelTagged && !f.excluded) {");
                const size_t holo = r.find("out = UiLayerFamily::kHolo;", tag == std::string::npos ? 0 : tag);
                return direct != std::string::npos && tag != std::string::npos && holo != std::string::npos && direct < tag && tag < holo;
            },
            [](std::string r) { return eraseAll(r, "&& !f.excluded"); });
    }
    // P11: the tagger spends no resolve on a view it already knows is not a surface.
    pin("P11: tagClassify keeps a view that is no 2D texture (and one it could not resolve, for a short while) and re-judges a 2D texture after a learned-set change from what it kept",
        between(depth, "uint8_t tagClassify(void* view) {", "// The shadow's view pointer for PS SRV slot i"),
        [](const std::string& f) {
            const size_t other = f.find("if (e->state == 2) { ++w.memoHits; return 0; }");
            const size_t unk = f.find("if (age < kTagUnknownLife) { ++w.unknownHits; return 0; }");
            const size_t keep = f.find("surfaceMatches(e->info)");
            const size_t resolve = f.find("bindingResolve(view, &info)");
            return other != std::string::npos && unk != std::string::npos && keep != std::string::npos && resolve != std::string::npos &&
                   other < resolve && unk < resolve && keep < resolve && f.find("info.dim == 0") != std::string::npos;
        },
        [](std::string f) { return eraseAll(f, "if (e->state == 2) { ++w.memoHits; return 0; }"); });
    check(readFile("src\\d3d11\\binding_shadow.cpp").find("out->dim = static_cast<uint32_t>(dim);") != std::string::npos,
          "P11b: the shadow's resolver reports the dimension even when the call returns false (a 3D or 1D texture), which is what tells 'known, no panel' from 'not knowable'");
}

// ---- the runner ---------------------------------------------------------------------------------------------------------------------------
struct Case { const char* name; void (*fn)(); bool needsRoot; };
const Case kCases[] = {
    {"B1", caseB1, false}, {"B2", caseB2, false}, {"B3", caseB3, false}, {"B4", caseB4, false}, {"B5", caseB5, false}, {"B6", caseB6, false},
    {"R1", caseR1, false}, {"R2", caseR2, false}, {"A1", caseA1, false},
    {"C1", caseC1, false}, {"C2", caseC2, false}, {"C3", caseC3, false}, {"C4", caseC4, false},
    {"T1", caseT1, false}, {"T2", caseT2, false}, {"T3", caseT3, false}, {"T4", caseT4, false}, {"T5", caseT5, false},
    {"N1", caseN1, false}, {"N2", caseN2, false}, {"N3", caseN3, false},
    {"M1", caseM1, false}, {"P", casePins, true},
};

}  // namespace

int main(int argc, char** argv) {
    std::string only;
    bool self = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (argv[i][0] != '-') g_root = argv[i];
        else { std::fputs("usage: ui_panel_tag_test --self-test [<repo root>] [--only B1,C2,...] | --dry-run\n", stderr); return 2; }
    }
    if (dry) { std::puts("ui panel tag test: dry run (no checks run)"); return 0; }
    if (!self) { std::fputs("usage: ui_panel_tag_test --self-test [<repo root>] [--only B1,C2,...] | --dry-run\n", stderr); return 2; }
    for (const Case& c : kCases) {
        if (!only.empty() && (',' + only + ',').find(',' + std::string(c.name) + ',') == std::string::npos) continue;
        if (c.needsRoot && (g_root.empty() || !only.empty())) continue;
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::printf("ui panel tag test: FAIL %s: %s\n", c.name, e.what());
            return 1;
        }
    }
    std::printf("ui panel tag test: all %u checks passed\n", g_checks);
    return 0;
}
