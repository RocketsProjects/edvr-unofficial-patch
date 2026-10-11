// The render thread's CPU, sampled from inside the process (docs/engine-render-performance-2026-09-19.md, the hangar arc).
//
// Where the game's render thread spends its time, by sampling it: a worker thread stops the render thread at about
// 1 kHz for ten seconds, reads its registers and copies the stack above them, lets it go, and then walks the copy to
// the leaf and up to eight return addresses. Afterwards each address is resolved to a module (module+RVA) and to a
// function (the RUNTIME_FUNCTION of the module's .pdata that holds it, so no symbols are needed). The report counts
// leaves, functions (inclusive and exclusive) and module classes.
//
// THE SAFETY ARGUMENT is the stall sampler's (stall_sampler.h, its header comment), and this file adds nothing to the
// stopped window: the only calls that stop the thread are stall::captureThread (SuspendThread, GetThreadContext, a
// bounded copy, ResumeThread), and that function is reused unchanged. The walk (stall::unwindCapture) runs after the
// resume, as the stall sampler's does. The module and .pdata resolution runs once, after the whole window, on the
// worker. Nothing here is allocated, logged or locked while a thread is stopped; the sample buffer exists before the
// first stop. The handle the worker samples through is a DuplicateHandle of the render thread's own pseudo-handle,
// made by that thread (src\d3d11\cpu_profile.cpp), with the same rights the stall sampler asks for.
//
// THE OVERHEAD CAP. Every sample stops the thread for its own suspend cost (stoppedTicks), so the next sample is not
// due sooner than suspend / kMaxStoppedFraction after this one's due time: at 1 kHz with a 30 us stop that is the
// period, and with a 100 us stop the rate falls to 500 Hz. The achieved rate and the total stopped time are logged, so
// a capture that ran slow says so.
//
// HEADER ONLY, so tools\cpu_sampler_test runs this code against a busy thread with known hot functions. The graphics
// half's glue (src\d3d11\cpu_profile.cpp) adds the hotkey, the configuration key, the log and the CSV file's place.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "stall_sampler.h"

namespace edvr::cpu {

constexpr double kWindowSeconds = 10.0;        // the production window
constexpr double kTargetHz = 1000.0;           // the rate asked for
constexpr double kMaxStoppedFraction = 0.05;   // the render thread is stopped at most this share of the window
constexpr unsigned kFrames = 9;                // the leaf (RIP) and up to eight return addresses
constexpr unsigned kMaxSamples = 12000;        // 12 s at the target rate: the window is 10 s, the rest is slack
constexpr unsigned kMaxModules = 96;
constexpr unsigned kSlots = 1u << 14;          // open-addressing slots for locations and functions (half-full at most)
constexpr unsigned kTopN = 40;                 // the leaf and function tables
constexpr unsigned kTopModules = 12;
constexpr uint32_t kNoFunction = 0xFFFFFFFFu;  // a frame in a module with no .pdata entry holding its RVA
constexpr DWORD kTimerHighRes = 0x00000002;    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803 and later)

// Which part of the process a module is. The report's per-class shares are these.
enum class Class : uint8_t { Unknown = 0, Elite, EdvrD3d11, EdvrOpenvr, Vendor, D3dDriver, NtdllKernel, Other, Count };

inline const char* className(Class c) noexcept {
    switch (c) {
        case Class::Unknown: return "unknown";
        case Class::Elite: return "elite";
        case Class::EdvrD3d11: return "edvr_d3d11";
        case Class::EdvrOpenvr: return "edvr_openvr";
        case Class::Vendor: return "vendor";
        case Class::D3dDriver: return "d3d_driver";
        case Class::NtdllKernel: return "ntdll_kernel";
        case Class::Other: return "other";
        default: return "unknown";
    }
}

inline bool startsNoCase(const char* s, const char* prefix) noexcept {
    for (; *prefix; ++s, ++prefix) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// `edvr` is true for this DLL's own module (stall::thisModule). The rest is by file name: the game's executable, EDVR's
// own OpenXR runtime (openvr_api.dll, the one this build ships in Openvr\win64), the OS images a wait sits in, the
// graphics drivers and the D3D/DXGI front ends, and the vendor runtimes (Oculus, OpenXR loaders, DLSS, FSR, XeSS).
inline Class classify(const char* name, bool edvr) noexcept {
    if (edvr) return Class::EdvrD3d11;
    if (!name || !name[0] || name[0] == '?') return Class::Unknown;
    if (stall::equalsNoCase(name, "EliteDangerous64.exe")) return Class::Elite;
    if (stall::equalsNoCase(name, "openvr_api.dll")) return Class::EdvrOpenvr;
    if (stall::isSystemModule(name)) return Class::NtdllKernel;
    static const char* const kDriver[] = {"d3d11.dll", "dxgi.dll", "d3d12.dll", "dxcore.dll", "nvwgf2umx", "nvldumdx",
                                          "nvd3dumx", "nvoglv", "igd1", "amdvlk", "amdxc"};
    for (const char* p : kDriver) {
        if (startsNoCase(name, p)) return Class::D3dDriver;
    }
    static const char* const kVendor[] = {"ovr", "libovr", "oculus", "openxr", "vrclient", "vrserver", "steamvr", "openvr",
                                          "nvngx", "amd_fidelity", "libxess", "xess", "ffx", "fsr"};
    for (const char* p : kVendor) {
        if (startsNoCase(name, p)) return Class::Vendor;
    }
    return Class::Other;
}

// The sampling rate's pacing, as a pure function so the rig can drive it. `dueUs` is when this sample was due, `nowUs`
// the clock when it was taken, `suspendUs` how long the thread was stopped for it. The next sample is due one period
// later, or later still when the stop was long enough that the thread would otherwise be stopped more than the cap
// allows. A sample that is more than a period behind does not catch up in a burst: the schedule restarts from now.
struct Pacer {
    static double nextDueUs(double dueUs, double nowUs, double suspendUs, double periodUs, double maxFraction) noexcept {
        double spacing = maxFraction > 0.0 ? suspendUs / maxFraction : periodUs;
        if (spacing < periodUs) spacing = periodUs;
        double next = dueUs + spacing;
        if (next < nowUs - periodUs) next = nowUs;
        return next;
    }
};

// One sample, as the window left it. The module and RVA of each frame are filled in by Session::resolve.
struct Sample {
    uint32_t tUs = 0;                  // from the window's start
    uint32_t suspendUs = 0;            // the stopped window: suspend, context, copy, resume
    uint8_t status = 0;                // stall::CaptureStatus
    uint8_t frames = 0;                // how many of pc[] the walk gave (0 when the thread was not read)
    uintptr_t pc[kFrames] = {};        // the leaf, then the return addresses, innermost first
    int16_t mod[kFrames] = {};         // module index, or -1 when no module holds the address
    uint32_t rva[kFrames] = {};        // the RVA in that module (the low 32 bits of the address when -1)
};

struct ModuleRec {
    char name[32] = {};                // the file name, cut at 31 characters
    uintptr_t base = 0;
    Class cls = Class::Unknown;
    const RUNTIME_FUNCTION* fns = nullptr;   // the module's .pdata, read in place (the module is loaded for the process)
    uint32_t nfns = 0;
    uint32_t leaf = 0;                 // samples whose leaf is in this module
    uint32_t stack = 0;                // samples with at least one frame in this module
};

// A location (module+RVA of a leaf) or a function (module+begin of a .pdata entry), counted once per key.
struct LocRec {
    uint64_t key = 0;                  // 0 is empty
    uint32_t count = 0;
    int16_t mod = -1;
    uint32_t rva = 0;
};
struct FnRec {
    uint64_t key = 0;                  // 0 is empty
    uint32_t incl = 0;                 // samples with the function anywhere on the stack
    uint32_t excl = 0;                 // samples whose leaf is in the function
    int16_t mod = -1;
    uint32_t begin = 0, end = 0;       // the .pdata range (begin = kNoFunction when the module has none for the frame)
};

// What a frame's key is. A module-less frame keys on its full address with the top bit set, so it cannot collide.
inline uint64_t locKey(int mod, uint32_t rva, uintptr_t pc) noexcept {
    return mod < 0 ? (static_cast<uint64_t>(pc) | (uint64_t{1} << 63)) : ((static_cast<uint64_t>(mod) + 1) << 32) | rva;
}
inline uint64_t fnKey(int mod, uint32_t begin, uintptr_t pc) noexcept {
    return mod < 0 ? (static_cast<uint64_t>(pc) | (uint64_t{1} << 63)) : ((static_cast<uint64_t>(mod) + 1) << 32) | begin;
}

// Reads the module's exception directory (.pdata) in place. False when the image has none, or is not a PE image.
inline bool loadPdata(uintptr_t base, const RUNTIME_FUNCTION*& fns, uint32_t& n) noexcept {
    fns = nullptr;
    n = 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION)) return false;
        fns = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
        n = dir.Size / sizeof(RUNTIME_FUNCTION);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fns = nullptr;
        n = 0;
        return false;
    }
}

// The .pdata entry holding `rva`: the last entry whose BeginAddress is at or below it, if its EndAddress is above it.
// The table is sorted by BeginAddress, as the unwinder requires.
inline bool findFunction(const RUNTIME_FUNCTION* fns, uint32_t n, uint32_t rva, uint32_t& begin, uint32_t& end) noexcept {
    if (!fns || !n) return false;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (fns[mid].BeginAddress <= rva) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return false;
    const RUNTIME_FUNCTION& f = fns[lo - 1];
    if (rva < f.BeginAddress || rva >= f.EndAddress) return false;
    begin = f.BeginAddress;
    end = f.EndAddress;
    return true;
}

// A fixed-size open-addressing table. Allocated once, before the window; a full table drops the key and counts it.
template <typename R>
class Table {
public:
    bool create(unsigned slots) {
        recs_.reset(new (std::nothrow) R[slots]);
        n_ = recs_ ? slots : 0;
        used_ = 0;
        dropped_ = 0;
        return recs_ != nullptr;
    }
    // The record for `key`, created if new; null when the table is too full to take it.
    R* add(uint64_t key) noexcept {
        if (!n_) return nullptr;
        const unsigned mask = n_ - 1;
        unsigned i = static_cast<unsigned>((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
        for (unsigned probe = 0; probe < n_; ++probe, i = (i + 1) & mask) {
            R& r = recs_[i];
            if (r.key == key) return &r;
            if (r.key == 0) {
                if (used_ * 2u >= n_) break;
                r.key = key;
                ++used_;
                return &r;
            }
        }
        ++dropped_;
        return nullptr;
    }
    template <typename F>
    void each(F f) const {
        for (unsigned i = 0; i < n_; ++i) {
            if (recs_[i].key) f(recs_[i]);
        }
    }
    unsigned used() const noexcept { return used_; }
    uint32_t dropped() const noexcept { return dropped_; }

private:
    std::unique_ptr<R[]> recs_;
    unsigned n_ = 0;
    unsigned used_ = 0;
    uint32_t dropped_ = 0;
};

// The high-resolution waitable timer the pacing sleeps on, or Sleep(1) when the OS will not make one. Made before the
// window, so the window itself makes nothing.
class WaitTimer {
public:
    WaitTimer() {
        h_ = CreateWaitableTimerExW(nullptr, nullptr, kTimerHighRes, TIMER_ALL_ACCESS);
        highRes_ = h_ != nullptr;
        if (!h_) h_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    ~WaitTimer() {
        if (h_) CloseHandle(h_);
    }
    WaitTimer(const WaitTimer&) = delete;
    WaitTimer& operator=(const WaitTimer&) = delete;
    bool highRes() const noexcept { return highRes_; }
    void waitUs(double us) noexcept {
        if (!h_ || us <= 0.0) {
            if (us > 0.0) Sleep(1);
            return;
        }
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(us * 10.0);   // negative: relative, in 100 ns
        if (!SetWaitableTimer(h_, &due, 0, nullptr, nullptr, FALSE)) {
            Sleep(1);
            return;
        }
        WaitForSingleObject(h_, INFINITE);
    }

private:
    HANDLE h_ = nullptr;
    bool highRes_ = false;
};

// One line of the report, or of the log's start line. `user` is whatever the sink needs.
using LineSink = void (*)(const char* line, void* user);

// What a run says about itself in its lines.
struct RunInfo {
    DWORD thread = 0;
    const char* trigger = "?";         // "hotkey.profile_cpu", "diag.profile_cpu = on", or a rig's name
    double windowS = kWindowSeconds;
    double targetHz = kTargetHz;
    double dutyPct = kMaxStoppedFraction * 100.0;
    const char* csvName = "none";      // the file's name in the log directory, or "none"
    const char* build = "?";           // the EDVR build that took the samples (the CSV's header comment)
};

namespace detail {
inline void put(LineSink sink, void* user, const char* fmt, ...) noexcept {
    char line[1100];   // the formats below are short by construction; the log's note keeps about 1166 characters
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<size_t>(n) >= sizeof(line)) line[sizeof(line) - 1] = 0;
    if (sink) sink(line, user);
}
}  // namespace detail

class Session {
public:
    // Everything the run needs, allocated now. False when the memory is not there: nothing is sampled.
    bool prepare(unsigned maxSamples = kMaxSamples) {
        samples_.reset(new (std::nothrow) Sample[maxSamples]);
        mods_.reset(new (std::nothrow) ModuleRec[kMaxModules]);
        cap_ = samples_ ? maxSamples : 0;
        return samples_ && mods_ && locs_.create(kSlots) && fns_.create(kSlots);
    }

    // The window. Samples `thread` (its stack registered as [lo, hi)) for `windowS` seconds at `targetHz`, stopping it
    // at most `maxFraction` of the time. The only calls that stop the thread are inside stall::captureThread; the walk
    // of each copy follows its resume. Nothing else touches the thread, and nothing is logged here.
    void sample(HANDLE thread, uint64_t lo, uint64_t hi, double windowS, double targetHz, double maxFraction) {
        count_ = 0;
        failures_ = 0;
        suspendTotalUs_ = 0.0;
        suspendMaxUs_ = 0;
        wallUs_ = 0.0;
        bufferFull_ = false;
        resolved_ = false;
        if (!cap_ || !thread || !lo || hi <= lo || windowS <= 0.0 || targetHz <= 0.0) return;
        std::unique_ptr<stall::Capture> cap(new (std::nothrow) stall::Capture());
        if (!cap) return;
        WaitTimer timer;
        timer_highres_ = timer.highRes();
        LARGE_INTEGER f{}, t0{}, t{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const double freq = static_cast<double>(f.QuadPart);
        const double periodUs = 1e6 / targetHz;
        const double windowUs = windowS * 1e6;
        double dueUs = 0.0;
        uintptr_t pcs[kFrames] = {};
        for (;;) {
            QueryPerformanceCounter(&t);
            double nowUs = static_cast<double>(t.QuadPart - t0.QuadPart) * 1e6 / freq;
            if (nowUs >= windowUs) break;
            if (count_ >= cap_) {
                bufferFull_ = true;
                break;
            }
            if (dueUs > nowUs) {
                timer.waitUs(dueUs - nowUs);
                QueryPerformanceCounter(&t);
                nowUs = static_cast<double>(t.QuadPart - t0.QuadPart) * 1e6 / freq;
                if (nowUs >= windowUs) break;
            }
            // ---- the thread is stopped inside captureThread, and only there ----
            stall::captureThread(thread, lo, hi, *cap);
            // ---- the thread is running again: walk the copy ----
            const unsigned n = stall::unwindCapture(*cap, pcs, kFrames);
            Sample& s = samples_[count_++];
            s.tUs = static_cast<uint32_t>(nowUs);
            s.suspendUs = static_cast<uint32_t>(static_cast<double>(cap->stoppedTicks) * 1e6 / freq);
            s.status = static_cast<uint8_t>(cap->status);
            s.frames = static_cast<uint8_t>(n);
            for (unsigned j = 0; j < kFrames; ++j) s.pc[j] = j < n ? pcs[j] : 0;
            if (!n) ++failures_;
            suspendTotalUs_ += s.suspendUs;
            if (s.suspendUs > suspendMaxUs_) suspendMaxUs_ = s.suspendUs;
            dueUs = Pacer::nextDueUs(dueUs, nowUs, s.suspendUs, periodUs, maxFraction);
        }
        QueryPerformanceCounter(&t);
        wallUs_ = static_cast<double>(t.QuadPart - t0.QuadPart) * 1e6 / freq;
    }

    // After the window, on the worker: each frame's module and RVA, the module's function (.pdata), the tables and the
    // per-class and per-module counts. Takes the loader's locks; the render thread is running.
    void resolve() {
        valid_ = 0;
        for (unsigned m = 0; m < nMods_; ++m) {
            mods_[m].leaf = 0;
            mods_[m].stack = 0;
        }
        nMods_ = 0;
        for (unsigned c = 0; c < static_cast<unsigned>(Class::Count); ++c) {
            leafClass_[c] = 0;
            stackClass_[c] = 0;
        }
        for (unsigned i = 0; i < count_; ++i) {
            Sample& s = samples_[i];
            if (!s.frames) continue;
            ++valid_;
            uint64_t seenFn[kFrames] = {};
            unsigned seenFns = 0;
            uint32_t classBits = 0;
            int seenMods[kFrames] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
            unsigned seenModN = 0;
            for (unsigned j = 0; j < s.frames; ++j) {
                uintptr_t base = 0;
                const int m = findModule(s.pc[j], base);
                s.mod[j] = static_cast<int16_t>(m);
                const uint32_t rva = m >= 0 ? static_cast<uint32_t>(s.pc[j] - base) : static_cast<uint32_t>(s.pc[j]);
                s.rva[j] = rva;
                uint32_t begin = kNoFunction, end = 0;
                if (m >= 0 && mods_[m].fns && findFunction(mods_[m].fns, mods_[m].nfns, rva, begin, end)) {
                    // the .pdata entry holds this frame
                } else {
                    begin = kNoFunction;
                    end = 0;
                }
                const uint64_t fk = fnKey(m, begin, s.pc[j]);
                if (FnRec* fr = fns_.add(fk)) {
                    if (fr->incl == 0 && fr->excl == 0) {   // a new record: name it once
                        fr->mod = static_cast<int16_t>(m);
                        fr->begin = m < 0 ? static_cast<uint32_t>(s.pc[j]) : begin;
                        fr->end = m < 0 ? 0 : end;
                    }
                    bool dup = false;
                    for (unsigned k = 0; k < seenFns; ++k) dup = dup || seenFn[k] == fk;
                    if (!dup) {
                        seenFn[seenFns++] = fk;
                        ++fr->incl;
                    }
                    if (j == 0) ++fr->excl;
                }
                if (m >= 0) {
                    bool dupMod = false;
                    for (unsigned k = 0; k < seenModN; ++k) dupMod = dupMod || seenMods[k] == m;
                    if (!dupMod) {
                        seenMods[seenModN++] = m;
                        ++mods_[m].stack;
                    }
                    classBits |= 1u << static_cast<unsigned>(mods_[m].cls);
                } else {
                    classBits |= 1u << static_cast<unsigned>(Class::Unknown);
                }
                if (j == 0) {
                    if (LocRec* lr = locs_.add(locKey(m, rva, s.pc[j]))) {
                        lr->mod = static_cast<int16_t>(m);
                        lr->rva = rva;
                        ++lr->count;
                    }
                    if (m >= 0) ++mods_[m].leaf;
                    leafClass_[static_cast<unsigned>(m >= 0 ? mods_[m].cls : Class::Unknown)]++;
                }
            }
            for (unsigned c = 0; c < static_cast<unsigned>(Class::Count); ++c) {
                if (classBits & (1u << c)) stackClass_[c]++;
            }
        }
        resolved_ = true;
    }

    unsigned samples() const noexcept { return count_; }
    unsigned valid() const noexcept { return valid_; }
    uint32_t failures() const noexcept { return failures_; }
    bool bufferFull() const noexcept { return bufferFull_; }
    double wallUs() const noexcept { return wallUs_; }
    double suspendTotalUs() const noexcept { return suspendTotalUs_; }
    uint32_t suspendMaxUs() const noexcept { return suspendMaxUs_; }
    bool highResTimer() const noexcept { return timer_highres_; }
    const Sample* sampleAt(unsigned i) const noexcept { return i < count_ ? &samples_[i] : nullptr; }

    double achievedHz() const noexcept { return wallUs_ > 0.0 ? count_ * 1e6 / wallUs_ : 0.0; }

    // The log's start line: written before the window, so a run that dies in it still says it began.
    void emitStart(LineSink sink, void* user, const RunInfo& info) const {
        detail::put(sink, user,
                    "cpu profile: start thread=%lu trigger=%s window_s=%.1f target_hz=%.0f duty_cap_pct=%.1f csv=%s",
                    static_cast<unsigned long>(info.thread), info.trigger, info.windowS, info.targetHz, info.dutyPct,
                    info.csvName);
    }

    // The report: the pacing line, the top leaves, the top functions (inclusive and exclusive), the class shares, the
    // top modules and the end line. Every line starts "cpu profile:" and is under 1100 characters.
    void emitReport(LineSink sink, void* user, const RunInfo& info) const {
        const double pctOf = valid_ ? 100.0 / valid_ : 0.0;
        const double suspendPct = wallUs_ > 0.0 ? 100.0 * suspendTotalUs_ / wallUs_ : 0.0;
        const double meanUs = count_ ? suspendTotalUs_ / count_ : 0.0;
        detail::put(sink, user,
                    "cpu profile: pacing samples=%u valid=%u failures=%u buffer_full=%d achieved_hz=%.1f wall_ms=%.1f "
                    "suspended_ms=%.2f (%.2f%% of wall) suspend_mean_us=%.1f suspend_max_us=%u timer=%s",
                    count_, valid_, failures_, bufferFull_ ? 1 : 0, achievedHz(), wallUs_ / 1000.0,
                    suspendTotalUs_ / 1000.0, suspendPct, meanUs, suspendMaxUs_, timer_highres_ ? "high_res" : "sleep_1ms");
        detail::put(sink, user,
                    "cpu profile: leaf samples=%u; the top %u module+RVA by the instruction the thread was on (count, share)",
                    valid_, kTopN);
        std::vector<const LocRec*> locs;
        locs_.each([&](const LocRec& r) { locs.push_back(&r); });
        topSort(locs, [](const LocRec* a, const LocRec* b) {
            return a->count != b->count ? a->count > b->count : a->key < b->key;
        });
        for (unsigned i = 0; i < std::min<size_t>(kTopN, locs.size()); ++i) {
            const LocRec& r = *locs[i];
            char where[64];
            locName(r.mod, r.rva, where, sizeof(where));
            detail::put(sink, user, "cpu profile: leaf %2u %7u %6.2f%% %s", i + 1, r.count, r.count * pctOf, where);
        }
        detail::put(sink, user,
                    "cpu profile: fn samples=%u; the top %u functions from .pdata, inclusive (anywhere on the stack) "
                    "and exclusive (the leaf)", valid_, kTopN);
        std::vector<const FnRec*> fns;
        fns_.each([&](const FnRec& r) { fns.push_back(&r); });
        topSort(fns, [](const FnRec* a, const FnRec* b) {
            return a->incl != b->incl ? a->incl > b->incl : a->key < b->key;
        });
        for (unsigned i = 0; i < std::min<size_t>(kTopN, fns.size()); ++i) {
            const FnRec& r = *fns[i];
            char where[96];
            fnName(r, where, sizeof(where));
            detail::put(sink, user, "cpu profile: fn %2u incl %7u %6.2f%% excl %7u %6.2f%% %s", i + 1, r.incl,
                        r.incl * pctOf, r.excl, r.excl * pctOf, where);
        }
        detail::put(sink, user,
                    "cpu profile: class samples=%u; leaf = the leaf frame's module, on_stack = a frame anywhere in it", valid_);
        for (unsigned c = 0; c < static_cast<unsigned>(Class::Count); ++c) {
            detail::put(sink, user, "cpu profile: class %s leaf %u %6.2f%% on_stack %u %6.2f%%",
                        className(static_cast<Class>(c)), leafClass_[c], leafClass_[c] * pctOf, stackClass_[c],
                        stackClass_[c] * pctOf);
        }
        std::vector<const ModuleRec*> mods;
        for (unsigned m = 0; m < nMods_; ++m) mods.push_back(&mods_[m]);
        topSort(mods, [](const ModuleRec* a, const ModuleRec* b) {
            return a->leaf != b->leaf ? a->leaf > b->leaf : std::strcmp(a->name, b->name) < 0;
        });
        detail::put(sink, user, "cpu profile: module top=%u by leaf share; modules loaded in the process: %u",
                    static_cast<unsigned>(std::min<size_t>(kTopModules, mods.size())), nMods_);
        for (unsigned i = 0; i < std::min<size_t>(kTopModules, mods.size()); ++i) {
            const ModuleRec& m = *mods[i];
            if (!m.leaf && !m.stack) continue;
            detail::put(sink, user, "cpu profile: module %s leaf %u %6.2f%% on_stack %u %6.2f%%", m.name, m.leaf,
                        m.leaf * pctOf, m.stack, m.stack * pctOf);
        }
        detail::put(sink, user, "cpu profile: end samples=%u valid=%u csv=%s%s", count_, valid_, info.csvName,
                    valid_ ? "" : " (no sample read the thread: the leaf and function tables are empty)");
    }

    // The raw samples as CSV: the contract tools\elite_hotspots.py reads (and edvr_log.py --cpu-profile <csv>).
    //   one row per valid sample: module,rva_hex,frame1_module,frame1_rva_hex,...,frame8_module,frame8_rva_hex
    //   module is the file name of the module holding the leaf ("?" when none does), rva_hex is hex without 0x,
    //   frames run innermost first, at most 8 (the leaf is not repeated), and a short row ends the frame list.
    //   Lines starting # are comments: the build, the sample rate and the window, plus two comment tables the
    //   re-aggregation needs (# module,<name>,<class> and # function,<name>,<begin hex>,<end hex> from .pdata).
    //   The optional header line starts with module,rva_hex.
    bool writeCsv(FILE* f, const RunInfo& info) const {
        if (!f || !resolved_) return false;
        constexpr unsigned kCsvFrames = 8;
        std::fprintf(f, "# edvr cpu profile raw samples: one row per valid sample, the leaf first, then up to 8 return "
                        "frames, innermost first; a short row ends its frame list\n");
        std::fprintf(f, "# build=%s thread=%lu trigger=%s window_s=%.3f sample_rate_target_hz=%.1f achieved_hz=%.2f "
                        "samples=%u valid=%u failures=%u\n",
                     info.build, static_cast<unsigned long>(info.thread), info.trigger, info.windowS, info.targetHz,
                     achievedHz(), count_, valid_, failures_);
        for (unsigned m = 0; m < nMods_; ++m) {
            std::fprintf(f, "# module,%s,%s\n", mods_[m].name, className(mods_[m].cls));
        }
        fns_.each([&](const FnRec& r) {
            if (r.mod >= 0 && r.begin != kNoFunction)
                std::fprintf(f, "# function,%s,%x,%x\n", mods_[r.mod].name, r.begin, r.end);
        });
        std::fprintf(f, "module,rva_hex");
        for (unsigned j = 1; j <= kCsvFrames; ++j) std::fprintf(f, ",frame%u_module,frame%u_rva_hex", j, j);
        std::fputc('\n', f);
        for (unsigned i = 0; i < count_; ++i) {
            const Sample& s = samples_[i];
            if (!s.frames) continue;
            std::fprintf(f, "%s,%x", nameOf(s.mod[0]), s.rva[0]);
            for (unsigned j = 1; j < s.frames && j <= kCsvFrames; ++j)
                std::fprintf(f, ",%s,%x", nameOf(s.mod[j]), s.rva[j]);
            std::fputc('\n', f);
        }
        return std::ferror(f) == 0;
    }

private:
    // The index of the module holding `pc`, adding it to the table the first time it is seen. -1 when none holds it.
    int findModule(uintptr_t pc, uintptr_t& base) noexcept {
        base = 0;
        HMODULE h = nullptr;
        if (!pc || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(pc), &h) || !h)
            return -1;
        base = reinterpret_cast<uintptr_t>(h);
        for (unsigned m = 0; m < nMods_; ++m) {
            if (mods_[m].base == base) return static_cast<int>(m);
        }
        if (nMods_ >= kMaxModules) return -1;
        ModuleRec& r = mods_[nMods_];
        r = ModuleRec{};
        r.base = base;
        wchar_t path[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(h, path, MAX_PATH);
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        DWORD start = 0;
        for (DWORD i = 0; i < len; ++i) {
            if (path[i] == L'\\' || path[i] == L'/') start = i + 1;
        }
        size_t n = 0;
        for (DWORD i = start; i < len && n + 1 < sizeof(r.name); ++i) r.name[n++] = path[i] < 0x80 ? static_cast<char>(path[i]) : '?';
        r.name[n] = 0;
        if (!n) std::snprintf(r.name, sizeof(r.name), "?");
        r.cls = classify(r.name, h == stall::thisModule());
        loadPdata(base, r.fns, r.nfns);
        return static_cast<int>(nMods_++);
    }

    // The module's file name for a frame, "?" when no module holds it.
    const char* nameOf(int mod) const noexcept {
        return mod >= 0 && static_cast<unsigned>(mod) < nMods_ ? mods_[mod].name : "?";
    }

    void locName(int mod, uint32_t rva, char* out, size_t cap) const noexcept {
        if (mod >= 0) std::snprintf(out, cap, "%s+0x%x", mods_[mod].name, rva);
        else std::snprintf(out, cap, "?+0x%x (no module)", rva);
    }

    void fnName(const FnRec& r, char* out, size_t cap) const noexcept {
        if (r.mod < 0) std::snprintf(out, cap, "?+0x%x (no module)", r.begin);
        else if (r.begin == kNoFunction) std::snprintf(out, cap, "%s (no .pdata entry for the frame)", mods_[r.mod].name);
        else std::snprintf(out, cap, "%s+0x%x..0x%x", mods_[r.mod].name, r.begin, r.end);
    }

    template <typename P, typename Less>
    static void topSort(std::vector<P>& v, Less less) {
        const size_t k = std::min<size_t>(kTopN, v.size());
        std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end(), less);
    }

    std::unique_ptr<Sample[]> samples_;
    unsigned cap_ = 0;
    unsigned count_ = 0;
    unsigned valid_ = 0;
    uint32_t failures_ = 0;
    bool bufferFull_ = false;
    bool resolved_ = false;
    bool timer_highres_ = false;
    double wallUs_ = 0.0;
    double suspendTotalUs_ = 0.0;
    uint32_t suspendMaxUs_ = 0;

    std::unique_ptr<ModuleRec[]> mods_;
    unsigned nMods_ = 0;
    Table<LocRec> locs_;
    Table<FnRec> fns_;
    uint32_t leafClass_[static_cast<unsigned>(Class::Count)] = {};
    uint32_t stackClass_[static_cast<unsigned>(Class::Count)] = {};
};

}  // namespace edvr::cpu
