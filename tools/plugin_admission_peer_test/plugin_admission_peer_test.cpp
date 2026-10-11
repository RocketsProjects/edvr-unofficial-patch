#include "../../src/common/plugin_admission_peer.h"

#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using edvr::plugin_admission::AdmissionPeer;

namespace {
int failures = 0;

void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::wstring modulePath(HMODULE module) {
    wchar_t buffer[32768]{};
    const DWORD size = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    return size ? std::wstring(buffer, size) : std::wstring();
}

bool isLoaded(const std::wstring& path) {
    HMODULE module = nullptr;
    const BOOL found = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           path.c_str(), &module);
    return found != FALSE && module != nullptr;
}

class ScopedModule final {
public:
    explicit ScopedModule(HMODULE module = nullptr) noexcept : module_(module) {}
    ~ScopedModule() noexcept { release(); }
    ScopedModule(const ScopedModule&) = delete;
    ScopedModule& operator=(const ScopedModule&) = delete;
    HMODULE get() const noexcept { return module_; }
    bool release() noexcept {
        HMODULE module = std::exchange(module_, nullptr);
        return !module || FreeLibrary(module) != FALSE;
    }

private:
    HMODULE module_ = nullptr;
};

fs::path stage(const fs::path& source, const fs::path& root, const wchar_t* folder) {
    const fs::path directory = root / folder;
    fs::create_directories(directory);
    const fs::path target = directory / source.filename();
    fs::copy_file(source, target, fs::copy_options::overwrite_existing);
    return target;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--dry-run") {
        std::cout << "plugin admission peer fixture: dry-run OK (no files written)\n";
        return 0;
    }
    if (argc != 5 || std::wstring(argv[1]) != L"--self-test") {
        std::cerr << "usage: plugin_admission_peer_test --self-test provider.dll old_peer.dll forwarder.dll\n";
        return 2;
    }
    wchar_t temp[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
    if (!tempLength || tempLength >= MAX_PATH) return 2;
    fs::path tempDirectory;
    fs::path root;
    bool ownsRoot = false;
    int result = 1;
    try {
        for (int index = 2; index < 5; ++index) {
            const fs::path input(argv[index]);
            if (!input.is_absolute() || !fs::is_regular_file(input))
                throw std::runtime_error("fixture inputs must be absolute DLL file paths");
        }
        tempDirectory = fs::canonical(fs::path(temp));
        root = tempDirectory / (L"edvr_admission_peer_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        if (!tempDirectory.is_absolute() || root.parent_path() != tempDirectory)
            throw std::runtime_error("fixture temp path is not an absolute child of GetTempPath");
        if (!fs::create_directory(root)) throw std::runtime_error("could not create unique fixture directory");
        ownsRoot = true;
        if (fs::canonical(root).parent_path() != tempDirectory)
            throw std::runtime_error("fixture directory resolved outside GetTempPath");
        const fs::path providerA = stage(argv[2], root, L"a");
        const fs::path providerB = stage(argv[2], root, L"b");
        const fs::path oldPath = stage(argv[3], root, L"old");
        const fs::path forwarderPath = stage(argv[4], root, L"forward");
        const fs::path forwardedProviderPath = stage(argv[2], root, L"forward");
        const std::wstring a = providerA.wstring();
        const std::wstring b = providerB.wstring();
        const std::wstring old = oldPath.wstring();
        const std::wstring forwarder = forwarderPath.wstring();
        const std::wstring forwardedProvider = forwardedProviderPath.wstring();

        check(!AdmissionPeer::acquire(a, "FixtureAdmissionProbe"), "reject provider before it is loaded");
        check(!AdmissionPeer::acquire(L"provider_fixture.dll", "FixtureAdmissionProbe"), "reject relative path");
        check(!AdmissionPeer::acquire(L"\\\\server\\share\\provider_fixture.dll", "FixtureAdmissionProbe"), "reject UNC path");
        check(!AdmissionPeer::acquire(L"\\\\?\\C:\\provider_fixture.dll", "FixtureAdmissionProbe"), "reject extended path");
        check(!AdmissionPeer::acquire(a, ""), "reject empty export name");
        std::wstring differentlyCased = a;
        for (wchar_t& ch : differentlyCased) {
            if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
        check(!AdmissionPeer::acquire(differentlyCased, "FixtureAdmissionProbe"), "case-insensitive path still requires a loaded module");

        ScopedModule forwarderModule(LoadLibraryExW(forwarder.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(forwarderModule.get() != nullptr, "test explicitly loads forwarder fixture");
        if (forwarderModule.get()) {
            check(!isLoaded(forwardedProvider), "forward target starts unmapped");
            check(!AdmissionPeer::acquire(forwarder, "FixtureAdmissionProbe"), "reject forwarded export before loader resolution");
            check(!isLoaded(forwardedProvider), "rejecting forwarder does not map its target");
        }
        check(forwarderModule.release(), "release unloaded-target forwarder fixture");
        ScopedModule forwardedTargetModule(LoadLibraryExW(forwardedProvider.c_str(), nullptr,
                                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(forwardedTargetModule.get() != nullptr, "test explicitly preloads forwarder target");
        ScopedModule preloadedForwarder(LoadLibraryExW(forwarder.c_str(), nullptr,
                                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(preloadedForwarder.get() != nullptr, "test explicitly reloads forwarder fixture");
        if (forwardedTargetModule.get() && preloadedForwarder.get()) {
            FARPROC forwardedProc = GetProcAddress(preloadedForwarder.get(), "FixtureAdmissionProbe");
            HMODULE addressOwner = nullptr;
            check(forwardedProc != nullptr, "test resolves actual forwarded fixture export");
            check(forwardedProc && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(forwardedProc),
                  &addressOwner) && addressOwner == forwardedTargetModule.get() && addressOwner != preloadedForwarder.get(),
                  "forwarded export address belongs to target module, not forwarder");
            check(!AdmissionPeer::acquire(forwarder, "FixtureAdmissionProbe"),
                  "reject forwarded export when target is already loaded");
            check(isLoaded(forwardedProvider), "preloaded forward target remains mapped after rejection");
        }
        check(preloadedForwarder.release(), "release preloaded forwarder fixture");
        check(forwardedTargetModule.release(), "release preloaded forward target fixture");

        ScopedModule loadedA(LoadLibraryExW(a.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(loadedA.get() != nullptr, "test explicitly loads first provider fixture");
        if (!loadedA.get()) throw std::runtime_error("could not load first provider fixture");
        std::wstring embeddedNul = a;
        embeddedNul.push_back(L'\0');
        embeddedNul += L"ignored trailing input";
        check(!AdmissionPeer::acquire(embeddedNul, "FixtureAdmissionProbe"),
              "reject embedded NUL even when its prefix names a loaded provider");
        check(!AdmissionPeer::acquire(b, "FixtureAdmissionProbe"), "reject different path with duplicate basename when not loaded");

        ScopedModule loadedB(LoadLibraryExW(b.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(loadedB.get() != nullptr, "test explicitly loads second provider fixture");
        if (!loadedB.get()) throw std::runtime_error("could not load second provider fixture");
        check(loadedA.get() != loadedB.get(), "duplicate basenames map as distinct modules");
        ScopedModule loadedOld(LoadLibraryExW(old.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(loadedOld.get() != nullptr, "test explicitly loads old peer fixture");
        if (!loadedOld.get()) throw std::runtime_error("could not load old peer fixture");
        check(isLoaded(old), "old peer is mapped before missing-export check");
        check(!AdmissionPeer::acquire(old, "FixtureAdmissionProbe"), "reject loaded old peer missing export");
        check(loadedOld.release(), "release old peer fixture after rejected lookup");
        check(!isLoaded(old), "rejected old peer unloads after its only explicit reference");

        {
            AdmissionPeer peer = AdmissionPeer::acquire(a, "FixtureAdmissionProbe");
            check(static_cast<bool>(peer), "accept exact loaded provider path");
            check(peer.module() == loadedA.get(), "retain exact provider module");
            check(peer.path().size() == a.size() && CompareStringOrdinal(peer.path().c_str(),
                  static_cast<int>(peer.path().size()), a.c_str(), static_cast<int>(a.size()), TRUE) == CSTR_EQUAL,
                  "record canonical absolute provider path");
            if (peer) {
                using Probe = int (*)(int);
                check(reinterpret_cast<Probe>(peer.proc())(5) == 22, "fixture export remains callable");
            }
            check(!AdmissionPeer::acquire(forwarder, "FixtureAdmissionProbe"), "reject forwarded export owned by another module");
            check(!isLoaded(forwardedProvider), "preloaded exact peer does not make forwarder acceptable");
            check(!AdmissionPeer::acquire(a, "FixtureNoSuchExport"), "reject invalid export name");

            AdmissionPeer casePeer = AdmissionPeer::acquire(differentlyCased, "FixtureAdmissionProbe");
            check(static_cast<bool>(casePeer) && casePeer.module() == loadedA.get(),
                  "match exact module path without case sensitivity");
            AdmissionPeer secondPeer = AdmissionPeer::acquire(b, "FixtureAdmissionProbe");
            check(static_cast<bool>(secondPeer) && secondPeer.module() == loadedB.get(),
                  "select second exact path when duplicate basename is loaded");

            check(loadedA.release(), "release test's explicit provider reference");
            check(isLoaded(a), "peer reference keeps provider mapped after caller releases its reference");
        }
        check(!isLoaded(a), "provider unloads after peer releases its owned reference");

        check(loadedB.release(), "release second provider fixture");
        check(!isLoaded(b), "second provider unloads after its final reference");

        ScopedModule late(LoadLibraryExW(a.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(late.get() != nullptr, "test explicitly loads provider for late-load case");
        if (late.get()) {
            AdmissionPeer peer = AdmissionPeer::acquire(a, "FixtureAdmissionProbe");
            check(static_cast<bool>(peer), "a later acquisition succeeds once the provider is loaded");
            check(late.release(), "release late explicit provider reference");
        }
        result = failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "fixture error: " << error.what() << '\n';
        result = 2;
    } catch (...) {
        std::cerr << "fixture error: unknown exception\n";
        result = 2;
    }
    if (ownsRoot) {
        try {
            if (!root.is_absolute() || root.parent_path() != tempDirectory ||
                fs::canonical(root).parent_path() != tempDirectory) {
                throw std::runtime_error("refusing cleanup outside owned GetTempPath child");
            }
            fs::remove_all(root);
        } catch (const std::exception& error) {
            std::cerr << "fixture cleanup error: " << error.what() << '\n';
            result = 2;
        }
    }
    if (result == 0) std::cout << "plugin admission peer fixture: OK\n";
    return result;
}
