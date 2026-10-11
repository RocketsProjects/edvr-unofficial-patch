#include "../../src/common/plugin_admission_commit.h"

#include <atomic>
#include <cstring>

namespace {
using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;
using namespace edvr::plugin_selection::commit_v1;

const std::uint8_t fingerprint[32] = {0x29, 0x10, 0x77};
test_seam::GroupV1 groups[] = {{0, 3, 3}, {1, 1, 1}, {2, 2, 2}};
test_seam::CatalogViewV1 catalog{groups, 3, 2, fingerprint};
std::atomic<unsigned> commitCalls{0};
HMODULE expectedGraphics = nullptr;

bool loadedExpected(void*, std::uint64_t base) noexcept {
    return reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(base)) == expectedGraphics;
}
bool hasOwnQuery(void*, std::uint64_t base) noexcept {
    auto module = reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(base));
    const auto proc = GetProcAddress(module, "edvrQueryPluginAdmissionV1");
    HMODULE owner = nullptr;
    return proc && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(proc), &owner) && owner == module;
}
HRESULT queryContract(void*, std::uint64_t base, GraphicsContractV1* output) noexcept {
    if (!output) return E_INVALIDARG;
    const auto module = reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(base));
    using Query = HRESULT (WINAPI *)(std::uint32_t, std::uint32_t, std::uint32_t, void*);
    const auto query = reinterpret_cast<Query>(GetProcAddress(module, "edvrQueryPluginAdmissionV1"));
    if (!query) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return query(1, 1, sizeof(*output), output);
}
bool retainGraphics(void*, std::uint64_t base) noexcept {
    HMODULE retained = nullptr;
    if (!base || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(base)), &retained)) return false;
    if (retained != expectedGraphics) {
        if (retained) FreeLibrary(retained);
        return false;
    }
    return true;
}
void releaseGraphics(void*, std::uint64_t base) noexcept {
    if (base) FreeLibrary(reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(base)));
}
PeerHooksV1 hooks{nullptr, hasOwnQuery, loadedExpected, queryContract,
                  retainGraphics, releaseGraphics};
CommitStoreV1 store;
}

extern "C" __declspec(dllexport) void WINAPI FixtureSetExpectedGraphics(HMODULE module) noexcept {
    expectedGraphics = module;
}
extern "C" __declspec(dllexport) unsigned WINAPI FixtureCommitCalls() noexcept {
    return commitCalls.load();
}
extern "C" __declspec(dllexport) BOOL WINAPI FixtureReadCommitted(
    AdmissionCommitRequestV1* output) noexcept {
    return store.read(output) ? TRUE : FALSE;
}
extern "C" __declspec(dllexport) void WINAPI FixtureResetCommit() noexcept {
    store.resetForTest();
}
extern "C" __declspec(dllexport) HRESULT WINAPI edvrCommitPluginSelectionAdmissionV1(
    const AdmissionCommitRequestV1* request) noexcept {
    ++commitCalls;
    if (!request) return E_INVALIDARG;
    return store.commitWithCatalogForTest(*request, catalog, hooks);
}
