// The render thread's CPU profile, tested on a thread with known hot functions (src\common\cpu_sampler.h, src\d3d11\cpu_profile.cpp,
// docs\engine-render-performance-2026-09-19.md). The real capture, walk, resolution, report and CSV code runs against a busy
// thread in this exe, which calls hot_outer in hot_target.dll (built beside it, a module that is not this exe). hot_spin_hot
// is three quarters of the busy time, and the thread sleeps now and then so the wait class has samples.
//
//   cpu_sampler_test.exe --dry-run                       touches nothing
//   cpu_sampler_test.exe --self-test <root> <outdir>     root is the repository (its source is checked); outdir gets
//                                                        cpu_profile_report.log and cpu_profile_rig.csv, which the build
//                                                        then gives to edvr_log.py --cpu-profile
//
// Checks: the hot leaf dominates the leaf and function tables; hot_outer is on most stacks; the wait class has samples; every
// listed .pdata range equals RtlLookupFunctionEntry's, the oracle; the report's lines parse, each under 1100 characters; the
// CSV's sample rows find their functions in its function rows; the pacer keeps the duty cap on a fake clock; the window
// holds no call but stall::captureThread; and the suspend cost of a sample is measured and reported.
#include <windows.h>

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

// ---- the busy thread: the render thread the profile samples ----------------------------------------------------------

using OuterFn = void (*)();

struct Busy {
    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    HANDLE thread = nullptr;     // this thread's own duplicate, made by the thread itself (as cpu_profile.cpp does)
    DWORD tid = 0;
    uint64_t lo = 0, hi = 0;
    OuterFn outer = nullptr;
    unsigned calls = 0;
};

void busyMain(Busy* b) {
    b->tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &b->thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    b->lo = lo;
    b->hi = hi;
    b->ready.store(true);
    while (!b->stop.load()) {
        b->outer();
        if ((++b->calls % 8u) == 0) Sleep(3);   // a wait now and then: samples land in ntdll
    }
}

// ---- report parsing (the rig reads its own lines back: the format is the contract) ---------------------------------

struct FnRow {
    unsigned idx = 0, incl = 0, excl = 0;
    double inclPct = 0.0, exclPct = 0.0;
    std::string module;
    bool hasRange = false;
    uint32_t begin = 0, end = 0;
};

bool parseFnRow(const std::string& line, FnRow& r) {
    char label[160] = {};
    unsigned idx = 0, incl = 0, excl = 0;
    double ip = 0.0, ep = 0.0;
    if (std::sscanf(line.c_str(), "cpu profile: fn %u incl %u %lf%% excl %u %lf%% %159s", &idx, &incl, &ip, &excl, &ep, label) != 6)
        return false;
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

bool parseLeafRow(const std::string& line, unsigned& count, double& pct, std::string& label) {
    char text[160] = {};
    unsigned idx = 0;
    if (std::sscanf(line.c_str(), "cpu profile: leaf %u %u %lf%% %159s", &idx, &count, &pct, text) != 4) return false;
    label = text;
    return true;
}

// The report's per-class share lines: "cpu profile: class <name> leaf <n> <p>% on_stack <n> <p>%".
bool parseClassRow(const std::string& line, std::string& name, unsigned& leaf, unsigned& onStack) {
    char text[64] = {};
    double lp = 0.0, sp = 0.0;
    if (std::sscanf(line.c_str(), "cpu profile: class %63s leaf %u %lf%% on_stack %u %lf%%", text, &leaf, &lp, &onStack, &sp) != 5)
        return false;
    name = text;
    return true;
}

// The module as the rig's own module table names it: the module's name, cut from the file path.
std::string exeDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

// The oracle: the RUNTIME_FUNCTION the OS would use for `rva` in the module named `name`.
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
// ---- the CSV, read back as tools\elite_hotspots.py will (the contract in cpu_sampler.h's Session::writeCsv) --------

struct CsvCheck {
    unsigned sampleRows = 0;
    unsigned badRows = 0;             // a frame list that is not in pairs, or longer than 8 frames
    unsigned framesWithModule = 0;    // leaves in a named module
    unsigned framesInFunction = 0;    // of those, the leaves a "# function" range holds
    unsigned moduleRows = 0;
    unsigned functionRows = 0;
};

CsvCheck checkCsv(const std::string& text) {
    CsvCheck c;
    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> ranges;   // by module file name
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.compare(0, 9, "# module,") == 0) {
                ++c.moduleRows;
            } else if (line.compare(0, 11, "# function,") == 0) {
                char name[64] = {};
                unsigned b = 0, e = 0;
                if (std::sscanf(line.c_str(), "# function,%63[^,],%x,%x", name, &b, &e) == 3) {
                    ranges[name].push_back({b, e});
                    ++c.functionRows;
                }
            }
            continue;
        }
        if (line.compare(0, 11, "module,rva_") == 0) continue;   // the optional header line
        std::vector<std::string> f;
        std::string cell;
        std::istringstream row(line);
        while (std::getline(row, cell, ',')) f.push_back(cell);
        if (f.size() < 2) {
            ++c.badRows;
            continue;
        }
        ++c.sampleRows;
        const size_t frameCells = f.size() - 2;
        if (frameCells % 2 != 0 || frameCells > 16) ++c.badRows;
        const std::string& mod = f[0];
        const uint32_t rva = static_cast<uint32_t>(std::strtoul(f[1].c_str(), nullptr, 16));
        if (mod == "?") continue;
        ++c.framesWithModule;
        for (const auto& r : ranges[mod]) {
            if (rva >= r.first && rva < r.second) {
                ++c.framesInFunction;
                break;
            }
        }
    }
    return c;
}

// ---- the tests ----------------------------------------------------------------------------------------------------

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
    ok &= count("stall::captureThread(") == 1;   // the one call that stops the thread, in Session::sample
    ok &= count("Log::") == 0;                    // the header logs nothing: the glue passes the sink
    check(ok, "no stop or context call outside stall::captureThread; the header logs nothing");
    return ok;
}

bool testCapture(const std::string& outDir, const std::string& root, bool& wroteOutput) {
    (void)root;
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
    check(outer && hotLeaf, "hot_target.dll exports hot_outer and hot_spin_hot");
    if (!outer || !hotLeaf) return false;
    const uint32_t hotRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hotLeaf) - reinterpret_cast<uintptr_t>(dll));

    Busy busy;
    busy.outer = outer;
    std::thread worker(busyMain, &busy);
    while (!busy.ready.load()) Sleep(1);

    Session session;
    check(session.prepare(), "the sample buffer and tables are allocated before the window");
    std::vector<std::string> lines;
    edvr::cpu::RunInfo info;
    info.thread = busy.tid;
    info.trigger = "rig busy thread";
    info.build = "cpu_sampler_test (no EDVR build)";
    info.windowS = 2.0;
    info.csvName = "cpu_profile_rig.csv";
    session.emitStart(&collect, &lines, info);
    session.sample(busy.thread, busy.lo, busy.hi, info.windowS, edvr::cpu::kTargetHz, edvr::cpu::kMaxStoppedFraction);
    busy.stop.store(true);
    worker.join();
    session.resolve();
    session.emitReport(&collect, &lines, info);

    const unsigned valid = session.valid();
    std::printf("  window: %u samples, %u valid, %u failures, %.1f Hz achieved, timer %s\n", session.samples(), valid,
                session.failures(), session.achievedHz(), session.highResTimer() ? "high_res" : "sleep_1ms");
    const double meanSuspend = session.samples() ? session.suspendTotalUs() / session.samples() : 0.0;
    std::printf("  per-sample suspend: mean %.1f us, max %u us; stopped %.2f ms in %.0f ms of window\n", meanSuspend,
                session.suspendMaxUs(), session.suspendTotalUs() / 1000.0, session.wallUs() / 1000.0);

    check(valid >= 600, "at least 300 Hz of valid samples over the 2 s window");
    check(meanSuspend > 0.0 && meanSuspend < 500.0, "the per-sample suspend cost is measured and sane (0 to 500 us)");

    // Parse the report's rows back.
    std::vector<FnRow> fns;
    std::vector<std::pair<unsigned, std::string>> leaves;
    std::map<std::string, std::pair<unsigned, unsigned>> classes;
    unsigned lineErrors = 0;
    bool sawStart = false, sawEnd = false, sawPacing = false;
    for (const std::string& line : lines) {
        if (line.size() >= 1100 || line.compare(0, 12, "cpu profile:") != 0) {
            if (!lineErrors) std::printf("    first bad line (%zu chars): %s\n", line.size(), line.c_str());
            ++lineErrors;
        }
        if (line.find("cpu profile: start") == 0) sawStart = true;
        if (line.find("cpu profile: end") == 0) sawEnd = true;
        if (line.find("cpu profile: pacing") == 0) sawPacing = true;
        FnRow r;
        if (line.find("cpu profile: fn ") == 0 && parseFnRow(line, r)) fns.push_back(r);
        unsigned count = 0;
        double pct = 0.0;
        std::string label;
        if (line.find("cpu profile: leaf ") == 0 && parseLeafRow(line, count, pct, label)) leaves.push_back({count, label});
        std::string name;
        unsigned leaf = 0, stack = 0;
        if (line.find("cpu profile: class ") == 0 && parseClassRow(line, name, leaf, stack)) classes[name] = {leaf, stack};
    }
    check(lineErrors == 0, "every report line starts 'cpu profile:' and is under 1100 characters");
    check(sawStart && sawPacing && sawEnd, "the report has its start, pacing and end lines");
    check(!fns.empty() && fns.size() <= edvr::cpu::kTopN, "the function table has rows and at most 40");
    check(!leaves.empty() && leaves.size() <= edvr::cpu::kTopN, "the leaf table has rows and at most 40");

    // The hot leaf dominates: the top exclusive function is hot_spin_hot, and hot_outer is on most stacks.
    const FnRow* topExcl = nullptr;
    for (const FnRow& r : fns) {
        if (!topExcl || r.excl > topExcl->excl) topExcl = &r;
    }
    check(topExcl && topExcl->hasRange && topExcl->module == "hot_target.dll" && topExcl->begin <= hotRva &&
              hotRva < topExcl->end,
          "the top exclusive function is hot_spin_hot in hot_target.dll");
    const double hotExclPct = topExcl ? topExcl->exclPct : 0.0;
    std::printf("  hot_spin_hot leaf share %.2f%% of valid samples (about 0.75 of the busy share)\n", hotExclPct);
    check(hotExclPct >= 25.0, "hot_spin_hot is at least a quarter of the samples (it is 0.75 of the busy time)");
    const uint32_t outerRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(outer) - reinterpret_cast<uintptr_t>(dll));
    bool outerOk = false;
    for (const FnRow& r : fns) {
        if (r.hasRange && r.module == "hot_target.dll" && r.begin <= outerRva && outerRva < r.end) {
            outerOk = r.inclPct >= 40.0 && r.inclPct > r.exclPct;
            std::printf("  hot_outer inclusive %.2f%%, exclusive %.2f%%\n", r.inclPct, r.exclPct);
        }
    }
    check(outerOk, "hot_outer is on at least 40% of the stacks, and more than its own leaf share");
    check(classes.count("ntdll_kernel") && classes["ntdll_kernel"].first * 100u >= 5u * valid && valid > 0,
          "the wait class has at least 5% of the leaves (the thread's Sleep calls)");
    check(classes.count("other") && classes["other"].first > 0, "the rig's own modules are the 'other' class");

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
        if (r.hasRange && r.module == "hot_target.dll" && r.begin == ob && r.end == oe) hotInTable = true;
    }
    check(hotOracle && hotInTable, "hot_spin_hot's range from the table equals the OS's (oracle)");

    // The frames: the walk reaches the caller, on the hot samples at least.
    unsigned hotSamples = 0;
    unsigned long long frameSum = 0;
    for (unsigned i = 0; i < session.samples(); ++i) {
        const edvr::cpu::Sample* s = session.sampleAt(i);
        if (!s || !s->frames) continue;
        if (s->mod[0] >= 0 && s->pc[0] && s->rva[0] >= ob && s->rva[0] < oe) {
            ++hotSamples;
            frameSum += s->frames;
        }
    }
    const double meanFrames = hotSamples ? static_cast<double>(frameSum) / hotSamples : 0.0;
    std::printf("  hot leaf samples %u, mean frames %.2f\n", hotSamples, meanFrames);
    check(hotSamples > 0 && meanFrames >= 3.0, "the walk reaches hot_outer and the thread's entry on hot samples (3+ frames)");

    // The CSV: write it, read it back the way edvr_log.py will.
    const std::string csvPath = outDir + "\\cpu_profile_rig.csv";
    if (FILE* f = std::fopen(csvPath.c_str(), "wb")) {
        check(session.writeCsv(f, info), "the CSV writes");
        std::fclose(f);
    } else {
        check(false, "the CSV opens for writing");
    }
    const CsvCheck csv = checkCsv(readFile(csvPath));
    std::printf("  csv: %u sample rows, %u module rows, %u function rows; leaf frames in a listed function %u of %u\n",
                csv.sampleRows, csv.moduleRows, csv.functionRows, csv.framesInFunction, csv.framesWithModule);
    check(csv.sampleRows == session.valid() && csv.badRows == 0,
          "one CSV row per valid sample, every frame list in module,rva pairs of at most 8 frames");
    check(csv.framesWithModule > 0 && csv.framesInFunction * 100u >= csv.framesWithModule * 95u,
          "95% of the CSV's module leaves fall in a listed .pdata function");

    // The report as a log file the build reads back with edvr_log.py --cpu-profile.
    {
        std::ofstream out(outDir + "\\cpu_profile_report.log", std::ios::binary);
        for (const std::string& line : lines) out << line << "\n";
        wroteOutput = static_cast<bool>(out);
    }
    check(wroteOutput, "the report file writes");

    // A session that never sampled says so: its report has zero samples and no leaf rows.
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

    CloseHandle(busy.thread);
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
    std::puts("cpu_sampler_test: the render thread's CPU profile, on a busy thread with known hot functions");
    testClassify();
    testPacer();
    testSourceWindow(root);
    bool wrote = false;
    testCapture(outDir, root, wrote);
    if (g_fails) {
        std::printf("cpu_sampler_test: %d check(s) FAILED\n", g_fails);
        return 1;
    }
    std::printf("cpu_sampler_test: all checks passed\n");
    return 0;
}
