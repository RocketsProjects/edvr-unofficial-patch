#include "plugin_admission_commit.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;
using namespace edvr::plugin_selection::commit_v1;

namespace {
const std::uint8_t fingerprint[32] = {0x71, 0x31, 0x10};
test_seam::GroupV1 groups[] = {{0, 3, 3}, {1, 1, 1}, {2, 2, 2}};
test_seam::CatalogViewV1 catalog{groups, 3, 2, fingerprint};

void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

PluginSelectionSnapshotV1 snapshot(PluginSelectionProfileV1 profile, std::uint32_t mask = 0) {
    PluginSelectionSnapshotV1 out{};
    out.structSize = sizeof(out);
    out.abiVersion = kPluginSelectionSnapshotVersionV1;
    out.recordState = static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported);
    out.installedProfile = out.recordProfile = static_cast<std::uint32_t>(profile);
    out.recordSchemaVersion = 1;
    out.recordCatalogSchemaVersion = out.compiledCatalogSchemaVersion = catalog.schema;
    out.recordedPluginMask = mask;
    std::memcpy(out.catalogFingerprint, catalog.fingerprint, sizeof(out.catalogFingerprint));
    return out;
}

AdmissionCommitRequestV1 request(PluginSelectionProfileV1 profile, std::uint32_t mask = 0) {
    AdmissionCommitRequestV1 out{};
    out.structSize = sizeof(out);
    out.abiVersion = commit_v1::kAbiVersion;
    out.requiredCapabilities = kRequiredCapabilities;
    out.activeProfile = static_cast<std::uint32_t>(profile);
    out.supportedProfileMask = 3;
    out.snapshot = snapshot(profile, mask);
    out.decision = test_seam::decideWithCatalog(S_OK, &out.snapshot, profile,
                                                0x12345000, catalog);
    return out;
}

struct Peer {
    GraphicsContractV1 contract{};
    std::atomic<unsigned> loadedCalls{0}, exportCalls{0}, queryCalls{0};
    std::atomic<unsigned> retainCalls{0}, releaseCalls{0};
    bool loaded = true, ownExport = true, retain = true;
    AdmissionCommitRequestV1* mutateRequest = nullptr;
    PeerHooksV1* mutateHooks = nullptr;
    HRESULT queryStatus = S_OK;
    std::atomic<bool> blockQuery{false}, queryEntered{false}, allowQuery{true};
    std::atomic<bool> badHookOrder{false};
};

bool loadedGraphics(void* context, std::uint64_t base) noexcept {
    auto& peer = *static_cast<Peer*>(context);
    ++peer.loadedCalls;
    if (peer.retainCalls != 1) peer.badHookOrder = true;
    return base == 0x12345000 && peer.loaded;
}
bool exportOwnModule(void* context, std::uint64_t base) noexcept {
    auto& peer = *static_cast<Peer*>(context);
    ++peer.exportCalls;
    if (peer.retainCalls != 1 || peer.loadedCalls != 1) peer.badHookOrder = true;
    return base == 0x12345000 && peer.ownExport;
}
HRESULT queryContract(void* context, std::uint64_t base, GraphicsContractV1* out) noexcept {
    auto& peer = *static_cast<Peer*>(context);
    ++peer.queryCalls;
    if (peer.retainCalls != 1 || peer.loadedCalls != 1 || peer.exportCalls != 1)
        peer.badHookOrder = true;
    if (base != 0x12345000 || !out) return E_INVALIDARG;
    if (peer.blockQuery) {
        peer.queryEntered = true;
        while (!peer.allowQuery.load()) std::this_thread::yield();
    }
    if (peer.mutateRequest) {
        peer.mutateRequest->snapshot.reserved[0] = 0xA55Au;
        peer.mutateRequest->decision.selectedPluginMask ^= 1u;
    }
    if (peer.mutateHooks) {
        peer.mutateHooks->context = nullptr;
        peer.mutateHooks->releaseGraphics = nullptr;
    }
    if (peer.queryStatus == S_OK) *out = peer.contract;
    return peer.queryStatus;
}
bool retainGraphics(void* context, std::uint64_t base) noexcept {
    auto& peer = *static_cast<Peer*>(context);
    ++peer.retainCalls;
    if (peer.loadedCalls != 0 || peer.exportCalls != 0 || peer.queryCalls != 0)
        peer.badHookOrder = true;
    return base == 0x12345000 && peer.retain;
}
void releaseGraphics(void* context, std::uint64_t base) noexcept {
    auto& peer = *static_cast<Peer*>(context);
    if (base == 0x12345000) ++peer.releaseCalls;
}
PeerHooksV1 hooks(Peer& peer) {
    return {&peer, exportOwnModule, loadedGraphics, queryContract, retainGraphics, releaseGraphics};
}
void useContract(Peer& peer) {
    peer.contract = test_seam::contractWithCatalog(catalog);
}
HRESULT commit(CommitStoreV1& store, const AdmissionCommitRequestV1& value,
               const PeerHooksV1& peerHooks) {
    return store.commitWithCatalogForTest(value, catalog, peerHooks);
}

void wireAndValidProfiles() {
    check(sizeof(AdmissionCommitRequestV1) == 168 &&
          offsetof(AdmissionCommitRequestV1, snapshot) == 24 &&
          offsetof(AdmissionCommitRequestV1, decision) == 104,
          "request keeps the fixed 168-byte ABI");
    for (auto profile : {PluginSelectionProfileV1::Vr, PluginSelectionProfileV1::Flat}) {
        Peer peer{}; useContract(peer);
        const auto value = request(profile, 0); // Empty masks are valid source records.
        {
            CommitStoreV1 store;
            check(commit(store, value, hooks(peer)) == S_OK,
                  "ready synthetic catalog admits an empty selection for each profile");
            AdmissionCommitRequestV1 readback{};
            check(store.read(&readback) && std::memcmp(&readback, &value, sizeof(value)) == 0,
                  "committed request is available as the immutable late-startup snapshot");
            check(peer.loadedCalls == 1 && peer.exportCalls == 1 && peer.queryCalls == 1 &&
                  peer.retainCalls == 1 && peer.releaseCalls == 0 && !peer.badHookOrder,
                  "first success retains before every peer qualification callback");
            check(commit(store, value, hooks(peer)) == S_OK && peer.queryCalls == 1 &&
                  peer.retainCalls == 1,
                  "identical repeat succeeds without repeating peer work");
            auto different = value;
            different.snapshot.recordedPluginMask = 1;
            different.decision.selectedPluginMask = 1;
            check(commit(store, different, hooks(peer)) == E_ACCESSDENIED && store.read(&readback),
                  "different repeat is rejected without clearing the committed request");
        }
        check(peer.releaseCalls == 1, "store teardown releases its one owned provider reference");
    }
}

void rejectMalformedAndUnreadyBeforePeerCalls() {
    Peer peer{}; useContract(peer);
    CommitStoreV1 store;
    auto malformed = request(PluginSelectionProfileV1::Vr);
    malformed.snapshot.reserved[0] = 1;
    check(commit(store, malformed, hooks(peer)) == E_INVALIDARG && peer.queryCalls == 0 &&
          peer.retainCalls == 0,
          "malformed full source snapshot is rejected before peer callbacks");

    test_seam::GroupV1 unreadyGroups[] = {{0, 3, 3}, {1, 1, 0}, {2, 2, 2}};
    test_seam::CatalogViewV1 unready{unreadyGroups, 3, 2, fingerprint};
    const auto src = snapshot(PluginSelectionProfileV1::Vr);
    auto candidate = test_seam::decideWithCatalog(S_OK, &src,
        PluginSelectionProfileV1::Vr, 0x12345000, unready);
    malformed = request(PluginSelectionProfileV1::Vr);
    malformed.snapshot = src;
    malformed.decision = candidate;
    malformed.supportedProfileMask = test_seam::contractWithCatalog(unready).supportedProfileMask;
    check(store.commitWithCatalogForTest(malformed, unready, hooks(peer)) == E_INVALIDARG &&
          peer.queryCalls == 0 && peer.retainCalls == 0,
          "not-ready profile never consults or retains a peer");
}

void everyPrepublishFailureUnwinds() {
    const auto value = request(PluginSelectionProfileV1::Vr, 1);
    for (unsigned failure = 0; failure != 5; ++failure) {
        Peer peer{}; useContract(peer);
        if (failure == 0) peer.loaded = false;
        if (failure == 1) peer.ownExport = false;
        if (failure == 2) peer.queryStatus = E_FAIL;
        if (failure == 3) peer.contract.reserved = 1;
        if (failure == 4) peer.retain = false;
        CommitStoreV1 store;
        check(commit(store, value, hooks(peer)) == E_ACCESSDENIED,
              "peer preflight or retain failure does not publish");
        AdmissionCommitRequestV1 readback{};
        check(!store.read(&readback) && peer.releaseCalls == (failure == 4 ? 0u : 1u) &&
              peer.retainCalls == 1 && !peer.badHookOrder,
              "failed prepublish attempt releases exactly the durable reference it acquired");
        peer.loadedCalls.store(0);
        peer.exportCalls.store(0);
        peer.queryCalls.store(0);
        peer.retainCalls.store(0);
        peer.badHookOrder.store(false);
        peer.loaded = peer.ownExport = peer.retain = true;
        peer.queryStatus = S_OK;
        useContract(peer);
        check(commit(store, value, hooks(peer)) == S_OK,
              "failed attempt fully returns the store to Empty for retry");
    }
}

void concurrentPreparingDoesNotWaitOrPublishEarly() {
    Peer peer{}; useContract(peer);
    peer.blockQuery = true;
    peer.allowQuery = false;
    CommitStoreV1 store;
    const auto value = request(PluginSelectionProfileV1::Flat);
    HRESULT first = E_FAIL;
    std::thread worker([&] { first = commit(store, value, hooks(peer)); });
    while (!peer.queryEntered.load()) std::this_thread::yield();
    check(commit(store, value, hooks(peer)) == E_PENDING,
          "concurrent call sees Preparing and returns E_PENDING without waiting on peer query");
    AdmissionCommitRequestV1 readback{};
    check(!store.read(&readback), "Preparing state is not visible as a committed snapshot");
    peer.allowQuery = true;
    worker.join();
    check(first == S_OK && store.read(&readback), "preparing owner publishes after successful peer retain");
}

void callbackMutationCannotChangeFrozenCommit() {
    Peer peer{}; useContract(peer);
    auto callerValue = request(PluginSelectionProfileV1::Vr, 1);
    const auto frozenValue = callerValue;
    peer.mutateRequest = &callerValue;
    auto mutableHooks = hooks(peer);
    peer.mutateHooks = &mutableHooks;
    {
        CommitStoreV1 store;
        check(commit(store, callerValue, mutableHooks) == S_OK,
              "callback mutation of caller request and hooks cannot invalidate frozen inputs");
        AdmissionCommitRequestV1 readback{};
        check(store.read(&readback) && std::memcmp(&readback, &frozenValue, sizeof(frozenValue)) == 0,
              "store publishes the exact validated copy captured before callbacks");
        check(commit(store, frozenValue, hooks(peer)) == S_OK && peer.queryCalls == 1,
              "repeat of the original frozen value is still idempotent after callback mutation");
    }
    check(peer.releaseCalls == 1,
          "frozen release hook unwinds the owned reference after caller hook mutation");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "--dry-run") != 0 &&
                      std::strcmp(argv[1], "--self-test") != 0)) {
        std::fprintf(stderr, "usage: plugin_admission_commit_test.exe --dry-run|--self-test\n");
        return 2;
    }
    wireAndValidProfiles();
    rejectMalformedAndUnreadyBeforePeerCalls();
    everyPrepublishFailureUnwinds();
    concurrentPreparingDoesNotWaitOrPublishEarly();
    callbackMutationCannotChangeFrozenCommit();
    std::puts("plugin_admission_commit_test: OK");
    return 0;
}
