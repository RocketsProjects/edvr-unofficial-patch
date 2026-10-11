#!/usr/bin/env python3
"""Read-only selection snapshot transport and startup-order source pins."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def body(source, signature):
    start = source.find(signature)
    if start < 0:
        raise AssertionError("missing scope: " + signature)
    opening = source.find("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:pos]
    raise AssertionError("unterminated scope: " + signature)


def ordered(source, *needles):
    positions = [source.find(needle) for needle in needles]
    if any(pos < 0 for pos in positions) or positions != sorted(positions):
        raise AssertionError("missing or reordered fragments: " + repr(needles))


def load(root):
    paths = {
        "proxy": root / "src/d3d11/d3d11_proxy.cpp",
        "transport_h": root / "src/d3d11/plugin_selection_transport.h",
        "transport": root / "src/d3d11/plugin_selection_transport.cpp",
        "module": root / "src/openxr/native_module.cpp",
        "build": root / "build.bat",
        "pe": root / "tools/openxr_pe.py",
        "module_def": root / "src/openxr/native_module.def",
    }
    return {key: path.read_text(encoding="utf-8") for key, path in paths.items()}


def check(sources):
    proxy_init = body(sources["proxy"], "BOOL CALLBACK initOnceCallback(")
    ordered(proxy_init, "cfg.init(", "Log::get().open(",
            "captureFromModuleDirectory(*g_moduleDir)",
            "registerPolicy(&policy)")
    if proxy_init.count("captureFromModuleDirectory(*g_moduleDir)") != 1:
        raise AssertionError("proxy must capture one selection snapshot per InitOnce")

    transport = sources["transport"]
    if "L" + '"\\\\edvr_install\\\\state.ini"' not in transport:
        raise AssertionError("capture must read the installer state.ini path")
    if "1024u * 1024u" not in sources["transport_h"] or \
            "kMaxRuntimeStateBytes" not in transport or "CreateFileW" not in transport:
        raise AssertionError("capture must use the bounded 1 MiB Win32 reader")
    if "std::unique_ptr<void, FileCloser> fileOwner(file)" not in transport:
        raise AssertionError("state reader must close its handle on every exit path")
    if "ERROR_FILE_NOT_FOUND" not in transport or "ERROR_PATH_NOT_FOUND" not in transport:
        raise AssertionError("only missing file/path may map to valid Absent")
    read_result = body(transport, "HRESULT decodeStateFileReadResultV1(")
    ordered(read_result, "if (FAILED(readStatus))", "if (missing)",
            "byteCount > kMaxRuntimeStateBytes")
    if read_result.count("decodePluginSelectionSnapshotV1(") != 2:
        raise AssertionError("missing and present state paths must both use the shared decoder")
    query = body(transport, "extern \"C\" HRESULT WINAPI edvrQueryPluginSelectionV1(")
    ordered(query, "if (version !=", "if (!published.ready)",
            "if (FAILED(status))", "copyPluginSelectionSnapshotV1(")

    module_start = body(sources["module"], "vr::EVRInitError start(")
    ordered(module_start, "const HRESULT acquired=", "if (acquired!=S_OK)",
            "queryPluginSelectionSnapshot(", "waitForRender(",
            "make_unique<NativeRuntimeHost>")
    if "recordedPluginMask" in module_start:
        raise AssertionError("transport-only host startup must not consume selection mask")
    if "hasPluginSelectionSnapshot" not in module_start:
        raise AssertionError("query status and copied snapshot must stay on the generation")
    if "queryPluginSelectionSnapshot(\n        generation->binding.provider(), &generation->pluginSelectionSnapshot)" not in module_start:
        raise AssertionError("query must copy into the generation-owned snapshot")
    if "edvrQueryPluginSelectionV1" not in sources["build"] or \
            "edvrQueryPluginSelectionV1" not in sources["pe"]:
        raise AssertionError("graphics export must be in build and PE required-export gates")
    for source in ("src\\common\\plugin_selection_snapshot.cpp",
                   "src\\common\\iniedit.cpp",
                   "src\\d3d11\\plugin_selection_transport.cpp",
                   "plugin_selection_transport_test.cpp"):
        if source not in sources["build"]:
            raise AssertionError("build recipe is missing transport dependency: " + source)
    if "`r`n" in sources["build"]:
        raise AssertionError("build recipe contains a literal PowerShell newline escape")
    if "edvrQueryPluginSelectionV1" in sources["module_def"]:
        raise AssertionError("selection query must not change OpenVR ordinal exports")


def self_test(sources):
    check(sources)
    mutations = []
    proxy = sources["proxy"]
    mutations.append(dict(sources, proxy=proxy.replace(
        "    (void)edvr::plugin_selection_transport::captureFromModuleDirectory(*g_moduleDir);\n", "", 1)))
    capture = "    (void)edvr::plugin_selection_transport::captureFromModuleDirectory(*g_moduleDir);\n"
    moved = proxy.replace(capture, "", 1).replace(
        "    return TRUE;", capture + "    return TRUE;", 1)
    mutations.append(dict(sources, proxy=moved))
    module = sources["module"]
    query_block = ("    generation->pluginSelectionQueryStatus = queryPluginSelectionSnapshot(\n"
                   "        generation->binding.provider(), &generation->pluginSelectionSnapshot);\n"
                   "    generation->hasPluginSelectionSnapshot = SUCCEEDED(generation->pluginSelectionQueryStatus);\n")
    mutations.append(dict(sources, module=module.replace(query_block, "", 1)))
    mutations.append(dict(sources, module=module.replace(
        "generation->binding.provider(), &generation->pluginSelectionSnapshot",
        "generation->binding.provider(), nullptr", 1)))
    mutations.append(dict(sources, module=module.replace(
        "generation->hasPluginSelectionSnapshot = SUCCEEDED(generation->pluginSelectionQueryStatus);",
        "if (generation->pluginSelectionSnapshot.recordedPluginMask) generation->hasPluginSelectionSnapshot = true;", 1)))
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
        print("plugin_selection_transport_source_test: FAIL: %s" % error, file=sys.stderr)
        return 1
    if args.self_test:
        print("plugin_selection_transport_source_test: OK (%d in-memory mutations)" % count)
    else:
        print("plugin_selection_transport_source_test: dry-run OK (no files read or written beyond source inputs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
