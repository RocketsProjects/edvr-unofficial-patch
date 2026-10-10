#!/usr/bin/env python3
"""Pin grouped plugin lifecycle records to their production host callsites.

The native dispatch and module rigs exercise callbacks; these textual pins
protect host order, component IDs, staged fallback, and draw-boundary scope.
Mutants are applied in memory only.
"""
import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
PATHS = (
    ROOT / "src/plugins/comfort/scanners/scanner_lifecycle.h",
    ROOT / "src/plugins/comfort/scanners/scanner_lifecycle.cpp",
    ROOT / "src/plugins/comfort/exposure/exposure_lifecycle.cpp",
    ROOT / "src/plugins/comfort/comfort_lifecycle.h",
    ROOT / "src/plugins/comfort/comfort_lifecycle.cpp",
    ROOT / "src/plugins/explorer_cam/explorer_cam_lifecycle.cpp",
    ROOT / "src/d3d11/vscreen.cpp",
    ROOT / "src/d3d11/device_hook.cpp",
    ROOT / "src/d3d11/exposure_fix.cpp",
    ROOT / "build.bat",
    ROOT / "tools/build_flat_sdk_bench_proxy.py",
    ROOT / "src/d3d11/perf_monitor.cpp",
)


def body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing scope {signature}")
    opening = source.find("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:pos]
    raise AssertionError(f"unterminated scope {signature}")


def ordered(source: str, *needles: str) -> None:
    positions = [source.find(needle) for needle in needles]
    if any(p < 0 for p in positions) or positions != sorted(positions):
        raise AssertionError(f"missing or reordered fragments: {needles}")


def once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise AssertionError(f"mutation anchor count {source.count(old)}: {old}")
    return source.replace(old, new, 1)


def replace_body(source: str, signature: str, old: str, new: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function {signature}")
    opening = source.find("{", start)
    depth = 0
    closing = -1
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                closing = pos
                break
    if closing < 0:
        raise AssertionError(f"unterminated function {signature}")
    inner = once(source[opening + 1:closing], old, new)
    return source[:opening + 1] + inner + source[closing:]


def check(scanner_h: str, scanner: str, exposure: str, comfort_h: str,
          comfort: str, explorer: str, vscreen: str, device: str,
          exposure_core: str, build: str, flat_proxy: str, perf_monitor: str,
          registry_h: str) -> None:
    for token in ("kComfortExposureLifecycleComponent = 0",
                  "kComfortScannersLifecycleComponent = 1",
                  "kComfortTransitionFlashLifecycleComponent = 2"):
        # Component constants are declared in plugin_registry.h, checked via
        # production references and registry rig/build source coverage below.
        if token not in registry_h:
            raise AssertionError(f"missing stable Comfort component ID: {token}")
    for token in ("kScannerLifecycleConfigurePanel = 1",
                  "kScannerLifecycleConfigureReveal = 2",
                  "kScannerLifecycleRevealFrame = 3",
                  "kScannerLifecycleShutdownPanel = 4"):
        if token not in scanner_h:
            raise AssertionError(f"missing Scanner stage ID: {token}")

    cfg = body(scanner, "void configure(void* opaqueConfig)")
    ordered(cfg, "fssPanelConfigure(cfg)", "fssRevealConfigure(cfg)")
    if "if (!opaqueConfig) return;" not in cfg:
        raise AssertionError("Scanner configure must guard null config")
    if body(scanner, "void frame(void*, uint32_t)").count("fssRevealFrameBoundary()") != 1:
        raise AssertionError("Scanner frame must forward reveal exactly once")
    if body(scanner, "void shutdown(void*)").count("fssRevealShutdown()") != 1:
        raise AssertionError("Scanner final callback must shut down reveal exactly once")
    record = body(scanner, "const EdvrPluginLifecycleOps kScannerLifecycleOps =")
    for value in ("plugins::kPluginComfort", '"comfort"',
                  "edvr::kComfortScannersLifecycleComponent", "configure,", "frame,", "shutdown,"):
        if value not in record:
            raise AssertionError(f"Scanner record missing {value}")

    exp_record = body(exposure, "void initializeExposureLifecycleOps(")
    for value in ("plugins::kPluginComfort", "edvr::kComfortExposureLifecycleComponent",
                  "frame,", "shutdown,", "configureState"):
        if value not in exp_record:
            raise AssertionError(f"Exposure record missing {value}")

    if "kComfortLifecycleTransitionFlashFrame = 1" not in comfort_h:
        raise AssertionError("Transition Flash stage ID changed")
    c_record = body(comfort, "const EdvrPluginLifecycleOps kComfortLifecycleOps =")
    for value in ("plugins::kPluginComfort", '"comfort"',
                  "edvr::kComfortTransitionFlashLifecycleComponent", "configure,", "frame,", "shutdown,"):
        if value not in c_record:
            raise AssertionError(f"Transition Flash record missing {value}")
    if "transitionFlashEyeBaseFrameBoundary(frameNo)" not in body(comfort, "void frame(void*, uint32_t frameNo)"):
        raise AssertionError("Transition Flash normal frame callback was lost")

    x_record = body(explorer, "const EdvrPluginLifecycleOps kExplorerCamLifecycleOps =")
    for value in ("plugins::kPluginExplorerCam", '"explorer-cam"', "nullptr,", "frame,", "shutdown,"):
        if value not in x_record:
            raise AssertionError(f"Explorer record missing {value}")
    if "    nullptr,\n    frame,\n    shutdown," not in x_record:
        raise AssertionError("Explorer must own frame/shutdown only, with no configure callback")
    if "explorerCamFrameBoundary(frameNo)" not in body(explorer, "void frame(void*, uint32_t frameNo)"):
        raise AssertionError("Explorer normal frame callback missing")
    if "explorerCamShutdown()" not in body(explorer, "void shutdown(void*)"):
        raise AssertionError("Explorer cleanup callback missing")

    # Both host configure entry points retain stage order and independent
    # registration/fallback state for scanner and Transition Flash.
    stages = ("plugins::scanners::kScannerLifecycleConfigurePanel, cfg,\n        fssPanelConfigure);",
              "plugins::scanners::kScannerLifecycleConfigureReveal, cfg,\n        fssRevealConfigure);",
              "selectComfortLifecycle();", "configureComfortLifecycle(cfg);")
    for sig in ("void vScreenRefreshConfig()", "void installVScreenFixes("):
        site = body(vscreen, sig)
        ordered(site, "selectScannerLifecycle();", *stages)
        if site.count("configureScannerLifecycleStage(") != 2:
            raise AssertionError(f"{sig} must dispatch exactly two scanner configure stages")

    scanner_dispatch = body(vscreen, "void configureScannerLifecycleStage(")
    ordered(scanner_dispatch,
            "pluginRegistryConfigureLifecycleComponentStage(",
            "edvr::kComfortScannersLifecycleComponent", "return;", "fallback(cfg);")
    if scanner_dispatch.count("fallback(cfg);") != 1:
        raise AssertionError("Scanner configure fallback must occur exactly once")
    for name, accessor in (("ensureScannerLifecycle", "scannerLifecycleOps"),
                           ("ensureComfortLifecycle", "comfortLifecycleOps")):
        ensure = body(vscreen, f"bool {name}()")
        if "if (!g_state) return false;" not in ensure or "pluginRegistryRegisterLifecycle(" not in ensure or accessor not in ensure:
            raise AssertionError(f"{name} must reject missing VScreen state and return registry acceptance")
        if "pluginRegistryHasLifecycle" in ensure:
            raise AssertionError(f"{name} must use registration result, not a later registry lookup")
    frame_dispatch = body(vscreen, "void frameScannerLifecycleStage(")
    ordered(frame_dispatch, "pluginRegistryFrameLifecycleComponentStage(",
            "edvr::kComfortScannersLifecycleComponent", "return;", "fallback();")
    if frame_dispatch.count("fallback();") != 1:
        raise AssertionError("Scanner frame fallback must occur exactly once")
    reveal = body(body(vscreen, "void vScreenFrameBoundary()"), "tkFssReveal.run([&] {")
    if reveal.count("frameScannerLifecycleStage(") != 1:
        raise AssertionError("Scanner reveal callback escaped the FSS BoundaryTick")

    teardown = body(vscreen, "void shutdownVScreenFixes()")
    ordered(teardown, "pluginRegistryShutdown();",
            "shutdownScannerLifecycleStage(", "fssPanelRectShutdown();",
            "shutdownScannerLifecycle();")
    scanner_final = body(vscreen, "void shutdownScannerLifecycle()")
    ordered(scanner_final, "g_scannerLifecycleRegistered = false;",
            "pluginRegistryShutdownLifecycleComponent(", "return;", "fssRevealShutdown();")

    device_tick = body(device, "tkExplorerCam.run([] {")
    ordered(device_tick, "static_cast<uint32_t>(g_state->frameCounter)",
            "pluginRegistryHasLifecycle(plugins::kPluginExplorerCam)",
            "pluginRegistryFrameLifecycle(plugins::kPluginExplorerCam, frameNo)",
            "explorerCamFrameBoundary(frameNo)")
    if device_tick.count("explorerCamFrameBoundary(frameNo)") != 1:
        raise AssertionError("Explorer frame fallback must execute once")
    dev_shutdown = body(device, "void shutdownDeviceHooks()")
    ordered(dev_shutdown, "pluginRegistryHasLifecycle(plugins::kPluginExplorerCam)",
            "pluginRegistryShutdownLifecycle(plugins::kPluginExplorerCam)",
            "explorerCamShutdown();")
    transition_shutdown = dev_shutdown[dev_shutdown.find(
        "if (pluginRegistryHasLifecycleComponent("):]
    ordered(transition_shutdown, "if (pluginRegistryHasLifecycleComponent(",
            "edvr::kComfortTransitionFlashLifecycleComponent",
            "pluginRegistryShutdownLifecycleComponent(", "transitionFlashEyeBaseShutdown();")

    exp = body(exposure_core, "void exposureConfigure(Config& cfg)")
    ordered(exp, "pluginRegistryConfigureLifecycleComponent(",
            "edvr::kComfortExposureLifecycleComponent", "exposurePluginConfigure(state, cfg)")
    exp_frames = body(exposure_core, "void exposureFixFrameBoundary()")
    if exp_frames.count("pluginRegistryFrameLifecycleComponentStage(") != 2:
        raise AssertionError("Exposure must route both staged frame callbacks through component 0")
    if exp_frames.count("edvr::kComfortExposureLifecycleComponent") != 2:
        raise AssertionError("Exposure frame component routing changed")
    exp_shutdown = body(exposure_core, "void shutdownExposureFix()")
    ordered(exp_shutdown, "pluginRegistryShutdownLifecycleComponent(",
            "edvr::kComfortExposureLifecycleComponent", "exposurePluginShutdownResources(")

    # Per-draw handlers must stay free of registry/lifecycle dispatch.
    for sig in ("__declspec(noinline) void forwardVerdictBegin(",
                "__declspec(noinline) void forwardVerdictEnd("):
        if "Lifecycle" in body(vscreen, sig):
            raise AssertionError("registry lifecycle access entered a per-draw forwarder")

    required = ("src\\plugins\\comfort\\exposure\\exposure_lifecycle.cpp",
                "src\\plugins\\comfort\\scanners\\scanner_lifecycle.cpp",
                "src\\plugins\\comfort\\comfort_lifecycle.cpp",
                "src\\plugins\\explorer_cam\\explorer_cam_lifecycle.cpp")
    for path in required:
        if build.count(path) < 2:
            raise AssertionError(f"build must compile/archive and rig-link {path}")
    if "plugin_exposure.lib" in build or "plugin_scanners.lib" in build:
        raise AssertionError("old separate Exposure/Scanner archives remain")
    temporal_start = build.find(":rig_native_temporal_test")
    temporal_end = build.find("\n:rig_", temporal_start + 1)
    temporal_rig = build[temporal_start:temporal_end if temporal_end >= 0 else None]
    if temporal_start < 0 or '/I"%GEN%"' not in temporal_rig or \
            '"src\\plugins\\comfort\\comfort_lifecycle.cpp"' not in temporal_rig:
        raise AssertionError("native temporal adapter rig must include generated manifest headers")
    if build.count("plugin_explorer_cam.lib") < 2 or flat_proxy.count("plugin_explorer_cam.lib") < 2:
        raise AssertionError("Explorer archive is not linked by production, predicate, and flat proxy consumers")
    if "plugin_exposure.lib" in flat_proxy or "plugin_scanners.lib" in flat_proxy:
        raise AssertionError("flat proxy retains removed Exposure/Scanner archives")

    labels = body(perf_monitor,
                  "constexpr const char* kStableCostOwnerNames[plugin_cost::kOwnerCount] =")
    ordered(labels, '"temporal-aa"', '"cockpit-visuals"', '"exposure"',
            '"scanners"', '"intro"', '"on-foot-panel"', '"comfort"',
            '"performance"', '"diagnostics"', '"core"')
    if "plugins::kManifest[i]" in perf_monitor or "plugins::kManifest[" in perf_monitor:
        raise AssertionError("stable historical cost labels must not follow current catalog indices")
    if "const char* ownerName = kStableCostOwnerNames[i];" not in perf_monitor:
        raise AssertionError("plugin cost logger lost its stable row label lookup")


def expect(label: str, sources: tuple[str, ...], changed: tuple[str, ...]) -> None:
    try:
        check(*changed)
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"mutation not detected: {label}")


def self_test(s: tuple[str, ...]) -> None:
    # Component/source IDs and actual production routes.
    expect("scanner component owner", s, s[:1] + (once(s[1], "edvr::kComfortScannersLifecycleComponent", "edvr::kComfortExposureLifecycleComponent"),) + s[2:])
    expect("scanner configure order", s, s[:1] + (once(s[1], "fssPanelConfigure(cfg);\n    fssRevealConfigure(cfg);", "fssRevealConfigure(cfg);\n    fssPanelConfigure(cfg);"),) + s[2:])
    expect("exposure component owner", s, s[:2] + (once(s[2], "kComfortExposureLifecycleComponent", "kComfortScannersLifecycleComponent"),) + s[3:])
    expect("transition component owner", s, s[:4] + (once(s[4], "kComfortTransitionFlashLifecycleComponent", "kComfortExposureLifecycleComponent"),) + s[5:])
    expect("Explorer must not configure", s, s[:5] + (once(s[5], "    nullptr,\n    frame,", "    configure,\n    frame,"),) + s[6:])
    expect("vscreen component staged API", s, s[:6] + (replace_body(s[6], "void frameScannerLifecycleStage(", "pluginRegistryFrameLifecycleComponentStage(", "pluginRegistryFrameLifecycleStage("),) + s[7:])
    expect("Scanner refresh config fallback", s, s[:6] + (replace_body(s[6], "void configureScannerLifecycleStage(", "fallback(cfg);", "fallback(cfg);\n    fallback(cfg);"),) + s[7:])
    expect("Scanner no-state fallback selection", s, s[:6] + (replace_body(s[6], "bool ensureScannerLifecycle()", "if (!g_state) return false;", "if (g_state) return false;"),) + s[7:])
    expect("Comfort no-state fallback selection", s, s[:6] + (replace_body(s[6], "bool ensureComfortLifecycle()", "if (!g_state) return false;", "if (g_state) return false;"),) + s[7:])
    expect("Scanner reveal frame boundary scope", s, s[:6] + (replace_body(s[6], "void vScreenFrameBoundary()", "frameScannerLifecycleStage(\n            plugins::scanners::kScannerLifecycleRevealFrame, s->frameNo,\n            fssRevealFrameBoundary);", "fssRevealFrameBoundary();"),) + s[7:])
    expect("Scanner teardown interleave", s, s[:6] + (replace_body(s[6], "void shutdownVScreenFixes()", "fssPanelRectShutdown();\n    shutdownScannerLifecycle();", "shutdownScannerLifecycle();\n    fssPanelRectShutdown();"),) + s[7:])
    expect("Explorer device hook owner", s, s[:7] + (replace_body(s[7], "tkExplorerCam.run([] {", "pluginRegistryHasLifecycle(plugins::kPluginExplorerCam)", "pluginRegistryHasLifecycle(plugins::kPluginComfort)"),) + s[8:])
    expect("Explorer shutdown route", s, s[:7] + (replace_body(s[7], "void shutdownDeviceHooks()", "pluginRegistryShutdownLifecycle(plugins::kPluginExplorerCam)", "pluginRegistryShutdownLifecycle(plugins::kPluginComfort)"),) + s[8:])
    expect("transition shutdown order", s, s[:7] + (replace_body(s[7], "void shutdownDeviceHooks()", "transitionFlashEyeBaseShutdown();", "explorerCamShutdown();"),) + s[8:])
    expect("exposure configure route", s, s[:8] + (replace_body(s[8], "void exposureConfigure(Config& cfg)", "kComfortExposureLifecycleComponent", "kComfortScannersLifecycleComponent"),) + s[9:])
    expect("exposure staged frame route", s, s[:8] + (once(s[8], "pluginRegistryFrameLifecycleComponentStage(\n            plugins::kPluginComfort,\n            edvr::kComfortExposureLifecycleComponent,\n            plugins::exposure::kExposureLifecycleResetPairing", "pluginRegistryFrameLifecycleStage(\n            plugins::kPluginComfort,\n            edvr::kComfortExposureLifecycleComponent,\n            plugins::exposure::kExposureLifecycleResetPairing"),) + s[9:])
    expect("exposure final shutdown route", s, s[:8] + (replace_body(s[8], "void shutdownExposureFix()", "kComfortExposureLifecycleComponent", "kComfortScannersLifecycleComponent"),) + s[9:])
    changed_build = s[9].replace('"%OBJ%\\plugins\\explorer_cam\\plugin_explorer_cam.lib" ^',
                                 '"%OBJ%\\plugins\\missing\\plugin_explorer_cam.lib" ^', 1)
    expect("Explorer archive linkage", s, s[:-1] + (changed_build,))
    build = s[9]
    start = build.find(":rig_native_temporal_test")
    end = build.find("\n:rig_", start + 1)
    rig = build[start:end if end >= 0 else None]
    changed_rig = once(rig, '/I"%GEN%" /I"third_party\\openxr\\include"',
                       '/I"missing-gen" /I"third_party\\openxr\\include"')
    changed_build = build[:start] + changed_rig + (build[end:] if end >= 0 else "")
    expect("native temporal generated include path", s,
           s[:9] + (changed_build,) + s[10:])
    expect("flat proxy Explorer archive linkage", s,
           s[:10] + (once(s[10], '"explorer-cam": root / "build" / "obj" / "plugins" / "explorer_cam" / "plugin_explorer_cam.lib"',
                          '"explorer-cam": root / "build" / "obj" / "plugins" / "missing" / "plugin_explorer_missing.lib"'),) + s[11:])
    expect("stable cost Exposure row label", s,
           s[:11] + (once(s[11], '"exposure"', '"comfort"'),) + s[12:])
    expect("stable cost Scanners row label", s,
           s[:11] + (once(s[11], '"scanners"', '"comfort"'),) + s[12:])
    expect("stable cost Core row label", s,
           s[:11] + (once(s[11], '"core"', '"other"'),) + s[12:])
    expect("cost labels independent of catalog order", s,
           s[:11] + (once(s[11], "const char* ownerName = kStableCostOwnerNames[i];",
                          "const char* ownerName = plugins::kManifest[i].id;"),) + s[12:])


def main() -> int:
    parser = argparse.ArgumentParser(description="Pin grouped plugin lifecycle host wiring")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true", help="read/check sources only")
    mode.add_argument("--self-test", action="store_true", help="check wiring and in-memory mutations")
    args = parser.parse_args()
    sources = (*tuple(path.read_text(encoding="utf-8") for path in PATHS),
               (ROOT / "src/d3d11/plugin_registry.h").read_text(encoding="utf-8"))
    check(*sources)
    if args.self_test:
        self_test(sources)
        print("PASS: grouped Comfort/Explorer lifecycle wiring (24 mutations)")
    else:
        print("DRY RUN: grouped Comfort/Explorer lifecycle pins passed; no files written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
