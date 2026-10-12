#pragma once
// Pure logic for the GPU pass profiler (gpu_pass_profile.h): what a pass IS
// (classification), how cube-face runs group, the stamp budget, the per-pass
// shader tally, the summary arithmetic and the CSV text. Nothing here touches
// D3D11 or Win32, so tools\gpu_pass_profile_test compiles it alone. The rules
// are stated in docs\gpu-pass-profile-2026-10-12.md; change them there too.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace edvr::gpupass {

constexpr uint32_t kCaptureFrames = 4;         // whole frames per press
constexpr uint32_t kMaxStampsPerFrame = 4096;  // timestamp queries per frame; a boundary past the cap is counted, never issued
constexpr uint32_t kHashSlots = 8;             // distinct VS/PS pairs tracked per pass; the rest fold into one count
constexpr uint32_t kTopHashes = 3;             // shader pairs listed per pass
constexpr uint32_t kTopPasses = 15;            // passes in the summary's top list
constexpr uint32_t kCubeRun = 6;               // consecutive same-size square passes read as one cube map

enum class PassClass : uint8_t {
    Edvr = 0,         // inside an EDVR census section: EDVR's own work, excluded from Elite's total
    Compute,          // dispatches and no draws
    Clear,            // clears and no draws
    Shadow,           // depth only, square, >= 512 or an array of slices
    ProbeCube,        // a square colour target of a cube resource, or one of kCubeRun face passes in a row
    SceneGBuffer,     // two or more colour targets at the eye size, or a depth-only pass at the eye size
    FullscreenAtEye,  // one colour target at the eye size with one or two draws (a full-screen triangle or quad)
    ForwardAtEye,     // one colour target at the eye size with three or more draws
    PostSmaller,      // draws into a colour target smaller than the eye (downsampled post chains)
    Other,
    Count
};
inline constexpr const char* kPassClassNames[] = {
    "edvr", "compute", "clear", "shadow", "probe_cube", "scene_gbuffer",
    "fullscreen_at_eye", "forward_at_eye", "post_smaller", "other"};
static_assert(sizeof(kPassClassNames) / sizeof(kPassClassNames[0]) == static_cast<size_t>(PassClass::Count),
              "one name for each pass class");

inline const char* passClassName(PassClass c) {
    const size_t i = static_cast<size_t>(c);
    return i < static_cast<size_t>(PassClass::Count) ? kPassClassNames[i] : "other";
}

// One bound target, as the classifier and the CSV need it. w and h are the
// view's own mip size; slices is the array size the view addresses (1 for a
// plain 2D view); cube is whether the underlying resource is a cube map.
struct TargetShape {
    uint32_t w = 0, h = 0;
    uint32_t format = 0;   // DXGI_FORMAT
    uint32_t slices = 1;
    bool cube = false;
    bool square() const { return w != 0 && w == h; }
};

// Everything a pass segment has accumulated, before classification.
struct PassFacts {
    bool edvr = false;          // inside an EDVR census section
    uint32_t colours = 0;       // non-null colour targets bound
    TargetShape colour;         // the first of them
    bool hasDepth = false;
    TargetShape depth;
    uint32_t draws = 0, dispatches = 0, clears = 0;
};

struct EyeSize {
    uint32_t w = 0, h = 0;
};

inline std::string targetText(const TargetShape& t) {
    if (!t.w) return "-";
    return std::to_string(t.w) + "x" + std::to_string(t.h);
}

// The class of one segment. Order matters: the first rule that holds wins.
inline PassClass classifyPass(const PassFacts& f, EyeSize eye) {
    if (f.edvr) return PassClass::Edvr;
    if (f.draws == 0 && f.dispatches > 0) return PassClass::Compute;
    if (f.draws == 0) return f.clears > 0 ? PassClass::Clear : PassClass::Other;
    const bool eyeColour = eye.w != 0 && f.colour.w == eye.w && f.colour.h == eye.h;
    if (f.colours == 0 && f.hasDepth) {
        if (f.depth.square() && (f.depth.w >= 512 || f.depth.slices > 1)) return PassClass::Shadow;
        if (eye.w != 0 && f.depth.w == eye.w && f.depth.h == eye.h) return PassClass::SceneGBuffer;
        return PassClass::Other;
    }
    if (f.colours >= 1 && f.colour.square() && f.colour.cube) return PassClass::ProbeCube;
    if (f.colours >= 2 && eyeColour) return PassClass::SceneGBuffer;
    if (eyeColour) return f.draws <= 2 ? PassClass::FullscreenAtEye : PassClass::ForwardAtEye;
    if (eye.w != 0 && f.colours >= 1 &&
        static_cast<uint64_t>(f.colour.w) * f.colour.h < static_cast<uint64_t>(eye.w) * eye.h)
        return PassClass::PostSmaller;
    return PassClass::Other;
}

// A cube map drawn face by face is kCubeRun or more consecutive passes, each
// one colour target, square, drawn into, all the same size. Relabels those
// runs ProbeCube in place. cls[i] is the class of facts[i].
inline bool cubeCandidate(const PassFacts& f) {
    return !f.edvr && f.colours == 1 && f.colour.square() && f.draws > 0;
}
inline void markCubeRuns(const std::vector<PassFacts>& facts, std::vector<PassClass>& cls) {
    size_t i = 0;
    while (i < facts.size()) {
        if (!cubeCandidate(facts[i])) { ++i; continue; }
        size_t j = i + 1;
        while (j < facts.size() && cubeCandidate(facts[j]) && facts[j].colour.w == facts[i].colour.w) ++j;
        if (j - i >= kCubeRun)
            for (size_t k = i; k < j; ++k) cls[k] = PassClass::ProbeCube;
        i = j;
    }
}

// One stamp budget per frame. take(reserve) issues a stamp only while at
// least reserve stamps stay free after it, so a boundary (reserve 1) can never
// take the slot the frame's end needs (reserve 0). A refused stamp is counted.
struct StampBudget {
    uint32_t cap = kMaxStampsPerFrame;
    uint32_t used = 0, overflow = 0;
    bool take(uint32_t reserve) {
        if (used + reserve >= cap) { ++overflow; return false; }
        ++used;
        return true;
    }
};

struct HashTally {
    uint64_t vs = 0, ps = 0;
    uint32_t draws = 0;
};

// The shader pairs a pass drew with, by draw count. Eight slots; a pair past
// them is folded into one count, so a pass that uses many shaders still
// reports its top three correctly for the heavy ones.
class PassHashes {
public:
    void note(uint64_t vs, uint64_t ps) {
        for (uint32_t i = 0; i < used_; ++i)
            if (slots_[i].vs == vs && slots_[i].ps == ps) { ++slots_[i].draws; return; }
        if (used_ < kHashSlots) { slots_[used_++] = HashTally{vs, ps, 1}; return; }
        ++folded_;
    }
    uint32_t folded() const { return folded_; }
    std::vector<HashTally> top() const {
        std::vector<HashTally> out(slots_, slots_ + used_);
        std::stable_sort(out.begin(), out.end(),
                         [](const HashTally& a, const HashTally& b) { return a.draws > b.draws; });
        if (out.size() > kTopHashes) out.resize(kTopHashes);
        return out;
    }
private:
    HashTally slots_[kHashSlots]{};
    uint32_t used_ = 0, folded_ = 0;
};

// One row of the CSV: one pass segment of one captured frame.
struct Row {
    uint32_t frame = 0, index = 0;
    PassClass cls = PassClass::Other;
    bool edvr = false;
    double ms = 0;
    uint32_t draws = 0, dispatches = 0, clears = 0;
    uint64_t tris = 0;          // estimate: count / 3 x instances for the triangle draws; 0 otherwise
    uint32_t colours = 0;
    std::string rt = "-", dsv = "-", top = "-";
    uint32_t rtFormat = 0, dsvFormat = 0, slices = 1;
    bool cube = false;
    // The grouping key: class and target signature, not the frame or the index.
    std::string key() const {
        return std::string(passClassName(cls)) + " rt=" + rt + " colours=" + std::to_string(colours) +
               " slices=" + std::to_string(slices) + (cube ? " cube" : "") + " dsv=" + dsv;
    }
};

inline std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

inline const char* csvHeader() {
    return "frame,pass,class,edvr,ms,draws,dispatches,clears,tris,colours,rt,rt_fmt,slices,cube,dsv,dsv_fmt,top_vs_ps_draws";
}

inline std::string csvRow(const Row& r) {
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.4f", r.ms);
    std::string out;
    out += std::to_string(r.frame) + "," + std::to_string(r.index) + "," + passClassName(r.cls) + "," +
           (r.edvr ? "1" : "0") + "," + ms + "," + std::to_string(r.draws) + "," +
           std::to_string(r.dispatches) + "," + std::to_string(r.clears) + "," + std::to_string(r.tris) + "," +
           std::to_string(r.colours) + "," + csvField(r.rt) + "," + std::to_string(r.rtFormat) + "," +
           std::to_string(r.slices) + "," + (r.cube ? "1" : "0") + "," + csvField(r.dsv) + "," +
           std::to_string(r.dsvFormat) + "," + csvField(r.top);
    return out;
}

// "vs:ps:draws;..." with the hashes in hex. "-" when the pass drew nothing.
inline std::string topText(const std::vector<HashTally>& top) {
    if (top.empty()) return "-";
    std::string out;
    char buf[64];
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) out += ';';
        std::snprintf(buf, sizeof buf, "%016llx:%016llx:%u", static_cast<unsigned long long>(top[i].vs),
                      static_cast<unsigned long long>(top[i].ps), top[i].draws);
        out += buf;
    }
    return out;
}

struct ClassMean {
    double ms = 0, passes = 0;   // per frame
};
struct Summary {
    struct Group {
        double ms = 0, passes = 0, draws = 0;
        PassClass cls = PassClass::Other;
        std::string sig;
    };
    uint32_t frames = 0;                   // captured frames the means are over
    double frameMs = 0, eliteMs = 0, edvrMs = 0;   // per frame, mean
    ClassMean cls[static_cast<size_t>(PassClass::Count)]{};
    std::vector<Group> top;                // per frame, most ms first, at most kTopPasses
};

// frameMs[f] is frame f's span; frameOk[f] is false for a frame whose result
// was not read (its rows must be absent). Every mean is over the frames that
// were read, so a missing frame is said, not averaged in as zero.
inline Summary summarise(const std::vector<Row>& rows, const std::vector<double>& frameMs,
                         const std::vector<bool>& frameOk) {
    Summary s;
    double frameSum = 0;
    for (size_t f = 0; f < frameOk.size(); ++f)
        if (frameOk[f]) {
            ++s.frames;
            frameSum += f < frameMs.size() ? frameMs[f] : 0;
        }
    if (!s.frames) return s;
    const double n = static_cast<double>(s.frames);
    std::map<std::string, Summary::Group> groups;
    double elite = 0, edvr = 0;
    for (const Row& r : rows) {
        if (r.frame >= frameOk.size() || !frameOk[r.frame]) continue;
        ClassMean& c = s.cls[static_cast<size_t>(r.cls)];
        c.ms += r.ms;
        c.passes += 1;
        (r.edvr ? edvr : elite) += r.ms;
        Summary::Group& g = groups[r.key()];
        g.ms += r.ms;
        g.passes += 1;
        g.draws += r.draws;
        g.cls = r.cls;
        g.sig = "rt " + r.rt + " colours " + std::to_string(r.colours) + " slices " +
                std::to_string(r.slices) + (r.cube ? " cube" : "") + " depth " + r.dsv;
    }
    s.frameMs = frameSum / n;
    s.eliteMs = elite / n;
    s.edvrMs = edvr / n;
    for (ClassMean& c : s.cls) {
        c.ms /= n;
        c.passes /= n;
    }
    for (auto& kv : groups) {
        Summary::Group g = kv.second;
        g.ms /= n;
        g.passes /= n;
        g.draws /= n;
        s.top.push_back(g);
    }
    std::stable_sort(s.top.begin(), s.top.end(),
                     [](const Summary::Group& a, const Summary::Group& b) { return a.ms > b.ms; });
    if (s.top.size() > kTopPasses) s.top.resize(kTopPasses);
    return s;
}

}  // namespace edvr::gpupass
