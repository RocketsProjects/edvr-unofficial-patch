#pragma once
// The terrain cull cover: a CodeHook at the entry of Frontier's cull-plane builder (EliteDangerous64.exe+0x443E800, build 332841) that widens the vertical fov and aspect it is
// handed to the largest half-angles of the frustum EDVR hands the game. The mechanism, the offsets and the evidence are in terrain_cull_cover_core.h and docs/terrain-culling.md;
// the rig is tools\terrain_cover_test.
//
// TEMPORARY (advanced.terrain_cull_cover = on | off, default on, live within a second): the arc's fix, and the switch for a flight's off/on comparison against the edge-black watch.
// Installed ONCE at startup in the VR profile (a flat session says it is inert and patches nothing). A build whose PE stamp is not 332841's is scanned for the call site; anything
// short of one exact match is ONE log line (`terrain cull cover: inert: <why>`) and the hook stays out: never retried.
#include <cstddef>
#include <cstdint>

namespace edvr {

// Install the hook (once; later calls do nothing). Called from device_hook after the other startup installers. `flatProfile` true: say it is inert and patch nothing.
void terrainCullCoverInstall(bool flatProfile);

// Once a second from device_hook's config tick, after the reload: the live key, and the 60 s census (written while the hook is live, calls or none). `nowMs` is a monotonic
// millisecond clock.
void terrainCullCoverPoll(uint64_t nowMs);

// A FreeLibrary teardown: the open window is written, if the hook is live. The game never gets here (it never unloads this DLL).
void terrainCullCoverShutdown();

// The frusta handed to the game for this frame ({left, right, top, bottom} tangents per eye), from the native temporal channel's beginFrame. Publishes T and H for the detour.
// Cheap, thread-safe; a frame that is not finite and non-zero leaves the last known pair alone.
void terrainCullCoverNoteFrusta(const float frusta[2][4]);

#ifdef EDVR_TERRAIN_COVER_TEST
// The rig's seams: install against a synthetic module image instead of the game's, take the key from a string instead of the config, read what the instruments said, and start over
// (uninstalling the hook from the synthetic function). Production never defines this.
struct TerrainCoverTestState {
    bool attempted = false, installed = false, on = false, byStamp = false;
    uint64_t calls = 0, widened = 0;
    uint32_t callLines = 0;
};
void terrainCullCoverTestSetImage(const uint8_t* base);   // the module base to inspect (null: the game's)
void terrainCullCoverTestSetKey(const char* text);        // the key's value (null: read the config)
void terrainCullCoverTestReset();                         // uninstall, forget the attempt, the counts, the tangents and the lines
TerrainCoverTestState terrainCullCoverTestState();
size_t terrainCullCoverTestLineCount();
const char* terrainCullCoverTestLine(size_t i);
#endif

}  // namespace edvr
