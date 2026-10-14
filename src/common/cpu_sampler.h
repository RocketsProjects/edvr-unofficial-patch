// The process's CPU, sampled from inside the process (docs/render-thread-cpu-2026-10-10.md, docs/engine-render-performance-2026-09-19.md).
//
// Where the threads of the game spend their time, by sampling them: a worker thread picks the threads to read (the render
// thread that presents, the main thread, the owner (EDVR's OpenXR owner thread: a thread that starts in a module exporting
// VR_GetGenericInterface), and the four others that used the most CPU over a one-second pre-window), then
// stops them one at a time, round-robin, at a combined rate for ten seconds. Each stop reads the registers, copies the
// stack above them, lets the thread go, and walks the copy to the leaf and up to eight return addresses. Afterwards each
// address is resolved to a module (module+RVA) and to a function (the RUNTIME_FUNCTION of the module's .pdata that holds
// it, so no symbols are needed). The report gives each thread its CPU time, its top functions (inclusive and exclusive)
// and its class split; the render thread also gets its top leaves and, when the game is the build this was written for,
// the time it spends in the producer handshake (render waits for producer).
//
// THE SAFETY ARGUMENT is the stall sampler's (stall_sampler.h, its header comment), and this file adds nothing to the
// stopped window: the only calls that stop a thread are stall::captureThread (SuspendThread, GetThreadContext, a bounded
// copy, ResumeThread), reused unchanged. The walk (stall::unwindCapture) runs after the resume, as the stall sampler's
// does. Every thread's stack region, the module table, the .pdata and the PE stamp are read before or after the window, on
// the worker: never while a thread is stopped. Nothing is allocated, logged or locked inside a stop, and the sample buffers
// exist before the first one. A target's stack region is the stack registration of the render thread (its own), or for
// any other thread the committed stack limits in that thread's TEB, read from the TEB's NT_TIB: a bounded read of another
// thread's memory, which the stopped window never makes.
//
// THE OVERHEAD CAP. Every sample stops one thread for its own suspend cost (stoppedTicks), so the next sample is not due
// sooner than suspend / kMaxStoppedFraction after this one's due time: at 1.2 kHz with a 30 us stop the period governs,
// and with a 100 us stop the combined rate falls to 500 Hz. The achieved rate and the total stopped time are logged.
//
// HEADER ONLY, so tools\cpu_sampler_test runs this code against busy threads with known hot functions. The graphics half's
// glue (src\d3d11\cpu_profile.cpp) adds the hotkey, the configuration key, the log and the CSV file's place.
#pragma once

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "stall_sampler.h"

namespace edvr::cpu {

constexpr double kWindowSeconds = 10.0;        // the production window
constexpr double kPreWindowSeconds = 1.0;      // the CPU deltas that choose the threads are taken over this long
constexpr double kTargetHz = 1200.0;           // the combined rate, round-robin over the targets
constexpr double kMaxStoppedFraction = 0.05;   // the stopped share of the window, all threads together, is at most this
constexpr unsigned kFrames = 9;                // the leaf (RIP) and up to eight return addresses
constexpr unsigned kMaxSamples = 14000;        // 10 s at the combined rate is 12000; the rest is slack
constexpr unsigned kMaxTargets = 7;            // the render thread, the main thread, the owner, and up to four others
constexpr unsigned kMaxOthers = 4;
constexpr unsigned kMaxThreadsSeen = 256;
constexpr unsigned kMaxModules = 96;
constexpr unsigned kLeafSlots = 1u << 14;      // the render thread's leaf table (half-full at most)
constexpr unsigned kFnSlots = 1u << 12;        // each thread's function and leaf-location tables
constexpr unsigned kTopN = 20;                 // the functions per thread
constexpr unsigned kTopLeaves = 20;            // the render thread's leaves
constexpr uint32_t kNoFunction = 0xFFFFFFFFu;  // a frame in a module with no .pdata entry holding its RVA
constexpr DWORD kTimerHighRes = 0x00000002;    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803 and later)

// The producer handshake the render thread waits in (game build 332841): the function at EliteDangerous64.exe+0x5d6d20 that
// waits on a condition variable for the next frame to be published, the sample at 0x5d6d7f, and the range that function
// spans. The measurement is taken only when the game is that build: its PE stamp and image size, the same gate as
// ui_sizing_math.h's kUiPanelStamp and kUiPanelImageSize (the same build), must match.
constexpr uint32_t kEliteStamp = 1788384820u;
constexpr uint32_t kEliteImageSize = 104894464u;
constexpr uint32_t kEliteWaitBegin = 0x5d6d20u;
constexpr uint32_t kEliteWaitEnd = 0x5d6de2u;

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

// `edvr` is true for this DLL's own module (stall::thisModule). The rest is by file name: the game's executable, EDVR's own
// OpenXR runtime (openvr_api.dll, the one this build ships in Openvr\win64), the OS images a wait sits in, the graphics
// drivers and the D3D/DXGI front ends, and the vendor runtimes (Oculus, OpenXR loaders, DLSS, FSR, XeSS).
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

// The upscalers and frame generators: a wait inside one is the GPU's, not a runtime's.
inline bool isUpscalerName(const char* name) noexcept {
    static const char* const kNames[] = {"nvngx", "amd_fidelity", "libxess", "xess", "ffx", "fsr"};
    for (const char* p : kNames) {
        if (startsNoCase(name, p)) return true;
    }
    return false;
}

// The Elite handshake's gate: the game's own PE stamp and image size are the build this function's range was read from.
inline bool eliteBuildGate(uint32_t stamp, uint32_t size) noexcept {
    return stamp == kEliteStamp && size == kEliteImageSize;
}

// ---- busy or waiting, and waiting on what (docs/cpu-frametime-2026-10-14.md) --------------------------------------------
// A stopped thread is WAITING when its leaf sits in an OS wait stub (a named export of ntdll, kernelbase or win32u whose name
// says it waits, found by the export table, so no symbols are needed); anything else is BUSY, including a thread that spins in
// the graphics driver. A waiting sample is then attributed by its stack, strongest evidence first: the game's frame handshake
// function (gated on the game's build), EDVR's OpenXR runtime or a vendor runtime (the xrWaitFrame and owner hops), the
// graphics driver or the D3D/DXGI front ends (the GPU), then by the kind of stub (a park is a lock or a condition variable).
enum class WaitKind : uint8_t { Busy = 0, Handshake, Xr, Gpu, Lock, Event, Sleep, Count };
constexpr unsigned kKinds = static_cast<unsigned>(WaitKind::Count);

inline const char* waitKindName(WaitKind k) noexcept {
    switch (k) {
        case WaitKind::Busy: return "busy";
        case WaitKind::Handshake: return "handshake";
        case WaitKind::Xr: return "xr";
        case WaitKind::Gpu: return "gpu";
        case WaitKind::Lock: return "lock";
        case WaitKind::Event: return "event";
        case WaitKind::Sleep: return "sleep";
        default: return "?";
    }
}

// What an OS stub does, from its export name. None is a stub (or any function) that does not wait.
enum class StubKind : uint8_t { None = 0, Park, Event, Sleep };

inline bool nameHas(const char* name, const char* part) noexcept {
    return name && part && std::strstr(name, part) != nullptr;
}

// The names are the syscall stubs and their kernel32/user32 wrappers: NtWaitForAlertByThreadId is what SRW locks, critical
// sections, WaitOnAddress and condition variables park in; the WaitFor family and message waits are kernel-object waits.
inline StubKind stubKindOfExport(const char* name) noexcept {
    if (!name || !name[0]) return StubKind::None;
    if (nameHas(name, "WaitForAlertByThreadId") || nameHas(name, "WaitOnAddress") || nameHas(name, "SleepConditionVariable"))
        return StubKind::Park;
    if (nameHas(name, "DelayExecution") || nameHas(name, "YieldExecution")) return StubKind::Sleep;
    if (nameHas(name, "WaitFor") || nameHas(name, "SignalAndWait") || nameHas(name, "WaitMessage") ||
        nameHas(name, "MsgWait") || nameHas(name, "RemoveIoCompletion") || nameHas(name, "WaitHighEventPair") ||
        nameHas(name, "WaitLowEventPair"))
        return StubKind::Event;
    return StubKind::None;
}

struct ExportRec {
    uint32_t rva = 0;
    StubKind kind = StubKind::None;
};
constexpr uint32_t kStubSpan = 0x60;   // a leaf this close above an export's start is inside that stub

// The module's named exports, by RVA, with what each one does. Forwarded exports (an RVA inside the export directory) are
// skipped. Empty when the image has no export directory.
inline void loadExports(uintptr_t base, std::vector<ExportRec>& out) {
    out.clear();
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return;
        const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const auto* funcs = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
        const auto* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
        const auto* ords = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
            const WORD o = ords[i];
            if (o >= ed->NumberOfFunctions) continue;
            const DWORD rva = funcs[o];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;   // a forwarder
            const char* name = reinterpret_cast<const char*>(base + names[i]);
            out.push_back({rva, stubKindOfExport(name)});
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.clear();
    }
    std::sort(out.begin(), out.end(), [](const ExportRec& a, const ExportRec& b) {
        return a.rva != b.rva ? a.rva < b.rva : static_cast<unsigned>(a.kind) > static_cast<unsigned>(b.kind);
    });
}

// What the function that holds `rva` does: the nearest export at or below it, if the leaf is within a stub's span of it.
inline StubKind stubKindAt(const std::vector<ExportRec>& exports, uint32_t rva) noexcept {
    auto it = std::upper_bound(exports.begin(), exports.end(), rva,
                               [](uint32_t v, const ExportRec& e) { return v < e.rva; });
    if (it == exports.begin()) return StubKind::None;
    --it;
    // Several exports share one RVA (Nt* and Zw*): the sort put the one that waits first, so look at the run's first element.
    auto first = it;
    while (first != exports.begin() && (first - 1)->rva == it->rva) --first;
    if (rva - it->rva >= kStubSpan) return StubKind::None;
    return first->kind;
}

// The attribution of one sample, pure so the rig can drive it. `stub` is what the leaf's function does; the flags say what
// the walked stack holds. A sample whose leaf is not a wait stub is busy whatever its stack holds.
inline WaitKind attributeSample(StubKind stub, bool handshakeFrame, bool xrFrame, bool gpuFrame) noexcept {
    if (stub == StubKind::None) return WaitKind::Busy;
    if (handshakeFrame) return WaitKind::Handshake;
    if (xrFrame) return WaitKind::Xr;
    if (gpuFrame) return WaitKind::Gpu;
    return stub == StubKind::Park ? WaitKind::Lock : stub == StubKind::Sleep ? WaitKind::Sleep : WaitKind::Event;
}

// ---- the frame, from the render thread's Presents ---------------------------------------------------------------------
// A frame is the interval between two of the render thread's Presents. Each sample falls in one; per frame and per target
// the model counts the samples and how many were busy, and for the render thread how many sat in each wait kind. A frame's
// critical path is named from the render thread's samples alone: the render thread presents last, so what it spent the frame
// waiting on is what finished last before the frame could go. Under kRenderBound of its samples waiting, the render thread
// itself was the critical path.
enum class Crit : uint8_t { Unknown = 0, Producer, Render, Xr, Gpu, Sync, Count };
constexpr unsigned kCrits = static_cast<unsigned>(Crit::Count);

inline const char* critName(Crit c) noexcept {
    switch (c) {
        case Crit::Producer: return "producer";
        case Crit::Render: return "render";
        case Crit::Xr: return "xr";
        case Crit::Gpu: return "gpu";
        case Crit::Sync: return "sync";
        default: return "unknown";
    }
}

constexpr unsigned kMaxFrames = 4096;
constexpr unsigned kFrameBlock = 30;   // the summary line covers this many frames
constexpr double kRenderBoundWaitShare = 0.25;

// One sample, reduced to what the frame model needs.
struct FrameSample {
    uint32_t tUs = 0;                  // from the window's start
    uint8_t target = 0;
    uint8_t kind = 0;                  // WaitKind
    uint8_t edvrLeaf = 0;              // 1: leaf in EDVR's d3d11 DLL, 2: leaf in EDVR's OpenXR runtime, 0: neither
    uint8_t edvrStack = 0;             // 1: an EDVR frame anywhere on the stack
};

struct FrameRow {
    uint32_t index = 0;                // 0-based among the complete frames of the window
    double startMs = 0.0;              // from the window's start
    double frameMs = 0.0;
    uint16_t n[kMaxTargets] = {};      // samples in this frame, by target
    uint16_t busy[kMaxTargets] = {};   // of those, busy
    uint16_t edvr[kMaxTargets] = {};   // of the busy ones, the leaf in EDVR code
    uint16_t renderKind[kKinds] = {};  // the render thread's samples by WaitKind
    Crit crit = Crit::Unknown;
};

// The critical path of a frame: from the render thread's wait counts. Pure.
inline Crit classifyFrame(const FrameRow& r, unsigned renderTarget) noexcept {
    const unsigned n = renderTarget < kMaxTargets ? r.n[renderTarget] : 0;
    if (!n) return Crit::Unknown;
    unsigned waits = 0;
    for (unsigned k = 1; k < kKinds; ++k) waits += r.renderKind[k];
    if (waits < kRenderBoundWaitShare * n) return Crit::Render;
    const unsigned producer = r.renderKind[static_cast<unsigned>(WaitKind::Handshake)];
    const unsigned xr = r.renderKind[static_cast<unsigned>(WaitKind::Xr)];
    const unsigned gpu = r.renderKind[static_cast<unsigned>(WaitKind::Gpu)];
    const unsigned sync = r.renderKind[static_cast<unsigned>(WaitKind::Lock)] +
                          r.renderKind[static_cast<unsigned>(WaitKind::Event)] +
                          r.renderKind[static_cast<unsigned>(WaitKind::Sleep)];
    // The largest wait names the path; a tie goes to the producer, then the GPU, then the runtime.
    Crit best = Crit::Producer;
    unsigned top = producer;
    if (gpu > top) { best = Crit::Gpu; top = gpu; }
    if (xr > top) { best = Crit::Xr; top = xr; }
    if (sync > top) { best = Crit::Sync; top = sync; }
    return best;
}

// Builds the frames of a window. `presentUs` are the render thread's Present times from the window's start, ascending; the
// frames are the intervals between neighbours that lie wholly inside [0, wallUs). `samples` are ascending in tUs. Pure.
inline void buildFrames(const FrameSample* samples, unsigned ns, unsigned nTargets, unsigned renderTarget,
                        const double* presentUs, unsigned np, double wallUs, std::vector<FrameRow>& out) {
    out.clear();
    unsigned si = 0;
    for (unsigned p = 0; p + 1 < np && out.size() < kMaxFrames; ++p) {
        const double a = presentUs[p], b = presentUs[p + 1];
        if (a < 0.0 || b > wallUs || b <= a) continue;
        while (si < ns && samples[si].tUs < a) ++si;
        FrameRow row;
        row.index = static_cast<uint32_t>(out.size());
        row.startMs = a / 1000.0;
        row.frameMs = (b - a) / 1000.0;
        for (unsigned j = si; j < ns && samples[j].tUs < b; ++j) {
            const FrameSample& s = samples[j];
            if (s.target >= nTargets || s.target >= kMaxTargets) continue;
            ++row.n[s.target];
            if (s.kind == static_cast<uint8_t>(WaitKind::Busy)) {
                ++row.busy[s.target];
                if (s.edvrLeaf) ++row.edvr[s.target];
            }
            if (s.target == renderTarget && s.kind < kKinds) ++row.renderKind[s.kind];
        }
        row.crit = classifyFrame(row, renderTarget);
        out.push_back(row);
    }
}

// The pacing as a pure function so the rig can drive it. `dueUs` is when this sample was due, `nowUs` the clock when it was
// taken, `suspendUs` how long the thread was stopped for it. The next sample is due one period later, or later still when
// the stop was long enough that the stopped share would otherwise exceed the cap. A sample more than a period behind does
// not catch up in a burst: the schedule restarts from now.
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
    uint8_t target = 0;                // the index of the thread stopped, in Session's targets
    uintptr_t pc[kFrames] = {};        // the leaf, then the return addresses, innermost first
    int16_t mod[kFrames] = {};         // module index, or -1 when no module holds the address
    uint32_t rva[kFrames] = {};        // the RVA in that module (the low 32 bits of the address when -1)
    uint8_t kind = 0;                  // WaitKind, set by Session::resolve (0 is busy)
    uint8_t edvrLeaf = 0;              // 1: the leaf is in EDVR's d3d11 DLL, 2: in EDVR's OpenXR runtime
    uint8_t edvrStack = 0;             // 1: an EDVR frame is anywhere on the walked stack
};

struct ModuleRec {
    char name[32] = {};                // the file name, cut at 31 characters
    uintptr_t base = 0;
    Class cls = Class::Unknown;
    const RUNTIME_FUNCTION* fns = nullptr;   // the module's .pdata, read in place (the module is loaded for the process)
    uint32_t nfns = 0;
    std::vector<ExportRec> exports;    // the OS images' named exports, read the first time a leaf lands in one
    bool exportsLoaded = false;
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

// The PE TimeDateStamp and SizeOfImage of the module at `base`. False when it is not a PE image.
inline bool peStampAndSize(uintptr_t base, uint32_t& stamp, uint32_t& size) noexcept {
    stamp = 0;
    size = 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        stamp = nt->FileHeader.TimeDateStamp;
        size = nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        stamp = 0;
        size = 0;
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

// The module holding `pc`: its base, and its file name cut to `cap` characters ("?" when none holds it).
inline bool moduleOfAddress(uintptr_t pc, uintptr_t& base, char* name, size_t cap) noexcept {
    base = 0;
    if (cap) name[0] = 0;
    HMODULE h = nullptr;
    if (!pc || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(pc), &h) || !h)
        return false;
    base = reinterpret_cast<uintptr_t>(h);
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(h, path, MAX_PATH);
    if (len >= MAX_PATH) len = MAX_PATH - 1;
    DWORD start = 0;
    for (DWORD i = 0; i < len; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') start = i + 1;
    }
    size_t n = 0;
    for (DWORD i = start; i < len && n + 1 < cap; ++i) name[n++] = path[i] < 0x80 ? static_cast<char>(path[i]) : '?';
    name[n] = 0;
    if (!n && cap >= 2) std::snprintf(name, cap, "?");
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
    // The record for `key` if it is there; never creates.
    const R* find(uint64_t key) const noexcept {
        if (!n_) return nullptr;
        const unsigned mask = n_ - 1;
        unsigned i = static_cast<unsigned>((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
        for (unsigned probe = 0; probe < n_; ++probe, i = (i + 1) & mask) {
            const R& r = recs_[i];
            if (r.key == key) return &r;
            if (r.key == 0) return nullptr;
        }
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

// ---- threads: the list, the CPU deltas, the stacks, the start addresses, the choice ----------------------------------

// One thread the window may stop. `handle` carries the query, suspend and context rights and is closed by its owner.
struct Target {
    DWORD tid = 0;
    HANDLE handle = nullptr;
    uint64_t lo = 0, hi = 0;           // the stack the stop may copy from
    bool render = false;               // the thread that presents (the glue's Present thread)
    bool main = false;                 // the process's primary thread (the earliest created), when it is not the render thread
    char label[16] = {};               // "render", "main" or "t<tid>"
    char role[8] = {};                 // "render", "main" or "other"
    char start[96] = {};               // the thread's start address as module+RVA; "?" when it could not be read
    uint64_t preCpu = 0;               // the CPU of the pre-window, 100 ns units (the choice's evidence)
    uint64_t cpuStart = 0, cpuEnd = 0; // GetThreadTimes over the window, user + kernel, 100 ns units
};

// The thread snapshot the choice reads. The handle is owned by whoever holds the snapshot until it is taken into a Target.
struct ThreadSnap {
    DWORD tid = 0;
    HANDLE handle = nullptr;
    uint64_t created = 0, cpu0 = 0, cpu1 = 0;
};

inline uint64_t fileTimeTicks(const FILETIME& f) noexcept {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// User plus kernel CPU of a thread, 100 ns units; 0 when the thread cannot be read.
inline uint64_t threadCpu(HANDLE h) noexcept {
    FILETIME c{}, e{}, k{}, u{};
    if (!h || !GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return fileTimeTicks(k) + fileTimeTicks(u);
}

inline uint64_t threadCreated(HANDLE h) noexcept {
    FILETIME c{}, e{}, k{}, u{};
    if (!h || !GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return fileTimeTicks(c);
}

// NtQueryInformationThread, resolved from ntdll (the same export every query of a thread's start address and TEB uses).
using NtQueryThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
struct ThreadBasicInfo {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    PVOID UniqueProcess;
    PVOID UniqueThread;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
};
inline NtQueryThreadFn ntQueryThread() noexcept {
    static const NtQueryThreadFn fn = reinterpret_cast<NtQueryThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    return fn;
}

// The committed stack limits of another thread, from its TEB's NT_TIB: StackLimit (the lowest committed byte) and
// StackBase (the top). False when the TEB cannot be read. The active stack is committed from its stack pointer up to the
// base, so a copy above the pointer never faults.
inline bool threadStack(HANDLE h, uint64_t& lo, uint64_t& hi) noexcept {
    lo = 0;
    hi = 0;
    const NtQueryThreadFn query = ntQueryThread();
    if (!query || !h) return false;
    ThreadBasicInfo tbi{};
    ULONG len = 0;
    if (query(h, 0, &tbi, sizeof(tbi), &len) != 0 || !tbi.TebBaseAddress) return false;
    __try {
        const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(tbi.TebBaseAddress);
        lo = reinterpret_cast<uintptr_t>(tib->StackLimit);
        hi = reinterpret_cast<uintptr_t>(tib->StackBase);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lo = 0;
        hi = 0;
    }
    return hi > lo;
}

// "module+0xRVA" of a thread's start address, "?" when it cannot be read. Empty handle, empty answer.
inline void threadStartLabel(HANDLE h, char* out, size_t cap) noexcept {
    std::snprintf(out, cap, "?");
    const NtQueryThreadFn query = ntQueryThread();
    if (!query || !h) return;
    uintptr_t start = 0;
    ULONG len = 0;
    if (query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), &len) != 0 || !start) return;
    uintptr_t base = 0;
    char name[32] = {};
    if (!moduleOfAddress(start, base, name, sizeof(name))) {
        std::snprintf(out, cap, "?+0x%llx", static_cast<unsigned long long>(start));
        return;
    }
    std::snprintf(out, cap, "%s+0x%llx", name, static_cast<unsigned long long>(start - base));
}

// Every thread of process `pid` except `self`, opened with the rights a stop needs. Returns how many (at most `max`).
inline unsigned listThreads(DWORD pid, DWORD self, ThreadSnap* snap, unsigned max) noexcept {
    HANDLE toolhelp = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (toolhelp == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    unsigned n = 0;
    if (Thread32First(toolhelp, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || n >= max) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                                  THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            ThreadSnap& s = snap[n++];
            s.tid = te.th32ThreadID;
            s.handle = h;
            s.created = threadCreated(h);
        } while (Thread32Next(toolhelp, &te));
    }
    CloseHandle(toolhelp);
    return n;
}

// The module at `base` exports VR_GetGenericInterface: openvr_api.dll does, and it is EDVR's OpenXR runtime's entry point in
// this build, so a thread that starts in it is the owner thread (the one that services the runtime's calls).
inline bool exportsVrGenericInterface(uintptr_t base) noexcept {
    return base && GetProcAddress(reinterpret_cast<HMODULE>(base), "VR_GetGenericInterface") != nullptr;
}

// A thread's Win32 start address (the lpStartAddress it was created with), from ThreadQuerySetWin32StartAddress.
inline bool threadStartAddress(HANDLE h, uintptr_t& start) noexcept {
    start = 0;
    const NtQueryThreadFn query = ntQueryThread();
    if (!query || !h) return false;
    ULONG len = 0;
    return query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), &len) == 0 && start != 0;
}

// Chooses the targets from a snapshot: the render thread (given, with its stack registration), the main thread (the
// earliest created of the process: the loader creates the primary thread first), the owner (a thread that starts in a
// module exporting VR_GetGenericInterface; with several, the one with the most CPU over the pre-window), and up to
// kMaxOthers others by the same CPU. Each chosen snapshot's handle moves into its Target (the snapshot's becomes null).
// `how` gets one line saying what was chosen and why, every owner candidate included. Returns how many targets were
// written to `out`.
inline unsigned chooseTargets(ThreadSnap* snap, unsigned n, DWORD renderTid, uint64_t renderLo, uint64_t renderHi,
                              Target* out, unsigned maxOut, char* how, size_t howCap) noexcept {
    if (howCap) how[0] = 0;
    int render = -1, main = -1;
    for (unsigned i = 0; i < n; ++i) {
        if (snap[i].tid == renderTid) render = static_cast<int>(i);
        if (snap[i].created && (main < 0 || snap[i].created < snap[main].created)) main = static_cast<int>(i);
    }
    auto cpuOf = [&](int i) -> uint64_t { return snap[i].cpu1 >= snap[i].cpu0 ? snap[i].cpu1 - snap[i].cpu0 : 0; };
    enum Kind { kOther, kRender, kMain, kOwner };
    unsigned k = 0;
    auto take = [&](int i, Kind kind, uint64_t lo, uint64_t hi) {
        Target& t = out[k++];
        ThreadSnap& s = snap[i];
        t.tid = s.tid;
        t.handle = s.handle;
        s.handle = nullptr;
        t.render = kind == kRender;
        t.main = kind == kMain;
        t.preCpu = cpuOf(i);
        if (kind == kRender) std::snprintf(t.label, sizeof(t.label), "render");
        else if (kind == kMain) std::snprintf(t.label, sizeof(t.label), "main");
        else if (kind == kOwner) std::snprintf(t.label, sizeof(t.label), "owner");
        else std::snprintf(t.label, sizeof(t.label), "t%lu", static_cast<unsigned long>(t.tid));
        std::snprintf(t.role, sizeof(t.role), "%s",
                      kind == kRender ? "render" : kind == kMain ? "main" : kind == kOwner ? "owner" : "other");
        if (kind == kRender) {
            t.lo = lo;
            t.hi = hi;
        } else if (!threadStack(t.handle, t.lo, t.hi)) {
            t.lo = t.hi = 0;
        }
        threadStartLabel(t.handle, t.start, sizeof(t.start));
    };
    size_t used = 0;
    auto add = [&](const char* text) {
        if (!howCap || used + 1 >= howCap) return;
        const int w = std::snprintf(how + used, howCap - used, "%s", text);
        if (w > 0) used = std::min(howCap - 1, used + static_cast<size_t>(w));
    };
    if (render >= 0 && k < maxOut) {
        take(render, kRender, renderLo, renderHi);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "render=render-thread t%lu", static_cast<unsigned long>(renderTid));
        add(buf);
    } else {
        add("render=not-in-list");
    }
    if (main >= 0 && main != render && k < maxOut) {
        take(main, kMain, 0, 0);
        char buf[80];
        std::snprintf(buf, sizeof(buf), "; main=earliest-created t%lu", static_cast<unsigned long>(snap[main].tid));
        add(buf);
    } else if (main >= 0 && main == render) {
        add("; main=the render thread");
    }
    // The owner candidates: threads that start in a module exporting VR_GetGenericInterface, neither render nor main.
    int cands[kMaxThreadsSeen];
    unsigned nc = 0;
    for (unsigned i = 0; i < n && nc < kMaxThreadsSeen; ++i) {
        if (static_cast<int>(i) == render || static_cast<int>(i) == main || !snap[i].handle) continue;
        uintptr_t start = 0, base = 0;
        char name[32] = {};
        if (!threadStartAddress(snap[i].handle, start) || !moduleOfAddress(start, base, name, sizeof(name)) ||
            !exportsVrGenericInterface(base))
            continue;
        cands[nc++] = static_cast<int>(i);
    }
    std::sort(cands, cands + nc, [&](int a, int b) {
        const uint64_t da = cpuOf(a), db = cpuOf(b);
        return da != db ? da > db : snap[a].tid < snap[b].tid;
    });
    if (!nc) {
        add("; owner=none (no thread starts in a module that exports VR_GetGenericInterface)");
    } else {
        add("; owner candidates:");
        for (unsigned j = 0; j < nc; ++j) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " t%lu %.1fms", static_cast<unsigned long>(snap[cands[j]].tid), cpuOf(cands[j]) / 10000.0);
            add(buf);
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "; owner=t%lu (most pre-window CPU)", static_cast<unsigned long>(snap[cands[0]].tid));
        add(buf);
        if (k < maxOut) take(cands[0], kOwner, 0, 0);
    }
    // The others: the highest CPU over the pre-window, excluding the three above.
    const int owner = nc ? cands[0] : -1;
    int order[kMaxThreadsSeen];
    unsigned m = 0;
    for (unsigned i = 0; i < n && m < kMaxThreadsSeen; ++i) {
        if (snap[i].handle && static_cast<int>(i) != render && static_cast<int>(i) != main && static_cast<int>(i) != owner)
            order[m++] = static_cast<int>(i);
    }
    std::sort(order, order + m, [&](int a, int b) {
        const uint64_t da = cpuOf(a), db = cpuOf(b);
        return da != db ? da > db : snap[a].tid < snap[b].tid;
    });
    add("; others by pre-window CPU:");
    unsigned others = 0;
    for (unsigned j = 0; j < m && others < kMaxOthers && k < maxOut; ++j) {
        const int i = order[j];
        char buf[64];
        std::snprintf(buf, sizeof(buf), " t%lu %.1fms", static_cast<unsigned long>(snap[i].tid), cpuOf(i) / 10000.0);
        add(buf);
        take(i, kOther, 0, 0);
        ++others;
    }
    return k;
}

// The whole choice, on the worker: list the threads, take their CPU, wait the pre-window, take it again and choose.
// Handles that were not chosen are closed here. Returns how many targets were written to `out`.
inline unsigned selectThreads(DWORD pid, DWORD self, DWORD renderTid, uint64_t renderLo, uint64_t renderHi,
                              double preWindowS, Target* out, unsigned maxOut, char* how, size_t howCap) {
    std::unique_ptr<ThreadSnap[]> snap(new (std::nothrow) ThreadSnap[kMaxThreadsSeen]);
    if (!snap) return 0;
    const unsigned n = listThreads(pid, self, snap.get(), kMaxThreadsSeen);
    for (unsigned i = 0; i < n; ++i) snap[i].cpu0 = threadCpu(snap[i].handle);
    Sleep(static_cast<DWORD>(preWindowS * 1000.0));
    for (unsigned i = 0; i < n; ++i) snap[i].cpu1 = threadCpu(snap[i].handle);
    const unsigned k = chooseTargets(snap.get(), n, renderTid, renderLo, renderHi, out, maxOut, how, howCap);
    for (unsigned i = 0; i < n; ++i) {
        if (snap[i].handle) CloseHandle(snap[i].handle);
    }
    return k;
}

// One line of the report, or of the log's start line. `user` is whatever the sink needs.
using LineSink = void (*)(const char* line, void* user);

// What a run says about itself in its lines.
struct RunInfo {
    DWORD thread = 0;                  // the render thread's tid
    const char* trigger = "?";         // "hotkey.profile_cpu", "diag.profile_cpu = on", or a rig's name
    double windowS = kWindowSeconds;
    double targetHz = kTargetHz;
    double dutyPct = kMaxStoppedFraction * 100.0;
    const char* csvName = "none";      // the file's name in the log directory, or "none"
    const char* build = "?";           // the EDVR build that took the samples (the CSV's header comment)
    const char* framesCsvName = "none"; // the per-frame CSV's name in the log directory, or "none"
    unsigned threads = 0;              // how many targets the window stopped
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
        aggs_.reset(new (std::nothrow) Agg[kMaxTargets]);
        cap_ = samples_ ? maxSamples : 0;
        if (!samples_ || !mods_ || !aggs_) return false;
        for (unsigned t = 0; t < kMaxTargets; ++t) {
            if (!aggs_[t].fns.create(kFnSlots) || !aggs_[t].busyFns.create(kFnSlots) || !aggs_[t].locs.create(kFnSlots))
                return false;
        }
        return true;
    }

    // The window. Samples the targets round-robin for `windowS` seconds at `targetHz` combined, stopping any one of them
    // and holding the combined stopped share to `maxFraction`. Each target's CPU is read before and after the window,
    // on this thread, outside every stop. The only calls that stop a thread are inside stall::captureThread.
    void sample(const Target* targets, unsigned n, double windowS, double targetHz, double maxFraction) {
        count_ = 0;
        valid_ = 0;
        failures_ = 0;
        suspendTotalUs_ = 0.0;
        suspendMaxUs_ = 0;
        wallUs_ = 0.0;
        bufferFull_ = false;
        resolved_ = false;
        nTargets_ = n > kMaxTargets ? kMaxTargets : n;
        for (unsigned t = 0; t < nTargets_; ++t) {
            targets_[t] = targets[t];
            targets_[t].cpuStart = threadCpu(targets_[t].handle);
            targets_[t].cpuEnd = 0;
        }
        if (!cap_ || !nTargets_ || windowS <= 0.0 || targetHz <= 0.0) return;
        std::unique_ptr<stall::Capture> cap(new (std::nothrow) stall::Capture());
        if (!cap) return;
        WaitTimer timer;
        timer_highres_ = timer.highRes();
        LARGE_INTEGER f{}, t0{}, t{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        t0Qpc_ = static_cast<uint64_t>(t0.QuadPart);
        freqQpc_ = static_cast<double>(f.QuadPart);
        const double freq = static_cast<double>(f.QuadPart);
        const double periodUs = 1e6 / targetHz;
        const double windowUs = windowS * 1e6;
        double dueUs = 0.0;
        unsigned slot = 0;
        uintptr_t pcs[kFrames] = {};
        // The round-robin order: every target once, and the render thread and the busiest non-owner thread (the likeliest
        // producer) twice, so a frame holds three or four of each of their samples instead of two.
        unsigned sched[2 * kMaxTargets] = {};
        unsigned nSched = 0;
        {
            int heavy = -1;
            for (unsigned i = 0; i < nTargets_; ++i) {
                if (targets_[i].render || std::strcmp(targets_[i].role, "owner") == 0) continue;
                if (heavy < 0 || targets_[i].preCpu > targets_[heavy].preCpu) heavy = static_cast<int>(i);
            }
            for (unsigned i = 0; i < nTargets_; ++i) {
                sched[nSched++] = i;
                if (targets_[i].render || static_cast<int>(i) == heavy) sched[nSched++] = i;
            }
        }
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
            const unsigned who = sched[slot % nSched];
            ++slot;
            const Target& tg = targets_[who];
            // ---- the thread is stopped inside captureThread, and only there ----
            stall::captureThread(tg.handle, tg.lo, tg.hi, *cap);
            // ---- the thread is running again: walk the copy ----
            const unsigned n2 = stall::unwindCapture(*cap, pcs, kFrames);
            Sample& s = samples_[count_++];
            s.tUs = static_cast<uint32_t>(nowUs);
            s.suspendUs = static_cast<uint32_t>(static_cast<double>(cap->stoppedTicks) * 1e6 / freq);
            s.status = static_cast<uint8_t>(cap->status);
            s.frames = static_cast<uint8_t>(n2);
            s.target = static_cast<uint8_t>(who);
            for (unsigned j = 0; j < kFrames; ++j) s.pc[j] = j < n2 ? pcs[j] : 0;
            suspendTotalUs_ += s.suspendUs;
            if (s.suspendUs > suspendMaxUs_) suspendMaxUs_ = s.suspendUs;
            dueUs = Pacer::nextDueUs(dueUs, nowUs, s.suspendUs, periodUs, maxFraction);
        }
        QueryPerformanceCounter(&t);
        wallUs_ = static_cast<double>(t.QuadPart - t0.QuadPart) * 1e6 / freq;
        for (unsigned tt = 0; tt < nTargets_; ++tt) targets_[tt].cpuEnd = threadCpu(targets_[tt].handle);
    }

    // The presents (the frame count) over the window, from the glue; the render thread's wait is per frame.
    void setPresents(unsigned long long frames) noexcept { presents_ = frames; }

    // The render thread's Present times over the window, as QueryPerformanceCounter ticks, ascending. Call after sample(); times
    // before the window's start are dropped. The frame model (analyze) reads them.
    void setPresentStamps(const uint64_t* qpc, unsigned n) {
        presentUs_.clear();
        if (!qpc || freqQpc_ <= 0.0) return;
        for (unsigned i = 0; i < n; ++i) {
            if (qpc[i] < t0Qpc_) continue;
            const double us = static_cast<double>(qpc[i] - t0Qpc_) * 1e6 / freqQpc_;
            if (!presentUs_.empty() && us <= presentUs_.back()) continue;
            presentUs_.push_back(us);
        }
    }

    // After resolve: the producer (inferred) and the frames. The producer is the busiest target that is neither the render
    // thread nor the owner, by the share of its samples that were running: the thread the render thread's handshake waits on
    // is a thread the job system keeps busy, and this does not claim more than that. The frames come from the Present times.
    void analyze() {
        producer_ = -1;
        frames_.clear();
        if (!resolved_) return;
        double bestShare = 0.0;
        for (unsigned t = 0; t < nTargets_; ++t) {
            if (targets_[t].render || std::strcmp(targets_[t].role, "owner") == 0 || !aggs_[t].valid) continue;
            const double share = static_cast<double>(aggs_[t].kindCount[0]) / aggs_[t].valid;
            if (share > bestShare) {
                bestShare = share;
                producer_ = static_cast<int>(t);
            }
        }
        const int r = targetIndex("render");
        if (r < 0 || presentUs_.size() < 2) return;
        std::vector<FrameSample> fs;
        fs.reserve(count_);
        for (unsigned i = 0; i < count_; ++i) {
            const Sample& s = samples_[i];
            if (!s.frames) continue;
            FrameSample f;
            f.tUs = s.tUs;
            f.target = s.target;
            f.kind = s.kind;
            f.edvrLeaf = s.edvrLeaf;
            f.edvrStack = s.edvrStack;
            fs.push_back(f);
        }
        buildFrames(fs.data(), static_cast<unsigned>(fs.size()), nTargets_, static_cast<unsigned>(r), presentUs_.data(),
                    static_cast<unsigned>(presentUs_.size()), wallUs_, frames_);
    }
    int producerTarget() const noexcept { return producer_; }
    const std::vector<FrameRow>& frames() const noexcept { return frames_; }

    // After the window, on the worker: each frame's module and RVA, each target's function and location tables, the class
    // counts, the render thread's handshake wait (gated on the game's build). Takes the loader's locks; threads are running.
    void resolve() {
        nMods_ = 0;
        eliteFound_ = false;
        eliteGateOk_ = false;
        eliteStamp_ = 0;
        eliteSize_ = 0;
        if (HMODULE h = GetModuleHandleW(L"EliteDangerous64.exe")) {
            eliteFound_ = true;
            if (peStampAndSize(reinterpret_cast<uintptr_t>(h), eliteStamp_, eliteSize_))
                eliteGateOk_ = eliteBuildGate(eliteStamp_, eliteSize_);
        }
        for (unsigned i = 0; i < count_; ++i) {
            Sample& s = samples_[i];
            const unsigned t = s.target < nTargets_ ? s.target : 0;
            Agg& a = aggs_[t];
            if (!s.frames) {
                ++a.failures;
                continue;
            }
            ++valid_;
            ++a.valid;
            int mods[kFrames] = {};
            uint64_t keys[kFrames] = {};
            bool handshakeFrame = false, xrFrame = false, gpuFrame = false, edvrStack = false;
            uint32_t classBits = 0;
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
                mods[j] = m;
                keys[j] = fnKey(m, begin, s.pc[j]);
                if (m >= 0) {
                    const ModuleRec& mr = mods_[m];
                    if (mr.cls == Class::Elite && rva >= kEliteWaitBegin && rva < kEliteWaitEnd) handshakeFrame = true;
                    if (mr.cls == Class::EdvrOpenvr || (mr.cls == Class::Vendor && !isUpscalerName(mr.name))) xrFrame = true;
                    if ((mr.cls == Class::Vendor && isUpscalerName(mr.name)) || mr.cls == Class::D3dDriver) gpuFrame = true;
                    if (mr.cls == Class::EdvrD3d11 || mr.cls == Class::EdvrOpenvr) edvrStack = true;
                }
                // The function table of every sample, then (below) of the busy ones.
                if (FnRec* fr = a.fns.add(keys[j])) nameFn(*fr, m, begin, end, s.pc[j]);
                if (m >= 0) classBits |= 1u << static_cast<unsigned>(mods_[m].cls);
                else classBits |= 1u << static_cast<unsigned>(Class::Unknown);
                if (j == 0) {
                    if (LocRec* lr = a.locs.add(locKey(m, rva, s.pc[j]))) {
                        lr->mod = static_cast<int16_t>(m);
                        lr->rva = rva;
                        ++lr->count;
                    }
                    a.leafClass[static_cast<unsigned>(m >= 0 ? mods_[m].cls : Class::Unknown)]++;
                }
            }
            countFunctions(a.fns, keys, s.frames);
            exclusiveFunction(a.fns, keys[0]);
            for (unsigned c = 0; c < static_cast<unsigned>(Class::Count); ++c) {
                if (classBits & (1u << c)) a.stackClass[c]++;
            }
            if (handshakeFrame) ++a.waitSamples;
            // Busy or waiting: the leaf's function, by the OS image's export table.
            StubKind stub = StubKind::None;
            const int leafMod = mods[0];
            if (leafMod >= 0 && mods_[leafMod].cls == Class::NtdllKernel) {
                ModuleRec& lm = mods_[leafMod];
                if (!lm.exportsLoaded) {
                    loadExports(lm.base, lm.exports);
                    lm.exportsLoaded = true;
                }
                stub = stubKindAt(lm.exports, s.rva[0]);
            }
            const WaitKind kind = attributeSample(stub, handshakeFrame && eliteGateOk_, xrFrame, gpuFrame);
            s.kind = static_cast<uint8_t>(kind);
            s.edvrStack = edvrStack ? 1 : 0;
            const Class leafCls = leafMod >= 0 ? mods_[leafMod].cls : Class::Unknown;
            s.edvrLeaf = leafCls == Class::EdvrD3d11 ? 1 : leafCls == Class::EdvrOpenvr ? 2 : 0;
            ++a.kindCount[static_cast<unsigned>(kind)];
            if (kind == WaitKind::Busy) {
                ++a.busyLeafClass[static_cast<unsigned>(leafCls)];
                if (s.edvrLeaf == 1) ++a.edvrLeafD3d;
                if (s.edvrLeaf == 2) ++a.edvrLeafOpenvr;
                if (edvrStack) ++a.edvrStackBusy;
                // The busy functions: the same walk, into the table of the samples that were running.
                for (unsigned j = 0; j < s.frames; ++j) {
                    if (FnRec* fr = a.busyFns.add(keys[j])) {
                        if (fr->incl == 0 && fr->excl == 0) {
                            const FnRec* all = a.fns.find(keys[j]);
                            if (all) { fr->mod = all->mod; fr->begin = all->begin; fr->end = all->end; }
                        }
                    }
                }
                countFunctions(a.busyFns, keys, s.frames);
                exclusiveFunction(a.busyFns, keys[0]);
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
    unsigned targetCount() const noexcept { return nTargets_; }
    const Target& targetAt(unsigned t) const noexcept { return targets_[t]; }
    unsigned targetValid(unsigned t) const noexcept { return t < nTargets_ ? aggs_[t].valid : 0; }
    unsigned targetWaits(unsigned t) const noexcept { return t < nTargets_ ? aggs_[t].waitSamples : 0; }
    bool eliteGateOk() const noexcept { return eliteGateOk_; }
    const Sample* sampleAt(unsigned i) const noexcept { return i < count_ ? &samples_[i] : nullptr; }
    double achievedHz() const noexcept { return wallUs_ > 0.0 ? count_ * 1e6 / wallUs_ : 0.0; }

    // Which of this session's targets a label names, or -1.
    int targetIndex(const char* label) const noexcept {
        for (unsigned t = 0; t < nTargets_; ++t) {
            if (std::strcmp(targets_[t].label, label) == 0) return static_cast<int>(t);
        }
        return -1;
    }

    // The log's start line: written before the window, so a run that dies in it still says it began.
    void emitStart(LineSink sink, void* user, const RunInfo& info) const {
        detail::put(sink, user,
                    "cpu profile: start thread=render trigger=%s window_s=%.1f target_hz=%.0f duty_cap_pct=%.1f "
                    "threads=%u csv=%s",
                    info.trigger, info.windowS, info.targetHz, info.dutyPct, info.threads, info.csvName);
    }

    // The report: the pacing line, then for each thread its CPU line, its top functions (inclusive and exclusive) and its
    // class split; the render thread's top leaves and its producer wait; and the end line. Every line starts "cpu profile:"
    // and is under 1100 characters.
    void emitReport(LineSink sink, void* user, const RunInfo& info) const {
        const double wallMs = wallUs_ / 1000.0;
        const double suspendPct = wallUs_ > 0.0 ? 100.0 * suspendTotalUs_ / wallUs_ : 0.0;
        const double meanUs = count_ ? suspendTotalUs_ / count_ : 0.0;
        detail::put(sink, user,
                    "cpu profile: pacing samples=%u valid=%u failures=%u buffer_full=%d achieved_hz=%.1f wall_ms=%.1f "
                    "suspended_ms=%.2f (%.2f%% of wall) suspend_mean_us=%.1f suspend_max_us=%u timer=%s threads=%u",
                    count_, valid_, failures_, bufferFull_ ? 1 : 0, achievedHz(), wallMs, suspendTotalUs_ / 1000.0,
                    suspendPct, meanUs, suspendMaxUs_, timer_highres_ ? "high_res" : "sleep_1ms", nTargets_);
        for (unsigned t = 0; t < nTargets_; ++t) {
            emitThread(sink, user, t, wallMs);
            emitAccounting(sink, user, t, wallMs);
        }
        emitWait(sink, user, info);
        emitFrameSummary(sink, user, info);
        detail::put(sink, user, "cpu profile: end samples=%u valid=%u csv=%s%s", count_, valid_, info.csvName,
                    valid_ ? "" : " (no sample read a thread: the tables are empty)");
    }

    // The raw samples as CSV: the contract tools\elite_hotspots.py reads (and edvr_log.py --cpu-profile <csv>).
    //   one row per valid sample: thread,module,rva_hex,frame1_module,frame1_rva_hex,...,frame8_module,frame8_rva_hex
    //   thread is the target's label ("render", "main" or "t<tid>"); module is the file name of the module holding the
    //   leaf ("?" when none does); rva_hex is hex without 0x; frames run innermost first, at most 8, the leaf not repeated,
    //   and a short row ends the frame list.
    //   Lines starting # are comments: the build, the sample rate and the window, plus two comment tables the re-aggregation
    //   needs (# module,<name>,<class> and # function,<name>,<begin hex>,<end hex> from .pdata).
    //   The optional header line starts with thread,module,rva_hex.
    bool writeCsv(FILE* f, const RunInfo& info) const {
        if (!f || !resolved_) return false;
        constexpr unsigned kCsvFrames = 8;
        std::fprintf(f, "# edvr cpu profile raw samples: one row per valid sample, the thread first, then the leaf, then up "
                        "to 8 return frames, innermost first; a short row ends its frame list\n");
        std::fprintf(f, "# build=%s thread=%lu trigger=%s window_s=%.3f sample_rate_target_hz=%.1f achieved_hz=%.2f "
                        "samples=%u valid=%u failures=%u threads=%u presents=%llu\n",
                     info.build, static_cast<unsigned long>(info.thread), info.trigger, info.windowS, info.targetHz,
                     achievedHz(), count_, valid_, failures_, nTargets_, static_cast<unsigned long long>(presents_));
        for (unsigned t = 0; t < nTargets_; ++t) {
            std::fprintf(f, "# target,%s,%s,%s\n", targets_[t].label, targets_[t].role, targets_[t].start);
        }
        for (unsigned m = 0; m < nMods_; ++m) {
            std::fprintf(f, "# module,%s,%s\n", mods_[m].name, className(mods_[m].cls));
        }
        // Every .pdata range a frame landed in, across the threads, once each.
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> seen;
        for (unsigned t = 0; t < nTargets_; ++t) {
            aggs_[t].fns.each([&](const FnRec& r) {
                if (r.mod >= 0 && r.begin != kNoFunction)
                    seen.push_back({std::string(mods_[r.mod].name), {r.begin, r.end}});
            });
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        for (const auto& r : seen) {
            std::fprintf(f, "# function,%s,%x,%x\n", r.first.c_str(), r.second.first, r.second.second);
        }
        std::fprintf(f, "thread,module,rva_hex");
        for (unsigned j = 1; j <= kCsvFrames; ++j) std::fprintf(f, ",frame%u_module,frame%u_rva_hex", j, j);
        std::fputc('\n', f);
        for (unsigned i = 0; i < count_; ++i) {
            const Sample& s = samples_[i];
            if (!s.frames) continue;
            std::fprintf(f, "%s,%s,%x", targets_[s.target].label, nameOf(s.mod[0]), s.rva[0]);
            for (unsigned j = 1; j < s.frames && j <= kCsvFrames; ++j)
                std::fprintf(f, ",%s,%x", nameOf(s.mod[j]), s.rva[j]);
            std::fputc('\n', f);
        }
        return std::ferror(f) == 0;
    }

    // The per-frame CSV, one row per complete frame. Counts are samples that fell in the frame; the *_ms columns scale a
    // thread's share of its samples in the frame to the frame's length (blank when the thread had none in it).
    bool writeFramesCsv(FILE* f) const {
        if (!f || !resolved_) return false;
        const int r = targetIndex("render"), o = targetIndex("owner"), m = targetIndex("main");
        std::fprintf(f, "# edvr cpu profile frames: one row per complete Present-to-Present frame; counts are samples; ms columns "
                        "are share_of_samples * frame_ms. crit: producer render xr gpu sync unknown. producer=%s\n",
                     producer_ >= 0 ? targets_[producer_].label : "none");
        std::fprintf(f, "frame,start_ms,frame_ms,crit,render_n,render_busy,render_handshake,render_xr,render_gpu,render_lock,"
                        "render_event,render_sleep,render_edvr,producer_n,producer_busy,owner_n,owner_busy,main_n,main_busy,"
                        "render_busy_ms,render_wait_ms,producer_busy_ms,owner_busy_ms,render_edvr_ms\n");
        auto cnt = [&](int t, const uint16_t* arr) { return t >= 0 ? arr[t] : 0; };
        auto ms = [&](double frameMs, unsigned n, unsigned d) {
            char buf[32];
            if (!d) { buf[0] = 0; return std::string(buf); }
            std::snprintf(buf, sizeof(buf), "%.3f", frameMs * n / d);
            return std::string(buf);
        };
        for (const FrameRow& fr : frames_) {
            const unsigned rn = cnt(r, fr.n), rb = cnt(r, fr.busy), pn = cnt(producer_, fr.n), pb = cnt(producer_, fr.busy);
            const unsigned on = cnt(o, fr.n), ob = cnt(o, fr.busy), mn = cnt(m, fr.n), mb = cnt(m, fr.busy);
            std::fprintf(f, "%u,%.3f,%.3f,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%s,%s,%s,%s\n", fr.index, fr.startMs,
                         fr.frameMs, critName(fr.crit), rn, rb, fr.renderKind[static_cast<unsigned>(WaitKind::Handshake)],
                         fr.renderKind[static_cast<unsigned>(WaitKind::Xr)], fr.renderKind[static_cast<unsigned>(WaitKind::Gpu)],
                         fr.renderKind[static_cast<unsigned>(WaitKind::Lock)], fr.renderKind[static_cast<unsigned>(WaitKind::Event)],
                         fr.renderKind[static_cast<unsigned>(WaitKind::Sleep)], cnt(r, fr.edvr), pn, pb, on, ob, mn, mb,
                         ms(fr.frameMs, rb, rn).c_str(), ms(fr.frameMs, rn - rb, rn).c_str(), ms(fr.frameMs, pb, pn).c_str(),
                         ms(fr.frameMs, ob, on).c_str(), ms(fr.frameMs, cnt(r, fr.edvr), rn).c_str());
        }
        return std::ferror(f) == 0;
    }

private:
    struct Agg {
        Table<FnRec> fns;
        Table<FnRec> busyFns;              // the same, over the samples that were running (not waiting)
        Table<LocRec> locs;
        uint32_t kindCount[kKinds] = {};   // samples by WaitKind
        uint32_t busyLeafClass[static_cast<unsigned>(Class::Count)] = {};
        uint32_t edvrLeafD3d = 0, edvrLeafOpenvr = 0, edvrStackBusy = 0;
        uint32_t valid = 0, failures = 0, waitSamples = 0;
        uint32_t leafClass[static_cast<unsigned>(Class::Count)] = {};
        uint32_t stackClass[static_cast<unsigned>(Class::Count)] = {};
    };

    static void nameFn(FnRec& fr, int m, uint32_t begin, uint32_t end, uintptr_t pc) noexcept {
        fr.mod = static_cast<int16_t>(m);
        fr.begin = m < 0 ? static_cast<uint32_t>(pc) : begin;
        fr.end = m < 0 ? 0 : end;
    }
    // Inclusive: each distinct function of the walked stack once.
    static void countFunctions(Table<FnRec>& t, const uint64_t* keys, unsigned n) noexcept {
        for (unsigned j = 0; j < n; ++j) {
            bool dup = false;
            for (unsigned k = 0; k < j; ++k) dup = dup || keys[k] == keys[j];
            if (dup) continue;
            if (FnRec* fr = t.add(keys[j])) ++fr->incl;
        }
    }
    static void exclusiveFunction(Table<FnRec>& t, uint64_t key) noexcept {
        if (FnRec* fr = t.add(key)) ++fr->excl;
    }

    // The index of the module holding `pc`, adding it to the table the first time it is seen. -1 when none holds it.
    int findModule(uintptr_t pc, uintptr_t& base) noexcept {
        char name[32] = {};
        if (!moduleOfAddress(pc, base, name, sizeof(name))) return -1;
        for (unsigned m = 0; m < nMods_; ++m) {
            if (mods_[m].base == base) return static_cast<int>(m);
        }
        if (nMods_ >= kMaxModules) return -1;
        ModuleRec& r = mods_[nMods_];
        r = ModuleRec{};
        r.base = base;
        std::memcpy(r.name, name, sizeof(r.name));
        r.cls = classify(r.name, base == reinterpret_cast<uintptr_t>(stall::thisModule()));
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

    // One thread's lines: its CPU, its functions, its classes; the render thread also its leaves.
    void emitThread(LineSink sink, void* user, unsigned t, double wallMs) const {
        const Target& tg = targets_[t];
        const Agg& a = aggs_[t];
        const double cpuMs = tg.cpuEnd >= tg.cpuStart ? (tg.cpuEnd - tg.cpuStart) / 10000.0 : 0.0;
        const double pctOf = a.valid ? 100.0 / a.valid : 0.0;
        const double ratePerS = wallMs > 0.0 ? a.valid * 1000.0 / wallMs : 0.0;
        detail::put(sink, user,
                    "cpu profile: thread=%s role=%s start=%s cpu_ms=%.1f cpu_pct=%.2f%% valid=%u failures=%u rate_hz=%.1f",
                    tg.label, tg.role, tg.start, cpuMs, wallMs > 0.0 ? 100.0 * cpuMs / wallMs : 0.0, a.valid, a.failures,
                    ratePerS);
        std::vector<const FnRec*> fns;
        a.fns.each([&](const FnRec& r) { fns.push_back(&r); });
        topSort(fns, [](const FnRec* x, const FnRec* y) {
            return x->incl != y->incl ? x->incl > y->incl : x->key < y->key;
        });
        for (unsigned i = 0; i < std::min<size_t>(kTopN, fns.size()); ++i) {
            const FnRec& r = *fns[i];
            char where[96];
            fnName(r, where, sizeof(where));
            detail::put(sink, user, "cpu profile: thread=%s fn %2u incl %7u %6.2f%% excl %7u %6.2f%% %s", tg.label, i + 1,
                        r.incl, r.incl * pctOf, r.excl, r.excl * pctOf, where);
        }
        for (unsigned c = 0; c < static_cast<unsigned>(Class::Count); ++c) {
            if (!a.leafClass[c] && !a.stackClass[c]) continue;
            detail::put(sink, user, "cpu profile: thread=%s class %s leaf %u %6.2f%% on_stack %u %6.2f%%", tg.label,
                        className(static_cast<Class>(c)), a.leafClass[c], a.leafClass[c] * pctOf, a.stackClass[c],
                        a.stackClass[c] * pctOf);
        }
        if (!tg.render) return;
        std::vector<const LocRec*> locs;
        a.locs.each([&](const LocRec& r) { locs.push_back(&r); });
        topSort(locs, [](const LocRec* x, const LocRec* y) {
            return x->count != y->count ? x->count > y->count : x->key < y->key;
        });
        for (unsigned i = 0; i < std::min<size_t>(kTopLeaves, locs.size()); ++i) {
            const LocRec& r = *locs[i];
            char where[64];
            locName(r.mod, r.rva, where, sizeof(where));
            detail::put(sink, user, "cpu profile: thread=%s leaf %2u %7u %6.2f%% %s", tg.label, i + 1, r.count, r.count * pctOf,
                        where);
        }
    }

    // Milliseconds per frame that `count` of a thread's `valid` samples stand for: the share of the window, over the Presents.
    double perFrameMs(uint32_t count, uint32_t valid) const noexcept {
        if (!valid || !presents_) return 0.0;
        return static_cast<double>(count) / valid * (wallUs_ / 1000.0) / static_cast<double>(presents_);
    }

    // One thread's frame accounting, in milliseconds per frame: busy against waiting and what it waited on, where the busy
    // time sat by module class, EDVR's share (leaf and stack) and its top functions; for the render thread and the inferred
    // producer, the game executable's top functions among the samples that were running.
    void emitAccounting(LineSink sink, void* user, unsigned t, double wallMs) const {
        const Target& tg = targets_[t];
        const Agg& a = aggs_[t];
        if (!a.valid) return;
        const double frameMs = presents_ ? wallMs / static_cast<double>(presents_) : 0.0;
        const double cpuMs = tg.cpuEnd >= tg.cpuStart ? (tg.cpuEnd - tg.cpuStart) / 10000.0 : 0.0;
        const uint32_t busy = a.kindCount[static_cast<unsigned>(WaitKind::Busy)];
        detail::put(sink, user,
                    "cpu profile: thread=%s per_frame frames=%llu frame_ms=%.3f busy_ms=%.3f wait_ms=%.3f cpu_ms=%.3f "
                    "edvr_d3d11_ms=%.3f edvr_openvr_ms=%.3f edvr_stack_ms=%.3f",
                    tg.label, static_cast<unsigned long long>(presents_), frameMs, perFrameMs(busy, a.valid),
                    perFrameMs(a.valid - busy, a.valid), presents_ ? cpuMs / static_cast<double>(presents_) : 0.0,
                    perFrameMs(a.edvrLeafD3d, a.valid), perFrameMs(a.edvrLeafOpenvr, a.valid),
                    perFrameMs(a.edvrStackBusy, a.valid));
        auto k = [&](WaitKind w) { return perFrameMs(a.kindCount[static_cast<unsigned>(w)], a.valid); };
        detail::put(sink, user,
                    "cpu profile: thread=%s wait_split frame_ms=%.3f busy=%.3f handshake=%.3f xr=%.3f gpu=%.3f lock=%.3f "
                    "event=%.3f sleep=%.3f samples=%u%s",
                    tg.label, frameMs, k(WaitKind::Busy), k(WaitKind::Handshake), k(WaitKind::Xr), k(WaitKind::Gpu),
                    k(WaitKind::Lock), k(WaitKind::Event), k(WaitKind::Sleep), a.valid,
                    eliteGateOk_ ? "" : " handshake=unavailable(game build is not 332841)");
        auto c = [&](Class cl) { return perFrameMs(a.busyLeafClass[static_cast<unsigned>(cl)], a.valid); };
        detail::put(sink, user,
                    "cpu profile: thread=%s busy_split elite=%.3f d3d_driver=%.3f edvr_d3d11=%.3f edvr_openvr=%.3f "
                    "vendor=%.3f system=%.3f other=%.3f",
                    tg.label, c(Class::Elite), c(Class::D3dDriver), c(Class::EdvrD3d11), c(Class::EdvrOpenvr),
                    c(Class::Vendor), c(Class::NtdllKernel), c(Class::Other) + c(Class::Unknown));
        auto top = [&](const char* tag, bool edvrOnly, unsigned limit) {
            std::vector<const FnRec*> v;
            a.busyFns.each([&](const FnRec& r) {
                if (r.mod < 0 || r.begin == kNoFunction) return;
                const Class cl = mods_[r.mod].cls;
                if (edvrOnly ? (cl == Class::EdvrD3d11 || cl == Class::EdvrOpenvr) : cl == Class::Elite) v.push_back(&r);
            });
            std::sort(v.begin(), v.end(), [](const FnRec* x, const FnRec* y) {
                return x->incl != y->incl ? x->incl > y->incl : x->key < y->key;
            });
            for (unsigned i = 0; i < std::min<size_t>(limit, v.size()); ++i) {
                char where[96];
                fnName(*v[i], where, sizeof(where));
                detail::put(sink, user, "cpu profile: thread=%s %s %2u incl %.3f excl %.3f ms_per_frame %s", tg.label, tag,
                            i + 1, perFrameMs(v[i]->incl, a.valid), perFrameMs(v[i]->excl, a.valid), where);
            }
        };
        top("edvr_fn", true, 10);
        if (tg.render || static_cast<int>(t) == producer_) top("elite_fn", false, kTopN);
    }

    // The producer, the critical path over the window's frames, and the 30-frame summaries.
    void emitFrameSummary(LineSink sink, void* user, const RunInfo& info) const {
        const double wallMs = wallUs_ / 1000.0;
        if (producer_ < 0) {
            detail::put(sink, user, "cpu profile: producer thread=none (no thread other than the render thread and the owner "
                                    "was read with samples)");
        } else {
            const Target& tg = targets_[producer_];
            const Agg& a = aggs_[producer_];
            const double share = a.valid ? static_cast<double>(a.kindCount[0]) / a.valid : 0.0;
            const double frameMs = presents_ ? wallMs / static_cast<double>(presents_) : 0.0;
            const double cpuMs = tg.cpuEnd >= tg.cpuStart ? (tg.cpuEnd - tg.cpuStart) / 10000.0 : 0.0;
            detail::put(sink, user,
                        "cpu profile: producer thread=%s start=%s inferred=busiest_thread_besides_render_and_owner "
                        "busy_ms=%.3f frame_ms=%.3f busy_pct_of_frame=%.1f cpu_ms=%.3f",
                        tg.label, tg.start, share * frameMs, frameMs, 100.0 * share,
                        presents_ ? cpuMs / static_cast<double>(presents_) : 0.0);
        }
        if (frames_.empty()) {
            detail::put(sink, user, "cpu profile: critical frames=0 (no Present timestamps, or no complete frame in the window)");
            detail::put(sink, user, "cpu profile: frames_csv=%s rows=0", info.framesCsvName);
            return;
        }
        unsigned crit[kCrits] = {};
        for (const FrameRow& f : frames_) ++crit[static_cast<unsigned>(f.crit)];
        detail::put(sink, user,
                    "cpu profile: critical frames=%u producer=%u render=%u xr=%u gpu=%u sync=%u unknown=%u "
                    "rule=render_waited_25pct_or_more_and_its_longest_wait_names_the_path",
                    static_cast<unsigned>(frames_.size()), crit[static_cast<unsigned>(Crit::Producer)],
                    crit[static_cast<unsigned>(Crit::Render)], crit[static_cast<unsigned>(Crit::Xr)],
                    crit[static_cast<unsigned>(Crit::Gpu)], crit[static_cast<unsigned>(Crit::Sync)],
                    crit[static_cast<unsigned>(Crit::Unknown)]);
        const int r = targetIndex("render"), o = targetIndex("owner");
        for (size_t at = 0; at < frames_.size(); at += kFrameBlock) {
            const size_t end = std::min(frames_.size(), at + kFrameBlock);
            if (end - at < kFrameBlock / 3 && at > 0) break;   // a short tail is not a summary
            double frameSum = 0.0;
            unsigned rn = 0, rbusy = 0, redvr = 0, pn = 0, pbusy = 0, on = 0, obusy = 0, rk[kKinds] = {}, cc[kCrits] = {};
            for (size_t i = at; i < end; ++i) {
                const FrameRow& f = frames_[i];
                frameSum += f.frameMs;
                if (r >= 0) { rn += f.n[r]; rbusy += f.busy[r]; redvr += f.edvr[r]; }
                if (producer_ >= 0) { pn += f.n[producer_]; pbusy += f.busy[producer_]; }
                if (o >= 0) { on += f.n[o]; obusy += f.busy[o]; }
                for (unsigned k = 0; k < kKinds; ++k) rk[k] += f.renderKind[k];
                ++cc[static_cast<unsigned>(f.crit)];
            }
            const double fm = frameSum / static_cast<double>(end - at);
            auto share = [&](unsigned n, unsigned d) { return d ? fm * n / d : 0.0; };
            detail::put(sink, user,
                        "cpu profile: window frames=%u-%u n=%u frame_ms=%.2f render_busy=%.2f wait_handshake=%.2f wait_xr=%.2f "
                        "wait_gpu=%.2f wait_sync=%.2f producer_busy=%.2f owner_busy=%.2f render_edvr=%.2f render_n=%u producer_n=%u "
                        "crit=producer:%u,render:%u,xr:%u,gpu:%u,sync:%u,unknown:%u",
                        static_cast<unsigned>(at), static_cast<unsigned>(end - 1), static_cast<unsigned>(end - at), fm,
                        share(rbusy, rn), share(rk[static_cast<unsigned>(WaitKind::Handshake)], rn),
                        share(rk[static_cast<unsigned>(WaitKind::Xr)], rn), share(rk[static_cast<unsigned>(WaitKind::Gpu)], rn),
                        share(rk[static_cast<unsigned>(WaitKind::Lock)] + rk[static_cast<unsigned>(WaitKind::Event)] +
                                  rk[static_cast<unsigned>(WaitKind::Sleep)], rn),
                        share(pbusy, pn), share(obusy, on), share(redvr, rn), rn, pn, cc[static_cast<unsigned>(Crit::Producer)],
                        cc[static_cast<unsigned>(Crit::Render)], cc[static_cast<unsigned>(Crit::Xr)],
                        cc[static_cast<unsigned>(Crit::Gpu)], cc[static_cast<unsigned>(Crit::Sync)],
                        cc[static_cast<unsigned>(Crit::Unknown)]);
        }
        detail::put(sink, user, "cpu profile: frames_csv=%s rows=%u", info.framesCsvName, static_cast<unsigned>(frames_.size()));
    }

    // The render thread's time in the producer handshake, per frame: the samples whose stack holds the handshake's
    // function, scaled to the window's wall time and divided by the presents. Measured only on the gated build.
    void emitWait(LineSink sink, void* user, const RunInfo& info) const {
        (void)info;
        const int r = targetIndex("render");
        if (r < 0) return;
        if (!eliteFound_) {
            detail::put(sink, user, "cpu profile: thread=render waits for producer: not measured: EliteDangerous64.exe is not "
                                    "loaded in this process");
            return;
        }
        if (!eliteGateOk_) {
            detail::put(sink, user, "cpu profile: thread=render waits for producer: not measured: the game's PE stamp %u and "
                                    "image size %u are not build 332841 (stamp %u, size %u)",
                        eliteStamp_, eliteSize_, kEliteStamp, kEliteImageSize);
            return;
        }
        const uint32_t valid = aggs_[r].valid;
        const uint32_t waits = aggs_[r].waitSamples;
        const double wallMs = wallUs_ / 1000.0;
        const double waitMs = valid ? waits * wallMs / valid : 0.0;
        const double perFrame = presents_ ? waitMs / static_cast<double>(presents_) : 0.0;
        detail::put(sink, user,
                    "cpu profile: thread=render waits for producer: ms_per_frame=%.3f frames=%llu samples=%u of %u "
                    "share_pct=%.2f window_ms=%.1f wait_ms=%.1f",
                    perFrame, static_cast<unsigned long long>(presents_), waits, valid,
                    valid ? 100.0 * waits / valid : 0.0, wallMs, waitMs);
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
    unsigned long long presents_ = 0;
    uint64_t t0Qpc_ = 0;               // QueryPerformanceCounter at the window's start
    double freqQpc_ = 0.0;
    std::vector<double> presentUs_;    // the render thread's Presents, from the window's start
    std::vector<FrameRow> frames_;
    int producer_ = -1;

    unsigned nTargets_ = 0;
    Target targets_[kMaxTargets];
    std::unique_ptr<Agg[]> aggs_;

    std::unique_ptr<ModuleRec[]> mods_;
    unsigned nMods_ = 0;
    bool eliteFound_ = false;
    bool eliteGateOk_ = false;
    uint32_t eliteStamp_ = 0;
    uint32_t eliteSize_ = 0;
};

}  // namespace edvr::cpu
