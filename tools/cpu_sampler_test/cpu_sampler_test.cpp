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
    while (!b->stop.load()) {
        b->outer();
        if ((++b->calls % 64u) == 0) Sleep(3);
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
    bool sampled = false;
    std::thread worker([&] {
        Target targets[edvr::cpu::kMaxTargets];
        picked = edvr::cpu::selectThreads(GetCurrentProcessId(), GetCurrentThreadId(), render.tid, render.lo, render.hi,
                                          edvr::cpu::kPreWindowSeconds, targets, edvr::cpu::kMaxTargets, how, sizeof(how));
        info.threads = picked;
        if (!session.prepare()) return;
        session.emitStart(&collect, &lines, info);
        session.sample(targets, picked, info.windowS, edvr::cpu::kTargetHz, edvr::cpu::kMaxStoppedFraction);
        session.setPresents(0);
        session.resolve();
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

    std::printf("  choice: %s\n", how);
    check(sampled, "the choice and the sampling ran to the report");
    check(picked >= 3, "the render thread, the main thread and the busy thread were chosen (three or more targets)");
    const int rIdx = session.targetIndex("render");
    const int mIdx = session.targetIndex("main");
    char busyLabel[16];
    std::snprintf(busyLabel, sizeof(busyLabel), "t%lu", static_cast<unsigned long>(busy.tid));
    const int bIdx = session.targetIndex(busyLabel);
    check(rIdx >= 0 && mIdx >= 0 && bIdx >= 0, "the labels render, main and the busy thread are present");
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
    check(everyValid && hi <= 2 * lo + 2, "every chosen thread is read, and the round-robin is balanced");

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
        if (!oracleRange(r.module, r.begin, ob, oe) || ob != r.begin || oe != r.end) ++mismatched;
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
