#pragma once
// The headset camera tag's hook (src\common\hmd_camera_tag.h says what and why): a CodeHook on the camera coordinator, EliteDangerous64.exe+0x594B60, build 332841.
// VR only: a flat session says it is inert and patches nothing. Any build but 332841's, or a prologue or call-site miss, is one log line and nothing patched.
#include <cstddef>
#include <cstdint>

namespace edvr {

// Installs the hook once (later calls do nothing). Called from device_hook beside the other engine hooks. flatProfile true: says it is inert.
void hmdCameraTagInstall(bool flatProfile);

// The frame the chooser is on (temporal_pass's frame boundary): the detour records it with each call.
void hmdCameraTagNoteFrame(uint32_t frame);

// The hook's state, for the 30 s line. live false: why says why it is inert.
struct HmdCameraTagState {
    bool live = false;
    char why[200] = {};
    uint64_t lastC = 0;        // the last detour call's coordinator pointer (rcx)
    uint32_t lastThread = 0;   // ...its thread id
    uint32_t lastFrame = 0;    // ...the frame the chooser was on
};
HmdCameraTagState hmdCameraTagState();

// The coordinator calls the detour has seen since the last take (the 30 s line's window). Takes and clears.
uint64_t hmdCameraTagTakeCalls();

}  // namespace edvr
