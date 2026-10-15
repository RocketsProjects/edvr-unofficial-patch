// The `fss tiles` instrument and the tiles candidate (fix.fss_eye_sync = tiles): the Full System Scanner's tile
// renderer, per view. The pure half and the story are in fss_tiles_core.h; this is the hook glue.
//
// Called from exposure_fix.cpp's DispatchIndirect hook (the one place that already owns slot 42) and from vscreen's
// frame boundary. Free outside an FSS zoom window: the hook asks one bool.
#pragma once

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DeviceContext;

namespace edvr {

class Config;

namespace detail {
extern bool g_fssTilesActive;
extern bool g_fssTilesWatch;   // a window is capturing and has buffers to watch
extern bool g_fssTilesDraws;   // ... and has the per-eye textures of the tile dispatch to count draws into
extern bool g_fssRevealWatch;  // VR profile, the scanner is on show: the reveal fix looks at DrawInstanced calls
}  // namespace detail

typedef void(__stdcall* FssTilesRealDispatchIndirect)(ID3D11DeviceContext*, ID3D11Buffer*, unsigned int);

// fix.fss_eye_sync = tiles. Read at install and on the ini reload path.
void fssTilesConfigure(Config& cfg);

// Once per frame at the boundary. `chromeAgo` is the frames since the scanner's chrome last matched (0xFFFFFFFF: never), the journal's FSS focus is
// `focusKnown` / `focus`. The module's own gate (fss_tiles_core.h ZoomGate) decides from them whether a zoom window is open.
void fssTilesFrameBoundary(uint32_t frameNo, uint32_t chromeAgo, bool focusKnown, bool focus);

// The instrument is armed in every VR session, so the fast Status.json poll, the zoom watchers (gamepad included) and the chrome tracker run whatever
// fix.fss_eye_sync is set to (flight e05b6be6: under `off` they did not, and the window opened on the FSS exit).
bool fssTilesWantsSignals();
bool fssTilesWantsChrome();

// Record-only: a write to one of the window's buffers (the tile renderer's argument buffer, outputs, lists and tile-info
// buffer). `call` indexes fsstiles::writeCallName. Owner context only (the caller tests foreignContext). One bool when no
// window is watching.
void fssTilesNoteWriteSlow(const void* resourceOrView, bool isView, uint32_t call);
inline void fssTilesNoteWrite(const void* resource, uint32_t call) {
    if (detail::g_fssTilesWatch) fssTilesNoteWriteSlow(resource, false, call);
}
inline void fssTilesNoteWriteView(const void* view, uint32_t call) {
    if (detail::g_fssTilesWatch) fssTilesNoteWriteSlow(view, true, call);
}

// Record-only: a draw call on the owner context. Counts it when the bound render target is one of the window's per-eye input
// textures (SRV slots 3 and 4 of the tile dispatch), with the viewport and scissor of the first and last sampled draw.
void fssTilesNoteDrawSlow(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances);
inline void fssTilesNoteDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances) {
    if (detail::g_fssTilesDraws) fssTilesNoteDrawSlow(ctx, kind, count, instances);
}

// The eye-dump key was pressed (temporal_pass.cpp, temporalPassArmEyeDump): inside a window, write the raw lists and tile-info
// buffers of the next sixteen frames to edvr_logssstiles, beside the eye run, so the dump's black tiles can be read against them.
void fssTilesNoteEyeDump();

// A camera jump or the player's zoom press on this frame, with the frames since the scanner's chrome last matched. The gate accepts it only while the chrome
// is held; a refused one is logged with its reason.
void fssTilesNoteZoomSignal(uint32_t frameNo, bool press, uint32_t chromeAgo);

// The reveal fix (flight d7769063): the left eye's reveal composite reads a 63 x 63 downsample of the ring layer drawn before the ring's second draw group. At the first reveal composite
// of a frame, the frame's first downsample into that same texture is replayed from the finished layer (fss_tiles_replay.h), so the left t1 matches the right one. The game's own un-hooked
// entry points are passed so the replay is invisible to the hooks (any may be null). Called for every DrawInstanced on the owner context, VR profile only; one bool when the scanner is not
// on show. The decision is fsstiles::RevealReplay (fss_tiles_core.h).
struct FssRealCalls {
    void* vsSetCb = nullptr; void* psSetCb = nullptr; void* psSetSrv = nullptr; void* vsSetSrv = nullptr; void* vsSetShader = nullptr; void* psSetShader = nullptr;
    void* omSetRts = nullptr; void* omSetBlend = nullptr; void* rsSetViewports = nullptr; void* drawInstanced = nullptr;
};
void fssRevealOnDrawSlow(ID3D11DeviceContext* ctx, const FssRealCalls& calls, uint32_t perInstance, uint32_t instances, uint32_t startVertex, uint32_t startInstance);
inline bool fssRevealWatching() { return detail::g_fssRevealWatch; }

// A zoom window is open, a read-back is outstanding, or a summary is owed: the DispatchIndirect hook must call in.
inline bool fssTilesActive() { return detail::g_fssTilesActive; }

// Owner-context DispatchIndirect, with the bound compute shader's content hash. Returns true when this call issued the
// dispatch itself (the tiles candidate drew it from the first view's buffers); false means the caller forwards it.
bool fssTilesDispatchIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, unsigned int off, uint64_t csHash,
                              FssTilesRealDispatchIndirect real);

}  // namespace edvr
