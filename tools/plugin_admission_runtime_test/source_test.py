#!/usr/bin/env python3
"""Source gates for the dormant graphics/native admission integration fixture."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def load(root):
    paths = {
        "graphics_transport": root / "src/d3d11/plugin_admission_transport.cpp",
        "graphics_header": root / "src/d3d11/plugin_admission_transport.h",
        "proxy": root / "src/d3d11/d3d11_proxy.cpp",
        "native_client": root / "src/openxr/native_graphics_client.h",
        "native_module": root / "src/openxr/native_module.cpp",
        "native_admission": root / "src/openxr/native_plugin_admission.cpp",
        "native_def": root / "src/openxr/native_module.def",
        "pe_check": root / "tools/openxr_pe.py",
        "ordinal_fixture": root / "tools/openxr_module_test/openxr_module_test.cpp",
        "runtime_fixture": root / "tools/plugin_admission_runtime_test/runtime_test.cpp",
        "graphics_fixture": root / "tools/plugin_admission_runtime_test/runtime_graphics_fixture.cpp",
        "graphics_fixture_def": root / "tools/plugin_admission_runtime_test/runtime_graphics_fixture.def",
        "peer_fixture": root / "tools/plugin_admission_runtime_test/native_peer.cpp",
        "provider_fixture": root / "tools/plugin_admission_runtime_test/provider.cpp",
    }
    return {key: path.read_text(encoding="utf-8") for key, path in paths.items()}


def body(source, marker):
    start = source.find(marker)
    if start < 0:
        raise AssertionError("missing function/source marker: " + marker)
    opening = source.find("{", start)
    if opening < 0:
        raise AssertionError("missing function body: " + marker)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError("unterminated function body: " + marker)


def ordered(source, *needles):
    positions = [source.find(needle) for needle in needles]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError("missing or reordered source fragments: " + repr(needles))


def check(sources):
    graphics = sources["graphics_transport"]
    query = body(graphics, "extern \"C\" HRESULT WINAPI edvrQueryPluginAdmissionV1(")
    contract_branch = query[query.find("QueryKindV1::Contract"):query.find("QueryKindV1::Decision")]
    forbidden = ("ensureInitialised", "InitOnceExecuteOnce", "Config::get", "Log::get",
                 "captureFromModuleDirectory", "AdmissionPeer", "GetProcAddress", "wait_for")
    if any(token in contract_branch for token in forbidden):
        raise AssertionError("Contract query must stay pure and independent of the initializer and peers")
    ordered(contract_branch, "admission_v1::contract()", "std::memcpy(output")
    decision_branch = query[query.find("QueryKindV1::Decision"):]
    ordered(decision_branch, "g_decisionReady.load(std::memory_order_acquire)",
            "std::memcpy(output")
    candidate = body(graphics, "DecisionV1 candidateWithNativeCommit(")
    ordered(candidate, "candidate.mode != static_cast<std::uint32_t>(Mode::AdmittedSelection)",
            "AdmissionPeer::acquire", "commit(&request)")
    if "profile != PluginSelectionProfileV1::Vr" not in candidate:
        raise AssertionError("Flat must remain on the graphics-only route")
    publish = body(graphics, "void publishCandidate(")
    ordered(publish, "if (g_decisionReady.load", "admission_v1::decide(",
            "g_publishedDecision = decision", "g_decisionReady.store(true, std::memory_order_release)")

    proxy = sources["proxy"]
    callback = body(proxy, "BOOL CALLBACK initOnceCallback(")
    ordered(callback, "captureFromModuleDirectory", "publishCandidate")

    client = sources["native_client"]
    acquire = body(client, "HRESULT acquire(const std::wstring& trustedAbsolutePath,")
    ordered(acquire, "if (expectedAdmission)", "expectedBase", "reinterpret_cast<std::uint64_t>(retained) != expectedBase",
            "AdmissionPeer::acquire", "GetProcAddress(retained, name)",
            "reinterpret_cast<decltype(&edvrAcquireNativeGraphics)>(snapshot)")
    ordered(acquire, "std::memcmp(&decision, expectedAdmission", "GetProcAddress(retained, name)")
    ordered(acquire, "if (queried != S_OK) return FAILED(queried) ? queried : E_ACCESSDENIED;",
            "publishedLegacyForProvider(decision, retained)", "GetProcAddress(retained, name)")
    legacy = body(client, "static bool publishedLegacyForProvider(")
    ordered(legacy,
            "if (decision.reason == static_cast<std::uint32_t>(Reason::ProviderRejected))\n      return false;",
            "if (base != reinterpret_cast<std::uint64_t>(provider)) return false;",
            "if (decision.reason == static_cast<std::uint32_t>(Reason::SnapshotUnavailable))")

    native = sources["native_module"]
    start = body(native, "vr::EVRInitError start(")
    ordered(start, "admissionBuildEnabled", "readCommitted(&admissionReservation)",
            "generation->binding.acquire", "generation->host=std::make_unique<NativeRuntimeHost>")
    if "admissionBuildEnabled && !hasAdmissionReservation" not in start:
        raise AssertionError("unreserved provider guard must run only when this build has admission support")

    native_def = sources["native_def"]
    for ordinal in range(1, 17):
        if "@%d" % ordinal not in native_def:
            raise AssertionError("historical/private export ordinal %d was removed" % ordinal)
    if "edvrCommitPluginSelectionAdmissionV1 @17" not in native_def:
        raise AssertionError("native admission commit must append at ordinal 17")
    pe = sources["pe_check"]
    ord_fixture = sources["ordinal_fixture"]
    if "edvrCommitPluginSelectionAdmissionV1" not in pe or \
       "edvrCommitPluginSelectionAdmissionV1" not in ord_fixture:
        raise AssertionError("PE parser and runtime ordinal fixture must include the appended commit export")

    fixture = sources["runtime_fixture"]
    scratch = body(fixture, "fs::path makeRoot(")
    ordered(scratch, "modulePath(nullptr)", "fs::canonical(fs::path(runnerPath))",
            "fs::canonical(fs::path(fixturePath))", "fixture.parent_path() != base",
            "fs::create_directory(root)")
    if "GetTempPathW" in scratch:
        raise AssertionError("runtime scratch must use the validated writable build directory")
    for label in (
        "pure Contract query completes within a finite timeout while graphics init is held",
        "reserved HMODULE mismatch rejects before paired graphics exports",
        "missing or late admission Decision rejects before paired exports",
        "S_FALSE admission query is rejected before paired exports",
        "malformed late Decision is rejected before paired exports",
        "Legacy response cannot downgrade an earlier positive reservation",
        "matching late Decision passes the guard and reaches the paired export",
        "valid generated VR selection with rejected native peer falls back to Legacy",
        "failed native commit produces provider-zero Legacy and strict unreserved startup rejects before paired exports",
        "missing native peer publishes provider-zero Legacy for a valid VR selection",
        "missing native peer produces provider-zero Legacy and strict unreserved startup rejects before paired exports",
        "absent VR source record publishes provider-backed Legacy",
        "opaque VR source record publishes provider-backed Legacy",
        "stale VR catalog schema publishes provider-backed Legacy",
        "provider-backed VR source Legacy passes the native guard and reaches the paired graphics callback",
        "unknown selected ID remains a provider-backed Legacy source record",
        "actual Flat transport admits selected Temporal-AA without Diagnostics",
        "actual Flat transport accepts an explicit empty selection",
        "Flat/profile mismatch is a provider-backed Legacy record",
        "Flat selection rejects a VR-only plugin while preserving Legacy owners",
        "opaque Flat source record stays Legacy with the real provider identity",
        "stale catalog schema stays an opaque provider-backed Legacy record",
        "absent Flat source record preserves Legacy owners",
        "real VR capture and publish reaches the generated-catalog peer commit",
        "generated VR Diagnostics selection publishes only after validated peer commit",
        "unpublished Deferred decision blocks Performance startup",
        "published Legacy decision retains the active profile's Performance owner fallback",
        "published Legacy rejects an unknown out-of-range startup owner",
        "startup-owner queries do not call optional paired graphics exports",
        "exact loaded fake DLL commits a validated immutable reservation",
        "committed peer reference keeps the graphics provider mapped",
        "explicit fixture reset releases the retained graphics reference outside DLL teardown",
        "native peer can unload after its reservation is reset",
        "unpublished Decision rejects unreserved startup before paired exports",
        "published validated Legacy permits unreserved paired graphics startup",
    ):
        if label not in fixture:
            raise AssertionError("runtime fixture omits required assertion: " + label)
    for required in ("GetTickCount64() + 9000", "std::_Exit(9)",
                     "FixturePublishCandidate", "FixturePublishFlatCandidate",
                     "AdmissionPeer::acquire"):
        if required not in fixture:
            raise AssertionError("runtime fixture omits bounded real-DLL seam: " + required)
    ordered(fixture, "ScopedModule peerModule(", "const auto reset =", "ResetScope",
            "AdmissionPeer peer =", "reset();", "resetScope.disarm();",
            "peerModule.release()")
    ordered(fixture, "returnedDecision.reserved[0] = 1;",
            "setDecision(S_OK, &returnedDecision);", "NativeGraphicsClient malformed;")
    if "const unsigned beforePairs = pairCount();" not in fixture or \
       "const unsigned pairDelta = afterPairs - beforePairs;" not in fixture:
        raise AssertionError("paired-export assertions must use per-call deltas")
    if "expected.abiVersion = admission_v1::kAbiVersion" not in fixture or \
       "commit.abiVersion = commit_v1::kAbiVersion" not in fixture:
        raise AssertionError("fixture must distinguish decision and commit ABI versions")
    if "~ResetScope() { if (function) function(); }" not in fixture:
        raise AssertionError("exceptional exit must reset a positive fake peer before DLL unload")
    for name in ("edvrAcquireNativeGraphics", "edvrAcquireGraphicsBridge",
                 "edvrAcquireRenderBoundary"):
        for source in (sources["graphics_fixture"], sources["provider_fixture"]):
            marker = "extern \"C\" HRESULT WINAPI " + name + "("
            signature = source.split(marker, 1)[1].split("{", 1)[0]
            if "noexcept" in signature:
                raise AssertionError("paired export fixture conflicts with shared C header: " + name)
    if "FixturePublishCandidate" not in sources["graphics_fixture"] or \
       "FixturePublishFlatCandidate" not in sources["graphics_fixture"] or \
       "publishCandidate(directory, graphics" not in sources["graphics_fixture"]:
        raise AssertionError("generated-profile cases must call production publishCandidate")
    if "FixtureOwnerAllowedForStartup" not in sources["graphics_fixture_def"] or \
       "FixtureOwnerAllowedForStartup" not in sources["graphics_fixture"] or \
       "plugin_admission_transport::ownerAllowedForStartup" not in sources["graphics_fixture"]:
        raise AssertionError("runtime fixture must export the actual graphics transport startup predicate")
    if "FixturePublishFlatCandidate" not in sources["graphics_fixture_def"]:
        raise AssertionError("Flat production candidate route must be exported to the runtime fixture")
    owner_case = body(fixture, "void generatedLegacy(")
    ordered(owner_case, "FixtureOwnerAllowedForStartup",
            "check(!ownerAllowed(kPerformancePluginIndex)", "publish(directory.c_str()",
            "check(ownerAllowed(kPerformancePluginIndex)",
            "check(!ownerAllowed(0xffffffffu)",
            "pairCalls() == 0", "count() == expectedPeerCalls")
    ordered(owner_case, "expectedReason == Reason::ProviderRejected",
            "unreservedStartup.acquire(", "graphicsPath, nullptr, true);",
            "providerBase == 0 && startup == E_ACCESSDENIED &&", "pairDelta == 0")
    missing_peer = body(fixture, "void generatedVrMissingPeer(")
    ordered(missing_peer, 'writeState(root, "performance", "vr")',
            'root / L"Openvr" / L"win64"', "FixturePublishCandidate",
            "Reason::ProviderRejected", "providerBase == 0",
            "unreservedStartup.acquire(graphicsPath, nullptr, true)",
            "startup == E_ACCESSDENIED && pairCalls() - beforePairs == 0")
    vr_source = body(fixture, "void generatedVrSourceLegacyCase(")
    ordered(vr_source, "FixturePublishCandidate", "Reason::InvalidSnapshot",
            "reinterpret_cast<std::uintptr_t>(graphicsModule.get())",
            "acceptedLegacy.acquire(graphicsPath, nullptr, true)",
            "startup == E_FAIL && pairCalls() - beforePairs == 1")
    main = body(fixture, "int wmain(")
    ordered(main, "generatedVrMissingPeer(graphics)",
            "VrSourceKind::Missing", "VrSourceKind::Opaque",
            "VrSourceKind::StaleSchema")
    flat_case = body(fixture, "void generatedFlatCase(")
    ordered(flat_case, "FixturePublishFlatCandidate", "publish(root.c_str()",
            "decision.activeProfile", "decision.selectedPluginMask",
            "providerBase == static_cast<std::uint64_t>", "pairCalls() == 0")
    peer_case = body(fixture, "void generatedVrPeerCommit(")
    ordered(peer_case, "FixtureUseGeneratedCatalogForCommit", "useGenerated();",
            "publish(directory.c_str()", "Mode::AdmittedSelection",
            "commits() == 1 && readCommit(&stored)", "resetCommit();")
    if "store.commitWithCatalogForTest" not in sources["peer_fixture"]:
        raise AssertionError("positive DLL fixture must call the actual commit store through its test catalog seam")
    if "FixtureResetCommit" not in sources["peer_fixture"]:
        raise AssertionError("fixture must reset its committed reference before native DLL teardown")
    if "static CommitStoreV1* const value = new (std::nothrow) CommitStoreV1" not in sources["native_admission"]:
        raise AssertionError("production store must quarantine its provider reference through DLL teardown")
    if "contractWithCatalog(catalog)" not in sources["provider_fixture"]:
        raise AssertionError("positive fake provider must publish the matching synthetic native contract")


def self_test(sources):
    check(sources)
    mutants = []
    mutants.append(dict(sources, graphics_transport=sources["graphics_transport"].replace(
        "const Contract value = edvr::plugin_selection::admission_v1::contract();",
        "ensureInitialised(); const Contract value = edvr::plugin_selection::admission_v1::contract();", 1)))
    mutants.append(dict(sources, native_client=sources["native_client"].replace(
        "if (reinterpret_cast<std::uint64_t>(retained) != expectedBase)",
        "if (reinterpret_cast<std::uint64_t>(retained) == expectedBase)", 1)))
    mutants.append(dict(sources, graphics_transport=sources["graphics_transport"].replace(
        "candidate.mode != static_cast<std::uint32_t>(Mode::AdmittedSelection)",
        "candidate.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection)", 1)))
    mutants.append(dict(sources, native_def=sources["native_def"].replace(
        "edvrCommitPluginSelectionAdmissionV1 @17", "edvrCommitPluginSelectionAdmissionV1 @16", 1)))
    mutants.append(dict(sources, runtime_fixture=sources["runtime_fixture"].replace(
        "resetScope.disarm();", "/* reset guard still armed */", 1)))
    mutants.append(dict(sources, graphics_fixture=sources["graphics_fixture"].replace(
        "const EdvrNativeGraphicsRequest*, EdvrNativeGraphicsTable*) {",
        "const EdvrNativeGraphicsRequest*, EdvrNativeGraphicsTable*) noexcept {", 1)))
    mutants.append(dict(sources, native_client=sources["native_client"].replace(
        "if (queried != S_OK) return FAILED(queried) ? queried : E_ACCESSDENIED;",
        "if (queried == S_OK) return E_ACCESSDENIED;", 1)))
    mutants.append(dict(sources, native_client=sources["native_client"].replace(
        "if (decision.reason == static_cast<std::uint32_t>(Reason::ProviderRejected))\n      return false;",
        "if (decision.reason == static_cast<std::uint32_t>(Reason::ProviderRejected))\n      return true;", 1)))
    mutants.append(dict(sources, native_client=sources["native_client"].replace(
        "if (base != reinterpret_cast<std::uint64_t>(provider)) return false;",
        "if (base == reinterpret_cast<std::uint64_t>(provider)) return false;", 1)))
    mutants.append(dict(sources, runtime_fixture=sources["runtime_fixture"].replace(
        "commit.abiVersion = commit_v1::kAbiVersion",
        "commit.abiVersion = kAbiVersion", 1)))
    mutants.append(dict(sources, runtime_fixture=sources["runtime_fixture"].replace(
        "fixture.parent_path() != base", "false", 1)))
    mutants.append(dict(sources, runtime_fixture=sources["runtime_fixture"].replace(
        "returnedDecision.reserved[0] = 1;\n                    setDecision(S_OK, &returnedDecision);",
        "setDecision(S_OK, &returnedDecision);\n                    returnedDecision.reserved[0] = 1;", 1)))
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
        print("plugin_admission_runtime_source_test: FAIL: %s" % error, file=sys.stderr)
        return 1
    if args.self_test:
        print("plugin_admission_runtime_source_test: OK (%d in-memory mutations)" % count)
    else:
        print("plugin_admission_runtime_source_test: dry-run OK (source inputs read; no files written)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
