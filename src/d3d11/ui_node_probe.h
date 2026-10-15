#pragma once
// An OBSERVE-ONLY probe on the engine's UI node (build 332841): where, among the D3D draws EDVR sees, does the game call the routine that
// composites its interface? docs/ui-layer-2026-09-23.md, "2026-10-14: the panel data-flow census", the engine findings:
//
//   CONFIRMED (offline, Ghidra 12.1.3 on the Epic exe, SHA E6BE8BBE...988): the per-view pipeline attaches the UIPipelineFeature's node to
//   NodeGroup::GUI (attach thunk exe+0x28A9C30). That node is an instance of the class with vtable exe+0x52EA038 (constructed by
//   exe+0x27F37B0 from the view setup exe+0x28A4D30, handed the ScaleformRenderQueue). Its vtable slot 5, at exe+0x52EA060, holds
//   exe+0x285A540.
//   INFERRED (decompiled, not traced): that routine is the node's per-frame composite (it issues a draw through exe+0x140774D70 with the
//   queue's output as the source); whether it runs on the thread that issues the eye draws, and where it falls among them, is what this probe
//   measures.
//
// The probe swaps ONE data pointer (the vtable slot) for a forwarder that counts and calls the original: it patches no code. It is pinned
// by identity and prologue as the cull cover is: the PE stamp and image size of build 332841, the slot holding exactly the function's
// address, and the function's first 29 bytes. Anything else is ONE line (`ui node probe: inert: <why>`) and nothing is patched, once. It
// moves nothing and decides nothing; the layer never reads it.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace unode {

constexpr uint32_t kExpectedTimestamp = 1788384820u;   // build 332841's PE stamp and image size (the pair every build-keyed hook checks)
constexpr uint32_t kExpectedImageSize = 104894464u;
constexpr uint32_t kVtableRva = 0x52EA038u;            // the UI node class's primary vtable
constexpr uint32_t kSlot = 5;                          // the slot
constexpr uint32_t kSlotRva = kVtableRva + 8u * kSlot; // 0x52EA060
constexpr uint32_t kFunctionRva = 0x285A540u;          // the routine it holds
// push rbp / push r15 / sub rsp,88h / mov [rsp+80h],rsi / mov rsi,rcx / mov rcx,[rcx+0B0h] (the ScaleformRenderQueue pointer)
constexpr size_t kHeadBytes = 29;
constexpr uint8_t kHead[kHeadBytes] = {0x40, 0x55, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB4, 0x24,
                                       0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x89, 0xB0, 0x00, 0x00, 0x00};

struct Located {
    uint32_t stamp = 0;
    char why[260] = {};
};

inline uint32_t load32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint16_t load16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

// base: the module's load address. imageSize: from the header. The reads are inside the image once the header's SizeOfImage is checked.
inline bool locate(const uint8_t* base, Located* out) {
    *out = Located{};
    if (!base) { std::snprintf(out->why, sizeof(out->why), "the game's module could not be resolved"); return false; }
    const uint32_t peOff = load32(base + 0x3C);
    if (peOff < 0x40 || peOff > 0x300 || std::memcmp(base + peOff, "PE\0\0", 4) != 0 || load16(base + peOff + 24) != 0x20B) {
        std::snprintf(out->why, sizeof(out->why), "the module is not a PE32+ image");
        return false;
    }
    out->stamp = load32(base + peOff + 8);
    const uint32_t imageSize = load32(base + peOff + 0x50);
    if (out->stamp != kExpectedTimestamp || imageSize != kExpectedImageSize) {
        std::snprintf(out->why, sizeof(out->why), "the PE stamp %u and image size %u are not build 332841's (%u, %u); the node's routine is pinned to that build",
                      out->stamp, imageSize, kExpectedTimestamp, kExpectedImageSize);
        return false;
    }
    const uint64_t want = reinterpret_cast<uint64_t>(base) + kFunctionRva;
    const uint64_t have = load64(base + kSlotRva);
    if (have != want) {
        std::snprintf(out->why, sizeof(out->why), "the UI node class's slot %u at exe+0x%X holds %llX, not exe+0x%X (%llX)", kSlot, kSlotRva,
                      static_cast<unsigned long long>(have), kFunctionRva, static_cast<unsigned long long>(want));
        return false;
    }
    if (std::memcmp(base + kFunctionRva, kHead, kHeadBytes) != 0) {
        std::snprintf(out->why, sizeof(out->why), "the routine at exe+0x%X does not start with the bytes read in the disassembly", kFunctionRva);
        return false;
    }
    return true;
}

}  // namespace unode

// Install the probe (once; later calls do nothing). Called from device_hook after the other startup installers.
void uiNodeProbeInstall();

// The probe's census clause, whole, for the "ui boundary cost" line: live with its counts and where the last call fell, inert with its
// reason, or not attempted. Never empty: a line without it is a build that never carried the probe.
void uiNodeProbeClause(char* out, size_t cap);

#ifdef EDVR_UI_NODE_PROBE_TEST
// The rig's seams: install against a synthetic image, read the counters, start over.
void uiNodeProbeTestSetImage(const uint8_t* base);
void uiNodeProbeTestReset();
bool uiNodeProbeTestInstalled();
bool uiNodeProbeTestAttempted();
uint64_t uiNodeProbeTestCalls();
#endif

}  // namespace edvr
