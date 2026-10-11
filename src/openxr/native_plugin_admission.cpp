#include "native_plugin_admission.h"

#include "../common/plugin_admission_peer.h"

#include <cstring>
#include <new>
#include <string>

namespace {
using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;
using namespace edvr::plugin_selection::commit_v1;

using QueryAdmissionV1 = HRESULT (WINAPI *)(std::uint32_t, std::uint32_t,
                                            std::uint32_t, void*);

std::wstring nativeModulePath() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&nativeModulePath), &self) || !self)
        return {};
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(self, path, static_cast<DWORD>(_countof(path)));
    if (!length || length >= _countof(path)) return {};
    return std::wstring(path, length);
}

std::wstring expectedGraphicsPath() {
    std::wstring path = nativeModulePath();
    if (path.empty()) return {};
    for (unsigned i = 0; i < 3; ++i) {
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        path.resize(slash);
    }
    path += L"\\d3d11.dll";
    wchar_t full[32768]{};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(_countof(full)), full, nullptr);
    if (!length || length >= _countof(full)) return {};
    return std::wstring(full, length);
}

bool sameMappedPath(HMODULE module, const std::wstring& expected) noexcept {
    if (!module || expected.empty()) return false;
    wchar_t actual[32768]{};
    const DWORD length = GetModuleFileNameW(module, actual, static_cast<DWORD>(_countof(actual)));
    if (!length || length >= _countof(actual)) return false;
    wchar_t full[32768]{};
    const DWORD fullLength = GetFullPathNameW(actual, static_cast<DWORD>(_countof(full)), full, nullptr);
    if (!fullLength || fullLength >= _countof(full)) return false;
    return CompareStringOrdinal(full, static_cast<int>(fullLength), expected.c_str(),
                                static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

edvr::plugin_admission::AdmissionPeer acquireGraphics(std::uint64_t base) noexcept {
    try {
        auto peer = edvr::plugin_admission::AdmissionPeer::acquire(
            expectedGraphicsPath(), "edvrQueryPluginAdmissionV1");
        if (!peer || reinterpret_cast<std::uint64_t>(peer.module()) != base) return {};
        return peer;
    } catch (...) {
        return {};
    }
}

bool queryExportOwnModule(void*, std::uint64_t base) noexcept {
    return static_cast<bool>(acquireGraphics(base));
}

bool loadedGraphicsIsExpected(void*, std::uint64_t base) noexcept {
    return static_cast<bool>(acquireGraphics(base));
}

HRESULT queryContract(void*, std::uint64_t base, GraphicsContractV1* output) noexcept {
    if (!output) return E_INVALIDARG;
    auto peer = acquireGraphics(base);
    if (!peer) return E_ACCESSDENIED;
    const auto query = reinterpret_cast<QueryAdmissionV1>(peer.proc());
    GraphicsContractV1 returned{};
    const HRESULT status = query(admission_v1::kQueryVersionV1,
        static_cast<std::uint32_t>(admission_v1::QueryKindV1::Contract),
        sizeof(returned), &returned);
    if (status != S_OK) return FAILED(status) ? status : E_UNEXPECTED;
    std::memcpy(output, &returned, sizeof(returned));
    return S_OK;
}

bool retainGraphics(void*, std::uint64_t base) noexcept {
    HMODULE retained = nullptr;
    if (!base || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(base)), &retained) || !retained)
        return false;
    bool expected = false;
    try {
        expected = reinterpret_cast<std::uint64_t>(retained) == base &&
            sameMappedPath(retained, expectedGraphicsPath());
    } catch (...) {
        expected = false;
    }
    if (!expected) FreeLibrary(retained);
    return expected;
}

void releaseGraphics(void*, std::uint64_t base) noexcept {
    if (base) FreeLibrary(reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(base)));
}

CommitStoreV1* commitStore() noexcept {
    // A positive admission is immutable across VR shutdown/reinit. There is no
    // terminal native teardown outside DLL process detach, where destructors
    // run under loader lock. Quarantine this one store and its graphics module
    // reference until OS process exit instead of calling FreeLibrary there.
    static CommitStoreV1* const value = new (std::nothrow) CommitStoreV1;
    return value;
}

const PeerHooksV1 kPeerHooks = {
    nullptr, &queryExportOwnModule, &loadedGraphicsIsExpected,
    &queryContract, &retainGraphics, &releaseGraphics};
} // namespace

namespace edvr::openxr::native_plugin_admission {

bool readCommitted(
    edvr::plugin_selection::commit_v1::AdmissionCommitRequestV1* output) noexcept {
    constexpr std::uint32_t vrProfileMask =
        edvr::plugin_selection::kPluginSelectionVrProfileMaskV1;
    if (!output ||
        (edvr::plugin_selection::admission_v1::contract().supportedProfileMask & vrProfileMask) == 0)
        return false;
    const auto* store = ::commitStore();
    return store && store->read(output);
}

} // namespace edvr::openxr::native_plugin_admission

extern "C" HRESULT WINAPI edvrCommitPluginSelectionAdmissionV1(
    const edvr::plugin_selection::commit_v1::AdmissionCommitRequestV1* request) noexcept {
    if (!request) return E_INVALIDARG;
    const edvr::plugin_selection::commit_v1::AdmissionCommitRequestV1 frozen = *request;
    if (frozen.activeProfile != static_cast<std::uint32_t>(
            edvr::plugin_selection::PluginSelectionProfileV1::Vr)) return E_ACCESSDENIED;
    auto* store = commitStore();
    return store ? store->commit(frozen, kPeerHooks) : E_OUTOFMEMORY;
}
