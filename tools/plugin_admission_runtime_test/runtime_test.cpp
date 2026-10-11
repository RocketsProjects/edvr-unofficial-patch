#include "../../src/common/plugin_admission_peer.h"
#include "../../src/common/plugin_admission_commit.h"
#include "../../src/openxr/native_graphics_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;
using namespace edvr::plugin_selection::commit_v1;
using edvr::openxr::NativeGraphicsClient;
using edvr::plugin_admission::AdmissionPeer;

namespace {
const std::uint8_t fingerprint[32] = {0x29, 0x10, 0x77};
test_seam::GroupV1 groups[] = {{0, 3, 3}, {1, 1, 1}, {2, 2, 2}};
test_seam::CatalogViewV1 catalog{groups, 3, 2, fingerprint};
unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

class Watchdog final {
public:
    Watchdog() : worker_([this] {
        const ULONGLONG deadline = GetTickCount64() + 9000;
        while (!done_.load(std::memory_order_acquire)) {
            if (GetTickCount64() >= deadline) std::_Exit(9);
            Sleep(10);
        }
    }) {}
    ~Watchdog() {
        done_.store(true, std::memory_order_release);
        worker_.join();
    }
private:
    std::atomic<bool> done_{false};
    std::thread worker_;
};

std::wstring modulePath(HMODULE module) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    return length && length < std::size(path) ? std::wstring(path, length) : std::wstring();
}

bool isLoaded(const std::wstring& path) {
    HMODULE module = nullptr;
    const BOOL loaded = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        path.c_str(), &module);
    return loaded && module;
}

fs::path makeRoot(const std::wstring& fixturePath) {
    // The runner and fixture DLLs are built together in the writable build
    // directory. Confine scratch state to that verified parent; sandbox TEMP
    // can point at a private package path that is not usable by this process.
    const std::wstring runnerPath = modulePath(nullptr);
    if (runnerPath.empty()) throw std::runtime_error("runner path unavailable");
    const fs::path base = fs::canonical(fs::path(runnerPath)).parent_path();
    const fs::path fixture = fs::canonical(fs::path(fixturePath));
    if (!fs::is_regular_file(fixture) || fixture.parent_path() != base)
        throw std::runtime_error("fixture DLL is outside the runner build directory");
    const fs::path root = base / (L"edvr_admission_runtime_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root) || fs::canonical(root).parent_path() != base)
        throw std::runtime_error("could not create confined fixture directory");
    return root;
}

void cleanupRoot(const fs::path& root) {
    const fs::path absolute = fs::absolute(root);
    if (!absolute.is_absolute() || absolute.parent_path() != fs::canonical(root.parent_path()) ||
        fs::canonical(root).parent_path() != fs::canonical(root.parent_path()))
        throw std::runtime_error("refusing fixture cleanup outside its owned temp child");
    fs::remove_all(root);
}

void writeState(const fs::path& root, const char* selectedIds) {
    const fs::path install = root / L"edvr_install";
    fs::create_directories(install);
    std::ofstream file(install / L"state.ini", std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("could not write temporary fixture state");
    file << "[plugins]\r\nselection_schema_version = 1\r\n"
         << "catalog_schema_version = 2\r\nprofile = vr\r\nselected_ids = "
         << selectedIds << "\r\n";
}

using QueryAdmission = HRESULT (WINAPI *)(std::uint32_t, std::uint32_t,
                                           std::uint32_t, void*);
using FixtureBegin = BOOL (WINAPI *)();
using FixtureRelease = void (WINAPI *)();
using FixtureCount = unsigned (WINAPI *)();
using FixturePublish = HRESULT (WINAPI *)(const wchar_t*, HMODULE);
using FixtureOwnerAllowed = BOOL (WINAPI *)(std::uint32_t);

class ScopedModule final {
public:
    explicit ScopedModule(HMODULE module = nullptr) noexcept : module_(module) {}
    ~ScopedModule() noexcept { release(); }
    ScopedModule(const ScopedModule&) = delete;
    ScopedModule& operator=(const ScopedModule&) = delete;
    HMODULE get() const noexcept { return module_; }
    HMODULE release() noexcept {
        HMODULE module = std::exchange(module_, nullptr);
        if (module) FreeLibrary(module);
        return module;
    }
private:
    HMODULE module_ = nullptr;
};

void heldContractQuery(const std::wstring& graphicsPath) {
    ScopedModule module(LoadLibraryExW(graphicsPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
    check(module.get() != nullptr, "load actual graphics admission transport fixture");
    if (!module.get()) return;
    auto query = reinterpret_cast<QueryAdmission>(GetProcAddress(module.get(), "edvrQueryPluginAdmissionV1"));
    auto begin = reinterpret_cast<FixtureBegin>(GetProcAddress(module.get(), "FixtureBeginHeldInit"));
    auto release = reinterpret_cast<FixtureRelease>(GetProcAddress(module.get(), "FixtureReleaseHeldInit"));
    check(query && begin && release, "graphics fixture exposes Contract query and held-init controls");
    const bool held = query && begin && release && begin();
    if (held) {
        std::mutex mutex;
        std::condition_variable changed;
        bool completed = false;
        HRESULT result = E_FAIL;
        GraphicsContractV1 contract{};
        std::thread worker([&] {
            result = query(1, 1, sizeof(contract), &contract);
            { std::lock_guard<std::mutex> lock(mutex); completed = true; }
            changed.notify_one();
        });
        bool completedWhileHeld = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            completedWhileHeld = changed.wait_for(lock, std::chrono::milliseconds(700),
                [&] { return completed; });
        }
        release();
        worker.join();
        check(completedWhileHeld && result == S_OK && contract.structSize == sizeof(contract),
              "pure Contract query completes within a finite timeout while graphics init is held");
    } else {
        if (release) release();
        check(false, "fixture held-init callback entered");
    }
}

void nativePreflight(const std::wstring& graphicsPath) {
    ScopedModule module(LoadLibraryExW(graphicsPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
    check(module.get() != nullptr, "load graphics provider for native preflight");
    if (!module.get()) return;
    auto pairCalls = reinterpret_cast<FixtureCount>(GetProcAddress(module.get(), "FixturePairCalls"));
    auto query = reinterpret_cast<QueryAdmission>(GetProcAddress(module.get(), "edvrQueryPluginAdmissionV1"));
    check(pairCalls && query, "graphics provider exposes pair-call counter and admission query");
    if (pairCalls && query) {
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(module.get()));
        DecisionV1 expected{};
        expected.structSize = sizeof(expected);
        expected.abiVersion = admission_v1::kAbiVersion;
        expected.mode = static_cast<std::uint32_t>(Mode::AdmittedSelection);
        expected.graphicsProviderBaseLow = static_cast<std::uint32_t>(base + 0x1000);
        expected.graphicsProviderBaseHigh = static_cast<std::uint32_t>((base + 0x1000) >> 32);
        NativeGraphicsClient wrongProvider;
        const HRESULT wrong = wrongProvider.acquire(graphicsPath, &expected, false);
        check(wrong == E_ACCESSDENIED && pairCalls() == 0,
              "reserved HMODULE mismatch rejects before paired graphics exports");

        expected.graphicsProviderBaseLow = static_cast<std::uint32_t>(base);
        expected.graphicsProviderBaseHigh = static_cast<std::uint32_t>(base >> 32);
        NativeGraphicsClient lateMismatch;
        const HRESULT decision = lateMismatch.acquire(graphicsPath, &expected, false);
        check(decision != S_OK && pairCalls() == 0,
              "missing or late admission Decision rejects before paired exports");
    }
}

void generatedLegacy(const std::wstring& graphicsPath, const std::wstring& peerPath,
                     const char* selectedIds, Reason expectedReason, const char* label) {
    const fs::path root = makeRoot(graphicsPath);
    try {
        writeState(root, selectedIds);
        {
            const fs::path peerDirectory = root / L"Openvr" / L"win64";
            fs::create_directories(peerDirectory);
            const fs::path fakePeer = peerDirectory / L"openvr_api.dll";
            fs::copy_file(peerPath, fakePeer);
            ScopedModule peerModule(LoadLibraryExW(fakePeer.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
            ScopedModule graphicsModule(LoadLibraryExW(graphicsPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
            check(peerModule.get() && graphicsModule.get(), "preload exact-path fake native peer and graphics provider");
            if (peerModule.get() && graphicsModule.get()) {
                auto count = reinterpret_cast<FixtureCount>(GetProcAddress(peerModule.get(), "FixtureCommitCalls"));
                auto publish = reinterpret_cast<FixturePublish>(GetProcAddress(graphicsModule.get(), "FixturePublishCandidate"));
                auto query = reinterpret_cast<QueryAdmission>(GetProcAddress(graphicsModule.get(), "edvrQueryPluginAdmissionV1"));
                auto pairCalls = reinterpret_cast<FixtureCount>(GetProcAddress(graphicsModule.get(), "FixturePairCalls"));
                auto ownerAllowed = reinterpret_cast<FixtureOwnerAllowed>(
                    GetProcAddress(graphicsModule.get(), "FixtureOwnerAllowedForStartup"));
                check(count && publish && query && pairCalls && ownerAllowed,
                      "actual runtime admission and startup-owner exports are present");
                if (count && publish && query && pairCalls && ownerAllowed) {
                    constexpr std::uint32_t kPerformancePluginIndex = 6;
                    check(!ownerAllowed(kPerformancePluginIndex),
                          "unpublished Deferred decision blocks Performance startup");
                    const std::wstring directory = root.wstring();
                    check(publish(directory.c_str(), graphicsModule.get()) == S_OK,
                          "capture temporary source snapshot and publish actual production candidate");
                    DecisionV1 decision{};
                    check(query(1, 2, sizeof(decision), &decision) == S_OK,
                          "published Decision is available to a late native reader");
                    check(decision.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
                          decision.reason == static_cast<std::uint32_t>(expectedReason), label);
                    check(ownerAllowed(kPerformancePluginIndex),
                          "published current-generated Legacy permits Performance startup");
                    check(!ownerAllowed(0xffffffffu),
                          "published Legacy rejects an unknown out-of-range startup owner");
                    check(pairCalls() == 0,
                          "startup-owner queries do not call optional paired graphics exports");
                    check(count() == 0, "all-unready or invalid snapshot never calls native peer commit");
                }
            }
        }
        cleanupRoot(root);
    } catch (...) {
        cleanupRoot(root);
        throw;
    }
}

PluginSelectionSnapshotV1 syntheticSnapshot() {
    PluginSelectionSnapshotV1 out{};
    out.structSize = sizeof(out);
    out.abiVersion = kPluginSelectionSnapshotVersionV1;
    out.recordState = static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported);
    out.installedProfile = out.recordProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Vr);
    out.recordSchemaVersion = 1;
    out.recordCatalogSchemaVersion = out.compiledCatalogSchemaVersion = catalog.schema;
    std::memcpy(out.catalogFingerprint, catalog.fingerprint, sizeof(out.catalogFingerprint));
    return out;
}

void exactLoadedPositiveCommit(const std::wstring& providerPath, const std::wstring& peerPath) {
    const fs::path root = makeRoot(providerPath);
    try {
        const fs::path providerCopy = root / L"provider_fixture.dll";
        const fs::path peerCopy = root / L"peer_fixture.dll";
        fs::copy_file(providerPath, providerCopy);
        fs::copy_file(peerPath, peerCopy);
        ScopedModule provider(LoadLibraryExW(providerCopy.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        ScopedModule peerModule(LoadLibraryExW(peerCopy.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR));
        check(provider.get() && peerModule.get(), "load positive synthetic provider and exact fake native peer DLLs");
        if (provider.get() && peerModule.get()) {
            using SetProvider = void (WINAPI *)(HMODULE);
            using ReadCommitted = BOOL (WINAPI *)(AdmissionCommitRequestV1*);
            using CommitCount = unsigned (WINAPI *)();
            using ResetCommit = void (WINAPI *)();
            using SetDecision = void (WINAPI *)(HRESULT, const DecisionV1*);
            using PairCount = unsigned (WINAPI *)();
            const auto setProvider = reinterpret_cast<SetProvider>(GetProcAddress(peerModule.get(), "FixtureSetExpectedGraphics"));
            const auto read = reinterpret_cast<ReadCommitted>(GetProcAddress(peerModule.get(), "FixtureReadCommitted"));
            const auto count = reinterpret_cast<CommitCount>(GetProcAddress(peerModule.get(), "FixtureCommitCalls"));
            const auto reset = reinterpret_cast<ResetCommit>(GetProcAddress(peerModule.get(), "FixtureResetCommit"));
            // Declared after peerModule, before AdmissionPeer: exceptional exit
            // resets a positive store while its DLL and the provider are mapped.
            struct ResetScope {
                ResetCommit function;
                ~ResetScope() { if (function) function(); }
                void disarm() noexcept { function = nullptr; }
            } resetScope{reset};
            const auto setDecision = reinterpret_cast<SetDecision>(GetProcAddress(provider.get(), "FixtureSetDecision"));
            const auto pairCount = reinterpret_cast<PairCount>(GetProcAddress(provider.get(), "FixturePairCalls"));
            check(setProvider && read && count && reset && setDecision && pairCount,
                  "fake DLLs expose commit-store and admission response controls");
            if (setProvider && read && count && reset && setDecision && pairCount) {
                setProvider(provider.get());
                const auto queryDecision = reinterpret_cast<QueryAdmission>(
                    GetProcAddress(provider.get(), "edvrQueryPluginAdmissionV1"));
                const auto checkGuard = [&](NativeGraphicsClient& client,
                                            const DecisionV1* expectedAdmission,
                                            bool unreserved, HRESULT expectedStatus,
                                            unsigned expectedPairDelta, const char* label) {
                    const unsigned beforePairs = pairCount();
                    const HRESULT actual = client.acquire(providerCopy.wstring(),
                                                          expectedAdmission, unreserved);
                    const unsigned afterPairs = pairCount();
                    const unsigned pairDelta = afterPairs - beforePairs;
                    if (actual != expectedStatus || pairDelta != expectedPairDelta) {
                        DecisionV1 observed{};
                        const HRESULT queried = queryDecision
                            ? queryDecision(1, 2, sizeof(observed), &observed) : E_NOINTERFACE;
                        std::fprintf(stderr,
                            "guard diagnostic: %s actual=%08lx expected=%08lx before_pairs=%u "
                            "after_pairs=%u pair_delta=%u expected_delta=%u "
                            "query=%08lx mode=%u reason=%u reserved0=%u\n",
                            label, static_cast<unsigned long>(actual),
                            static_cast<unsigned long>(expectedStatus), beforePairs, afterPairs,
                            pairDelta, expectedPairDelta,
                            static_cast<unsigned long>(queried), observed.mode, observed.reason,
                            observed.reserved[0]);
                    }
                    check(actual == expectedStatus && pairDelta == expectedPairDelta, label);
                };
                NativeGraphicsClient unpublished;
                check(unpublished.acquire(providerCopy.wstring(), nullptr, true) == E_PENDING &&
                      pairCount() == 0,
                      "unpublished Decision rejects unreserved startup before paired exports");
                AdmissionPeer peer = AdmissionPeer::acquire(peerCopy.wstring(),
                    "edvrCommitPluginSelectionAdmissionV1");
                check(static_cast<bool>(peer) && peer.module() == peerModule.get(),
                      "positive commit uses the exact already-loaded native DLL path");
                if (peer) {
                    AdmissionCommitRequestV1 commit{};
                    commit.structSize = sizeof(commit);
                    commit.abiVersion = commit_v1::kAbiVersion;
                    commit.requiredCapabilities = commit_v1::kRequiredCapabilities;
                    commit.activeProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Vr);
                    commit.supportedProfileMask = 3;
                    commit.snapshot = syntheticSnapshot();
                    const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(provider.get()));
                    commit.decision = test_seam::decideWithCatalog(S_OK, &commit.snapshot,
                        PluginSelectionProfileV1::Vr, base, catalog);
                    DecisionV1 returnedDecision = commit.decision;
                    setDecision(S_FALSE, &returnedDecision);
                    NativeGraphicsClient falseSuccess;
                    check(falseSuccess.acquire(providerCopy.wstring(), &commit.decision, false) == E_ACCESSDENIED &&
                          pairCount() == 0,
                          "S_FALSE admission query is rejected before paired exports");
                    returnedDecision.reserved[0] = 1;
                    setDecision(S_OK, &returnedDecision);
                    NativeGraphicsClient malformed;
                    checkGuard(malformed, &commit.decision, false, E_ACCESSDENIED, 0,
                               "malformed late Decision is rejected before paired exports");
                    returnedDecision = commit.decision;
                    returnedDecision.mode = static_cast<std::uint32_t>(Mode::Legacy);
                    setDecision(S_OK, &returnedDecision);
                    NativeGraphicsClient legacyAfterCommit;
                    checkGuard(legacyAfterCommit, &commit.decision, false, E_ACCESSDENIED, 0,
                               "Legacy response cannot downgrade an earlier positive reservation");
                    setDecision(S_OK, &commit.decision);
                    NativeGraphicsClient matching;
                    checkGuard(matching, &commit.decision, false, E_FAIL, 1,
                               "matching late Decision passes the guard and reaches the paired export");
                    const DecisionV1 publishedLegacy = test_seam::decideWithCatalog(
                        E_FAIL, nullptr, PluginSelectionProfileV1::Vr, base, catalog);
                    setDecision(S_OK, &publishedLegacy);
                    NativeGraphicsClient unreservedLegacy;
                    checkGuard(unreservedLegacy, nullptr, true, E_FAIL, 1,
                               "published validated Legacy permits unreserved paired graphics startup");
                    setDecision(S_OK, &commit.decision);
                    const HRESULT status = reinterpret_cast<NativeCommitExportV1>(peer.proc())(&commit);
                    AdmissionCommitRequestV1 returned{};
                    check(status == S_OK && read(&returned) &&
                          std::memcmp(&returned, &commit, sizeof(commit)) == 0 && count() == 1,
                          "exact loaded fake DLL commits a validated immutable reservation");

                    const std::wstring loadedProvider = modulePath(provider.get());
                    check(provider.release() != nullptr && isLoaded(loadedProvider),
                          "committed peer reference keeps the graphics provider mapped");
                    reset();
                    resetScope.disarm();
                    check(!isLoaded(loadedProvider),
                          "explicit fixture reset releases the retained graphics reference outside DLL teardown");
                    peer = AdmissionPeer{};
                    peerModule.release();
                    check(!isLoaded(loadedProvider), "native peer can unload after its reservation is reset");
                }
            }
        }
        cleanupRoot(root);
    } catch (...) {
        cleanupRoot(root);
        throw;
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--dry-run") {
        std::puts("plugin admission runtime fixture: dry-run OK (no files written)");
        return 0;
    }
    if (argc != 5 || std::wstring(argv[1]) != L"--self-test") {
        std::fprintf(stderr, "usage: plugin_admission_runtime_test --self-test runtime_graphics.dll native_peer.dll positive_provider.dll\n");
        return 2;
    }
    Watchdog watchdog;
    try {
        const std::wstring graphics(argv[2]);
        const std::wstring native(argv[3]);
        const std::wstring positive(argv[4]);
        heldContractQuery(graphics);
        nativePreflight(graphics);
        generatedLegacy(graphics, native, "", Reason::ProfileNotReady,
                        "current all-unready generated profile stays Legacy");
        generatedLegacy(graphics, native, "performance", Reason::InvalidSnapshot,
                        "invalid selected snapshot stays Legacy");
        exactLoadedPositiveCommit(positive, native);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fixture error: %s\n", error.what());
        return 2;
    }
    std::printf("plugin_admission_runtime_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
