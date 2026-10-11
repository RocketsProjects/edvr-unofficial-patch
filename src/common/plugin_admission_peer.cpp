#include "plugin_admission_peer.h"

#include <utility>
#include <vector>

namespace edvr::plugin_admission {
namespace {

bool isAbsoluteDrivePath(const std::wstring& path) noexcept {
    return path.size() >= 3 &&
           ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

bool canonicalAbsolutePath(const std::wstring& input, std::wstring* output) noexcept {
    if (!output || input.size() > 32767 || input.find(L'\0') != std::wstring::npos ||
        !isAbsoluteDrivePath(input)) return false;
    const DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0 || needed > 32768) return false;
    std::vector<wchar_t> buffer;
    try {
        buffer.resize(static_cast<size_t>(needed) + 1);
    } catch (...) {
        return false;
    }
    const DWORD written = GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()),
                                           buffer.data(), nullptr);
    if (written == 0 || written >= buffer.size() || !isAbsoluteDrivePath(buffer.data())) return false;
    try {
        output->assign(buffer.data(), written);
        for (wchar_t& ch : *output) {
            if (ch == L'/') ch = L'\\';
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool modulePath(HMODULE module, std::wstring* output) noexcept {
    if (!module || !output) return false;
    std::vector<wchar_t> buffer;
    try {
        buffer.resize(512);
    } catch (...) {
        return false;
    }
    for (;;) {
        const DWORD written = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return false;
        if (written < buffer.size() - 1) {
            try {
                output->assign(buffer.data(), written);
            } catch (...) {
                return false;
            }
            return true;
        }
        if (buffer.size() >= 32768) return false;
        try {
            buffer.resize((buffer.size() * 2) > 32768 ? 32768 : buffer.size() * 2);
        } catch (...) {
            return false;
        }
    }
}

bool samePath(const std::wstring& left, const std::wstring& right) noexcept {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

// A forwarder RVA points into the export directory and makes GetProcAddress
// ask the loader to resolve another module. Reject it from the mapped image
// before calling GetProcAddress, so qualification itself cannot trigger a load.
bool hasDirectExport(HMODULE module, const char* wanted) noexcept {
    __try {
        const auto* base = reinterpret_cast<const unsigned char*>(module);
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(base, &memory, sizeof(memory)) || memory.AllocationBase != module ||
            memory.RegionSize < sizeof(IMAGE_DOS_HEADER)) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
        if (ntOffset > memory.RegionSize || sizeof(IMAGE_NT_HEADERS) > memory.RegionSize - ntOffset) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + ntOffset);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER) ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return false;
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;
        const size_t directoryCount = nt->OptionalHeader.NumberOfRvaAndSizes;
        const size_t optionalBytes = nt->FileHeader.SizeOfOptionalHeader;
        const size_t directoryOffset = offsetof(IMAGE_OPTIONAL_HEADER, DataDirectory);
        if (optionalBytes < directoryOffset ||
            directoryCount > (optionalBytes - directoryOffset) / sizeof(IMAGE_DATA_DIRECTORY)) return false;
        if (imageSize < ntOffset + offsetof(IMAGE_NT_HEADERS, OptionalHeader) +
                            sizeof(IMAGE_OPTIONAL_HEADER) ||
            directoryCount <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
        const IMAGE_DATA_DIRECTORY directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
            directory.VirtualAddress >= imageSize ||
            directory.Size > imageSize - directory.VirtualAddress) return false;
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        if ((exports->NumberOfNames && (!exports->AddressOfNames || !exports->AddressOfNameOrdinals)) ||
            (exports->NumberOfFunctions && !exports->AddressOfFunctions)) return false;
        if (exports->NumberOfNames > imageSize / sizeof(DWORD) ||
            exports->NumberOfFunctions > imageSize / sizeof(DWORD) ||
            exports->AddressOfNames >= imageSize || exports->AddressOfNameOrdinals >= imageSize ||
            exports->AddressOfFunctions >= imageSize ||
            static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD) > imageSize - exports->AddressOfNames ||
            static_cast<size_t>(exports->NumberOfNames) * sizeof(WORD) > imageSize - exports->AddressOfNameOrdinals ||
            static_cast<size_t>(exports->NumberOfFunctions) * sizeof(DWORD) > imageSize - exports->AddressOfFunctions) return false;

        const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            const DWORD nameRva = names[i];
            if (nameRva >= imageSize) return false;
            const char* name = reinterpret_cast<const char*>(base + nameRva);
            size_t j = 0;
            while (nameRva + j < imageSize && name[j] && wanted[j] && name[j] == wanted[j]) ++j;
            if (nameRva + j >= imageSize || name[j] != wanted[j]) continue;
            if (name[j] != '\0' || wanted[j] != '\0') continue;
            const WORD ordinal = ordinals[i];
            if (ordinal >= exports->NumberOfFunctions) return false;
            const DWORD functionRva = functions[ordinal];
            if (!functionRva || functionRva >= imageSize) return false;
            const size_t exportBegin = directory.VirtualAddress;
            const size_t exportEnd = exportBegin + directory.Size;
            return functionRva < exportBegin || functionRva >= exportEnd;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool belongsToModule(FARPROC proc, HMODULE expected) noexcept {
    if (!proc || !expected) return false;
    HMODULE actual = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(proc), &actual)) {
        return false;
    }
    return actual == expected;
}

}  // namespace

AdmissionPeer::AdmissionPeer(HMODULE module, FARPROC proc, std::wstring path) noexcept
    : module_(module), proc_(proc), path_(std::move(path)) {}

AdmissionPeer::~AdmissionPeer() noexcept { reset(); }

AdmissionPeer::AdmissionPeer(AdmissionPeer&& other) noexcept
    : module_(other.module_), proc_(other.proc_), path_(std::move(other.path_)) {
    other.module_ = nullptr;
    other.proc_ = nullptr;
}

AdmissionPeer& AdmissionPeer::operator=(AdmissionPeer&& other) noexcept {
    if (this != &other) {
        reset();
        module_ = other.module_;
        proc_ = other.proc_;
        path_ = std::move(other.path_);
        other.module_ = nullptr;
        other.proc_ = nullptr;
    }
    return *this;
}

void AdmissionPeer::reset() noexcept {
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
    proc_ = nullptr;
    path_.clear();
}

AdmissionPeer AdmissionPeer::acquire(const std::wstring& expectedAbsolutePath,
                                     const char* exportName) noexcept {
    if (!exportName || !*exportName) return {};
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(exportName); *p; ++p) {
        if (*p >= 0x80) return {};
    }

    std::wstring expected;
    if (!canonicalAbsolutePath(expectedAbsolutePath, &expected)) return {};

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(0, expected.c_str(), &module) || !module) return {};

    std::wstring mappedPath;
    if (!modulePath(module, &mappedPath)) {
        FreeLibrary(module);
        return {};
    }
    std::wstring canonicalMapped;
    if (!canonicalAbsolutePath(mappedPath, &canonicalMapped) || !samePath(expected, canonicalMapped)) {
        FreeLibrary(module);
        return {};
    }

    if (!hasDirectExport(module, exportName)) {
        FreeLibrary(module);
        return {};
    }
    FARPROC proc = GetProcAddress(module, exportName);
    if (!proc || !belongsToModule(proc, module)) {
        FreeLibrary(module);
        return {};
    }
    return AdmissionPeer(module, proc, std::move(canonicalMapped));
}

}  // namespace edvr::plugin_admission
