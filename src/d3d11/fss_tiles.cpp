#include "fss_tiles.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/eye_sync.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/timing.h"
#include "fss_tiles_core.h"
#include "fss_tiles_d3d.h"
#include "temporal_pass.h"   // temporalPassArmEyeDump: the automatic eye run
#include "eye_final_capture.h"   // g_reduceLevel: the automatic eye run covers the whole eye at reduced size
#include "device_hook.h"         // deviceHookFssModeLatch: the FSS latch, for the exit guard and the arrival
#include "fss_ring_hook.h"       // fssRingHookWindow / fssRingHookWindowSummary: the ring pass observer
#include "binding_shadow.h"   // bindingGet, bindingResolveProbe: the render target of a draw
#include "journal_watch.h"   // journalFssFocus: the zoom window's authority

// See fss_tiles_core.h for the story. Two jobs, both inside an FSS zoom window only:
//   1. the INSTRUMENT: read the 12-byte argument block of every tile-renderer dispatch back (copy to a staging ring, Map
//      two frames later with DO_NOT_WAIT, so nothing ever stalls) and log the group counts with the identities of the
//      argument buffer and of every SRV/UAV the dispatch used -- so one flight says whether the two views share them.
//   2. the CANDIDATE (fix.fss_eye_sync = tiles): the second view's dispatch is drawn from the first view's argument
//      buffer and buffer bindings, then the second view's own bindings are put back.
// The instrument runs under every value of the key (off, on, mirror, tiles), so an A/B reads the same lines.

namespace edvr {

namespace detail {
bool g_fssTilesActive = false;
bool g_fssTilesWatch = false;
bool g_fssTilesDraws = false;
}  // namespace detail

namespace {

using namespace fsstiles;

enum Decline : uint32_t { kDeclineShape, kDeclineNoEye0, kDeclineSameArgs, kDeclineOffset, kDeclineNoBuffers, kDeclineCount };

struct FrameBuf {
    uint32_t frame = 0;
    bool     used = false;
    bool     drained = true;
    bool     logFrame = false;
    uint32_t n = 0;
    Rec      rec[kMaxPerFrame];
};

struct EyeZero {
    bool     valid = false;
    uint32_t frame = 0;
    uint32_t off = 0;
    void*    args = nullptr;                 // a reference
    void*    views[kSrvSlots] = {};          // references
    Slot     slot[kSrvSlots];
};

// v2: one frame's copies of the lists and the tile-info buffers, read back two frames later.
struct Sample {
    uint32_t frame = 0;
    bool     used = false;
    bool     drained = true;
    bool     files = false;                 // write the raw bytes to disk
    bool     ranEye0 = false;               // eye 1 drew eye 0's lists on this frame (the tiles candidate)
    bool     have[kRegions] = {};
    uint32_t bytes[kRegions] = {};
    uint32_t groups[2][2] = {};             // [eye][cs] x group counts, filled from the argument read-back for the split line
    bool     haveGroups[2][2] = {};
};

struct OtherCs {
    uint64_t hash = 0;
    uint32_t n = 0;
};

struct State {
    FaultBudget budget{"fssTiles", 5};
    bool     lockstep = false;               // fix.fss_eye_sync = tiles
    bool     armedNoted = false;
    bool     lastLockstepNoted = false;
    RevealMachine rv;                        // v9: the gate, the window, the eye runs and the reveal capture's clock (fss_tiles_core.h)
    bool     zoomOpen = false;
    uint32_t frame = 0;
    uint32_t windowNo = 0;

    // per frame
    ViewTable views;
    uint32_t prevViews = 0;
    uint32_t occ = 0;
    Intern   ids;

    // the staging ring
    ID3D11Buffer* staging[kStagingRing] = {};
    bool     stagingFailed = false;
    FrameBuf fb[kStagingRing];
    Sample   smp[kStagingRing];
    ID3D11Buffer* pool[kStagingRing][kRegions] = {};
    bool     poolFailedNoted = false;
    WatchTable watch;
    uint32_t listLines = 0;
    uint32_t writeLines = 0;
    uint32_t writes[kWriteCalls][kRoles] = {};   // [call][Role]
    uint32_t samplesRead = 0;
    uint32_t sampleFiles = 0;
    uint32_t dumpFramesLeft = 0;            // the eye dump asked for the next frames' raw buffers
    uint32_t eyeDumpNotes = 0;
    bool     dirMade = false;
    ListSummary lastList[2][2];
    bool     haveLastList[2][2] = {};
    uint64_t lastInfoHash[2] = {};
    bool     haveLastInfo[2] = {};
    uint32_t lastLoggedFrame = 0;
    // v3: the band watch
    // v4
    struct FpItem {
        bool     have = false;
        bool     tex = false;
        uint32_t id = 0, w = 0, h = 0, fmt = 0, bytes = 0, bpp = 0;
    };
    struct Fp {
        uint32_t frame = 0;
        bool     used = false;
        bool     drained = true;
        FpItem   it[2 * kFpItems];           // [eye * kFpItems + k], kFpSlots[k]
        uint32_t cube[2][5] = {};            // slot 6: id, w, h, layers, format
    };
    struct GDraw {
        const void* res = nullptr;
        uint32_t eye = 0;
        Role     role = Role::None;
        uint32_t draws = 0, weight = 0;
        bool     haveVp = false;
        D3D11_VIEWPORT vp0{}, vpN{};
        D3D11_RECT sc0{}, scN{};
    };
    struct GTot {
        uint32_t frames = 0, draws = 0, weight = 0, minDraws = 0, maxDraws = 0, vpVaries = 0;
        bool     haveVp = false;
        D3D11_VIEWPORT vp0{};
    };
    Fp       fp[kStagingRing];
    ID3D11Resource* fpPool[kStagingRing][2 * kFpItems] = {};
    uint32_t inputLines = 0, fpRead = 0;
    uint32_t gridFrames[kFpItems] = {}, gridDiffer[kFpItems] = {};
    GDraw    gd[16];
    uint32_t gdN = 0;
    GTot     gtot[3][8];
    uint32_t gbufLines = 0;
    void*    rtvKey[48] = {};
    const void* rtvRes[48] = {};
    uint32_t rtvN = 0;
    bool     watchGbuf = false;
    uint32_t eventFrame = 0;
    // v6
    struct ShapeRec { uint32_t w = 0, h = 0, fmt = 0; };
    struct TgtRec { const void* res = nullptr; uint32_t shape = 0, order = 0; };
    struct ViewRec { void* view = nullptr; uint32_t shape = 0; const void* res = nullptr; };
    struct LEnt { DrawStat s; const void* target = nullptr; bool ring = false; uint32_t srvIds[4] = {}; };
    struct Solo { DrawKey k; uint32_t draws = 0, frames = 0; bool ring = false; };
    struct RingFam { uint64_t vs = 0, ps = 0; uint32_t draws = 0; };
    ShapeRec shapes[64];
    uint32_t shapeN = 0;
    TgtRec   tgts[64];
    uint32_t tgtN = 0;
    ViewRec  views2[128];
    uint32_t viewN = 0;
    LEnt     led[kLedgerCap];
    uint32_t ledN = 0, ledOverflow = 0;
    Solo     solo[32];
    uint32_t soloN = 0;
    RingFam  ringFam[8];
    uint32_t ringFamN = 0;
    Ranking  rank;
    uint32_t ledgerFrames = 0, framesWithDiff = 0, pairedFrames = 0, eyediffLines = 0, unpaired = 0, ringLines = 0;
    uint32_t ringFrames[3] = {}, ringDrawTotal[3] = {}, ringBoth[4] = {}, ringShared[4] = {};
    uint32_t rtvShape[48] = {};
    uint32_t reduceUntil = 0;
    uint32_t ledOverflowTotal = 0;
    struct BandFrame {
        BandRec  rec;
        uint32_t rawBytes[4] = {};          // [eye * 2 + role]
        uint8_t  raw[4][kBandRaw];
    };
    BandWatch bandWatch;
    BandFrame bring[kBandBefore + 1];
    uint32_t bringHead = 0, bringCount = 0;
    bool     windowEventFired = false;
    uint32_t windowEvents = 0;
    uint32_t autoRuns = 0;                  // session
    bool     autoRunPending = false;
    bool     autoArming = false;
    uint32_t afterLeft = 0;
    uint32_t bandLines = 0;
    uint32_t bandFiles = 0;
    double   lastSplit[2] = {-100.0, -100.0};

    // eye zero's bindings, per shader
    EyeZero  e0[2];
    Swap     sw;
    bool     dispatched = false;

    // the window's tallies
    uint32_t framesSeen = 0;
    uint32_t lastFrameCounted = 0xFFFFFFFFu;
    uint32_t targetDispatches = 0;
    uint32_t readbacks = 0;
    uint32_t dropped = 0;
    uint32_t lines = 0;
    uint32_t substituted = 0;
    uint32_t declined[kDeclineCount] = {};
    bool     declineNoted[kDeclineCount] = {};
    bool     engagedNoted = false;
    Range    range[3][2];
    Share    share;
    OtherCs  other[8];
    bool     summaryDue = false;
    uint32_t summaryAfterFrame = 0;
    bool     noTargetNoted = false;
    // A zoom signalled but no window opened: say why, a few times.
    bool     signalPending = false;
    bool     signalPress = false;
    uint32_t signalFrame = 0;
    uint32_t noWindowNotes = 0;
    uint32_t chromeAgo = 0xFFFFFFFFu;
    // v8
    LatchGuard latch;
    char     restartWhy[100] = "";
    char     pendingStartedBy[100] = "";
    char     startedBy[140] = "";
    Validity cap;
    struct RPEnt { int cam = 0; unsigned role = 255; bool scratch = false; const void* target = nullptr; uint32_t draws = 0, inst = 0, tw = 0, th = 0, tfmt = 0, srvIds[4] = {}, vpW = 0, vpH = 0; };
    struct RPTot { int cam = 0; unsigned role = 255; bool scratch = false; uint32_t targetId = 0, tw = 0, th = 0, tfmt = 0, frames = 0, draws = 0, minD = 0, maxD = 0, samplesOffscreen = 0; };
    RPEnt    rp[48];
    uint32_t rpN = 0;
    RPTot    rptot[16];
    uint32_t rptotN = 0;
    ID3D11Resource* ringTargetRef = nullptr;
    uint32_t ringTargetId = 0, ringTargetW = 0, ringTargetH = 0, ringTargetFmt = 0;
    uint32_t rpLines = 0;
    struct RingFill { uint32_t frame = 0, boxes = 0, id = 0; };
    RingFill ringFill[160];
    uint32_t ringFillN = 0;
    // v9
    TargetTracker tracker;
    struct TgtHist { uint32_t id = 0, w = 0, h = 0, fmt = 0, frame = 0; };
    TgtHist  tgtHist[8];
    uint32_t tgtHistN = 0, tgtChangeLines = 0;
    struct RingSnap { bool used = false, drained = true; uint32_t frame = 0, id = 0, w = 0, h = 0, fmt = 0, bpp = 0; };
    RingSnap snap;
    ID3D11Resource* snapStage = nullptr;
    uint32_t snapSkipped = 0;
    bool     snapWarned = false;
    struct CompEnt { const void* rt = nullptr; uint64_t vs = 0, ps = 0; uint32_t slot = 0, eye = 3, draws = 0, inst = 0, firstSeq = 0, lastSeq = 0, w = 0, h = 0, fmt = 0, vpW = 0, vpH = 0, tgt = 0; };
    struct CompTot { uint64_t vs = 0, ps = 0; uint32_t rtId = 0, slot = 0, eye = 3, w = 0, h = 0, fmt = 0, frames = 0, draws = 0, order[4] = {}; };
    CompEnt  comp[48];
    uint32_t compN = 0;
    CompTot  ctot[24];
    uint32_t ctotN = 0, compLines = 0;
    uint32_t drawSeq = 0, ringSeqFirst = 0, ringSeqLast = 0, ringSeqN = 0;
    bool     refusedEventNoted = false;
};

State g;

const char* const kDeclineText[kDeclineCount] = {
    "the previous frame did not have exactly two argument buffers",
    "no first-view dispatch of this shader in this frame",
    "both views use the same argument buffer",
    "the two dispatches use different offsets into it",
    "no buffer binding differs between the views, or the sizes differ",
};

void releaseEyeZero(EyeZero& z) {
    if (z.args) static_cast<IUnknown*>(z.args)->Release();
    for (void*& v : z.views) {
        if (v) static_cast<IUnknown*>(v)->Release();
        v = nullptr;
    }
    z = EyeZero();
}

void resetWindowTallies() {
    g.framesSeen = 0;
    g.lastFrameCounted = 0xFFFFFFFFu;
    g.targetDispatches = g.readbacks = g.dropped = g.lines = g.substituted = 0;
    for (auto& d : g.declined) d = 0;
    for (auto& d : g.declineNoted) d = false;
    g.engagedNoted = false;
    for (auto& r : g.range) for (auto& x : r) x = Range();
    g.share = Share();
    for (auto& o : g.other) o = OtherCs();
    g.noTargetNoted = false;
    g.watch.clear();
    g.listLines = g.writeLines = g.samplesRead = g.sampleFiles = 0;
    for (auto& w : g.writes) for (auto& x : w) x = 0;
    for (auto& a : g.haveLastList) for (auto& b : a) b = false;
    g.haveLastInfo[0] = g.haveLastInfo[1] = false;
    g.lastLoggedFrame = 0;
    g.lastSplit[0] = g.lastSplit[1] = -100.0;
    g.bandWatch = BandWatch();
    g.bringHead = g.bringCount = 0;
    g.windowEventFired = false;
    g.windowEvents = 0;
    g.afterLeft = 0;
    g.bandLines = g.bandFiles = 0;
    g.inputLines = g.fpRead = 0;
    for (auto& x : g.gridFrames) x = 0;
    for (auto& x : g.gridDiffer) x = 0;
    g.gdN = 0;
    for (auto& r : g.gtot) for (auto& t : r) t = State::GTot();
    g.gbufLines = 0;
    g.rtvN = 0;
    if (g.ringTargetRef) { g.ringTargetRef->Release(); g.ringTargetRef = nullptr; }
    g.ringTargetId = g.ringTargetW = g.ringTargetH = g.ringTargetFmt = 0;
    g.rpN = g.rptotN = g.rpLines = g.ringFillN = 0;
    for (auto& t : g.rptot) t = State::RPTot();
    g.cap = Validity();
    g.tracker.reset();
    g.tgtHistN = g.compN = g.ctotN = g.compLines = g.snapSkipped = 0;
    for (auto& t : g.ctot) t = State::CompTot();
    g.drawSeq = g.ringSeqFirst = g.ringSeqLast = g.ringSeqN = 0;
    g.snapWarned = false;
    g.refusedEventNoted = false;
    g.ledgerFrames = g.framesWithDiff = g.pairedFrames = g.eyediffLines = g.unpaired = g.ringLines = 0;
    g.ledN = g.ledOverflow = g.ledOverflowTotal = g.viewN = g.soloN = g.ringFamN = 0;
    g.shapeN = g.tgtN = 0;
    g.rank = Ranking();
    for (auto& x : g.ringFrames) x = 0;
    for (auto& x : g.ringDrawTotal) x = 0;
    for (auto& x : g.ringBoth) x = 0;
    for (auto& x : g.ringShared) x = 0;
}

bool pendingReadbacks() {
    for (const FrameBuf& f : g.fb) if (f.used && !f.drained) return true;
    for (const Sample& f : g.smp) if (f.used && !f.drained) return true;
    for (const State::Fp& f : g.fp) if (f.used && !f.drained) return true;
    if (g.snap.used && !g.snap.drained) return true;
    return false;
}

void refreshActive() {
    detail::g_fssTilesActive =
        g.rv.win.capturing || (g.lockstep && g.rv.win.open) || g.summaryDue || pendingReadbacks();
    detail::g_fssTilesWatch = g.rv.win.capturing && g.watch.n > 0;
    g.watchGbuf = false;
    for (uint32_t i = 0; i < g.watch.n; ++i) {
        if (g.watch.role[i] >= Role::Gbuf8 && g.watch.role[i] <= Role::Ring3) g.watchGbuf = true;
    }
    detail::g_fssTilesDraws = g.rv.win.capturing;   // v8: the draws made inside the ring pass are recorded for the whole capture
}

bool ensureStaging(ID3D11DeviceContext* ctx) {
    if (g.staging[0]) return true;
    if (g.stagingFailed) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) { g.stagingFailed = true; return false; }
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 12 * kMaxPerFrame;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = true;
    for (auto& b : g.staging) {
        if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) { ok = false; break; }
    }
    dev->Release();
    if (!ok) {
        for (auto& b : g.staging) { if (b) { b->Release(); b = nullptr; } }
        g.stagingFailed = true;
        Log::get().note("fss tiles: the read-back staging buffers could not be created; the instrument logs bindings only "
                        "(groups=(unread)).");
        return false;
    }
    return true;
}

void noteOther(uint64_t h) {
    for (auto& o : g.other) {
        if (o.hash == h) { ++o.n; return; }
        if (o.hash == 0) { o.hash = h; o.n = 1; return; }
    }
}

// ---- the read-back --------------------------------------------------------------------------------------------------
void processFrame(FrameBuf& f) {
    Rec* first[2][2] = {};   // [cs][eye 0/1]
    for (uint32_t i = 0; i < f.n; ++i) {
        Rec& r = f.rec[i];
        const int c = csIndex(r.cs);
        if (c < 0) continue;
        if (r.haveGroups) g.range[r.eye > 2 ? 2 : r.eye][c].add(r.groups[0], r.groups[1], r.groups[2]);
        if (r.eye < 2 && !first[c][r.eye]) first[c][r.eye] = &r;
        if (f.logFrame && g.lines < kMaxLinesPerWindow) {
            char line[512];
            formatRec(line, sizeof(line), r);
            Log::get().note("%s", line);
            ++g.lines;
            if (g.lines == kMaxLinesPerWindow) {
                Log::get().note("fss tiles: line cap (%u) reached for this window; the closing summary still counts "
                                "every dispatch.", kMaxLinesPerWindow);
            }
        }
    }
    for (int c = 0; c < 2; ++c) {
        if (first[c][0] && first[c][1]) accumulateShare(g.share, *first[c][0], *first[c][1]);
    }
    Sample& sm = g.smp[f.frame % kStagingRing];
    if (sm.used && sm.frame == f.frame) {
        for (int e = 0; e < 2; ++e) {
            for (int c = 0; c < 2; ++c) {
                if (first[c][e] && first[c][e]->haveGroups) {
                    sm.groups[e][c] = first[c][e]->groups[0];
                    sm.haveGroups[e][c] = true;
                }
            }
        }
    }
    ++g.readbacks;
}

void drain(ID3D11DeviceContext* ctx) {
    if (!g.staging[0]) return;
    for (;;) {
        FrameBuf* pick = nullptr;
        for (uint32_t k = 0; k < kStagingRing; ++k) {
            FrameBuf& f = g.fb[k];
            if (!f.used || f.drained || g.frame < f.frame + kReadbackDelay) continue;
            if (!pick || f.frame < pick->frame) pick = &f;
        }
        if (!pick) return;
        const uint32_t k = static_cast<uint32_t>(pick - g.fb);
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(g.staging[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (FAILED(hr)) {
            // Still in flight (the GPU is behind); try again at the next call. Never wait. A buffer that stays
            // unreadable for a long time is dropped rather than held, so the ring never blocks a new frame.
            if (g.frame > pick->frame + 40) {
                g.dropped += pick->n;
                pick->drained = true;
                continue;
            }
            return;
        }
        const uint8_t* base = static_cast<const uint8_t*>(m.pData);
        for (uint32_t i = 0; i < pick->n; ++i) {
            uint32_t u[3];
            std::memcpy(u, base + 12 * i, 12);
            pick->rec[i].groups[0] = u[0];
            pick->rec[i].groups[1] = u[1];
            pick->rec[i].groups[2] = u[2];
            pick->rec[i].haveGroups = true;
        }
        ctx->Unmap(g.staging[k], 0);
        pick->drained = true;
        processFrame(*pick);
    }
}

void drainAll(ID3D11DeviceContext* ctx);
void ledgerReset();

// ---- v2: the lists and the tile-info buffer ---------------------------------------------------------------------------
const char* const kRegionFile[3] = {"listL", "listS", "info"};

void writeSampleFiles(const Sample& sm, const D3D11_MAPPED_SUBRESOURCE* m) {
    if (!g.dirMade) {
        g.dirMade = true;
        CreateDirectoryW((Log::get().dir() + L"\\fsstiles").c_str(), nullptr);
    }
    for (uint32_t r = 0; r < kRegions; ++r) {
        if (!sm.have[r]) continue;
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\fsstiles\\f%u_e%u_%S.bin", Log::get().dir().c_str(), sm.frame, r / 3,
                     kRegionFile[r % 3]);
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD wrote = 0;
        WriteFile(h, m[r].pData, sm.bytes[r], &wrote, nullptr);
        CloseHandle(h);
    }
}

// ---- v3: the band event -----------------------------------------------------------------------------------------------
void writeBandRaw(const State::BandFrame& f) {
    if (!g.dirMade) {
        g.dirMade = true;
        CreateDirectoryW((Log::get().dir() + L"\\fsstiles").c_str(), nullptr);
    }
    for (uint32_t idx = 0; idx < 4; ++idx) {
        if (!f.rawBytes[idx]) continue;
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\fsstiles\\f%u_e%u_%S.bin", Log::get().dir().c_str(), f.rec.frame, idx / 2,
                     kRegionFile[idx % 2]);
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD wrote = 0;
        WriteFile(h, f.raw[idx], f.rawBytes[idx], &wrote, nullptr);
        CloseHandle(h);
    }
    ++g.bandFiles;
}

void logBandFrame(const State::BandFrame& f) {
    if (g.bandLines >= kMaxBandLines) return;
    char line[600];
    formatBand(line, sizeof(line), f.rec);
    Log::get().note("%s", line);
    ++g.bandLines;
}

// The glue's half of arming the reveal capture (the machine's half is RevealMachine::arm): the band-event bookkeeping and a fresh ledger. The arrival and the all-small
// band event both come through here.
void armGlue(uint32_t eventFrame) {
    g.windowEventFired = true;
    ++g.windowEvents;
    g.afterLeft = kBandAfter;
    g.eventFrame = eventFrame;
    ledgerReset();
}

// The band event's start of the reveal capture (the secondary trigger). Returns how many eye runs were planned.
uint32_t armReveal(uint32_t eventFrame) {
    const uint32_t plan = g.rv.arm(g.frame);
    armGlue(eventFrame);
    return plan;
}

void processBand(const Sample& sm, const D3D11_MAPPED_SUBRESOURCE* m, const bool (*hl)[2]) {
    if (!hl[0][0] || !hl[1][0] || !hl[0][1] || !hl[1][1]) return;
    State::BandFrame& slotRef = g.bring[g.bringHead % (kBandBefore + 1)];
    State::BandFrame& f = slotRef;
    f.rec = BandRec();
    f.rec.frame = sm.frame;
    f.rec.ranEye0 = sm.ranEye0;
    for (uint32_t e = 0; e < 2; ++e) {
        const Band bl = bandOf(m[regionOf(e, 0)].pData, sm.bytes[regionOf(e, 0)]);
        const Band bs = bandOf(m[regionOf(e, 1)].pData, sm.bytes[regionOf(e, 1)]);
        f.rec.large[e] = bl.valid;
        f.rec.smallN[e] = bs.valid;
        f.rec.minY[e] = bl.minY;
        for (uint32_t role = 0; role < 2; ++role) {
            const uint32_t r = regionOf(e, role);
            const uint32_t idx = e * 2 + role;
            f.rawBytes[idx] = sm.bytes[r] <= kBandRaw ? sm.bytes[r] : 0;
            if (f.rawBytes[idx]) std::memcpy(f.raw[idx], m[r].pData, f.rawBytes[idx]);
        }
    }
    ++g.bringHead;
    if (g.bringCount < kBandBefore + 1) ++g.bringCount;

    const BandWatch::Reason why = g.bandWatch.feed(f.rec.large[0], f.rec.large[1]);
    if (why == BandWatch::AllSmall && (g.rv.win.open || g.rv.win.capturing) && !g.windowEventFired && g.rv.runs.sessionPlanned < kMaxAutoRuns && !chromeHeld(g.chromeAgo)) {
        if (!g.refusedEventNoted) {
            g.refusedEventNoted = true;
            Log::get().note("fss tiles: band event at f=%u refused: the scanner chrome is not held on this frame (last matched %s). An all-small frame without the scanner "
                            "is a transition (flight e05b6be6: the exit back to the cockpit), not the zoom; no reveal capture, no ledger and no eye run for it. Said once a window.",
                            sm.frame, g.chromeAgo == 0xFFFFFFFFu ? "never" : "recently, more than 5 frames ago");
        }
    } else if (why == BandWatch::AllSmall && (g.rv.win.open || g.rv.win.capturing) && !g.windowEventFired && g.rv.runs.sessionPlanned < kMaxAutoRuns) {
        const uint32_t plan = armReveal(sm.frame);
        Log::get().note("fss tiles: band event at f=%u (a large list went to zero: every cell is now the small shader's): eye0 L=%u (row %.2f), "
                        "eye1 L=%u (row %.2f). The 10 frames before, this one and the 60 after are logged as `band` lines and their raw lists "
                        "written to edvr_logs\\fsstiles. Capture continues until f=%u (%u frames); %u automatic eye run(s) at f=%u%s (planned "
                        "%u of %u this session); input and gbuf lines every %u frames.",
                        sm.frame, f.rec.large[0], rowOfLarge(f.rec.large[0]), f.rec.large[1], rowOfLarge(f.rec.large[1]), g.rv.revealUntil,
                        kRevealFrames, plan, plan ? g.rv.runs.due[0] : 0u, plan > 1 ? " and the others later" : "", g.rv.runs.sessionPlanned,
                        kMaxAutoRuns, kFingerEvery);
        // the frames before, oldest first (the ring holds this frame last)
        const uint32_t n = g.bringCount;
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t at = (g.bringHead - n + k) % (kBandBefore + 1);
            logBandFrame(g.bring[at]);
            writeBandRaw(g.bring[at]);
        }
        return;
    }
    if (g.afterLeft > 0) {
        --g.afterLeft;
        logBandFrame(f);
        writeBandRaw(f);
    }
}

void processSample(Sample& sm, const D3D11_MAPPED_SUBRESOURCE* m) {
    ListSummary ls[2][2];
    bool hl[2][2] = {};
    for (uint32_t e = 0; e < 2; ++e) {
        for (uint32_t role = 0; role < 2; ++role) {
            const uint32_t r = regionOf(e, role);
            if (!sm.have[r]) continue;
            ls[e][role] = summarizeList(m[r].pData, sm.bytes[r]);
            hl[e][role] = true;
        }
    }
    ++g.samplesRead;
    // Which frames get lines: the first few after the window opens, one in kDetailEvery, and any whose split moved by ten
    // points since the last line.
    double sp[2] = {};
    bool haveSp = true;
    for (uint32_t e = 0; e < 2; ++e) {
        if (sm.haveGroups[e][0] && sm.haveGroups[e][1]) sp[e] = splitPercent(sm.groups[e][0], sm.groups[e][1]);
        else haveSp = false;
    }
    const bool moved = haveSp && (sp[0] - g.lastSplit[0] > 10.0 || g.lastSplit[0] - sp[0] > 10.0 || sp[1] - g.lastSplit[1] > 10.0 ||
                                  g.lastSplit[1] - sp[1] > 10.0);
    const uint32_t d = sm.frame - g.rv.win.startFrame;
    const bool wanted = d < kDetailFrames || d % kDetailEvery == 0 || moved;
    if (wanted && g.listLines + 3 <= kMaxListLines) {
        if (haveSp) { g.lastSplit[0] = sp[0]; g.lastSplit[1] = sp[1]; }
        char a[900], b[900], c[700], t0[260], t1[260], t2[260], t3[260];
        t0[0] = t1[0] = t2[0] = t3[0] = 0;
        if (hl[0][0]) formatList(t0, sizeof(t0), "L", ls[0][0]);
        if (hl[0][1]) formatList(t1, sizeof(t1), "S", ls[0][1]);
        if (hl[1][0]) formatList(t2, sizeof(t2), "L", ls[1][0]);
        if (hl[1][1]) formatList(t3, sizeof(t3), "S", ls[1][1]);
        char split[200];
        if (haveSp) {
            std::snprintf(split, sizeof(split), "split eye0 small=%u/%u (%.1f%%) eye1 small=%u/%u (%.1f%%)", sm.groups[0][1],
                          sm.groups[0][0] + sm.groups[0][1], sp[0], sm.groups[1][1], sm.groups[1][0] + sm.groups[1][1], sp[1]);
        } else {
            std::snprintf(split, sizeof(split), "split (groups not read back for this frame)");
        }
        std::snprintf(a, sizeof(a), "fss tiles: lists f=%u %s | eye0 %s | eye0 %s", sm.frame, split, t0[0] ? t0 : "L -",
                      t1[0] ? t1 : "S -");
        std::snprintf(b, sizeof(b), "fss tiles: lists f=%u eye1 %s | eye1 %s", sm.frame, t2[0] ? t2 : "L -", t3[0] ? t3 : "S -");
        char i0[160] = "-", i1[160] = "-";
        uint64_t h0 = 0, h1 = 0;
        const uint32_t r0 = regionOf(0, 2), r1 = regionOf(1, 2);
        if (sm.have[r0]) {
            formatInfo(i0, sizeof(i0), "eye0", m[r0].pData, sm.bytes[r0]);
            h0 = fnv64(m[r0].pData, sm.bytes[r0]);
        }
        if (sm.have[r1]) {
            formatInfo(i1, sizeof(i1), "eye1", m[r1].pData, sm.bytes[r1]);
            h1 = fnv64(m[r1].pData, sm.bytes[r1]);
        }
        std::snprintf(c, sizeof(c), "fss tiles: info f=%u tile-info buffer %s | %s | identical=%s", sm.frame, i0, i1,
                      (sm.have[r0] && sm.have[r1]) ? (h0 == h1 ? "yes" : "no") : "n/a");
        Log::get().note("%s", a);
        Log::get().note("%s", b);
        Log::get().note("%s", c);
        g.listLines += 3;
    }
    processBand(sm, m, hl);
    if (sm.files && g.sampleFiles < kMaxDumpFrames) {
        writeSampleFiles(sm, m);
        ++g.sampleFiles;
    }
}

void drainSamples(ID3D11DeviceContext* ctx) {
    for (;;) {
        Sample* pick = nullptr;
        for (Sample& f : g.smp) {
            if (!f.used || f.drained || g.frame < f.frame + kReadbackDelay) continue;
            if (!pick || f.frame < pick->frame) pick = &f;
        }
        if (!pick) return;
        const uint32_t k = static_cast<uint32_t>(pick - g.smp);
        D3D11_MAPPED_SUBRESOURCE m[kRegions] = {};
        bool mapped[kRegions] = {};
        bool ok = true;
        for (uint32_t r = 0; r < kRegions && ok; ++r) {
            if (!pick->have[r]) continue;
            ok = SUCCEEDED(ctx->Map(g.pool[k][r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[r]));
            mapped[r] = ok;
        }
        if (ok) processSample(*pick, m);
        for (uint32_t r = 0; r < kRegions; ++r) {
            if (mapped[r]) ctx->Unmap(g.pool[k][r], 0);
        }
        if (ok) { pick->drained = true; continue; }
        if (g.frame > pick->frame + 40) { pick->drained = true; ++g.dropped; continue; }
        return;
    }
}

// The copies, queued before the dispatch the way the argument copy is. Eye 0 and 1 only.
void queueSample(ID3D11DeviceContext* ctx, const Rec& r, const Slot* slot) {
    if (r.eye > 1) return;
    const int ci = csIndex(r.cs);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const uint32_t ring = g.frame % kStagingRing;
    Sample& sm = g.smp[ring];
    if (!sm.used || sm.frame != g.frame) {
        if (sm.used && !sm.drained) ++g.dropped;
        sm = Sample();
        sm.frame = g.frame;
        sm.used = true;
        sm.drained = false;
        sm.files = g.dumpFramesLeft > 0 || g.frame - g.rv.win.startFrame < 3;
        if (g.dumpFramesLeft > 0) --g.dumpFramesLeft;
    }
    auto take = [&](uint32_t region, uint32_t slotIdx) {
        if (sm.have[region] || !slot[slotIdx].isBuffer || slot[slotIdx].bytes == 0 || slot[slotIdx].bytes > (1u << 20)) return;
        ID3D11Buffer* src = static_cast<ID3D11Buffer*>(const_cast<void*>(slot[slotIdx].res));
        if (!ensureStagingLike(dev, src, &g.pool[ring][region])) {
            if (!g.poolFailedNoted) {
                g.poolFailedNoted = true;
                Log::get().note("fss tiles: a staging twin for a list or tile-info buffer could not be created; those samples are skipped.");
            }
            return;
        }
        ctx->CopyResource(g.pool[ring][region], src);
        sm.have[region] = true;
        sm.bytes[region] = slot[slotIdx].bytes;
    };
    if (r.substituted && r.eye == 1) sm.ranEye0 = true;
    if (ci == 0) {
        take(regionOf(r.eye, 0), 2);
        take(regionOf(r.eye, 2), 1);
    } else if (ci == 1) {
        take(regionOf(r.eye, 1), 2);
    }
    dev->Release();
}

void registerWatch(const Rec& r, ID3D11Buffer* args, const Slot* slot, const void* const* uavRes) {
    g.watch.add(args, Role::Args, r.eye);
    for (int i = 0; i < 2; ++i) g.watch.add(uavRes[i], Role::Out, r.eye);
    if (slot[0].isBuffer) g.watch.add(slot[0].res, Role::Big, r.eye);
    if (slot[1].isBuffer) g.watch.add(slot[1].res, Role::Info, r.eye);
    if (slot[2].isBuffer) g.watch.add(slot[2].res, Role::List, r.eye);
    for (uint32_t k = 0; k < 4 && r.eye < 2; ++k) {
        if (slot[8 + k].res && !slot[8 + k].isBuffer) g.watch.add(slot[8 + k].res, static_cast<Role>(static_cast<uint32_t>(Role::Gbuf8) + k), r.eye);
    }
}

// ---- v4: the inputs ---------------------------------------------------------------------------------------------------
bool ensureFpBuffer(ID3D11Device* dev, ID3D11Resource** st) {
    if (*st) return true;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = kBufRegions * kBufRegionBytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* b = nullptr;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) return false;
    *st = b;
    return true;
}

bool ensureFpTexture(ID3D11Device* dev, DXGI_FORMAT fmt, ID3D11Resource** st) {
    if (*st) {
        D3D11_TEXTURE2D_DESC have{};
        static_cast<ID3D11Texture2D*>(*st)->GetDesc(&have);
        if (have.Format == fmt) return true;
        (*st)->Release();
        *st = nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = kTileGrid * kTilePx;
    d.MipLevels = d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return false;
    *st = t;
    return true;
}

// Slots 0, 3 and 4 of the large dispatch, once a kFingerEvery frames, eyes 0 and 1: identity and size always, and a coarse sample
// of the contents where it is safe (a colour texture, or the big buffer): 8 x 8 boxes of 16 x 16 pixels spread over a texture,
// 16 regions of 256 bytes spread over the buffer. Depth-stencil, multisampled and unknown formats are named, not sampled.
void queueFingerprint(ID3D11DeviceContext* ctx, const Rec& r, const Slot* slot) {
    if (r.eye > 1 || csIndex(r.cs) != 0 || g.frame % kFingerEvery != 0) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const uint32_t ring = g.frame % kStagingRing;
    State::Fp& fp = g.fp[ring];
    if (!fp.used || fp.frame != g.frame) {
        if (fp.used && !fp.drained) ++g.dropped;
        fp = State::Fp();
        fp.frame = g.frame;
        fp.used = true;
        fp.drained = false;
    }
    for (uint32_t k = 0; k < kFpItems; ++k) {
        const uint32_t s = kFpSlots[k];
        if (!slot[s].res) continue;
        ID3D11Resource* res = static_cast<ID3D11Resource*>(const_cast<void*>(slot[s].res));
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        State::FpItem& it = fp.it[r.eye * kFpItems + k];
        it.id = g.ids.id(res);
        ID3D11Resource** st = &g.fpPool[ring][r.eye * kFpItems + k];
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bd{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
            it.bytes = bd.ByteWidth;
            if (bd.ByteWidth < kBufRegions * kBufRegionBytes * 2 || !ensureFpBuffer(dev, st)) continue;
            for (uint32_t i = 0; i < kBufRegions; ++i) {
                const uint64_t off = (static_cast<uint64_t>(bd.ByteWidth) - kBufRegionBytes) * i / (kBufRegions - 1);
                D3D11_BOX box{};
                box.left = static_cast<UINT>(off & ~3ull);
                box.right = box.left + kBufRegionBytes;
                box.bottom = 1;
                box.back = 1;
                ctx->CopySubresourceRegion(*st, 0, i * kBufRegionBytes, 0, 0, res, 0, &box);
            }
            it.have = true;
        } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td{};
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
            it.tex = true;
            it.w = td.Width;
            it.h = td.Height;
            it.fmt = td.Format;
            it.bpp = bytesPerPixel(td.Format);
            if ((td.BindFlags & D3D11_BIND_DEPTH_STENCIL) || td.SampleDesc.Count > 1 || td.Width < kTileGrid * kTilePx ||
                td.Height < kTileGrid * kTilePx || !it.bpp || !ensureFpTexture(dev, td.Format, st)) {
                continue;
            }
            for (uint32_t gy = 0; gy < kTileGrid; ++gy) {
                for (uint32_t gx = 0; gx < kTileGrid; ++gx) {
                    D3D11_BOX box{};
                    box.left = (td.Width - kTilePx) * gx / (kTileGrid - 1);
                    box.top = (td.Height - kTilePx) * gy / (kTileGrid - 1);
                    box.right = box.left + kTilePx;
                    box.bottom = box.top + kTilePx;
                    box.back = 1;
                    ctx->CopySubresourceRegion(*st, 0, gx * kTilePx, gy * kTilePx, 0, res, 0, &box);
                }
            }
            it.have = true;
        }
    }
    if (slot[6].res) {
        ID3D11Resource* res = static_cast<ID3D11Resource*>(const_cast<void*>(slot[6].res));
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td{};
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
            fp.cube[r.eye][0] = g.ids.id(res);
            fp.cube[r.eye][1] = td.Width;
            fp.cube[r.eye][2] = td.Height;
            fp.cube[r.eye][3] = td.ArraySize;
            fp.cube[r.eye][4] = td.Format;
        }
    }
    dev->Release();
}

void formatInputItem(char* out, size_t cap, const char* tag, const State::FpItem& it, const char* grid) {
    if (!it.id) {
        std::snprintf(out, cap, "%s -", tag);
    } else if (!it.tex) {
        std::snprintf(out, cap, "%s B%u %u bytes nz16=%s", tag, it.id, it.bytes, it.have ? grid : "(not sampled)");
    } else {
        std::snprintf(out, cap, "%s T%u %ux%u fmt%u nz8x8=%s", tag, it.id, it.w, it.h, it.fmt, it.have ? grid : "(not sampled: depth, multisampled or unknown format)");
    }
}

void processFingerprint(State::Fp& fp, const D3D11_MAPPED_SUBRESOURCE* m) {
    char grids[2][kFpItems][96] = {};
    for (uint32_t e = 0; e < 2; ++e) {
        for (uint32_t k = 0; k < kFpItems; ++k) {
            const uint32_t at = e * kFpItems + k;
            const State::FpItem& it = fp.it[at];
            if (!it.have) continue;
            const uint8_t* p = static_cast<const uint8_t*>(m[at].pData);
            if (!it.tex) {
                char tmp[kBufRegions + 1];
                for (uint32_t i = 0; i < kBufRegions; ++i) tmp[i] = nzChar(countNonzero(p + i * kBufRegionBytes, kBufRegionBytes), kBufRegionBytes);
                tmp[kBufRegions] = 0;
                std::snprintf(grids[e][k], sizeof(grids[e][k]), "%s", tmp);
            } else {
                const uint32_t pitch = m[at].RowPitch;
                formatGrid(grids[e][k], sizeof(grids[e][k]), [&](uint32_t x, uint32_t y) {
                    uint32_t nz = 0;
                    for (uint32_t row = 0; row < kTilePx; ++row) {
                        nz += countNonzero(p + (y * kTilePx + row) * pitch + x * kTilePx * it.bpp, kTilePx * it.bpp);
                    }
                    return nzChar(nz, kTilePx * kTilePx * it.bpp);
                });
            }
        }
    }
    ++g.fpRead;
    static const char* const kTag[kFpItems] = {"slot0", "depth(slot3)", "stencil(slot4)", "t8", "t9", "t10", "t11"};
    for (uint32_t k = 0; k < kFpItems; ++k) {
        if (fp.it[k].have && fp.it[kFpItems + k].have) {
            ++g.gridFrames[k];
            if (std::strcmp(grids[0][k], grids[1][k]) != 0) ++g.gridDiffer[k];
        }
    }
    if (g.inputLines + 4 > kMaxInputLines) return;
    for (uint32_t e = 0; e < 2; ++e) {
        char part[kFpItems][280], lineA[900], lineB[1300];
        for (uint32_t k = 0; k < kFpItems; ++k) formatInputItem(part[k], sizeof(part[k]), kTag[k], fp.it[e * kFpItems + k], grids[e][k]);
        std::snprintf(lineA, sizeof(lineA), "fss tiles: input f=%u eye%u | %s | %s | %s | cube T%u %ux%ux%u fmt%u", fp.frame, e, part[0], part[1], part[2],
                      fp.cube[e][0], fp.cube[e][1], fp.cube[e][2], fp.cube[e][3], fp.cube[e][4]);
        std::snprintf(lineB, sizeof(lineB), "fss tiles: input f=%u eye%u G-buffer | %s | %s | %s | %s", fp.frame, e, part[3], part[4], part[5], part[6]);
        Log::get().note("%s", lineA);
        Log::get().note("%s", lineB);
        g.inputLines += 2;
    }
}

void drainFingerprints(ID3D11DeviceContext* ctx) {
    for (;;) {
        State::Fp* pick = nullptr;
        for (State::Fp& f : g.fp) {
            if (!f.used || f.drained || g.frame < f.frame + kReadbackDelay) continue;
            if (!pick || f.frame < pick->frame) pick = &f;
        }
        if (!pick) return;
        const uint32_t ring = static_cast<uint32_t>(pick - g.fp);
        D3D11_MAPPED_SUBRESOURCE m[2 * kFpItems] = {};
        bool mapped[2 * kFpItems] = {};
        bool ok = true;
        for (uint32_t i = 0; i < 2 * kFpItems && ok; ++i) {
            if (!pick->it[i].have) continue;
            ok = SUCCEEDED(ctx->Map(g.fpPool[ring][i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[i]));
            mapped[i] = ok;
        }
        if (ok) processFingerprint(*pick, m);
        for (uint32_t i = 0; i < 2 * kFpItems; ++i) {
            if (mapped[i]) ctx->Unmap(g.fpPool[ring][i], 0);
        }
        if (ok) { pick->drained = true; continue; }
        if (g.frame > pick->frame + 40) { pick->drained = true; ++g.dropped; continue; }
        return;
    }
}

// ---- v4: the draws into slots 3 and 4 -----------------------------------------------------------------------------------
const void* rtvResource(void* rtv, uint32_t* shapeOut);
// ---- v6: the ledger of draws into eye-sized and ring targets, diffed between the eyes ---------------------------------------------------
uint32_t shapeOf(uint32_t w, uint32_t h, uint32_t fmt) {
    for (uint32_t i = 0; i < g.shapeN; ++i) {
        if (g.shapes[i].w == w && g.shapes[i].h == h && g.shapes[i].fmt == fmt) return i + 1;
    }
    if (g.shapeN < 64) {
        g.shapes[g.shapeN] = {w, h, fmt};
        return ++g.shapeN;
    }
    return 0;
}

// The order in which a render target first appeared among the textures of its shape: 0 is the first (eye 0 where two eyes share a shape), 1 the second.
uint32_t targetOrder(const void* res, uint32_t shape) {
    uint32_t sameShape = 0;
    for (uint32_t i = 0; i < g.tgtN; ++i) {
        if (g.tgts[i].res == res) return g.tgts[i].order;
        if (g.tgts[i].shape == shape) ++sameShape;
    }
    if (g.tgtN < 64) {
        g.tgts[g.tgtN] = {res, shape, sameShape};
        ++g.tgtN;
    }
    return sameShape;
}

uint32_t shapeTargets(uint32_t shape) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.tgtN; ++i) n += g.tgts[i].shape == shape;
    return n;
}

const State::ViewRec* viewInfo(void* view) {
    for (uint32_t i = 0; i < g.viewN; ++i) {
        if (g.views2[i].view == view) return &g.views2[i];
    }
    ResourceInfo ri;
    State::ViewRec r;
    r.view = view;
    if (bindingResolveProbe(view, &ri)) {
        r.res = ri.resource;
        r.shape = ri.isTexture2D ? shapeOf(ri.a, ri.b, ri.fmt) : shapeOf(ri.a, 0, 0xFFFFu);
    }
    if (g.viewN < 128) {
        g.views2[g.viewN] = r;
        return &g.views2[g.viewN++];
    }
    return nullptr;
}

void ledgerRecord(ID3D11DeviceContext* ctx, const void* tgt, uint32_t rtShape, char kind, uint32_t count, uint32_t instances) {
    DrawKey key;
    key.kind = kind;
    key.count = count;
    key.rtShape = rtShape;
    key.vs = bindingShaderHash(BindSlot::Vs);
    key.ps = bindingShaderHash(BindSlot::Ps);
    uint32_t sig = 2166136261u;
    uint32_t srvIds[4] = {};
    const void* srvRes[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        void* v = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i));
        const State::ViewRec* vr = v ? viewInfo(v) : nullptr;
        const uint32_t sh = vr ? vr->shape : 0;
        srvRes[i] = vr ? vr->res : nullptr;
        srvIds[i] = srvRes[i] ? g.ids.id(srvRes[i]) : 0;
        sig = (sig ^ sh) * 16777619u;
    }
    State::LEnt* e = nullptr;
    for (uint32_t i = 0; i < g.ledN; ++i) {
        if (g.led[i].target == tgt && g.led[i].s.k == key) { e = &g.led[i]; break; }
    }
    if (!e) {
        if (g.ledN >= kLedgerCap) { ++g.ledOverflow; return; }
        e = &g.led[g.ledN++];
        e->target = tgt;
        e->s.k = key;
        e->s.srvSig = sig;
        UINT nv = 1;
        D3D11_VIEWPORT vp{};
        ctx->RSGetViewports(&nv, &vp);
        e->s.vpW = static_cast<uint32_t>(vp.Width);
        e->s.vpH = static_cast<uint32_t>(vp.Height);
        e->ring = isRingDraw(kind, count, key.vs, key.ps);
        for (uint32_t i = 0; i < 4; ++i) e->srvIds[i] = srvIds[i];
        if (e->ring) {
            // the ring quad's own inputs t0-t3: watched, so a clear, copy or draw into them is counted, in the eye the draw's target belongs to
            const uint32_t ord = targetOrder(tgt, rtShape);
            const uint32_t eye = shapeTargets(rtShape) == 2 ? ord : 3u;
            for (uint32_t i = 0; i < 4; ++i) {
                if (!srvRes[i]) continue;
                const int at = g.watch.find(srvRes[i]);
                if (at >= 0 && g.watch.role[at] >= Role::Ring0 && g.watch.eye[at] == 3 && eye != 3) g.watch.eye[at] = static_cast<uint8_t>(eye);
                else if (at < 0) g.watch.add(srvRes[i], static_cast<Role>(static_cast<uint32_t>(Role::Ring0) + i), eye);
            }
        }
    }
    ++e->s.n;
    e->s.inst += instances;
}

void ledgerReset() {
    g.ledN = 0;
    g.ledOverflow = 0;
    g.viewN = 0;
}

void flushLedger(uint32_t frame) {
    if (!g.rv.ledgerOn && g.ledN == 0) return;
    ++g.ledgerFrames;
    DrawStat a[256], b[256];
    bool anyDiff = false, pairedThisFrame = false;
    uint32_t ringDraws[3] = {}, ringIds[3][4] = {};
    uint64_t ringVs[3] = {}, ringPs[3] = {};
    for (uint32_t sh = 1; sh <= g.shapeN; ++sh) {
        const uint32_t nt = shapeTargets(sh);
        const void* t0 = nullptr;
        const void* t1 = nullptr;
        for (uint32_t i = 0; i < g.tgtN; ++i) {
            if (g.tgts[i].shape != sh) continue;
            if (g.tgts[i].order == 0) t0 = g.tgts[i].res;
            if (g.tgts[i].order == 1) t1 = g.tgts[i].res;
        }
        uint32_t na = 0, nb = 0;
        for (uint32_t i = 0; i < g.ledN; ++i) {
            const State::LEnt& e = g.led[i];
            if (e.s.k.rtShape != sh) continue;
            if (nt != 2) {
                g.unpaired += e.s.n;
                State::Solo* so = nullptr;
                for (uint32_t k = 0; k < g.soloN; ++k) if (g.solo[k].k == e.s.k) { so = &g.solo[k]; break; }
                if (!so && g.soloN < 32) { so = &g.solo[g.soloN++]; so->k = e.s.k; }
                if (so) { so->draws += e.s.n; ++so->frames; so->ring = so->ring || e.ring; }
            } else if (e.target == t0 && na < 256) {
                a[na++] = e.s;
            } else if (e.target == t1 && nb < 256) {
                b[nb++] = e.s;
            }
            if (e.ring) {
                const uint32_t eye = nt == 2 ? (e.target == t0 ? 0u : 1u) : 2u;
                ringDraws[eye] += e.s.n;
                if (!ringVs[eye]) { ringVs[eye] = e.s.k.vs; ringPs[eye] = e.s.k.ps; for (uint32_t k = 0; k < 4; ++k) ringIds[eye][k] = e.srvIds[k]; }
                bool known = false;
                for (uint32_t k = 0; k < g.ringFamN; ++k) if (g.ringFam[k].vs == e.s.k.vs && g.ringFam[k].ps == e.s.k.ps) { g.ringFam[k].draws += e.s.n; known = true; break; }
                if (!known && g.ringFamN < 8) { g.ringFam[g.ringFamN++] = {e.s.k.vs, e.s.k.ps, e.s.n}; }
            }
        }
        if (nt != 2 || na + nb == 0) continue;
        pairedThisFrame = true;
        DiffItem items[kDiffCap];
        const uint32_t n = diffEyes(a, na, b, nb, items, kDiffCap);
        if (!n) continue;
        anyDiff = true;
        for (uint32_t i = 0; i < n; ++i) g.rank.add(items[i]);
        if (g.eyediffLines < kMaxEyediffLines) {
            char pairText[80];
            std::snprintf(pairText, sizeof(pairText), "pair=%ux%u fmt%u", g.shapes[sh - 1].w, g.shapes[sh - 1].h, g.shapes[sh - 1].fmt);
            char line[1500];
            formatDiff(line, sizeof(line), frame, pairText, items, n);
            Log::get().note("%s", line);
            ++g.eyediffLines;
        }
    }
    if (pairedThisFrame) ++g.pairedFrames;
    if (anyDiff) ++g.framesWithDiff;
    // the ring family, per eye
    const bool ringSeen = ringDraws[0] || ringDraws[1] || ringDraws[2];
    if (ringSeen) {
        for (uint32_t eye = 0; eye < 3; ++eye) {
            if (ringDraws[eye]) { ++g.ringFrames[eye]; g.ringDrawTotal[eye] += ringDraws[eye]; }
        }
        if (ringDraws[0] && ringDraws[1]) {
            for (uint32_t k = 0; k < 4; ++k) {
                if (ringIds[0][k] && ringIds[1][k]) {
                    ++g.ringBoth[k];
                    if (ringIds[0][k] == ringIds[1][k]) ++g.ringShared[k];
                }
            }
        }
        if (frame % kFingerEvery == 0 && g.ringLines < kMaxRingLines) {
            char part[3][260];
            for (uint32_t eye = 0; eye < 3; ++eye) {
                if (!ringDraws[eye]) { std::snprintf(part[eye], sizeof(part[eye]), "eye%u none", eye); continue; }
                std::snprintf(part[eye], sizeof(part[eye]), "eye%u draws=%u (vs %08llX ps %08llX) t0-t3 = T%u,T%u,T%u,T%u", eye, ringDraws[eye],
                              static_cast<unsigned long long>(ringVs[eye] >> 32), static_cast<unsigned long long>(ringPs[eye] >> 32), ringIds[eye][0], ringIds[eye][1],
                              ringIds[eye][2], ringIds[eye][3]);
            }
            Log::get().note("fss tiles: ring quad f=%u %s | %s | unpaired-target draws: %s", frame, part[0], part[1], part[2]);
            ++g.ringLines;
        }
    }
    g.ledOverflowTotal += g.ledOverflow;
    ledgerReset();
}

const void* rtvResource(void* rtv, uint32_t* shapeOut) {
    for (uint32_t i = 0; i < g.rtvN; ++i) {
        if (g.rtvKey[i] == rtv) { *shapeOut = g.rtvShape[i]; return g.rtvRes[i]; }
    }
    ResourceInfo ri;
    const void* res = nullptr;
    uint32_t shape = 0;
    if (bindingResolveProbe(rtv, &ri)) {
        res = ri.resource;
        if (ri.isTexture2D && ri.a >= kMinTargetWidth) shape = shapeOf(ri.a, ri.b, ri.fmt);
    }
    if (g.rtvN < 48) {
        g.rtvKey[g.rtvN] = rtv;
        g.rtvRes[g.rtvN] = res;
        g.rtvShape[g.rtvN] = shape;
        ++g.rtvN;
    }
    *shapeOut = shape;
    return res;
}

// ---- v8/v9: the draws made inside the ring pass, the offscreen ring layer, and the draws that sample it --------------------------------------------------------
constexpr uint32_t kRingFirstFrames = 12;     // every frame's ring-path lines for this many frames after a capture starts
constexpr uint32_t kMaxRpLines = 400;
constexpr uint32_t kCompFirstFrames = 40;     // every frame's compositor lines for this many frames after a capture starts, then one frame in kFingerEvery
constexpr uint32_t kMaxCompLines = 500;
constexpr uint32_t kMaxFillLines = 160;

void adoptRingTarget(const void* res, uint32_t w, uint32_t h, uint32_t fmt, const ringhook::Current& rc) {
    const TargetTracker::Result tr = g.tracker.observe(res);
    if (tr == TargetTracker::Same) return;
    const uint32_t oldId = g.ringTargetId, ow = g.ringTargetW, oh = g.ringTargetH, of = g.ringTargetFmt;
    if (g.ringTargetRef) g.ringTargetRef->Release();
    ID3D11Resource* r = static_cast<ID3D11Resource*>(const_cast<void*>(res));
    r->AddRef();
    g.ringTargetRef = r;
    g.ringTargetId = g.ids.id(res);
    g.ringTargetW = w;
    g.ringTargetH = h;
    g.ringTargetFmt = fmt;
    if (g.tgtHistN < 8) g.tgtHist[g.tgtHistN++] = {g.ringTargetId, w, h, fmt, g.frame};
    if (tr == TargetTracker::First) {
        Log::get().note("fss tiles: ring offscreen target identified: T%u %ux%u fmt%u, render target 0 of the first draw made inside a ring-pass call with ring+0x6b0 set "
                        "(camera %d, role %u, pass %d, f=%u); its contents are sampled every %u frames from here on.",
                        g.ringTargetId, w, h, fmt, rc.camera, rc.role, rc.pass, g.frame, kFillEvery);
    } else if (g.tgtChangeLines < 40) {
        ++g.tgtChangeLines;
        Log::get().note("fss tiles: ring offscreen target CHANGED at f=%u: T%u %ux%u fmt%u -> T%u %ux%u fmt%u (change %u; camera %d, role %u, pass %d); the fill series, the "
                        "fingerprints and the compositor search follow the new target.",
                        g.frame, oldId, ow, oh, of, g.ringTargetId, w, h, fmt, g.tracker.changes, rc.camera, rc.role, rc.pass);
    }
}

void ringPathRecord(ID3D11DeviceContext* ctx, void* rtv, const void* res, const ringhook::Current& rc, uint32_t instances, uint32_t seq) {
    State::RPEnt* e = nullptr;
    for (uint32_t i = 0; i < g.rpN; ++i) {
        State::RPEnt& x = g.rp[i];
        if (x.cam == rc.camera && x.role == rc.role && x.scratch == rc.scratch && x.target == res) { e = &x; break; }
    }
    if (!e) {
        if (g.rpN >= 48) return;
        e = &g.rp[g.rpN++];
        *e = State::RPEnt();   // the entries are reused every frame: start each one clean, or `draws` runs on across frames (flight f5c7ad89)
        e->cam = rc.camera;
        e->role = rc.role;
        e->scratch = rc.scratch;
        e->target = res;
        ResourceInfo ri;
        if (bindingResolveProbe(rtv, &ri) && ri.isTexture2D) { e->tw = ri.a; e->th = ri.b; e->tfmt = ri.fmt; }
        for (uint32_t k = 0; k < 4; ++k) {
            void* v = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + k));
            const State::ViewRec* vr = v ? viewInfo(v) : nullptr;
            e->srvIds[k] = vr && vr->res ? g.ids.id(vr->res) : 0;
        }
        UINT nv = 1;
        D3D11_VIEWPORT vp{};
        ctx->RSGetViewports(&nv, &vp);
        e->vpW = static_cast<uint32_t>(vp.Width);
        e->vpH = static_cast<uint32_t>(vp.Height);
        if (rc.scratch && e->tw >= kMinTargetWidth) adoptRingTarget(res, e->tw, e->th, e->tfmt, rc);   // every change of the layer's target, not only the first
    }
    ++e->draws;
    e->inst += instances;
    if (rc.scratch) {
        if (!g.ringSeqN) g.ringSeqFirst = seq;
        g.ringSeqLast = seq;
        ++g.ringSeqN;
    }
}

// Any draw outside the ring pass that samples the current offscreen layer: pixel-shader slots t0-t3 of every draw (the binding shadow, free), and t4-t15 of draws that have a pixel
// shader and a render target at least kMinTargetWidth wide (a PSGetShaderResources each).
void compositorCheck(ID3D11DeviceContext* ctx, void* rtv, const void* res, uint32_t shape, uint32_t instances, uint32_t seq) {
    int slot = -1;
    for (uint32_t k = 0; k < 4 && slot < 0; ++k) {
        void* v = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + k));
        const State::ViewRec* vr = v ? viewInfo(v) : nullptr;
        if (vr && vr->res == g.ringTargetRef) slot = static_cast<int>(k);
    }
    const uint64_t vs = bindingShaderHash(BindSlot::Vs), ps = bindingShaderHash(BindSlot::Ps);
    if (slot < 0 && shape && ps) {
        ID3D11ShaderResourceView* sv[12] = {};
        ctx->PSGetShaderResources(4, 12, sv);
        for (uint32_t i = 0; i < 12; ++i) {
            if (!sv[i]) continue;
            ID3D11Resource* r = nullptr;
            sv[i]->GetResource(&r);
            if (r) {
                if (r == g.ringTargetRef && slot < 0) slot = static_cast<int>(4 + i);
                r->Release();
            }
            sv[i]->Release();
        }
    }
    if (slot < 0) return;
    State::CompEnt* e = nullptr;
    uint32_t eye = 3;
    if (shape) {
        const uint32_t ord = targetOrder(res, shape);
        eye = shapeTargets(shape) == 2 ? ord : 3u;
    }
    for (uint32_t i = 0; i < g.compN; ++i) {
        State::CompEnt& x = g.comp[i];
        if (x.rt == res && x.vs == vs && x.ps == ps && x.slot == static_cast<uint32_t>(slot)) { e = &x; break; }
    }
    if (!e) {
        if (g.compN >= 48) return;
        e = &g.comp[g.compN++];
        *e = State::CompEnt();
        e->rt = res;
        e->vs = vs;
        e->ps = ps;
        e->slot = static_cast<uint32_t>(slot);
        e->eye = eye;
        e->firstSeq = seq;
        ResourceInfo ri;
        if (bindingResolveProbe(rtv, &ri) && ri.isTexture2D) { e->w = ri.a; e->h = ri.b; e->fmt = ri.fmt; }
        UINT nv = 1;
        D3D11_VIEWPORT vp{};
        ctx->RSGetViewports(&nv, &vp);
        e->vpW = static_cast<uint32_t>(vp.Width);
        e->vpH = static_cast<uint32_t>(vp.Height);
    }
    e->lastSeq = seq;
    ++e->draws;
    e->inst += instances;
}

void flushCompositors(uint32_t frame) {
    const bool sample = frame - g.rv.win.startFrame < kCompFirstFrames || frame % kFingerEvery == 0;
    for (uint32_t i = 0; i < g.compN; ++i) {
        State::CompEnt& c = g.comp[i];
        c.tgt = g.ids.id(c.rt);
        const RingOrder ord = orderOf(c.firstSeq, g.ringSeqFirst, g.ringSeqLast, g.ringSeqN);
        State::CompTot* t = nullptr;
        for (uint32_t k = 0; k < g.ctotN; ++k) {
            State::CompTot& x = g.ctot[k];
            if (x.vs == c.vs && x.ps == c.ps && x.rtId == c.tgt && x.slot == c.slot && x.eye == c.eye) { t = &x; break; }
        }
        bool isNew = false;
        if (!t && g.ctotN < 24) {
            t = &g.ctot[g.ctotN++];
            t->vs = c.vs; t->ps = c.ps; t->rtId = c.tgt; t->slot = c.slot; t->eye = c.eye; t->w = c.w; t->h = c.h; t->fmt = c.fmt;
            isNew = true;
        }
        if (t) {
            ++t->frames;
            t->draws += c.draws;
            ++t->order[static_cast<uint32_t>(ord)];
        }
        if ((sample || isNew) && g.compLines < kMaxCompLines) {
            char eyeText[8];
            if (c.eye < 2) std::snprintf(eyeText, sizeof(eyeText), "eye%u", c.eye);
            else std::snprintf(eyeText, sizeof(eyeText), "eye?");
            Log::get().note("fss tiles: compositor f=%u %s draw #%u..#%u (%u draws, %u instances) vs %016llX ps %016llX into T%u %ux%u fmt%u (viewport %ux%u) samples the offscreen ring "
                            "layer T%u in PS t%u; %s [camera 1's ring draws this frame: %u, #%u..#%u]%s",
                            frame, eyeText, c.firstSeq, c.lastSeq, c.draws, c.inst, static_cast<unsigned long long>(c.vs), static_cast<unsigned long long>(c.ps), c.tgt, c.w, c.h, c.fmt,
                            c.vpW, c.vpH, g.ringTargetId, c.slot, orderText(ord), g.ringSeqN, g.ringSeqFirst, g.ringSeqLast, isNew ? " (first sighting)" : "");
            ++g.compLines;
        }
    }
    g.compN = 0;
}

// ---- v9: the offscreen layer's 8 x 8 fingerprint, every kFillEvery frames, taken at the end of the frame (after the frame's ring draws) --------------------------------
void ringSnapshot(uint32_t frame) {
    if (!g.ringTargetRef || frame % kFillEvery != 0) return;
    if (g.snap.used && !g.snap.drained) { ++g.snapSkipped; return; }
    ID3D11Device* dev = nullptr;
    g.ringTargetRef->GetDevice(&dev);
    if (!dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    g.ringTargetRef->GetType(&dim);
    if (ctx && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td{};
        static_cast<ID3D11Texture2D*>(g.ringTargetRef)->GetDesc(&td);
        const uint32_t bpp = bytesPerPixel(td.Format);
        if ((td.BindFlags & D3D11_BIND_DEPTH_STENCIL) || td.SampleDesc.Count > 1 || td.Width < kTileGrid * kTilePx || td.Height < kTileGrid * kTilePx || !bpp ||
            !ensureFpTexture(dev, td.Format, &g.snapStage)) {
            if (!g.snapWarned) {
                g.snapWarned = true;
                Log::get().note("fss tiles: ring offscreen target T%u %ux%u fmt%u is not fingerprinted (depth, multisampled, too small or an unknown format).", g.ringTargetId, td.Width, td.Height, td.Format);
            }
        } else {
            for (uint32_t gy = 0; gy < kTileGrid; ++gy) {
                for (uint32_t gx = 0; gx < kTileGrid; ++gx) {
                    D3D11_BOX box{};
                    box.left = (td.Width - kTilePx) * gx / (kTileGrid - 1);
                    box.top = (td.Height - kTilePx) * gy / (kTileGrid - 1);
                    box.right = box.left + kTilePx;
                    box.bottom = box.top + kTilePx;
                    box.back = 1;
                    ctx->CopySubresourceRegion(g.snapStage, 0, gx * kTilePx, gy * kTilePx, 0, g.ringTargetRef, 0, &box);
                }
            }
            g.snap.used = true;
            g.snap.drained = false;
            g.snap.frame = frame;
            g.snap.id = g.ringTargetId;
            g.snap.w = td.Width;
            g.snap.h = td.Height;
            g.snap.fmt = td.Format;
            g.snap.bpp = bpp;
        }
    }
    if (ctx) ctx->Release();
    dev->Release();
}

void drainRingSnap(uint32_t frame) {
    if (!g.snap.used || g.snap.drained || !g.snapStage || frame < g.snap.frame + kReadbackDelay) return;
    ID3D11Device* dev = nullptr;
    g.snapStage->GetDevice(&dev);
    if (!dev) { g.snap.drained = true; return; }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (ctx && SUCCEEDED(ctx->Map(g.snapStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        const uint8_t* p = static_cast<const uint8_t*>(m.pData);
        const uint32_t pitch = m.RowPitch, bpp = g.snap.bpp;
        char rg[96];
        uint32_t boxes = 0;
        formatGrid(rg, sizeof(rg), [&](uint32_t x, uint32_t y) {
            uint32_t nz = 0;
            for (uint32_t row = 0; row < kTilePx; ++row) nz += countNonzero(p + (y * kTilePx + row) * pitch + x * kTilePx * bpp, kTilePx * bpp);
            const char c = nzChar(nz, kTilePx * kTilePx * bpp);
            if (c != '.') ++boxes;
            return c;
        });
        ctx->Unmap(g.snapStage, 0);
        if (g.ringFillN < 160) { g.ringFill[g.ringFillN].frame = g.snap.frame; g.ringFill[g.ringFillN].boxes = boxes; g.ringFill[g.ringFillN].id = g.snap.id; ++g.ringFillN; }
        if (g.ringFillN <= kMaxFillLines) {
            Log::get().note("fss tiles: ring fill f=%u T%u %ux%u fmt%u nz8x8=%s (boxes written %u of 64)", g.snap.frame, g.snap.id, g.snap.w, g.snap.h, g.snap.fmt, rg, boxes);
        }
        g.snap.drained = true;
    } else if (frame > g.snap.frame + 40) {
        g.snap.drained = true;
    }
    if (ctx) ctx->Release();
    dev->Release();
}

void flushRingPath(uint32_t frame) {
    const bool sample = frame % kFingerEvery == 0 || frame - g.rv.win.startFrame < kRingFirstFrames;
    for (uint32_t i = 0; i < g.rpN; ++i) {
        State::RPEnt& e = g.rp[i];
        const uint32_t tid = g.ids.id(e.target);
        State::RPTot* t = nullptr;
        for (uint32_t k = 0; k < g.rptotN; ++k) {
            if (g.rptot[k].cam == e.cam && g.rptot[k].role == e.role && g.rptot[k].scratch == e.scratch && g.rptot[k].targetId == tid) { t = &g.rptot[k]; break; }
        }
        if (!t && g.rptotN < 16) {
            t = &g.rptot[g.rptotN++];
            t->cam = e.cam; t->role = e.role; t->scratch = e.scratch; t->targetId = tid; t->tw = e.tw; t->th = e.th; t->tfmt = e.tfmt;
        }
        bool samplesOffscreen = false;
        for (uint32_t k = 0; k < 4; ++k) {
            if (g.ringTargetId && e.srvIds[k] == g.ringTargetId && tid != g.ringTargetId) samplesOffscreen = true;
        }
        if (t) {
            if (t->frames == 0 || e.draws < t->minD) t->minD = e.draws;
            if (e.draws > t->maxD) t->maxD = e.draws;
            ++t->frames;
            t->draws += e.draws;
            if (samplesOffscreen) ++t->samplesOffscreen;
        }
        if (sample && g.rpLines < kMaxRpLines) {
            Log::get().note("fss tiles: ring path f=%u cam=%d role=%u 6b0=%s: draws=%u instances=%u into T%u %ux%u fmt%u (viewport %ux%u); samples t0-t3 = T%u,T%u,T%u,T%u%s", frame,
                            e.cam, e.role, e.scratch ? "set" : "clear", e.draws, e.inst, tid, e.tw, e.th, e.tfmt, e.vpW, e.vpH, e.srvIds[0], e.srvIds[1], e.srvIds[2], e.srvIds[3],
                            samplesOffscreen ? " [samples the offscreen target]" : "");
            ++g.rpLines;
        }
    }
    g.rpN = 0;
}

void flushDraws(uint32_t frame) {
    flushCompositors(frame);
    flushRingPath(frame);
    g.drawSeq = g.ringSeqFirst = g.ringSeqLast = g.ringSeqN = 0;
    const bool line = frame % kFingerEvery == 0 && g.gbufLines < kMaxGbufLines;
    for (uint32_t i = 0; i < g.gdN; ++i) {
        State::GDraw& d = g.gd[i];
        if (!d.draws) continue;
        const uint32_t ri = static_cast<uint32_t>(d.role) - static_cast<uint32_t>(Role::Gbuf8);
        State::GTot& t = g.gtot[d.eye > 1 ? 2 : d.eye][ri];
        if (t.frames == 0 || d.draws < t.minDraws) t.minDraws = d.draws;
        if (d.draws > t.maxDraws) t.maxDraws = d.draws;
        ++t.frames;
        t.draws += d.draws;
        t.weight += d.weight;
        if (!t.haveVp) { t.vp0 = d.vp0; t.haveVp = true; }
        else if (d.haveVp && (d.vp0.Width != t.vp0.Width || d.vp0.Height != t.vp0.Height || d.vp0.TopLeftX != t.vp0.TopLeftX ||
                              d.vp0.TopLeftY != t.vp0.TopLeftY || d.vpN.Width != t.vp0.Width || d.vpN.Height != t.vp0.Height)) {
            ++t.vpVaries;
        }
        if (line) {
            char what[16];
            if (ri < 4) std::snprintf(what, sizeof(what), "t%u", 8 + ri);
            else std::snprintf(what, sizeof(what), "ring_t%u", ri - 4);
            Log::get().note("fss tiles: gbuf f=%u eye%u %s T%u draws=%u instances=%u viewport first (%.0f,%.0f %.0fx%.0f) last (%.0f,%.0f %.0fx%.0f) "
                            "scissor first (%d,%d,%d,%d) last (%d,%d,%d,%d)",
                            frame, d.eye, what, g.ids.id(d.res), d.draws, d.weight, d.vp0.TopLeftX, d.vp0.TopLeftY,
                            d.vp0.Width, d.vp0.Height, d.vpN.TopLeftX, d.vpN.TopLeftY, d.vpN.Width, d.vpN.Height, d.sc0.left, d.sc0.top,
                            d.sc0.right, d.sc0.bottom, d.scN.left, d.scN.top, d.scN.right, d.scN.bottom);
            ++g.gbufLines;
        }
        d = State::GDraw();
    }
    g.gdN = 0;
    g.viewN = 0;   // the per-frame view cache
}

void drainAll(ID3D11DeviceContext* ctx) {
    drain(ctx);
    drainSamples(ctx);
    drainFingerprints(ctx);
}

void emitSummary() {
    g.summaryDue = false;
    {
        Validity v = g.cap;
        v.arrivalFrame = fssRingHookArrivalInWindow();
        v.runsRan = g.rv.runs.ran;
        v.runsPlanned = g.rv.runs.planned;
        char vl[700];
        formatValidity(vl, sizeof(vl), g.windowNo, g.startedBy[0] ? g.startedBy : "(unknown)", v);
        Log::get().note("%s", vl);
    }
    char lock[200];
    std::snprintf(lock, sizeof(lock), "substituted=%u declined(shape=%u,noEye0=%u,sameArgs=%u,offset=%u,noBuffers=%u)",
                  g.substituted, g.declined[0], g.declined[1], g.declined[2], g.declined[3], g.declined[4]);
    Log::get().note("fss tiles: window %u closed -- frames=%u tile dispatches=%u readbacks=%u dropped=%u lines=%u lockstep=%s %s",
                    g.windowNo, g.framesSeen, g.targetDispatches, g.readbacks, g.dropped, g.lines,
                    g.lockstep ? "on" : "off", lock);
    char body[600];
    size_t at = 0;
    body[0] = 0;
    for (int e = 0; e < 3; ++e) {
        for (int c = 0; c < 2; ++c) {
            const Range& r = g.range[e][c];
            if (!r.n) continue;
            at += static_cast<size_t>(std::snprintf(body + at, sizeof(body) - at, "%seye%d %s n=%u x=%u..%u mean=%.1f yz<=%u",
                                                    at ? " | " : "", e, c ? "small" : "large", r.n, r.lo, r.hi,
                                                    static_cast<double>(r.sum) / r.n, r.yzMax));
        }
    }
    Log::get().note("fss tiles: window %u groups -- %s", g.windowNo, at ? body : "none read back");
    {
        double mean[2] = {};
        for (int e = 0; e < 2; ++e) {
            const uint64_t large = g.range[e][0].sum, smallSum = g.range[e][1].sum;
            mean[e] = large + smallSum ? 100.0 * static_cast<double>(smallSum) / static_cast<double>(large + smallSum) : 0.0;
        }
        Log::get().note("fss tiles: window %u lists -- samples read=%u lines=%u raw files written for %u frame(s) in edvr_logs\\fsstiles; "
                        "mean split small/(large+small): eye0 %.1f%% eye1 %.1f%%",
                        g.windowNo, g.samplesRead, g.listLines, g.sampleFiles, mean[0], mean[1]);
        char wb[500];
        size_t wa = 0;
        wb[0] = 0;
        for (uint32_t c = 0; c < kWriteCalls; ++c) {
            for (uint32_t r = 1; r < kRoles; ++r) {
                if (!g.writes[c][r]) continue;
                wa += static_cast<size_t>(std::snprintf(wb + wa, sizeof(wb) - wa, "%s%s->%s x%u", wa ? ", " : "",
                                                        writeCallName(c), roleName(static_cast<Role>(r)), g.writes[c][r]));
            }
        }
        Log::get().note("fss tiles: window %u writes -- watched %u buffer(s) (args, outputs, lists, tile info); %s", g.windowNo,
                        g.watch.n, wa ? wb : "no clear, copy or update touched any of them");
    }
    Log::get().note("fss tiles: window %u band watch -- %u band event(s) (automatic eye runs started this session: %u of %u); large-list "
                    "size over the window eye0 %u..%u eye1 %u..%u cells, largest one-frame step eye0 %u eye1 %u (an event needs a step of %u, "
                    "a whole row, or a list going to zero); band lines %u, raw frames written %u",
                    g.windowNo, g.windowEvents, g.autoRuns, kMaxAutoRuns, g.bandWatch.lo[0], g.bandWatch.hi[0], g.bandWatch.lo[1],
                    g.bandWatch.hi[1], g.bandWatch.maxStep[0], g.bandWatch.maxStep[1], kGrid, g.bandLines, g.bandFiles);
    {
        char gb[1800];
        size_t ga = 0;
        gb[0] = 0;
        for (uint32_t e = 0; e < 2; ++e) {
            for (uint32_t k = 0; k < 8; ++k) {
                const State::GTot& t = g.gtot[e][k];
                char nm[12];
                if (k < 4) std::snprintf(nm, sizeof(nm), "t%u", 8 + k);
                else std::snprintf(nm, sizeof(nm), "ring_t%u", k - 4);
                ga += static_cast<size_t>(std::snprintf(gb + ga, sizeof(gb) - ga, "%seye%u %s: frames with draws %u, draws %u (per frame %u..%u), instances %u, viewport changes %u",
                                                        ga ? " | " : "", e, nm, t.frames, t.draws, t.minDraws, t.maxDraws, t.weight, t.vpVaries));
            }
        }
        Log::get().note("fss tiles: window %u upstream -- draws into the per-eye G-buffer textures t8-t11 and the ring quad's own inputs ring_t0-t3 (render target 0): %s", g.windowNo,
                        g.watchGbuf ? gb : "no render target of the window matched them (not drawn into through render target 0)");
        Log::get().note("fss tiles: window %u inputs -- fingerprints read=%u lines=%u; sampled grids that differ between the eyes: slot0 %u of %u, depth %u of %u, stencil %u of %u, "
                        "t8 %u of %u, t9 %u of %u, t10 %u of %u, t11 %u of %u frames; gbuf lines %u",
                        g.windowNo, g.fpRead, g.inputLines, g.gridDiffer[0], g.gridFrames[0], g.gridDiffer[1], g.gridFrames[1], g.gridDiffer[2], g.gridFrames[2],
                        g.gridDiffer[3], g.gridFrames[3], g.gridDiffer[4], g.gridFrames[4], g.gridDiffer[5], g.gridFrames[5], g.gridDiffer[6], g.gridFrames[6],
                        g.gbufLines);
        // the eye diff
        char rk[1400];
        size_t ra = 0;
        rk[0] = 0;
        uint32_t idx[8];
        const uint32_t nr = g.rank.order(idx, 8);
        static const char* const kWhat[6] = {"only-eye0", "only-eye1", "draws differ", "instances differ", "inputs differ", "viewport differ"};
        for (uint32_t i = 0; i < nr; ++i) {
            const RankEntry& re = g.rank.e[idx[i]];
            ra += static_cast<size_t>(std::snprintf(rk + ra, sizeof(rk) - ra, "%s[%u] %s %08llX/%08llX %c n=%u rt-shape %u in %u frame(s), total %llu", i ? "; " : "", i + 1,
                                                    kWhat[re.what], static_cast<unsigned long long>(re.k.vs >> 32), static_cast<unsigned long long>(re.k.ps >> 32), re.k.kind,
                                                    re.k.count, re.k.rtShape, re.frames, static_cast<unsigned long long>(re.total)));
        }
        Log::get().note("fss tiles: window %u eyediff -- ledger ran %u frame(s) from the all-small event (%s), twin-target pairs compared in %u, frames with a difference %u, eyediff lines %u; "
                        "ledger entries that did not fit %u; ranked differences (%u kinds%s): %s",
                        g.windowNo, g.ledgerFrames, g.windowEvents ? "armed by it" : "never armed: no all-small event in this window", g.pairedFrames, g.framesWithDiff,
                        g.eyediffLines, g.ledOverflowTotal, g.rank.used, g.rank.dropped ? ", more than the table holds" : "", nr ? rk : "none: the two eyes' lists matched in every compared frame");
        char so[900];
        size_t sa = 0;
        so[0] = 0;
        bool taken[32] = {};
        uint32_t bestIdx[6] = {};
        uint32_t nb = 0;
        for (uint32_t t = 0; t < 6; ++t) {
            int best = -1;
            for (uint32_t i = 0; i < g.soloN; ++i) {
                if (taken[i]) continue;
                if (best < 0 || g.solo[i].draws > g.solo[best].draws) best = static_cast<int>(i);
            }
            if (best < 0) break;
            taken[best] = true;
            bestIdx[nb++] = static_cast<uint32_t>(best);
        }
        for (uint32_t i = 0; i < nb; ++i) {
            const State::Solo& sl = g.solo[bestIdx[i]];
            sa += static_cast<size_t>(std::snprintf(so + sa, sizeof(so) - sa, "%s%08llX/%08llX %c n=%u rt-shape %u x%u in %u frame(s)%s", i ? "; " : "",
                                                    static_cast<unsigned long long>(sl.k.vs >> 32), static_cast<unsigned long long>(sl.k.ps >> 32), sl.k.kind, sl.k.count,
                                                    sl.k.rtShape, sl.draws, sl.frames, sl.ring ? " [ring]" : ""));
        }
        Log::get().note("fss tiles: window %u eyediff unpaired -- %u draw(s) went to render targets of a shape that does not hold exactly two textures (a shared or mono target, "
                        "so no eye to diff); the biggest families: %s", g.windowNo, g.unpaired, nb ? so : "none");
        char rf[500];
        size_t fa = 0;
        rf[0] = 0;
        for (uint32_t i = 0; i < g.ringFamN; ++i) {
            fa += static_cast<size_t>(std::snprintf(rf + fa, sizeof(rf) - fa, "%s%08llX/%08llX x%u", i ? "; " : "", static_cast<unsigned long long>(g.ringFam[i].vs >> 32),
                                                    static_cast<unsigned long long>(g.ringFam[i].ps >> 32), g.ringFam[i].draws));
        }
        Log::get().note("fss tiles: window %u ring quad -- ring-family draws (the 332753 VS hashes, the quad PS, the 13248-index mesh): eye0 in %u frame(s) %u draws | eye1 in %u frame(s) %u "
                        "draws | unpaired target %u frame(s) %u draws; ring PS t0-t3 the same resource in both eyes: t0 %u of %u, t1 %u of %u, t2 %u of %u, t3 %u of %u frames; "
                        "families: %s%s",
                        g.windowNo, g.ringFrames[0], g.ringDrawTotal[0], g.ringFrames[1], g.ringDrawTotal[1], g.ringFrames[2], g.ringDrawTotal[2], g.ringShared[0], g.ringBoth[0],
                        g.ringShared[1], g.ringBoth[1], g.ringShared[2], g.ringBoth[2], g.ringShared[3], g.ringBoth[3], g.ringFamN ? rf : "none",
                        g.ringFamN ? "" : " (no draw matched the ring family: the hashes may have changed since build 332753; the top differing and unpaired draws above are the candidates)");
        fssRingHookWindowSummary(g.windowNo);
        {
            char rt[1500];
            size_t ra2 = 0;
            rt[0] = 0;
            bool mainDraws = false, otherDraws = false, otherSamples = false;
            for (uint32_t i = 0; i < g.rptotN; ++i) {
                const State::RPTot& t = g.rptot[i];
                ra2 += static_cast<size_t>(std::snprintf(rt + ra2, sizeof(rt) - ra2, "%scam=%d role=%u 6b0=%s into T%u %ux%u fmt%u: frames %u, draws %u (per frame %u..%u)%s", i ? "; " : "",
                                                         t.cam, t.role, t.scratch ? "set" : "clear", t.targetId, t.tw, t.th, t.tfmt, t.frames, t.draws, t.minD, t.maxD,
                                                         t.samplesOffscreen ? " [samples the offscreen target]" : ""));
                if (g.ringTargetId && t.targetId == g.ringTargetId) {
                    if (t.role == 1) mainDraws = true; else otherDraws = true;
                }
                if (t.role != 1 && t.samplesOffscreen) otherSamples = true;
            }
            char tgt[120];
            if (g.ringTargetRef) std::snprintf(tgt, sizeof(tgt), "T%u %ux%u fmt%u", g.ringTargetId, g.ringTargetW, g.ringTargetH, g.ringTargetFmt);
            else std::snprintf(tgt, sizeof(tgt), "not identified (no draw inside a ring-pass call with ring+0x6b0 set)");
            int mainId = 0;
            const bool haveMain = fssRingHookMainCamera(&mainId);
            Log::get().note("fss tiles: window %u ring path -- offscreen ring target (ring+0x6b0): %s; drawn into by %s; the other camera's ring draws %s; main camera id %s; per camera: %s",
                            g.windowNo, tgt, !g.ringTargetRef ? "nothing seen" : (mainDraws && !otherDraws ? "the role-1 (main, left eye) camera only" : (mainDraws ? "both cameras" : "the other camera only")),
                            otherSamples ? "sample it" : (g.rptotN ? "do not sample it (they render directly, or into something else)" : "were not seen"),
                            haveMain ? "known" : "not read", g.rptotN ? rt : "no draw was made inside a ring-pass call");
            // the targets the layer had, in order
            char th[400];
            size_t ta = 0;
            th[0] = 0;
            for (uint32_t i = 0; i < g.tgtHistN; ++i) {
                ta += static_cast<size_t>(std::snprintf(th + ta, sizeof(th) - ta, "%sT%u %ux%u fmt%u from f=%u", i ? "; " : "", g.tgtHist[i].id, g.tgtHist[i].w, g.tgtHist[i].h, g.tgtHist[i].fmt,
                                                         g.tgtHist[i].frame));
            }
            Log::get().note("fss tiles: window %u ring offscreen target history -- %u change(s) after the first: %s", g.windowNo, g.tracker.changes, g.tgtHistN ? th : "no target");
            // the compositors: draws outside the ring pass that sampled the layer
            if (!g.ctotN) {
                Log::get().note("fss tiles: window %u compositors -- no draw outside the ring pass sampled the offscreen ring layer (checked: PS t0-t3 of every draw, t4-t15 of draws with a "
                                "pixel shader into a render target at least %u wide; vertex-shader and compute reads are not checked).", g.windowNo, kMinTargetWidth);
            } else {
                for (uint32_t i = 0; i < g.ctotN; ++i) {
                    const State::CompTot& t = g.ctot[i];
                    char eyeText[8];
                    if (t.eye < 2) std::snprintf(eyeText, sizeof(eyeText), "eye%u", t.eye);
                    else std::snprintf(eyeText, sizeof(eyeText), "eye?");
                    Log::get().note("fss tiles: window %u compositor %u of %u -- %s vs %016llX ps %016llX into T%u %ux%u fmt%u, samples the layer in PS t%u: %u frames, %u draws; "
                                    "per frame its first draw came before camera 1's ring draws %u, between them %u, after them %u, with no ring draw in the frame %u",
                                    g.windowNo, i + 1, g.ctotN, eyeText, static_cast<unsigned long long>(t.vs), static_cast<unsigned long long>(t.ps), t.rtId, t.w, t.h, t.fmt, t.slot,
                                    t.frames, t.draws, t.order[1], t.order[2], t.order[3], t.order[0]);
                }
            }
            // the fill series, kFillEvery frames apart, in lines of 24
            if (!g.ringFillN) {
                Log::get().note("fss tiles: window %u ring offscreen target fill -- no samples (no target, or none sampleable); %u skipped for a read-back still pending.", g.windowNo, g.snapSkipped);
            } else {
                for (uint32_t i = 0; i < g.ringFillN; i += 24) {
                    char fl[700];
                    size_t fa2 = 0;
                    fl[0] = 0;
                    for (uint32_t k = i; k < g.ringFillN && k < i + 24 && fa2 + 24 < sizeof(fl); ++k) {
                        fa2 += static_cast<size_t>(std::snprintf(fl + fa2, sizeof(fl) - fa2, "%sf%u:T%u:%u", k > i ? " " : "", g.ringFill[k].frame, g.ringFill[k].id, g.ringFill[k].boxes));
                    }
                    Log::get().note("fss tiles: window %u ring offscreen target fill (every %u frames; frame:target:boxes written of 64) %u-%u of %u: %s", g.windowNo, kFillEvery, i + 1,
                                    i + 24 < g.ringFillN ? i + 24 : g.ringFillN, g.ringFillN, fl);
                }
            }
        }
    }
    char sl[200];
    size_t sa = 0;
    sl[0] = 0;
    for (uint32_t i = 0; i < kSrvSlots; ++i) {
        sa += static_cast<size_t>(std::snprintf(sl + sa, sizeof(sl) - sa, "%s%u/%u", i ? " " : "", g.share.slotSame[i],
                                                g.share.slotBoth[i]));
    }
    Log::get().note("fss tiles: window %u sharing -- view pairs=%u sameArgsBuffer=%u sameUav0=%u; per SRV slot 0-7, same "
                    "resource in both views / bound in both: %s",
                    g.windowNo, g.share.pairs, g.share.argsSame, g.share.uavSame, sl);
    if (g.targetDispatches == 0) {
        char ot[200];
        size_t oa = 0;
        ot[0] = 0;
        for (const OtherCs& o : g.other) {
            if (!o.hash) continue;
            oa += static_cast<size_t>(std::snprintf(ot + oa, sizeof(ot) - oa, "%s%016llX x%u", oa ? ", " : "",
                                                    static_cast<unsigned long long>(o.hash), o.n));
        }
        Log::get().note("fss tiles: window %u saw NO dispatch of CS %016llX or %016llX -- the shader hashes may have changed "
                        "on this game build. Other indirect compute shaders seen: %s.",
                        g.windowNo, static_cast<unsigned long long>(kCsLarge), static_cast<unsigned long long>(kCsSmall),
                        oa ? ot : "none");
    }
}

// ---- one tile-renderer dispatch ---------------------------------------------------------------------------------------
void describeBindings(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView** sr, Slot* slot, Rec& r, const void** uavRes) {
    ctx->CSGetShaderResources(0, kCaptureSlots, sr);
    for (uint32_t i = 0; i < kCaptureSlots; ++i) {
        if (!sr[i]) continue;
        slot[i].view = sr[i];
        ID3D11Resource* res = nullptr;
        sr[i]->GetResource(&res);
        if (!res) continue;
        slot[i].res = res;
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bd{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
            slot[i].isBuffer = true;
            slot[i].bytes = bd.ByteWidth;
        }
        if (i < kSrvSlots) {
            r.srvId[i] = g.ids.id(res);
            r.srvKind[i] = slot[i].isBuffer ? 1 : 2;
            r.srvBytes[i] = slot[i].bytes;
        }
        res->Release();   // the view holds the resource; the pointer stays valid as an identity while the view is held
    }
    ID3D11UnorderedAccessView* uv[2] = {};
    ctx->CSGetUnorderedAccessViews(0, 2, uv);
    for (int i = 0; i < 2; ++i) {
        if (!uv[i]) continue;
        ID3D11Resource* res = nullptr;
        uv[i]->GetResource(&res);
        if (res) {
            r.uavId[i] = g.ids.id(res);
            uavRes[i] = res;   // an identity only; the game holds the view
            res->Release();
        }
        uv[i]->Release();
    }
}

bool onTarget(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT off, uint64_t h,
              FssTilesRealDispatchIndirect real) {
    const int ci = csIndex(h);
    if (g.lastFrameCounted != g.frame) {
        g.lastFrameCounted = g.frame;
        ++g.framesSeen;
    }
    ++g.targetDispatches;

    Rec r;
    r.frame = g.frame;
    r.cs = h;
    r.off = off;
    r.eye = g.views.eyeOf(args);
    r.occ = g.occ++;
    r.argsId = g.ids.id(args);

    ID3D11ShaderResourceView* sr[kCaptureSlots] = {};
    Slot slot[kCaptureSlots];
    const void* uavRes[2] = {};
    describeBindings(ctx, sr, slot, r, uavRes);
    if (g.rv.win.capturing) registerWatch(r, args, slot, uavRes);

    // ---- the candidate ----
    bool sub = false;
    uint32_t mask = 0;
    bool keepViews = false;   // eye zero's views are kept by the record
    if (g.lockstep && g.rv.win.open) {
        EyeZero& z = g.e0[ci];
        if (r.eye == 0) {
            releaseEyeZero(z);
            z.valid = true;
            z.frame = g.frame;
            z.off = off;
            args->AddRef();
            z.args = args;
            for (uint32_t i = 0; i < kSrvSlots; ++i) {
                z.views[i] = sr[i];
                z.slot[i] = slot[i];
            }
            keepViews = true;
        } else if (r.eye == 1) {
            Decline why = kDeclineCount;
            if (g.prevViews != 2) why = kDeclineShape;
            else if (!z.valid || z.frame != g.frame) why = kDeclineNoEye0;
            else if (z.args == static_cast<void*>(args)) why = kDeclineSameArgs;
            else if (z.off != off) why = kDeclineOffset;
            else {
                mask = substitutionMask(z.slot, slot);
                if (mask == 0) why = kDeclineNoBuffers;
            }
            if (why != kDeclineCount) {
                ++g.declined[why];
                if (!g.declineNoted[why]) {
                    g.declineNoted[why] = true;
                    Log::get().note("fss tiles: lockstep declined at f=%u (cs %016llX): %s. Counted in the window's "
                                    "summary; said once per window per reason.",
                                    g.frame, static_cast<unsigned long long>(h), kDeclineText[why]);
                }
            } else {
                sub = true;
            }
        }
    }

    // ---- the read-back copy (before the dispatch; it reads the arguments as this view's dispatch will) ----
    if (g.rv.win.capturing) {
        r.substituted = sub;
        queueSample(ctx, r, slot);
        queueFingerprint(ctx, r, slot);
    }
    if (g.rv.win.capturing && ensureStaging(ctx)) {
        FrameBuf& fb = g.fb[g.frame % kStagingRing];
        if (!fb.used || fb.frame != g.frame) {
            if (fb.used && !fb.drained) g.dropped += fb.n;
            fb.frame = g.frame;
            fb.used = true;
            fb.drained = false;
            fb.n = 0;
            fb.logFrame = g.rv.win.wantsDetail(g.frame);
        }
        if (fb.n < kMaxPerFrame) {
            D3D11_BOX box{};
            box.left = off;
            box.right = off + 12;
            box.bottom = 1;
            box.back = 1;
            ctx->CopySubresourceRegion(g.staging[g.frame % kStagingRing], 0, fb.n * 12, 0, 0, args, 0, &box);
            r.subMask = sub ? mask : 0;
            r.substituted = sub;
            fb.rec[fb.n++] = r;
        } else {
            ++g.dropped;
        }
    }

    // ---- the dispatch ----
    bool consumed = false;
    if (sub) {
        EyeZero& z = g.e0[ci];
        D3dOps ops;
        ops.ctx = ctx;
        ops.real = reinterpret_cast<void (*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT)>(real);
        g.dispatched = false;
        swapIn(ops, g.sw, z.views, mask);
        ops.dispatchIndirect(z.args, off);
        g.dispatched = true;
        swapBack(ops, g.sw);
        consumed = true;
        ++g.substituted;
        if (!g.engagedNoted) {
            g.engagedNoted = true;
            Log::get().note("fss tiles: lockstep engaged at f=%u (cs %016llX): the second view's dispatch ran from the first "
                            "view's argument buffer A%u and buffer bindings (SRV slots 0x%02X); its own bindings were put "
                            "back after the dispatch.",
                            g.frame, static_cast<unsigned long long>(h), g.ids.id(z.args), mask);
        }
    }

    for (uint32_t i = 0; i < kCaptureSlots; ++i) {
        // eye zero's record keeps slots 0-7 (kSrvSlots); the rest, and everything on another view, are released here
        if (sr[i] && !(keepViews && i < kSrvSlots)) { sr[i]->Release(); sr[i] = nullptr; }
    }
    return consumed;
}

}  // namespace

// ---- public ---------------------------------------------------------------------------------------------------------
void fssTilesConfigure(Config& cfg) {
    if (runtimeFlatProfile()) return;   // one view: nothing to compare, and the flat hook never reaches this module
    const bool want = eyeSyncFromConfig(cfg).tiles;
    g.lockstep = want;
    if (!g.armedNoted || g.lastLockstepNoted != want) {
        g.armedNoted = true;
        g.lastLockstepNoted = want;
        Log::get().note("fss tiles: instrument armed -- inside each FSS zoom window (at most %u a session) it reads back the "
                        "group counts of CS %016llX / %016llX per view and logs what each dispatch binds; the candidate "
                        "(fix.fss_eye_sync = tiles) is %s; it also watches the lists for the band event and starts the eye run itself "
                        "(at most %u a session, no key). A session with no 'window opened' line below never reached a zoom window.",
                        kMaxWindows, static_cast<unsigned long long>(kCsLarge), static_cast<unsigned long long>(kCsSmall),
                        want ? "ON" : "off", kMaxAutoRuns);
    }
    refreshActive();
}

void fssTilesNoteWriteSlow(const void* p, bool isView, uint32_t call) {
    if (!g.rv.win.capturing || call >= kWriteCalls || !p) return;
    guardedBudget(g.budget, [&] {
        const void* res = p;
        ID3D11Resource* got = nullptr;
        if (isView) {
            static_cast<ID3D11View*>(const_cast<void*>(p))->GetResource(&got);
            res = got;
        }
        const int i = g.watch.find(res);
        if (got) got->Release();
        if (i < 0) return;
        const Role role = g.watch.role[i];
        ++g.writes[call][static_cast<uint32_t>(role)];
        if (g.writeLines < kMaxWriteLines) {
            ++g.writeLines;
            Log::get().note("fss tiles: write f=%u %s -> %s A%u (eye %u)%s", g.frame, writeCallName(call), roleName(role),
                            g.ids.id(res), static_cast<unsigned>(g.watch.eye[i]),
                            g.writeLines == kMaxWriteLines ? "; write-line cap reached, the closing summary still counts them" : "");
        }
    });
}

void fssTilesNoteDrawSlow(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances) {
    if (!g.rv.win.capturing) return;
    guardedBudget(g.budget, [&] {
        const uint32_t seq = ++g.drawSeq;   // the draw's order index in the frame, every draw noted
        void* rtv = bindingGet(BindSlot::Rtv0);
        if (!rtv) return;
        uint32_t shape = 0;
        const void* res = rtvResource(rtv, &shape);
        if (!res) return;
        ringhook::Current rc;
        if (fssRingHookCurrent(&rc)) ringPathRecord(ctx, rtv, res, rc, instances, seq);
        else if (g.ringTargetRef) compositorCheck(ctx, rtv, res, shape, instances, seq);
        const int i = g.watch.find(res);
        if (g.watchGbuf && i >= 0 && g.watch.role[i] >= Role::Gbuf8 && g.watch.role[i] <= Role::Ring3) {
            const Role role = g.watch.role[i];
            State::GDraw* d = nullptr;
            for (uint32_t k = 0; k < g.gdN; ++k) {
                if (g.gd[k].res == res) d = &g.gd[k];
            }
            if (!d && g.gdN < 16) {
                d = &g.gd[g.gdN++];
                d->res = res;
                d->eye = g.watch.eye[i];
                d->role = role;
            }
            if (d) {
                ++d->draws;
                d->weight += instances;
                if (d->draws <= 2 || (d->draws & 15u) == 0) {
                    UINT nv = 1;
                    D3D11_VIEWPORT vp{};
                    ctx->RSGetViewports(&nv, &vp);
                    UINT ns = 1;
                    D3D11_RECT sc{};
                    ctx->RSGetScissorRects(&ns, &sc);
                    if (!d->haveVp) {
                        d->vp0 = vp;
                        d->sc0 = sc;
                        d->haveVp = true;
                    }
                    d->vpN = vp;
                    d->scN = sc;
                }
            }
        }
        if (g.rv.ledgerOn && shape) ledgerRecord(ctx, res, shape, kind, count, instances);
    });
}

void fssTilesNoteEyeDump() {
    if (g.autoArming) {
        Log::get().note("fss tiles: automatic eye run started at f=%u (the band event); the eye dump writes to edvr_logs\\eyes and the "
                        "raw lists of the frames around the event are in edvr_logs\\fsstiles.", g.frame);
        return;
    }
    if (!g.rv.win.capturing) {
        if (g.eyeDumpNotes < 3) {
            ++g.eyeDumpNotes;
            Log::get().note("fss tiles: the eye-dump key was pressed outside a capturing zoom window (f=%u): no raw buffers are "
                            "written for it. Press it inside the 10 s after a zoom. Said at most 3 times.", g.frame);
        }
        return;
    }
    g.dumpFramesLeft = kEyeDumpFrames;
    Log::get().note("fss tiles: eye-dump key at f=%u -- the raw lists and tile-info buffers of the next %u frames go to "
                    "edvr_logs\\fsstiles as f<frame>_e<eye>_<listL|listS|info>.bin, beside the eye run.", g.frame, kEyeDumpFrames);
}

bool fssTilesWantsSignals() { return !runtimeFlatProfile(); }
bool fssTilesWantsChrome() { return !runtimeFlatProfile(); }

namespace {
void agoText(char* out, size_t cap, uint32_t ago) {
    if (ago == 0xFFFFFFFFu) std::snprintf(out, cap, "never");
    else std::snprintf(out, cap, "%u frame(s) ago", ago);
}
}  // namespace

void fssTilesNoteZoomSignal(uint32_t frameNo, bool press, uint32_t chromeAgo) {
    g.latch.update(deviceHookFssModeLatch(), nowMs());
    g.signalPress = press;
    g.signalFrame = frameNo;
    char ago[40];
    agoText(ago, sizeof(ago), chromeAgo);
    // A jump this soon after the FSS latch closed is the exit (flight 0dbd3964: the latch closed 0.18 s before the exit jump, the chrome still held).
    if (!press && g.latch.jumpRefused(nowMs())) {
        if (g.noWindowNotes < 8) {
            ++g.noWindowNotes;
            Log::get().note("fss tiles: a camera jump at frame %u is not a window opener: the FSS latch closed less than 1 s ago (the exit back to the cockpit; the chrome was last matched %s). "
                            "Said at most 8 times.", frameNo, ago);
        }
        return;
    }
    const bool wasOpen = g.rv.gate.open;
    const ZoomGate::Refusal r = g.rv.gate.signal(frameNo, chromeAgo);
    if (r != ZoomGate::Accepted) {
        if (g.noWindowNotes < 8) {
            ++g.noWindowNotes;
            Log::get().note("fss tiles: a %s at frame %u is not a window opener: the scanner chrome is not held (last matched %s). A window opens only on a jump or a zoom press "
                            "while the scanner is up (flight e05b6be6: the exit jump back to the cockpit opened one). Said at most 8 times.",
                            press ? "zoom press" : "camera jump", frameNo, ago);
        }
        return;
    }
    if (wasOpen) {
        // The gate is already open (flight 0dbd3964: a press 0.5 s after the capture closed found it open and started nothing): a later accepted press or jump restarts the capture.
        g.rv.restartRequested = true;
        std::snprintf(g.restartWhy, sizeof(g.restartWhy), "%s at f=%u (a restart: the gate was open)", press ? "zoom press" : "camera jump", frameNo);
    } else {
        g.signalPending = true;
        std::snprintf(g.pendingStartedBy, sizeof(g.pendingStartedBy), "%s at f=%u", press ? "zoom press" : "camera jump", frameNo);
    }
}

void fssTilesFrameBoundary(uint32_t frameNo, uint32_t chromeAgo, bool focusKnown, bool focus) {
    if (g.rv.win.capturing) {
        ringSnapshot(g.frame);   // the offscreen layer at the end of the frame, after the frame's ring draws
        flushDraws(g.frame);
        flushLedger(g.frame);
    }
    g.frame = frameNo;
    g.chromeAgo = chromeAgo;
    g.prevViews = g.views.n;
    g.views.reset();
    g.occ = 0;
    const bool latchOpen = deviceHookFssModeLatch();
    g.latch.update(latchOpen, nowMs());
    // While the scanner chrome is held the ring pass hook looks for the arrival itself (the first call with ring+0x6b0 set), capture or not.
    fssRingHookScan(chromeHeld(chromeAgo));
    RevealMachine::In in;
    in.frame = frameNo;
    in.ms = nowMs();
    in.chromeAgo = chromeAgo;
    in.focusKnown = focusKnown;
    in.focus = focus;
    uint32_t aFrame = 0;
    float aAlpha = 0.0f;
    int aCam = 0;
    in.arrivalOffered = fssRingHookTakeArrival(&aFrame, &aAlpha, &aCam);
    const RevealMachine::Out o = g.rv.step(in);   // every decision that starts, restarts, ends or arms a capture; tools/fss_tiles_test replays flight f5c7ad89 through it
    g.zoomOpen = g.rv.gate.open;
    if (o.gateClose >= ZoomGate::Expired) {
        const bool pending = o.cancelledExit || o.endedBy[0];
        Log::get().note("fss tiles: window %u gate closed at f=%u: %s.%s", g.windowNo, frameNo, closeText(o.gateClose),
                        o.exited && pending ? " The reveal capture ends with it, and any pending eye run is cancelled." : "");
    }
    if (o.arrivalRefused && g.noWindowNotes < 8) {
        ++g.noWindowNotes;
        Log::get().note("fss tiles: an arrival signature at f=%u (camera %d, ring+0x6b0 set) is refused: the scanner chrome is not held on this frame. Said at most 8 times.", aFrame, aCam);
    }
    if (o.restartIgnored) {
        Log::get().note("fss tiles: a later zoom press or camera jump (%s) at f=%u does not restart the capture: the reveal capture the arrival armed is running (until f=%u).",
                        g.restartWhy, frameNo, g.rv.revealUntil);
    }
    char startedBy[140] = "";
    if (o.start == RevealMachine::StartArrival) {
        std::snprintf(startedBy, sizeof(startedBy), "ring arrival (the first ring-pass call with ring+0x6b0 set, camera %d, scan alpha %.3f, seen at f=%u)", aCam, static_cast<double>(aAlpha), aFrame);
    } else if (o.start == RevealMachine::StartRestart) {
        std::snprintf(startedBy, sizeof(startedBy), "%s", g.restartWhy);
    } else {
        std::snprintf(startedBy, sizeof(startedBy), "%s", g.pendingStartedBy[0] ? g.pendingStartedBy : "the gate");
    }
    const auto cancelNote = [&](uint32_t mask, const char* why) {
        for (uint32_t k = 0; k < kRunCount; ++k) {
            if (mask & (1u << k)) Log::get().note("fss tiles: automatic eye run %u cancelled at f=%u: %s.", k + 1, frameNo, why);
        }
    };
    cancelNote(o.cancelledRestart, "the capture was restarted");
    cancelNote(o.cancelledExit, o.exited ? closeText(o.gateClose) : "the capture ended");
    if (o.cancelledOver) {
        for (uint32_t k = 0; k < kRunCount; ++k) {
            if (o.cancelledOver & (1u << k)) {
                Log::get().note("fss tiles: automatic eye run %u never ran: the scanner chrome was not held on any frame from its due frame to the end of the capture.", k + 1);
            }
        }
    }
    if (o.restarted) {
        // The running capture is over: say so (its validity line says why it was cut short), and start the new one.
        g.cap.endedBy = o.start == RevealMachine::StartArrival ? "restarted by the arrival" : "restarted by a later zoom press or jump";
        emitSummary();
    }
    if (o.endedBy[0]) g.cap.endedBy = o.endedBy;
    if (o.ev == WindowMachine::Opened) {
        g.windowNo = g.rv.win.windows;
        resetWindowTallies();
        std::snprintf(g.startedBy, sizeof(g.startedBy), "%s", startedBy);
        g.cap.chromeAtStart = chromeHeld(chromeAgo);
        g.cap.endedBy = "";
        g.pendingStartedBy[0] = 0;
        if (!o.armed) g.rv.runs.planned = g.rv.runs.ran = 0;
        char ago[40];
        agoText(ago, sizeof(ago), chromeAgo);
        Log::get().note("fss tiles: window %u opened at frame %u (started by %s; lockstep=%s; chrome held=%s, last matched %s) -- logging the tile "
                        "renderer's dispatches for up to %llu ms, at most %u lines. Band watch: armed, waiting for a whole-row move of a large list "
                        "or one going to zero; the ring pass hook's arrival (the first call with ring+0x6b0 set) starts the reveal capture (%u frames) and the automatic eye runs (at most %u a session) at once.",
                        g.windowNo, frameNo, startedBy, g.lockstep ? "on" : "off", chromeHeld(chromeAgo) ? "yes" : "no", ago, static_cast<unsigned long long>(kWindowMs),
                        kMaxLinesPerWindow, kRevealFrames, kMaxAutoRuns);
        if (o.armed) {
            armGlue(frameNo);
            Log::get().note("fss tiles: arrival at f=%u (the ring pass, camera %d: ring+0x6b0 set, scan alpha %.3f, scanner chrome held): the capture starts here. Capture continues "
                            "until f=%u (%u frames); %u automatic eye run(s), the first at f=%u (offsets %u, %u, %u; planned %u of %u this session); ledger %u frames; fill series every %u frames; "
                            "input, gbuf, ring path, ring call and compositor lines follow.",
                            frameNo, aCam, static_cast<double>(aAlpha), g.rv.revealUntil, kRevealFrames, o.armedRuns, o.armedRuns ? g.rv.runs.due[0] : 0u, kRunOffsets[0], kRunOffsets[1],
                            kRunOffsets[2], g.rv.runs.sessionPlanned, kMaxAutoRuns, kLedgerFrames, kFillEvery);
        }
    } else if (o.ev == WindowMachine::Closed) {
        g.summaryDue = true;
        g.summaryAfterFrame = frameNo + kReadbackDelay + 6;
    } else if (o.start != RevealMachine::StartGate && o.ev == WindowMachine::None) {
        Log::get().note("fss tiles: a restart at f=%u (%s) found no window left (the session's %u-window cap).", frameNo, startedBy, kMaxWindows);
    }
    if (g.reduceUntil && frameNo > g.reduceUntil) { g.reduceUntil = 0; eye_final_capture::g_reduceLevel = 0; }
    for (uint32_t k = 0; k < kRunCount; ++k) {
        if (o.refused & (1u << k)) {
            char ago[40];
            agoText(ago, sizeof(ago), chromeAgo);
            Log::get().note("fss tiles: automatic eye run %u refused at f=%u: the scanner chrome is not held (last matched %s). It stays pending until the chrome is held or "
                            "the capture ends; an eye run without the scanner on show is a capture of the cockpit.", k + 1, frameNo, ago);
        }
        if (o.fire & (1u << k)) {
            ++g.autoRuns;
            eye_final_capture::g_reduceLevel = kReduceLevel;   // the final-image rows of this run: the whole submitted eye at 1/4 size, not the 1400 px centre crop
            g.reduceUntil = frameNo + 40;
            g.autoArming = true;
            temporalPassArmEyeDump();   // the eye run, from the Present tick like the key's own press
            g.autoArming = false;
        }
    }
    if (g.signalPending) {
        g.signalPending = false;
        if (!g.rv.win.open && g.rv.win.windows >= kMaxWindows && g.noWindowNotes < 8) {
            ++g.noWindowNotes;
            Log::get().note("fss tiles: a %s at frame %u was accepted but the session's window cap (%u) was reached, so no window opened. Said at most 8 times.",
                            g.signalPress ? "zoom press" : "camera jump", g.signalFrame, kMaxWindows);
        }
    }
    if (!g.rv.win.open) {
        for (EyeZero& z : g.e0) if (z.valid) releaseEyeZero(z);
    }
    if (g.rv.win.capturing && !g.noTargetNoted && frameNo - g.rv.win.startFrame == 30 && g.targetDispatches == 0) {
        g.noTargetNoted = true;
        Log::get().note("fss tiles: 30 frames into window %u and no dispatch of the tile renderer's shaders has been seen "
                        "(CS %016llX / %016llX); the closing summary lists the other indirect compute shaders.",
                        g.windowNo, static_cast<unsigned long long>(kCsLarge), static_cast<unsigned long long>(kCsSmall));
    }
    fssRingHookWindow(g.rv.win.capturing, g.windowNo, frameNo);
    drainRingSnap(frameNo);
    if (g.summaryDue && (!pendingReadbacks() || frameNo >= g.summaryAfterFrame)) emitSummary();
    refreshActive();
}

bool fssTilesDispatchIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, unsigned int off, uint64_t csHash,
                              FssTilesRealDispatchIndirect real) {
    if (!g.budget.shouldRun() || !args) return false;
    guardedBudget(g.budget, [&] { drainAll(ctx); });
    if (!isTileCs(csHash)) {
        if (g.rv.win.capturing) noteOther(csHash);
        return false;
    }
    if (!g.rv.win.capturing && !(g.lockstep && g.rv.win.open)) return false;
    bool consumed = false;
    g.dispatched = false;
    const bool ok = guardedBudget(g.budget, [&] { consumed = onTarget(ctx, args, off, csHash, real); });
    if (!ok || g.sw.swapped) {
        // A fault inside the substitution: put the second view's bindings back whatever happened, and let the caller
        // forward the dispatch only if ours never reached the driver.
        if (g.sw.swapped) {
            D3dOps ops;
            ops.ctx = ctx;
            swapBack(ops, g.sw);
        }
        consumed = ok ? consumed : g.dispatched;
    }
    refreshActive();
    return consumed;
}

}  // namespace edvr
