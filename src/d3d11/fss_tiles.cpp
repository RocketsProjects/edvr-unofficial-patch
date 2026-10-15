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
}

bool pendingReadbacks() {
    for (const FrameBuf& f : g.fb) if (f.used && !f.drained) return true;
    return false;
}

void refreshActive() {
    detail::g_fssTilesActive =
        g.win.capturing || (g.lockstep && g.win.open) || g.summaryDue || pendingReadbacks();
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
void describeBindings(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView** sr, Slot* slot, Rec& r) {
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
    describeBindings(ctx, sr, slot, r);

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
                        "(fix.fss_eye_sync = tiles) is %s. A session with no 'window opened' line below never reached a "
                        "zoom window.",
                        kMaxWindows, static_cast<unsigned long long>(kCsLarge), static_cast<unsigned long long>(kCsSmall),
                        want ? "ON" : "off");
    }
    refreshActive();
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
                        "renderer's dispatches for up to %llu ms, at most %u lines.",
                        g.windowNo, frameNo, g.lockstep ? "on" : "off", static_cast<unsigned long long>(kWindowMs),
                        kMaxLinesPerWindow);
    } else if (ev == WindowMachine::Closed) {
        g.summaryDue = true;
        g.summaryAfterFrame = frameNo + kReadbackDelay + 6;
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
    guardedBudget(g.budget, [&] { drain(ctx); });
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
