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
}  // namespace detail

typedef void(__stdcall* FssTilesRealDispatchIndirect)(ID3D11DeviceContext*, ID3D11Buffer*, unsigned int);

// fix.fss_eye_sync = tiles. Read at install and on the ini reload path.
void fssTilesConfigure(Config& cfg);

// Once per frame at the boundary. `zoomOpen` is vscreen's arrival window (journal-gated, 600 frames from the zoom).
void fssTilesFrameBoundary(uint32_t frameNo, bool zoomOpen);

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

// The eye-dump key was pressed (temporal_pass.cpp, temporalPassArmEyeDump): inside a window, write the raw lists and tile-info
// buffers of the next sixteen frames to edvr_logssstiles, beside the eye run, so the dump's black tiles can be read against them.
void fssTilesNoteEyeDump();

// A zoom was signalled (the player's zoom press, or a camera jump while the scanner's chrome is up) on this frame. If no
// window is open by the frame boundary, the module says why, at most 6 times a session.
void fssTilesNoteZoomSignal(uint32_t frameNo, bool press);

// A zoom window is open, a read-back is outstanding, or a summary is owed: the DispatchIndirect hook must call in.
inline bool fssTilesActive() { return detail::g_fssTilesActive; }

// Owner-context DispatchIndirect, with the bound compute shader's content hash. Returns true when this call issued the
// dispatch itself (the tiles candidate drew it from the first view's buffers); false means the caller forwards it.
bool fssTilesDispatchIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, unsigned int off, uint64_t csHash,
                              FssTilesRealDispatchIndirect real);

}  // namespace edvr
