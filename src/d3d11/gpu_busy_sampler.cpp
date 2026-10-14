// GPU busy time from the operating system (2026-10-14). The maths, the board and the log text are src\common\gpu_busy_os.h;
// this file is the kernel calls and the thread that makes them.
//
// WHAT IT READS. D3DKMTQueryStatistics, in gdi32.dll, with the game's adapter LUID and a handle to this process: per hardware
// node, the cumulative time that node ran this process's work (100 ns units; checked on an RTX 5090 against a GPU load of
// known length). It is the kernel's own bookkeeping and the source of Task Manager's per-process GPU column. It costs one
// syscall per node per sample (17 on an RTX 5090, a few microseconds each).
//
// WHERE IT RUNS. Never on the render thread: that thread only counts frames (gpuBusyFrame) and, once, reads the device's
// adapter LUID. A detached std::thread samples once a second. The graphics DLL is pinned before the first Present
// (module_pin.h), the same lifetime the journal worker and the heartbeat writer rely on.
//
// WHAT IT CANNOT SEE. The adapter the game's device sits on, and this process's work only: the runtime compositor's GPU work
// (a different process, a different queue) is not in the figure, and neither is another adapter's. Without
// D3DKMTQueryStatistics (Wine, DXVK) the figure is absent and the overlay says "--".
#include "gpu_busy_sampler.h"

#include <windows.h>
#include <winternl.h>   // NTSTATUS: d3dkmthk.h needs it, and WIN32_LEAN_AND_MEAN (the build's flag) leaves it out of windows.h
#include <d3d11.h>
#include <dxgi.h>
#include <d3dkmthk.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "gpu_census.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/timing.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (static_cast<NTSTATUS>(status) >= 0)
#endif

namespace edvr {
namespace {

constexpr DWORD kSampleMs = 1000;
constexpr uint64_t kLogEveryMs = 30000;
constexpr unsigned kReadFailLimit = 3;     // consecutive samples with no readable counted node
constexpr unsigned kBadWindowLimit = 10;   // consecutive windows dropped as unusable while frames were advancing

using QueryStatisticsFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
using QueryAdapterInfoFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
using OpenAdapterFromLuidFn = NTSTATUS(APIENTRY*)(D3DKMT_OPENADAPTERFROMLUID*);
using CloseAdapterFn = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);

struct Session {
    LUID luid{};
    char adapter[128] = {};
};

GpuBusyBoard g_board;
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};
HANDLE g_wake = nullptr;   // created with the thread, never closed: the thread may still be waiting on it at exit

uint64_t wallUs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * 1000000.0 / static_cast<double>(f.QuadPart));
}

void say(const char* line) { Log::get().note("%s", line); }

// Why there is no figure: once on the log, once on the board.
void giveUp(GpuBusyFail why, bool* said) {
    g_board.fail(why);
    if (said && *said) return;
    if (said) *said = true;
    char line[256];
    formatGpuBusyFailLine(line, sizeof(line), why);
    say(line);
}

void workerMain(Session* s) {
    bool said = false;
    try {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
        const auto queryStatistics = gdi ? reinterpret_cast<QueryStatisticsFn>(GetProcAddress(gdi, "D3DKMTQueryStatistics")) : nullptr;
        const auto queryAdapterInfo = gdi ? reinterpret_cast<QueryAdapterInfoFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")) : nullptr;
        const auto openAdapter = gdi ? reinterpret_cast<OpenAdapterFromLuidFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")) : nullptr;
        const auto closeAdapter = gdi ? reinterpret_cast<CloseAdapterFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
        if (!queryStatistics || !openAdapter) { giveUp(GpuBusyFail::NoProcs, &said); return; }

        // The node count, from the adapter's statistics.
        D3DKMT_QUERYSTATISTICS adapterQuery{};
        adapterQuery.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        adapterQuery.AdapterLuid = s->luid;
        if (!NT_SUCCESS(queryStatistics(&adapterQuery))) { giveUp(GpuBusyFail::NoStatistics, &said); return; }
        unsigned nodeCount = adapterQuery.QueryResult.AdapterInformation.NodeCount;
        if (nodeCount == 0) { giveUp(GpuBusyFail::NoNodes, &said); return; }
        if (nodeCount > kGpuBusyMaxNodes) nodeCount = kGpuBusyMaxNodes;

        GpuBusyNodeInfo nodes[kGpuBusyMaxNodes];
        for (unsigned i = 0; i < nodeCount; ++i) {
            nodes[i].id = i;
            nodes[i].counted = true;   // until the metadata says otherwise: with no metadata every node counts
        }
        // The engine type and name of each node (best effort: a refusal leaves the node counted and unnamed).
        if (queryAdapterInfo) {
            D3DKMT_OPENADAPTERFROMLUID open{};
            open.AdapterLuid = s->luid;
            if (NT_SUCCESS(openAdapter(&open))) {
                for (unsigned i = 0; i < nodeCount; ++i) {
                    D3DKMT_NODEMETADATA meta{};
                    meta.NodeOrdinalAndAdapterIndex = i;
                    D3DKMT_QUERYADAPTERINFO info{};
                    info.hAdapter = open.hAdapter;
                    info.Type = KMTQAITYPE_NODEMETADATA;
                    info.pPrivateDriverData = &meta;
                    info.PrivateDriverDataSize = sizeof(meta);
                    if (!NT_SUCCESS(queryAdapterInfo(&info))) continue;
                    nodes[i].engineType = static_cast<uint32_t>(meta.NodeData.EngineType);
                    nodes[i].counted = gpuBusyEngineCounts(nodes[i].engineType);
                    WideCharToMultiByte(CP_UTF8, 0, meta.NodeData.FriendlyName, -1, nodes[i].name, static_cast<int>(sizeof(nodes[i].name)),
                                        nullptr, nullptr);
                    nodes[i].name[sizeof(nodes[i].name) - 1] = 0;
                }
                if (closeAdapter) {
                    D3DKMT_CLOSEADAPTER close{};
                    close.hAdapter = open.hAdapter;
                    closeAdapter(&close);
                }
            }
        }

        // The process handle the per-process queries take. A pseudo-handle is the fallback.
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
        if (!process) process = GetCurrentProcess();

        auto readNodes = [&](uint64_t* running, bool* usable, unsigned* usableCounted) {
            *usableCounted = 0;
            for (unsigned i = 0; i < nodeCount; ++i) {
                D3DKMT_QUERYSTATISTICS q{};
                q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
                q.AdapterLuid = s->luid;
                q.hProcess = process;
                q.QueryProcessNode.NodeId = i;
                const bool ok = NT_SUCCESS(queryStatistics(&q));
                usable[i] = ok;
                running[i] = ok ? static_cast<uint64_t>(q.QueryResult.ProcessNodeInformation.RunningTime.QuadPart) : 0;
                if (ok && nodes[i].counted) ++*usableCounted;
            }
        };

        uint64_t running[kGpuBusyMaxNodes] = {};
        bool usable[kGpuBusyMaxNodes] = {};
        bool counted[kGpuBusyMaxNodes] = {};
        unsigned usableCounted = 0;
        readNodes(running, usable, &usableCounted);
        for (unsigned i = 0; i < nodeCount; ++i) {
            nodes[i].usable = usable[i];
            counted[i] = nodes[i].counted;
        }
        if (usableCounted == 0) { giveUp(GpuBusyFail::NoNodes, &said); return; }

        {
            char line[900];
            formatGpuBusyStartLine(line, sizeof(line), s->adapter, static_cast<uint32_t>(s->luid.HighPart), s->luid.LowPart, nodes,
                                   nodeCount, kSampleMs);
            say(line);
        }

        GpuBusyAccumulator acc;
        GpuBusyLogWindow logWindow;
        GpuBusyWindow window;
        acc.feed(wallUs(), g_frames.load(std::memory_order_relaxed), running, usable, counted, nodeCount, &window);   // the baseline
        uint64_t lastFrames = g_frames.load(std::memory_order_relaxed);
        uint64_t logStartMs = nowMs();
        double spanSum0 = 0.0;
        uint64_t spanCount0 = 0;
        gpuCensusFrameSpanTotals(&spanSum0, &spanCount0);
        unsigned readFails = 0, badWindows = 0;

        while (!g_stop.load(std::memory_order_acquire)) {
            WaitForSingleObject(g_wake, kSampleMs);
            if (g_stop.load(std::memory_order_acquire)) break;
            const uint64_t frames = g_frames.load(std::memory_order_relaxed);
            readNodes(running, usable, &usableCounted);
            if (usableCounted == 0) {
                if (++readFails >= kReadFailLimit) giveUp(GpuBusyFail::ReadFailed, &said);
                continue;
            }
            readFails = 0;
            if (frames == lastFrames) { acc.feed(wallUs(), frames, running, usable, counted, nodeCount, &window); continue; }   // paused: re-baseline
            lastFrames = frames;
            const GpuBusyAccumulator::Result r = acc.feed(wallUs(), frames, running, usable, counted, nodeCount, &window);
            if (r == GpuBusyAccumulator::Result::Window) {
                badWindows = 0;
                g_board.publish(window, nowMs());
                logWindow.add(window);
                said = false;   // a figure after a failure: the next failure is news again
            } else if (r == GpuBusyAccumulator::Result::Dropped) {
                if (++badWindows >= kBadWindowLimit) giveUp(GpuBusyFail::BadReadings, &said);
            }
            const uint64_t now = nowMs();
            if (now - logStartMs >= kLogEveryMs) {
                double spanSum = 0.0, spanMs = -1.0;
                uint64_t spanCount = 0;
                gpuCensusFrameSpanTotals(&spanSum, &spanCount);
                if (spanCount > spanCount0) spanMs = (spanSum - spanSum0) / static_cast<double>(spanCount - spanCount0);
                spanSum0 = spanSum;
                spanCount0 = spanCount;
                if (logWindow.windows) {
                    // The busiest counted node over the interval, for the list's marker.
                    unsigned busiest = 0;
                    double best = -1.0;
                    for (unsigned i = 0; i < nodeCount; ++i) {
                        if (!nodes[i].counted) continue;
                        const double p = logWindow.nodePercent(i);
                        if (p > best) { best = p; busiest = i; }
                    }
                    char line[900];
                    formatGpuBusyLogLine(line, sizeof(line), logWindow, nodes, nodeCount, busiest, spanMs);
                    say(line);
                }
                logWindow = GpuBusyLogWindow{};
                logStartMs = now;
            }
        }
    } catch (...) {
        giveUp(GpuBusyFail::BadReadings, &said);
    }
}

// The render thread's one-time start: the device's adapter, its LUID and name. DXGI objects are free-threaded; the LUID is
// copied out here so the worker holds no COM pointer.
void startOnce(ID3D11Device* device) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    if (runtimeFlatProfile()) return;   // the overlay is the headset's; the flat profile gets no sampler (and asks no more)
    bool ok = false, said = false;
    guarded("gpuBusyStart", [&] {
        IDXGIDevice* dxgiDevice = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) || !dxgiDevice) {
            giveUp(GpuBusyFail::NoAdapter, &said);
            return;
        }
        IDXGIAdapter* adapter = nullptr;
        const HRESULT got = dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
        if (FAILED(got) || !adapter) { giveUp(GpuBusyFail::NoAdapter, &said); return; }
        DXGI_ADAPTER_DESC desc{};
        const bool described = SUCCEEDED(adapter->GetDesc(&desc));
        adapter->Release();
        if (!described) { giveUp(GpuBusyFail::NoAdapter, &said); return; }
        Session* s = new Session();   // never freed: the thread may outlive any owner
        s->luid = desc.AdapterLuid;
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, s->adapter, static_cast<int>(sizeof(s->adapter)), nullptr, nullptr);
        s->adapter[sizeof(s->adapter) - 1] = 0;
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_wake) { giveUp(GpuBusyFail::NoAdapter, &said); return; }
        try {
            std::thread(workerMain, s).detach();
            ok = true;
        } catch (...) {
        }
    });
    if (!ok) giveUp(GpuBusyFail::NoAdapter, &said);   // no thread: once on the log and the board, and g_started stays set so nothing retries
}

}  // namespace

void gpuBusyFrame(ID3D11Device* device) noexcept {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (g_started.load(std::memory_order_relaxed) || !device) return;
    startOnce(device);
}

GpuBusyReading gpuBusyReading() noexcept { return g_board.read(nowMs()); }

void gpuBusyShutdown() noexcept {
    g_stop.store(true, std::memory_order_release);
    if (g_wake) SetEvent(g_wake);
}

}  // namespace edvr
