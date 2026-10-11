#include "plugin_selection_transport.h"

#include "../common/runtime_profile.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace {
using edvr::plugin_selection::PluginSelectionProfileV1;
using edvr::plugin_selection::PluginSelectionSnapshotV1;
using edvr::plugin_selection::decodePluginSelectionSnapshotV1;
using edvr::plugin_selection::copyPluginSelectionSnapshotV1;
using edvr::plugin_selection::pluginSelectionSnapshotMatchesBuildV1;
using edvr::plugin_selection::kSelectionBuildMismatch;

struct PublishedSnapshot {
    std::mutex mutex;
    PluginSelectionSnapshotV1 snapshot{};
    HRESULT status = E_PENDING;
    bool ready = false;
};

struct FileCloser {
    void operator()(void* value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

PublishedSnapshot& publishedSnapshot() {
    static PublishedSnapshot value;
    return value;
}

HRESULT win32Failure(DWORD error) noexcept {
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

HRESULT readBoundedStateFile(const std::wstring& path, std::string* output,
                             bool* missing) noexcept {
    if (!output || !missing) return E_INVALIDARG;
    try {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                output->clear();
                *missing = true;
                return S_OK;
            }
            return win32Failure(error);
        }
        std::unique_ptr<void, FileCloser> fileOwner(file);

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) {
            const HRESULT result = win32Failure(GetLastError());
            return result;
        }
        if (size.QuadPart < 0 ||
            static_cast<unsigned long long>(size.QuadPart) >
                edvr::plugin_selection_transport::kMaxRuntimeStateBytes) {
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }

        std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < text.size()) {
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                text.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD received = 0;
            if (!ReadFile(file, &text[offset], requested, &received, nullptr)) {
                const HRESULT result = win32Failure(GetLastError());
                return result;
            }
            if (!received) {
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            }
            offset += received;
        }
        char extra = 0;
        DWORD received = 0;
        if (!ReadFile(file, &extra, 1, &received, nullptr)) {
            const HRESULT result = win32Failure(GetLastError());
            return result;
        }
        if (received) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        *output = std::move(text);
        *missing = false;
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

PluginSelectionProfileV1 installedProfile() noexcept {
    switch (edvr::g_runtimeProfile) {
    case edvr::RuntimeProfile::LegacyVr:
    case edvr::RuntimeProfile::Vr:
        return PluginSelectionProfileV1::Vr;
    case edvr::RuntimeProfile::Flat:
        return PluginSelectionProfileV1::Flat;
    default:
        return PluginSelectionProfileV1::Unknown;
    }
}
}  // namespace

namespace edvr::plugin_selection_transport {

HRESULT decodeStateFileReadResultV1(
    HRESULT readStatus, bool missing, const char* bytes, std::size_t byteCount,
    PluginSelectionProfileV1 profile, PluginSelectionSnapshotV1* out) noexcept {
    if (!out) return E_INVALIDARG;
    if (FAILED(readStatus)) return readStatus;
    if (readStatus != S_OK) return E_UNEXPECTED;
    if (missing) {
        if (byteCount) return E_INVALIDARG;
        return decodePluginSelectionSnapshotV1("", 0, profile, out);
    }
    if (byteCount > kMaxRuntimeStateBytes) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    if (byteCount && !bytes) return E_INVALIDARG;
    return decodePluginSelectionSnapshotV1(bytes ? bytes : "", byteCount, profile, out);
}

HRESULT captureFromModuleDirectory(const std::wstring& moduleDirectory) noexcept {
    HRESULT result = E_FAIL;
    PluginSelectionSnapshotV1 snapshot{};
    try {
        if (moduleDirectory.empty()) result = E_INVALIDARG;
        else {
            const std::wstring path = moduleDirectory + L"\\edvr_install\\state.ini";
            std::string text;
            bool missing = false;
            result = readBoundedStateFile(path, &text, &missing);
            if (SUCCEEDED(result)) {
                result = decodeStateFileReadResultV1(S_OK, missing,
                    text.empty() ? "" : text.data(), text.size(), installedProfile(), &snapshot);
                if (SUCCEEDED(result) && !pluginSelectionSnapshotMatchesBuildV1(snapshot))
                    snapshot.validationFlags |= kSelectionBuildMismatch;
            }
        }
    } catch (...) {
        result = E_OUTOFMEMORY;
    }
    auto& published = publishedSnapshot();
    {
        std::lock_guard<std::mutex> lock(published.mutex);
        published.status = result;
        if (SUCCEEDED(result)) published.snapshot = snapshot;
        else published.snapshot = {};
        published.ready = true;
    }
    return result;
}

}  // namespace edvr::plugin_selection_transport

extern "C" HRESULT WINAPI edvrQueryPluginSelectionV1(
    std::uint32_t version, std::uint32_t size, void* output) noexcept {
    if (version != edvr::plugin_selection::kPluginSelectionSnapshotVersionV1 ||
        size != sizeof(PluginSelectionSnapshotV1) || !output) return E_INVALIDARG;
    try {
        auto& published = publishedSnapshot();
        PluginSelectionSnapshotV1 snapshot{};
        HRESULT status = E_PENDING;
        {
            std::lock_guard<std::mutex> lock(published.mutex);
            if (!published.ready) return E_PENDING;
            status = published.status;
            if (FAILED(status)) return status;
            snapshot = published.snapshot;
        }
        return copyPluginSelectionSnapshotV1(version, size, &snapshot, output);
    } catch (...) {
        return E_FAIL;
    }
}
