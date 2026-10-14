// The process CPU profile, tested on threads with known hot functions (src\common\cpu_sampler.h, src\d3d11\cpu_profile.cpp,
// docs\render-thread-cpu-2026-10-10.md). The real choice, capture, resolution, report and CSV code runs here against three
// busy threads of this exe and the rig's primary thread:
//   render  calls hot_outer in hot_target.dll (hot_spin_hot is three quarters of it), sleeps now and then;
//   other   spins hot_cold_loop in hot_target.dll (its top function is hot_spin_cold), the busiest of the others;
//   idle    sleeps.
// The choice and the sampling run on a worker thread of this exe, as the glue's do, so the primary thread is listed as main.
//
//   cpu_sampler_test.exe --dry-run                       touches nothing
//   cpu_sampler_test.exe --self-test <root> <outdir>     root is the repository (its source is checked); outdir gets
//                                                        cpu_profile_report.log and cpu_profile_rig.csv, which the build
//                                                        then gives to edvr_log.py --cpu-profile
//
// Checks: the labels (render, main, and the busy thread) are chosen as they should be; the busy thread is the busiest of the
// others; every thread is sampled and the round-robin is balanced; the hot leaf of the render thread and the cold leaf of the
// busy thread dominate their own tables; every listed .pdata range equals RtlLookupFunctionEntry's; the report's and the CSV's
// lines parse; the producer wait is refused on a process without the game (and the gate accepts only the game's stamp); the
// pacer holds the stopped-share cap on a fake clock; the window calls nothing that stops a thread but stall::captureThread.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cpu_sampler.h"

namespace {

using edvr::cpu::Class;
using edvr::cpu::Pacer;
using edvr::cpu::Session;
using edvr::cpu::Target;

int g_fails = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

void collect(const char* line, void* user) {
    static_cast<std::vector<std::string>*>(user)->push_back(line);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string exeDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

// ---- the busy threads --------------------------------------------------------------------------------------------------

using OuterFn = void (*)();
using LoopFn = void (*)(volatile LONG*);

struct Busy {
    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    DWORD tid = 0;
    uint64_t lo = 0, hi = 0;
    OuterFn outer = nullptr;
    LoopFn loop = nullptr;
    LONG loopStop = 0;
    unsigned calls = 0;
    // The render role's Presents: a QueryPerformanceCounter stamp about every 8 ms, in a fixed array the worker reads by count.
    std::vector<uint64_t> stamps = std::vector<uint64_t>(65536);
    std::atomic<unsigned> stampCount{0};
};

// The render-role thread: registers its own stack, then calls hot_outer, sleeping a few milliseconds every 64th call (a
// small wait share, so the hot leaf leads the render table and the wait class still has samples).
void renderMain(Busy* b) {
    b->tid = GetCurrentThreadId();
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    b->lo = lo;
    b->hi = hi;
    b->ready.store(true);
    LARGE_INTEGER freq{}, last{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (!b->stop.load()) {
        b->outer();
        if ((++b->calls % 64u) == 0) Sleep(3);
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - last.QuadPart) * 1000 >= 8 * freq.QuadPart) {
            last = now;
            const unsigned n = b->stampCount.load(std::memory_order_relaxed);
            if (n < 65536) {
                b->stamps[n] = static_cast<uint64_t>(now.QuadPart);
                b->stampCount.store(n + 1, std::memory_order_release);
            }
        }
    }
}

// The busy thread: spins hot_cold_loop until told to stop. Its stack is not registered; the choice reads it from the TEB.
void busyMain(Busy* b) {
    b->tid = GetCurrentThreadId();
    b->ready.store(true);
    b->loop(&b->loopStop);
}

// The idle thread: sleeps in short steps.
void idleMain(Busy* b) {
    b->tid = GetCurrentThreadId();
    b->ready.store(true);
    while (!b->stop.load()) Sleep(5);
}

// ---- report parsing (the rig reads its own lines back: the format is the contract) -------------------------------------

struct FnRow {
    std::string thread;
    unsigned idx = 0, incl = 0, excl = 0;
    double inclPct = 0.0, exclPct = 0.0;
    std::string module;
    bool hasRange = false;
    uint32_t begin = 0, end = 0;
};

bool parseFnRow(const std::string& line, FnRow& r) {
    char thread[32] = {}, label[160] = {};
    unsigned idx = 0, incl = 0, excl = 0;
    double ip = 0.0, ep = 0.0;
    if (std::sscanf(line.c_str(), "cpu profile: thread=%31s fn %u incl %u %lf%% excl %u %lf%% %159s", thread, &idx, &incl,
                    &ip, &excl, &ep, label) != 7)
        return false;
    r.thread = thread;
    r.idx = idx;
    r.incl = incl;
    r.excl = excl;
    r.inclPct = ip;
    r.exclPct = ep;
    const char* plus = std::strchr(label, '+');
    r.module.assign(label, plus ? static_cast<size_t>(plus - label) : std::strlen(label));
    unsigned b = 0, e = 0;
    if (plus && std::sscanf(plus, "+0x%x..0x%x", &b, &e) == 2) {
        r.hasRange = true;
        r.begin = b;
        r.end = e;
    }
    return true;
}

// The class lines: "cpu profile: thread=<t> class <name> leaf <n> <p>% on_stack <n> <p>%".
bool parseClassRow(const std::string& line, std::string& thread, std::string& name, unsigned& leaf, unsigned& onStack) {
    char t[32] = {}, text[64] = {};
    double lp = 0.0, sp = 0.0;
    if (std::sscanf(line.c_str(), "cpu profile: thread=%31s class %63s leaf %u %lf%% on_stack %u %lf%%", t, text, &leaf,
                    &lp, &onStack, &sp) != 6)
        return false;
    thread = t;
    name = text;
    return true;
}

// The thread line: "cpu profile: thread=<t> role=<r> start=<s> cpu_ms=<x> cpu_pct=<p>% valid=<n> failures=<f> rate_hz=<h>".
bool parseThreadLine(const std::string& line, std::string& thread, std::string& role, double& cpuMs, unsigned& valid,
                     unsigned& failures) {
    char t[32] = {}, r[16] = {}, s[120] = {};
    double pct = 0.0, rate = 0.0;
    if (std::sscanf(line.c_str(), "cpu profile: thread=%31s role=%15s start=%119s cpu_ms=%lf cpu_pct=%lf%% valid=%u failures=%u "
                                  "rate_hz=%lf", t, r, s, &cpuMs, &pct, &valid, &failures, &rate) != 8)
        return false;
    thread = t;
    role = r;
    return true;
}

bool oracleRange(const std::string& name, uint32_t rva, uint32_t& begin, uint32_t& end) {
    HMODULE m = GetModuleHandleA(name.c_str());
    if (!m) return false;
    const DWORD64 base = reinterpret_cast<DWORD64>(m);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(base + rva, &imageBase, nullptr);
    if (!f || imageBase != base) return false;
    begin = f->BeginAddress;
    end = f->EndAddress;
    return true;
}

// ---- the CSV, read back as tools\elite_hotspots.py will (the contract in cpu_sampler.h's Session::writeCsv) ------------

struct CsvCheck {
    unsigned rows = 0;
    unsigned badRows = 0;             // a frame list that is not in pairs, or longer than 8 frames
    unsigned leafNamed = 0;           // leaves in a named module
    unsigned leafInFunction = 0;      // of those, the leaves a "# function" range holds
    std::map<std::string, unsigned> rowsByThread;
};

CsvCheck checkCsv(const std::string& text) {
    CsvCheck c;
    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> ranges;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            char name[64] = {};
            unsigned b = 0, e = 0;
            if (line.compare(0, 11, "# function,") == 0 &&
                std::sscanf(line.c_str(), "# function,%63[^,],%x,%x", name, &b, &e) == 3) {
                ranges[name].push_back({b, e});
            }
            continue;
        }
        if (line.compare(0, 14, "thread,module,") == 0) continue;   // the optional header line
        std::vector<std::string> f;
        std::string cell;
        std::istringstream row(line);
        while (std::getline(row, cell, ',')) f.push_back(cell);
        if (f.size() < 3) {
            ++c.badRows;
            continue;
        }
        ++c.rows;
        ++c.rowsByThread[f[0]];
        const size_t frameCells = f.size() - 3;
        if (frameCells % 2 != 0 || frameCells > 16) ++c.badRows;
        const std::string& mod = f[1];
        const uint32_t rva = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 16));
        if (mod == "?") continue;
        ++c.leafNamed;
        for (const auto& r : ranges[mod]) {
            if (rva >= r.first && rva < r.second) {
                ++c.leafInFunction;
                break;
            }
        }
    }
    return c;
}

// ---- the tests ---------------------------------------------------------------------------------------------------------

bool testClassify() {
    std::printf("classification by file name:\n");
    bool ok = true;
    ok &= (Class::Elite == edvr::cpu::classify("EliteDangerous64.exe", false));
    ok &= (Class::EdvrOpenvr == edvr::cpu::classify("openvr_api.dll", false));
    ok &= (Class::NtdllKernel == edvr::cpu::classify("ntdll.dll", false));
    ok &= (Class::NtdllKernel == edvr::cpu::classify("KernelBase.dll", false));
    ok &= (Class::D3dDriver == edvr::cpu::classify("nvwgf2umx.dll", false));
    ok &= (Class::D3dDriver == edvr::cpu::classify("dxgi.dll", false));
    ok &= (Class::Vendor == edvr::cpu::classify("OVRRT64_0.dll", false));
    ok &= (Class::Vendor == edvr::cpu::classify("nvngx_dlss.dll", false));
    ok &= (Class::Other == edvr::cpu::classify("hot_target.dll", false));
    ok &= (Class::EdvrD3d11 == edvr::cpu::classify("d3d11.dll", true));
    ok &= (Class::Unknown == edvr::cpu::classify("?", false));
    check(ok, "every class from its file name, and this DLL's own module by identity");
    return ok;
}

bool testPacer() {
    std::printf("pacer, on a fake clock:\n");
    const double period = 1000.0, cap = 0.05;
    bool ok = true;
    ok &= Pacer::nextDueUs(0, 0, 30, period, cap) == 1000.0;     // a 30 us stop: the period governs
    ok &= Pacer::nextDueUs(0, 0, 100, period, cap) == 2000.0;    // a 100 us stop: 100 / 0.05 = 2000 us
    ok &= Pacer::nextDueUs(1000, 1500, 10, period, cap) == 2000.0;
    ok &= Pacer::nextDueUs(0, 9000, 10, period, cap) == 9000.0;  // far behind: restart from now, no burst
    check(ok, "next due: the period, a long stop's spacing, and no catch-up burst after a lag");
    bool duty = true;
    for (double stop : {30.0, 100.0, 250.0}) {
        unsigned n = 0;
        double due = 0.0;
        while (due < 10e6) {
            ++n;
            due = Pacer::nextDueUs(due, due, stop, period, cap);
        }
        const double stoppedShare = n * stop / 10e6;
        std::printf("    stop %5.0f us: %5u samples in 10 s, stopped %.2f%% of the time\n", stop, n, stoppedShare * 100.0);
        duty = duty && stoppedShare <= cap + 1e-9;
    }
    check(duty, "the stopped share never exceeds the cap, at any suspend cost");
    return ok && duty;
}

bool testGate() {
    std::printf("the producer wait's gate:\n");
    const bool ok = edvr::cpu::eliteBuildGate(edvr::cpu::kEliteStamp, edvr::cpu::kEliteImageSize) &&
                    !edvr::cpu::eliteBuildGate(edvr::cpu::kEliteStamp, 1) &&
                    !edvr::cpu::eliteBuildGate(1, edvr::cpu::kEliteImageSize);
    check(ok, "the gate accepts the game's stamp and image size, and refuses either one alone");
    return ok;
}

bool testSourceWindow(const std::string& root) {
    std::printf("the window's source:\n");
    const std::string text = readFile(root + "\\src\\common\\cpu_sampler.h");
    if (text.empty()) {
        check(false, "src\\common\\cpu_sampler.h readable");
        return false;
    }
    auto count = [&](const char* token) {
        unsigned n = 0;
        for (size_t p = text.find(token); p != std::string::npos; p = text.find(token, p + 1)) ++n;
        return n;
    };
    bool ok = true;
    ok &= count("SuspendThread(") == 0;
    ok &= count("ResumeThread(") == 0;
    ok &= count("SetThreadContext(") == 0;
    ok &= count("GetThreadContext(") == 0;
    ok &= count("stall::captureThread(") == 1;   // the one call that stops a thread, in Session::sample
    ok &= count("Log::") == 0;                    // the header logs nothing: the glue passes the sink
    check(ok, "no stop or context call outside stall::captureThread; the header logs nothing");
    return ok;
}

// A number after `key` in a log line, or -1.
double kv(const std::string& line, const char* key) {
    const size_t at = line.find(key);
    return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + std::strlen(key));
}

const std::string* findLine(const std::vector<std::string>& lines, const std::string& prefix, const char* also = nullptr) {
    for (const std::string& l : lines) {
        if (l.find(prefix) == 0 && (!also || l.find(also) != std::string::npos)) return &l;
    }
    return nullptr;
}

// The operating system's own wait stubs, found by export table: what stops a sample counting as running.
bool testStubs() {
    using edvr::cpu::StubKind;
    bool ok = edvr::cpu::stubKindOfExport("NtWaitForSingleObject") == StubKind::Event &&
              edvr::cpu::stubKindOfExport("ZwWaitForMultipleObjects") == StubKind::Event &&
              edvr::cpu::stubKindOfExport("NtWaitForAlertByThreadId") == StubKind::Park &&
              edvr::cpu::stubKindOfExport("NtDelayExecution") == StubKind::Sleep &&
              edvr::cpu::stubKindOfExport("NtUserMsgWaitForMultipleObjectsEx") == StubKind::Event &&
              edvr::cpu::stubKindOfExport("NtClose") == StubKind::None && edvr::cpu::stubKindOfExport("memcpy") == StubKind::None &&
              edvr::cpu::stubKindOfExport("") == StubKind::None && edvr::cpu::stubKindOfExport(nullptr) == StubKind::None;
    check(ok, "export names: waits by kind (event, park, sleep), everything else None");
    // The real ntdll: the stubs found by address, inside their span, and a neighbour that does not wait.
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    std::vector<edvr::cpu::ExportRec> exports;
    edvr::cpu::loadExports(reinterpret_cast<uintptr_t>(nt), exports);
    auto rvaOf = [&](const char* n) {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetProcAddress(nt, n)) - reinterpret_cast<uintptr_t>(nt));
    };
    const uint32_t wso = rvaOf("NtWaitForSingleObject"), alert = rvaOf("NtWaitForAlertByThreadId"),
                   delay = rvaOf("NtDelayExecution"), close = rvaOf("NtClose");
    check(exports.size() > 500, "ntdll's export table is read (hundreds of named exports)");
    check(edvr::cpu::stubKindAt(exports, wso) == StubKind::Event && edvr::cpu::stubKindAt(exports, wso + 0x12) == StubKind::Event,
          "a leaf in NtWaitForSingleObject's stub is an event wait");
    check(edvr::cpu::stubKindAt(exports, alert + 0x12) == StubKind::Park, "a leaf in NtWaitForAlertByThreadId is a park (lock)");
    check(edvr::cpu::stubKindAt(exports, delay + 0x12) == StubKind::Sleep, "a leaf in NtDelayExecution is a sleep");
    check(edvr::cpu::stubKindAt(exports, close + 0x12) == StubKind::None, "a leaf in NtClose is not a wait");
    check(edvr::cpu::stubKindAt(exports, 0) == StubKind::None, "an RVA below every export is not a wait");
    using edvr::cpu::WaitKind;
    using edvr::cpu::attributeSample;
    const bool attr = attributeSample(StubKind::None, true, true, true) == WaitKind::Busy &&
                      attributeSample(StubKind::Event, true, true, true) == WaitKind::Handshake &&
                      attributeSample(StubKind::Park, false, true, true) == WaitKind::Xr &&
                      attributeSample(StubKind::Event, false, false, true) == WaitKind::Gpu &&
                      attributeSample(StubKind::Park, false, false, false) == WaitKind::Lock &&
                      attributeSample(StubKind::Event, false, false, false) == WaitKind::Event &&
                      attributeSample(StubKind::Sleep, false, false, false) == WaitKind::Sleep;
    check(attr, "a sample is busy unless its leaf waits; a wait is named by the handshake, then the runtime, then the GPU, then the stub");
    return ok && attr;
}

// The frame model on a window written by hand: frames are the Present intervals wholly inside the window, each sample lands in
// one, and a frame's critical path comes from what the render thread was waiting on.
bool testFrameModel() {
    using namespace edvr::cpu;
    // Targets: 0 render, 1 producer. Presents at 0, 10, 20, 30, 40 ms; the window ends at 45 ms, so four frames, 10 ms each, and
    // the stretch after 40 ms is not a frame.
    const double presents[] = {0, 10000, 20000, 30000, 40000};
    std::vector<FrameSample> s;
    auto add = [&](uint32_t us, uint8_t target, WaitKind k, uint8_t edvr = 0) {
        FrameSample f;
        f.tUs = us;
        f.target = target;
        f.kind = static_cast<uint8_t>(k);
        f.edvrLeaf = edvr;
        s.push_back(f);
    };
    // Frame 0: the render thread never waits (three busy samples, one in EDVR); the producer is busy twice.
    add(1000, 0, WaitKind::Busy); add(2000, 1, WaitKind::Busy); add(3000, 0, WaitKind::Busy, 1); add(5000, 1, WaitKind::Busy);
    add(8000, 0, WaitKind::Busy);
    // Frame 1: three of four render samples wait in the handshake.
    add(11000, 0, WaitKind::Handshake); add(12000, 0, WaitKind::Handshake); add(14000, 0, WaitKind::Handshake);
    add(18000, 0, WaitKind::Busy); add(13000, 1, WaitKind::Busy);
    // Frame 2: the render thread waits on the runtime (two) and on a lock (one), busy once: the runtime is the longest wait.
    add(21000, 0, WaitKind::Xr); add(23000, 0, WaitKind::Xr); add(25000, 0, WaitKind::Lock); add(28000, 0, WaitKind::Busy);
    // Frame 3: the GPU, two of two.
    add(31000, 0, WaitKind::Gpu); add(35000, 0, WaitKind::Gpu);
    // After the last Present: not in any frame.
    add(42000, 0, WaitKind::Handshake);
    std::stable_sort(s.begin(), s.end(), [](const FrameSample& a, const FrameSample& b) { return a.tUs < b.tUs; });
    std::vector<FrameRow> rows;
    buildFrames(s.data(), static_cast<unsigned>(s.size()), 2, 0, presents, 5, 45000.0, rows);
    bool ok = rows.size() == 4;
    ok = ok && rows[0].n[0] == 3 && rows[0].busy[0] == 3 && rows[0].edvr[0] == 1 && rows[0].n[1] == 2 && rows[0].crit == Crit::Render;
    ok = ok && rows[1].n[0] == 4 && rows[1].renderKind[static_cast<unsigned>(WaitKind::Handshake)] == 3 && rows[1].crit == Crit::Producer;
    ok = ok && rows[2].crit == Crit::Xr && rows[3].crit == Crit::Gpu;
    ok = ok && rows[3].frameMs > 9.99 && rows[3].frameMs < 10.01 && rows[3].startMs > 29.99 && rows[3].startMs < 30.01;
    check(ok, "frames: the Present intervals, each sample in one, the sample after the last Present in none");
    // A frame the render thread has no sample in is unknown, not render-bound; a window cutting a frame drops it.
    std::vector<FrameRow> cut;
    buildFrames(s.data(), static_cast<unsigned>(s.size()), 2, 0, presents, 5, 35000.0, cut);
    FrameRow empty;
    const bool edge = cut.size() == 3 && classifyFrame(empty, 0) == Crit::Unknown;
    check(edge, "frames: a frame that runs past the window is dropped; a frame with no render sample is unknown");
    FrameRow tie;
    tie.n[0] = 4;
    tie.renderKind[static_cast<unsigned>(WaitKind::Handshake)] = 2;
    tie.renderKind[static_cast<unsigned>(WaitKind::Gpu)] = 2;
    FrameRow low;
    low.n[0] = 8;
    low.renderKind[static_cast<unsigned>(WaitKind::Handshake)] = 1;
    const bool crit = classifyFrame(tie, 0) == Crit::Producer && classifyFrame(low, 0) == Crit::Render;
    check(crit, "critical path: a tie goes to the producer; one wait in eight is still the render thread's own frame");
    return ok && edge && crit;
}

// The accounting lines of the live window: each thread's per-frame, wait and busy split; the producer; the critical path.
void checkAccounting(const std::vector<std::string>& lines, const Session& session, unsigned picked, unsigned presents,
                     const char* busyLabel, const std::string& outDir) {
    unsigned perFrame = 0, waitSplit = 0, busySplit = 0;
    for (const std::string& l : lines) {
        if (l.find(" per_frame frames=") != std::string::npos) ++perFrame;
        if (l.find(" wait_split frame_ms=") != std::string::npos) ++waitSplit;
        if (l.find(" busy_split elite=") != std::string::npos) ++busySplit;
    }
    check(perFrame == picked && waitSplit == picked && busySplit == picked,
          "every thread has its per_frame, wait_split and busy_split line");
    const std::string* pf = findLine(lines, "cpu profile: thread=render per_frame");
    const std::string* ws = findLine(lines, "cpu profile: thread=render wait_split");
    check(pf && ws, "the render thread's per_frame and wait_split lines are there");
    if (pf && ws) {
        const double frameMs = kv(*pf, "frame_ms="), busyMs = kv(*pf, "busy_ms="), waitMs = kv(*pf, "wait_ms=");
        std::printf("  render per frame: frame %.3f ms, busy %.3f, wait %.3f; Present stamps in the window: %u\n", frameMs, busyMs,
                    waitMs, presents);
        check(presents >= 50 && frameMs > 4.0 && frameMs < 40.0, "the render role stamped Presents about every 8 ms");
        check(std::fabs(busyMs + waitMs - frameMs) < 0.01 * frameMs + 0.002, "busy plus waiting is the frame");
        check(kv(*ws, " sleep=") > 0.5, "the render thread's Sleep calls are counted as sleep waits (NtDelayExecution)");
        check(kv(*ws, " busy=") > 1.0, "and its hot loop as busy time");
        check(kv(*pf, "edvr_stack_ms=") > 0.0, "an EDVR-class frame on the render stack (the rig exe stands in for EDVR) is counted");
    }
    const std::string* prod = findLine(lines, "cpu profile: producer thread=");
    check(prod && prod->find(std::string("thread=") + busyLabel + " ") != std::string::npos,
          "the producer is the busiest thread besides render and owner (the spinning one)");
    if (prod) check(kv(*prod, "busy_pct_of_frame=") > 80.0, "the producer line reports it busy for most of the frame");
    const std::string* crit = findLine(lines, "cpu profile: critical frames=");
    unsigned frames = 0, sum = 0;
    if (crit) {
        frames = static_cast<unsigned>(kv(*crit, "frames="));
        sum = static_cast<unsigned>(kv(*crit, " producer=") + kv(*crit, " render=") + kv(*crit, " xr=") + kv(*crit, " gpu=") +
                                    kv(*crit, " sync=") + kv(*crit, " unknown="));
    }
    check(crit && frames >= 50 && sum == frames, "the critical line counts every complete frame once");
    check(session.frames().size() == frames, "the report's frame count is the model's");
    unsigned windows = 0;
    for (const std::string& l : lines) {
        if (l.find("cpu profile: window frames=") == 0) ++windows;
    }
    check(windows >= 1 && windows <= (frames + 29) / 30, "30-frame summary lines: one per full block");
    const std::string* csvLine = findLine(lines, "cpu profile: frames_csv=");
    check(csvLine && static_cast<unsigned>(kv(*csvLine, "rows=")) == frames, "the frames_csv line names the file and its rows");
    // The frames CSV: one row per frame, a fixed column count, written and read back.
    const std::string path = outDir + "\\cpu_frames_rig.csv";
    bool wrote = false;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        wrote = session.writeFramesCsv(f);
        std::fclose(f);
    }
    check(wrote, "the frames CSV writes");
    std::istringstream in(readFile(path));
    std::string row;
    unsigned rows = 0, headerCols = 0, badCols = 0;
    while (std::getline(in, row)) {
        if (row.empty() || row[0] == '#') continue;
        const unsigned cols = static_cast<unsigned>(std::count(row.begin(), row.end(), ',')) + 1;
        if (!headerCols) { headerCols = cols; continue; }
        ++rows;
        if (cols != headerCols) ++badCols;
    }
    check(rows == frames && headerCols == 24 && badCols == 0, "the frames CSV has its header and one 24-column row per frame");
}

bool testSelectAndCapture(const std::string& outDir, bool& wroteOutput) {
    HMODULE dll = nullptr;
    const std::string dllPath = exeDirectory() + "\\hot_target.dll";
    dll = LoadLibraryA(dllPath.c_str());
    if (!dll) {
        std::printf("  FAIL  hot_target.dll not loadable from %s\n", dllPath.c_str());
        ++g_fails;
        return false;
    }
    auto outer = reinterpret_cast<OuterFn>(GetProcAddress(dll, "hot_outer"));
    auto hotLeaf = reinterpret_cast<OuterFn>(GetProcAddress(dll, "hot_spin_hot"));
    auto coldLoop = reinterpret_cast<LoopFn>(GetProcAddress(dll, "hot_cold_loop"));
    check(outer && hotLeaf && coldLoop, "hot_target.dll exports hot_outer, hot_spin_hot and hot_cold_loop");
    if (!outer || !hotLeaf || !coldLoop) return false;
    const uint32_t outerRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(outer) - reinterpret_cast<uintptr_t>(dll));
    const uint32_t hotRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hotLeaf) - reinterpret_cast<uintptr_t>(dll));

    Busy render, busy, idle;
    render.outer = outer;
    busy.loop = coldLoop;
    std::thread tRender(renderMain, &render);
    std::thread tBusy(busyMain, &busy);
    std::thread tIdle(idleMain, &idle);
    while (!render.ready.load() || !busy.ready.load() || !idle.ready.load()) Sleep(1);
    const DWORD mainTid = GetCurrentThreadId();

    // The owner: a thread created at a start address in a second test module that exports VR_GetGenericInterface. Its
    // Win32 start address is in that module, so the choice must give it role=owner.
    const std::string ownerPath = exeDirectory() + "\\owner_target.dll";
    HMODULE ownerDll = LoadLibraryA(ownerPath.c_str());
    auto ownerFn = ownerDll ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(ownerDll, "owner_test_thread")) : nullptr;
    check(ownerDll && ownerFn && GetProcAddress(ownerDll, "VR_GetGenericInterface"),
          "owner_target.dll exports owner_test_thread and VR_GetGenericInterface");
    if (!ownerDll || !ownerFn) return false;
    static LONG ownerStop = 0;
    DWORD ownerTid = 0;
    HANDLE ownerThread = CreateThread(nullptr, 0, ownerFn, &ownerStop, 0, &ownerTid);
    check(ownerThread != nullptr, "the owner-role thread starts at an address in owner_target.dll");
    if (!ownerThread) return false;

    // The choice and the sampling, on a worker of this exe (not the primary thread, which the choice must list as main).
    Session session;
    std::vector<std::string> lines;
    edvr::cpu::RunInfo info;
    info.thread = render.tid;
    info.trigger = "rig threads";
    info.windowS = 2.0;
    info.csvName = "cpu_profile_rig.csv";
    info.build = "cpu_sampler_test (no EDVR build)";
    char how[480] = {};
    unsigned picked = 0;
    unsigned windowPresents = 0;
    bool sampled = false;
    std::thread worker([&] {
        Target targets[edvr::cpu::kMaxTargets];
        picked = edvr::cpu::selectThreads(GetCurrentProcessId(), GetCurrentThreadId(), render.tid, render.lo, render.hi,
                                          edvr::cpu::kPreWindowSeconds, targets, edvr::cpu::kMaxTargets, how, sizeof(how));
        info.threads = picked;
        if (!session.prepare()) return;
        session.emitStart(&collect, &lines, info);
        const unsigned stamps0 = render.stampCount.load(std::memory_order_acquire);
        session.sample(targets, picked, info.windowS, edvr::cpu::kTargetHz, edvr::cpu::kMaxStoppedFraction);
        const unsigned stamps1 = render.stampCount.load(std::memory_order_acquire);
        windowPresents = stamps1 - stamps0;
        session.setPresents(windowPresents);
        session.setPresentStamps(render.stamps.data() + stamps0, stamps1 - stamps0);
        session.resolve();
        session.analyze();
        info.framesCsvName = "cpu_frames_rig.csv";
        session.emitReport(&collect, &lines, info);
        for (unsigned i = 0; i < picked; ++i) {
            if (targets[i].handle) CloseHandle(targets[i].handle);
        }
        sampled = true;
    });
    worker.join();
    render.stop.store(true);
    busy.loopStop = 1;
    idle.stop.store(true);
    tRender.join();
    tBusy.join();
    tIdle.join();
    InterlockedExchange(&ownerStop, 1);
    WaitForSingleObject(ownerThread, 5000);
    CloseHandle(ownerThread);   // the module stays loaded until the checks below have read its exports

    std::printf("  choice: %s\n", how);
    check(sampled, "the choice and the sampling ran to the report");
    check(picked >= 4, "the render thread, the main thread, the owner and the busy thread were chosen (four or more targets)");
    const std::string pick(how);
    char ownerText[48];
    std::snprintf(ownerText, sizeof(ownerText), "owner=t%lu", static_cast<unsigned long>(ownerTid));
    check(pick.find("owner candidates:") != std::string::npos && pick.find(ownerText) != std::string::npos,
          "the pick line names the owner candidates and the owner it chose");
    const int oIdx = session.targetIndex("owner");
    check(oIdx >= 0 && session.targetAt(static_cast<unsigned>(oIdx)).tid == ownerTid,
          "the owner-role thread is labelled owner and is the thread that started in owner_target.dll");
    if (oIdx >= 0) check(std::strcmp(session.targetAt(static_cast<unsigned>(oIdx)).role, "owner") == 0,
                         "its role is owner");
    const int rIdx = session.targetIndex("render");
    const int mIdx = session.targetIndex("main");
    char busyLabel[16];
    std::snprintf(busyLabel, sizeof(busyLabel), "t%lu", static_cast<unsigned long>(busy.tid));
    const int bIdx = session.targetIndex(busyLabel);
    check(rIdx >= 0 && mIdx >= 0 && bIdx >= 0, "the labels render, main and the busy thread are present");
    if (bIdx >= 0) check(std::strcmp(session.targetAt(static_cast<unsigned>(bIdx)).role, "other") == 0,
                         "the busy thread, started in the rig's own exe, is not the owner (role other)");
    if (mIdx >= 0) check(session.targetAt(static_cast<unsigned>(mIdx)).tid == mainTid, "main is the primary thread (the earliest created)");
    if (rIdx >= 0) check(session.targetAt(static_cast<unsigned>(rIdx)).tid == render.tid, "render is the thread that presents");
    if (bIdx >= 0) {
        const Target& bt = session.targetAt(static_cast<unsigned>(bIdx));
        const double busyPct = 100.0 * static_cast<double>(bt.cpuEnd - bt.cpuStart) / 10000.0 / (session.wallUs() / 1000.0);
        std::printf("  busy thread CPU over the window: %.1f%%\n", busyPct);
        check(busyPct >= 50.0, "the busy thread used at least half the window's wall time");
    }

    // Every chosen thread is sampled, and the round-robin shares its samples evenly.
    unsigned lo = ~0u, hi = 0;
    bool everyValid = true;
    for (unsigned t = 0; t < session.targetCount(); ++t) {
        const unsigned v = session.targetValid(t);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        everyValid = everyValid && v > 0;
    }
    std::printf("  samples per thread: min %u, max %u of %u valid\n", lo, hi, session.valid());
    check(everyValid && hi <= 2 * lo + lo / 4 + 4, "every chosen thread is read, and the weighted round-robin is balanced (render and the busiest other get two slots)");

    // The stack registration of the busy thread comes from its TEB, not from its own call: its samples must not all fail.
    if (bIdx >= 0) {
        const unsigned bValid = session.targetValid(static_cast<unsigned>(bIdx));
        check(bValid * 2 >= session.samples() / session.targetCount(),
              "the busy thread's stack was read from its TEB (its samples are not failures)");
    }

    // The report's lines: the thread lines, the tops, the wait.
    std::vector<FnRow> fns;
    std::map<std::string, unsigned> threadLines;
    std::map<std::string, std::map<std::string, std::pair<unsigned, unsigned>>> classes;
    unsigned lineErrors = 0;
    bool sawStart = false, sawEnd = false, sawPacing = false, sawWaitLine = false, waitNotMeasured = false;
    for (const std::string& line : lines) {
        if (line.size() >= 1100 || line.compare(0, 12, "cpu profile:") != 0) {
            if (!lineErrors) std::printf("    first bad line (%zu chars): %s\n", line.size(), line.c_str());
            ++lineErrors;
        }
        if (line.find("cpu profile: start") == 0) sawStart = true;
        if (line.find("cpu profile: end") == 0) sawEnd = true;
        if (line.find("cpu profile: pacing") == 0) sawPacing = true;
        if (line.find("waits for producer") != std::string::npos) {
            sawWaitLine = true;
            if (line.find("not measured") != std::string::npos) waitNotMeasured = true;
        }
        std::string thread, role;
        double cpuMs = 0.0;
        unsigned valid = 0, failures = 0;
        if (line.find("cpu profile: thread=") == 0 && line.find(" role=") != std::string::npos &&
            parseThreadLine(line, thread, role, cpuMs, valid, failures))
            ++threadLines[thread];
        FnRow r;
        if (line.find("cpu profile: thread=") == 0 && line.find(" fn ") != std::string::npos && parseFnRow(line, r))
            fns.push_back(r);
        std::string ct, cn;
        unsigned leaf = 0, stack = 0;
        if (line.find(" class ") != std::string::npos && parseClassRow(line, ct, cn, leaf, stack))
            classes[ct][cn] = {leaf, stack};
    }
    check(lineErrors == 0, "every report line starts 'cpu profile:' and is under 1100 characters");
    check(sawStart && sawPacing && sawEnd, "the report has its start, pacing and end lines");
    check(threadLines.size() == picked, "one thread line per chosen thread");
    check(sawWaitLine && waitNotMeasured, "the producer wait is refused on a process without the game (not measured)");

    // The leaders of the tables: the render thread's top exclusive function is hot_spin_hot; the busy thread's is hot_spin_cold.
    auto topExcl = [&](const std::string& thread) -> const FnRow* {
        const FnRow* best = nullptr;
        for (const FnRow& r : fns) {
            if (r.thread == thread && (!best || r.excl > best->excl)) best = &r;
        }
        return best;
    };
    const FnRow* rTop = topExcl("render");
    if (rTop) std::printf("  render top exclusive: %s+0x%x..0x%x (hot_spin_hot at 0x%x)\n", rTop->module.c_str(), rTop->begin,
                          rTop->end, hotRva);
    check(rTop && rTop->hasRange && rTop->module == "hot_target.dll" && rTop->begin <= hotRva && hotRva < rTop->end,
          "the render thread's top exclusive function is hot_spin_hot in hot_target.dll");
    if (rTop) std::printf("  render hot_spin_hot leaf share %.2f%% of its valid samples\n", rTop->exclPct);
    if (rTop) check(rTop->exclPct >= 25.0, "hot_spin_hot is at least a quarter of the render thread's samples");
    bool outerOk = false;
    for (const FnRow& r : fns) {
        if (r.thread == "render" && r.hasRange && r.module == "hot_target.dll" && r.begin <= outerRva && outerRva < r.end) {
            outerOk = r.inclPct >= 40.0 && r.inclPct > r.exclPct;
            std::printf("  render hot_outer inclusive %.2f%%, exclusive %.2f%%\n", r.inclPct, r.exclPct);
        }
    }
    check(outerOk, "hot_outer is on at least 40% of the render thread's stacks");
    const FnRow* bTop = nullptr;
    if (bIdx >= 0) {
        std::snprintf(busyLabel, sizeof(busyLabel), "t%lu", static_cast<unsigned long>(busy.tid));
        bTop = topExcl(busyLabel);
    }
    check(bTop && bTop->module == "hot_target.dll" && bTop->exclPct >= 50.0,
          "the busy thread's top exclusive function is in hot_target.dll and holds at least half its samples");
    const bool wait = classes.count("render") && classes["render"].count("ntdll_kernel") &&
                      classes["render"]["ntdll_kernel"].first > 0;
    check(wait, "the render thread's wait class (its Sleep calls) has samples");

    // .pdata: every listed range is the OS's own RUNTIME_FUNCTION for that address.
    unsigned checked = 0, mismatched = 0;
    for (const FnRow& r : fns) {
        if (!r.hasRange) continue;
        uint32_t ob = 0, oe = 0;
        ++checked;
        if (!oracleRange(r.module, r.begin, ob, oe) || ob != r.begin || oe != r.end) {
            if (!mismatched) {
                HMODULE mh = GetModuleHandleA(r.module.c_str());
                DWORD64 ib = 0;
                PRUNTIME_FUNCTION pf = mh ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(mh) + r.begin, &ib, nullptr) : nullptr;
                std::printf("    first mismatch: %s+0x%x..0x%x (thread %s); module %p, lookup %p imageBase %llx%s\n",
                            r.module.c_str(), r.begin, r.end, r.thread.c_str(), static_cast<void*>(mh),
                            static_cast<void*>(pf), static_cast<unsigned long long>(ib),
                            pf ? (pf->BeginAddress == r.begin ? " (begin matches)" : " (begin differs)") : " (no entry)");
            }
            ++mismatched;
        }
    }
    std::printf("  .pdata ranges checked against RtlLookupFunctionEntry: %u, mismatched %u\n", checked, mismatched);
    check(checked > 0 && mismatched == 0, "every .pdata range the report names is the OS's own");
    uint32_t ob = 0, oe = 0;
    const bool hotOracle = oracleRange("hot_target.dll", hotRva, ob, oe);
    bool hotInTable = false;
    for (const FnRow& r : fns) {
        if (r.thread == "render" && r.hasRange && r.module == "hot_target.dll" && r.begin == ob && r.end == oe) hotInTable = true;
    }
    check(hotOracle && hotInTable, "hot_spin_hot's range from the render table equals the OS's (oracle)");

    checkAccounting(lines, session, picked, windowPresents, busyLabel, outDir);

    // The CSV, written and read back as edvr_log.py reads it.
    const std::string csvPath = outDir + "\\cpu_profile_rig.csv";
    if (FILE* f = std::fopen(csvPath.c_str(), "wb")) {
        check(session.writeCsv(f, info), "the CSV writes");
        std::fclose(f);
    } else {
        check(false, "the CSV opens for writing");
    }
    const CsvCheck csv = checkCsv(readFile(csvPath));
    std::printf("  csv: %u rows; by thread:", csv.rows);
    for (const auto& kv : csv.rowsByThread) std::printf(" %s=%u", kv.first.c_str(), kv.second);
    std::printf("; leaves in a listed function %u of %u\n", csv.leafInFunction, csv.leafNamed);
    check(csv.rows == session.valid() && csv.badRows == 0,
          "one CSV row per valid sample, labelled by its thread, every frame list in module,rva pairs of at most 8 frames");
    check(csv.leafNamed > 0 && csv.leafInFunction * 100u >= csv.leafNamed * 95u,
          "95% of the CSV's named leaves fall in a listed .pdata function");

    {
        std::ofstream out(outDir + "\\cpu_profile_report.log", std::ios::binary);
        for (const std::string& line : lines) out << line << "\n";
        wroteOutput = static_cast<bool>(out);
    }
    check(wroteOutput, "the report file writes");

    // A session that never sampled says so: its report has zero samples and no thread lines.
    Session never;
    never.prepare();
    std::vector<std::string> empty;
    never.resolve();
    never.emitReport(&collect, &empty, info);
    bool emptyOk = false;
    for (const std::string& line : empty) {
        if (line.find("cpu profile: end samples=0") == 0) emptyOk = true;
    }
    check(emptyOk, "a session with no samples reports 'end samples=0', not a silent absence");

    FreeLibrary(ownerDll);
    FreeLibrary(dll);
    return g_fails == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("cpu_sampler_test: dry-run (no thread stopped, no file written)");
        return 0;
    }
    if (argc < 4 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: cpu_sampler_test.exe --dry-run | --self-test <root> <outdir>");
        return 2;
    }
    const std::string root = argv[2];
    const std::string outDir = argv[3];
    std::puts("cpu_sampler_test: the process CPU profile, on threads with known hot functions");
    testClassify();
    testPacer();
    testGate();
    testStubs();
    testFrameModel();
    testSourceWindow(root);
    bool wrote = false;
    testSelectAndCapture(outDir, wrote);
    if (g_fails) {
        std::printf("cpu_sampler_test: %d check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("cpu_sampler_test: all checks passed");
    return 0;
}
