#!/usr/bin/env python3
"""Read-only source checks for exact loaded-module admission peer qualification."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def load(root):
    paths = {
        "header": root / "src/common/plugin_admission_peer.h",
        "helper": root / "src/common/plugin_admission_peer.cpp",
        "fixture": root / "tools/plugin_admission_peer_test/plugin_admission_peer_test.cpp",
        "provider_def": root / "tools/plugin_admission_peer_test/provider.def",
        "old_def": root / "tools/plugin_admission_peer_test/old_peer.def",
        "forwarder_def": root / "tools/plugin_admission_peer_test/forwarder.def",
    }
    return {name: path.read_text(encoding="utf-8") for name, path in paths.items()}


def ordered(source, *needles):
    positions = [source.find(needle) for needle in needles]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError("missing or reordered fragments: " + repr(needles))


def check(sources):
    header = sources["header"]
    helper = sources["helper"]
    fixture = sources["fixture"]
    if "AdmissionPeer(const AdmissionPeer&) = delete" not in header:
        raise AssertionError("peer must be move-only and own its module reference")
    if "GetModuleHandleExW(0, expected.c_str(), &module)" not in helper:
        raise AssertionError("peer must acquire a reference by exact path without loading")
    if "GetModuleFileNameW" not in helper or "samePath(expected, canonicalMapped)" not in helper:
        raise AssertionError("mapped module path must be checked against the requested canonical path")
    if "input.find(L'\\0') != std::wstring::npos" not in helper:
        raise AssertionError("embedded NUL must be rejected before Win32 path lookup")
    if "LoadLibrary" in helper:
        raise AssertionError("production peer helper must not load a module")
    ordered(helper, "hasDirectExport(module, exportName)", "GetProcAddress(module, exportName)",
            "belongsToModule(proc, module)")
    if "GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS" not in helper or "GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT" not in helper:
        raise AssertionError("resolved export ownership must be checked without taking another reference")
    if "functionRva < exportBegin || functionRva >= exportEnd" not in helper:
        raise AssertionError("forwarder RVAs inside the export directory must be rejected")
    if "directoryCount > (optionalBytes - directoryOffset) / sizeof(IMAGE_DATA_DIRECTORY)" not in helper:
        raise AssertionError("export directory count must fit the declared optional header")
    if "exports->NumberOfNames && (!exports->AddressOfNames || !exports->AddressOfNameOrdinals)" not in helper:
        raise AssertionError("nonempty export arrays must have nonzero RVAs")
    for needle in ("reject provider before it is loaded", "reject relative path",
                   "reject UNC path", "reject extended path", "reject embedded NUL",
                   "reject loaded old peer missing export", "reject forwarded export before loader resolution",
                   "rejecting forwarder does not map its target", "forwarded export address belongs to target module",
                   "duplicate basenames map as distinct modules",
                   "peer reference keeps provider mapped", "provider unloads after peer releases",
                   "rejected old peer unloads after its only explicit reference"):
        if needle not in fixture:
            raise AssertionError("missing Win32 fixture assertion: " + needle)
    if "FixtureAdmissionProbe" not in sources["provider_def"] or "FixtureAdmissionProbe" in sources["old_def"]:
        raise AssertionError("provider and old-peer fixture exports are malformed")
    if "FixtureAdmissionProbe=provider_fixture.FixtureAdmissionProbe" not in sources["forwarder_def"]:
        raise AssertionError("forwarder fixture must resolve to the same named provider export")
    if "edvrConfigureNativeRuntime" in helper or "edvrGetNativeRuntimeStatus" in helper:
        raise AssertionError("generic peer helper must not invoke production ABI exports")


def self_test(sources):
    check(sources)
    mutations = []
    mutations.append(dict(sources, helper=sources["helper"].replace(
        "if (!hasDirectExport(module, exportName)) {", "if (false) {", 1)))
    mutations.append(dict(sources, helper=sources["helper"].replace(
        "if (!proc || !belongsToModule(proc, module)) {", "if (!proc) {", 1)))
    mutations.append(dict(sources, helper=sources["helper"].replace(
        "GetModuleHandleExW(0, expected.c_str(), &module)",
        "GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, expected.c_str(), &module)", 1)))
    mutations.append(dict(sources, helper=sources["helper"].replace(
        "input.find(L'\\0') != std::wstring::npos ||", "", 1)))
    mutations.append(dict(sources, helper=sources["helper"].replace(
        "directoryCount > (optionalBytes - directoryOffset) / sizeof(IMAGE_DATA_DIRECTORY)",
        "false", 1)))
    for index, mutant in enumerate(mutations):
        try:
            check(mutant)
        except AssertionError:
            continue
        raise AssertionError("source mutation %d survived" % index)
    return len(mutations)


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
        print("plugin_admission_peer_source_test: FAIL: %s" % error, file=sys.stderr)
        return 1
    if args.self_test:
        print("plugin_admission_peer_source_test: OK (%d in-memory mutations)" % count)
    else:
        print("plugin_admission_peer_source_test: dry-run OK (source inputs read; no files written)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
