#include "../../src/d3d11/plugin_admission_transport.h"
#include "../../src/d3d11/plugin_selection_transport.h"
#include "../../src/common/native_graphics.h"
#include "../../src/common/graphics_bridge.h"
#include "../../src/common/render_boundary.h"
#include "../../src/common/runtime_profile.h"

#include <atomic>
#include <thread>

namespace {
INIT_ONCE fixtureInitOnce = INIT_ONCE_STATIC_INIT;
HANDLE initEntered = nullptr;
HANDLE releaseInit = nullptr;
HANDLE initThread = nullptr;
std::atomic<unsigned> pairCalls{0};

BOOL CALLBACK heldInit(PINIT_ONCE, PVOID, PVOID*) {
    if (initEntered) SetEvent(initEntered);
    if (releaseInit) WaitForSingleObject(releaseInit, INFINITE);
    return TRUE;
}
DWORD WINAPI enterHeldInit(void*) {
    InitOnceExecuteOnce(&fixtureInitOnce, heldInit, nullptr, nullptr);
    return 0;
}
}

extern "C" __declspec(dllexport) BOOL WINAPI FixtureBeginHeldInit() noexcept {
    if (!initEntered) initEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!releaseInit) releaseInit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!initEntered || !releaseInit) return FALSE;
    initThread = CreateThread(nullptr, 0, enterHeldInit, nullptr, 0, nullptr);
    if (!initThread) return FALSE;
    return WaitForSingleObject(initEntered, 2000) == WAIT_OBJECT_0;
}
extern "C" __declspec(dllexport) void WINAPI FixtureReleaseHeldInit() noexcept {
    if (releaseInit) SetEvent(releaseInit);
    if (initThread) {
        WaitForSingleObject(initThread, INFINITE);
        CloseHandle(initThread);
        initThread = nullptr;
    }
}
extern "C" __declspec(dllexport) unsigned WINAPI FixturePairCalls() noexcept {
    return pairCalls.load();
}
extern "C" __declspec(dllexport) BOOL WINAPI FixtureOwnerAllowedForStartup(
    std::uint32_t generatedGroupIndex) noexcept {
    return edvr::plugin_admission_transport::ownerAllowedForStartup(
        generatedGroupIndex) ? TRUE : FALSE;
}
extern "C" __declspec(dllexport) HRESULT WINAPI FixturePublishCandidate(
    const wchar_t* moduleDirectory, HMODULE graphics) noexcept {
    if (!moduleDirectory) return E_INVALIDARG;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    const std::wstring directory(moduleDirectory);
    const HRESULT capture = edvr::plugin_selection_transport::captureFromModuleDirectory(directory);
    edvr::plugin_admission_transport::publishCandidate(directory, graphics,
        edvr::plugin_selection::PluginSelectionProfileV1::Vr);
    return capture;
}
extern "C" __declspec(dllexport) HRESULT WINAPI FixturePublishFlatCandidate(
    const wchar_t* moduleDirectory, HMODULE graphics) noexcept {
    if (!moduleDirectory) return E_INVALIDARG;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    const std::wstring directory(moduleDirectory);
    const HRESULT capture = edvr::plugin_selection_transport::captureFromModuleDirectory(directory);
    edvr::plugin_admission_transport::publishCandidate(directory, graphics,
        edvr::plugin_selection::PluginSelectionProfileV1::Flat);
    return capture;
}
extern "C" HRESULT WINAPI edvrAcquireNativeGraphics(
    const EdvrNativeGraphicsRequest*, EdvrNativeGraphicsTable*) {
    ++pairCalls;
    return E_FAIL;
}
extern "C" HRESULT WINAPI edvrAcquireGraphicsBridge(
    const EdvrGraphicsBridgeRequest*, EdvrGraphicsBridgeTable*) {
    ++pairCalls;
    return E_FAIL;
}
extern "C" HRESULT WINAPI edvrAcquireRenderBoundary(
    const EdvrRenderBoundaryRequest*, EdvrRenderBoundaryTable*) {
    ++pairCalls;
    return E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        if (initEntered) CloseHandle(initEntered);
        if (releaseInit) CloseHandle(releaseInit);
        if (initThread) CloseHandle(initThread);
    }
    return TRUE;
}
