#pragma once
// The producer probe for FUN_14434D120 (Elite build 332841, RVA 0x434D120): a COUNT-AND-TAG instrument, TEMPORARY, no key, no emission, no
// behaviour change (docs/dlss-blur-elements-2026-10-14.md, "Producer probe, 2026-10-16").
//
// Why it exists. The shield strike's pool records (slots 157/162/119/124) never reach the hooked rigid producers (42B4130 inside the builder
// 42B4420, 4312E00, 369C9C0). The candidate that does build them is FUN_14434D470 (matrix to packed quaternion), which appends through
// FUN_14434D120(bucket = rcx, modelKey = rdx = matrixOwner+0x180, &record = r8, nodeKey = r9), onto the SAME bucket and node structure
// the rigid producers use (FUN_143696FA0(bucket+0x260, key), a 0x150-byte record copy into a node, bucket+0x2A4 bumped). Nothing in EDVR
// hooked or counted it. This header holds everything the hook does and the log says, apart from the hook itself, so that a rig can drive it
// without the game: the prologue verdict, the lock-free window counters, the 30 s line, and the eye-run capture.
//
// Hot path. The hook runs 77+ times a frame on job threads. Everything here is a relaxed atomic add or a CAS into a fixed table: no
// allocation, no lock, no syscall. The only allocation is the eye-run capture buffer, made when the eye run arms (never on the hot path).
//
// Windows. Counters live in two Window cells; takeWindow() (the 30 s summary, render thread) flips the active cell, reads the retired one
// and clears the one that was retired before it. A straggler that loaded the old index just before the flip lands in the retired cell and
// is lost or read late: this is a diagnostic count, not an accounting, and the line says "per window" for that reason.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace edvr { namespace producer_probe {

constexpr uintptr_t kRva = 0x434D120u;
constexpr unsigned kPrologueBytes = 20;
// mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi; push rdi; sub rsp,20h. The first instruction is exactly five bytes, so the
// five-byte E9 patch lands on an instruction boundary.
constexpr uint8_t kPrologue[kPrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                               0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr unsigned kRecordBytes = 336;            // 0x150
constexpr unsigned kModelBlockOffset = 0x20;      // the model+0x478 block the rigid producers copy: +0x20..+0x9F
constexpr unsigned kModelBlockBytes = 0x80;

// --- The prologue verdict ---------------------------------------------------------------------------------------------------
// nullptr = go; otherwise the reason the probe stands down. expectOurs false: the stock prologue must be there (the install check).
// expectOurs true: our own E9 to `ourRelay` followed by the stock tail (the check after the install). A foreign patch, a game update
// and a wrong executable all stand the probe down, never patch over it.
inline const char* signatureVerdict(uint32_t timestamp, uint32_t imageSize, uint32_t wantTimestamp, uint32_t wantImageSize,
                                    const uint8_t (&got)[kPrologueBytes], bool expectOurs, intptr_t expectedJump) noexcept {
    if (timestamp != wantTimestamp || imageSize != wantImageSize) return "not build 332841 (PE timestamp/size)";
    const bool stock = std::memcmp(got, kPrologue, kPrologueBytes) == 0;
    if (!expectOurs) return stock ? nullptr : "prologue mismatch at RVA 0x434D120";
    if (stock) return "our patch is gone (FUN_14434D120's entry was rewritten)";
    int32_t jump = 0;
    std::memcpy(&jump, got + 1, 4);
    if (got[0] == 0xE9 && static_cast<intptr_t>(jump) == expectedJump && std::memcmp(got + 5, kPrologue + 5, kPrologueBytes - 5) == 0)
        return nullptr;
    return "prologue mismatch at RVA 0x434D120";
}

// --- Fixed lock-free tables --------------------------------------------------------------------------------------------------
template <unsigned N> struct Set {
    static_assert(N >= 8 && (N & (N - 1)) == 0, "a power of two");
    static constexpr unsigned kSlots = N;
    std::atomic<uint64_t> slot[N];
    std::atomic<uint32_t> count;
    std::atomic<uint32_t> overflow;
    static uint64_t norm(uint64_t key) noexcept { return key ? key : ~0ull; }
    static unsigned home(uint64_t k) noexcept { return static_cast<unsigned>((k * 0x9E3779B97F4A7C15ull) >> 40) & (N - 1); }
    // True when the key was new. A full table counts an overflow and takes nothing.
    bool insert(uint64_t key) noexcept {
        const uint64_t k = norm(key);
        unsigned i = home(k);
        for (unsigned n = 0; n < N; ++n, i = (i + 1) & (N - 1)) {
            uint64_t seen = slot[i].load(std::memory_order_relaxed);
            if (seen == k) return false;
            if (seen == 0) {
                if (slot[i].compare_exchange_strong(seen, k, std::memory_order_relaxed)) {
                    count.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                if (seen == k) return false;   // another thread put the same key here first
            }
        }
        overflow.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool contains(uint64_t key) const noexcept {
        const uint64_t k = norm(key);
        unsigned i = home(k);
        for (unsigned n = 0; n < N; ++n, i = (i + 1) & (N - 1)) {
            const uint64_t seen = slot[i].load(std::memory_order_relaxed);
            if (seen == k) return true;
            if (seen == 0) return false;
        }
        return false;
    }
    void clear() noexcept {
        for (unsigned i = 0; i < N; ++i) slot[i].store(0, std::memory_order_relaxed);
        count.store(0, std::memory_order_relaxed);
        overflow.store(0, std::memory_order_relaxed);
    }
};

constexpr unsigned kCallerSlots = 8;
enum RecordKind : unsigned { kModelBlockZero = 0, kModelBlockSet = 1, kUnreadable = 2 };
enum RigidSource : unsigned { kBuilder = 0, kDirect0 = 1, kDirect1 = 2 };   // 42B4420 (and 42B4130 inside it), 4312E00, 369C9C0

struct Window {
    std::atomic<uint64_t> calls;
    std::atomic<uint64_t> kinds[3];
    std::atomic<uint64_t> rigidNotes[3];
    std::atomic<uint32_t> renderSeen;
    std::atomic<uint64_t> callerRva[kCallerSlots];
    std::atomic<uint64_t> callerCount[kCallerSlots];
    std::atomic<uint32_t> callerOverflow;
    Set<1024> buckets;        // buckets 434D120 appended to
    Set<2048> rigidBuckets;   // buckets the rigid producers appended to (count delta > 0)
    Set<64> threads;          // threads 434D120 ran on
    Set<64> rigidThreads;     // threads the rigid producers appended on
    void clear() noexcept {
        calls.store(0, std::memory_order_relaxed);
        for (auto& k : kinds) k.store(0, std::memory_order_relaxed);
        for (auto& k : rigidNotes) k.store(0, std::memory_order_relaxed);
        renderSeen.store(0, std::memory_order_relaxed);
        for (unsigned i = 0; i < kCallerSlots; ++i) {
            callerRva[i].store(0, std::memory_order_relaxed);
            callerCount[i].store(0, std::memory_order_relaxed);
        }
        callerOverflow.store(0, std::memory_order_relaxed);
        buckets.clear();
        rigidBuckets.clear();
        threads.clear();
        rigidThreads.clear();
    }
};

inline Window g_window[2];
inline std::atomic<uint32_t> g_active{0};
inline std::atomic<bool> g_enabled{false};
inline std::atomic<const char*> g_status{"not requested"};

inline bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }
inline const char* status() noexcept { return g_status.load(std::memory_order_acquire); }
// The install outcome: nullptr reason = hooked. Anything else stands the probe down (nothing is counted, the line says why).
inline void applyVerdict(const char* why) noexcept {
    g_status.store(why ? why : "hooked", std::memory_order_release);
    g_enabled.store(why == nullptr, std::memory_order_release);
}
inline void resetAll() noexcept {   // the rig only
    g_window[0].clear();
    g_window[1].clear();
    g_active.store(0);
    g_enabled.store(false);
    g_status.store("not requested");
}

// One call of 434D120, after its forward. bucket = rcx, ret = the native caller's return address as an RVA, kind = RecordKind.
inline void noteCall(uint32_t tid, uint32_t renderTid, uint64_t bucket, uint64_t retRva, RecordKind kind) noexcept {
    if (!enabled()) return;
    Window& w = g_window[g_active.load(std::memory_order_relaxed) & 1];
    w.calls.fetch_add(1, std::memory_order_relaxed);
    w.kinds[kind].fetch_add(1, std::memory_order_relaxed);
    w.buckets.insert(bucket);
    w.threads.insert(tid);
    if (renderTid && tid == renderTid) w.renderSeen.store(1, std::memory_order_relaxed);
    for (unsigned i = 0; i < kCallerSlots; ++i) {
        uint64_t seen = w.callerRva[i].load(std::memory_order_relaxed);
        if (seen == 0 && w.callerRva[i].compare_exchange_strong(seen, retRva, std::memory_order_relaxed)) seen = retRva;
        if (seen == retRva) {
            w.callerCount[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    w.callerOverflow.fetch_add(1, std::memory_order_relaxed);
}
// A rigid producer's call that appended records to `bucket` on this thread.
inline void noteRigid(RigidSource source, uint32_t tid, uint64_t bucket) noexcept {
    if (!enabled()) return;
    Window& w = g_window[g_active.load(std::memory_order_relaxed) & 1];
    w.rigidNotes[source].fetch_add(1, std::memory_order_relaxed);
    if (!w.rigidBuckets.contains(bucket)) w.rigidBuckets.insert(bucket);
    if (!w.rigidThreads.contains(tid)) w.rigidThreads.insert(tid);
}

// --- The 30 s line -----------------------------------------------------------------------------------------------------------
constexpr unsigned kListedThreads = 6;
constexpr unsigned kListedCallers = 4;
struct Snapshot {
    uint64_t calls = 0, kinds[3] = {}, rigidNotes[3] = {};
    uint32_t buckets = 0, rigidBuckets = 0, shared = 0, bucketOverflow = 0, rigidOverflow = 0;
    uint32_t threads = 0, rigidThreads = 0, sharedThreads = 0, threadOverflow = 0;
    bool renderSeen = false;
    uint32_t listedThreads[kListedThreads] = {};
    unsigned listedThreadCount = 0;
    uint64_t callerRva[kListedCallers] = {}, callerCount[kListedCallers] = {};
    unsigned callerCountListed = 0;
    uint32_t callerOverflow = 0;
};

inline Snapshot takeWindow() noexcept {
    const uint32_t cur = g_active.load(std::memory_order_acquire) & 1;
    g_window[cur ^ 1].clear();                       // the window retired before this one; its stragglers are dropped here
    g_active.store(cur ^ 1, std::memory_order_release);
    Window& w = g_window[cur];
    Snapshot s;
    s.calls = w.calls.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < 3; ++i) {
        s.kinds[i] = w.kinds[i].load(std::memory_order_relaxed);
        s.rigidNotes[i] = w.rigidNotes[i].load(std::memory_order_relaxed);
    }
    s.buckets = w.buckets.count.load(std::memory_order_relaxed);
    s.bucketOverflow = w.buckets.overflow.load(std::memory_order_relaxed);
    s.rigidBuckets = w.rigidBuckets.count.load(std::memory_order_relaxed);
    s.rigidOverflow = w.rigidBuckets.overflow.load(std::memory_order_relaxed);
    s.threads = w.threads.count.load(std::memory_order_relaxed);
    s.threadOverflow = w.threads.overflow.load(std::memory_order_relaxed);
    s.rigidThreads = w.rigidThreads.count.load(std::memory_order_relaxed);
    s.renderSeen = w.renderSeen.load(std::memory_order_relaxed) != 0;
    for (unsigned i = 0; i < w.buckets.kSlots; ++i) {
        const uint64_t k = w.buckets.slot[i].load(std::memory_order_relaxed);
        if (k && w.rigidBuckets.contains(k)) ++s.shared;
    }
    for (unsigned i = 0; i < w.threads.kSlots; ++i) {
        const uint64_t k = w.threads.slot[i].load(std::memory_order_relaxed);
        if (!k) continue;
        if (w.rigidThreads.contains(k)) ++s.sharedThreads;
        if (s.listedThreadCount < kListedThreads) s.listedThreads[s.listedThreadCount++] = static_cast<uint32_t>(k);
    }
    std::sort(s.listedThreads, s.listedThreads + s.listedThreadCount);
    // The callers, busiest first.
    uint64_t rva[kCallerSlots], n[kCallerSlots];
    unsigned used = 0;
    for (unsigned i = 0; i < kCallerSlots; ++i) {
        const uint64_t r = w.callerRva[i].load(std::memory_order_relaxed);
        if (!r) continue;
        rva[used] = r;
        n[used++] = w.callerCount[i].load(std::memory_order_relaxed);
    }
    for (unsigned a = 0; a < used; ++a)
        for (unsigned b = a + 1; b < used; ++b)
            if (n[b] > n[a]) { std::swap(n[a], n[b]); std::swap(rva[a], rva[b]); }
    for (unsigned i = 0; i < used && i < kListedCallers; ++i) {
        s.callerRva[i] = rva[i];
        s.callerCount[i] = n[i];
        ++s.callerCountListed;
    }
    s.callerOverflow = w.callerOverflow.load(std::memory_order_relaxed) + (used > kListedCallers ? used - kListedCallers : 0u);
    return s;
}

constexpr size_t kLineBytes = 1100;   // the log's line buffer is 1200
// engine motion: producer 434D120 probe (hooked, 30 s, 1800 frames): calls 138600 (77.0 a frame), 41 distinct buckets (0 lost to a full table);
// 3 threads [1204 3392 5116], render thread no, 3 of them also appended for the rigid producers (5 threads); the rigid producers appended to
// 58 buckets (builder 42B4420 9000, direct 4312E00 4100, direct 369C9C0 0), 36 of the probe's 41 buckets among them; callers 0x434D741 x138600;
// model block +0x20..+0x9F zero 138600, set 0, unreadable 0.
inline void formatLine(const Snapshot& s, const char* statusText, double seconds, double frames, char (&out)[kLineBytes]) noexcept {
    size_t used = 0;
    auto put = [&](const char* format, auto... args) {
        if (used + 1 >= kLineBytes) return;
        const int n = std::snprintf(out + used, kLineBytes - used, format, args...);
        if (n > 0) used = std::min(used + static_cast<size_t>(n), kLineBytes - 1);
    };
    const bool hooked = statusText && std::strcmp(statusText, "hooked") == 0;
    if (!hooked) {
        put("engine motion: producer 434D120 probe (%s): the probe is not running, so nothing is counted. \"not requested\" means engine "
            "motion never attached it; any other text is the reason it stood down (the game build, a foreign patch on the prologue, or "
            "CodeHook refusing). This line is the only evidence; there is no count to read.",
            statusText ? statusText : "no status");
        out[used] = 0;
        return;
    }
    const double perFrame = frames > 0 ? static_cast<double>(s.calls) / frames : 0.0;
    put("engine motion: producer 434D120 probe (hooked, %.0f s, %.0f frames): calls %llu (%.1f a frame)", seconds, frames,
        static_cast<unsigned long long>(s.calls), perFrame);
    if (s.calls == 0) {
        put(", NEVER FIRED in this window: the producer did not run (or engine motion was off), so the shield records are not built here. "
            "The rigid producers appended to %u buckets this window (builder 42B4420 %llu, direct 4312E00 %llu, direct 369C9C0 %llu).",
            s.rigidBuckets, static_cast<unsigned long long>(s.rigidNotes[kBuilder]),
            static_cast<unsigned long long>(s.rigidNotes[kDirect0]), static_cast<unsigned long long>(s.rigidNotes[kDirect1]));
        out[used] = 0;
        return;
    }
    put(", %u distinct buckets (%u lost to a full table); %u threads [", s.buckets, s.bucketOverflow, s.threads);
    for (unsigned i = 0; i < s.listedThreadCount; ++i) put("%s%u", i ? " " : "", s.listedThreads[i]);
    put("%s], render thread %s, %u of them also appended for the rigid producers (%u rigid threads); ",
        s.threads > s.listedThreadCount ? " ..." : "", s.renderSeen ? "YES" : "no", s.sharedThreads, s.rigidThreads);
    put("the rigid producers appended to %u buckets (builder 42B4420 %llu, direct 4312E00 %llu, direct 369C9C0 %llu; %u lost), "
        "%u of the probe's %u buckets among them; callers",
        s.rigidBuckets, static_cast<unsigned long long>(s.rigidNotes[kBuilder]), static_cast<unsigned long long>(s.rigidNotes[kDirect0]),
        static_cast<unsigned long long>(s.rigidNotes[kDirect1]), s.rigidOverflow, s.shared, s.buckets);
    for (unsigned i = 0; i < s.callerCountListed; ++i)
        put(" 0x%llX x%llu", static_cast<unsigned long long>(s.callerRva[i]), static_cast<unsigned long long>(s.callerCount[i]));
    if (s.callerOverflow) put(" (+%u more)", s.callerOverflow);
    put("; model block +0x20..+0x9F zero %llu, set %llu, unreadable %llu.", static_cast<unsigned long long>(s.kinds[kModelBlockZero]),
        static_cast<unsigned long long>(s.kinds[kModelBlockSet]), static_cast<unsigned long long>(s.kinds[kUnreadable]));
    out[used] = 0;
}

// --- The eye run's capture ---------------------------------------------------------------------------------------------------
// While an eye run's ledger is armed, every call's record (all 336 bytes, as the caller built it) is kept with its bucket, model key, node
// key, thread, caller and the ledger frame it ran in, and written beside the ledger as producer434d120_<stamp>.bin. The pool dumps
// (pool_<stamp>_<frame>.bin) hold the records the copier uploaded; tools/eye_run_ledger.py matches the two by content and prints the
// pool slots each captured record landed in. The node address is not used: 434D120 returns it to nobody the hook can see, and the pool
// slot is what the ledger keys on.
constexpr uint32_t kCaptureCap = 8192;
#pragma pack(push, 8)
struct CaptureEntry {
    uint32_t frame, tid;
    uint64_t bucket, modelKey, nodeKey, ret;
    uint8_t record[kRecordBytes];
};
struct CaptureHeader {
    char magic[8];   // "EDVRP434"
    uint32_t version, entryBytes, count, dropped, frame0, frame1, recordBytes, pad;
};
#pragma pack(pop)
static_assert(sizeof(CaptureEntry) == 376, "tools/eye_run_ledger.py reads 376-byte entries");
static_assert(sizeof(CaptureHeader) == 40, "tools/eye_run_ledger.py reads a 40-byte header");

inline std::atomic<CaptureEntry*> g_capture{nullptr};
inline std::atomic<bool> g_captureOn{false};
inline std::atomic<uint32_t> g_captureCount{0}, g_captureDropped{0}, g_frame{0}, g_captureLo{0}, g_captureHi{0};

inline bool captureOn() noexcept { return g_captureOn.load(std::memory_order_relaxed); }
inline void setFrame(uint32_t frame) noexcept { g_frame.store(frame, std::memory_order_relaxed); }
// Arms the capture for ledger frames [lo, hi]. False when the buffer cannot be had (the line then says so; nothing else changes).
inline bool armCapture(uint32_t lo, uint32_t hi) noexcept {
    CaptureEntry* buffer = g_capture.load(std::memory_order_acquire);
    if (!buffer) {
        buffer = new (std::nothrow) CaptureEntry[kCaptureCap];
        if (!buffer) return false;
        g_capture.store(buffer, std::memory_order_release);
    }
    g_captureCount.store(0, std::memory_order_relaxed);
    g_captureDropped.store(0, std::memory_order_relaxed);
    g_captureLo.store(lo, std::memory_order_relaxed);
    g_captureHi.store(hi, std::memory_order_relaxed);
    g_captureOn.store(true, std::memory_order_release);
    return true;
}
inline void noteCapture(uint32_t tid, uint64_t bucket, uint64_t modelKey, uint64_t nodeKey, uint64_t retRva, const uint8_t* record) noexcept {
    if (!captureOn()) return;
    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    if (frame < g_captureLo.load(std::memory_order_relaxed) || frame > g_captureHi.load(std::memory_order_relaxed)) return;
    CaptureEntry* buffer = g_capture.load(std::memory_order_acquire);
    if (!buffer) return;
    const uint32_t i = g_captureCount.fetch_add(1, std::memory_order_relaxed);
    if (i >= kCaptureCap) {
        g_captureDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CaptureEntry& e = buffer[i];
    e.frame = frame;
    e.tid = tid;
    e.bucket = bucket;
    e.modelKey = modelKey;
    e.nodeKey = nodeKey;
    e.ret = retRva;
    std::memcpy(e.record, record, kRecordBytes);
}
struct CaptureView {
    CaptureHeader header;
    const CaptureEntry* entries;
};
// Stops the capture and returns what it holds (entries point into the buffer, which lives for the process).
inline CaptureView stopCapture() noexcept {
    g_captureOn.store(false, std::memory_order_release);
    CaptureView v{};
    const uint32_t taken = g_captureCount.load(std::memory_order_relaxed);
    const uint32_t kept = std::min(taken, kCaptureCap);
    std::memcpy(v.header.magic, "EDVRP434", 8);
    v.header.version = 1;
    v.header.entryBytes = sizeof(CaptureEntry);
    v.header.count = kept;
    v.header.dropped = g_captureDropped.load(std::memory_order_relaxed);
    v.header.frame0 = g_captureLo.load(std::memory_order_relaxed);
    v.header.frame1 = g_captureHi.load(std::memory_order_relaxed);
    v.header.recordBytes = kRecordBytes;
    v.entries = g_capture.load(std::memory_order_acquire);
    return v;
}

}}  // namespace edvr::producer_probe
