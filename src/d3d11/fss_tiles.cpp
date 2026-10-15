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
    WindowMachine win;
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
    uint32_t writes[kWriteCalls][6] = {};   // [call][Role]
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
}

bool pendingReadbacks() {
    for (const FrameBuf& f : g.fb) if (f.used && !f.drained) return true;
    for (const Sample& f : g.smp) if (f.used && !f.drained) return true;
    return false;
}

void refreshActive() {
    detail::g_fssTilesActive =
        g.win.capturing || (g.lockstep && g.win.open) || g.summaryDue || pendingReadbacks();
    detail::g_fssTilesWatch = g.win.capturing && g.watch.n > 0;
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
    if (why != BandWatch::None && g.win.open && !g.windowEventFired && g.autoRuns < kMaxAutoRuns) {
        g.windowEventFired = true;
        ++g.windowEvents;
        ++g.autoRuns;
        g.autoRunPending = true;
        g.afterLeft = kBandAfter;
        Log::get().note("fss tiles: band event at f=%u (%s): eye0 L=%u (row %.2f), eye1 L=%u (row %.2f). The 10 frames before, this one "
                        "and the 60 after are logged as `band` lines and their raw lists written to edvr_logs\\fsstiles; the eye run "
                        "starts at the next frame boundary (automatic run %u of %u this session).",
                        sm.frame, why == BandWatch::AllSmall ? "a large list went to zero: every cell is now the small shader's"
                                                             : "a large list moved by a whole row or more",
                        f.rec.large[0], rowOfLarge(f.rec.large[0]), f.rec.large[1], rowOfLarge(f.rec.large[1]), g.autoRuns,
                        kMaxAutoRuns);
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
    const uint32_t d = sm.frame - g.win.startFrame;
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
        sm.files = g.dumpFramesLeft > 0 || g.frame - g.win.startFrame < 3;
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
}

void drainAll(ID3D11DeviceContext* ctx) {
    drain(ctx);
    drainSamples(ctx);
}

void emitSummary() {
    g.summaryDue = false;
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
            for (uint32_t r = 1; r < 6; ++r) {
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
    ctx->CSGetShaderResources(0, kSrvSlots, sr);
    for (uint32_t i = 0; i < kSrvSlots; ++i) {
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
        r.srvId[i] = g.ids.id(res);
        r.srvKind[i] = slot[i].isBuffer ? 1 : 2;
        r.srvBytes[i] = slot[i].bytes;
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

    ID3D11ShaderResourceView* sr[kSrvSlots] = {};
    Slot slot[kSrvSlots];
    const void* uavRes[2] = {};
    describeBindings(ctx, sr, slot, r, uavRes);
    if (g.win.capturing) registerWatch(r, args, slot, uavRes);

    // ---- the candidate ----
    bool sub = false;
    uint32_t mask = 0;
    bool keepViews = false;   // eye zero's views are kept by the record
    if (g.lockstep && g.win.open) {
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
    if (g.win.capturing) {
        r.substituted = sub;
        queueSample(ctx, r, slot);
    }
    if (g.win.capturing && ensureStaging(ctx)) {
        FrameBuf& fb = g.fb[g.frame % kStagingRing];
        if (!fb.used || fb.frame != g.frame) {
            if (fb.used && !fb.drained) g.dropped += fb.n;
            fb.frame = g.frame;
            fb.used = true;
            fb.drained = false;
            fb.n = 0;
            fb.logFrame = g.win.wantsDetail(g.frame);
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

    if (!keepViews) {
        for (auto& v : sr) {
            if (v) { v->Release(); v = nullptr; }
        }
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
    if (!g.win.capturing || call >= kWriteCalls || !p) return;
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

void fssTilesNoteEyeDump() {
    if (g.autoArming) {
        Log::get().note("fss tiles: automatic eye run started at f=%u (the band event); the eye dump writes to edvr_logs\\eyes and the "
                        "raw lists of the frames around the event are in edvr_logs\\fsstiles.", g.frame);
        return;
    }
    if (!g.win.capturing) {
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

void fssTilesNoteZoomSignal(uint32_t frameNo, bool press) {
    g.signalPending = true;
    g.signalPress = press;
    g.signalFrame = frameNo;
}

void fssTilesFrameBoundary(uint32_t frameNo, bool zoomOpen) {
    g.frame = frameNo;
    g.prevViews = g.views.n;
    g.views.reset();
    g.occ = 0;
    g.zoomOpen = zoomOpen;
    const WindowMachine::Event ev = g.win.step(zoomOpen, frameNo, nowMs());
    if (ev == WindowMachine::Opened) {
        g.windowNo = g.win.windows;
        resetWindowTallies();
        Log::get().note("fss tiles: window %u opened at frame %u (the zoom window; lockstep=%s) -- logging the tile "
                        "renderer's dispatches for up to %llu ms, at most %u lines. Band watch: armed, waiting for a whole-row move of a large list "
                        "or one going to zero (automatic eye run, at most %u a session).",
                        g.windowNo, frameNo, g.lockstep ? "on" : "off", static_cast<unsigned long long>(kWindowMs),
                        kMaxLinesPerWindow, kMaxAutoRuns);
    } else if (ev == WindowMachine::Closed) {
        g.summaryDue = true;
        g.summaryAfterFrame = frameNo + kReadbackDelay + 6;
    }
    if (g.autoRunPending) {
        g.autoRunPending = false;
        g.autoArming = true;
        temporalPassArmEyeDump();   // the eye run, from the Present tick like the key's own press
        g.autoArming = false;
    }
    if (g.signalPending) {
        g.signalPending = false;
        if (!g.win.open && g.noWindowNotes < 6) {
            ++g.noWindowNotes;
            const char* why;
            if (!journalFssFocusKnown()) why = "the journal's GuiFocus has not been read (Status.json unknown)";
            else if (!journalFssFocus()) why = "the journal says the game is not in the FSS (GuiFocus is not 9)";
            else if (g.win.windows >= kMaxWindows) why = "the session's window cap was reached";
            else why = "the arrival window did not open for a reason this module cannot see";
            Log::get().note("fss tiles: a zoom was signalled (%s at frame %u) but no window opened: %s; the FSS key watcher is %s. "
                            "Said at most 6 times.",
                            g.signalPress ? "your zoom press" : "a camera jump with the scanner up", g.signalFrame, why,
                            eyeSyncFromConfig(Config::get()).needsFssSignals() ? "running" : "NOT running (fix.fss_eye_sync is off)");
        }
    }
    if (!g.win.open) {
        for (EyeZero& z : g.e0) if (z.valid) releaseEyeZero(z);
    }
    if (g.win.capturing && !g.noTargetNoted && frameNo - g.win.startFrame == 30 && g.targetDispatches == 0) {
        g.noTargetNoted = true;
        Log::get().note("fss tiles: 30 frames into window %u and no dispatch of the tile renderer's shaders has been seen "
                        "(CS %016llX / %016llX); the closing summary lists the other indirect compute shaders.",
                        g.windowNo, static_cast<unsigned long long>(kCsLarge), static_cast<unsigned long long>(kCsSmall));
    }
    if (g.summaryDue && (!pendingReadbacks() || frameNo >= g.summaryAfterFrame)) emitSummary();
    refreshActive();
}

bool fssTilesDispatchIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, unsigned int off, uint64_t csHash,
                              FssTilesRealDispatchIndirect real) {
    if (!g.budget.shouldRun() || !args) return false;
    guardedBudget(g.budget, [&] { drainAll(ctx); });
    if (!isTileCs(csHash)) {
        if (g.win.capturing) noteOther(csHash);
        return false;
    }
    if (!g.win.capturing && !(g.lockstep && g.win.open)) return false;
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
