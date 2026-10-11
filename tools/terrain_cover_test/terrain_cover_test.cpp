// The terrain cull cover (src/d3d11/terrain_cull_cover_core.h, terrain_cull_cover_hook.cpp; docs\terrain-culling.md, 2026-10-10).
//
//   C1  the rule: the three rigs' predicted cull half-angles (cropped Crystal Super, uncropped Crystal Super, Quest 3 via Virtual Desktop), a symmetric headset, an input that is
//       already wider, non-finite / zero / out-of-range input, the key off, no tangents; it never narrows (a sweep) and a second pass changes nothing
//   C2  T and H from the frusta handed to the game; the packed word
//   C3  the callee's head, the call-site signature and the scan, on synthetic buffers
//   C4  locate() on synthetic modules: the known build (stamp and size), a wrong stamp (the scan: one match, none, two, a wrong callee), a known stamp over wrong bytes
//   C5  the production hook, compiled with EDVR_TERRAIN_COVER_TEST and the real CodeHook, on a synthetic module whose cull-plane builder has the same six-argument shape
//       (four pointers and two floats on the stack): ONLY arguments 5 and 6 change, the other four registers and the return value are the caller's; the key off, no tangents
//       and a flat session pass through bit for bit; the first four calls and the 60 s census are what the log says; a refusal is one line
//   C6  tools\terrain_cover_fixture.log is what the production hook writes for the scripted session (python tools\edvr_log.py --tally cover reads it)
//   C7  the wiring, read as text
//
//   terrain_cover_test --self-test <repo root> | --dry-run | --print-fixture
#include "../../src/d3d11/terrain_cull_cover_core.h"
#include "../../src/d3d11/terrain_cull_cover_hook.h"
#include "../../src/common/config.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <string>
#include <vector>

namespace tc = edvr::tcover;

namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
}

static unsigned checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", what); std::exit(1); }
}
static bool closeTo(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
static double deg(double radians) { return radians * 57.29577951308232; }
static bool sameBits(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }

// ---- the three rigs (analysis\decomp\verify_20261010_tiles_frustum_emu.txt): tangents, the size the game is told, and what the game feeds the builder -------------------------
struct Rig {
    const char* name;
    float outer, inner, up, down;
    float width, height;
    float vfov() const { return std::atan(up) + std::atan(down); }
    float aspect() const { return width / height; }
    void frusta(float f[2][4]) const {   // {left, right, top, bottom}; eye 0's outer side is its left
        f[0][0] = -outer; f[0][1] = inner; f[0][2] = -up; f[0][3] = down;
        f[1][0] = -inner; f[1][1] = outer; f[1][2] = -up; f[1][3] = down;
    }
};
static const Rig kCropped = {"cropped Crystal Super", 1.2543f, 0.8746f, 1.0293f, 1.0293f, 4508.0f, 4358.0f};
static const Rig kUncropped = {"uncropped Crystal Super", 1.5294f, 1.0324f, 1.2648f, 1.2648f, 5424.0f, 5356.0f};
static const Rig kQuest3 = {"Quest 3 via VD", 1.3764f, 0.8391f, 0.9657f, 1.4282f, 3072.0f, 3264.0f};

static void caseRule() {
    // The predicted cull half-angles, from the analysis (cull +-45.83 v / +-46.80 h against an outer edge 51.44; +-51.67 / +-52.02 against 56.82; +-49.50 / +-47.78 against 54.00
    // outward and 55.00 down), and the values the builder is to be handed instead (computed outside the code under test).
    struct Case { const Rig* rig; double inVfovDeg, inAspect, outVfovDeg, outAspect; } cases[] = {
        {&kCropped, 91.6544, 1.034419, 91.6544, 1.218595},
        {&kUncropped, 103.3375, 1.012696, 103.3375, 1.209203},
        {&kQuest3, 99.0013, 0.941176, 110.0020, 0.963731},
    };
    for (const Case& c : cases) {
        float f[2][4];
        c.rig->frusta(f);
        float tanV = 0, tanH = 0;
        check(tc::frustaTangents(f, &tanV, &tanH), "C1 the rig's frusta give tangents");
        const tc::Cover r = tc::widen(c.rig->vfov(), c.rig->aspect(), tanV, tanH, true);
        check(closeTo(deg(c.rig->vfov()), c.inVfovDeg, 2e-4) && closeTo(c.rig->aspect(), c.inAspect, 1e-5), "C1 the rig's in-values are what the analysis says the game feeds the builder");
        check(r.widened && closeTo(deg(r.vfov), c.outVfovDeg, 5e-4) && closeTo(r.aspect, c.outAspect, 2e-5), c.rig->name);
        // What the builder then computes from the new values covers the image's edges.
        const double a = r.vfov * 0.5, b = std::atan(double(r.aspect) * std::tan(a));
        check(a + 1e-6 >= std::atan(double(tanV)) && b + 1e-6 >= std::atan(double(tanH)), "C1 the half-angles the builder will derive reach the largest tangents");
        check(a + 1e-6 >= c.rig->vfov() * 0.5 && b + 1e-6 >= std::atan(double(c.rig->aspect()) * std::tan(c.rig->vfov() * 0.5)), "C1 ...and are never narrower than the game's own");
        const tc::Cover again = tc::widen(r.vfov, r.aspect, tanV, tanH, true);
        check(!again.widened && sameBits(again.vfov, r.vfov) && sameBits(again.aspect, r.aspect), "C1 a second pass over the widened values changes nothing");
    }
    {   // the cropped Crystal Super is wide enough vertically already: only the aspect moves, the vFOV bit for bit
        float f[2][4];
        kCropped.frusta(f);
        float tanV, tanH;
        tc::frustaTangents(f, &tanV, &tanH);
        const tc::Cover r = tc::widen(kCropped.vfov(), kCropped.aspect(), tanV, tanH, true);
        check(sameBits(r.vfov, kCropped.vfov()) && r.aspect > kCropped.aspect(), "C1 only the axis that falls short is widened: the vertical half-angle is untouched, bit for bit");
    }
    {   // a symmetric headset whose frustum the centred cull covers
        const tc::Cover r = tc::widen(1.5f, 1.0f, 0.9f, 0.9f, true);
        check(!r.widened && sameBits(r.vfov, 1.5f) && sameBits(r.aspect, 1.0f), "C1 a symmetric frustum inside the cull is passed through bit for bit");
        // the same numbers from the game's own sums: the rounding of a float is not widening
        const float t = 1.2648f, vf = 2.0f * std::atan(t);
        const tc::Cover s = tc::widen(vf, 1.0f, t, t, true);
        check(!s.widened && sameBits(s.vfov, vf) && sameBits(s.aspect, 1.0f), "C1 tangents equal to the game's own half-angles (to a float's rounding) widen nothing");
    }
    {   // an input that is already wider than the frusta
        float f[2][4];
        kCropped.frusta(f);
        float tanV, tanH;
        tc::frustaTangents(f, &tanV, &tanH);
        const tc::Cover r = tc::widen(2.4f, 1.6f, tanV, tanH, true);
        check(!r.widened && sameBits(r.vfov, 2.4f) && sameBits(r.aspect, 1.6f), "C1 an input already wider than the frusta is unchanged");
    }
    {   // not finite, zero, out of range: unchanged
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        const float vf[] = {nan, inf, -inf, 0.0f, -1.0f, 3.1415927f, 3.2f, 100.0f};
        for (float v : vf) {
            const tc::Cover r = tc::widen(v, 1.0f, 1.5f, 1.5f, true);
            check(!r.widened && sameBits(r.vfov, v) && sameBits(r.aspect, 1.0f), "C1 a vFOV that is not finite or not in (0, pi) is passed through");
        }
        const float asp[] = {nan, inf, 0.0f, -1.0f, -0.5f};
        for (float a : asp) {
            const tc::Cover r = tc::widen(1.0f, a, 1.5f, 1.5f, true);
            check(!r.widened && sameBits(r.vfov, 1.0f) && sameBits(r.aspect, a), "C1 an aspect that is not finite or not positive is passed through");
        }
        const float tn[] = {nan, inf, 0.0f, -1.0f};
        for (float t : tn) {
            check(!tc::widen(1.0f, 1.0f, t, 1.5f, true).widened && !tc::widen(1.0f, 1.0f, 1.5f, t, true).widened, "C1 tangents that are not finite and positive (unknown) are passed through");
        }
        check(!tc::widen(1.0f, 1.0f, 1.5f, 1.5f, false).widened && sameBits(tc::widen(1.0f, 1.0f, 1.5f, 1.5f, false).aspect, 1.0f), "C1 the key off passes everything through");
        check(tc::widen(1.0f, 1.0f, 1.5f, 1.5f, true).widened, "C1 ...and the same numbers with it on are widened (the control)");
    }
    {   // it never narrows, whatever it is given
        unsigned widenedCases = 0, total = 0;
        for (double v = 20.0; v <= 175.0; v += 7.5)
            for (double asp = 0.4; asp <= 2.6; asp += 0.3)
                for (double tv = 0.2; tv <= 3.2; tv += 0.45)
                    for (double th = 0.2; th <= 3.2; th += 0.45) {
                        const float vfov = float(v / 57.29577951308232), aspect = float(asp);
                        const tc::Cover r = tc::widen(vfov, aspect, float(tv), float(th), true);
                        ++total;
                        const double aIn = vfov * 0.5, bIn = std::atan(double(aspect) * std::tan(aIn));
                        const double aOut = r.vfov * 0.5, bOut = std::atan(double(r.aspect) * std::tan(aOut));
                        if (!(std::isfinite(r.vfov) && std::isfinite(r.aspect) && r.aspect > 0.0f && aOut >= aIn - 1e-6 && bOut >= bIn - 1e-6)) check(false, "C1 narrowed or went non-finite in the sweep");
                        const double aT = std::atan(double(float(tv))), bT = std::atan(double(float(th)));
                        const bool expectWidened = aT > aIn + tc::kWidenEpsilon || bT > bIn + tc::kWidenEpsilon;
                        if (r.widened != expectWidened) check(false, "C1 widened exactly when a tangent reaches past the cull by more than the epsilon");
                        if (r.widened) {
                            ++widenedCases;
                            if (aT > aIn + tc::kWidenEpsilon) check(aOut + 1e-6 >= aT, "C1 a widened vertical axis reaches its tangent");
                            if (bT > bIn + tc::kWidenEpsilon) check(bOut + 1e-6 >= bT, "C1 a widened horizontal axis reaches its tangent");
                        } else {
                            check(sameBits(r.vfov, vfov) && sameBits(r.aspect, aspect), "C1 not widened means unchanged, bit for bit");
                        }
                    }
        check(widenedCases > 100 && widenedCases < total, "C1 the sweep both widens and leaves alone");
    }
}

static void caseTangents() {
    float f[2][4], tanV = 0, tanH = 0;
    kQuest3.frusta(f);
    check(tc::frustaTangents(f, &tanV, &tanH) && closeTo(tanV, 1.4282, 1e-6) && closeTo(tanH, 1.3764, 1e-6), "C2 Quest 3: T is the larger of up and down over both eyes, H the larger of the sides");
    kCropped.frusta(f);
    check(tc::frustaTangents(f, &tanV, &tanH) && closeTo(tanV, 1.0293, 1e-6) && closeTo(tanH, 1.2543, 1e-6), "C2 cropped Crystal Super: H is the outer tangent");
    f[1][1] = 1.9f;   // the other eye reaches further
    check(tc::frustaTangents(f, &tanV, &tanH) && closeTo(tanH, 1.9, 1e-6), "C2 the maximum is over both eyes");
    kCropped.frusta(f);
    f[0][2] = std::numeric_limits<float>::quiet_NaN();
    check(!tc::frustaTangents(f, &tanV, &tanH), "C2 a NaN anywhere: no tangents");
    kCropped.frusta(f);
    f[1][3] = 0.0f;
    check(!tc::frustaTangents(f, &tanV, &tanH), "C2 a zero anywhere: no tangents");
    kCropped.frusta(f);
    f[0][0] = std::numeric_limits<float>::infinity();
    check(!tc::frustaTangents(f, &tanV, &tanH), "C2 an infinity anywhere: no tangents");
    float a = 0, b = 0;
    tc::unpackTangents(tc::packTangents(1.0293f, 1.2543f), &a, &b);
    check(sameBits(a, 1.0293f) && sameBits(b, 1.2543f) && tc::packTangents(1.0f, 1.0f) != 0, "C2 the pair rides in one word and comes back exactly (and a real pair is never the unknown 0)");
}

// ---- synthetic modules ---------------------------------------------------------------------------------------------------------------------------------------------
// A module image reserved at its claimed size with only the pages a case needs committed (the rest would fault, and the production reads are bounded by what they ask for).
class Synth {
public:
    Synth(uint32_t stamp, uint32_t sizeOfImage, uint32_t textRva, uint32_t textSize) : size_(sizeOfImage) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, sizeOfImage, MEM_RESERVE, PAGE_READWRITE));
        check(base != nullptr, "a synthetic module could be reserved");
        commit(0, 0x1000, PAGE_READWRITE);
        put32(0x3C, 0x80);
        std::memcpy(base + 0x80, "PE\0\0", 4);
        put16(0x80 + 4, 0x8664);
        put16(0x80 + 6, 1);
        put32(0x80 + 8, stamp);
        put16(0x80 + 20, 240);
        put16(0x80 + 24, 0x20B);
        put32(0x80 + 0x50, sizeOfImage);
        uint8_t* section = base + 0x80 + 24 + 240;
        std::memcpy(section, ".text\0\0\0", 8);
        std::memcpy(section + 8, &textSize, 4);
        std::memcpy(section + 12, &textRva, 4);
        const uint32_t characteristics = 0x60000020u;
        std::memcpy(section + 36, &characteristics, 4);
    }
    ~Synth() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;
    void commit(uint32_t rva, uint32_t length, DWORD protection) {
        const uint32_t start = rva & ~0xFFFu, end = (rva + length + 0xFFFu) & ~0xFFFu;
        check(VirtualAlloc(base + start, end - start, MEM_COMMIT, protection) != nullptr, "a synthetic module's pages could be committed");
    }
    void put32(uint32_t rva, uint32_t v) { std::memcpy(base + rva, &v, 4); }
    void put16(uint32_t rva, uint16_t v) { std::memcpy(base + rva, &v, 2); }
    // Pseudo-random filler over a committed range (no run of it is a call-site fragment).
    void fill(uint32_t rva, uint32_t length) {
        uint32_t x = 12345u;
        for (uint32_t i = 0; i < length; ++i) {
            x = x * 1664525u + 1013904223u;
            base[rva + i] = uint8_t(x >> 24);
            if (base[rva + i] == 0xF3) base[rva + i] = 0xF2;   // no accidental fragment starts
        }
    }
    uint8_t* base = nullptr;
    uint32_t size() const { return size_; }

private:
    uint32_t size_;
};

// The cull-plane builder as a synthetic function: its real head (32 bytes), mov rbp,rcx, the mulss by the constant at constRva, then a body that records its four register
// arguments and the two stack floats through r8 (a record: [0] arg 5, [4] arg 6, [8] rcx, [16] rdx, [24] r9) and returns rdx, and the epilogue. Returns its length.
static const uint8_t kBody[] = {
    0x49, 0x89, 0x48, 0x08,                                      // mov [r8+8], rcx
    0x49, 0x89, 0x50, 0x10,                                      // mov [r8+16], rdx
    0x4D, 0x89, 0x48, 0x18,                                      // mov [r8+24], r9
    0xF3, 0x0F, 0x10, 0x8C, 0x24, 0xC0, 0x00, 0x00, 0x00,        // movss xmm1,[rsp+0C0h]   arg 5
    0xF3, 0x41, 0x0F, 0x11, 0x08,                                // movss [r8],xmm1
    0xF3, 0x0F, 0x10, 0x8C, 0x24, 0xC8, 0x00, 0x00, 0x00,        // movss xmm1,[rsp+0C8h]   arg 6
    0xF3, 0x41, 0x0F, 0x11, 0x48, 0x04,                          // movss [r8+4],xmm1
    0x48, 0x8B, 0x58, 0x10,                                      // mov rbx,[rax+10h]
    0x48, 0x8B, 0x68, 0x18,                                      // mov rbp,[rax+18h]
    0x48, 0x8B, 0x70, 0x20,                                      // mov rsi,[rax+20h]
    0x48, 0x89, 0xD0,                                            // mov rax,rdx
    0x48, 0x81, 0xC4, 0x90, 0x00, 0x00, 0x00,                    // add rsp,90h
    0x5F,                                                        // pop rdi
    0xC3};                                                       // ret
static void putCallee(Synth& s, uint32_t rva, uint32_t constRva, float constant = 0.5f) {
    uint8_t* p = s.base + rva;
    std::memcpy(p, tc::kHead, tc::kHeadBytes);
    p[32] = 0x48; p[33] = 0x8B; p[34] = 0xE9;                    // mov rbp,rcx
    std::memcpy(p + 35, tc::kMulss, 4);
    const int32_t rel = int32_t(int64_t(constRva) - int64_t(rva + 43));
    std::memcpy(p + 39, &rel, 4);
    std::memcpy(p + 43, kBody, sizeof(kBody));
    std::memcpy(s.base + constRva, &constant, 4);
}
// The call site: A, a gap, B, C, D, a gap, E8 rel32 to targetRva. Returns the RVA of the E8.
static uint32_t putSite(Synth& s, uint32_t rva, uint32_t targetRva, const std::vector<uint8_t>& gapAB, const std::vector<uint8_t>& gapDE) {
    uint8_t* p = s.base + rva;
    size_t at = 0;
    const auto put = [&](const uint8_t* bytes, size_t n) { std::memcpy(p + at, bytes, n); at += n; };
    put(tc::kFragA, sizeof(tc::kFragA));
    put(gapAB.data(), gapAB.size());
    put(tc::kFragB, sizeof(tc::kFragB));
    put(tc::kFragC, sizeof(tc::kFragC));
    put(tc::kFragD, sizeof(tc::kFragD));
    put(gapDE.data(), gapDE.size());
    const uint32_t callRva = rva + uint32_t(at);
    p[at] = 0xE8;
    const int32_t rel = int32_t(int64_t(targetRva) - int64_t(callRva + 5));
    std::memcpy(p + at + 1, &rel, 4);
    return callRva;
}
static const std::vector<uint8_t> kRealGapAB = {0x0F, 0x29, 0x8D, 0xC0, 0x00, 0x00, 0x00};                // movaps [rbp+0C0h],xmm1
static const std::vector<uint8_t> kRealGapDE = {0x44, 0x0F, 0x29, 0xA5, 0xE0, 0x00, 0x00, 0x00};          // movaps [rbp+0E0h],xmm12

// The known build's layout: the builder at 0x443E800, the 0.5 at 0x4DDF878, the call site at 0x4455FDD (E8 at 0x4456007).
static void buildKnown(Synth& s) {
    s.commit(0x443E000, 0x2000, PAGE_EXECUTE_READWRITE);
    s.commit(0x4455000, 0x2000, PAGE_EXECUTE_READWRITE);
    s.commit(0x4DDF000, 0x1000, PAGE_READWRITE);
    putCallee(s, tc::kTargetRva, 0x4DDF878);
    const uint32_t call = putSite(s, tc::kSiteRva, tc::kTargetRva, kRealGapAB, kRealGapDE);
    check(call == tc::kCallRva, "C3 the synthetic call site puts its E8 where the game's is (0x4456007)");
}

static void caseSignature() {
    Synth s(1u, 0x40000u, 0x1000, 0x20000);
    s.commit(0x1000, 0x20000, PAGE_EXECUTE_READWRITE);
    s.fill(0x1000, 0x20000);
    putCallee(s, 0x9000, 0x1A000);
    tc::Image img;
    img.base = s.base;
    img.size = s.size();
    check(tc::calleeMatches(img, 0x9000), "C3 the callee's head and its mulss by 0.5 are recognised");
    for (uint32_t k : {0u, 3u, 6u, 7u, 15u, 16u, 22u, 23u, 27u, 28u, 31u}) {
        s.base[0x9000 + k] ^= 0x01;
        check(!tc::calleeMatches(img, 0x9000), "C3 one wrong byte in the head is refused");
        s.base[0x9000 + k] ^= 0x01;
    }
    check(tc::calleeMatches(img, 0x9000), "C3 ...and the control, restored");
    {
        const float notHalf = 0.6f;
        std::memcpy(s.base + 0x1A000, &notHalf, 4);
        check(!tc::calleeMatches(img, 0x9000), "C3 a mulss whose operand is not 0.5 is refused");
        const float half = 0.5f;
        std::memcpy(s.base + 0x1A000, &half, 4);
    }
    {
        uint8_t keep = s.base[0x9000 + 35];
        s.base[0x9000 + 35] = 0x90;   // no mulss at all in the window
        check(!tc::calleeMatches(img, 0x9000), "C3 no mulss in the window is refused");
        s.base[0x9000 + 35] = keep;
        check(!tc::calleeMatches(img, s.size() - 20), "C3 a head that runs past the image is refused, not read");
    }
    // The call-site chain.
    const uint32_t call = putSite(s, 0x5003, 0x9000, kRealGapAB, kRealGapDE);
    uint32_t callRva = 0, targetRva = 0;
    check(tc::matchSite(img, 0x5003, &callRva, &targetRva) && callRva == call && targetRva == 0x9000, "C3 the call-site chain is matched and the call's target read (rel32 backwards)");
    putSite(s, 0x6003, 0x9000, {}, {});
    check(tc::matchSite(img, 0x6003, &callRva, &targetRva) && targetRva == 0x9000, "C3 the interleaved instructions may be absent");
    putSite(s, 0x7003, 0x9000, std::vector<uint8_t>(16, 0x90), std::vector<uint8_t>(16, 0x90));
    check(tc::matchSite(img, 0x7003, &callRva, &targetRva) && targetRva == 0x9000, "C3 ...or sixteen bytes each");
    putSite(s, 0x7803, 0x9000, std::vector<uint8_t>(17, 0x90), kRealGapDE);
    check(!tc::matchSite(img, 0x7803, &callRva, &targetRva), "C3 seventeen bytes between A and B is not the signature");
    putSite(s, 0x7C03, 0x9000, kRealGapAB, std::vector<uint8_t>(17, 0x90));
    check(!tc::matchSite(img, 0x7C03, &callRva, &targetRva), "C3 seventeen bytes between D and the call is not the signature");
    {
        putSite(s, 0x8003, 0x9000, kRealGapAB, kRealGapDE);
        s.base[0x8003 + 5] = 0x71;   // r14+0x71
        check(!tc::matchSite(img, 0x8003, &callRva, &targetRva), "C3 a different offset in fragment A is not the signature");
        putSite(s, 0x8203, 0x9000, kRealGapAB, kRealGapDE);
        s.base[0x8203 + 6 + 7 + 5] = 0x91;   // r14+0x91 in B
        check(!tc::matchSite(img, 0x8203, &callRva, &targetRva), "C3 a different offset in fragment B is not the signature");
        putSite(s, 0x8403, 0x9000, kRealGapAB, kRealGapDE);
        s.base[0x8403 + 6 + 7 + 9 + 5] = 0x30;   // [rsp+30h] in C
        check(!tc::matchSite(img, 0x8403, &callRva, &targetRva), "C3 a different stack slot in fragment C is not the signature");
        putSite(s, 0x8603, 0x9000, kRealGapAB, kRealGapDE);
        s.base[0x8603 + 6 + 7 + 9 + 6 + 5] = 0x28;   // [rsp+28h] in D (the slots swapped)
        check(!tc::matchSite(img, 0x8603, &callRva, &targetRva), "C3 the two stack slots swapped is not the signature");
    }
    {   // the scan: counts, and a site cut off by the end of the range is not a site
        tc::SiteMatch found[4];
        check(tc::scanSites(img, 0x1000, 0x20000, found, 4) == 3 && found[0].siteRva == 0x5003 && found[0].callRva == call && found[0].targetRva == 0x9000 && found[1].siteRva == 0x6003 &&
                  found[2].siteRva == 0x7003,
              "C3 the scan finds exactly the three whole sites, in order, with their calls and targets");
        check(tc::scanSites(img, 0x5004, 0x1000, found, 4) == 0, "C3 a range that starts inside the signature finds none");
        check(tc::scanSites(img, 0x5003, 6, found, 4) == 1 && tc::scanSites(img, 0x1000, 0x5003 + 6 - 0x1000, found, 4) == 1 && tc::scanSites(img, 0x1000, 0x5003 + 5 - 0x1000, found, 4) == 0,
              "C3 a fragment A that ends exactly at the end of the range is inside it; one byte less is not");
        check(tc::scanSites(img, 0x1000, 0x5003 + 20 - 0x1000, found, 4) == 1, "C3 a signature that starts inside the range is counted, those that start past its end are not");
        check(tc::scanSites(img, 0x1000, 0x20000, found, 1) == 3 && found[0].siteRva == 0x5003, "C3 the count is of all, the listing capped");
        check(tc::scanSites(img, 0x30000, 0x20000, found, 4) == 0, "C3 a range outside the image is empty");
    }
}

static void caseLocate() {
    tc::Located loc;
    {   // the known build
        Synth s(tc::kExpectedTimestamp, tc::kExpectedImageSize, 0x1000, 0x4400000);
        buildKnown(s);
        check(tc::locate(s.base, 0, &loc) && loc.byStamp && loc.targetRva == 0x443E800 && loc.callRva == 0x4456007 && loc.stamp == 1788384820u,
              "C4 a module with build 332841's stamp and size, the builder's head and the call site is found at 0x443E800");
        check(std::string(loc.how) == "PE stamp 1788384820 and image size 104894464 are build 332841's; the callee's head and the call site at exe+0x4455FDD verified", "C4 ...saying how");
        s.base[0x443E800 + 5] ^= 1;
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why).rfind("the PE stamp 1788384820 and image size 104894464 are build 332841's, but the head at exe+0x443E800 is not", 0) == 0,
              "C4 the right stamp over a wrong head is inert, with the scan not tried");
        s.base[0x443E800 + 5] ^= 1;
        s.base[0x4455FDD + 5] ^= 1;
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why).rfind("the PE stamp 1788384820 and image size 104894464 are build 332841's, but the call site at exe+0x4455FDD is not the one read", 0) == 0,
              "C4 ...a wrong call site likewise");
        s.base[0x4455FDD + 5] ^= 1;
        {   // the call goes somewhere else
            const int32_t rel = int32_t(0x443E900 - (0x4456007 + 5));
            std::memcpy(s.base + 0x4456008, &rel, 4);
            check(!tc::locate(s.base, 0, &loc) && std::string(loc.why).find("the call site at exe+0x4455FDD is not the one read") != std::string::npos, "C4 ...and a call to another function");
            const int32_t back = int32_t(0x443E800 - (0x4456007 + 5));
            std::memcpy(s.base + 0x4456008, &back, 4);
        }
        {
            const float notHalf = 2.0f;
            std::memcpy(s.base + 0x4DDF878, &notHalf, 4);
            check(!tc::locate(s.base, 0, &loc), "C4 the head right and its constant not 0.5 is inert");
            const float half = 0.5f;
            std::memcpy(s.base + 0x4DDF878, &half, 4);
        }
        check(tc::locate(s.base, 0, &loc), "C4 the control, restored");
        s.commit(0x1000, 0x1000, PAGE_READWRITE);                 // a code section small enough to be all committed
        s.put32(0x80 + 24 + 240 + 8, 0x1000);
        s.put32(0x80 + 8, tc::kExpectedTimestamp + 1);
        check(!tc::locate(s.base, 0, &loc) && loc.byStamp == false && std::string(loc.why).find("is not build 332841's") != std::string::npos, "C4 a stamp one off leaves the known address and scans");
        s.put32(0x80 + 8, tc::kExpectedTimestamp);
        s.put32(0x80 + 0x50, tc::kExpectedImageSize + 0x1000);
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why).find("is not build 332841's") != std::string::npos, "C4 ...and so does an image size one page off");
    }
    {   // not a PE
        Synth s(1u, 0x40000u, 0x1000, 0x20000);
        s.put32(0x3C, 0x10);
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why) == "the PE header offset is implausible", "C4 a header offset that cannot be a PE's is inert");
        s.put32(0x3C, 0x80);
        s.base[0x80] = 'X';
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why) == "the module is not a PE32+ image", "C4 no PE signature is inert");
        check(!tc::locate(nullptr, 0, &loc) && std::string(loc.why) == "the game's module could not be resolved", "C4 no module is inert");
    }
    // Another build: the signature scan.
    const auto scanImage = [](Synth& s) {
        s.commit(0x1000, 0x20000, PAGE_EXECUTE_READWRITE);
        s.fill(0x1000, 0x20000);
        putCallee(s, 0x9000, 0x1A000);
    };
    {
        Synth s(1234567u, 0x40000u, 0x1000, 0x20000);
        scanImage(s);
        const uint32_t call = putSite(s, 0x5003, 0x9000, std::vector<uint8_t>(3, 0x90), std::vector<uint8_t>(11, 0x90));
        check(tc::locate(s.base, 0, &loc) && !loc.byStamp && loc.targetRva == 0x9000 && loc.callRva == call, "C4 another build with one call site and a callee of the same head is found by scanning");
        char want[200];
        std::snprintf(want, sizeof(want), "found by signature scan: the one call site at exe+0x%X reaches it; PE stamp 1234567 is not build 332841's", call);
        check(std::string(loc.how) == want, "C4 ...saying how");
        putSite(s, 0xB021, 0x9000, kRealGapAB, kRealGapDE);
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why) == "the PE stamp 1234567 is not build 332841's and the signature scan of the 128 KB code section found more than one call site (ambiguous)",
              "C4 two call sites are ambiguous and inert");
    }
    {
        Synth s(1234567u, 0x40000u, 0x1000, 0x20000);
        scanImage(s);
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why) == "the PE stamp 1234567 is not build 332841's and the signature scan of the 128 KB code section found no call site", "C4 no call site is inert");
        const uint32_t call = putSite(s, 0x5003, 0x9000, kRealGapAB, kRealGapDE);
        s.base[0x9000 + 20] ^= 1;
        char want[300];
        std::snprintf(want, sizeof(want), "the PE stamp 1234567 is not build 332841's; the one call site at exe+0x%X reaches exe+0x9000, whose head is not the cull-plane builder's", call);
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why) == want, "C4 one call site whose callee has another head is inert");
        s.base[0x9000 + 20] ^= 1;
        putCallee(s, 0x9000, 0x1A000, 0.25f);
        check(!tc::locate(s.base, 0, &loc), "C4 ...and one whose constant is not 0.5");
    }
    {   // the .text section is the one scanned (a named section wins), and a module with none is inert
        Synth s(1234567u, 0x40000u, 0x1000, 0x20000);
        scanImage(s);
        putSite(s, 0x5003, 0x9000, kRealGapAB, kRealGapDE);
        uint8_t* section = s.base + 0x80 + 24 + 240;
        std::memcpy(section, ".rdata\0\0", 8);
        const uint32_t noFlags = 0x40000040u;
        std::memcpy(section + 36, &noFlags, 4);
        check(!tc::locate(s.base, 0, &loc) && std::string(loc.why).find("no code section could be read") != std::string::npos, "C4 a module with no code section is inert");
        s.put16(0x80 + 6, 0);
        check(!tc::locate(s.base, 0, &loc), "C4 ...and one with no sections at all");
    }
}

// ---- the production hook -----------------------------------------------------------------------------------------------------------------------------------------
using BuilderFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, float, float);
struct Record {
    float a5, a6;
    uint64_t rcx, rdx, r9;
};
struct Seen {
    float a5, a6;
    uint64_t rcx, rdx, r9;
    uintptr_t ret;
    bool recordedAtR8;
};
static Seen callBuilder(uint8_t* fn, float vfov, float aspect) {
    Record rec{};
    rec.rcx = 0xDEAD;   // overwritten by the synthetic body when r8 is the record
    const uintptr_t ret = reinterpret_cast<BuilderFn>(fn)(0x1111111122222222ull, 0x3333333344444444ull, reinterpret_cast<uintptr_t>(&rec), 0x5555555566666666ull, vfov, aspect);
    return Seen{rec.a5, rec.a6, rec.rcx, rec.rdx, rec.r9, ret, rec.rcx != 0xDEAD};
}
static std::vector<std::string> lines() {
    std::vector<std::string> out;
    for (size_t i = 0; i < edvr::terrainCullCoverTestLineCount(); ++i) out.emplace_back(edvr::terrainCullCoverTestLine(i));
    return out;
}
static size_t countWith(const std::vector<std::string>& v, const char* needle) {
    size_t n = 0;
    for (const auto& l : v) n += l.find(needle) != std::string::npos ? 1u : 0u;
    return n;
}
static void setKey(const char* value) { edvr::Config::get().set("advanced.terrain_cull_cover", value); }

static void caseHook() {
    using namespace edvr;
    setKey("on");
    Synth s(tc::kExpectedTimestamp, tc::kExpectedImageSize, 0x1000, 0x4400000);
    buildKnown(s);
    uint8_t* fn = s.base + tc::kTargetRva;
    const uint64_t kRcx = 0x1111111122222222ull, kRdx = 0x3333333344444444ull, kR9 = 0x5555555566666666ull;

    // the control: the synthetic builder hands back what it was given
    {
        const Seen c = callBuilder(fn, 1.6f, 1.03f);
        check(c.a5 == 1.6f && c.a6 == 1.03f && c.rcx == kRcx && c.rdx == kRdx && c.r9 == kR9 && c.ret == kRdx && c.recordedAtR8, "C5.a before any hook the synthetic builder records its six arguments and returns rdx");
    }
    float cropped[2][4], quest[2][4];
    kCropped.frusta(cropped);
    kQuest3.frusta(quest);
    uint8_t before[16];
    std::memcpy(before, fn, 16);

    // a flat session: inert, nothing patched
    terrainCullCoverTestSetImage(s.base);
    terrainCullCoverInstall(true);
    check(lines().size() == 1 && lines()[0] == "terrain cull cover: inert: the flat profile has no stereo frustum to cover; nothing was patched" && std::memcmp(before, fn, 16) == 0 &&
              !terrainCullCoverTestState().installed,
          "C5.b a flat session says it is inert in one line and patches nothing");
    terrainCullCoverInstall(false);
    check(lines().size() == 1, "C5.c ...and a second install attempt in the same session does nothing (once, ever)");
    terrainCullCoverPoll(1000);
    check(lines().size() == 1, "C5.d a hook that is not live says nothing in the census");

    // the VR session: the hook goes in
    terrainCullCoverTestReset();
    terrainCullCoverTestSetImage(s.base);
    terrainCullCoverInstall(false);
    check(lines().size() == 1 && lines()[0] == "terrain cull cover: hook live at exe+0x443E800 (PE stamp 1788384820 and image size 104894464 are build 332841's; the callee's head and the call site at exe+0x4455FDD verified)" &&
              terrainCullCoverTestState().installed && terrainCullCoverTestState().byStamp && std::memcmp(before, fn, 16) != 0 && fn[0] == 0xE9,
          "C5.e the known build: one line, 'hook live at exe+0x443E800', the entry patched with a jump");

    // no tangents known yet (before the first frame): through, bit for bit
    {
        const Seen u = callBuilder(fn, kCropped.vfov(), kCropped.aspect());
        check(sameBits(u.a5, kCropped.vfov()) && sameBits(u.a6, kCropped.aspect()) && u.rcx == kRcx && u.rdx == kRdx && u.r9 == kR9 && u.ret == kRdx && u.recordedAtR8,
              "C5.f before any frame has said the frusta, the builder is handed its own arguments");
    }
    // the frusta arrive; the cropped Crystal Super's aspect is widened and nothing else changes
    terrainCullCoverNoteFrusta(cropped);
    {
        const Seen w = callBuilder(fn, kCropped.vfov(), kCropped.aspect());
        check(sameBits(w.a5, kCropped.vfov()) && closeTo(w.a6, 1.218595, 2e-5) && w.a6 > kCropped.aspect(), "C5.g cropped Crystal Super: argument 6 (the aspect) is widened to 1.2186, argument 5 (the vFOV) is not");
        check(w.rcx == kRcx && w.rdx == kRdx && w.r9 == kR9 && w.ret == kRdx && w.recordedAtR8, "C5.h ...and the four register arguments reach the original unchanged, r8 is the record it was given, and its return value is the caller's");
    }
    terrainCullCoverNoteFrusta(quest);
    {
        const Seen w = callBuilder(fn, kQuest3.vfov(), kQuest3.aspect());
        check(closeTo(deg(w.a5), 110.0020, 5e-4) && closeTo(w.a6, 0.963731, 2e-5) && w.rcx == kRcx && w.rdx == kRdx && w.r9 == kR9, "C5.i Quest 3: both arguments widened (vFOV 110.00, aspect 0.9637), the others as given");
    }
    // a frame that says nothing usable leaves the last pair
    {
        float bad[2][4];
        kCropped.frusta(bad);
        bad[0][1] = std::numeric_limits<float>::quiet_NaN();
        terrainCullCoverNoteFrusta(bad);
        const Seen w = callBuilder(fn, kQuest3.vfov(), kQuest3.aspect());
        check(closeTo(deg(w.a5), 110.0020, 5e-4), "C5.j a frame with a NaN tangent leaves the last good pair in force");
        terrainCullCoverNoteFrusta(nullptr);
    }
    // the first four calls, the census
    {
        const std::vector<std::string> v = lines();
        check(countWith(v, "terrain cull cover: call ") == 4, "C5.k only the first four calls of the session are said one by one");
        check(v.size() >= 5 && v[1] == "terrain cull cover: call 1 of 4: in aspect 1.0344 vfov 91.65 deg -> out aspect 1.0344 vfov 91.65 deg (unchanged); T=n/a H=n/a; mode on",
              "C5.l call 1, before any frame: in and out equal, tangents unknown");
        check(v[2] == "terrain cull cover: call 2 of 4: in aspect 1.0344 vfov 91.65 deg -> out aspect 1.2186 vfov 91.65 deg (widened); T=1.0293 H=1.2543; mode on", "C5.m call 2, cropped Crystal Super: the exact line");
        check(v[3] == "terrain cull cover: call 3 of 4: in aspect 0.9412 vfov 99.00 deg -> out aspect 0.9637 vfov 110.00 deg (widened); T=1.4282 H=1.3764; mode on", "C5.n call 3, Quest 3: the exact line");
    }
    // the poll: the key, the window
    terrainCullCoverPoll(10000);
    {
        const std::vector<std::string> v = lines();
        check(v.back() == "terrain cull cover: on (the terrain cull frustum is widened to the largest half-angles of the frustum handed to the game, never narrowed)",
              "C5.o the first poll says the mode");
    }
    {
        for (int i = 0; i < 50; ++i) callBuilder(fn, kQuest3.vfov(), kQuest3.aspect());
        const size_t n = lines().size();
        terrainCullCoverPoll(40000);
        check(lines().size() == n, "C5.p inside the window the poll says nothing");
        terrainCullCoverPoll(69999);
        check(lines().size() == n, "C5.q ...not at 59.999 s");
        terrainCullCoverPoll(70000);
        check(lines().size() == n + 1 && lines().back() == "terrain cull cover: 60 s: mode on calls 54 widened 53; last in aspect 0.9412 vfov 99.00 deg -> out aspect 0.9637 vfov 110.00 deg; T=1.4282 H=1.3764",
              "C5.r at 60 s: the window line, exact (54 calls: 4 before the poll started and 50 after, the first of them unwidened for want of tangents)");
        terrainCullCoverPoll(130000);
        check(lines().back() == "terrain cull cover: 60 s: mode on calls 0 widened 0; last in aspect 0.9412 vfov 99.00 deg -> out aspect 0.9637 vfov 110.00 deg; T=1.4282 H=1.3764",
              "C5.s the next window, with no calls, is written all the same (zeros included)");
    }
    // the key off: written live, within a poll; the window closes at the change; the builder is handed its own arguments
    {
        for (int i = 0; i < 5; ++i) callBuilder(fn, kQuest3.vfov(), kQuest3.aspect());
        setKey("off");
        const size_t n = lines().size();
        terrainCullCoverPoll(150000);
        const std::vector<std::string> v = lines();
        check(v.size() == n + 2 && v[n] == "terrain cull cover: 20 s: mode on calls 5 widened 5; last in aspect 0.9412 vfov 99.00 deg -> out aspect 0.9637 vfov 110.00 deg; T=1.4282 H=1.3764" &&
                  v[n + 1] == "terrain cull cover: off (the game's own centred terrain cull frustum, as shipped)",
              "C5.t the key off: the open window is closed at the change with its own mode and seconds, then the change is said");
        const Seen o = callBuilder(fn, kQuest3.vfov(), kQuest3.aspect());
        check(sameBits(o.a5, kQuest3.vfov()) && sameBits(o.a6, kQuest3.aspect()) && o.rcx == kRcx && o.rdx == kRdx && o.r9 == kR9, "C5.u with the key off the builder is handed exactly its own arguments");
        setKey("on");
        terrainCullCoverPoll(151000);
        const Seen w = callBuilder(fn, kQuest3.vfov(), kQuest3.aspect());
        check(closeTo(deg(w.a5), 110.0020, 5e-4), "C5.v ...and on again, a second later, it widens again");
        setKey("OFF");
        terrainCullCoverPoll(152000);
        check(sameBits(callBuilder(fn, kQuest3.vfov(), kQuest3.aspect()).a6, kQuest3.aspect()), "C5.w the key is read like the project's others (any case)");
        setKey("on");
        terrainCullCoverPoll(153000);
    }
    // the counts and the shutdown
    {
        check(terrainCullCoverTestState().calls == 62 && terrainCullCoverTestState().widened == 59 && terrainCullCoverTestState().callLines == 4, "C5.x the session's calls and widenings are counted (62 calls, 59 widened: not the one with no tangents and not the two with the key off)");
        terrainCullCoverPoll(154000);
        const size_t n = lines().size();
        terrainCullCoverShutdown();
        check(lines().size() == n + 1 && lines().back().find("terrain cull cover: 1 s: mode on calls ") == 0, "C5.y a teardown writes the open window, as long as the poll saw it run");
        const size_t m = lines().size();
        terrainCullCoverShutdown();
        check(lines().size() == m, "C5.y2 ...and a second teardown, with no time passed, writes nothing");
    }
    terrainCullCoverTestReset();
    check(std::memcmp(before, fn, 16) == 0, "C5.z a reset puts the synthetic function's bytes back");
    {
        const Seen c = callBuilder(fn, 1.6f, 1.03f);
        check(c.a5 == 1.6f && c.a6 == 1.03f, "C5.za ...and it works as it did");
    }

    // refusals
    {
        s.base[0x443E800 + 7] ^= 1;
        terrainCullCoverTestSetImage(s.base);
        terrainCullCoverInstall(false);
        check(lines().size() == 1 && lines()[0].rfind("terrain cull cover: inert: the PE stamp 1788384820 and image size 104894464 are build 332841's, but the head at exe+0x443E800 is not", 0) == 0 &&
                  !terrainCullCoverTestState().installed && fn[0] == 0x48,
              "C5.zb a head that is not the one read: one 'inert' line naming why, nothing patched");
        s.base[0x443E800 + 7] ^= 1;
        terrainCullCoverTestReset();
    }
}

static void caseHookScanned() {
    using namespace edvr;
    setKey("on");
    Synth s(777777u, 0x40000u, 0x1000, 0x20000);
    s.commit(0x1000, 0x20000, PAGE_EXECUTE_READWRITE);
    s.fill(0x1000, 0x20000);
    putCallee(s, 0x9000, 0x1A000);
    const uint32_t call = putSite(s, 0x5003, 0x9000, std::vector<uint8_t>(3, 0x90), std::vector<uint8_t>(11, 0x90));
    uint8_t* fn = s.base + 0x9000;
    terrainCullCoverTestSetImage(s.base);
    terrainCullCoverInstall(false);
    char want[300];
    std::snprintf(want, sizeof(want), "terrain cull cover: hook live at exe+0x9000 (found by signature scan: the one call site at exe+0x%X reaches it; PE stamp 777777 is not build 332841's)", call);
    check(lines().size() == 1 && lines()[0] == want && terrainCullCoverTestState().installed && !terrainCullCoverTestState().byStamp, "C5.zc another build with one call site: the hook goes in where the scan found it, and the line says how");
    float f[2][4];
    kQuest3.frusta(f);
    terrainCullCoverNoteFrusta(f);
    const Seen w = callBuilder(fn, kQuest3.vfov(), kQuest3.aspect());
    check(closeTo(deg(w.a5), 110.0020, 5e-4) && w.rcx == 0x1111111122222222ull && w.r9 == 0x5555555566666666ull && w.recordedAtR8, "C5.zd ...and it widens the same way");
    terrainCullCoverTestReset();
    // two sites: inert
    putSite(s, 0xB021, 0x9000, kRealGapAB, kRealGapDE);
    terrainCullCoverTestSetImage(s.base);
    terrainCullCoverInstall(false);
    check(lines().size() == 1 && lines()[0].find("terrain cull cover: inert: the PE stamp 777777 is not build 332841's and the signature scan") == 0 && lines()[0].find("ambiguous") != std::string::npos &&
              !terrainCullCoverTestState().installed && fn[0] == 0x48,
          "C5.ze two call sites: inert, one line, nothing patched");
    terrainCullCoverTestReset();
}

// ---- the fixture: the production hook's lines for a scripted session -----------------------------------------------------------------------------------------------------
// Ten minutes of a flight on the cropped Crystal Super: the builder is called twice a frame at 90 Hz (180 a second), the key is on for the first 150 s, off for 150 s, on again.
// Every line is stamped with the scripted clock, 10:00:00.000 plus the time.
static std::string clockPrefix(uint64_t ms) {
    char b[40];
    const uint64_t t = 36000000ull + ms;
    std::snprintf(b, sizeof(b), "[%02u:%02u:%02u.%03u] ", unsigned(t / 3600000 % 24), unsigned(t / 60000 % 60), unsigned(t / 1000 % 60), unsigned(t % 1000));
    return b;
}
static std::string fixtureLog() {
    using namespace edvr;
    setKey("on");
    Synth s(tc::kExpectedTimestamp, tc::kExpectedImageSize, 0x1000, 0x4400000);
    buildKnown(s);
    uint8_t* fn = s.base + tc::kTargetRva;
    terrainCullCoverTestReset();
    terrainCullCoverTestSetImage(s.base);
    std::string out;
    size_t taken = 0;
    const auto drain = [&](uint64_t ms) {
        const std::vector<std::string> v = lines();
        for (; taken < v.size(); ++taken) out += clockPrefix(ms) + v[taken] + "\n";
    };
    terrainCullCoverInstall(false);
    drain(2000);
    float f[2][4];
    kCropped.frusta(f);
    terrainCullCoverNoteFrusta(f);
    uint64_t now = 3000;
    terrainCullCoverPoll(now);
    drain(now);
    for (uint64_t second = 1; second <= 600; ++second) {
        now = 3000 + second * 1000;
        if (second == 150) setKey("off");
        if (second == 300) setKey("on");
        for (int i = 0; i < 180; ++i) callBuilder(fn, kCropped.vfov(), kCropped.aspect());
        terrainCullCoverPoll(now);
        drain(now);
    }
    terrainCullCoverShutdown();
    drain(now);
    terrainCullCoverTestReset();
    setKey("on");
    return out;
}

// ---- the wiring, as text -----------------------------------------------------------------------------------------------------------------------------------------------
static std::string slurp(const std::string& path) {
    std::string out;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
        char buffer[8192];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, got);
        std::fclose(f);
    }
    std::string clean;
    for (char c : out) if (c != '\r') clean += c;
    return clean;
}
static size_t occurrences(const std::string& text, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
    return n;
}
// How many times `needle` appears as code: at the start of a line (after blanks), not inside a comment.
static size_t occurrencesLive(const std::string& text, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        size_t back = at;
        while (back > 0 && (text[back - 1] == ' ' || text[back - 1] == '\t')) --back;
        if (back == 0 || text[back - 1] == '\n') ++n;
        at += needle.size();
    }
    return n;
}
static bool precedes(const std::string& text, const std::string& a, const std::string& b) {
    const size_t i = text.find(a), j = text.find(b);
    return i != std::string::npos && j != std::string::npos && i < j;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("terrain_cover_test: dry-run (no module patched, no file)");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--print-fixture") {
        const std::string text = fixtureLog();
        _setmode(_fileno(stdout), _O_BINARY);   // LF only: tools\terrain_cover_fixture.log is this, byte for byte
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    if (argc != 3 || std::string(argv[1]) != "--self-test") return 2;
    const std::string root = argv[2];

    caseRule();
    caseTangents();
    caseSignature();
    caseLocate();
    caseHook();
    caseHookScanned();

    // C6 the fixture
    {
        const std::string built = fixtureLog();
        const std::string fixture = slurp(root + "\\tools\\terrain_cover_fixture.log");
        check(!fixture.empty(), "C6 tools\\terrain_cover_fixture.log is readable");
        check(fixture == built, "C6 the log tool's fixture file is what the production hook writes for the scripted session, byte for byte (python tools\\edvr_log.py --tally cover reads it; regenerate with --print-fixture)");
        check(occurrences(built, "hook live at exe+0x443E800") == 1 && occurrences(built, "call 4 of 4") == 1 && occurrences(built, "call 5 of 4") == 0 && occurrences(built, ": mode on calls ") >= 6 &&
                  occurrences(built, ": mode off calls ") >= 3 && occurrences(built, "terrain cull cover: off (") == 1 && occurrences(built, "terrain cull cover: on (") == 2,
              "C6 ...and it carries the live line, the first four calls, windows in both modes and the two changes");
    }

    // C7 the wiring
    {
        const std::string device = slurp(root + "\\src\\d3d11\\device_hook.cpp");
        const std::string temporal = slurp(root + "\\src\\d3d11\\native_temporal.cpp");
        const std::string hook = slurp(root + "\\src\\d3d11\\terrain_cull_cover_hook.cpp");
        const std::string build = slurp(root + "\\build.bat");
        const std::string ini = slurp(root + "\\edvr.ini");
        check(occurrencesLive(device, "terrainCullCoverInstall(runtimeFlatProfile());") == 1 && precedes(device, "vertexResyncInstall();", "terrainCullCoverInstall(runtimeFlatProfile());"),
              "C7.a device_hook.cpp installs the cover once, with the profile, in the startup block");
        check(occurrencesLive(device, "terrainCullCoverPoll(stampMs());") == 1 && precedes(device, "vScreenRefreshConfig();", "terrainCullCoverPoll(stampMs());"), "C7.b ...polls it once a second after the config reload");
        check(occurrencesLive(device, "terrainCullCoverShutdown();") == 1, "C7.c ...and says its open window at a teardown");
        check(occurrencesLive(temporal, "edvr::terrainCullCoverNoteFrusta(s->frusta);") == 1 &&
                  precedes(temporal, "std::memcpy(s->frusta,f->frusta,sizeof(s->frusta));", "edvr::terrainCullCoverNoteFrusta(s->frusta);") &&
                  precedes(temporal, "edvr::terrainCullCoverNoteFrusta(s->frusta);", "g_vertical.store("),
              "C7.d native_temporal.cpp's begin() tells the cover the frusta right after it takes them");
        check(occurrences(hook, "return forward(a, b, c, d, r.vfov, r.aspect);") == 1 && occurrences(hook, "BuilderFn = uintptr_t (__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, float, float);") == 1,
              "C7.e the detour forwards the four register arguments untouched and only the two floats it computed");
        check(occurrences(hook, "getBool(\"advanced.terrain_cull_cover\", true)") == 2, "C7.f the key is read (default on) at install and at each poll, and nowhere in the detour");
        check(occurrences(build, "\"src\\d3d11\\terrain_cull_cover_hook.cpp\"") >= 1 && occurrences(build, ":rig_terrain_cover_test") == 1, "C7.g build.bat compiles the hook into the DLL and has this rig");
        check(occurrences(ini, "#terrain_cull_cover = on") == 1, "C7.h edvr.ini documents the key");
    }

    std::printf("terrain_cover_test: %u checks, 0 failures\n", checks);
    return 0;
}
