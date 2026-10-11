#!/usr/bin/env python3
"""Read-only source contract checks for the shared admission commit store."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def load(root):
    paths = {
        "header": root / "src/common/plugin_admission_commit.h",
        "store": root / "src/common/plugin_admission_commit.cpp",
        "native": root / "src/openxr/native_plugin_admission.cpp",
        "fixture": root / "tools/plugin_admission_commit_test/plugin_admission_commit_test.cpp",
    }
    return {name: path.read_text(encoding="utf-8") for name, path in paths.items()}


def ordered(text, *parts):
    positions = [text.find(part) for part in parts]
    if any(pos < 0 for pos in positions) or positions != sorted(positions):
        raise AssertionError("missing or reordered source fragments: " + repr(parts))


def check(sources):
    header, store, native, fixture = (sources[name] for name in
                                      ("header", "store", "native", "fixture"))
    if "sizeof(AdmissionCommitRequestV1) == 168" not in header or \
       "offsetof(AdmissionCommitRequestV1, snapshot) == 24" not in header or \
       "offsetof(AdmissionCommitRequestV1, decision) == 104" not in header:
        raise AssertionError("request wire size or offsets drifted")
    if "std::atomic<State> state_{State::Empty}" not in header:
        raise AssertionError("store state must be atomic and start Empty")
    if "const AdmissionCommitRequestV1 frozenRequest = request" not in store:
        raise AssertionError("caller request must be frozen before peer callbacks")
    if "const PeerHooksV1 frozenHooks = hooks" not in store:
        raise AssertionError("caller hooks must be frozen before peer callbacks")
    if "commitWithCatalogForTest" not in header or "#ifdef EDVR_ADMISSION_COMMIT_TEST" not in header:
        raise AssertionError("synthetic catalog seam must be fixture-only")
    if "commitWithCatalogForTest" in store and "generatedCatalog()" not in store:
        raise AssertionError("production commit must own its generated catalog")
    implementation = store.split("HRESULT CommitStoreV1::commitImpl", 1)[1].split(
        "bool CommitStoreV1::read", 1)[0]
    ordered(implementation, "const AdmissionCommitRequestV1 frozenRequest = request",
            "const PeerHooksV1 frozenHooks = hooks", "compare_exchange_strong",
            "validRequest(frozenRequest, catalog)",
            "frozenHooks.retainGraphics(frozenHooks.context, provider)",
            "frozenHooks.loadedGraphicsIsExpected(frozenHooks.context, provider)",
            "frozenHooks.queryExportOwnModule(frozenHooks.context, provider)",
            "frozenHooks.queryContract(frozenHooks.context, provider",
            "validPeerContract(peerContract, catalog)",
            "committed_ = frozenRequest", "state_.store(State::Committed")
    ordered(implementation, "if (retainedBase != 0)",
            "frozenHooks.releaseGraphics(frozenHooks.context, retainedBase)",
            "state_.store(State::Empty")
    if "std::memcmp(&expected, &request.decision, sizeof(expected)) == 0" not in store:
        raise AssertionError("candidate decision must be recomputed and byte-compared")
    if "std::memcmp(&actual, &expected, sizeof(expected)) == 0" not in store:
        raise AssertionError("peer contract must match the complete native contract")
    if "return E_PENDING" not in store or "std::memcmp(&committed_, &frozenRequest" not in store:
        raise AssertionError("Preparing and committed repeat semantics are missing")
    if "retainedHooks_ = frozenHooks" not in implementation:
        raise AssertionError("successful store must own the frozen release hook")
    if "static CommitStoreV1* const value = new (std::nothrow) CommitStoreV1" not in native:
        raise AssertionError("production committed ref must avoid CRT detach destructor")
    wrapper = native.split("extern \"C\" HRESULT WINAPI edvrCommitPluginSelectionAdmissionV1", 1)[1]
    ordered(wrapper, "if (!request) return E_INVALIDARG", "frozen = *request",
            "frozen.activeProfile", "store->commit(frozen, kPeerHooks)")
    for fragment in (
        "Empty masks are valid source records", "malformed full source snapshot",
        "not-ready profile never consults or retains a peer", "everyPrepublishFailureUnwinds",
        "identical repeat succeeds without repeating peer work",
        "different repeat is rejected without clearing the committed request",
        "concurrent call sees Preparing and returns E_PENDING",
        "owned reference", "each profile", "callback mutation of caller request and hooks",
        "exact validated copy captured before callbacks",
        "first success retains before every peer qualification callback",
        "failed prepublish attempt releases exactly the durable reference"):
        if fragment not in fixture:
            raise AssertionError("fixture is missing assertion text: " + fragment)


def self_test(sources):
    check(sources)
    mutants = []
    mutants.append(dict(sources, header=sources["header"].replace(
        "sizeof(AdmissionCommitRequestV1) == 168", "sizeof(AdmissionCommitRequestV1) == 160", 1)))
    mutants.append(dict(sources, store=sources["store"].replace(
        "validPeerContract(peerContract, catalog)", "true", 1)))
    mutants.append(dict(sources, store=sources["store"].replace(
        "return E_PENDING", "return S_OK", 1)))
    mutants.append(dict(sources, store=sources["store"].replace(
        "committed_ = frozenRequest", "committed_ = AdmissionCommitRequestV1{}", 1)))
    mutants.append(dict(sources, store=sources["store"].replace(
        "const AdmissionCommitRequestV1 frozenRequest = request;", "const auto& frozenRequest = request;", 1)))
    mutants.append(dict(sources, store=sources["store"].replace(
        "const PeerHooksV1 frozenHooks = hooks;", "const auto& frozenHooks = hooks;", 1)))
    mutants.append(dict(sources, store=sources["store"].replace(
        "if (retainedBase != 0)", "if (false)", 1)))
    mutants.append(dict(sources, native=sources["native"].replace(
        "new (std::nothrow) CommitStoreV1", "CommitStoreV1{}", 1)))
    mutants.append(dict(sources, native=sources["native"].replace(
        "frozen = *request", "frozen{}", 1)))
    for index, mutant in enumerate(mutants):
        try:
            check(mutant)
        except AssertionError:
            continue
        raise AssertionError("source mutation %d survived" % index)
    return len(mutants)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        sources = load(args.root)
        count = self_test(sources) if args.self_test else (check(sources) or 0)
    except (OSError, UnicodeError, AssertionError) as error:
        print("plugin_admission_commit_source_test: FAIL: %s" % error, file=sys.stderr)
        return 1
    if args.self_test:
        print("plugin_admission_commit_source_test: OK (%d in-memory mutations)" % count)
    else:
        print("plugin_admission_commit_source_test: dry-run OK (source inputs read; no files written)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
