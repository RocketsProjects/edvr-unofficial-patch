// The graphics half's glue for the process CPU profile (cpu_profile.h, src\common\cpu_sampler.h): the keys, the start on the
// render thread, the worker, and the log and CSV lines. The sampling itself is cpu::Session; the choice of threads is
// cpu::selectThreads. This file decides when a capture runs and where its output goes.
#include "cpu_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "../common/config.h"
#include "../common/cpu_sampler.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

namespace edvr {
namespace {

// The capture's state. kRequested is set by a key (or the timed `on` mode) on whatever thread runs the frame tick;
// kRunning is set by the render thread at its Present, which is the only thread that can read its own stack registration.
enum Phase : int { kIdle = 0, kRequested = 1, kRunning = 2 };
std::atomic<int> g_phase{kIdle};
std::atomic<const char*> g_trigger{"?"};

// Presents counted while a capture runs: the render thread's Presents over the window, for the per-frame wait.
std::atomic<unsigned long long> g_presents{0};

// The render thread's Present times (QueryPerformanceCounter ticks), written by cpuProfileBeat while a capture runs, indexed by
// the Present count. Static storage (32 KB of zero pages, touched only during a capture) so the render thread never races a free.
constexpr unsigned kMaxStamps = 4096;
uint64_t g_stamps[kMaxStamps];

// diag.profile_cpu. `on` waits for the first in-scene frame, then 20 s, then requests one capture and is done.
bool g_onMode = false;                  // set once, by cpuProfileConfigure
std::atomic<bool> g_waitingForScene{false};
std::atomic<unsigned long long> g_dueMs{0};   // GetTickCount64 at which the `on` capture is requested; 0 when none is due
constexpr unsigned long long kOnDelayMs = 20000;

// The job the worker runs: the render thread's tid and its stack registration, and the stamp the file and its lines carry.
struct Job {
    DWORD tid = 0;
    uint64_t lo = 0, hi = 0;
    const char* trigger = "?";
    char stamp[24] = {};             // YYYYMMDD_HHMMSS, local time
};

void logLine(const char* line, void*) {
    Log::get().note("%s", line);
}

// The worker: chooses the threads (one second of CPU deltas), samples them for the window, resolves, and writes the report
// and the CSV. Nothing here runs while a thread is stopped except inside cpu::Session::sample, which holds only
// stall::captureThread.
void runCapture(const Job& job) {
    cpu::RunInfo info;
    info.thread = job.tid;
    info.trigger = job.trigger;
    info.build = EDVR_VERSION_STRING;
    std::wstring path = Config::get().logDir() + L"\\cpu_profile_";
    for (const char* c = job.stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";
    char csvName[64];
    std::snprintf(csvName, sizeof(csvName), "cpu_profile_%s.csv", job.stamp);
    // The per-frame CSV (cpu_frames_<stamp>.csv) is opened the same way, before the window.
    std::wstring framesPath = Config::get().logDir() + L"\\cpu_frames_";
    for (const char* c = job.stamp; *c; ++c) framesPath.push_back(static_cast<wchar_t>(*c));
    framesPath += L".csv";
    char framesName[64];
    std::snprintf(framesName, sizeof(framesName), "cpu_frames_%s.csv", job.stamp);
    FILE* ff = nullptr;
    if (_wfopen_s(&ff, framesPath.c_str(), L"wb") != 0 || !ff) ff = nullptr;
    info.framesCsvName = ff ? framesName : "none";
    // The file is opened before the window: the window does no I/O.
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) f = nullptr;
    info.csvName = f ? csvName : "none";
    if (!f) Log::get().note("cpu profile: could not open edvr_logs\\%s for the raw samples; the report still follows.", csvName);

    cpu::Session* session = new (std::nothrow) cpu::Session();
    cpu::Target* targets = new (std::nothrow) cpu::Target[cpu::kMaxTargets];
    if (!session || !targets || !session->prepare()) {
        Log::get().note("cpu profile: no memory for the sample buffers; nothing sampled.");
        if (f) std::fclose(f);
        if (ff) std::fclose(ff);
        delete session;
        delete[] targets;
        return;
    }
    char how[480] = {};
    const unsigned n = cpu::selectThreads(GetCurrentProcessId(), GetCurrentThreadId(), job.tid, job.lo, job.hi,
                                          cpu::kPreWindowSeconds, targets, cpu::kMaxTargets, how, sizeof(how));
    info.threads = n;
    session->emitStart(&logLine, nullptr, info);
    Log::get().note("cpu profile: pick pre_window_s=%.1f %s", cpu::kPreWindowSeconds, how);
    const unsigned long long presents0 = g_presents.load(std::memory_order_relaxed);
    session->sample(targets, n, cpu::kWindowSeconds, cpu::kTargetHz, cpu::kMaxStoppedFraction);
    const unsigned long long presents1 = g_presents.load(std::memory_order_relaxed);
    session->setPresents(presents1 - presents0);
    // The Present times of the window (the slots from presents0 on; a slot a late beat has not written yet reads 0 and is dropped).
    {
        const unsigned lo = presents0 < kMaxStamps ? static_cast<unsigned>(presents0) : kMaxStamps;
        const unsigned hi = presents1 < kMaxStamps ? static_cast<unsigned>(presents1) : kMaxStamps;
        uint64_t window[kMaxStamps];
        unsigned nw = 0;
        for (unsigned i = lo; i < hi; ++i) {
            if (g_stamps[i]) window[nw++] = g_stamps[i];
        }
        session->setPresentStamps(window, nw);
    }
    session->resolve();
    session->analyze();
    session->emitReport(&logLine, nullptr, info);
    if (f) {
        if (!session->writeCsv(f, info)) Log::get().note("cpu profile: writing %s failed; the report above is complete.", csvName);
        std::fclose(f);
    }
    if (ff) {
        if (!session->writeFramesCsv(ff)) Log::get().note("cpu profile: writing %s failed; the report above is complete.", framesName);
        std::fclose(ff);
    }
    for (unsigned i = 0; i < n; ++i) {
        if (targets[i].handle) CloseHandle(targets[i].handle);
    }
    delete session;
    delete[] targets;
}

DWORD WINAPI workerMain(LPVOID param) {
    Job* job = static_cast<Job*>(param);
    guarded("cpuProfile/run", [job] { runCapture(*job); });
    delete job;
    g_phase.store(kIdle, std::memory_order_release);
    return 0;
}

// On the render thread, at its Present: the thread's own stack registration (the one the stop copies from), and the
// worker. Nothing is started when a step fails, and the log says which.
void startHere() {
    int expected = kRequested;
    if (!g_phase.compare_exchange_strong(expected, kRunning)) return;
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    Job* job = new (std::nothrow) Job();
    if (!job) {
        g_phase.store(kIdle, std::memory_order_release);
        Log::get().note("cpu profile: no memory for the job; nothing sampled.");
        return;
    }
    job->tid = GetCurrentThreadId();
    job->lo = lo;
    job->hi = hi;
    job->trigger = g_trigger.load(std::memory_order_acquire);
    g_presents.store(0, std::memory_order_relaxed);
    std::memset(g_stamps, 0, sizeof(g_stamps));
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(job->stamp, sizeof(job->stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    HANDLE worker = CreateThread(nullptr, 0, &workerMain, job, 0, nullptr);
    if (!worker) {
        delete job;
        g_phase.store(kIdle, std::memory_order_release);
        Log::get().note("cpu profile: could not start the sampling thread (error %lu); nothing sampled.",
                        static_cast<unsigned long>(GetLastError()));
        return;
    }
    CloseHandle(worker);
}

}  // namespace

void cpuProfileConfigure() {
    const std::string v = Config::get().getString("diag.profile_cpu", "off");
    if (v == "on") {
        g_onMode = true;
        g_waitingForScene.store(true, std::memory_order_release);
        Log::get().note(
            "cpu profile: diag.profile_cpu = on: one capture of 10 s, 20 s after the first in-scene frame. Flat has no "
            "scene flag, so in flat it waits and never fires: bind hotkey.profile_cpu there.");
    } else if (v == "off" || v.empty()) {
        Log::get().note("cpu profile: diag.profile_cpu = off: no thread and no samples until hotkey.profile_cpu is pressed.");
    } else {
        Log::get().note("cpu profile: diag.profile_cpu = %s is not on or off; treated as off.", v.c_str());
    }
}

void cpuProfileRequest(const char* trigger) {
    int expected = kIdle;
    if (!g_phase.compare_exchange_strong(expected, kRequested)) {
        Log::get().note("cpu profile: %s pressed, but a capture is already waiting or running; nothing started.",
                        trigger ? trigger : "?");
        return;
    }
    g_trigger.store(trigger ? trigger : "?", std::memory_order_release);
    Log::get().note("cpu profile: capture requested by %s; it starts at the render thread's next Present and samples "
                    "the render, main, owner and busiest threads for 10 s (per-frame accounting from the render thread's Presents).", trigger ? trigger : "?");
}

void cpuProfileFrameTick() {
    if (!g_onMode) return;   // off: one branch
    if (g_waitingForScene.load(std::memory_order_acquire)) {
        if (!sceneArrived()) return;
        g_waitingForScene.store(false, std::memory_order_release);
        g_dueMs.store(GetTickCount64() + kOnDelayMs, std::memory_order_release);
        Log::get().note("cpu profile: the first in-scene frame; the capture is requested in 20 s.");
        return;
    }
    const unsigned long long due = g_dueMs.load(std::memory_order_acquire);
    if (due && GetTickCount64() >= due) {
        g_dueMs.store(0, std::memory_order_release);
        cpuProfileRequest("diag.profile_cpu = on");
    }
}

void cpuProfileBeat() {
    const int phase = g_phase.load(std::memory_order_relaxed);
    if (phase == kRunning) {   // a capture runs: count this Present for its per-frame figures, and stamp it
        const unsigned long long idx = g_presents.fetch_add(1, std::memory_order_relaxed);
        if (idx < kMaxStamps) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            g_stamps[idx] = static_cast<uint64_t>(t.QuadPart);
        }
        return;
    }
    if (phase != kRequested) return;   // idle: one relaxed load
    guarded("cpuProfile/start", [] { startHere(); });
}

}  // namespace edvr
