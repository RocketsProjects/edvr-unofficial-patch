// The graphics half's glue for the render-thread CPU profile (cpu_profile.h, src\common\cpu_sampler.h): the keys, the
// start on the render thread, the worker, and the log and CSV lines. The sampling itself is cpu::Session; this file
// decides when one runs and where its output goes.
#include "cpu_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
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
// kRunning is set by the render thread at its Present, which is the only thread that can duplicate its own handle.
enum Phase : int { kIdle = 0, kRequested = 1, kRunning = 2 };
std::atomic<int> g_phase{kIdle};
std::atomic<const char*> g_trigger{"?"};

// diag.profile_cpu. `on` waits for the first in-scene frame, then 20 s, then requests one capture and is done.
bool g_onMode = false;                  // set once, by cpuProfileConfigure
std::atomic<bool> g_waitingForScene{false};
std::atomic<unsigned long long> g_dueMs{0};   // GetTickCount64 at which the `on` capture is requested; 0 when none is due
constexpr unsigned long long kOnDelayMs = 20000;

// The job the worker runs: the render thread's own handle (a duplicate, closed by the worker), its stack registration,
// and the stamp the file and its lines carry.
struct Job {
    HANDLE thread = nullptr;
    uint64_t lo = 0, hi = 0;
    DWORD tid = 0;
    const char* trigger = "?";
    char stamp[24] = {};             // YYYYMMDD_HHMMSS, local time
};

void logLine(const char* line, void*) {
    Log::get().note("%s", line);
}

// The worker: samples the render thread for the window, resolves, and writes the report and the CSV. Nothing here runs
// while the render thread is stopped except inside cpu::Session::sample, which holds only stall::captureThread.
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
    // The file is opened before the window: the window does no I/O.
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) f = nullptr;
    info.csvName = f ? csvName : "none";
    if (!f) Log::get().note("cpu profile: could not open edvr_logs\\%s for the raw samples; the report still follows.", csvName);

    cpu::Session* session = new (std::nothrow) cpu::Session();
    if (!session || !session->prepare()) {
        Log::get().note("cpu profile: no memory for the sample buffers; nothing sampled.");
        if (f) std::fclose(f);
        delete session;
        return;
    }
    session->emitStart(&logLine, nullptr, info);
    session->sample(job.thread, job.lo, job.hi, cpu::kWindowSeconds, cpu::kTargetHz, cpu::kMaxStoppedFraction);
    session->resolve();
    session->emitReport(&logLine, nullptr, info);
    if (f) {
        if (!session->writeCsv(f, info)) Log::get().note("cpu profile: writing %s failed; the report above is complete.", csvName);
        std::fclose(f);
    }
    delete session;
}

DWORD WINAPI workerMain(LPVOID param) {
    Job* job = static_cast<Job*>(param);
    guarded("cpuProfile/run", [job] { runCapture(*job); });
    CloseHandle(job->thread);
    delete job;
    g_phase.store(kIdle, std::memory_order_release);
    return 0;
}

// On the render thread, at its Present. Duplicates the thread's own handle with the stall sampler's rights, reads its
// stack registration, and starts the worker. Nothing is started when any step fails, and the log says which.
void startHere() {
    int expected = kRequested;
    if (!g_phase.compare_exchange_strong(expected, kRunning)) return;
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0) ||
        !h) {
        g_phase.store(kIdle, std::memory_order_release);
        Log::get().note("cpu profile: could not duplicate the render thread's handle (error %lu); nothing sampled.",
                        static_cast<unsigned long>(GetLastError()));
        return;
    }
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    Job* job = new (std::nothrow) Job();
    if (!job) {
        CloseHandle(h);
        g_phase.store(kIdle, std::memory_order_release);
        Log::get().note("cpu profile: no memory for the job; nothing sampled.");
        return;
    }
    job->thread = h;
    job->lo = lo;
    job->hi = hi;
    job->tid = GetCurrentThreadId();
    job->trigger = g_trigger.load(std::memory_order_acquire);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(job->stamp, sizeof(job->stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    HANDLE worker = CreateThread(nullptr, 0, &workerMain, job, 0, nullptr);
    if (!worker) {
        CloseHandle(h);
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
                    "that thread for 10 s.", trigger ? trigger : "?");
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
    if (g_phase.load(std::memory_order_relaxed) != kRequested) return;
    guarded("cpuProfile/start", [] { startHere(); });
}

}  // namespace edvr
