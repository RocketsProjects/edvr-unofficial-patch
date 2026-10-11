#pragma once

#include <string>

#include <windows.h>

namespace edvr::plugin_admission {

// A qualified, already-loaded provider at a drive-absolute Windows path
// (for example C:\\game\\provider.dll). UNC and extended paths are rejected.
// Acquisition never maps a DLL: it takes one reference to the module at the
// exact canonical path and retains it until this object is destroyed.
class AdmissionPeer final {
public:
    AdmissionPeer() noexcept = default;
    ~AdmissionPeer() noexcept;

    AdmissionPeer(const AdmissionPeer&) = delete;
    AdmissionPeer& operator=(const AdmissionPeer&) = delete;
    AdmissionPeer(AdmissionPeer&& other) noexcept;
    AdmissionPeer& operator=(AdmissionPeer&& other) noexcept;

    static AdmissionPeer acquire(const std::wstring& expectedAbsolutePath,
                                 const char* exportName) noexcept;

    explicit operator bool() const noexcept { return module_ != nullptr && proc_ != nullptr; }
    HMODULE module() const noexcept { return module_; }
    FARPROC proc() const noexcept { return proc_; }
    const std::wstring& path() const noexcept { return path_; }

private:
    AdmissionPeer(HMODULE module, FARPROC proc, std::wstring path) noexcept;
    void reset() noexcept;

    HMODULE module_ = nullptr;
    FARPROC proc_ = nullptr;
    std::wstring path_;
};

}  // namespace edvr::plugin_admission
