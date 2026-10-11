// The occlusion census (occlusion_census_core.h has the numbers): a temporary measurement of how much of the scene Elite
// draws that the depth buffer has already hidden. Question: in a station interior, does Elite still draw ships, hull and
// planet that nothing visible needs? Four frames of every scene draw are bracketed with an occlusion query each; a draw
// whose query reports ZERO samples was fully hidden at the point Elite issued it.
//
// Two diagnostic keys, both temporary (edvr.ini [hotkey] and [diag]):
//
//   hotkey.occlusion_census   a key, empty is off (the default in VR and flat). A press arms one capture; it records the
//                             next four frames from the next frame tick.
//   diag.occlusion_census     off | on (default off). `on` arms one capture 20 s after the first in-scene frame (VR: the
//                             shared scene flag; flat: the flat runtime's scene size), so a remote run needs no key press.
//
// Off costs nothing per draw: the real draw pointers are the game's own trampolines except while a capture records
// (occlusionCensusInstallWrappers swaps them in for those four frames only). The report and the per-draw CSV are written
// after the read-back, to the graphics log ("occlusion census:" lines) and edvr_logs\occlusion_census_<stamp>.csv.
#pragma once

#include "occlusion_census_core.h"

struct ID3D11DeviceContext;

namespace edvr {

// ensureState, after the diagnostic keys are bound: reads diag.occlusion_census and says, once, what is armed.
void occlusionCensusConfigure();

// A press of hotkey.occlusion_census (tickHotkeys). Refused, and said so, while a capture is already waiting or running.
void occlusionCensusRequest(const char* trigger);

// tickHotkeys, once a frame: starts a requested capture, counts its frames, reads its queries back, and writes the report.
void occlusionCensusFrameTick();

// The draw wrappers' calls (vscreen.cpp): Begin before the real draw, End after it. `scene` is the caller's answer to
// "does this draw land on the scene's colour target". Both do nothing unless a capture is recording.
void occlusionCensusBegin(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, bool scene,
                          occlusion::Ticket* t);
void occlusionCensusEnd(ID3D11DeviceContext* ctx, const occlusion::Ticket& t);

// vscreen.cpp: points the real draw pointers at the census wrappers (on) or back at the game's own (off).
void occlusionCensusInstallWrappers(bool on);

}  // namespace edvr
