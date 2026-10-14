#pragma once
// The scene inspector (docs/scene-inspector-2026-10-14.md): a developer tool. Look at something that is not behaving under DLSS, press
// hotkey.scene_inspect (unbound by default), and the log, a CSV and a few PNGs say which draws wrote the pixels at the centre of the
// left eye's view, what EDVR did with each, what the temporal pass handed DLSS there, and which rule explains the smear.
//
// One press captures ONE frame. Every eye draw of the left eye is duplicated once more, scissored to a 16x16 box and then to its centre
// pixel, writing nothing (colour mask 0, depth and stencil writes off), each under an occlusion query. The duplicate runs BEFORE the
// game's own draw, so its count is what the draw adds over the earlier ones. After the capture frame the answers are read back, and the
// temporal pass's offer of its inputs (colour, depth, the motion handed to DLSS, the decision texture, the masks) and of DLSS's output
// is staged in the same frame and read over the following boundaries.
//
// Cost: unarmed, each eye draw pays one relaxed atomic load (sceneInspectWantsDraws), the temporal pass two per frame (the inputs), the
// frame boundary one. Nothing else runs; no state is created, no hook is installed at press time.
#include <cstdint>
#include <string>

struct ID3D11BlendState;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// What the module needs of the rest of the DLL, given once at install, so that it links alone into its own test rig.
struct SceneInspectEnv {
    int (*eyeOfTarget)(const void* resource) = nullptr;   // 0 or 1; -1 when the pass's table does not know the target
    // The eye's advertised frustum {left, right, down, up} (tangents, y up) and its rotation to the head (row-major 3x3, -z forward). False when
    // the runtime has not published a frame; eyeToHead is then left untouched.
    bool (*eyeGeometry)(uint32_t eye, float frustum[4], float eyeToHead[9]) = nullptr;
    // The blend state past the binding shadow (vScreenOMSetBlendStateRaw): the duplicate is not the game's bind.
    void (*setBlendRaw)(ID3D11DeviceContext* ctx, ID3D11BlendState* state, const float blendFactor[4], uint32_t sampleMask) = nullptr;
    std::wstring (*logDir)() = nullptr;
    const char* (*iniName)() = nullptr;
};

// Once, at install: said in the log (bound or not), and the environment kept.
void sceneInspectInstall(bool flatProfile, bool keyBound, const char* key, const SceneInspectEnv& env);
// A press: arms a capture at the next frame boundary. Refused (and said) while one runs, or in the flat profile.
void sceneInspectRequest(const char* trigger);
// vScreenFrameBoundary: starts, advances and finishes the capture.
void sceneInspectFrameBoundary(uint32_t gameFrame) noexcept;

// The draw path's gate, one relaxed load: a capture is collecting its frame's draws.
bool sceneInspectWantsDraws() noexcept;

// What the draw path knows about one eye draw by the time its routing is decided (vscreen.cpp fills it; the rig fills it by hand).
struct SceneDrawInfo {
    char kind = '?';               // D, I, N or X
    uint32_t count = 0, instances = 1;
    uint32_t ordinal = 0;          // the draw's place among the frame's eye draws (both eyes counted)
    uint32_t gameFrame = 0;
    uint64_t vs = 0, ps = 0;
    const char* verdict = "";      // EDVR's DrawVerdict name, one word
    int uiFamily = 0;              // UiLayerFamily as an int, 0 none
    bool uiTaken = false;
    bool uiDecided = false;
    int uiDecision = 0;            // UiLayerDecision as an int, meaningful when uiDecided
    bool udCoverage = false;       // ui_depth chose a coverage shader for the draw
    bool udWantMask = false;       // ...and the reissue marks the reactive mask
    int udMaskSlot = 0;
    bool holoMatched = false;
    bool holoWorld = false;
    bool engineSubstituted = false;
};
using SceneReplayFn = void (*)(void* arg);   // re-issues the draw through the real entry point, below every EDVR hook
// Offer one eye draw of the collecting frame. Safe to call for any draw. Every state it touches is put back before it returns.
void sceneInspectDraw(ID3D11DeviceContext* ctx, const SceneDrawInfo& info, SceneReplayFn replay, void* arg) noexcept;
void sceneInspectNoteIndirect() noexcept;   // an indirect draw: counted, never tested
// A ClearRenderTargetView of a target (its resource): draws before it, into that target, are not on screen and leave the stacks.
void sceneInspectNoteClear(const void* resource) noexcept;

// ---- the temporal pass's side ----------------------------------------------------------------------------------------------------------

// The pass asks per eye per frame: a capture is waiting for eye 0's inputs.
bool sceneInspectInputsWanted() noexcept;
struct SceneInputsIn {
    ID3D11Texture2D* colour = nullptr;      // R8G8B8A8_UNORM: DLSS's colour input
    ID3D11Texture2D* motion = nullptr;      // R16G16_FLOAT: the motion vectors DLSS is handed
    ID3D11Texture2D* depth = nullptr;       // R32_FLOAT: the pass's depth copy
    ID3D11Texture2D* decision = nullptr;    // R32G32B32A32_FLOAT: physical motion xy, predicted depth, flags (the capture-only shader variant)
    ID3D11Texture2D* coverage = nullptr;    // R8_UNORM: the UI coverage mask; null when nothing was marked
    ID3D11Texture2D* bias = nullptr;        // R8_UNORM: the bias-current-colour mask; null when not built
    uint32_t w = 0, h = 0;                  // the inputs' size
    uint32_t sceneFrame = 0;                // the pass's own frame number
    // Which game resources DLSS relates to, for naming the draws' targets: the texture the game submitted (DLSS's colour input) and the scene
    // depth texture the pass reads. Compared by address only. Both null: not known, and no target is called an intermediate pass.
    const void* colourSource = nullptr;
    const void* sceneDepth = nullptr;
    // The pass's camera term (the constant buffer's rows): tanNow/tanPrev {l r down up}, the world path's rotation rows and translation
    // (tvCam.w 1 = on), the head's, and the depth encoding (knobs.x, knobs.z). The jitter is the output's (SceneOutputsIn).
    float tanNow[4] = {}, tanPrev[4] = {};
    float worldRows[3][4] = {}, headRows[3][4] = {};
    float tvCam[4] = {}, tvUsed[4] = {}, knobs[4] = {};
};
void sceneInspectInputsOffer(ID3D11DeviceContext* ctx, const SceneInputsIn& in) noexcept;
void sceneInspectInputsRefuse(const char* why) noexcept;
struct SceneOutputsIn {
    ID3D11Texture2D* dlss = nullptr;        // DLSS's output (before the UI resolve)
    ID3D11Texture2D* final = nullptr;       // what goes to the headset (after the UI resolve)
    uint32_t w = 0, h = 0, outW = 0, outH = 0;
    float jx = 0.0f, jy = 0.0f;
    bool reset = false;
    const char* resetWhy = "";
    bool reactiveHanded = false;
    float frameMs = 0.0f;
    bool uiResolved = false;
    unsigned preset = 0;
    float preExposure = 1.0f, exposureScale = 1.0f;
    bool exposureTexture = false, hdr = false;
};
void sceneInspectOutputsOffer(ID3D11DeviceContext* ctx, const SceneOutputsIn& in) noexcept;

}  // namespace edvr
