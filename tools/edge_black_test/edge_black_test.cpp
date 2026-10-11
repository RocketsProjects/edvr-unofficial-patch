// The edge-black watch (src/d3d11/edge_black.cpp, edge_black_core.h, edge_black_shader.h; docs\terrain-culling.md, 2026-10-10).
//
// The pure part, case by case: the thresholds, the region labels per eye, the clustering, the transient rule over a history of frames (one frame lit-black-lit,
// two frames lit-black-black-lit, a longer black, a dim neighbour, a frame missing from the sequence), black-start, the window, the event cap, the exact lines.
// The GPU part on WARP with the production compute shader (the build's own bytecode): a scripted flash in a synthetic eye texture comes out as exactly the
// event it should, at the cells it should, with a region of the texture, a mirrored eye, a resize in the middle of a flash, a format and a bind the watch
// refuses. tools\edge_black_fixture.log is held to what the core writes for a scripted five-minute run (python tools\edvr_log.py --tally edge reads it).
//
//   edge_black_test --self-test | --dry-run | --print-fixture
#include "../../src/d3d11/edge_black.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <io.h>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>

namespace edvr {
std::vector<std::string> testLog;
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
int64_t qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t.QuadPart; }
void Log::note(const char* fmt, ...) {
    char line[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    testLog.emplace_back(line);
}
// shader_swap.cpp's production job, without its guard and budget: make the compute shader from the build's bytecode.
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx, const void* bytecode, size_t length, const char*, const char*) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11ComputeShader* cs = nullptr;
    if (dev) { dev->CreateComputeShader(bytecode, length, nullptr, &cs); dev->Release(); }
    return cs;
}
}  // namespace edvr

using namespace edvr;
using namespace edvr::edgeblack;

static unsigned checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", what); std::exit(1); }
}
static void hr(HRESULT r) { check(SUCCEEDED(r), "a D3D operation"); }

struct Lines {
    std::vector<std::string> v;
    void operator()(const char* s) { v.emplace_back(s); }
    size_t count(const std::string& prefix) const { size_t n = 0; for (const auto& l : v) if (l.rfind(prefix, 0) == 0) ++n; return n; }
    const std::string* find(const std::string& text) const { for (const auto& l : v) if (l.find(text) != std::string::npos) return &l; return nullptr; }
};

// A 128 x 128 grid of one value, with rectangles of another.
struct Grid {
    std::vector<float> d;   // on the heap: a Grid is 64 KB and a statement makes five of them, on a stack that is 1 MB
    explicit Grid(float fill = 0.5f) : d(kPoints, fill) {}
    float* v() { return d.data(); }
    const float* v() const { return d.data(); }
    Grid& rect(int x0, int y0, int w, int h, float value) {
        for (int y = y0; y < y0 + h && y < kGrid; ++y)
            for (int x = x0; x < x0 + w && x < kGrid; ++x) d[y * kGrid + x] = value;
        return *this;
    }
    Grid& point(int x, int y, float value) { d[y * kGrid + x] = value; return *this; }
};

static FrameInfo frame(uint64_t seq) {
    FrameInfo f;
    f.seq = seq;
    f.wall = {10, 20, 30, static_cast<unsigned>(seq % 1000)};
    return f;
}

// -------------------------------------------------------------------------------------------------------------------------------------------------
// The scripted five-minute run (tools\edge_black_fixture.log): 11 ms a frame, both eyes. Every 400th frame from the 200th a 10 x 12 black square in eye 0's outer-bottom
// corner for a frame; from the 500th, every 997th frame an 8 x 8 square in eye 1's inner-top for two frames; every 61st frame two stray black points in eye 1 (under the
// cluster size: counted as transient points, no event); from frame 12000 a black band enters from the top of both eyes over 600 frames, stays 100 and goes (a horizon:
// black-start points and no transients); the head speed is known except on every 90th frame.
static std::string fixtureLog() {
    Watch w;
    Lines lines;
    char start[640];
    formatStart(start, "R8G8B8A8_TYPELESS", "code / 255");
    lines(start);
    uint64_t seq = 100, nowMs = 5000;
    for (int f = 0; f < 27000; ++f) {
        ++seq;
        nowMs += 11;
        for (int eye = 0; eye < 2; ++eye) {
            Grid g(0.5f);
            if (eye == 0 && f % 400 == 200) g.rect(0, 116, 10, 12, 0.0f);
            if (eye == 1 && f >= 500 && ((f - 500) % 997 == 0 || (f - 500) % 997 == 1)) g.rect(8, 16, 8, 8, 0.0f);
            if (eye == 1 && f % 61 == 5) { g.point(30, 30, 0.0f); g.point(31, 30, 0.0f); }
            if (f >= 12000 && f < 12700) g.rect(0, 0, kGrid, std::min(16 + (f - 12000) / 6, 80), 0.0f);
            FrameInfo info;
            info.seq = seq;
            const uint64_t t = 36000000ull + nowMs;
            info.wall = {static_cast<unsigned>(t / 3600000 % 24), static_cast<unsigned>(t / 60000 % 60), static_cast<unsigned>(t / 1000 % 60), static_cast<unsigned>(t % 1000)};
            info.headDegPerSec = (f % 90 == 0) ? std::numeric_limits<float>::quiet_NaN() : 20.0f + static_cast<float>(f % 90);
            w.ingest(eye, g.v(), info, nowMs, lines);
        }
    }
    w.close(nowMs, lines);
    std::string out;
    for (const auto& l : lines.v) out += "[00:00:00.000] " + l + "\n";
    return out;
}

// -------------------------------------------------------------------------------------------------------------------------------------------------
// The GPU half on WARP.
struct Gpu {
    Ptr<ID3D11Device> dev;
    Ptr<ID3D11DeviceContext> ctx;
    Ptr<ID3D11Query> query;
    void init() {
        D3D_FEATURE_LEVEL level{};
        hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx));
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        hr(dev->CreateQuery(&qd, &query));
    }
    void wait() {   // the GPU is idle: every copy made so far is readable without waiting
        ctx->End(query.Get());
        ctx->Flush();
        BOOL done = FALSE;
        while (ctx->GetData(query.Get(), &done, sizeof(done), 0) != S_OK || !done) Sleep(0);
    }
    Ptr<ID3D11Texture2D> texture(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
        Ptr<ID3D11Texture2D> t;
        hr(dev->CreateTexture2D(&td, nullptr, &t));
        return t;
    }
    // An 8-bit texture: every texel `value` (R = G = B) except the rectangles given as {x0, y0, x1, y1} (texels, half open), which are 0.
    void fill(ID3D11Texture2D* tex, UINT w, UINT h, uint8_t value, const std::vector<std::array<UINT, 4>>& blacks) {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, value);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        for (const auto& b : blacks)
            for (UINT y = b[1]; y < b[3]; ++y)
                for (UINT x = b[0]; x < b[2]; ++x) {
                    uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                    p[0] = p[1] = p[2] = 0;
                }
        ctx->UpdateSubresource(tex, 0, nullptr, px.data(), w * 4, 0);
    }
};

static void resetGpuState() {
    g_eye[0] = EyeGpu();
    g_eye[1] = EyeGpu();
    g_watch = Watch();
    g_started = false;
    g_closed = false;
    testLog.clear();
}

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// One eye's frame through the production entry point, then the GPU idle so the copy is readable at the next call.
static void sampleFrame(Gpu& gpu, ID3D11Texture2D* tex, int eye, uint64_t seq, const uint32_t region[4], bool flipU = false, bool flipV = false, const float* head = nullptr) {
    edgeBlackSample(gpu.dev.Get(), tex, eye, seq, region, flipU, flipV, head);
    gpu.wait();
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("edge_black_test: dry-run (no device, no files)");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--print-fixture") {
        const std::string text = fixtureLog();
        _setmode(_fileno(stdout), _O_BINARY);   // LF only: tools\edge_black_fixture.log is this, byte for byte
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    if (argc != 2 || std::string(argv[1]) != "--self-test") return 2;

    // ---- the thresholds ---------------------------------------------------------------------------------------------------------------------------
    check(classify(0.0f) == Black && classify(0.00392f) == Black && classify(1.0f / 255.0f) == Black && classify(-1.0f) == Black,
          "black: below 0.004, which takes an 8-bit code of 0 or 1 (1/255 = 0.00392)");
    check(classify(0.004f) == Dim && classify(2.0f / 255.0f) == Dim && classify(0.03f) == Dim && classify(7.0f / 255.0f) == Dim,
          "dim: from 0.004 up to and including 0.03 (code 2 to 7 of 255)");
    check(classify(0.0301f) == Lit && classify(8.0f / 255.0f) == Lit && classify(0.5f) == Lit && classify(40.0f) == Lit && classify(std::numeric_limits<float>::infinity()) == Lit,
          "lit: above 0.03 (code 8 of 255 and up; a float texture's HDR values too)");
    check(classify(std::numeric_limits<float>::quiet_NaN()) == Dim, "a NaN is dim: not a black to stand behind, not a lit one");

    // ---- the region labels, per eye ----------------------------------------------------------------------------------------------------------------
    check(regionOf(0, 0.07f, 0.88f) == 0 && regionOf(0, 0.07f, 0.50f) == 1 && regionOf(0, 0.07f, 0.10f) == 2,
          "eye 0 (outer is x < 0.5): the left edge is outer, bottom, middle, top");
    check(regionOf(1, 0.93f, 0.88f) == 0 && regionOf(1, 0.93f, 0.50f) == 1 && regionOf(1, 0.93f, 0.10f) == 2,
          "eye 1 (outer is x > 0.5): the right edge is outer");
    check(regionOf(0, 0.93f, 0.88f) == 3 && regionOf(0, 0.93f, 0.50f) == 4 && regionOf(0, 0.93f, 0.10f) == 5 && regionOf(1, 0.07f, 0.88f) == 3 && regionOf(1, 0.07f, 0.10f) == 5,
          "...and the other edge is inner, bottom, middle, top");
    check(regionOf(0, 0.50f, 0.50f) == 6 && regionOf(1, 0.41f, 0.40f) == 6 && regionOf(0, 0.59f, 0.60f) == 6, "the middle third within 0.1 of the midline is the centre");
    check(regionOf(0, 0.39f, 0.50f) == 1 && regionOf(0, 0.61f, 0.50f) == 4 && regionOf(0, 0.50f, 0.20f) == 5 && regionOf(1, 0.50f, 0.90f) == 3,
          "...but 0.11 from it in the middle third, or on the midline in the top or bottom third, is outer or inner");
    check(regionOf(0, 0.1f, 1.0f / 3.0f) == 1 && regionOf(0, 0.1f, 0.3333f) == 2 && regionOf(0, 0.1f, 2.0f / 3.0f) == 0 && regionOf(0, 0.1f, 0.6666f) == 1,
          "the thirds: exactly a third is the middle, exactly two thirds is the bottom");
    check(std::string(regionName(0)) == "outer-bottom" && std::string(regionName(1)) == "outer-middle" && std::string(regionName(2)) == "outer-top" &&
              std::string(regionName(3)) == "inner-bottom" && std::string(regionName(4)) == "inner-middle" && std::string(regionName(5)) == "inner-top" &&
              std::string(regionName(6)) == "centre" && kRegions == 7,
          "the labels are named as the lines print them");

    // ---- the clustering --------------------------------------------------------------------------------------------------------------------------
    {
        uint8_t f[kPoints];
        Cluster c;
        const auto clear = [&] { std::memset(f, 0, sizeof(f)); };
        const auto set = [&](int x, int y) { f[y * kGrid + x] = 1; };
        clear();
        check(findClusters(f, kMinCluster, &c) == 0 && c.points == 0, "no point set: no cluster");
        set(10, 10);
        check(findClusters(f, kMinCluster, &c) == 0 && c.points == 1, "one point: a cluster of one, not of three");
        set(11, 11);   // touching on a diagonal
        check(findClusters(f, kMinCluster, &c) == 0 && c.points == 2, "two points touching on a diagonal are one cluster of two (the 8-neighbourhood)");
        set(12, 11);
        check(findClusters(f, kMinCluster, &c) == 1 && c.points == 3 && c.minX == 10 && c.maxX == 12 && c.minY == 10 && c.maxY == 11 && !c.border,
              "a third touching point makes a cluster of three: its bounding box in cells");
        check(std::fabs(c.cx - 11.5f / static_cast<float>(kGrid)) < 1e-6f && std::fabs(c.cy - (10.5f + 11.5f + 11.5f) / 3.0f / static_cast<float>(kGrid)) < 1e-6f,
              "...and its centre, as normalised x, y");
        clear();
        set(10, 10); set(12, 10); set(14, 10);   // a gap of one between each: not touching
        check(findClusters(f, kMinCluster, &c) == 0 && c.points == 1, "three points with a gap between each do not touch");
        clear();
        for (int x = 0; x < 3; ++x) set(x, kGrid - 1);
        check(findClusters(f, kMinCluster, &c) == 1 && c.border && c.minY == kGrid - 1 && c.maxY == kGrid - 1, "a cluster on the image's edge says so");
        clear();
        for (int y = 20; y < 24; ++y) for (int x = 30; x < 34; ++x) set(x, y);   // 16
        set(5, 5); set(5, 6); set(6, 6);                                          // 3
        set(60, 60); set(61, 60);                                                 // 2
        check(findClusters(f, kMinCluster, &c) == 2 && c.points == 16 && c.minX == 30 && c.maxX == 33 && c.minY == 20 && c.maxY == 23,
              "separate clusters: two have three or more, the largest is reported");
        clear();
        set(0, 0); set(1, 0); set(2, 0); set(60, 60); set(61, 60); set(62, 60);
        check(findClusters(f, kMinCluster, &c) == 2 && c.points == 3 && c.minX == 0, "on a tie the first found, reading top to bottom, is the largest");
        clear();
        for (int i = 0; i < kPoints; ++i) f[i] = 1;
        check(findClusters(f, kMinCluster, &c) == 1 && c.points == kPoints && c.border && std::fabs(c.cx - 0.5f) < 1e-6f && std::fabs(c.cy - 0.5f) < 1e-6f,
              "the whole grid is one cluster of 128 x 128 = 16384, centred (no stack overflow)");
        clear();
        for (int i = 0; i < kGrid; ++i) { set(i, i); set(kGrid - 1 - i, i); }
        check(findClusters(f, kMinCluster, &c) == 1 && c.points == 2 * kGrid - 0 && c.border, "two diagonals crossing at the middle are one cluster");
    }

    // ---- the transient rule over a history --------------------------------------------------------------------------------------------------------
    {
        const auto run = [&](const std::vector<Grid>& frames, std::vector<EyeHistory::Verdict>* out, unsigned* dropped = nullptr, uint64_t firstSeq = 1) {
            EyeHistory h;
            out->clear();
            unsigned d = 0;
            uint64_t seq = firstSeq;
            for (const Grid& g : frames) {
                EyeHistory::Verdict v;
                d += h.push(g.v(), frame(seq++), &v);
                out->push_back(v);
            }
            if (dropped) *dropped = d;
        };
        std::vector<EyeHistory::Verdict> v;
        // Lit, black, lit, lit: the second frame is judged when the fourth arrives.
        run({Grid(), Grid().rect(10, 10, 4, 3, 0.0f), Grid(), Grid()}, &v);
        check(!v[0].evaluated && !v[1].evaluated && !v[2].evaluated && v[3].evaluated && v[3].info.seq == 2 && v[3].transient == 12 && v[3].blackStart == 0 && v[3].clusters == 1 &&
                  v[3].largest.points == 12 && v[3].largest.minX == 10 && v[3].largest.maxX == 13 && v[3].largest.minY == 10 && v[3].largest.maxY == 12,
              "LIT, BLACK, LIT: the black frame's 12 points are transient, judged when the frame two after it arrives (the first three pushes judge nothing)");
        // Lit, black, black, lit, lit: both black frames are transient.
        run({Grid(), Grid().rect(10, 10, 4, 3, 0.0f), Grid().rect(10, 10, 4, 3, 0.0f), Grid(), Grid()}, &v);
        check(v[3].evaluated && v[3].info.seq == 2 && v[3].transient == 12 && v[4].evaluated && v[4].info.seq == 3 && v[4].transient == 12 && v[3].blackStart == 0 && v[4].blackStart == 0,
              "LIT, BLACK, BLACK, LIT: a two-frame run is transient in both frames, and neither is a black-start");
        // Three black frames: a hole that stays.
        run({Grid(), Grid().rect(10, 10, 4, 3, 0.0f), Grid().rect(10, 10, 4, 3, 0.0f), Grid().rect(10, 10, 4, 3, 0.0f), Grid()}, &v);
        check(v[3].evaluated && v[3].transient == 0 && v[3].blackStart == 12 && v[3].clusters == 0 && v[4].evaluated && v[4].transient == 0 && v[4].blackStart == 0,
              "LIT, BLACK, BLACK, BLACK: not transient (a hole that stays); the frame it opened in counts 12 black-start points, the next frame none");
        // Black that never was lit before.
        run({Grid().rect(10, 10, 4, 3, 0.0f), Grid().rect(10, 10, 4, 3, 0.0f), Grid(), Grid()}, &v);
        check(v[3].evaluated && v[3].transient == 0 && v[3].blackStart == 0, "BLACK, BLACK, LIT: black with nothing lit before it is neither");
        // A dim point between.
        run({Grid(), Grid().rect(10, 10, 4, 3, 0.01f), Grid(), Grid()}, &v);
        check(v[3].transient == 0, "LIT, DIM, LIT: dim is not black");
        run({Grid(), Grid().rect(10, 10, 4, 3, 0.0f), Grid().rect(10, 10, 4, 3, 0.01f), Grid()}, &v);
        check(v[3].transient == 0, "LIT, BLACK, DIM, LIT: a dim frame inside the run is not a two-frame flash");
        run({Grid().rect(10, 10, 4, 3, 0.01f), Grid().rect(10, 10, 4, 3, 0.0f), Grid(), Grid()}, &v);
        check(v[3].transient == 0, "DIM, BLACK, LIT: what comes before must be lit");
        run({Grid(), Grid().rect(10, 10, 4, 3, 0.0f), Grid().rect(10, 10, 4, 3, 0.0f), Grid().rect(10, 10, 4, 3, 0.01f)}, &v);
        check(v[3].transient == 0 && v[3].blackStart == 12, "LIT, BLACK, BLACK, DIM: black that is not lit again is black-start");
        // Fewer than three points: counted, no event.
        run({Grid(), Grid().point(5, 5, 0.0f).point(6, 5, 0.0f), Grid(), Grid()}, &v);
        check(v[3].evaluated && v[3].transient == 2 && v[3].clusters == 0, "two touching transient points are counted and are not an event");
        run({Grid(), Grid().point(5, 5, 0.0f).point(9, 5, 0.0f).point(40, 40, 0.0f), Grid(), Grid()}, &v);
        check(v[3].transient == 3 && v[3].clusters == 0, "three transient points that do not touch are not an event");
        run({Grid(), Grid().point(5, 5, 0.0f).point(6, 6, 0.0f).point(7, 6, 0.0f), Grid(), Grid()}, &v);
        check(v[3].transient == 3 && v[3].clusters == 1 && v[3].largest.points == 3, "three that touch (one on a diagonal) are");
        // Points that flash at different times do not make a cluster.
        run({Grid(), Grid().point(5, 5, 0.0f).point(6, 5, 0.0f), Grid().point(7, 5, 0.0f), Grid(), Grid()}, &v);
        check(v[3].evaluated && v[3].transient == 2 && v[3].clusters == 0 && v[4].evaluated && v[4].transient == 1, "points black in different frames are judged in their own frames");
        // A frame missing from the sequence ends the history.
        {
            EyeHistory h;
            EyeHistory::Verdict r;
            unsigned dropped = 0;
            dropped += h.push(Grid().v(), frame(1), &r);
            dropped += h.push(Grid().rect(10, 10, 4, 3, 0.0f).v(), frame(2), &r);
            dropped += h.push(Grid().v(), frame(3), &r);
            check(dropped == 0 && h.held() == 3, "three consecutive frames are held");
            dropped += h.push(Grid().v(), frame(5), &r);   // 4 is missing
            check(dropped == 3 && h.held() == 1 && !r.evaluated, "FRAME 4 MISSING: the three frames held are dropped (counted lost), the history starts again at 5");
            dropped = h.push(Grid().v(), frame(6), &r);
            dropped += h.push(Grid().v(), frame(7), &r);
            check(dropped == 0 && !r.evaluated, "...and nothing is judged until four consecutive frames have come since");
            h.push(Grid().v(), frame(8), &r);
            check(r.evaluated && r.info.seq == 6 && r.transient == 0, "...then the first verdict is on frame 6");
            // lit, black, [gap], lit: a flash seen across a gap is not one.
            EyeHistory g;
            g.push(Grid().v(), frame(1), &r);
            g.push(Grid().rect(10, 10, 4, 3, 0.0f).v(), frame(2), &r);
            g.push(Grid().v(), frame(4), &r);
            g.push(Grid().v(), frame(5), &r);
            g.push(Grid().v(), frame(6), &r);
            g.push(Grid().v(), frame(7), &r);
            check(r.evaluated && r.transient == 0, "lit, black, [frame 3 missing], lit is not a flash: the gap ends the history");
            check(g.reset() == 2 && g.held() == 0, "a reset of a full history counts the two frames whose verdicts were still to come");
            EyeHistory h2;
            h2.push(Grid().v(), frame(1), &r);
            h2.push(Grid().v(), frame(2), &r);
            check(h2.reset() == 2, "...and of a shorter one every frame it held");
        }
        // A frame is judged once, in order.
        {
            EyeHistory h;
            EyeHistory::Verdict r;
            std::vector<uint64_t> judged;
            for (uint64_t s = 1; s <= 10; ++s) { h.push(Grid().v(), frame(s), &r); if (r.evaluated) judged.push_back(r.info.seq); }
            check(judged.size() == 7 && judged.front() == 2 && judged.back() == 8, "frames are judged once each, in order, two behind the newest, from the second");
        }
    }

    // ---- the head's speed ----------------------------------------------------------------------------------------------------------------------------
    {
        const float a[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const float angle = 1.0f * 3.14159265f / 180.0f;   // one degree about y
        const float b[12] = {std::cos(angle), 0, std::sin(angle), 0, 0, 1, 0, 0, -std::sin(angle), 0, std::cos(angle), 0};
        const float speed = headSpeedDegPerSec(a, b, 0.01);
        check(std::fabs(speed - 100.0f) < 0.05f, "one degree in 10 ms is 100 degrees a second");
        check(headSpeedDegPerSec(a, a, 0.011) == 0.0f, "no turn is 0");
        check(headSpeedDegPerSec(nullptr, b, 0.01) != headSpeedDegPerSec(nullptr, b, 0.01) && headSpeedDegPerSec(a, b, 0.0001) != headSpeedDegPerSec(a, b, 0.0001) &&
                  headSpeedDegPerSec(a, b, 0.6) != headSpeedDegPerSec(a, b, 0.6) && headSpeedDegPerSec(a, nullptr, 0.01) != headSpeedDegPerSec(a, nullptr, 0.01),
              "no previous pose, a gap of under half a millisecond or over half a second: NaN, unknown");
    }

    // ---- the lines -------------------------------------------------------------------------------------------------------------------------------------
    {
        FrameInfo info = frame(12345);
        info.wall = {9, 5, 7, 42};
        info.headDegPerSec = 61.4f;
        Cluster c;
        c.points = 25; c.minX = 0; c.maxX = 4; c.minY = 122; c.maxY = 127; c.cx = 0.0390625f; c.cy = 0.9453125f; c.border = true;
        char line[384];
        formatEvent(line, 0, info, c, 0, 1);
        check(std::string(line) == "edge black: event eye=0 frame=12345 t=09:05:07.042 points=25 cells=5x6 centre=0.04,0.95 region=outer-bottom border=1 head=61 deg/s",
              "the event line, exact");
        info.headDegPerSec = std::numeric_limits<float>::quiet_NaN();
        c.border = false;
        formatEvent(line, 1, info, c, 5, 2);
        check(std::string(line) == "edge black: event eye=1 frame=12345 t=09:05:07.042 points=25 cells=5x6 centre=0.04,0.95 region=inner-top border=0 "
                                   "head=unknown (no head pose known to the graphics half) clusters=2",
              "...with no head pose it says so, and a second cluster in the frame is named");
        char start[640];
        formatStart(start, "R8G8B8A8_TYPELESS", "code / 255");
        check(std::string(start) ==
                  "edge black: watching stage game (the texture Elite submits, before any EDVR pass): a 128x128 grid per eye each frame, the largest of R, G, B at each point "
                  "through a plain view of R8G8B8A8_TYPELESS (code / 255, the texture's own encoding); black below 0.004, lit above 0.03; an event is 3 or more touching "
                  "transient-black points (lit, black, lit; or lit, black, black, lit); readback 4 frames deep; the first 60 events are listed, a summary every 60 s.",
              "the start line, exact: the stage, the grid, the thresholds, the encoding, the readback depth");
        Counts w;
        w.frames = 5400; w.eyes = 10800; w.events = 3; w.region[0] = 2; w.region[3] = 1; w.largest = 25; w.transient = 80; w.blackStart = 7; w.lost = 1;
        char sum[512];
        formatSummary(sum, 60, w);
        check(std::string(sum) == "edge black: 60 s: frames 5400 eyes-sampled 10800 events 3 (outer-bottom 2, outer-middle 0, outer-top 0, inner-bottom 1, inner-middle 0, inner-top 0, "
                                  "centre 0) largest 25 points; transient points total 80; black-start points (black now, lit before, still black) 7; samples lost 1",
              "the summary line, exact");
    }

    // ---- the window: a summary every 60 s, events or none, the event cap, the close -------------------------------------------------------------------
    {
        Watch w;
        Lines lines;
        Grid lit;
        uint64_t seq = 1, now = 1000;
        for (int i = 0; i < 100; ++i) { w.ingest(0, lit.v(), frame(seq), now, lines); w.ingest(1, lit.v(), frame(seq), now, lines); ++seq; now += 11; }
        check(lines.v.empty() && w.window().frames == 100 && w.window().eyes == 200, "a window under 60 s writes nothing; it counts frames (a sequence number once) and eye samples");
        now = 1000 + 59999;
        w.ingest(0, lit.v(), frame(seq), now, lines);
        check(lines.v.empty(), "59.999 s: not yet");
        now = 1000 + 60000;
        w.ingest(0, lit.v(), frame(seq), now, lines);
        check(lines.v.size() == 1 && lines.v[0].rfind("edge black: 60 s: frames 101 eyes-sampled 202 events 0 (", 0) == 0 && contains(lines.v[0], "largest 0 points; transient points total 0;"),
              "60 s: the summary is written with ZERO events (a missing line means the watch never ran)");
        check(w.window().frames == 0 && !w.open() && w.total().frames == 101, "...and the window starts again with the next sample");
        // A window of events.
        for (int k = 0; k < 4; ++k) {
            w.ingest(0, lit.v(), frame(++seq), now += 11, lines);
            w.ingest(0, Grid().rect(0, 116, 10, 12, 0.0f).v(), frame(++seq), now += 11, lines);
            w.ingest(0, lit.v(), frame(++seq), now += 11, lines);
            w.ingest(0, lit.v(), frame(++seq), now += 11, lines);
        }
        check(w.window().events == 4 && w.window().region[0] == 4 && w.window().largest == 120 && w.listed() == 4 && lines.count("edge black: event eye=0 ") == 4,
              "four flashes in an eye: four events, in outer-bottom, the largest of 30 points, each listed");
        const std::string* e = lines.find("edge black: event eye=0 ");
        check(e && contains(*e, "points=120 cells=10x12 centre=0.04,0.95 region=outer-bottom border=1 head=unknown"), "...an event line reads as it should (border, region, no head pose)");
        // The cap.
        for (int k = 0; k < 70; ++k) {
            w.ingest(1, lit.v(), frame(++seq), now += 11, lines);
            w.ingest(1, Grid().rect(120, 8, 8, 8, 0.0f).v(), frame(++seq), now += 11, lines);
            w.ingest(1, lit.v(), frame(++seq), now += 11, lines);
            w.ingest(1, lit.v(), frame(++seq), now += 11, lines);
        }
        check(w.listed() == kEventsListed && lines.count("edge black: event ") == kEventsListed && lines.count("edge black: 60 events listed;") == 1,
              "the first 60 events are listed and then one line says the rest are counted only");
        check(w.window().events == 74, "...but every event is counted (4 + 70)");
        w.close(now + 30000, lines);
        check(lines.v.size() >= 3 && lines.v[lines.v.size() - 2].rfind("edge black: ", 0) == 0 && contains(lines.v[lines.v.size() - 2], " s: frames ") && contains(lines.v[lines.v.size() - 2], "events 74 (") &&
                  lines.v.back().rfind("edge black: closed -- totals for the run: frames ", 0) == 0 && contains(lines.v.back(), "events 74 (") && w.total().events == 74,
              "the close writes the open window (74 events) and then the totals for the run");
        // An eye reset (a size change) and lost samples.
        Watch r;
        Lines rl;
        r.ingest(0, lit.v(), frame(1), 0, rl);
        r.ingest(0, Grid().rect(0, 116, 10, 12, 0.0f).v(), frame(2), 11, rl);
        r.ingest(0, lit.v(), frame(3), 22, rl);
        r.resetEye(0);
        check(r.window().lost == 3, "a reset of an eye (a resize) counts the frames it held as lost");
        r.ingest(0, lit.v(), frame(4), 33, rl);
        r.ingest(0, lit.v(), frame(5), 44, rl);
        r.ingest(0, lit.v(), frame(6), 55, rl);
        r.ingest(0, lit.v(), frame(7), 66, rl);
        check(r.window().events == 0 && rl.v.empty(), "...and the flash that straddled it is not an event: the history before the reset is gone");
        r.noteLost(5);
        check(r.window().lost == 8, "samples that never reached the watch are counted lost too");
        r.ingest(2, lit.v(), frame(8), 77, rl);
        r.resetEye(-1);
        check(r.window().eyes == 7, "an eye that does not exist is ignored");
    }

    // ---- the cost of the pure part ----------------------------------------------------------------------------------------------------------------------
    {
        Watch w;
        Lines lines;
        Grid g;
        for (int i = 0; i < kPoints; i += 7) g.v()[i] = (i % 3) ? 0.0f : 0.02f;
        const int n = 4000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) w.ingest(i & 1, g.v(), frame(1 + i / 2), 1000 + static_cast<uint64_t>(i), lines);
        const auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / n;
        std::printf("edge_black_test: the judgement of one eye's grid averages %.1f us on this machine\n", us);
        check(us < 1000.0, "the judgement of one eye's grid costs far under a millisecond (it is three passes over 16384 bytes)");
    }

    // ---- the fixture ---------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string built = fixtureLog();
        std::string fixture;
        FILE* f = nullptr;
        if (fopen_s(&f, "tools/edge_black_fixture.log", "rb") == 0 && f) {
            char buffer[4096];
            size_t got;
            while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) fixture.append(buffer, got);
            std::fclose(f);
        }
        std::string normalised;
        for (char ch : fixture) if (ch != '\r') normalised += ch;
        check(!fixture.empty(), "tools/edge_black_fixture.log is readable from the repo root");
        check(normalised == built, "the log tool's fixture file is what the watch writes for the scripted run, byte for byte (python tools\\edvr_log.py --tally edge reads it; regenerate with --print-fixture)");
        size_t events = 0, summaries = 0, closes = 0, caps = 0, starts = 0;
        size_t at = 0;
        while ((at = built.find("edge black: event ", at)) != std::string::npos) { ++events; at += 10; }
        at = 0;
        while ((at = built.find(" s: frames ", at)) != std::string::npos) { ++summaries; at += 10; }
        closes = built.find("edge black: closed -- totals for the run:") != std::string::npos ? 1 : 0;
        caps = built.find("edge black: 60 events listed;") != std::string::npos ? 1 : 0;
        starts = built.find("edge black: watching stage game") != std::string::npos ? 1 : 0;
        check(events == 60 && summaries == 5 && closes == 1 && caps == 1 && starts == 1,
              "...and it carries the start line, 60 event lines, the cap line, five summaries (four of 60 s and the last of 57) and the closing totals");
    }

    // ---- the GPU half, on WARP, with the build's own compute shader --------------------------------------------------------------------------------------
    Gpu gpu;
    gpu.init();
    const UINT W = 512, H = 384;   // a cell is 4 x 3 texels (128 x 128 cells)
    const uint32_t full[4] = {0, 0, W, H};
    const auto eventLines = [&] {
        std::vector<std::string> out;
        for (const auto& l : testLog) if (l.rfind("edge black: event ", 0) == 0) out.push_back(l);
        return out;
    };
    {   // a flash in the bottom-left corner, eye 0
        resetGpuState();
        auto lit = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        auto flash = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        gpu.fill(lit.Get(), W, H, 128, {});
        gpu.fill(flash.Get(), W, H, 128, {{0, H - 24, 24, H}});
        for (uint64_t s = 1; s <= 9; ++s) sampleFrame(gpu, s == 5 ? flash.Get() : lit.Get(), 0, s, full);
        const auto events = eventLines();
        check(events.size() == 1, "GPU: a one-frame black square in the corner of a lit eye texture is exactly one event");
        check(!events.empty() && contains(events[0], "event eye=0 frame=5 ") && contains(events[0], "points=48 cells=6x8 centre=0.02,0.97 region=outer-bottom border=1 head=unknown"),
              "...frame 5, 48 points (6 x 8 cells of 4 x 3 texels), the centre of the corner, outer-bottom, on the border");
        check(testLog.size() >= 2 && testLog[0].rfind("edge black: watching stage game ", 0) == 0 && contains(testLog[0], "through a plain view of R8G8B8A8_TYPELESS (code / 255,"),
              "...after one start line, naming the format and its encoding");
        size_t starts = 0;
        for (const auto& l : testLog) if (l.rfind("edge black: watching ", 0) == 0) ++starts;
        check(starts == 1, "...said once");
        check(g_watch.total().events + g_watch.window().events == 1 && g_watch.window().transient == 48, "...48 transient points counted");
        edgeBlackClose();
        check(testLog.back().rfind("edge black: closed -- totals for the run: frames 8 eyes-sampled 8 events 1 (outer-bottom 1, ", 0) == 0 && contains(testLog.back(), "samples lost 0"),
              "the close: nine frames sampled and eight read back (the ninth is still in flight), one event, outer-bottom, none lost");
        const std::string& summary = testLog[testLog.size() - 2];
        check(summary.rfind("edge black: ", 0) == 0 && contains(summary, " s: frames 8 eyes-sampled 8 events 1 "), "...after the open window's summary");
    }
    {   // the same flash in eye 1 is the inner side
        resetGpuState();
        auto lit = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        auto flash = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        gpu.fill(lit.Get(), W, H, 128, {});
        gpu.fill(flash.Get(), W, H, 128, {{0, H - 24, 24, H}});
        for (uint64_t s = 1; s <= 9; ++s) sampleFrame(gpu, s == 5 || s == 6 ? flash.Get() : lit.Get(), 1, s, full);
        const auto events = eventLines();
        check(events.size() == 2 && contains(events[0], "event eye=1 frame=5 ") && contains(events[0], "region=inner-bottom") && contains(events[1], "event eye=1 frame=6 "),
              "GPU: eye 1's left edge is its inner side, and a two-frame flash is an event in each of its two frames");
    }
    {   // a head pose: the speed in the line
        resetGpuState();
        auto lit = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        auto flash = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        gpu.fill(lit.Get(), W, H, 128, {});
        gpu.fill(flash.Get(), W, H, 128, {{0, H - 24, 24, H}});
        float head[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (uint64_t s = 1; s <= 9; ++s) {
            const float a = 0.02f * static_cast<float>(s);   // a steady turn about y; the frames are however far apart the machine ran them
            head[0] = head[10] = std::cos(a); head[2] = std::sin(a); head[8] = -std::sin(a);
            Sleep(3);   // frames at least a few milliseconds apart, so there is a speed to report
            sampleFrame(gpu, s == 5 ? flash.Get() : lit.Get(), 0, s, full, false, false, head);
        }
        const auto events = eventLines();
        check(events.size() == 1 && contains(events[0], " deg/s") && !contains(events[0], "head=unknown"), "GPU: with a head pose the line carries a head speed in deg/s");
    }
    {   // a region of a bigger texture, and a mirrored eye
        resetGpuState();
        const UINT TW = 640, TH = 480;
        auto lit = gpu.texture(TW, TH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        auto outside = gpu.texture(TW, TH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        auto inside = gpu.texture(TW, TH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        gpu.fill(lit.Get(), TW, TH, 128, {});
        gpu.fill(outside.Get(), TW, TH, 128, {{576, 0, 640, 480}, {0, 0, 64, 480}, {0, 416, 640, 480}});   // everything outside the region
        gpu.fill(inside.Get(), TW, TH, 128, {{64, 392, 88, 416}});                                         // the region's bottom-left corner
        const uint32_t region[4] = {64, 32, 576, 416};   // 512 x 384
        for (uint64_t s = 1; s <= 9; ++s) sampleFrame(gpu, s == 5 ? outside.Get() : lit.Get(), 0, s, region);
        check(eventLines().empty(), "GPU: black outside the submitted region, however much of it, is not seen");
        for (uint64_t s = 11; s <= 19; ++s) sampleFrame(gpu, s == 15 ? inside.Get() : lit.Get(), 0, s, region);
        const auto events = eventLines();
        check(events.size() == 1 && contains(events[0], "frame=15 ") && contains(events[0], "points=48 cells=6x8 centre=0.02,0.97 region=outer-bottom border=1"),
              "...and the region's own corner is the image's corner: the same 48 points at the same cells as the whole-texture case");
        testLog.clear();
        g_eye[0] = EyeGpu();
        g_watch = Watch();
        for (uint64_t s = 21; s <= 29; ++s) sampleFrame(gpu, s == 25 ? inside.Get() : lit.Get(), 0, s, region, true, false);
        const auto flipped = eventLines();
        check(flipped.size() == 1 && contains(flipped[0], "frame=25 ") && contains(flipped[0], "points=48 cells=6x8 centre=0.98,0.97 region=inner-bottom border=1"),
              "...a mirrored eye (bounds the other way round) is un-mirrored: the corner that was left is the right, an inner corner of eye 0");
    }
    {   // a resize in the middle of a flash
        resetGpuState();
        auto litA = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        auto flashA = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        auto litB = gpu.texture(640, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        gpu.fill(litA.Get(), W, H, 128, {});
        gpu.fill(flashA.Get(), W, H, 128, {{0, H - 24, 24, H}});
        gpu.fill(litB.Get(), 640, H, 128, {});
        const uint32_t regionB[4] = {0, 0, 640, H};
        sampleFrame(gpu, litA.Get(), 0, 1, full);
        sampleFrame(gpu, litA.Get(), 0, 2, full);
        sampleFrame(gpu, flashA.Get(), 0, 3, full);
        sampleFrame(gpu, litB.Get(), 0, 4, regionB);   // the eye texture was recreated at another size: the flash cannot be judged
        for (uint64_t s = 5; s <= 10; ++s) sampleFrame(gpu, litB.Get(), 0, s, regionB);
        check(eventLines().empty(), "GPU: a flash with a resize right after it is not an event (the history is gone, not guessed)");
        edgeBlackClose();
        const std::string& closing = testLog.back();
        check(closing.rfind("edge black: closed -- totals for the run:", 0) == 0 && !contains(closing, "samples lost 0"), "...and the frames lost to the resize are counted");
    }
    {   // formats and binds the watch refuses
        resetGpuState();
        auto srgb = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_SHADER_RESOURCE);
        gpu.fill(srgb.Get(), W, H, 128, {});
        sampleFrame(gpu, srgb.Get(), 0, 1, full);
        sampleFrame(gpu, srgb.Get(), 0, 2, full);
        check(testLog.size() == 1 && contains(testLog[0], "edge black: eye 0 is not watched: its format is not one the watch reads (the submitted texture is R8G8B8A8_UNORM_SRGB)."),
              "GPU: an sRGB texture is refused, said once, per eye");
        resetGpuState();
        auto noView = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET);
        sampleFrame(gpu, noView.Get(), 1, 1, full);
        check(testLog.size() == 1 && contains(testLog[0], "edge black: eye 1 is not watched: the texture refuses a shader view (the submitted texture is R8G8B8A8_TYPELESS)."),
              "...so is a texture that refuses a shader view");
        resetGpuState();
        auto lit = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        const uint32_t empty[4] = {10, 10, 10, 20}, past[4] = {0, 0, W + 1, H};
        sampleFrame(gpu, lit.Get(), 0, 1, empty);
        sampleFrame(gpu, lit.Get(), 0, 2, past);
        edgeBlackSample(nullptr, lit.Get(), 0, 3, full, false, false, nullptr);
        edgeBlackSample(gpu.dev.Get(), nullptr, 0, 3, full, false, false, nullptr);
        edgeBlackSample(gpu.dev.Get(), lit.Get(), 2, 3, full, false, false, nullptr);
        check(testLog.empty() && g_watch.window().eyes == 0, "an empty region, a region past the texture, no device, no texture and an eye that does not exist sample nothing and say nothing");
    }
    {   // a float texture is read in its own encoding
        resetGpuState();
        auto lit = gpu.texture(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        auto flash = gpu.texture(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        std::vector<uint16_t> half(static_cast<size_t>(W) * H * 4, 0x3800), halfFlash;   // 0x3800 = 0.5
        halfFlash = half;
        for (UINT y = H - 24; y < H; ++y) for (UINT x = 0; x < 24; ++x) { uint16_t* p = &halfFlash[(static_cast<size_t>(y) * W + x) * 4]; p[0] = p[1] = p[2] = 0; }
        gpu.ctx->UpdateSubresource(lit.Get(), 0, nullptr, half.data(), W * 8, 0);
        gpu.ctx->UpdateSubresource(flash.Get(), 0, nullptr, halfFlash.data(), W * 8, 0);
        for (uint64_t s = 1; s <= 9; ++s) sampleFrame(gpu, s == 5 ? flash.Get() : lit.Get(), 0, s, full);
        check(eventLines().size() == 1 && contains(testLog[0], "R16G16B16A16_FLOAT (the float value,"), "GPU: a half-float texture is read as its float value: the same flash, one event");
    }
    {   // the largest of R, G and B: a pure red or blue point is lit, only all-three-black is black
        resetGpuState();
        auto lit = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        auto red = gpu.texture(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
        gpu.fill(lit.Get(), W, H, 128, {});
        std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 128);
        for (UINT c = 0; c < 3; ++c)   // three squares: pure red, pure green, pure blue
            for (UINT y = H - 24; y < H; ++y) for (UINT x = c * 100; x < c * 100 + 24; ++x) { uint8_t* p = &px[(static_cast<size_t>(y) * W + x) * 4]; p[0] = p[1] = p[2] = 0; p[c] = 200; }
        gpu.ctx->UpdateSubresource(red.Get(), 0, nullptr, px.data(), W * 4, 0);
        for (uint64_t s = 1; s <= 9; ++s) sampleFrame(gpu, s == 5 ? red.Get() : lit.Get(), 0, s, full);
        check(eventLines().empty(), "GPU: squares that are pure red, pure green and pure blue are lit, not black (the largest of R, G, B)");
    }

    std::printf("edge_black_test: %u checks, 0 failures\n", checks);
    return 0;
}
