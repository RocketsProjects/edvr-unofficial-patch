// A module the render-thread CPU sampler's rig keeps busy, so the sampler has known hot functions to find in a module that
// is not the test's own executable (the way the game's frames sit in EliteDangerous64.exe and the drivers sit in theirs).
//
// hot_spin_hot is the hot leaf: three of every four calls of hot_outer, so about three quarters of the busy time is in it.
// hot_spin_cold is the cold leaf. hot_outer keeps its frame and calls both, so it is on the stack of every busy sample.
// Build: cl /LD.
#include <windows.h>

#pragma optimize("", off)

extern "C" __declspec(dllexport) __declspec(noinline) void hot_spin_hot() {
    // A local array so the function has a frame of its own, and so it has unwind data the sampler can look up.
    volatile char pad[64];
    pad[0] = 0;
    for (volatile unsigned i = 0; i < 120000u; ++i) pad[0] = static_cast<char>(pad[0] + 1);
}

extern "C" __declspec(dllexport) __declspec(noinline) void hot_spin_cold() {
    volatile char pad[64];
    pad[0] = 0;
    for (volatile unsigned i = 0; i < 40000u; ++i) pad[0] = static_cast<char>(pad[0] + 1);
}

extern "C" __declspec(dllexport) __declspec(noinline) void hot_outer() {
    volatile char pad[64];
    pad[0] = 0;
    hot_spin_hot();
    hot_spin_hot();
    hot_spin_hot();
    hot_spin_cold();
    pad[0] = 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
