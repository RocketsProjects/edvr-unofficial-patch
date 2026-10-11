// The edge-black watch, the GPU half (docs\terrain-culling.md, 2026-10-10; TEMPORARY, an always-on instrument of this test build).
//
// Pilots see quick black flashes, square, at the outer corners of an eye over flat planet ground. Is the black IN the image Elite hands EDVR, or added
// after EDVR submits? This samples the texture Elite submits -- the stage the luma probe calls "game" -- every frame, both eyes, on a 128 x 128 grid, and
// judges it for transient black (edge_black_core.h has the rule and every line it writes). 128 and not 64 so that a black shape two grid cells across (the
// least that can make an event of three touching points) is about 1.5 degrees on a 4508 px, 48 px/deg eye; a 64 grid left anything under 3 degrees uncounted.
//
// COST. Per eye per frame, on the render thread: one dispatch of 16 x 16 thread groups of 8 x 8 (16384 single-texel loads and 16384 four-byte stores: a few
// microseconds of GPU), one 64 KB copy into a staging buffer of a ring four frames deep, one non-blocking Map of the oldest ready buffer (64 KB read, never
// waited on: a buffer not yet ready is left for the next frame), and the pure judgement of the grid it returned (three passes over 16384 bytes: about 25 to 27
// microseconds measured in tools\edge_black_test). About a dozen D3D calls; the periodic-work line "periodic work: edge_black" in the graphics log says what
// the whole of it measured in a flight. No full-eye copy, no device wait, no query. The ring is the only memory it keeps (64 KB x 4 x 2 staging, 64 KB x 2 output).
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace edvr {

// Called by native_temporal.cpp's treat(), once per eye per frame, after the submit's own validation and before any EDVR pass touches the texture.
// `region` is the part of the texture the frame was rendered into (left, top, right, bottom in texels), `flipU`/`flipV` whether the submit's bounds mirror
// it, `seq` the frame's sequence number, `head34` the frame's head pose (row-major 3x4, device to world) or null when none is known. Never blocks, never
// waits on the device, never fails the submit: a texture it cannot sample is said once and left alone.
void edgeBlackSample(ID3D11Device* device, ID3D11Texture2D* tex, int eye, uint64_t seq, const uint32_t region[4], bool flipU, bool flipV, const float* head34);

// The run is ending (the native temporal channel closing): the open window and the totals are written.
void edgeBlackClose();

}  // namespace edvr
