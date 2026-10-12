#!/usr/bin/env python3
"""Pin grouped plugin lifecycle records to their production host callsites.

The native dispatch and module rigs exercise callbacks; these textual pins
protect host order, component IDs, staged fallback, and draw-boundary scope.
Mutants are applied in memory only.
"""
import argparse
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
MUTATION_COUNT = 0
SLICE_PATHS = {
    "build": ROOT / "build.bat",
    "remlok_header": ROOT / "src/plugins/cockpit_visuals/remlok_lifecycle.h",
    "remlok": ROOT / "src/plugins/cockpit_visuals/remlok_fix.cpp",
    "scheduler_header": ROOT / "src/plugins/diagnostics/scheduler_lifecycle.h",
    "scheduler": ROOT / "src/plugins/diagnostics/scheduler_lifecycle.cpp",
    "probe": ROOT / "src/plugins/diagnostics/scheduler_stack_probe.cpp",
    "scheduler_hook": ROOT / "src/plugins/diagnostics/scheduler_stack_hook.cpp",
    "proxy": ROOT / "src/d3d11/d3d11_proxy.cpp",
    "native_settings": ROOT / "src/d3d11/native_render_settings.cpp",
    "native_policy_header": ROOT / "src/d3d11/native_render_policy.h",
    "native_policy": ROOT / "src/plugins/performance/openxr_resolution_policy.cpp",
    "fov_settings_header": ROOT / "src/plugins/performance/fov_trim_settings_policy.h",
    "fov_settings": ROOT / "src/plugins/performance/fov_trim_settings_policy.cpp",
    "native_frame_fov_adapter": ROOT / "src/d3d11/native_frame_fov_trim_policy.h",
    "native_frame": ROOT / "src/d3d11/native_frame.cpp",
    "fov_settings_fixture": ROOT / "tools/fov_trim_settings_test/fov_trim_settings_test.cpp",
    "native_frame_fixture": ROOT / "tools/native_frame_test/native_frame_test.cpp",
    "native_module": ROOT / "src/openxr/native_module.cpp",
    "native_host": ROOT / "src/openxr/native_runtime_host.h",
    "native_fss_client": ROOT / "src/openxr/native_fss_client.h",
    "native_fov_startup": ROOT / "src/openxr/native_fov_startup.h",
    "fov_facade": ROOT / "src/openxr/native_fov_trim.h",
    "trim_fixture": ROOT / "tools/native_trim_test/native_trim_test.cpp",
    "relay_header": ROOT / "src/d3d11/scheduler_relay.h",
    "relay_host": ROOT / "src/d3d11/kinematic_eval_hook.cpp",
    "vscreen": ROOT / "src/d3d11/vscreen.cpp",
    "device": ROOT / "src/d3d11/device_hook.cpp",
    "ui_layer": ROOT / "src/d3d11/ui_layer.cpp",
    "exposure_fix": ROOT / "src/d3d11/exposure_fix.cpp",
    "texture_sampler_policy": ROOT / "src/plugins/performance/texture_sampler_policy.cpp",
    "texture_sampler_policy_header": ROOT / "src/plugins/performance/texture_sampler_policy.h",
    "registry": ROOT / "src/d3d11/plugin_registry.cpp",
    "registry_header": ROOT / "src/d3d11/plugin_registry.h",
    "explorer_owner_gate": ROOT / "src/d3d11/explorer_cam_owner_gate.h",
    "explorer_fixture": ROOT / "tools/explorer_cam_test/explorer_cam_test.cpp",
    "diagnostics_owner_gate": ROOT / "src/d3d11/diagnostics_owner_gate.h",
    "cockpit_owner_gate": ROOT / "src/d3d11/cockpit_visuals_owner_gate.h",
    "intro_owner_gate": ROOT / "src/d3d11/intro_owner_gate.h",
    "comfort_owner_gate": ROOT / "src/d3d11/comfort_owner_gate.h",
    "cockpit_api_fixture": ROOT / "tools/cockpit_api_test/cockpit_api_test.cpp",
    "vscreen_predicate_fixture": ROOT / "tools/vscreen_predicate_test/vscreen_predicate_test.cpp",
    "native_fss": ROOT / "src/d3d11/native_fss.cpp",
    "native_fss_fixture": ROOT / "tools/native_fss_test/native_fss_test.cpp",
    "native_treatment_fixture": ROOT / "tools/openxr_native_test/treatment_cases.h",
    "native_treatment_runner": ROOT / "tools/openxr_native_test/openxr_native_test.cpp",
    "native_temporal": ROOT / "src/d3d11/native_temporal.cpp",
    "native_temporal_fixture": ROOT / "tools/native_temporal_test/native_temporal_test.cpp",
    "plugin_dispatch_fixture": ROOT / "tools/plugin_dispatch_test/plugin_dispatch_test.cpp",
    "terrain_cover": ROOT / "src/d3d11/terrain_cull_cover_hook.cpp",
    "terrain_cover_fixture": ROOT / "tools/terrain_cover_test/terrain_cover_test.cpp",
    "sunglare": ROOT / "src/d3d11/sunglare_fix.cpp",
    "onfoot_owner_gate": ROOT / "src/d3d11/on_foot_panel_owner_gate.h",
    "screen_motion": ROOT / "src/d3d11/screen_motion.cpp",
    "flat_runtime": ROOT / "src/d3d11/flat_runtime.cpp",
    "native_sharpen": ROOT / "src/d3d11/native_sharpen.cpp",
    "vr_world_route": ROOT / "src/d3d11/vr_world_route.cpp",
    "temporal_pass": ROOT / "src/d3d11/temporal_pass.cpp",
    "menu": ROOT / "src/d3d11/menu.cpp",
    "intro_upscale": ROOT / "src/plugins/intro/intro_upscale.cpp",
    "vscreen_res": ROOT / "src/d3d11/vscreen_res.cpp",
    "vscreen_res_header": ROOT / "src/d3d11/vscreen_res.h",
    "vscreen_fit_fixture": ROOT / "tools/vscreen_fit_test/vscreen_fit_test.cpp",
    "screen_motion_fixture": ROOT / "tools/screen_motion_test/screen_motion_test.cpp",
}
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
    if not scanner_dispatch.lstrip().startswith(
            "if (!g_state || !g_state->comfortOwnerAllowed) return;"):
        raise AssertionError("Scanner configure dispatch must stop before registry and direct fallback when Comfort is denied")
    registry_dispatch = scanner_dispatch[scanner_dispatch.find("if (g_scannerLifecycleRegistered"):]
    ordered(registry_dispatch,
            "pluginRegistryConfigureLifecycleComponentStage(",
            "edvr::kComfortScannersLifecycleComponent", "return;", "fallback(cfg);")
    if scanner_dispatch.count("fallback(cfg);") != 1:
        raise AssertionError("Scanner configure fallback must occur exactly once")
    for name, accessor in (("ensureScannerLifecycle", "scannerLifecycleOps"),
                           ("ensureComfortLifecycle", "comfortLifecycleOps")):
        ensure = body(vscreen, f"bool {name}()")
        if "if (!g_state || !g_state->comfortOwnerAllowed) return false;" not in ensure or "pluginRegistryRegisterLifecycle(" not in ensure or accessor not in ensure:
            raise AssertionError(f"{name} must reject missing or denied Comfort ownership and return registry acceptance")
        if "pluginRegistryHasLifecycle" in ensure:
            raise AssertionError(f"{name} must use registration result, not a later registry lookup")
    frame_dispatch = body(vscreen, "void frameScannerLifecycleStage(")
    if not frame_dispatch.lstrip().startswith(
            "if (!g_state || !g_state->comfortOwnerAllowed) return;"):
        raise AssertionError("Scanner frame dispatch must stop before registry and direct fallback when Comfort is denied")
    registry_frame_dispatch = frame_dispatch[frame_dispatch.find("if (g_scannerLifecycleRegistered"):]
    ordered(registry_frame_dispatch, "pluginRegistryFrameLifecycleComponentStage(",
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
            "if (!g_state || !g_state->comfortOwnerAllowed) return;")
    registered_shutdown = scanner_final[scanner_final.find("if (registered)"):]
    ordered(registered_shutdown, "pluginRegistryShutdownLifecycleComponent(",
            "return;", "fssRevealShutdown();")

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


def check_module_slices(sources: dict[str, str]) -> None:
    rem_h = sources["remlok_header"]
    rem = sources["remlok"]
    scheduler_h = sources["scheduler_header"]
    scheduler = sources["scheduler"]
    probe = sources["probe"]
    scheduler_hook = sources["scheduler_hook"]
    proxy = sources["proxy"]
    native_settings = sources["native_settings"]
    native_policy_h = sources["native_policy_header"]
    native_policy = sources["native_policy"]
    fov_settings_h = sources["fov_settings_header"]
    fov_settings = sources["fov_settings"]
    native_frame_fov_adapter = sources["native_frame_fov_adapter"]
    native_frame = sources["native_frame"]
    fov_settings_fixture = sources["fov_settings_fixture"]
    native_frame_fixture = sources["native_frame_fixture"]
    native_module = sources["native_module"]
    native_host = sources["native_host"]
    native_fov_startup = sources["native_fov_startup"]
    fov_facade = sources["fov_facade"]
    trim_fixture = sources["trim_fixture"]
    relay_h = sources["relay_header"]
    relay = sources["relay_host"]
    vscreen = sources["vscreen"]
    device = sources["device"]
    diagnostics_owner_gate = sources["diagnostics_owner_gate"]
    registry = sources["registry"]
    registry_header = sources["registry_header"]
    build = sources["build"]
    explorer_owner_gate = sources["explorer_owner_gate"]
    explorer_fixture = sources["explorer_fixture"]
    cockpit_owner_gate = sources["cockpit_owner_gate"]
    intro_owner_gate = sources["intro_owner_gate"]
    cockpit_api_fixture = sources["cockpit_api_fixture"]
    vscreen_predicate_fixture = sources["vscreen_predicate_fixture"]
    sunglare = sources["sunglare"]
    comfort_owner_gate = sources["comfort_owner_gate"]
    native_fss = sources["native_fss"]
    native_fss_fixture = sources["native_fss_fixture"]
    native_treatment_fixture = sources["native_treatment_fixture"]
    native_treatment_runner = sources["native_treatment_runner"]
    native_temporal = sources["native_temporal"]
    native_temporal_fixture = sources["native_temporal_fixture"]
    plugin_dispatch_fixture = sources["plugin_dispatch_fixture"]
    native_runtime_host = sources["native_host"]
    native_fss_client = sources["native_fss_client"]
    terrain_cover = sources["terrain_cover"]
    terrain_cover_fixture = sources["terrain_cover_fixture"]
    flat_proxy = (ROOT / "tools/build_flat_sdk_bench_proxy.py").read_text(encoding="utf-8")

    if "const EdvrPluginLifecycleOps& remlokLifecycleOps();" not in rem_h:
        raise AssertionError("Cockpit Visuals private header must expose its RemLok lifecycle record")
    rem_config = body(rem, "void configure(void* opaqueConfig)")
    if "if (!opaqueConfig) return;" not in rem_config or \
            "edvr::remlokConfigure(*static_cast<edvr::Config*>(opaqueConfig))" not in rem_config:
        raise AssertionError("RemLok lifecycle configure must delegate the host Config to the module facade")
    for signature, target in (("void frame(void*, uint32_t)", "edvr::remlokFrameBoundary()"),
                              ("void shutdown(void*)", "edvr::remlokShutdown()")):
        if body(rem, signature).count(target) != 1:
            raise AssertionError(f"RemLok lifecycle callback lost {target}")
    rem_record = body(rem, "const EdvrPluginLifecycleOps kRemlokLifecycleOps =")
    for token in ("plugins::kPluginCockpitVisuals", '"cockpit-visuals"',
                  "&configure,", "&frame,", "&shutdown,", "nullptr,\n    0,"):
        if token not in rem_record:
            raise AssertionError(f"RemLok lifecycle record missing {token}")

    for sig in ("void vScreenRefreshConfig()", "void installVScreenFixes("):
        site = body(vscreen, sig)
        if site.count("configureRemlokLifecycle(cfg)") != 1:
            raise AssertionError(f"{sig} must configure RemLok once at its former host position")
    rem_configure_host = body(vscreen, "void configureRemlokLifecycle(Config& cfg)")
    ordered(rem_configure_host,
            "if (!g_state || !g_state->cockpitVisualsOwnerAllowed) return;",
            "pluginRegistryRegisterLifecycle(", "pluginRegistryConfigureLifecycle(",
            "return;\n    }\n    remlokConfigure(cfg);")
    rem_frame_host = body(vscreen, "void frameRemlokLifecycle(uint32_t sceneFrame)")
    ordered(rem_frame_host,
            "if (!g_state || !g_state->cockpitVisualsOwnerAllowed) return;",
            "pluginRegistryHasLifecycle(", "pluginRegistryFrameLifecycle(",
            "return;\n    }\n    remlokFrameBoundary();")
    rem_shutdown_host = body(vscreen, "void shutdownRemlokLifecycle()")
    ordered(rem_shutdown_host,
            "if (!g_state || !g_state->cockpitVisualsOwnerAllowed) return;",
            "pluginRegistryHasLifecycle(", "pluginRegistryShutdownLifecycle(",
            "return;\n    }\n    remlokShutdown();")
    if body(body(vscreen, "void vScreenFrameBoundary()"), "tkRemlok.run(").count(
            "frameRemlokLifecycle(s->frameNo)") != 1:
        raise AssertionError("RemLok lifecycle frame callback moved off its existing body-frame tick")
    if body(vscreen, "void shutdownVScreenFixes()").count(
            "shutdownRemlokLifecycle()") != 1:
        raise AssertionError("VScreen shutdown must retire RemLok through its lifecycle wrapper")

    if "const EdvrPluginLifecycleOps& schedulerLifecycleOps();" not in scheduler_h:
        raise AssertionError("Diagnostics private header must expose the scheduler lifecycle record")
    diagnostics_gate = body(
        diagnostics_owner_gate,
        "bool runDiagnosticsOwned(bool ownerAllowed, Callback&& callback)")
    ordered(diagnostics_gate, "if (!ownerAllowed) return false;",
            "std::forward<Callback>(callback)();", "return true;")
    sched_config = body(scheduler, "void configure(void* rawConfig)")
    ordered(sched_config, "if (!rawConfig) return;", "edvrDiagnosticsSchedulerConfigure(",
            "getBool(\"advanced.scheduler_probe\", false)")
    if "&edvr::schedulerStackProbe" not in body(
            scheduler, "const EdvrPluginLifecycleOps kSchedulerLifecycleOps ="):
        raise AssertionError("Diagnostics lifecycle record must own the scheduler probe state")
    for signature, target in (("void frame(void* state, uint32_t sceneFrame)",
                               "edvrDiagnosticsSchedulerFrame(state, sceneFrame)"),
                              ("void shutdown(void* state)",
                               "edvrDiagnosticsSchedulerShutdown(state)")):
        if body(scheduler, signature).count(target) != 1:
            raise AssertionError(f"Diagnostics lifecycle callback lost {target}")
    for sig in ("void vScreenRefreshConfig()", "void installVScreenFixes("):
        site = body(vscreen, sig)
        if site.count("configureSchedulerLifecycle(cfg)") != 1:
            raise AssertionError(f"{sig} must configure Diagnostics at the former scheduler probe position")
    sched_config_host = body(vscreen, "void configureSchedulerLifecycle(Config& cfg)")
    ordered(sched_config_host,
            "if (!g_state || !g_state->diagnosticsOwnerAllowed) return;",
            "pluginRegistryRegisterLifecycle(",
            "pluginRegistryConfigureLifecycle(",
            "        return;\n    }\n    edvrDiagnosticsSchedulerConfigure(",
            "edvrDiagnosticsSchedulerConfigure(edvrDiagnosticsSchedulerState(),")
    sched_shutdown_host = body(vscreen, "void shutdownSchedulerLifecycle()")
    ordered(sched_shutdown_host,
            "if (!g_state || !g_state->diagnosticsOwnerAllowed) return;",
            "pluginRegistryHasLifecycle(",
            "pluginRegistryShutdownLifecycle(",
            "        return;\n    }\n    edvrDiagnosticsSchedulerShutdown(",
            "edvrDiagnosticsSchedulerShutdown(edvrDiagnosticsSchedulerState())")
    device_tick = body(device, "tkSchedulerProbe.run([] {")
    scheduler_tick = body(device, "void presentFrameBoundary()")
    ordered(scheduler_tick, "if (diagnosticsOwnerAllowed()) tkSchedulerProbe.run([] {",
            "pluginRegistryHasLifecycle(",
            "pluginRegistryFrameLifecycle(", "} else {", "edvrDiagnosticsSchedulerFrame(")
    if "schedulerStackProbe" in device_tick or "SchedulerStackProbe" in device_tick:
        raise AssertionError("core Present host must dispatch Diagnostics through scalar lifecycle callbacks")
    if "if (diagnosticsOwnerAllowed()) tkSchedulerProbe.run([] {" not in scheduler_tick:
        raise AssertionError("Diagnostics scheduler tick and direct fallback must be unreachable without cached ownership")

    device_state = body(device, "struct State {")
    if "bool diagnosticsOwnerAllowed = false;" not in device_state:
        raise AssertionError("device Diagnostics owner cache must default to denied")
    device_startup = body(device, "State& ensureState()")
    ordered(device_startup,
            "g_state->diagnosticsOwnerAllowed =",
            "plugin_admission_transport::ownerAllowedForStartup(",
            "configureDiagnosticHotkeys();",
            "registerDiagnosticMenuActions();")
    diag_hotkeys = body(device, "void configureDiagnosticHotkeys() {")
    ordered(diag_hotkeys,
            'getString("hotkey.toggle_exposure", "SCROLLLOCK")',
            "if (!s.diagnosticsOwnerAllowed) return;",
            'getString("hotkey.dump_camera", "PAUSE")',
            'getString("hotkey.dump_draws", runtimeFlatProfile() ? "NUMLOCK" : "")',
            'getString("hotkey.dump_eyes", "")')
    menu_register = body(device, "void registerDiagnosticMenuActions() {")
    if "if (!g_state || !g_state->diagnosticsOwnerAllowed) return;" not in menu_register:
        raise AssertionError("Diagnostics menu actions, including marker, must remain absent without owner admission")
    flat_capture = body(device, "bool captureFlatProbeShader(char stage, uint64_t hash)")
    if "if (!diagnosticsOwnerAllowed() || !g_state || !runtimeFlatProfile()" not in flat_capture:
        raise AssertionError("flat diagnostic shader capture must require Diagnostics ownership")
    flat_lookup = body(device, "bool flatProbeShaderLookup(char stage, uint64_t hash,")
    if "diagnosticsOwnerAllowed" in flat_lookup or \
            "s.flatProbeShaders.find(std::make_pair(stage, hash))" not in flat_lookup:
        raise AssertionError("Core flat renderer shader lookup must retain its cache path independent of Diagnostics")

    vscreen_state = body(vscreen, "struct State {")
    if "bool diagnosticsOwnerAllowed = false;" not in vscreen_state:
        raise AssertionError("vScreen Diagnostics owner cache must default to denied")
    install_startup = body(vscreen, "void installVScreenFixes(")
    owner_snapshot = "const bool diagnosticsAllowed = diagnosticsOwnerAllowed();"
    owner_cache = "g_state->diagnosticsOwnerAllowed = diagnosticsAllowed;"
    if install_startup.count(owner_snapshot) != 1 or install_startup.count(owner_cache) != 1:
        raise AssertionError("vScreen startup must cache the published Diagnostics owner exactly once")
    ordered(install_startup, owner_snapshot, "if (!wantVoid && scale == 1.0f &&",
            owner_cache, "readCensusSkip(cfg,",
            "configureSchedulerLifecycle(cfg)")
    if install_startup.count("configureSchedulerLifecycle(cfg)") != 1:
        raise AssertionError("vScreen startup must configure the Diagnostics lifecycle once")
    refresh = body(vscreen, "void vScreenRefreshConfig()")
    if "diagnosticsOwnerAllowed()" in refresh or \
            "s->diagnosticsOwnerAllowed" not in refresh:
        raise AssertionError("vScreen reload must reuse its cached Diagnostics admission without a second query")
    if refresh.count("configureSchedulerLifecycle(cfg)") != 1:
        raise AssertionError("vScreen reload must configure the Diagnostics lifecycle once")
    census_reader = body(vscreen, "void readCensusSkip(Config& cfg, State* s)")
    ordered(census_reader, "if (!s || !s->diagnosticsOwnerAllowed) return;",
            'cfg.getString("advanced.census_skip"')
    if census_reader.count('cfg.getString("advanced.census_skip"') != 1:
        raise AssertionError("denied Diagnostics owner must not read census configuration")
    for gate in (
            "EDVR_GATE_DIAGNOSTICS(census_skip_count, s->censusSkipCount != 0)",
            "EDVR_GATE_DIAGNOSTICS(census_skip_ranges, s->censusSkipRangeCount != 0)",
            "EDVR_GATE_DIAGNOSTICS(census_skip_off, s->censusSkipOffCount != 0)",
            "EDVR_GATE_DIAGNOSTICS(census_quad_skip_armed, s->quadSkipArmed)",
            "EDVR_GATE_DIAGNOSTICS(census_auto, s->censusAutoW != 0)"):
        if vscreen.count(gate) != 1:
            raise AssertionError(f"Diagnostics census legacy gate must be owner-filtered exactly once: {gate}")
    if "if (s->diagnosticsOwnerAllowed || s->temporalAAOwnerAllowed)\n        depthProbeConfigure(cfg, s->diagnosticsOwnerAllowed," not in body(
            vscreen, "void vScreenRefreshConfig()") or \
            "if (g_state->diagnosticsOwnerAllowed || g_state->temporalAAOwnerAllowed)\n        depthProbeConfigure(cfg, g_state->diagnosticsOwnerAllowed," not in body(
                vscreen, "void installVScreenFixes("):
        raise AssertionError("shared depth discovery must run for either admitted Diagnostics or TemporalAA owner")
    if "if (s->diagnosticsOwnerAllowed || s->temporalAAOwnerAllowed)\n        objectProbeConfigure(cfg, s->diagnosticsOwnerAllowed," not in body(
            vscreen, "void vScreenRefreshConfig()") or \
            "if (g_state->diagnosticsOwnerAllowed || g_state->temporalAAOwnerAllowed)\n        objectProbeConfigure(cfg, g_state->diagnosticsOwnerAllowed," not in body(
                vscreen, "void installVScreenFixes("):
        raise AssertionError("shared object discovery must run for either admitted Diagnostics or TemporalAA owner")
    sched_config = body(vscreen, "void configureSchedulerLifecycle(Config& cfg)")
    ordered(sched_config,
            "if (!g_state || !g_state->diagnosticsOwnerAllowed) return;",
            "pluginRegistryRegisterLifecycle(",
            "pluginRegistryConfigureLifecycle(",
            "        return;\n    }\n    edvrDiagnosticsSchedulerConfigure(",
            'cfg.getBool("advanced.scheduler_probe", false)')
    sched_shutdown = body(vscreen, "void shutdownSchedulerLifecycle()")
    ordered(sched_shutdown,
            "if (!g_state || !g_state->diagnosticsOwnerAllowed) return;",
            "pluginRegistryHasLifecycle(",
            "pluginRegistryShutdownLifecycle(",
            "        return;\n    }\n    edvrDiagnosticsSchedulerShutdown(",
            "edvrDiagnosticsSchedulerShutdown(edvrDiagnosticsSchedulerState())")
    relay_feed = body(scheduler_hook, "void schedulerRelayFeed(")
    ordered(relay_feed, "slot->load(std::memory_order_acquire)",
            "if(probe)probe->noteEntry(target,static_cast<uintptr_t>(entryRsp))")
    hook_ops = body(scheduler_hook, "const EdvrSchedulerRelayObserverOps kSchedulerRelayObserverOps =")
    for token in ("sizeof(EdvrSchedulerRelayObserverOps)", "&observer", "&schedulerRelayFeed"):
        if token not in hook_ops:
            raise AssertionError(f"Diagnostics relay record missing {token}")
    hook_install = body(scheduler_hook, "const char* attachSchedulerStackHooks(")
    ordered(hook_install, "edvrKinematicEvalSchedulerObserverAttach(",
            "&kSchedulerRelayObserverOps)", "observer.store(probe")
    hook_remove = body(scheduler_hook, "void detachSchedulerStackHooks(")
    ordered(hook_remove, "observer.store(nullptr", "edvrKinematicEvalSchedulerObserverDetach(",
            "&kSchedulerRelayObserverOps)")
    if "SchedulerStackProbe schedulerStackProbe;" not in probe:
        raise AssertionError("moved probe must retain module-owned process-lifetime state")

    if "sizeof(EdvrSchedulerRelayObserverOps) == 24" not in relay_h or \
            "offsetof(EdvrSchedulerRelayObserverOps, feed) == 16" not in relay_h:
        raise AssertionError("scheduler relay record must keep its fixed 24-byte POD layout")
    bracket = body(relay, "uintptr_t __fastcall bracket(")
    if bracket.count("schedulerObserver.load(std::memory_order_acquire)") != 1:
        raise AssertionError("job bracket must take exactly one scheduler observer snapshot")
    ordered(bracket, "schedulerObserver.load(std::memory_order_acquire)",
            "scheduler->feed(scheduler->context,job,",
            "struct JobBit", "KinematicEvalProbe* probe=")
    if "schedulerStackProbe" in relay or "edvrDiagnosticsSchedulerRelayJob" in relay:
        raise AssertionError("core kinematic relay must not know the Diagnostics module feed symbol or class")
    gate = body(relay, "void recomputeGateLocked() noexcept")
    if "schedulerObserver.load(std::memory_order_acquire)!=nullptr" not in gate or \
            "observer.load(std::memory_order_acquire)!=nullptr" not in gate or \
            "emitWanted.load(std::memory_order_acquire)" not in gate:
        raise AssertionError("scheduler ownership must share the recomputed gate with existing consumers")
    attach = body(relay, "extern \"C\" const char* edvrKinematicEvalSchedulerObserverAttach(")
    ordered(attach, "ops->structSize!=sizeof(EdvrSchedulerRelayObserverOps)",
            "std::lock_guard<std::mutex> lock(g_installMutex)",
            "if(active)return active==ops?\"installed\":\"observer_busy\"",
            "ensureInstalled(base)", "schedulerObserver.store(ops", "recomputeGateLocked()")
    detach = body(relay, "extern \"C\" void edvrKinematicEvalSchedulerObserverDetach(")
    ordered(detach, "std::lock_guard<std::mutex> lock(g_installMutex)",
            "schedulerObserver.load(std::memory_order_acquire)!=exactOps",
            "schedulerObserver.store(nullptr", "recomputeGateLocked()")

    if 'Config::get().getString("fix.openxr_resolution"' in native_settings:
        raise AssertionError("core render settings broker must not own resolution policy reads")
    query = body(native_settings, "extern \"C\" BOOL WINAPI edvrQueryNativeRenderSettings(")
    ordered(query, "answer.openxrRenderScale = EDVR_NATIVE_RENDER_SCALE_DEFAULT",
            "answer.matchedEntry = 0", "answer.entryCount = 0",
            "if (g_policyInstalled)", "g_policy.resolve(", "writeSettingsOutput(output, answer)")
    for token in ("edvrPublishNativeRenderSizing", "edvrQueryNativeRenderSizing",
                  "nativeRenderLabels"):
        if token not in native_settings:
            raise AssertionError(f"core sizing broker lost shared service {token}")
    if "fix.openxr_resolution" not in native_policy or "Log::get().note" not in native_policy:
        raise AssertionError("Performance implementation must own resolution config reads and reports")
    if "static_assert(sizeof(OpsV1) == 32" not in native_policy_h:
        raise AssertionError("native render policy bridge must keep its fixed POD record")

    fov_keys = ("experimental.fov_trim_vertical", "experimental.fov_trim_outer",
                "experimental.fov_trim_nasal")
    if any(fov_settings.count(f'getString("{key}"') != 1 for key in fov_keys):
        raise AssertionError("Performance FOV settings policy must own each literal Config read once")
    if any(key in native_frame for key in fov_keys):
        raise AssertionError("core native-frame host must not retain FOV settings Config reads")
    for token in ("sizeof(FovTrimSettingsInputV1) == 76",
                  "sizeof(FovTrimSettingsPolicyOpsV1) == 32",
                  "CreateFovTrimSettingsV1 create;",
                  "DestroyFovTrimSettingsV1 destroy;",
                  "ResolveFovTrimSettingsV1 resolve;"):
        if token not in fov_settings_h:
            raise AssertionError(f"FOV settings policy ABI/lifetime pin missing: {token}")
    if "setNativeFrameFovTrimSettingsPolicyOpsV1" not in native_frame_fov_adapter:
        raise AssertionError("private native-frame FOV policy adapter disappeared")
    resolve_trim = body(native_frame, "HRESULT resolveTrim(uint32_t out[3])")
    if "if (!g_trimPolicyPresent)" not in resolve_trim:
        raise AssertionError("absent FOV selection must return before policy or label work")
    ordered(resolve_trim, "if (!g_trimPolicyPresent)",
            "fovTrimSettingsPolicyOpsV1()", "if (!g_trimPolicyState)",
            "g_trimPolicyOps.create()", "nativeRenderLabels(&labels)",
            "g_trimPolicyOps.resolve(g_trimPolicyState")
    if "edvr::Config::get()" in resolve_trim:
        raise AssertionError("FOV settings Config reads must remain inside Performance")
    resolve_policy = body(fov_settings, "bool resolve(")
    ordered(resolve_policy, "if (!output) return false;", "if (!policyContext ||",
            "input->structSize != sizeof(*input)",
            "return false;\n    }\n\n    auto& state",
            'getString("experimental.fov_trim_vertical"',
            'getString("experimental.fov_trim_outer"',
            'getString("experimental.fov_trim_nasal"')
    for token in ("input->version != kFovTrimSettingsPolicyVersion1",
                  "input->haveHeadset > 1",
                  "std::memchr(input->runtimeToken", "std::memchr(input->systemToken"):
        if token not in resolve_policy:
            raise AssertionError(f"FOV settings input validation disappeared before Config reads: {token}")
    begin = body(native_frame,
                 "HRESULT WINAPI beginFrame(void* context, const EdvrNativeFrameInput* input,")
    ordered(begin, "return E_INVALIDARG;", "resolveTrim(trim)")
    setter = body(native_frame, "bool setNativeFrameFovTrimSettingsPolicyOpsV1(")
    ordered(setter, "if (ops && !validTrimOps(*ops)) return false;",
            "if (g_trimPolicyState) g_trimPolicyOps.destroy(g_trimPolicyState);",
            "g_trimPolicyState = nullptr;",
            "g_trimPolicyOps = ops ? *ops : FovTrimSettingsPolicyOpsV1{};",
            "g_trimPolicyPresent = ops != nullptr;")
    factory = body(fov_settings, "const FovTrimSettingsPolicyOpsV1* fovTrimSettingsPolicyOpsV1()")
    if "static const FovTrimSettingsPolicyOpsV1 ops" not in factory or "create()" in factory:
        raise AssertionError("FOV settings factory must return an immutable table without creating state")
    for callback in ("&create", "&destroy", "&resolve"):
        if callback not in factory:
            raise AssertionError(f"FOV settings factory lost callback {callback}")
    ordered(begin, "return E_INVALIDARG;", "resolveTrim(trim)",
            "result.trimVerticalDeg", "result.trimOuterDeg", "result.trimNasalDeg")
    ordered(begin, "if (FAILED(trimStatus)) return trimStatus;",
            "result.trimVerticalDeg", "result.trimOuterDeg", "result.trimNasalDeg")
    close = body(native_frame, "HRESULT WINAPI close(")
    if "g_trimPolicyOps.destroy" in close:
        raise AssertionError("FOV policy state must survive close/reacquire")
    cleanup = body(native_frame, "~TrimPolicyCleanup()")
    ordered(cleanup, "if (g_trimPolicyState)",
            "g_trimPolicyOps.destroy(g_trimPolicyState);")
    if "g_mutex" in cleanup or "lock_guard" in cleanup:
        raise AssertionError("CRT FOV policy teardown must not wait on the host mutex")

    # The optional FOV implementation may be absent, but Elite's mandatory
    # game-facing size rounding still belongs to the core facade's identity
    # path. Keep raw dimensions for runtime geometry and round only the values
    # exposed as game recommendations.
    identity = body(fov_facade, "void resetIdentity(")
    ordered(identity, "gameFacingDimension(dims[eye].width)",
            "gameFacingDimension(dims[eye].height)",
            "out.trueRecommended = gameDims", "out.recommended = gameDims",
            "out.treatedFor = gameDims")
    if "out.recommended = dims[eye]" in identity:
        raise AssertionError("absent FOV policy must retain mandatory game-facing size rounding")
    for token in ("NativeFovTrim absent(nullptr)",
                  "absent.beginFrame(seanIni,sean,menuDims,9,false)",
                  "absent.recommended(eye).width==evenWidth",
                  "absent.treatedFor(eye).height==3914"):
        if token not in trim_fixture:
            raise AssertionError(f"native trim fixture lost absent-policy rounding coverage: {token}")
    native_start = body(native_module, "vr::EVRInitError start(uint32_t token,")
    ordered(native_start, "const bool hasAdmissionReservation =",
            "std::make_unique<NativeRuntimeHost>")
    startup_selection = native_start[native_start.find("const auto* fovTrimOps ="):
                                     native_start.find("generation->host=std::make_unique<NativeRuntimeHost>")]
    ordered(startup_selection, "selectOps(", "hasAdmissionReservation",
            "admissionReservation.decision", "edvr::plugins::kPluginPerformance",
            "ownerAllowedForStartup", "nativeFovTrimOpsV1()")
    if "generation->binding.device(),fovTrimOps" not in native_start:
        raise AssertionError("native host startup must pass its selected FOV ops explicitly")
    if "fovTrimOps=\n          edvr::plugins::performance::nativeFovTrimOpsV1()" not in native_host:
        raise AssertionError("native host default FOV factory changed unexpectedly; startup must still override it explicitly")
    ordered(native_fov_startup, "if (hasAdmissionReservation && !ownerAllowed(decision, generatedGroupIndex))",
            "return nullptr;", "return factory();")
    ordered(native_frame_fov_adapter,
            "inline bool applyNativeFrameFovTrimStartupAdmissionV1(",
            "if (ownerAllowed) return true;",
            "setNativeFrameFovTrimSettingsPolicyOpsV1(nullptr)")
    native_init = body(proxy, "BOOL CALLBACK initOnceCallback(")
    ordered(native_init, "publishCandidate(",
            "ownerAllowedForStartup(", "edvr::plugins::kPluginPerformance)",
            "applyNativeFrameFovTrimStartupAdmissionV1(",
            "pinGraphicsModuleOnce(")
    render_gate = native_init[native_init.find("if (edvr::native_render_policy::resolutionPolicyStartupAllowed("):]
    ordered(render_gate, "resolutionPolicyStartupAllowed(",
            "edvr::runtimeFeaturesAllowed()",
            "pluginRegistryProfileSupports(edvr::plugins::kPluginPerformance)",
            "performanceOwnerAllowed))", "registerPolicy(&policy)")
    for token in ("disabledStartupOps == nullptr", "startupCounts.factoryCalls == 0",
                  "startupCounts.creates == 0", "legacy native startup preserves default FOV policy"):
        if token not in native_frame_fixture:
            raise AssertionError(f"native startup FOV fixture lost absence/legacy coverage: {token}")
    for token in ("applyNativeFrameFovTrimStartupAdmissionV1(true)",
                  "applyNativeFrameFovTrimStartupAdmissionV1(false)",
                  "g_trimPolicyCounts.creates == 0 &&",
                  "g_trimPolicyCounts.resolves == 0 &&",
                  "g_trimPolicyCounts.notes == 0"):
        if token not in native_frame_fixture:
            raise AssertionError(f"native-frame startup FOV fixture lost lazy/absence coverage: {token}")
    startup_fixture_start = native_frame_fixture.find(
        "setNativeFrameFovTrimSettingsPolicyOpsV1(&copiedTrimOps)")
    startup_fixture_end = native_frame_fixture.find("uint32_t absentTrimAccessor", startup_fixture_start)
    startup_fixture = native_frame_fixture[startup_fixture_start:startup_fixture_end]
    ordered(startup_fixture, "setNativeFrameFovTrimSettingsPolicyOpsV1(&copiedTrimOps)",
            "applyNativeFrameFovTrimStartupAdmissionV1(true)",
            "applyNativeFrameFovTrimStartupAdmissionV1(false)",
            "table.beginFrame(table.context, &absentTrimFrame")
    if "static_assert(edvr::native_render_policy::kPerformanceManifestIndex ==" not in proxy or \
            "edvr::plugins::kPluginPerformance," not in proxy:
        raise AssertionError("proxy policy bootstrap must stay tied to the generated Performance manifest index")
    init = body(proxy, "BOOL CALLBACK initOnceCallback(")
    ordered(init, "cfg.init(", "Log::get().open(",
            "pluginRegistryProfileSupports(edvr::plugins::kPluginPerformance)",
            "registerPolicy(&policy)", "return TRUE;")
    shutdown = body(proxy, "void shutdown()")
    ordered(shutdown, "unregisterPolicy(", "shutdownDeviceHooks()")

    required_build_sources = (
        "src\\plugins\\cockpit_visuals\\remlok_fix.cpp",
        "src\\plugins\\diagnostics\\scheduler_stack_probe.cpp",
        "src\\plugins\\diagnostics\\scheduler_stack_hook.cpp",
        "src\\plugins\\diagnostics\\scheduler_lifecycle.cpp",
        "src\\plugins\\performance\\openxr_resolution_policy.cpp",
        "src\\plugins\\performance\\fov_trim_policy.cpp",
        "src\\plugins\\performance\\fov_trim_settings_policy.cpp",
        "src\\plugins\\performance\\texture_sampler_policy.cpp",
    )
    for path in required_build_sources:
        if build.count(path) < 2:
            raise AssertionError(f"build must compile/archive and preserve direct rig source coverage for {path}")
    if "plugin_performance.lib" not in build or "plugin_performance.lib" not in flat_proxy:
        raise AssertionError("Performance archive must feed production and flat SDK bench links")
    runtime_start = build.find("echo [edvr] === edvr_openxr_runtime.dll ===")
    runtime_end = build.find("REM Shared by the installer and installer_test rigs below.", runtime_start + 1)
    runtime_compile = build[runtime_start:runtime_end if runtime_end >= 0 else None]
    if runtime_start < 0 or \
            '"src\\plugins\\performance\\fov_trim_policy.cpp"' not in runtime_compile:
        raise AssertionError("OpenXR production DLL must compile the Performance FOV policy TU")
    trim_start = build.find(":rig_native_trim_test")
    trim_end = build.find("\n:rig_", trim_start + 1)
    trim_rig = build[trim_start:trim_end if trim_end >= 0 else None]
    if trim_start < 0 or \
            '"src\\plugins\\performance\\fov_trim_policy.cpp"' not in trim_rig:
        raise AssertionError("native trim rig must link the production FOV policy TU")
    native_start = build.find(":rig_openxr_native_tests")
    native_end = build.find("\n:rig_", native_start + 1)
    native_rig = build[native_start:native_end if native_end >= 0 else None]
    if native_start < 0 or \
            '"src\\plugins\\performance\\fov_trim_policy.cpp"' not in native_rig:
        raise AssertionError("OpenXR native host fixture must link the Performance FOV factory")
    performance_archive = build[
        build.find('if not exist "%OBJ%\\plugins\\performance"'):
        build.find('if not exist "%OBJ%\\plugins\\intro"',
                   build.find('if not exist "%OBJ%\\plugins\\performance"'))]
    if '"src\\plugins\\performance\\fov_trim_settings_policy.cpp"' not in performance_archive or \
            '"%OBJ%\\plugins\\performance\\fov_trim_settings_policy.obj"' not in performance_archive:
        raise AssertionError("FOV settings policy must be compiled into the production Performance archive")
    if '"src\\plugins\\performance\\texture_sampler_policy.cpp"' not in performance_archive or \
            '"%OBJ%\\plugins\\performance\\texture_sampler_policy.obj"' not in performance_archive:
        raise AssertionError("sampler policy must be compiled into the production Performance archive")
    sampler_start = build.find(":rig_performance_sampler_test")
    sampler_end = build.find("\n:rig_", sampler_start + 1)
    sampler_rig = build[sampler_start:sampler_end if sampler_end >= 0 else None]
    for token in ('"tools\\performance_sampler_test\\texture_sampler_policy_test.cpp"',
                  '"src\\plugins\\performance\\texture_sampler_policy.cpp"',
                  '"src\\common\\config.cpp"', '"src\\common\\log.cpp"',
                  '"%BUILD%\\performance_sampler_test.exe" --dry-run',
                  '"%BUILD%\\performance_sampler_test.exe" --self-test'):
        if token not in sampler_rig:
            raise AssertionError(f"native sampler rig recipe lost {token}")
    frame_start = build.find(":rig_native_frame_test")
    frame_end = build.find("\n:rig_", frame_start + 1)
    frame_rig = build[frame_start:frame_end if frame_end >= 0 else None]
    if frame_start < 0 or \
            '"src\\plugins\\performance\\fov_trim_settings_policy.cpp"' not in frame_rig:
        raise AssertionError("native-frame fixture must link the production FOV settings resolver")
    fov_start = build.find(":rig_fov_trim_settings_test")
    fov_end = build.find("\n:rig_", fov_start + 1)
    fov_rig = build[fov_start:fov_end if fov_end >= 0 else None]
    for token in ('"tools\\fov_trim_settings_test\\fov_trim_settings_test.cpp"',
                  '"src\\plugins\\performance\\fov_trim_settings_policy.cpp"',
                  '"src\\common\\config.cpp"', '"src\\common\\log.cpp"',
                  '"%BUILD%\\fov_trim_settings_test.exe" --dry-run',
                  '"%BUILD%\\fov_trim_settings_test.exe" --self-test'):
        if token not in fov_rig:
            raise AssertionError(f"native FOV settings policy rig recipe lost {token}")
    if "ops->create()" not in fov_settings_fixture or \
            "ops->destroy(state)" not in fov_settings_fixture or \
            "ops.resolve(state" not in fov_settings_fixture:
        raise AssertionError("FOV settings fixture must execute the production create/resolve/destroy API")
    for token in ("g_trimPolicyCounts.creates == 0",
                  "selected Performance lazily creates once",
                  "provider output matches the temporal accessor",
                  "provider close retains the selected module state",
                  "the real policy cache and note cadence persist across provider close/reacquire",
                  "absent selection after replacement does no callback",
                  "install a valid test policy whose module-state allocator fails",
                  "allocationFailure.trimVerticalDeg == -41.0f",
                  "allocationFailure.trimOuterDeg == -42.0f",
                  "allocationFailure.trimNasalDeg == -43.0f"):
        if token not in native_frame_fixture:
            raise AssertionError(f"native-frame FOV settings fixture lost lifetime coverage: {token}")
    probe_start = build.find(":rig_scheduler_stack_probe_test")
    probe_end = build.find("\n:rig_", probe_start + 1)
    probe_rig = build[probe_start:probe_end if probe_end >= 0 else None]
    if probe_start < 0 or '/I"%GEN%"' not in probe_rig or \
            '"tools\\scheduler_stack_probe_test\\scheduler_stack_probe_test.cpp"' not in probe_rig:
        raise AssertionError("scheduler lifecycle adapter rig must include generated manifest headers")
    if '"src\\plugins\\diagnostics\\scheduler_lifecycle.cpp"' in probe_rig:
        raise AssertionError("scheduler probe rig owns its direct adapter include; do not compile it twice")
    classification_start = build.find(":rig_object_classification")
    classification_end = build.find("\n:rig_", classification_start + 1)
    classification_rig = build[classification_start:classification_end if classification_end >= 0 else None]
    if classification_start < 0 or '/I"%GEN%"' not in classification_rig:
        raise AssertionError("object classification fixture must resolve the generated manifest dependency")

    register = body(registry, "bool pluginRegistryRegisterLifecycle(")
    if 'std::strcmp(record.implementationStatus, "phase1-lifecycle") != 0' not in register or \
            'std::strcmp(record.implementationStatus, "phase1-pilot") != 0' not in register:
        raise AssertionError("lifecycle registry must accept both lifecycle modules and the Cockpit pilot")
    if "componentIndex != 0 && ops->manifestIndex != plugins::kPluginComfort" not in register:
        raise AssertionError("nonzero lifecycle components must remain restricted to Comfort")
    registry_draws = body(registry, "bool pluginRegistryWantsDraws(void* legacyState)")
    ordered(registry_draws, "if (g_drawGateFailOpen) return true;",
            "for (uint32_t i = 0; i < g_legacyCount; ++i)",
            "g_legacy[i].predicate(", "for (uint32_t i = 0; i < g_registeredPluginCount; ++i)")

    # Explorer Cam is admitted once after device hooks commit. The cached
    # decision gates registration, all Present bookkeeping, and both shutdown
    # routes, while the helper keeps denied callbacks completely cold.
    hook_device = body(device, "void hookDevice(ID3D11Device* device)")
    ordered(hook_device, "if (!s.deviceHook.commit())", "s.device = device;",
            "s.explorerCamOwnerAllowed =\n        plugin_admission_transport::ownerAllowedForStartup(\n            plugins::kPluginExplorerCam);",
            "runExplorerCamOwned(s.explorerCamOwnerAllowed, [] {",
            "pluginRegistryRegisterLifecycle(", "explorerCamLifecycleOps());")
    if hook_device.count("ownerAllowedForStartup(\n            plugins::kPluginExplorerCam)") != 1:
        raise AssertionError("Explorer admission must be snapshotted once after device-hook commit")
    if device.count("ownerAllowedForStartup(\n            plugins::kPluginExplorerCam)") != 1:
        raise AssertionError("Explorer transport admission must not be queried again after startup")
    if "explorerCamOwnerAllowed = false;" not in device:
        raise AssertionError("Explorer cached ownership must default to denied before startup")
    startup_gate = body(hook_device,
                        "runExplorerCamOwned(s.explorerCamOwnerAllowed, [] {")
    ordered(startup_gate, "pluginRegistryRegisterLifecycle(",
            "&plugins::explorer_cam::explorerCamLifecycleOps()")
    if hook_device.count("pluginRegistryRegisterLifecycle(\n            &plugins::explorer_cam::explorerCamLifecycleOps())") != 1:
        raise AssertionError("Explorer lifecycle registration must remain inside one owner gate")
    if "explorerCamLifecycleOps()" in hook_device[:hook_device.find(
            "runExplorerCamOwned(s.explorerCamOwnerAllowed, [] {")]:
        raise AssertionError("Explorer lifecycle factory moved ahead of the owner gate")

    present = body(device,
                   "runExplorerCamOwned(g_state->explorerCamOwnerAllowed, [] {")
    ordered(present, "tkExplorerCam.run([] {", "pluginRegistryFrameLifecycle(",
            "explorerCamFrameBoundary(frameNo)")
    if device.count("tkExplorerCam.run([] {") != 1:
        raise AssertionError("Explorer boundary tick must have exactly one gated callsite")
    if present.count("ownerAllowedForStartup"):
        raise AssertionError("Explorer Present must use the cached startup decision")

    shutdown = body(device, "void shutdownDeviceHooks()")
    shutdown_gate = body(shutdown,
                         "runExplorerCamOwned(g_state && g_state->explorerCamOwnerAllowed, [] {")
    ordered(shutdown_gate,
            "pluginRegistryShutdownLifecycle(plugins::kPluginExplorerCam)",
            "explorerCamShutdown();")
    if shutdown.count("pluginRegistryShutdownLifecycle(plugins::kPluginExplorerCam)") != 1 or \
            shutdown.count("explorerCamShutdown();") != 1:
        raise AssertionError("both Explorer shutdown routes must remain inside one owner gate")
    if shutdown.count("ownerAllowedForStartup"):
        raise AssertionError("Explorer shutdown must use the cached startup decision")

    if "if (!ownerAllowed) return false;\n    callback();\n    return true;" not in explorer_owner_gate:
        raise AssertionError("denied Explorer owner gate must return before invoking its callback")
    owner_fixture = body(explorer_fixture, "void testOwnerAbsence(const Pages& p)")
    for token in ("runExplorerCamOwned(false", "runExplorerCamOwned(true",
                  "settingsReads() == 0", "stolenBytes(7) == 0"):
        if token not in owner_fixture:
            raise AssertionError(f"Explorer fixture lost cached-owner gate evidence: {token}")
    if "runExplorerCamOwned" not in owner_fixture:
        raise AssertionError("Explorer fixture lost cached-owner gate coverage")
    if body(explorer_fixture, "void testGlue()").count("testOwnerAbsence(p);") != 1:
        raise AssertionError("Explorer owner absence fixture must run in the glue suite")

    # Cockpit Visuals has one startup admission snapshot. Its cached result
    # owns every Cockpit setup, dispatch and teardown route; the one shared
    # Comfort glare-train observation remains available while denied.
    if "bool cockpitVisualsOwnerAllowed = false;" not in vscreen:
        raise AssertionError("Cockpit owner cache must default to denied")
    if "bool introOwnerAllowed = false;" not in vscreen:
        raise AssertionError("Intro owner cache must default to denied")
    if "if (!ownerAllowed) return false;\n    std::forward<Callback>(callback)();\n    return true;" not in cockpit_owner_gate:
        raise AssertionError("denied Cockpit owner helper must return without invoking its callback")
    cockpit_query_token = ("ownerAllowedForStartup(\n"
                           "            plugins::kPluginCockpitVisuals)")
    if vscreen.count(cockpit_query_token) != 1 or \
            vscreen.count("readIntroStartupAdmission()") != 2:
        raise AssertionError("Cockpit owner and Intro decision each need exactly one VScreen startup snapshot")
    if "ownerAllowedForStartup" in body(vscreen, "void vScreenFrameBoundary()") or \
            "ownerAllowedForStartup" in body(vscreen, "void shutdownVScreenFixes()"):
        raise AssertionError("Cockpit Present and shutdown must use the cached startup decision")
    startup = body(vscreen, "void installVScreenFixes(")
    query_start = startup.find("ownerAllowedForStartup(")
    query_end = startup.find(");", query_start)
    query = startup[query_start:query_end + 2] if query_start >= 0 and query_end >= 0 else ""
    ordered(startup, cockpit_query_token,
            "runCockpitVisualsOwned(cockpitVisualsOwnerAllowed, [&] {")
    if "plugins::kPluginCockpitVisuals" not in query:
        raise AssertionError("VScreen startup must sample the Cockpit admission decision")
    startup_gate = body(startup,
                         "runCockpitVisualsOwned(cockpitVisualsOwnerAllowed, [&] {")
    if "cockpitVisualsPluginOps()" not in startup_gate:
        raise AssertionError("denied Cockpit ownership must skip its optional factory/registration")
    if startup.count("cockpitVisualsPluginOps()") != 1 or \
            startup.count(cockpit_query_token) != 1:
        raise AssertionError("Cockpit startup factory/query must each have one gated callsite")
    if "(cockpitVisualsOwnerAllowed ||\n         !(comfortOwnerAllowed && exposureDampingActive()))" not in startup:
        raise AssertionError("dormant vScreen must preserve Comfort glare observation when Cockpit is denied")

    # Intro shares vScreen's transport but owns its factory, lifecycle and
    # draw side effects. Its one startup query must precede config/factory
    # work; all module callbacks remain behind the cached decision.
    if "if (!ownerAllowed) return false;\n    std::forward<Callback>(callback)();\n    return true;" not in intro_owner_gate:
        raise AssertionError("denied Intro owner helper must return without invoking its callback")
    intro_admission_reader = body(vscreen, "IntroStartupAdmission readIntroStartupAdmission()")
    ordered(intro_admission_reader, "edvrQueryPluginAdmissionV1(",
            "plugin_admission_transport::kQueryVersionV1",
            "QueryKindV1::Decision", "sizeof(decision), &decision",
            "return introStartupAdmission(decision, status == S_OK);")
    if intro_admission_reader.count("edvrQueryPluginAdmissionV1(") != 1 or \
            "status == S_OK" not in intro_admission_reader or \
            "decision.size" in intro_admission_reader or \
            "ownerAllowedForStartup(" in intro_admission_reader:
        raise AssertionError("Intro reader must make one exact Decision query and hand exact S_OK status to the pure admission helper")
    intro_decision_helper = body(
        intro_owner_gate, "inline IntroStartupAdmission introStartupAdmissionWithValidators(")
    ordered(intro_decision_helper, "if (!querySucceeded) return {};",
            "result.ownerAllowed = ownerAllowed(decision);",
            "result.strictSelection = result.ownerAllowed &&",
            "Mode::AdmittedSelection")
    if "decision.size" in intro_decision_helper or \
            "decision.version" in intro_decision_helper:
        raise AssertionError("Intro helper must delegate wire validation to the authoritative pure owner rule")
    strict_mode_start = intro_decision_helper.find("if (decision.mode ==")
    strict_mode_end = intro_decision_helper.find("result.strictModeSelection = true;", strict_mode_start)
    strict_mode_gate = intro_decision_helper[strict_mode_start:strict_mode_end]
    ordered(strict_mode_gate, "Mode::AdmittedSelection", "isSelected(decision, i)")
    production_intro_adapter = body(
        intro_owner_gate, "inline IntroStartupAdmission introStartupAdmission(")
    if "bool strictModeSelection = false;" not in intro_owner_gate or \
            "introStartupAdmissionWithValidators(" not in production_intro_adapter or \
            "plugin_selection::admission_v1::ownerAllowedForStartup(" not in production_intro_adapter or \
            "plugin_selection::admission_v1::selected(d, i)" not in production_intro_adapter:
        raise AssertionError("production strict-mode decision must reuse the validated Intro owner/selection algorithm")
    if "#ifdef EDVR_SELECTION_ADMISSION_TEST" not in intro_owner_gate or \
            "introStartupAdmissionWithValidators(" not in intro_owner_gate or \
            "ownerAllowedForStartupWithCatalog(" not in intro_owner_gate or \
            "selectedWithCatalog(d, i, catalog)" not in intro_owner_gate:
        raise AssertionError("closed-catalog test adapter must share the production strict-selection algorithm")
    ordered(startup, "readIntroStartupAdmission();", "Config& cfg = Config::get();")
    if startup.count("readIntroStartupAdmission()") != 1 or \
            "const IntroStartupAdmission introAdmission = readIntroStartupAdmission();" not in startup or \
            "const bool introOwnerAllowed = introAdmission.ownerAllowed;" not in startup or \
            "g_state->introOwnerAllowed = introOwnerAllowed;" not in startup:
        raise AssertionError("Intro admission must be sampled once and cached before its configuration")
    dormant_start = startup.find("if (!wantVoid && scale == 1.0f &&")
    dormant_end = startup.find("// Preserve the existing retry path", dormant_start)
    if dormant_start < 0 or dormant_end < 0:
        raise AssertionError("vScreen dormant transport-only decision scope is missing")
    dormant_condition = startup[dormant_start:dormant_end]
    owner_snapshots = (
        "const bool diagnosticsAllowed = diagnosticsOwnerAllowed();",
        "const bool temporalAAOwnerAllowed = edvr::temporalAAOwnerAllowed();",
        "const bool strictSelectedVScreenOwner = strictAdmissionNeedsVScreenHooks(\n"
        "        introAdmission, diagnosticsAllowed, temporalAAOwnerAllowed);",
    )
    if any(startup.count(token) != 1 or startup.find(token) > dormant_start
           for token in owner_snapshots):
        raise AssertionError("D and Temporal-AA owner decisions must be cached once before dormant startup selection")
    if "!introAdmission.strictSelection &&" not in dormant_condition or \
            "!strictSelectedVScreenOwner &&" not in dormant_condition:
        raise AssertionError("dormant startup must preserve Intro strict-selection semantics and the strict D/TAA bypass")
    strict_helper = body(sources["intro_owner_gate"],
                         "inline bool strictAdmissionNeedsVScreenHooks(")
    ordered(strict_helper, "admission.strictModeSelection &&",
            "(diagnosticsOwnerAllowed || temporalAAOwnerAllowed)")
    intro_registration = body(vscreen, "void selectIntroVideoLifecycle()")
    if "runIntroOwned(g_state && g_state->introOwnerAllowed" not in intro_registration or \
            "g_introLifecycleRegistered = ensureIntroVideoLifecycle();" not in body(
                intro_registration, "runIntroOwned(g_state && g_state->introOwnerAllowed, [&] {"):
        raise AssertionError("Intro lifecycle factory/registration must be enclosed by cached owner admission")
    intro_factory = body(vscreen, "bool ensureIntroVideoLifecycle()")
    ordered(intro_factory, "if (!g_state || !g_state->introOwnerAllowed) return false;",
            "pluginRegistryRegisterLifecycle(introVideoLifecycleOps())")
    if vscreen.count("introVideoLifecycleOps()") != 1:
        raise AssertionError("Intro lifecycle factory must have one owner-checked registration callsite")
    for signature, calls in (
            ("void configureIntroLifecycleStage(",
             ("pluginRegistryConfigureLifecycleStage(kIntroPluginIndex, stage, &cfg)",
              "fallback(cfg)")),
            ("void configureIntroVideoLifecycle(Config& cfg)",
             ("pluginRegistryConfigureLifecycle(kIntroPluginIndex, &cfg)",
              "introSkipConfigure(cfg)", "introUpscaleConfigure(cfg)")),
            ("void frameIntroLifecycleStage(",
             ("pluginRegistryFrameLifecycleStage(kIntroPluginIndex, stage, context,",
              "fallback(context, sceneFrame != 0)")),
            ("void frameIntroVideoLifecycle(uint32_t sceneFrame)",
             ("pluginRegistryFrameLifecycle(kIntroPluginIndex, sceneFrame)",
              "introSkipTick(sceneFrame != 0)")),
            ("void shutdownIntroLifecycleStage(",
             ("pluginRegistryShutdownLifecycleStage(kIntroPluginIndex, stage)",
              "fallback()")),
            ("void shutdownIntroVideoLifecycle()",
             ("pluginRegistryShutdownLifecycle(kIntroPluginIndex)",
              "introUpscaleShutdown()", "introSkipShutdown()"))):
        scope = body(vscreen, signature)
        gate_start = "runIntroOwned(g_state && g_state->introOwnerAllowed, [&] {"
        gated = body(scope, gate_start)
        if any(call not in gated for call in calls):
            raise AssertionError(f"{signature} must gate its registry and direct fallback routes")
    for signature in ("void vScreenRefreshConfig()", "void installVScreenFixes("):
        scope = body(vscreen, signature)
        if scope.count("configureIntroLifecycleStage(") != 4 or \
                scope.count("configureIntroVideoLifecycle(cfg)") != 1 or \
                scope.count("selectIntroVideoLifecycle()") != 1:
            raise AssertionError(f"{signature} must retain the complete Intro config sweep")
        for call in ("holoConfigure(cfg)",):
            if scope.count(call) != 1 or call not in body(
                    scope, "runIntroOwned(" ):
                raise AssertionError(f"{signature} must owner-gate {call}")
    intro_interests = body(vscreen, "static bool configureLegacyDrawInterests(")
    intro_interest_gate = body(intro_interests,
                               "runIntroOwned(introOwnerAllowed, [&] {")
    if "enable(InterestId::IntroCurveObserve, introCurveDrawInterestConfigured());" not in intro_interest_gate or \
            "enable(InterestId::PanelCurveObserve,\n           onFootPanelOwnerAllowed && panelCurveDrawInterestConfigured());" not in intro_interests:
        raise AssertionError("Intro curve interest must be gated while shared On-foot panel curve stays active")

    # Both startup and reload configure all module-owned settings from the
    # cached bit. Keep exact call counts to catch a newly added ungated route.
    for signature in ("void vScreenRefreshConfig()", "void installVScreenFixes("):
        site = body(vscreen, signature)
        gates = [body(site[offset:], "runCockpitVisualsOwned(")
                 for offset in (i for i in range(len(site))
                                if site.startswith("runCockpitVisualsOwned(", i))]
        for call in ("configureRemlokLifecycle(cfg);", "targetSharpConfigure(cfg);",
                     "wakePulseConfigure(cfg);", "sunglareConfigure(cfg);",
                     "particleConfigure(cfg);"):
            if site.count(call) != 1 or sum(call in gate for gate in gates) != 1:
                raise AssertionError(f"{signature} must gate exactly one {call}")
        owner_arg = ("s->cockpitVisualsOwnerAllowed" if "vScreenRefreshConfig" in signature
                     else "g_state->cockpitVisualsOwnerAllowed")
        if site.count(f"runCockpitVisualsOwned({owner_arg}, [&] {{") != 5:
            raise AssertionError(f"{signature} must use the cached State bit for all five Cockpit config routes")
        for core_call in ("holoConfigure(cfg);", "uiDepthConfigure(cfg);",
                          "exposureConfigure(cfg);", "configureComfortLifecycle(cfg);",
                          "pluginRegistryConfigure(&cfg);"):
            if any(core_call in gate for gate in gates):
                raise AssertionError(f"Cockpit gate swallowed shared/core configuration: {core_call}")
    if ("{\"legacy.sun-glare\", &drawGate_sun_glare, true, "
            "LegacyGateOwner::CockpitOrComfort}" not in vscreen):
        raise AssertionError("shared Comfort glare observation lost its legacy draw-gate subscription")
    sun_gate = body(vscreen, "uint32_t drawGate_sun_glare(void* opaque)")
    if not sun_gate:
        raise AssertionError("sun-glare draw gate must preserve admitted Legacy behavior and use only Comfort damping when denied")
    ordered(sun_gate, "!s->cockpitVisualsOwnerAllowed && !s->comfortOwnerAllowed",
            "s->cockpitVisualsOwnerAllowed ? sunglareWantsDraws()",
            ": exposureDampingActive()")
    if "drawGateSubscribed(State* s) { return pluginRegistryWantsDraws(s); }" not in vscreen or \
            "(void)registerLegacyDrawGates(g_state);" not in startup:
        raise AssertionError("shared glare observer is not connected to the live VScreen draw boundary")
    if ("{\"legacy.sun-glare\", &drawGate_sun_glare, true, "
            "LegacyGateOwner::CockpitOrComfort}" not in vscreen):
        raise AssertionError("Comfort glare subscription must be registered as a state-backed legacy gate")

    interests = body(vscreen, "static bool configureLegacyDrawInterests(")
    for token in ("runCockpitVisualsOwned(cockpitVisualsOwnerAllowed, [&] {",
                  "enable(InterestId::TargetSharp", "enable(InterestId::WitchspaceStars",
                  "append(targetSharpDrawInterestFilters)",
                  "append(witchspaceStarsDrawInterestFilters)"):
        if token not in interests:
            raise AssertionError(f"Cockpit legacy draw-interest route is not owner-gated: {token}")
    interest_gates = [body(interests[offset:], "runCockpitVisualsOwned(")
                      for offset in (i for i in range(len(interests))
                                     if interests.startswith("runCockpitVisualsOwned(", i))]
    for token in ("InterestId::TargetSharp", "InterestId::WitchspaceStars",
                  "targetSharpDrawInterestFilters", "witchspaceStarsDrawInterestFilters"):
        if not any(token in gate for gate in interest_gates):
            raise AssertionError(f"Cockpit draw-interest escaped its owner gate: {token}")

    draw_visitor = body(vscreen, "struct VScreenDrawLadderVisitor")
    eligible = body(draw_visitor, "__forceinline bool eligible()")
    ordered(eligible,
            "SiteType::id == draw_ladder::SiteId::kNightVisionClaim",
            "if (!s->cockpitVisualsOwnerAllowed) return false;",
            "if constexpr (SiteType::shaderCandidateGated)")

    def visitor_site(site_id: str) -> str:
        marker = f"}} else if constexpr (id == SiteId::{site_id}) {{"
        start = draw_visitor.find(marker)
        if start < 0:
            raise AssertionError(f"Cockpit draw visitor lost route {site_id}")
        end = draw_visitor.find("} else if constexpr (id == SiteId::", start + len(marker))
        return draw_visitor[start:end if end >= 0 else None]

    for site_id in ("kNightVisionClaim", "kTargetSharpClaim", "kWitchspaceStarsSkip",
                    "kParticleSubstitute", "kSpaceDustSkip", "kOffscreenWakePulseSkip",
                    "kRemlokHideSkip", "kRemlokScissorClaim", "kSunglareNomination",
                    "kSunglareSkip", "kSunglareSteadyClaim", "kGlareClampClaim"):
        branch = visitor_site(site_id)
        if "!s->cockpitVisualsOwnerAllowed" not in branch:
            raise AssertionError(f"Cockpit draw route {site_id} bypasses cached ownership")
    intro_panel_site = visitor_site("kIntroPanelClaim")
    intro_curve_site = visitor_site("kIntroCurveObserve")
    if "s->introOwnerAllowed && kind == 'X' && count == 6 && introPanelWants()" not in intro_panel_site or \
            "s->introOwnerAllowed && kind == 'X' && count == 6 && introCurveWants()" not in intro_curve_site:
        raise AssertionError("Intro visitors must gate both panel resolution and curve module work")
    forward_begin = body(vscreen, "__declspec(noinline) void forwardVerdictBegin(")
    forward_end = body(vscreen, "__declspec(noinline) void forwardVerdictEnd(")
    if "runIntroOwned(g_state && g_state->introOwnerAllowed,\n                      [&] { holoBegin(self); })" not in forward_begin or \
            "runIntroOwned(g_state && g_state->introOwnerAllowed,\n                      [&] { backdropBegin(self); })" not in forward_begin or \
            "runIntroOwned(g_state && g_state->introOwnerAllowed,\n                      [&] { holoEnd(self); })" not in forward_end:
        raise AssertionError("Intro begin/end forwarding must stay behind cached owner admission")
    frame_boundary = body(vscreen, "void vScreenFrameBoundary()")
    intro_tick_gate = body(frame_boundary,
                           "runIntroOwned(g_state->introOwnerAllowed, [&] {")
    if any(token not in intro_tick_gate for token in
           ("tkIntroPanel.run(", "tkIntroCurve.run(", "tkIntroSkip.run(", "tkLoaderPanel.run(")):
        raise AssertionError("the complete Intro tick/bookkeeping bundle must be skipped without its owner")
    shutdown_scope = body(vscreen, "void shutdownVScreenFixes()")
    if "runIntroOwned(g_state->introOwnerAllowed, [&] { holoShutdown(); });" not in shutdown_scope or \
            "shutdownIntroLifecycleStage(kIntroLifecycleBackdrop, backdropShutdown);" not in shutdown_scope or \
            "panelCurveShutdown();" not in shutdown_scope:
        raise AssertionError("Intro teardown must be gated while shared On-foot curve teardown stays live")
    hooked_draw = body(vscreen, "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(")
    if "if (v == DrawVerdict::kIntroPanel && g_state->introOwnerAllowed)" not in hooked_draw or \
            "introPanelEndDraw(self);" not in hooked_draw or \
            "if (g_state->introOwnerAllowed && g_state->introCurveThisDraw)" not in hooked_draw or \
            "runIntroOwned(g_state->introOwnerAllowed," not in hooked_draw or \
            "introCurveEndDraw();" not in hooked_draw:
        raise AssertionError("direct Intro panel/curve draw-end routes must remain owner-gated")
    glare_skip_branch = visitor_site("kSunglareSkip")
    denied_glare = body(glare_skip_branch,
                        "if (!s->cockpitVisualsOwnerAllowed)")
    if "sunglareTrackExposureGlareTrain(kind, count, instances);" not in denied_glare:
        raise AssertionError("denied Cockpit glare lost the approved shared train observation")
    if "sunglareOnEyeDrawObserved" in denied_glare or "sunglareOnEyeDraw(" in denied_glare:
        raise AssertionError("denied Cockpit glare must not reach Cockpit action/billboard paths")
    for route in ("forwardVerdictBegin", "forwardVerdictEnd"):
        forward = body(vscreen, f"__declspec(noinline) void {route}(")
        gates = [body(forward[offset:], "runCockpitVisualsOwned(")
                 for offset in (i for i in range(len(forward))
                                if forward.startswith("runCockpitVisualsOwned(", i))]
        calls = ("remlokScissorBegin(self)", "targetSharpBegin(self)",
                 "pluginRegistryBegin(kPluginClaimNightVision,self)",
                 "sunglareBegin(self)", "particleBegin(self)") if route.endswith("Begin") else (
                 "remlokScissorEnd(self)", "targetSharpEnd(self)",
                 "pluginRegistryEnd(kPluginClaimNightVision,self)",
                 "sunglareEnd(self)", "particleEnd(self)")
        for call in calls:
            if forward.count(call) != 1 or sum(call in gate for gate in gates) != 1:
                raise AssertionError(f"Cockpit verdict {route} route escaped owner gate: {call}")
    glare_observer = body(sunglare, "void sunglareTrackExposureGlareTrain(")
    ordered(glare_observer, "exposureDampingActive()", "sunglareIsGlareTrain(",
            "g_lastSeenMs = nowMs();")
    for forbidden in ("sunglareOnEyeDraw", "sunglareConfigure", "particleOnDraw",
                      "billboard", "g_sunglareWorld", "g_sunglareMode",
                      "g_mode", "g_keep"):
        if forbidden in glare_observer:
            raise AssertionError(f"shared denied-owner glare observer reached Cockpit work: {forbidden}")

    refresh = body(vscreen, "void vScreenRefreshConfig()")
    if refresh.count("runCockpitVisualsOwned(") < 5:
        raise AssertionError("Cockpit reload must gate each direct module configuration route")
    frame = body(vscreen, "void vScreenFrameBoundary()")
    for tick, owner_arg in (("tkWakePulse.run(", "g_state->cockpitVisualsOwnerAllowed"),
                            ("tkRemlok.run(", "s->cockpitVisualsOwnerAllowed")):
        if frame.count(tick) != 1:
            raise AssertionError(f"Cockpit frame route must preserve one {tick} callsite")
        tick_start = frame.find(tick)
        tick_end = frame.find("});", tick_start)
        if tick_end < 0 or f"runCockpitVisualsOwned({owner_arg}, [&] {{" not in \
                frame[max(0, tick_start - 180):tick_end + 3]:
            raise AssertionError(f"Cockpit frame route {tick} must be inside the cached owner gate")
    for core_tick in ("tkDrawCensusBoundary.run(", "frameComfortLifecycleStage("):
        if core_tick not in frame:
            raise AssertionError(f"shared/core frame service disappeared under Cockpit gating: {core_tick}")
    for at in (i for i in range(len(frame)) if frame.startswith("runCockpitVisualsOwned(", i)):
        gate_body = body(frame[at:], "runCockpitVisualsOwned(")
        if any(core_tick in gate_body for core_tick in
               ("tkDrawCensusBoundary.run(", "frameComfortLifecycleStage(")):
            raise AssertionError("Cockpit frame gate swallowed a shared/core service")
    map_hook = body(vscreen, "HRESULT STDMETHODCALLTYPE hookedMap(")
    for route in ("res == billboardTarget()", "res == particleTarget()",
                  "res == sunglareSceneCbTarget()"):
        if f"s->cockpitVisualsOwnerAllowed && mapSub0 && {route}" not in map_hook:
            raise AssertionError(f"Cockpit Map capture route lacks owner guard: {route}")
    unmap_hook = body(vscreen, "void STDMETHODCALLTYPE hookedUnmap(")
    unmap_gates = [body(unmap_hook[offset:], "runCockpitVisualsOwned(")
                   for offset in (i for i in range(len(unmap_hook))
                                  if unmap_hook.startswith("runCockpitVisualsOwned(", i))]
    for route in ("sunglareSceneRows(s->camData, s->camBytes)",
                  "sunglareSceneRows(s->sceneCbData, s->sceneCbBytes)"):
        if unmap_hook.count(route) != 1 or sum(route in gate for gate in unmap_gates) != 1:
            raise AssertionError(f"Cockpit camera/scene capture escaped owner gate: {route}")
    if unmap_hook.count("runCockpitVisualsOwned(s->cockpitVisualsOwnerAllowed, [&] {") != 2:
        raise AssertionError("Unmap scene/camera captures must use the cached State owner bit")
    for route in ("if (s->cockpitVisualsOwnerAllowed && res == s->partResource && s->partData)",
                  "if (s->cockpitVisualsOwnerAllowed && res == s->bbResource && s->bbData)"):
        if route not in unmap_hook:
            raise AssertionError(f"Cockpit Unmap capture route lacks owner guard: {route}")
    shutdown = body(vscreen, "void shutdownVScreenFixes()")
    shutdown_gates = [body(shutdown[offset:], "runCockpitVisualsOwned(")
                      for offset in (i for i in range(len(shutdown))
                                     if shutdown.startswith("runCockpitVisualsOwned(", i))]
    for call in ("particleShutdown();", "wakePulseShutdown();", "targetSharpShutdown();",
                 "billboardShutdown();"):
        if shutdown.count(call) != 1 or sum(call in gate for gate in shutdown_gates) != 1:
            raise AssertionError(f"Cockpit teardown route escaped owner gate: {call}")
    if shutdown.count("runCockpitVisualsOwned(g_state->cockpitVisualsOwnerAllowed, [&] {") != 4:
        raise AssertionError("direct Cockpit shutdowns must use the cached State owner bit")
    if shutdown.count("shutdownRemlokLifecycle();") != 1 or \
            "if (!g_state || !g_state->cockpitVisualsOwnerAllowed) return;" not in \
            body(vscreen, "void shutdownRemlokLifecycle()"):
        raise AssertionError("RemLok lifecycle shutdown must return immediately when Cockpit ownership is denied")

    if "bool cockpitOwnerBoundaryChecks(" not in cockpit_api_fixture or \
            "ok &= cockpitOwnerBoundaryChecks(immediate, stock, cfg);" not in cockpit_api_fixture:
        raise AssertionError("real Cockpit owner-absence fixture is not invoked")
    api_owner_fixture = body(cockpit_api_fixture,
                             "bool cockpitOwnerBoundaryChecks(")
    for token in ("runCockpitVisualsOwned(false", "ownerCallbackCalls == 0",
                  "g_targetConfigReads == 0", "g_bindingResolveCalls == 0",
                  "ownerDeniedShader == stock", "runCockpitVisualsOwned(true"):
        if token not in api_owner_fixture:
            raise AssertionError(f"real Cockpit owner-absence fixture lost evidence: {token}")
    if "bool testCockpitOwnerDeniedGlareAndCore(" not in vscreen_predicate_fixture or \
            "okay &= testCockpitOwnerDeniedGlareAndCore(device, immediate);" not in vscreen_predicate_fixture:
        raise AssertionError("owner-denied glare/core fixture is missing")
    predicate_owner_fixture = body(vscreen_predicate_fixture,
                                   "bool testCockpitOwnerDeniedGlareAndCore(")
    for token in ("vScreenSunglareSkipPredicateTestVisit(",
                  "shared Comfort tracking stamps the exact glare train",
                  "owner absent leaves the real core context visitor and trace active",
                  "sunglarePredicateTestExposureDamping(true)"):
        if token not in predicate_owner_fixture:
            raise AssertionError(f"owner-denied VScreen fixture lost evidence: {token}")
    if "owner-denied real TargetSharp visitor ignores stale active state before module facts or WARP candidate queries" not in vscreen_predicate_fixture:
        raise AssertionError("real TargetSharp owner-denied WARP candidate evidence is missing")

    # Comfort absence is published once with the graphics hooks, then copied
    # into VScreen state. All owner work consumes that immutable decision;
    # native FSS treats only the explicit S_FALSE provider result as absence.
    if "bool comfortOwnerAllowed = false;" not in device or \
            "bool comfortOwnerAllowed = false;" not in vscreen:
        raise AssertionError("Comfort ownership caches must default to denied")
    if "if (!ownerAllowed) return false;\n    std::forward<Callback>(callback)();\n    return true;" not in comfort_owner_gate:
        raise AssertionError("denied Comfort owner helper must return without invoking its callback")
    hook_startup = body(device, "void hookDevice(ID3D11Device* device)")
    ordered(hook_startup, "if (!s.deviceHook.commit())", "s.device = device;",
            "s.comfortOwnerAllowed =\n        plugin_admission_transport::ownerAllowedForStartup(\n            plugins::kPluginComfort);",
            "runComfortOwned(s.comfortOwnerAllowed, [&] {\n        fssResConfigure(sentinelCfg);")
    comfort_query = "plugin_admission_transport::ownerAllowedForStartup(\n            plugins::kPluginComfort);"
    if device.count(comfort_query) != 1 or hook_startup.count(comfort_query) != 1:
        raise AssertionError("device hook must sample Comfort ownership exactly once after commit")
    if hook_startup.count("fssResConfigure(sentinelCfg);") != 1 or \
            "fssResConfigure(sentinelCfg);" not in body(
                hook_startup, "runComfortOwned(s.comfortOwnerAllowed, [&] {"):
        raise AssertionError("pre-commit FSS config must be deferred inside the published owner gate")
    ordered(hook_startup, comfort_query,
            "runComfortOwned(s.comfortOwnerAllowed, [&] {\n"
            "        if (initializeComfortStartup(s))")
    if "initializeComfortStartup(s)" not in body(
            hook_startup,
            "runComfortOwned(s.comfortOwnerAllowed, [&] {\n"
            "        if (initializeComfortStartup(s))"):
        raise AssertionError("initial FSS bindings/latch must be contained by the published owner gate")
    if hook_startup.count("initializeComfortStartup(s)") != 1 or \
            device.count("initializeComfortStartup(") != 2:
        raise AssertionError("initial Comfort FSS bindings/latch must run once after owner publication")
    comfort_startup = body(device, "bool initializeComfortStartup(State& state)")
    ordered(comfort_startup,
            "if (!state.comfortOwnerAllowed || state.comfortStartupInitialized)",
            "state.comfortStartupInitialized = true;",
            "state.fssEnterKey.setGameMirrored(true);",
            "if (Config::get().getBool(\"hotkey.read_game_bindings\", true))",
            "state.fssModeLatchWanted = eyeSyncFromConfig(Config::get()).any();")
    startup_state = body(device, "State& ensureState()")
    if "fssModeLatchWanted = false;" not in device or \
            "initializeComfortStartup(" in startup_state or \
            "state.fssEnterKey.setGameMirrored(true)" in startup_state or \
            "state.fssModeLatchWanted = eyeSyncFromConfig(Config::get()).any();" in startup_state:
        raise AssertionError("ensureState must leave owner-dependent FSS setup cold until post-commit publication")
    if "comfortStartupInitialized = false;" not in device:
        raise AssertionError("Comfort startup FSS adoption must be a per-State one-shot")
    if "ownerAllowedForStartup" in body(device, "HRESULT STDMETHODCALLTYPE hookedPresent("):
        raise AssertionError("Present must consume the cached Comfort decision")
    for gate_start, call in (
            ("runComfortOwned(s.comfortOwnerAllowed, [&] {\n                installExposureFix(device, ctxMode);",
             "installExposureFix(device, ctxMode);"),
            ("runComfortOwned(s.comfortOwnerAllowed, [] {\n                installGlitchFrameFix();",
             "installGlitchFrameFix();")):
        if call not in body(hook_startup, gate_start):
            raise AssertionError(f"Comfort startup install is outside cached owner admission: {call}")
    if "terrainCullCoverInstall(s.comfortOwnerAllowed," not in hook_startup:
        raise AssertionError("terrain startup must receive the published Comfort owner decision")
    device_shutdown = body(device, "void shutdownDeviceHooks()")
    comfort_shutdown = body(device_shutdown, "if (comfortOwnerAllowed()) {")
    if "shutdownGlitchFrameFix();" not in comfort_shutdown or \
            "plugins::kPluginComfort" not in comfort_shutdown or \
            "transitionFlashEyeBaseShutdown();" not in comfort_shutdown or \
            device_shutdown.count("if (comfortOwnerAllowed())") != 2 or \
            "if (comfortOwnerAllowed()) shutdownExposureFix();" not in device_shutdown:
        raise AssertionError("device-hook Comfort teardown must skip denied transition and exposure paths")

    vstartup = body(vscreen, "void installVScreenFixes(")
    if "const bool comfortOwnerAllowed = edvr::comfortOwnerAllowed();" not in vstartup or \
            "g_state->comfortOwnerAllowed = comfortOwnerAllowed;" not in vstartup:
        raise AssertionError("VScreen must copy the device-published Comfort decision into State")
    if "ownerAllowedForStartup(\n            plugins::kPluginComfort)" in vstartup or \
            "ownerAllowedForStartup(\n            plugins::kPluginComfort)" in vscreen:
        raise AssertionError("VScreen must not re-query Comfort admission during setup, Present, or shutdown")
    if "comfortOwnerAllowed" not in body(vscreen, "void vScreenFrameBoundary()"):
        raise AssertionError("Comfort frame paths must use the cached State bit")
    if "configureLegacyDrawInterests(g_state->cockpitVisualsOwnerAllowed,\n                                 g_state->comfortOwnerAllowed,\n                                 g_state->introOwnerAllowed,\n                                 g_state->onFootPanelOwnerAllowed);" not in vstartup or \
            "configureLegacyDrawInterests(s->cockpitVisualsOwnerAllowed,\n                                 s->comfortOwnerAllowed,\n                                 s->introOwnerAllowed,\n                                 s->onFootPanelOwnerAllowed);" not in body(
                vscreen, "void vScreenRefreshConfig()"):
        raise AssertionError("legacy draw-interest configuration must receive all four cached owner decisions")
    interests = body(vscreen, "static bool configureLegacyDrawInterests(")
    if "runComfortOwned(comfortOwnerAllowed, [&] {\n        enable(InterestId::FssPanel" not in interests or \
            "runComfortOwned(comfortOwnerAllowed, [&] {\n        filtersFit = filtersFit && append(fssPanelDrawInterestFilters)" not in interests:
        raise AssertionError("Comfort scanner interests and filters must remain cold when denied")
    for signature, calls in (
            ("void vScreenRefreshConfig()", ("fssResConfigure(cfg)", "exposureConfigure(cfg)")),):
        scope = body(vscreen, signature)
        comfort_gates = [body(scope[offset:], "runComfortOwned(")
                         for offset in (i for i in range(len(scope))
                                        if scope.startswith("runComfortOwned(", i))]
        for call in calls:
            if scope.count(call) != 1 or sum(call in gate for gate in comfort_gates) != 1:
                raise AssertionError(f"{signature} must owner-gate exactly one {call}")

    gates = body(vscreen, "bool registerLegacyDrawGates(State* state,")
    if "if (!legacyDrawGateOwnerAllowed(registration, state)) continue;" not in gates:
        raise AssertionError("denied Comfort/Cockpit legacy gates must be omitted before registry dispatch")
    owner_filter = body(vscreen, "bool legacyDrawGateOwnerAllowed(")
    for token in ("LegacyGateOwner::Core: return true;",
                  "case LegacyGateOwner::Comfort:",
                  "return state && state->comfortOwnerAllowed;",
                  "case LegacyGateOwner::Intro:",
                  "return state && state->introOwnerAllowed;",
                  "case LegacyGateOwner::CockpitOrComfort:",
                  "state->comfortOwnerAllowed"):
        if token not in owner_filter:
            raise AssertionError(f"legacy draw registration owner filter lost {token}")
    registrations = body(vscreen, "constexpr LegacyDrawGateRegistration kLegacyDrawGateRegistrations[] =")
    if registrations.count('{"legacy.') != 30:
        raise AssertionError("state-backed owner metadata must preserve the exact 30-row legacy registry table")
    for row in ("legacy.transition-flash-eye-count", "legacy.fss-resolution",
                "legacy.fss-panel", "legacy.fss-reveal", "legacy.sun-glare",
                "legacy.hologram-depth", "legacy.backdrop", "legacy.loader-panel",
                "legacy.intro-panel", "legacy.intro-curve"):
        if row not in registrations:
            raise AssertionError(f"Comfort draw-gate registration missing {row}")
    for row in ("{\"legacy.transition-flash-eye-count\", &drawGate_transition_flash_eye_count, true, LegacyGateOwner::Comfort}",
                "{\"legacy.fss-resolution\", &drawGate_fss_resolution, true, LegacyGateOwner::Comfort}",
                "{\"legacy.fss-panel\", &drawGate_fss_panel, true, LegacyGateOwner::Comfort}",
                "{\"legacy.fss-reveal\", &drawGate_fss_reveal, true, LegacyGateOwner::Comfort}"):
        if row not in registrations:
            raise AssertionError("Comfort draw bridges must carry State for their cached owner checks")
    if ("{\"legacy.sun-glare\", &drawGate_sun_glare, true, "
            "LegacyGateOwner::CockpitOrComfort}" not in registrations):
        raise AssertionError("shared glare gate must remain subscribed for either admitted owner")
    for row in (
            "{\"legacy.hologram-depth\", &drawGate_hologram_depth, true, LegacyGateOwner::Intro}",
            "{\"legacy.backdrop\", &drawGate_backdrop, true, LegacyGateOwner::Intro}",
            "{\"legacy.loader-panel\", &drawGate_loader_panel, true, LegacyGateOwner::Intro}",
            "{\"legacy.intro-panel\", &drawGate_intro_panel, true, LegacyGateOwner::Intro}",
            "{\"legacy.intro-curve\", &drawGate_intro_curve, true, LegacyGateOwner::Intro}"):
        if row not in registrations:
            raise AssertionError(f"Intro draw subscription must carry cached owner State: {row}")
    if '{"legacy.panel-curve", &drawGate_panel_curve, true, LegacyGateOwner::OnFootPanel}' not in registrations:
        raise AssertionError("panel-curve observer must retain On-foot ownership while its runtime remains shared with Intro")
    comfort_gates = body(vscreen, "#define EDVR_GATE_COMFORT(name, expression)")
    if "if (!s || !s->comfortOwnerAllowed) return 0;" not in comfort_gates:
        raise AssertionError("owner-only Comfort draw bridges must exit before evaluating module predicates")
    intro_gates = body(vscreen, "#define EDVR_GATE_INTRO(name, expression)")
    if "if (!s || !s->introOwnerAllowed) return 0;" not in intro_gates:
        raise AssertionError("owner-only Intro draw bridges must exit before evaluating module predicates")

    scanner_config = body(vscreen, "void configureScannerLifecycleStage(")
    scanner_frame = body(vscreen, "void frameScannerLifecycleStage(")
    scanner_shutdown = body(vscreen, "void shutdownScannerLifecycle()")
    for scope, guard in ((scanner_config, "if (!g_state || !g_state->comfortOwnerAllowed) return;"),
                         (scanner_frame, "if (!g_state || !g_state->comfortOwnerAllowed) return;"),
                         (scanner_shutdown, "if (!g_state || !g_state->comfortOwnerAllowed) return;")):
        if guard not in scope:
            raise AssertionError("Scanner registry and direct fallback routes must stop when Comfort is denied")
    for lifecycle in ("bool ensureScannerLifecycle()", "bool ensureComfortLifecycle()"):
        scope = body(vscreen, lifecycle)
        if "if (!g_state || !g_state->comfortOwnerAllowed) return false;" not in scope or \
                "pluginRegistryRegisterLifecycle(" not in scope:
            raise AssertionError(f"{lifecycle} must avoid Comfort factory/registration when denied")
    if "if (!g_state || !g_state->comfortOwnerAllowed) return;" not in \
            body(vscreen, "void configureComfortLifecycle(Config& cfg)"):
        raise AssertionError("transition configuration must skip registry and direct fallback when denied")

    fss_provider = body(native_fss, "extern \"C\" HRESULT WINAPI edvrAcquireNativeFss(")
    ordered(fss_provider, "*t={sizeof(*t),EDVR_NATIVE_FSS_VERSION_2};",
            "const bool ownerAllowed=edvr::comfortOwnerAllowed();",
            "if(!ownerAllowed)return S_FALSE;", "std::lock_guard<std::mutex>lock(mu);")
    if "fssHealAcquire()" in fss_provider[:fss_provider.find("if(!ownerAllowed)return S_FALSE;")]:
        raise AssertionError("denied native FSS acquire must not allocate module state or acquire healing")
    if "nativeFssComfortOwnerDenied()" not in native_fss_fixture or \
            "nativeFssComfortOwnerDenied();" not in native_fss_fixture:
        raise AssertionError("real denied native FSS acquire fixture is missing or not invoked")
    absent_table = body(native_fss_client, "static bool validOwnerAbsentTable(")
    if "!table.context" not in absent_table or "!table.close" not in absent_table:
        raise AssertionError("FSS client must validate the complete empty versioned absence table")
    client_acquire = body(native_fss_client, "HRESULT acquire(")
    if "if(r==S_FALSE)" not in client_acquire or "validOwnerAbsentTable(candidate)" not in client_acquire or \
            "if(r!=S_OK)" not in client_acquire:
        raise AssertionError("FSS client must accept only a valid S_FALSE absence response and preserve other errors")
    host_open = body(native_runtime_host, "bool open(const RuntimeOptions& options)")
    startup_treatment_call = (
        "if(!acquireStartupTreatmentProviders(options.graphicsProvider,externalDevice,runtimeGeneration))return false;")
    if host_open.count(startup_treatment_call) != 1:
        raise AssertionError("native startup must use the tested production treatment acquisition seam exactly once")
    ordered(host_open, "runtimeGeneration=gate.beginGeneration();",
            "compositorGeneration=poses.begin();",
            "if(runtimeGeneration&&compositorGeneration&&options.graphicsProvider&&externalDevice) {",
            startup_treatment_call,
            "acquired=features.acquire(options.graphicsProvider",
            "acquired=fss.acquire(options.graphicsProvider",
            "acquired=timing.acquire(options.graphicsProvider")
    host_fss = native_runtime_host
    fss_acquire = host_fss.find("acquired=fss.acquire(options.graphicsProvider")
    host_fss_scope = host_fss[fss_acquire:fss_acquire + 900] if fss_acquire >= 0 else ""
    ordered(host_fss_scope, "acquired=fss.acquire(options.graphicsProvider", "acquired!=S_OK&&acquired!=S_FALSE",
            "native_fss,provider_absent=comfort_owner_denied")
    if "if(acquired!=S_OK)return false;" in host_fss_scope:
        raise AssertionError("native host must continue on explicit Comfort absence but reject actual acquire errors")
    if "if(fss.acquired())" not in host_fss:
        raise AssertionError("native FSS frame callbacks must be skipped when the optional provider is absent")
    if '#include "temporal_startup_cases.h"' not in native_treatment_runner or \
            "edvr::openxr::test::runTemporalStartupCases(check);" not in native_treatment_runner:
        raise AssertionError("production startup treatment acquisition fixture is missing or not invoked")
    if "edvr::openxr::test::runTreatmentCases(check);" not in native_treatment_runner:
        raise AssertionError("real native host treatment fixture is not invoked")
    treatment_cases = body(native_treatment_fixture,
                           "template<class Check> void runTreatmentCases(Check check)")
    for token in ("state.fssAcquireMode=State::FssAcquireMode::ownerAbsent;\n    const HRESULT absent=h.fss.acquire(provider,device.Get(),1);",
                  "absent==S_FALSE&&!h.fss.acquired()",
                  "State::FssAcquireMode::malformedAbsent", "State::FssAcquireMode::failure",
                  "State::FssAcquireMode::otherPositive", "State::FssAcquireMode::admitted",
                  "check(h.fss.close()==S_OK&&!h.fss.acquired(),\n    \"host releases the admitted FSS table before its isolated absence capture\");\n  state.fssAcquireMode=State::FssAcquireMode::ownerAbsent;\n  check(h.fss.acquire(provider,device.Get(),2)==S_FALSE&&!h.fss.acquired(),",
                  "result==vr::VRCompositorError_None&&state.calls==\"TSPM\"&&h.graphicsCalls.calls-before==3",
                  "the real host treatment chain feeds each later stage the prior output with FSS unacquired"):
        if token not in treatment_cases:
            raise AssertionError(f"loaded FSS client/host absence fixture lost evidence: {token}")

    # The first D3D device publishes the owner decision before swapchain or
    # factory hooks can expose Present. Native runtime startup then waits for
    # a render callback before invoking the host, whose FSS acquire sees that
    # already-published provider state.
    proxy_attach = body(proxy, "void attachToDevice(ID3D11Device* device,")
    ordered(proxy_attach, "edvr::publishGameDevice(device);", "edvr::hookDevice(device);",
            "edvr::hookSwapChain(swapChain);", "edvr::hookFactoryForDevice(device);")
    module_start = body(native_module, "vr::EVRInitError start(")
    ordered(module_start, "generation->binding.acquire(options_.graphicsProxy",
            "generation->binding.waitForRender(", "generation->route.bind()",
            "generation->host->start(token,cancelled,interfaces)")
    if "plugin_admission_transport::ownerAllowedForStartup(\n            plugins::kPluginComfort)" in module_start:
        raise AssertionError("OpenXR startup must consume published owner state instead of re-querying admission")

    native_frame_state = body(native_frame, "extern \"C\" HRESULT WINAPI edvrAcquireNativeFrame(")
    if "state.comfortOwnerAllowed = edvr::comfortOwnerAllowed();" not in native_frame_state:
        raise AssertionError("native frame provider must snapshot the published Comfort decision")
    frame_core = body(native_frame, "HRESULT WINAPI beginFrame(")
    if "state->comfortOwnerAllowed" not in frame_core or "transitionEnabled = state->comfortOwnerAllowed &&" not in native_frame:
        raise AssertionError("native frame transition output and latch must honor cached owner state")

    # Native Temporal-AA is optional, while Comfort owns the shared terrain
    # frusta publisher through native FSS. Absence at either provider must be
    # exact and must not reserve its pool or perform device/config work.
    temporal_state = body(native_temporal, "struct State {")
    temporal_acquire = body(native_temporal,
                            "extern \"C\" HRESULT WINAPI edvrAcquireNativeTemporal(")
    temporal_begin = body(native_temporal, "HRESULT WINAPI begin(")
    if "edvr::comfortOwnerAllowed()" in temporal_acquire or \
            "terrainCullCoverNoteFrusta" in temporal_begin:
        raise AssertionError("Temporal-AA provider must not own or duplicate the shared Comfort frusta publisher")
    ordered(temporal_acquire, "*t={sizeof(*t),EDVR_NATIVE_TEMPORAL_VERSION_1};",
            "if(!edvr::temporalAAOwnerAllowed())return S_FALSE;",
            "std::lock_guard<std::mutex> lock(mutex)", "s.pendingSettings=readConfig();")
    if "temporalAAOwnerAllowed()" in temporal_begin:
        raise AssertionError("native Temporal-AA Begin must use its acquired provider without re-querying admission")
    temporal_fixture_run = body(native_temporal_fixture, "void run()")
    if "nativeTemporalOwnerDenied();" not in native_temporal_fixture or \
            "Temporal-AA-denied provider returns exact S_FALSE and a versioned empty table" not in native_temporal_fixture:
        raise AssertionError("real native Temporal-AA denied/admitted provider fixture is missing or not invoked")
    fss_state = body(native_fss, "struct State {")
    fss_acquire = body(native_fss, "extern \"C\" HRESULT WINAPI edvrAcquireNativeFss(")
    fss_begin = body(native_fss, "HRESULT WINAPI begin(void* p,const EdvrNativeFssFrame* f)")
    if "ownerAllowed=false" not in fss_state or \
            fss_acquire.count("edvr::comfortOwnerAllowed()") != 1:
        raise AssertionError("native FSS must cache Comfort ownership once for its shared frusta service")
    ordered(fss_acquire, "*t={sizeof(*t),EDVR_NATIVE_FSS_VERSION_2};",
            "const bool ownerAllowed=edvr::comfortOwnerAllowed();",
            "if(!ownerAllowed)return S_FALSE;",
            "GetDeviceRemovedReason()", "std::lock_guard<std::mutex>lock(mu)")
    ordered(fss_begin, "if(s->ownerAllowed)edvr::terrainCullCoverNoteFrusta(f->frusta);",
            "s->sync=edvr::eyeSyncFromConfig(edvr::Config::get());")
    if "nativeFssPublishesComfortFrustaOnce();" not in native_fss_fixture or \
            "Comfort FSS publishes the exact valid game-facing frusta once even when healing is off" not in native_fss_fixture or \
            "nativeFssComfortOwnerDenied();" not in native_fss_fixture:
        raise AssertionError("native FSS must retain its denied-owner and shared frusta positive-control fixtures")
    terrain_install = body(terrain_cover, "void terrainCullCoverInstall(bool ownerAllowed, bool flatProfile)")
    if "if (!ownerAllowed) return;" not in terrain_install:
        raise AssertionError("denied Comfort ownership must skip terrain hook installation")
    for terrain_scope in (body(terrain_cover, "void terrainCullCoverNoteFrusta("),
                          body(terrain_cover, "void terrainCullCoverPoll(")):
        if "!g_ownerAllowed.load(std::memory_order_acquire)" not in terrain_scope or \
                "!g_installed.load(std::memory_order_acquire)" not in terrain_scope:
            raise AssertionError("terrain NoteFrusta and polling must remain cold when installation was denied")
    if "terrainCullCoverInstall(false, false)" not in terrain_cover_fixture or \
            "Comfort owner absence skips terrain scan/install" not in terrain_cover_fixture:
        raise AssertionError("terrain denial fixture evidence is missing")

    if "comfortOwnerDeniedLifecycleFallbacks()" not in plugin_dispatch_fixture or \
            "comfortOwnerDeniedLifecycleFallbacks();" not in plugin_dispatch_fixture:
        raise AssertionError("Comfort denied lifecycle/fallback fixture is missing or not invoked")
    if "core context census remains live in the real visitor while Comfort ownership is denied" not in vscreen_predicate_fixture:
        raise AssertionError("core context service must remain live in the denied-owner visitor fixture")
    for token in ("kFssPanelClaim", "kFssRevealClaim", "true, false, 'X', 6, 1, 5, 6",
                  "g_getVertexShaderCalls == 0", "owner-present stale FSS panel reaches the real scanner selector"):
        if token not in vscreen_predicate_fixture:
            raise AssertionError(f"real denied Scanner visitor fixture lost {token}")
    if ("fssPanelSite, immediate, immediate, 0, true, true,\n"
            "                              &deniedScanner, true, false, 'X', 6, 1, 5, 6)" not in
            vscreen_predicate_fixture):
        raise AssertionError("FSS panel real visitor must pass Comfort denied to its owner gate")
    if "testDiagnosticsOwnerLegacyGateRegistration()" not in vscreen_predicate_fixture or \
            "okay &= testDiagnosticsOwnerLegacyGateRegistration();" not in vscreen_predicate_fixture:
        raise AssertionError("actual 64-combination owner registry fixture is missing or not invoked")
    gate_fixture = body(vscreen_predicate_fixture,
                        "bool testDiagnosticsOwnerLegacyGateRegistration()")
    for token in (
            "for (unsigned mask = 0; mask < 64; ++mask)",
            "(mask & 1u) != 0", "(mask & 2u) != 0", "(mask & 4u) != 0",
            "(mask & 8u) != 0", "(mask & 16u) != 0", "(mask & 32u) != 0",
            "cockpit, comfort, intro, onFoot, diagnostics, temporalAA,",
            "(cockpit ? 6u : 0u)",
            "(comfort ? 4u : 0u)", "(intro ? 6u : 0u)",
            "(onFoot ? 3u : 0u)", "((cockpit || comfort) ? 1u : 0u)",
            "(diagnostics ? 7u : 0u)",
            "(temporalAA ? 1u : 0u)",
            "((diagnostics || temporalAA) ? 2u : 0u)",
            "gotIntro == (intro ? 6u : 0u)",
            "gotDiagnostics == (diagnostics ? 7u : 0u)",
            "temporalShared == ((diagnostics || temporalAA) ? 2u : 0u)",
            "temporalOwner == (temporalAA ? 1u : 0u)",
            "actual legacy registry preserves exact callback inventory across all 64 owner combinations"):
        if token not in gate_fixture:
            raise AssertionError(f"64-combination owner registry fixture lost {token}")
    intro_visitor_fixture = body(vscreen_predicate_fixture,
                                 "bool testIntroOwnerDeniedVisitor(")
    for token in ("SiteId::kIntroPanelClaim", "SiteId::kIntroCurveObserve",
                  "true, true, 'X', 6, 1, 0, 0, 0, false",
                  "SiteOutcome::Declined", "g_resourceAttempts == 0",
                  "SiteOutcome::Observed", "afterDenied.entries == before.entries",
                  "edvr::panelCurveSurfaceWanted()", "afterAllowed.entries == before.entries + 1",
                  "okay &= testIntroOwnerDeniedVisitor(device, immediate);"):
        if token not in intro_visitor_fixture and token not in vscreen_predicate_fixture:
            raise AssertionError(f"real Intro denied/admitted visitor fixture lost {token}")
    scrim_fixture = body(vscreen_predicate_fixture,
                         "bool testIntroOwnerDeniedScrim(")
    for token in ("SiteId::kScrimClaim", "g_scrimOn = true",
                  "SiteOutcome::Declined", "g_resourceAttempts == 0",
                  "SiteOutcome::Claimed", "g_resourceAttempts == 1",
                  "testIntroOwnerDeniedScrim(device, immediate);"):
        if token not in scrim_fixture and token not in vscreen_predicate_fixture:
            raise AssertionError(f"real Intro scrim denied/admitted WARP fixture lost {token}")
    if 'EDVR_GATE_INTRO(scrim, scrimWantsDraws())' not in vscreen or \
            '{"legacy.scrim", &drawGate_scrim, true, LegacyGateOwner::Intro}' not in vscreen or \
            'if (!s->introOwnerAllowed) return SiteResult::declined();' not in vscreen or \
            'runIntroOwned(g_state && g_state->introOwnerAllowed,\n                      [&] { scrimBegin(self); });' not in vscreen or \
            'runIntroOwned(g_state && g_state->introOwnerAllowed,\n                      [&] { scrimEnd(self); });' not in vscreen or \
            'runIntroOwned(s->introOwnerAllowed, [&] { scrimConfigure(cfg); });' not in vscreen or \
            'runIntroOwned(g_state->introOwnerAllowed, [&] { scrimConfigure(cfg); });' not in vscreen or \
            'runIntroOwned(g_state->introOwnerAllowed, [&] { scrimShutdown(); });' not in vscreen:
        raise AssertionError("Intro scrim legacy gate, visitor, lifecycle, and teardown must use the cached Intro owner")
    admission_fixture = body(plugin_dispatch_fixture, "void testIntroStartupAdmissionSnapshot()")
    for token in ("introStartupAdmission(unavailable, false)",
                  "introStartupAdmission(vrCandidate, true)",
                  "vrIntro.strictSelection == vrAdmitted",
                  "introStartupAdmission(providerRejected, true)",
                  "introStartupAdmission(malformed, true)",
                  "legacyRejected.ownerAllowed && !legacyRejected.strictSelection",
                  "testIntroStartupAdmissionSnapshot();"):
        if token not in admission_fixture and token not in plugin_dispatch_fixture:
            raise AssertionError(f"Intro startup query/Legacy/strict selection fixture lost {token}")
    strict_policy_fixture = body(
        plugin_dispatch_fixture,
        "void testDiagnosticsTemporalStartupAdmissionPolicy()")
    for token in ("makeDecision(diagnosticsBit)", "makeDecision(temporalAaBit)",
                  "makeDecision(introBit)", "makeDecision(0)",
                  "controlled strict Diagnostics-only selection keeps the shared startup path awake",
                  "controlled strict Temporal-AA-only selection keeps the shared startup path awake",
                  "strict Intro-only selection retains Intro startup without waking the Diagnostics/Temporal-AA exception",
                  "real generated empty catalog remains Legacy-compatible and supplies no strict-positive runtime evidence",
                  "failed and malformed snapshots fail closed even when cached Diagnostics and Temporal-AA owner flags are present"):
        if token not in strict_policy_fixture:
            raise AssertionError(f"controlled strict-policy fixture lost {token}")
    if "testDiagnosticsTemporalStartupAdmissionPolicy();" not in plugin_dispatch_fixture:
        raise AssertionError("controlled D/TAA strict-policy fixture must stay invoked")
    registry_visit = body(registry, "uint32_t pluginRegistryVisitLegacyDrawGatesForTest(")
    if "for (uint32_t i = 0; i < g_legacyCount; ++i)" not in registry_visit or \
            "gate.predicate(gate.state ? gate.state : legacyState)" not in registry_visit:
        raise AssertionError("draw-gate fixture must invoke the actual stored registry callbacks")
    if "pluginRegistryVisitLegacyDrawGatesForTest(void* legacyState) noexcept;" not in registry_header:
        raise AssertionError("stored draw-gate visit seam must be declared for the fixture")
    if "nativeFrameComfortOwnerDenied" not in native_frame_fixture or \
            "nativeFrameComfortOwnerDenied();" not in native_frame_fixture or \
            "deniedComfortOutput.transitionEnabled == 0" not in native_frame_fixture:
        raise AssertionError("real native-frame denied-owner transition fixture is missing or not invoked")
    temporal_run = body(native_temporal_fixture, "void run()")
    if "coverNotes==0" not in temporal_run or \
            "Temporal-AA frame begin leaves Comfort frusta publication to the FSS provider" not in temporal_run:
        raise AssertionError("native Temporal-AA fixture must prove its frame does not duplicate Comfort frusta publication")
    ordered(temporal_run, "auto t=acquire(d,11)", "auto f=frame(11,1);auto p=begin(t,f)",
            "check(coverNotes==0,\"Temporal-AA frame begin leaves Comfort frusta publication to the FSS provider without duplication\")",
            "check(p.tangentShift[0][0]==0&&p.tangentShift[1][1]==0",
            "check(treat(t,1,1,source.Get())==S_OK")
    if "comfortOwnerAllowed" in temporal_run or "runComfortOwned" in temporal_run:
        raise AssertionError("independent native temporal core must not be swallowed by Comfort ownership")

    check_onfoot_boundary(sources)
    check_temporal_aa_boundary(sources)


def check_temporal_aa_boundary(sources: dict[str, str]) -> None:
    """Keep Temporal-AA config and callbacks behind the published owner cache."""
    device = sources["device"]
    vscreen = sources["vscreen"]
    native_temporal = sources["native_temporal"]
    native_sharpen = sources["native_sharpen"]
    screen_motion = sources["screen_motion"]
    flat_runtime = sources["flat_runtime"]
    exposure_fix = sources["exposure_fix"]
    world_route = sources["vr_world_route"]
    ui_layer = sources["ui_layer"]
    vscreen_res = sources["vscreen_res"]
    vscreen_res_header = sources["vscreen_res_header"]
    menu = sources["menu"]
    intro_upscale = sources["intro_upscale"]
    sampler_policy = sources["texture_sampler_policy"]
    sampler_policy_header = sources["texture_sampler_policy_header"]

    if "bool temporalAAOwnerAllowed = false;" not in body(device, "struct State {"):
        raise AssertionError("device Temporal-AA owner cache must default to denied")
    startup = body(device, "void hookDevice(")
    if startup.count("plugin_admission_transport::ownerAllowedForStartup(\n            plugins::kPluginTemporalAa)") != 1:
        raise AssertionError("device hook must sample Temporal-AA admission exactly once")
    ordered(startup, "if (!s.deviceHook.commit())", "s.device = device;",
            "s.temporalAAOwnerAllowed =", "plugins::kPluginTemporalAa)",
            "runComfortOwned(s.comfortOwnerAllowed")
    if "return g_state && g_state->temporalAAOwnerAllowed;" not in body(
            device, "bool temporalAAOwnerAllowed() noexcept"):
        raise AssertionError("Temporal-AA getter must expose only the cached device decision")

    present_boundary = body(device, "void presentFrameBoundary()")
    celestial_status_tick = (
        "if (g_state->temporalAAOwnerAllowed)\n"
        "        tkCelestialStatus.run(tickCelestialStatus);"
    )
    if present_boundary.count(celestial_status_tick) != 1:
        raise AssertionError(
            "Celestial Status must skip its budgeted frame tick without the cached Temporal-AA owner")

    resolver = body(vscreen_res, "void resolveVScreenTargetResolution(")
    route_facts = body(vscreen_res, "vscreenfit::RouteFacts routeFactsFromConfig(")
    denied_facts = body(route_facts, "} else {")
    if "bool temporalAAOwnerAllowed" not in vscreen_res_header or \
            "bool temporalAAOwnerAllowed" not in vscreen_res or \
            "in.route = routeFactsFromConfig(cfg, temporalAAOwnerAllowed);" not in resolver:
        raise AssertionError("vScreen fit resolver must receive and forward the cached Temporal-AA owner")
    ordered(route_facts, "if (temporalAAOwnerAllowed) {",
            'cfg.getString("fix.ui_quality", "100")',
            'cfg.getString("fix.temporal_aa", "off")',
            "f.layerWhy = uiLayerNotLiveReasonFor(", "} else {")
    if 'f.layerWhy = "the Temporal-AA owner is not admitted";' not in denied_facts or \
            "fix.ui_quality" in denied_facts or "fix.temporal_aa" in denied_facts:
        raise AssertionError("denied fit route must be truthful while leaving cold Temporal-AA keys unread")
    if "resolveVScreenTargetResolution(cfg, &w, &h, s.temporalAAOwnerAllowed);" not in startup or \
            "resolveVScreenTargetResolution(Config::get(), &vw, &vh, temporalAAOwnerAllowed()," not in menu or \
            "resolveVScreenTargetResolution(cfg, &vw, &vh, temporalAAOwnerAllowed()," not in intro_upscale:
        raise AssertionError("vScreen fit callers must supply the published Temporal-AA decision")
    preset_disabled = body(menu, "bool dlssPresetDisabled()")
    status = body(menu, "void buildStatus(")
    if "if (!temporalAAOwnerAllowed()) return true;" not in preset_disabled or \
            "Config::get().getString(\"fix.temporal_aa\", \"off\")" not in preset_disabled:
        raise AssertionError("DLSS preset dimming must stop before reading Temporal-AA config when owner is absent")
    if 'if (!temporalAAOwnerAllowed()) {\n        statusLine(c, "Temporal AA", "owner not admitted");\n    } else {' not in status or \
            "const std::string mode = Config::get().getString(\"fix.temporal_aa\", \"off\");" not in status:
        raise AssertionError("menu Temporal-AA telemetry must show owner denial without reading its mode")
    if "if (dlssRow && temporalAAOwnerAllowed()) {" not in menu or \
            "runtimeFlatProfile() ? Config::get().requestedTemporalMode()" not in menu:
        raise AssertionError("DLSS preset row label must read the mode only under Temporal-AA ownership")
    warning = body(menu, "void flatWarningTick(")
    ordered(warning, "const bool refusing = temporalAAOwnerAllowed() &&",
            "flatRuntimeStructuralRefusal(&reason, &standing)",
            "temporalModeEnabled(Config::get().requestedTemporalMode())",
            "if (refusing || s.open || s.flatSettingsForce)",
            "if (s.flatSettings.poll(now, force))")
    if "if (runtimeFlatProfile() && temporalAAOwnerAllowed()) {\n            const char* wrapper = contextWrapperFile();" not in menu:
        raise AssertionError("flat wrapper note must not probe wrappers or read requested mode without TAA ownership")
    ordered(startup, "s.temporalAAOwnerAllowed =",
            "const bool samplerOwnerAllowed =",
            "TextureSamplerHostContextV1 samplerHostContext{",
            "s.temporalAAOwnerAllowed ? 1u : 0u",
            "samplerStartupConfigureForOwner(",
            "runComfortOwned(s.comfortOwnerAllowed")
    if startup.count("plugins::kPluginTemporalAa)") != 1 or \
            "struct TextureSamplerHostContextV1" not in sampler_policy_header or \
            "sizeof(TextureSamplerHostContextV1) == 12" not in sampler_policy_header:
        raise AssertionError("sampler policy must receive the single cached Temporal-AA snapshot through its typed host context")
    sampler_config = body(sampler_policy, "void resolveTextureSamplerSettingsV1(")
    ordered(sampler_config, "bool temporalOwnerAllowed = false;",
            "static_cast<const TextureSamplerHostContextV1*>(hostContext)",
            "context->structSize == sizeof(*context)",
            "context->version == kTextureSamplerHostContextVersion1",
            "context->temporalAAOwnerAllowed <= 1u",
            "temporalOwnerAllowed = context->temporalAAOwnerAllowed != 0;",
            '"advanced.texture_anisotropy"',
            "const bool temporal = temporalOwnerAllowed && temporalModeEnabled(",
            'getString("fix.temporal_aa", "off")', '"advanced.texture_lod_bias"')
    if '"advanced.texture_lod_bias", temporal ? "auto" : "0"' not in sampler_config:
        raise AssertionError("sampler policy must preserve the configured-mode default while treating malformed or absent TAA context as off")

    if "bool temporalAAOwnerAllowed = false;" not in body(vscreen, "struct State {"):
        raise AssertionError("vScreen Temporal-AA owner cache must default to denied")
    install = body(vscreen, "void installVScreenFixes(")
    cached_copy = "const bool temporalAAOwnerAllowed = edvr::temporalAAOwnerAllowed();"
    if install.count(cached_copy) != 1 or "ownerAllowedForStartup(plugins::kPluginTemporalAa)" in install:
        raise AssertionError("vScreen must copy the device-published Temporal-AA decision once")
    ordered(install, cached_copy, "Config& cfg = Config::get();",
            "g_state->temporalAAOwnerAllowed = temporalAAOwnerAllowed;")
    dormant_start = install.find("if (!wantVoid && scale == 1.0f &&")
    dormant_end = install.find("// Preserve the existing retry path", dormant_start)
    if dormant_start < 0 or dormant_end < 0:
        raise AssertionError("vScreen dormant transport-only decision scope is missing")
    dormant_condition = install[dormant_start:dormant_end]
    owner_snapshots = (
        "const bool diagnosticsAllowed = diagnosticsOwnerAllowed();",
        cached_copy,
        "const bool strictSelectedVScreenOwner = strictAdmissionNeedsVScreenHooks(\n"
        "        introAdmission, diagnosticsAllowed, temporalAAOwnerAllowed);",
    )
    if any(install.count(token) != 1 or install.find(token) > dormant_start
           for token in owner_snapshots):
        raise AssertionError("D and Temporal-AA owner decisions must be cached once before dormant startup selection")
    if "!introAdmission.strictSelection &&" not in dormant_condition or \
            "!strictSelectedVScreenOwner &&" not in dormant_condition:
        raise AssertionError("transport-only startup must preserve Intro semantics and qualify D/TAA by accepted strict selection")
    strict_helper = body(sources["intro_owner_gate"],
                         "inline bool strictAdmissionNeedsVScreenHooks(")
    ordered(strict_helper, "admission.strictModeSelection &&",
            "(diagnosticsOwnerAllowed || temporalAAOwnerAllowed)")
    refresh = body(vscreen, "void vScreenRefreshConfig()")
    if "temporalAAOwnerAllowed()" in refresh or "s->temporalAAOwnerAllowed" not in refresh:
        raise AssertionError("vScreen reload must reuse cached Temporal-AA admission without a second query")
    if "if (s->temporalAAOwnerAllowed) {\n        sharpenPassConfigure(cfg);\n        temporalPassConfigure(cfg);\n    }" not in refresh or \
            "if (g_state->temporalAAOwnerAllowed) {\n        sharpenPassConfigure(cfg);\n        temporalPassConfigure(cfg);\n    }" not in install:
        raise AssertionError("Temporal-AA configure/read paths must remain behind cached ownership")
    ui_layer_config = body(ui_layer, "void uiLayerConfigure(")
    if ui_layer_config.count("uiPanelScaleSetTarget(target);") != 1 or \
            vscreen.count("uiLayerConfigure(cfg);") != 2 or \
            "if (s->temporalAAOwnerAllowed) {\n        uiDepthConfigure(cfg);\n        uiLayerConfigure(cfg);\n    }" not in refresh or \
            "if (g_state->temporalAAOwnerAllowed) {\n        uiDepthConfigure(cfg);\n        uiLayerConfigure(cfg);\n    }" not in install:
        raise AssertionError("TAA-only UI panel scaling must stay on its owner-gated uiLayerConfigure path")

    temporal_acquire = body(native_temporal,
                            'extern "C" HRESULT WINAPI edvrAcquireNativeTemporal(')
    if "*t={sizeof(*t),EDVR_NATIVE_TEMPORAL_VERSION_1};" not in temporal_acquire:
        raise AssertionError("native Temporal-AA absence must return an exact empty versioned table")
    ordered(temporal_acquire, "*t={sizeof(*t),EDVR_NATIVE_TEMPORAL_VERSION_1};",
            "if(!edvr::temporalAAOwnerAllowed())return S_FALSE;",
            "std::lock_guard<std::mutex> lock(mutex)", "s.pendingSettings=readConfig();")
    sharpen_acquire = body(native_sharpen,
                           'extern "C" HRESULT WINAPI edvrAcquireNativeSharpen(')
    if "*t={sizeof(*t),EDVR_NATIVE_SHARPEN_VERSION_1};" not in sharpen_acquire:
        raise AssertionError("native sharpen absence must return an exact empty versioned table")
    ordered(sharpen_acquire, "*t={sizeof(*t),EDVR_NATIVE_SHARPEN_VERSION_1};",
            "if(!edvr::temporalAAOwnerAllowed())return S_FALSE;",
            "QueryInterface", "std::lock_guard<std::mutex> lock(mutex)")

    motion_config = body(screen_motion, "void screenMotionConfigure(")
    ordered(motion_config,
            "const bool on = temporalAAOwnerAllowed &&",
            'temporalModeEnabled(cfg.getString("fix.temporal_aa","off"))')
    if "const bool on = temporalAAOwnerAllowed &&" not in motion_config:
        raise AssertionError("ScreenMotion must not read temporal mode when its owner is absent")

    flat_present = body(flat_runtime, "void flatRuntimePresent(")
    ordered(flat_present,
            "const auto mode = temporalAAOwnerAllowed\n        ? Config::get().requestedTemporalMode()",
            "const bool enabled = temporalAAOwnerAllowed && temporalModeEnabled(mode);",
            "const auto model = temporalAAOwnerAllowed\n        ? Config::get().getString(\"fix.temporal_aa_model\", \"k\")",
            "if (!enabled) {")
    no_taa_branch = body(flat_present, "if (!enabled) {")
    if "if(!manual&&!s.drawPackets.active())return;" not in no_taa_branch or \
            not no_taa_branch.rstrip().endswith("return;") or \
            "advanced.temporal_aa_jitter_phases" in no_taa_branch or \
            "advanced.temporal_aa_jitter_phases" not in flat_present or \
            "s.drawPackets.present(s.drawPacketContext.Get(),frame+1);" not in no_taa_branch or \
            "armDrawPackets(s,frame);" not in no_taa_branch:
        raise AssertionError("flat AA-off/owner-absent flow must preserve capture work and return before Temporal-AA jitter config")
    if "engineVelocityConfigure(enabled && !enginePausedThen);" not in flat_present or \
            "if (temporalAAOwnerAllowed)" not in flat_present:
        raise AssertionError("flat engine-motion setup must remain disabled with Temporal-AA ownership")
    flat_substitution = body(flat_runtime, "void flatRuntimeSubstitution(")
    ordered(flat_substitution, "if (!temporalAAOwnerAllowed()) return;",
            "if (!engineVelocityFlatPending()) return;", "switch (flatSubstAction(event))")
    if "if (temporalAAOwnerAllowed()) engineVelocityFlatLazy(!diagnostics);" not in flat_runtime:
        raise AssertionError("flat draw must keep diagnostic/Core work while only dispatching EngineVelocity when TAA is admitted")
    flat_before_present = body(flat_runtime, "void flatRuntimeBeforePresent(")
    ordered(flat_before_present, "mapBounce().present(",
            "if (temporalAAOwnerAllowed && owner())", "engineVelocityFlatFrameEndWithCost();",
            "if (owner()) gpuFrameClose(state());")
    flat_weapon_footprint = body(flat_runtime,
                                 "void flatRuntimeWeaponFootprintBeforePresent(")
    if "s.weaponFootprint.beforePresent(" not in flat_weapon_footprint or \
            "temporalAAOwnerAllowed" in flat_weapon_footprint:
        raise AssertionError("flat On-foot weapon-footprint capture remains independent of Temporal-AA ownership")
    route_mode = body(world_route, "FlatMonoResolveMode resolveModeFromConfig(")
    ordered(route_mode, "if (!g_temporalAAOwnerAllowed)",
            'Config::get().getString("fix.temporal_aa", "off")')

    # EngineVelocity is one Temporal-AA leg spread across device, context and
    # flat hooks. Preserve those host paths while requiring each optional tee
    # to consult the immutable cached owner before entering the module.
    velocity_draw_sites = re.findall(
        r"(?m)^\s*engineVelocityBeforeDraw(?:SampledBoundary|WithApi(?:<[^>\n]+>)?)\(",
        vscreen)
    velocity_draw_guards = re.findall(
        r"if\s*\(g_state->temporalAAOwnerAllowed\)\s*(?:\{\s*)?engineVelocityBeforeDraw|"
        r"if\s*\(self == g_state->ownerCtx && g_state->temporalAAOwnerAllowed\)\s*"
        r"(?:\{\s*)?engineVelocityBeforeDraw",
        vscreen)
    if len(velocity_draw_sites) != 7 or len(velocity_draw_guards) != len(velocity_draw_sites):
        raise AssertionError("every vScreen engine-velocity draw callback must stay behind cached Temporal-AA ownership")
    motion_resource_written = body(vscreen, "static void motionResourceWritten(")
    owner_active = body(vscreen, "bool temporalAAOwnerActive() noexcept {")
    if "return g_state && g_state->temporalAAOwnerAllowed;" not in owner_active:
        raise AssertionError("vScreen Temporal-AA accessor must expose only its cached owner decision")
    if ("if (temporalAAOwnerActive())\n        engineVelocityResourceWritten(resource);" not in motion_resource_written or
            motion_resource_written.count("engineVelocityResourceWritten(resource);") != 1):
        raise AssertionError("shared resource writes must consult the cached Temporal-AA accessor before EngineVelocity invalidation")
    for gate in (
            "if (mapData0 && type != D3D11_MAP_READ && g_state->temporalAAOwnerAllowed)\n"
            "        engineVelocityResourceMapped(res, mapped->pData, static_cast<int>(type));",
            "if (g_state->temporalAAOwnerAllowed)\n            tkEngineVelocity.run(",
            "if (g_state->temporalAAOwnerAllowed && celestialMotionAnyWatched())\n"
            "        celestialMotionConstantsUnmapped(res);",
            "if (g_state->temporalAAOwnerAllowed && celestialMotionAnyWatched())\n"
            "            celestialMotionConstantsWritten(dst, data, box);",
            "if (g_state->temporalAAOwnerAllowed)\n            tkCelestialMotion.run(",
            "if (g_state && g_state->temporalAAOwnerAllowed)\n        celestialMotionShutdown();"):
        if gate not in vscreen:
            raise AssertionError(f"Temporal-AA cached owner must gate this shared vScreen callback: {gate}")
    if "if (g_state->temporalAAOwnerAllowed)\n            engineVelocityRememberVs(" not in device or \
            "if (g_state->temporalAAOwnerAllowed)\n            engineVelocityRememberPs(" not in device or \
            "if(g_state->temporalAAOwnerAllowed && out && *out)\n                engineVelocityBufferCreated(" not in device or \
            "if (g_state->temporalAAOwnerAllowed)\n        tkEngineVelocityClock.run(" not in device:
        raise AssertionError("device shader/buffer capture and the EngineVelocity present clock must be owner-gated")
    texture_create = body(device, "HRESULT STDMETHODCALLTYPE hookedCreateTexture2D(")
    ordered(texture_create,
            "if (!fromEdvr && g_state->temporalAAOwnerAllowed && desc && self == g_state->device &&",
            "uiSurfacesPanelNet(*desc, init != nullptr, &netDesc)",
            "const HRESULT hr = createTexture2DForwarded(self, desc, init, out);",
            "if (!fromEdvr && g_state->temporalAAOwnerAllowed && out && *out) {",
            "uiSurfacesWantsAtlas(*desc)", "uiSurfacesWantsChain(*desc, init != nullptr)")
    if "uiAtlasNoteWrite(res, 1);" not in vscreen or \
            ("if (g_state->temporalAAOwnerAllowed && type != D3D11_MAP_READ)\n"
             "        uiAtlasNoteWrite(res, 1);") not in vscreen or \
            vscreen.count("if (g_state->temporalAAOwnerAllowed) uiAtlasNoteWrite(dst, 2);") != 2 or \
            "if (g_state->temporalAAOwnerAllowed) uiAtlasNoteWrite(dst, 0);" not in vscreen:
        raise AssertionError("UI atlas Map/copy/update observers must be gated without suppressing the shared forwarding hooks")
    if ("if (temporalAAOwnerAllowed() && engineVelocitySkinWanted() && skinChainBound())\n"
            "        engineVelocityNoteChainDispatch(self, x);") not in exposure_fix:
        raise AssertionError("Comfort's chain dispatch may observe EngineVelocity skin only under cached TAA ownership")
    for token in (
            "if (g_state->temporalAAOwnerAllowed && celestialMotionAnyWatched())",
            "if (g_state->temporalAAOwnerAllowed && mapData0 && type != D3D11_MAP_READ && celestialMotionAnyWatched()) {",
            "celestialMotionConstantsMapped(res, mapped->pData);",
            "celestialMotionConstantsUnknownWrite(dst);",
            "if (g_state->temporalAAOwnerAllowed && owner && celestialMotionLive()",
            "celestialMotionNoteDraw(self);",
            "if (g_state->temporalAAOwnerAllowed)\n            tkCelestialMotion.run(",
            "if (g_state && g_state->temporalAAOwnerAllowed)\n        celestialMotionShutdown();"):
        if token not in vscreen:
            raise AssertionError(f"planet-patch data tee lost its cached Temporal-AA owner boundary: {token}")

    for token in ("if(g_state->temporalAAOwnerAllowed && out && *out && celestialMotionAnyWatched())",
                  "celestialMotionConstantsUnknownWrite(static_cast<ID3D11Buffer*>(*out));",
                  "if (g_state->temporalAAOwnerAllowed)\n        celestialMotionNoteStatus("):
        if token not in device:
            raise AssertionError(f"device-side planet-patch observer lost its Temporal-AA owner boundary: {token}")


def check_onfoot_boundary(sources: dict[str, str]) -> None:
    device = sources["device"]
    vscreen = sources["vscreen"]
    gate = sources["onfoot_owner_gate"]
    screen_motion = sources["screen_motion"]
    native_frame = sources["native_frame"]
    temporal_pass = sources["temporal_pass"]
    world_route = sources["vr_world_route"]
    menu = sources["menu"]

    if "bool onFootPanelOwnerAllowed = false;" not in device or \
            "bool onFootPanelOwnerAllowed = false;" not in vscreen:
        raise AssertionError("On-foot ownership caches must default denied in device and VScreen State")
    if device.count("plugins::kPluginOnFootPanel") != 1 or \
            device.count("ownerAllowedForStartup(\n            plugins::kPluginOnFootPanel)") != 1:
        raise AssertionError("device startup must sample the published On-foot decision exactly once")
    device_query = device.index("plugins::kPluginOnFootPanel")
    if not (device.index("s.device = device;") < device_query <
            device.index('runOnFootPanelOwned(s.onFootPanelOwnerAllowed, [&] {\n            Config& cfg = Config::get();')):
        raise AssertionError("On-foot admission must be post-commit and precede its first owned configuration")
    gate_start = device.index("runOnFootPanelOwned(s.onFootPanelOwnerAllowed, [&] {")
    gate_end = device.index("        });", gate_start)
    resolution_gate = device[gate_start:gate_end]
    ordered(resolution_gate, "Config& cfg = Config::get();",
            "resolveVScreenTargetResolution(cfg, &w, &h, s.temporalAAOwnerAllowed);",
            "applied = (w && h) && applyVScreenModeResolution(w, h);")
    if "vScreenSetPanelSize(applied ? w : kStockW, applied ? h : kStockH);" not in device:
        raise AssertionError("stock panel dimensions must remain published when optional On-foot ownership is absent")
    if "if (onFootPanelOwnerAllowed()) revertVScreenModeResolution();" not in device:
        raise AssertionError("device shutdown must revert the On-foot mode only for an admitted owner")

    install = body(vscreen, "void installVScreenFixes(")
    if install.count("edvr::onFootPanelOwnerAllowed()") != 1 or \
            "ownerAllowedForStartup(plugins::kPluginOnFootPanel)" in install:
        raise AssertionError("VScreen must copy the device-published On-foot decision without a second transport query")
    ordered(install, "const bool onFootPanelOwnerAllowed = edvr::onFootPanelOwnerAllowed();",
            "runOnFootPanelOwned(onFootPanelOwnerAllowed, [&] {",
            'cfg.getBool("fix.black_void", true)', 'cfg.getFloat("fix.panel_distance", 1.0f)',
            "(!onFootPanelOwnerAllowed ||\n         !cfg.getBool(\"advanced.panel_hooks_always\", true))",
            "g_state->onFootPanelOwnerAllowed = onFootPanelOwnerAllowed;")
    if "if (g_state->onFootPanelOwnerAllowed)\n        g_state->distanceIndex = readDistanceIndex(cfg);" not in install:
        raise AssertionError("On-foot distance index adoption must be deferred until cached admission")
    refresh = body(vscreen, "void vScreenRefreshConfig()")
    if "runOnFootPanelOwned(s->onFootPanelOwnerAllowed, [&] {" not in refresh or \
            's->distanceIndex = readDistanceIndex(cfg);' not in refresh:
        raise AssertionError("On-foot live reload must keep its entire settings block behind cached ownership")
    if "if (s->onFootPanelOwnerAllowed || s->introOwnerAllowed)\n        panelCurveConfigure(cfg);" not in vscreen or \
            "if (g_state->onFootPanelOwnerAllowed || g_state->introOwnerAllowed)\n        panelCurveShutdown();" not in vscreen:
        raise AssertionError("panel-curve shared runtime must remain available to either On-foot or Intro")

    for row in ('{"legacy.panel-distance", &drawGate_panel_distance, true, LegacyGateOwner::OnFootPanel}',
                '{"legacy.panel-curve", &drawGate_panel_curve, true, LegacyGateOwner::OnFootPanel}',
                '{"legacy.vscreen-footprint", &drawGate_vscreen_footprint, true, LegacyGateOwner::OnFootPanel}'):
        if row not in vscreen:
            raise AssertionError(f"On-foot legacy row lost its owner metadata: {row}")
    registry = body(vscreen, "bool registerLegacyDrawGates(")
    ordered(registry, "if (!legacyDrawGateOwnerAllowed(registration, state)) continue;",
            "pluginRegistryRegisterLegacyDrawGate(registration.name,")
    if "case LegacyGateOwner::OnFootPanel:\n        return state && state->onFootPanelOwnerAllowed;" not in vscreen:
        raise AssertionError("On-foot draw-gate callback must consume its borrowed cached State")
    if "runVScreenFootprintOwned(g_state, [&] {" not in vscreen or \
            "if (g_state->onFootPanelOwnerAllowed) vscreenFootprintShutdown();" not in vscreen:
        raise AssertionError("On-foot footprint tick and teardown must use cached ownership")
    frame_boundary = body(vscreen, "void vScreenFrameBoundary()")
    if "runVScreenFootprintOwned(g_state, [&] {\n            tkVScreenFootprint.run([&] {" not in frame_boundary:
        raise AssertionError("footprint frame collection must stay inside the owner wrapper")
    for token in ("if (!s->onFootPanelOwnerAllowed)\n                return exited(id, DrawVerdict::kNone);",
                  "if (s->onFootPanelOwnerAllowed && panelCurveWants() &&",
                  "if (g_state->onFootPanelOwnerAllowed && self == g_state->ownerCtx"):
        if token not in vscreen:
            raise AssertionError(f"On-foot panel/curve visitor or direct draw lost its owner gate: {token}")

    if "if (onFootPanelOwnerAllowed) {\n        const bool weapon=cfg.getBool(\"fix.weapon_stability\",true);" not in screen_motion:
        raise AssertionError("weapon config must be lazy when On-foot ownership is absent")
    if "if (g.onFootPanelOwnerAllowed)\n        weaponMotionFrameBoundary(ctx);\n    if (!g.temporalAAOwnerAllowed) return;\n    g_gpu.tick(ctx,g.frame);\n    flushPanelCounts(ctx);" not in screen_motion:
        raise AssertionError("ScreenMotion must gate weapon work independently and keep core temporal-AA work owner-gated")
    if "g_gpu.reset(nullptr);g=State{};" not in screen_motion or \
            "if(weaponConfigured)weaponMotionShutdown();" not in screen_motion:
        raise AssertionError("ScreenMotion shutdown must preserve core cleanup and gate weapon teardown")

    if "state.onFootPanelOwnerAllowed = edvr::onFootPanelOwnerAllowed();" not in native_frame:
        raise AssertionError("native-frame provider must cache owner admission at Acquire")
    native_begin = body(native_frame, "HRESULT WINAPI beginFrame(")
    if "if (state->onFootPanelOwnerAllowed) {" not in native_begin or \
            '"fix.weapon_stability", true' not in native_begin:
        raise AssertionError("native-frame weapon pacing/config must use the Acquire cache")
    if "sceneReady" not in native_begin or "resubmitEnabled" not in native_begin or \
            "transitionEnabled" not in native_begin:
        raise AssertionError("core and Comfort native-frame outputs must remain independent of On-foot gating")
    if "auto* weapon=onFootPanelOwnerAllowed() ? weaponMotionView() : nullptr;" not in temporal_pass or \
            "ID3D11ShaderResourceView* map = onFootPanelOwnerAllowed()" not in world_route:
        raise AssertionError("temporal-AA and VR world routes must include weapon data only for admitted On-foot")
    if "if (onFootPanelOwnerAllowed())\n                    resolveVScreenTargetResolution(Config::get(), &vw, &vh, temporalAAOwnerAllowed()," not in menu:
        raise AssertionError("menu resolution hints must not read On-foot config when absent")

    fixture = sources["vscreen_predicate_fixture"]
    for token in ("testOnFootOwnerDeniedPanelCurve(device, immediate);",
                  "testOnFootOwnerDeniedFootprint(device, immediate);",
                  "testDiagnosticsOwnerLegacyGateRegistration();"):
        if token not in fixture:
            raise AssertionError(f"real WARP On-foot denial/admission fixture is not invoked: {token}")
    distance_fixture = body(fixture, "bool testPanelDistanceApiTransaction(")
    if "testPanelDistanceApiTransaction(immediate, deferred);" not in fixture or \
            "onFootDenied.onFootOwnerAllowed = false;" not in distance_fixture:
        raise AssertionError("real PanelDistance API transaction must run and include the denied-owner control")
    for token in ("outcome ==\n                          edvr::draw_ladder::SiteOutcome::Declined", "result.mapCalls == 0", "result.unmapCalls == 0",
                  "constantBufferCalls == 0", "afterDenied.entries == before.entries",
                  "g_resourceAttempts == 0", "afterAllowed.entries == before.entries + 1",
                  "copyPendingAfterDraw"):
        if token not in fixture:
            raise AssertionError(f"On-foot WARP fixture lost a denied-path/positive-control assertion: {token}")
    native_fixture = sources["native_frame_fixture"]
    if "nativeFrameOnFootOwnerDenied();" not in native_fixture or \
            "deniedOnFootOutput.deferredPacing == 0" not in native_fixture:
        raise AssertionError("real native-frame On-foot denial fixture is missing")
    motion_fixture = sources["screen_motion_fixture"]
    for token in ("On-foot admission remains independent: it reads weapon stability and dispatches its real frame path while TAA config stays absent",
                  "both admitted owners preserve the real core and weapon dispatch with live config reads"):
        if token not in motion_fixture:
            raise AssertionError(f"ScreenMotion On-foot absence control is missing: {token}")


def expect_slice(label: str, sources: dict[str, str]) -> None:
    global MUTATION_COUNT
    MUTATION_COUNT += 1
    try:
        check_module_slices(sources)
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"mutation not detected: {label}")


def self_test_slices(sources: dict[str, str]) -> None:
    changed = dict(sources)
    build = changed["build"]
    native_start = build.find(":rig_openxr_native_tests")
    native_end = build.find("\n:rig_", native_start + 1)
    native_rig = build[native_start:native_end if native_end >= 0 else None]
    changed["build"] = (build[:native_start] + once(
        native_rig, '"src\\plugins\\performance\\fov_trim_policy.cpp" ^',
        '"src\\plugins\\performance\\missing_fov_trim_policy.cpp" ^') +
        (build[native_end:] if native_end >= 0 else ""))
    expect_slice("OpenXR native host fixture links the Performance FOV factory", changed)

    changed = dict(sources)
    changed["fov_settings"] = once(changed["fov_settings"],
        'getString("experimental.fov_trim_vertical", "")',
        'getString("experimental.fov_trim_wrong", "")')
    expect_slice("Performance owns the literal FOV Config keys", changed)

    changed = dict(sources)
    changed["native_host"] = once(changed["native_host"],
        "if(!acquireStartupTreatmentProviders(options.graphicsProvider,externalDevice,runtimeGeneration))return false;",
        "/* startup acquisition seam bypassed */")
    expect_slice("native startup cannot bypass the behaviorally tested treatment acquisition seam", changed)

    changed = dict(sources)
    changed["native_host"] = once(changed["native_host"],
        "if(!acquireStartupTreatmentProviders(options.graphicsProvider,externalDevice,runtimeGeneration))return false;",
        "if(!acquireStartupTreatmentProviders(options.graphicsProvider,externalDevice,runtimeGeneration))return false;\n"
        "if(!acquireStartupTreatmentProviders(options.graphicsProvider,externalDevice,runtimeGeneration))return false;")
    expect_slice("native startup cannot acquire the treatment providers twice", changed)

    changed = dict(sources)
    changed["native_treatment_runner"] = once(changed["native_treatment_runner"],
        "edvr::openxr::test::runTemporalStartupCases(check);",
        "/* production startup fixture omitted */")
    expect_slice("the native startup treatment acquisition fixture remains invoked", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "if (!g_trimPolicyPresent)", "if (false)")
    expect_slice("absent FOV policy bypasses module and label work", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "g_trimPolicyState = g_trimPolicyOps.create();",
        "g_trimPolicyState = nullptr;")
    expect_slice("FOV policy state is created lazily on the selected path", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "if (g_trimPolicyState) g_trimPolicyOps.destroy(g_trimPolicyState);",
        "/* old FOV policy state not destroyed */")
    expect_slice("FOV policy state is destroyed when replaced or disabled", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "g_trimPolicyOps = ops ? *ops : FovTrimSettingsPolicyOpsV1{};",
        "/* retain borrowed FOV policy ops pointer */")
    expect_slice("native-frame host copies the immutable FOV policy table", changed)

    changed = dict(sources)
    changed["fov_settings"] = once(changed["fov_settings"],
        "input->structSize != sizeof(*input)", "false")
    expect_slice("malformed settings input is rejected before Config reads", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "if (FAILED(trimStatus)) return trimStatus;", "/* continue after failed trim resolve */")
    expect_slice("failed trim resolution leaves caller output unchanged", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "if (g_trimPolicyState) {",
        "std::lock_guard<std::mutex> lock(g_mutex);\n        if (g_trimPolicyState) {")
    expect_slice("CRT teardown does not wait on the native-frame mutex", changed)

    changed = dict(sources)
    changed["native_fov_startup"] = once(changed["native_fov_startup"],
        "if (hasAdmissionReservation && !ownerAllowed(decision, generatedGroupIndex))",
        "if (false)")
    expect_slice("denied native reservation skips the Performance FOV factory", changed)

    changed = dict(sources)
    changed["native_module"] = once(changed["native_module"],
        "generation->binding.device(),fovTrimOps",
        "generation->binding.device()")
    expect_slice("native host receives the explicitly selected FOV ops", changed)

    changed = dict(sources)
    changed["native_frame_fov_adapter"] = once(changed["native_frame_fov_adapter"],
        "return setNativeFrameFovTrimSettingsPolicyOpsV1(nullptr);", "return true;")
    expect_slice("denied graphics reservation installs absent FOV settings", changed)

    changed = dict(sources)
    changed["native_frame_fov_adapter"] = once(changed["native_frame_fov_adapter"],
        "if (ownerAllowed) return true;",
        "if (ownerAllowed) return setNativeFrameFovTrimSettingsPolicyOpsV1(\n        edvr::plugins::performance::fovTrimSettingsPolicyOpsV1());")
    expect_slice("allowed graphics startup keeps the settings policy lazy", changed)

    changed = dict(sources)
    changed["proxy"] = once(changed["proxy"],
        "            performanceOwnerAllowed))", "            true))")
    expect_slice("resolution startup gate receives admitted Performance ownership", changed)

    changed = dict(sources)
    changed["fov_facade"] = once(changed["fov_facade"],
        "out.recommended = gameDims;", "out.recommended = dims[eye];")
    expect_slice("absent FOV fallback preserves game-facing size rounding", changed)

    changed = dict(sources)
    changed["registry"] = replace_body(changed["registry"],
        "bool pluginRegistryRegisterLifecycle(",
        'std::strcmp(record.implementationStatus, "phase1-pilot") != 0',
        'std::strcmp(record.implementationStatus, "disabled") != 0')
    expect_slice("Cockpit pilot lifecycle admission", changed)

    changed = dict(sources)
    changed["native_settings"] = replace_body(changed["native_settings"],
        "extern \"C\" BOOL WINAPI edvrQueryNativeRenderSettings(",
        "if (g_policyInstalled)", "if (false)")
    expect_slice("native policy callback installation gate", changed)

    changed = dict(sources)
    changed["native_settings"] = once(changed["native_settings"],
        "answer.openxrRenderScale = EDVR_NATIVE_RENDER_SCALE_DEFAULT;", "/* absent policy default removed */")
    expect_slice("native policy absent defaults", changed)

    changed = dict(sources)
    changed["proxy"] = replace_body(changed["proxy"], "void shutdown()",
        "unregisterPolicy(", "/* unregister omitted */")
    expect_slice("policy unregistration before device shutdown", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"], "void frameRemlokLifecycle(uint32_t sceneFrame)",
        "pluginRegistryFrameLifecycle(", "/* registry frame omitted */")
    expect_slice("RemLok frame dispatch and fallback", changed)

    changed = dict(sources)
    changed["device"] = replace_body(changed["device"], "tkSchedulerProbe.run([] {",
        "pluginRegistryFrameLifecycle(", "/* registry frame omitted */")
    expect_slice("Diagnostics Present frame dispatch", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "bool diagnosticsOwnerAllowed = false;",
        "bool diagnosticsOwnerAllowed = true;")
    expect_slice("Diagnostics owner cache defaults to denied", changed)

    changed = dict(sources)
    changed["diagnostics_owner_gate"] = once(changed["diagnostics_owner_gate"],
        "if (!ownerAllowed) return false;", "if (ownerAllowed) return false;")
    expect_slice("Diagnostics shared callback wrapper returns before absent-owner work", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (!s.diagnosticsOwnerAllowed) return;",
        "/* Diagnostics hotkey read gate omitted */")
    expect_slice("Diagnostics dump hotkeys are not read without owner admission", changed)

    changed = dict(sources)
    changed["device"] = replace_body(changed["device"],
        "bool captureFlatProbeShader(char stage, uint64_t hash)",
        "if (!diagnosticsOwnerAllowed() || !g_state || !runtimeFlatProfile()",
        "if (!g_state || !runtimeFlatProfile()")
    expect_slice("flat diagnostic shader capture remains owner-gated", changed)

    changed = dict(sources)
    changed["device"] = replace_body(changed["device"],
        "bool flatProbeShaderLookup(char stage, uint64_t hash,",
        "if (!g_state || !hash || !data || !bytes ||",
        "if (!diagnosticsOwnerAllowed() || !g_state || !hash || !data || !bytes ||")
    expect_slice("Core flat renderer lookup stays live without Diagnostics", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void configureSchedulerLifecycle(Config& cfg)",
        "if (!g_state || !g_state->diagnosticsOwnerAllowed) return;",
        "/* Diagnostics lifecycle/fallback owner gate omitted */")
    expect_slice("Diagnostics lifecycle and direct fallback require owner admission", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (diagnosticsOwnerAllowed()) tkSchedulerProbe.run([] {",
        "tkSchedulerProbe.run([] {")
    expect_slice("Diagnostics scheduler frame fallback is owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void vScreenRefreshConfig()",
        "depthProbeConfigure(cfg, s->diagnosticsOwnerAllowed,",
        "depthProbeConfigure(cfg, diagnosticsOwnerAllowed(),")
    expect_slice("vScreen reload reuses the cached Diagnostics admission", changed)

    for gate, name in (
            ("EDVR_GATE_DIAGNOSTICS(census_skip_count, s->censusSkipCount != 0)", "census_skip_count"),
            ("EDVR_GATE_DIAGNOSTICS(census_skip_ranges, s->censusSkipRangeCount != 0)", "census_skip_ranges"),
            ("EDVR_GATE_DIAGNOSTICS(census_skip_off, s->censusSkipOffCount != 0)", "census_skip_off"),
            ("EDVR_GATE_DIAGNOSTICS(census_quad_skip_armed, s->quadSkipArmed)", "census_quad_skip_armed"),
            ("EDVR_GATE_DIAGNOSTICS(census_auto, s->censusAutoW != 0)", "census_auto")):
        changed = dict(sources)
        changed["vscreen"] = once(changed["vscreen"], gate, gate.replace(
            "EDVR_GATE_DIAGNOSTICS", "EDVR_GATE_STATE", 1))
        expect_slice(f"{name} legacy census row requires Diagnostics owner", changed)

    changed = dict(sources)
    changed["relay_host"] = replace_body(changed["relay_host"],
        "uintptr_t __fastcall bracket(",
        "schedulerObserver.load(std::memory_order_acquire)",
        "schedulerObserver.load(std::memory_order_relaxed)")
    expect_slice("single acquire observer snapshot", changed)

    changed = dict(sources)
    changed["relay_host"] = once(changed["relay_host"],
        "schedulerObserver.load(std::memory_order_acquire)!=nullptr", "false")
    expect_slice("relay gate ownership recomputation", changed)

    changed = dict(sources)
    changed["relay_host"] = replace_body(changed["relay_host"],
        'extern "C" const char* edvrKinematicEvalSchedulerObserverAttach(',
        'if(active)return active==ops?"installed":"observer_busy";',
        'if(active)return "observer_busy";')
    expect_slice("relay exact-record idempotence and busy replacement", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "s.explorerCamOwnerAllowed =\n        plugin_admission_transport::ownerAllowedForStartup(\n            plugins::kPluginExplorerCam);",
        "s.explorerCamOwnerAllowed = true;")
    expect_slice("Explorer startup snapshots transport ownership", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runExplorerCamOwned(g_state->explorerCamOwnerAllowed, [] {",
        "runExplorerCamOwned(true, [] {")
    expect_slice("Explorer Present uses the cached owner decision", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runExplorerCamOwned(g_state && g_state->explorerCamOwnerAllowed, [] {",
        "runExplorerCamOwned(true, [] {")
    expect_slice("Explorer shutdown uses the cached owner decision", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runExplorerCamOwned(s.explorerCamOwnerAllowed, [] {",
        "pluginRegistryRegisterLifecycle(\n            &plugins::explorer_cam::explorerCamLifecycleOps());")
    expect_slice("denied Explorer startup skips optional lifecycle factory", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runExplorerCamOwned(s.explorerCamOwnerAllowed, [] {\n"
        "        pluginRegistryRegisterLifecycle(\n"
        "            &plugins::explorer_cam::explorerCamLifecycleOps());\n"
        "    });",
        "runExplorerCamOwned(s.explorerCamOwnerAllowed, [] {});\n"
        "    pluginRegistryRegisterLifecycle(\n"
        "        &plugins::explorer_cam::explorerCamLifecycleOps());")
    expect_slice("Explorer startup registration moved outside owner gate", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runExplorerCamOwned(g_state && g_state->explorerCamOwnerAllowed, [] {\n"
        "        if (pluginRegistryHasLifecycle(plugins::kPluginExplorerCam)) {\n"
        "            pluginRegistryShutdownLifecycle(plugins::kPluginExplorerCam);\n"
        "        } else {\n"
        "            explorerCamShutdown();\n"
        "        }\n"
        "    });",
        "runExplorerCamOwned(g_state && g_state->explorerCamOwnerAllowed, [] {});\n"
        "    if (pluginRegistryHasLifecycle(plugins::kPluginExplorerCam)) {\n"
        "        pluginRegistryShutdownLifecycle(plugins::kPluginExplorerCam);\n"
        "    } else {\n"
        "        explorerCamShutdown();\n"
        "    }")
    expect_slice("Explorer shutdown dispatch moved outside owner gate", changed)

    changed = dict(sources)
    changed["explorer_owner_gate"] = once(changed["explorer_owner_gate"],
        "if (!ownerAllowed) return false;", "if (false) return false;")
    expect_slice("denied Explorer helper does not invoke its callback", changed)

    changed = dict(sources)
    changed["explorer_fixture"] = once(changed["explorer_fixture"],
        "testOwnerAbsence(p);", "/* owner absence fixture omitted */")
    expect_slice("Explorer owner absence fixture coverage", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "bool cockpitVisualsOwnerAllowed = false;",
        "bool cockpitVisualsOwnerAllowed = true;")
    expect_slice("Cockpit owner cache defaults to denied", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "plugin_admission_transport::ownerAllowedForStartup(\n"
        "            plugins::kPluginCockpitVisuals);",
        "plugin_admission_transport::ownerAllowedForStartup(\n"
        "            plugins::kPluginComfort);")
    expect_slice("Cockpit startup samples its own published owner", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runCockpitVisualsOwned(cockpitVisualsOwnerAllowed, [&] {\n"
        "        if (pluginRegistryProfileSupports(plugins::kPluginCockpitVisuals)) {\n"
        "            cockpitVisualsRegistered = pluginRegistryRegister(cockpitVisualsPluginOps());\n"
        "            if (!cockpitVisualsRegistered) {\n"
        "                Log::get().note(\"plugin registry: cockpit-visuals registration failed; module dispatch is unavailable\");\n"
        "                pluginRegistrySetDrawGateFailOpen();\n"
        "            }\n"
        "        } else {\n"
        "            Log::get().note(\"plugin registry: cockpit-visuals skipped for installation profile %s\",\n"
        "                            runtimeProfileName());\n"
        "        }\n"
        "    });",
        "runCockpitVisualsOwned(cockpitVisualsOwnerAllowed, [&] {});\n"
        "    cockpitVisualsRegistered = pluginRegistryRegister(cockpitVisualsPluginOps());")
    expect_slice("Cockpit startup registration remains inside owner gate", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void installVScreenFixes(",
        "runCockpitVisualsOwned(g_state->cockpitVisualsOwnerAllowed, [&] {\n"
        "        targetSharpConfigure(cfg);\n"
        "    });",
        "targetSharpConfigure(cfg);")
    expect_slice("Cockpit startup configuration is owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void vScreenRefreshConfig()",
        "runCockpitVisualsOwned(s->cockpitVisualsOwnerAllowed, [&] {\n"
        "        targetSharpConfigure(cfg);\n"
        "    });",
        "targetSharpConfigure(cfg);")
    expect_slice("Cockpit reload configuration is owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "struct VScreenDrawLadderVisitor",
        "} else if constexpr (id == SiteId::kTargetSharpClaim) {\n"
        "            if (!s->cockpitVisualsOwnerAllowed) return SiteResult::declined();",
        "} else if constexpr (id == SiteId::kTargetSharpClaim) {")
    expect_slice("Cockpit draw branch observes cached ownership", changed)

    changed = dict(sources)
    changed["cockpit_owner_gate"] = once(changed["cockpit_owner_gate"],
        "if (!ownerAllowed) return false;", "if (false) return false;")
    expect_slice("denied Cockpit helper does not invoke its callback", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "struct VScreenDrawLadderVisitor",
        "sunglareTrackExposureGlareTrain(kind, count, instances);",
        "sunglareOnEyeDraw(kind, count, instances);")
    expect_slice("denied Cockpit glare does only shared Comfort observation", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        ": exposureDampingActive())",
        ": sunglareWantsDraws())")
    expect_slice("denied sun-glare subscription avoids Cockpit draw state", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "(cockpitVisualsOwnerAllowed ||\n"
        "         !(comfortOwnerAllowed && exposureDampingActive()))",
        "(cockpitVisualsOwnerAllowed || true)")
    expect_slice("dormant hook preserves only active Comfort glare observation", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (s->cockpitVisualsOwnerAllowed && mapSub0 && res == billboardTarget())",
        "if (mapSub0 && res == billboardTarget())")
    expect_slice("Cockpit Map capture is owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void STDMETHODCALLTYPE hookedUnmap(",
        "runCockpitVisualsOwned(s->cockpitVisualsOwnerAllowed, [&] {\n"
        "            guardedBudget(g_sunglareRowsBudget, [&] {\n"
        "                // And the live feed:",
        "runCockpitVisualsOwned(true, [&] {\n"
        "            guardedBudget(g_sunglareRowsBudget, [&] {\n"
        "                // And the live feed:")
    expect_slice("Cockpit Unmap capture uses cached ownership", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void vScreenFrameBoundary()",
        "runCockpitVisualsOwned(g_state->cockpitVisualsOwnerAllowed, [&] {\n"
        "            tkWakePulse.run(",
        "tkWakePulse.run(")
    expect_slice("Cockpit Present tick is owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void shutdownVScreenFixes()",
        "runCockpitVisualsOwned(g_state->cockpitVisualsOwnerAllowed, [&] {\n"
        "        targetSharpShutdown();\n"
        "    });",
        "targetSharpShutdown();")
    expect_slice("Cockpit direct shutdown is owner-gated", changed)

    changed = dict(sources)
    changed["cockpit_api_fixture"] = once(changed["cockpit_api_fixture"],
        "ok &= cockpitOwnerBoundaryChecks(immediate, stock, cfg);",
        "/* Cockpit owner absence fixture omitted */")
    expect_slice("Cockpit real owner absence fixture stays invoked", changed)

    changed = dict(sources)
    changed["sunglare"] = replace_body(changed["sunglare"],
        "void sunglareTrackExposureGlareTrain(",
        "bool damping = false;", "const auto leakedMode = g_mode;\n  bool damping = false;")
    expect_slice("denied glare observer cannot read Cockpit mode", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (!s.deviceHook.commit()) {",
        "s.comfortOwnerAllowed =\n"
        "        plugin_admission_transport::ownerAllowedForStartup(\n"
        "            plugins::kPluginComfort);\n"
        "    if (!s.deviceHook.commit()) {")
    expect_slice("Comfort admission is sampled only after successful hook commit", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "plugins::kPluginComfort);",
        "plugins::kPluginExplorerCam);")
    expect_slice("device startup snapshots the Comfort owner, not another manifest", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runComfortOwned(s.comfortOwnerAllowed, [&] {\n        fssResConfigure(sentinelCfg);\n    });",
        "fssResConfigure(sentinelCfg);")
    expect_slice("pre-VScreen FSS setup remains behind the owner gate", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (initializeComfortStartup(s))\n            journalWatchSetEagerStatus",
        "if (false)\n            journalWatchSetEagerStatus")
    expect_slice("one-shot initial FSS bindings and latch remain in the published owner gate", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (!state.comfortOwnerAllowed || state.comfortStartupInitialized)",
        "if (!state.comfortOwnerAllowed)")
    expect_slice("initial Comfort FSS setup is one-shot per State", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runComfortOwned(s.comfortOwnerAllowed, [&] {\n                installExposureFix(device, ctxMode);\n            });",
        "installExposureFix(device, ctxMode);")
    expect_slice("Comfort exposure hooks remain gated at device startup", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "terrainCullCoverInstall(s.comfortOwnerAllowed,\n                                    runtimeFlatProfile());",
        "terrainCullCoverInstall(true, runtimeFlatProfile());")
    expect_slice("terrain install receives the cached owner decision", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "g_state->comfortOwnerAllowed = comfortOwnerAllowed;",
        "g_state->comfortOwnerAllowed = true;")
    expect_slice("VScreen keeps the device-published Comfort cache", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (!legacyDrawGateOwnerAllowed(registration, state)) continue;",
        "if (false) continue;")
    expect_slice("denied-owner draw subscriptions are filtered before registry dispatch", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "{\"legacy.fss-panel\", &drawGate_fss_panel, true, LegacyGateOwner::Comfort}",
        "{\"legacy.fss-panel\", &drawGate_fss_panel, false, LegacyGateOwner::Comfort}")
    expect_slice("FSS gate registration carries cached State into its callback", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (!s || !s->comfortOwnerAllowed) return 0;",
        "if (!s) return 0;")
    expect_slice("denied Comfort draw bridge avoids module predicate work", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runComfortOwned(s->comfortOwnerAllowed, [&] { fssResConfigure(cfg); });",
        "fssResConfigure(cfg);")
    expect_slice("Comfort reload configuration remains owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = replace_body(changed["vscreen"],
        "void frameScannerLifecycleStage(",
        "if (!g_state || !g_state->comfortOwnerAllowed) return;",
        "if (!g_state) return;")
    expect_slice("scanner lifecycle fallback does not run for absent Comfort", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (comfortOwnerAllowed()) {\n        shutdownGlitchFrameFix();",
        "if (true) {\n        shutdownGlitchFrameFix();")
    expect_slice("device transition shutdown is skipped without Comfort ownership", changed)

    changed = dict(sources)
    changed["native_fss"] = once(changed["native_fss"],
        "if(!ownerAllowed)return S_FALSE;", "if(false)return S_FALSE;")
    expect_slice("denied native FSS acquire exits before pool and healing state", changed)

    changed = dict(sources)
    changed["native_fss_client"] = once(changed["native_fss_client"],
        "if(r==S_FALSE) {", "if(false) {")
    expect_slice("OpenXR FSS client accepts only the validated optional-absence table", changed)

    changed = dict(sources)
    changed["native_host"] = once(changed["native_host"],
        "(acquired!=S_OK&&acquired!=S_FALSE)", "(acquired!=S_OK)")
    expect_slice("OpenXR host continues on optional S_FALSE but keeps other errors fatal", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "result.transitionEnabled = state->comfortOwnerAllowed &&", "result.transitionEnabled = true &&")
    expect_slice("native transition output uses the provider's cached Comfort owner", changed)

    changed = dict(sources)
    changed["terrain_cover"] = once(changed["terrain_cover"],
        "if (!ownerAllowed) return;", "if (false) return;")
    expect_slice("terrain hook installation is denied without Comfort ownership", changed)

    changed = dict(sources)
    changed["terrain_cover"] = replace_body(changed["terrain_cover"],
        "void terrainCullCoverNoteFrusta(",
        "!g_ownerAllowed.load(std::memory_order_acquire) ||\n        !g_installed.load(std::memory_order_acquire)",
        "!g_installed.load(std::memory_order_acquire)")
    expect_slice("terrain frusta/poll callbacks remain owner-gated after startup", changed)

    changed = dict(sources)
    changed["native_fss_fixture"] = once(changed["native_fss_fixture"],
        "nativeFssComfortOwnerDenied();", "/* denied FSS fixture omitted */")
    expect_slice("native FSS absence fixture is invoked", changed)

    changed = dict(sources)
    changed["native_treatment_runner"] = once(changed["native_treatment_runner"],
        "edvr::openxr::test::runTreatmentCases(check);", "/* host absence treatment fixture omitted */")
    expect_slice("real native FSS optional-absence treatment fixture is invoked", changed)

    changed = dict(sources)
    changed["native_treatment_fixture"] = once(changed["native_treatment_fixture"],
        "state.fssAcquireMode=State::FssAcquireMode::ownerAbsent;\n    const HRESULT absent=h.fss.acquire(provider,device.Get(),1);",
        "state.fssAcquireMode=State::FssAcquireMode::admitted;\n    const HRESULT absent=h.fss.acquire(provider,device.Get(),1);")
    expect_slice("real client exercises the actual optional FSS absence result", changed)

    changed = dict(sources)
    changed["native_treatment_fixture"] = once(changed["native_treatment_fixture"],
        "state.fssAcquireMode=State::FssAcquireMode::ownerAbsent;\n  check(h.fss.acquire(provider,device.Get(),2)==S_FALSE&&!h.fss.acquired(),",
        "state.fssAcquireMode=State::FssAcquireMode::admitted;\n  check(h.fss.acquire(provider,device.Get(),2)==S_FALSE&&!h.fss.acquired(),")
    expect_slice("real host FSS absence capture uses owner-denied provider state", changed)

    changed = dict(sources)
    changed["vscreen_predicate_fixture"] = once(changed["vscreen_predicate_fixture"],
        "okay &= testDiagnosticsOwnerLegacyGateRegistration();",
        "/* actual registry fixture omitted */")
    expect_slice("actual 64-combination owner-filtered registry fixture is invoked", changed)

    changed = dict(sources)
    changed["intro_owner_gate"] = once(changed["intro_owner_gate"],
        "if (!querySucceeded) return {};", "if (querySucceeded) return {};")
    expect_slice("failed Intro admission queries remain default-denied", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "return introStartupAdmission(decision, status == S_OK);",
        "return introStartupAdmission(decision, true);")
    expect_slice("Intro query success must reflect exact S_OK", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "const IntroStartupAdmission introAdmission = readIntroStartupAdmission();",
        "const IntroStartupAdmission introAdmission{};")
    expect_slice("Intro startup samples one published decision before config", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "!introAdmission.strictSelection &&", "!introOwnerAllowed &&")
    expect_slice("Intro strict-selection remains distinct from owner admission", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "const bool diagnosticsAllowed = diagnosticsOwnerAllowed();",
        "const bool diagnosticsAllowed = false;")
    expect_slice("Diagnostics admission is snapshotted before dormant startup selection", changed)

    changed = dict(sources)
    changed["intro_owner_gate"] = once(changed["intro_owner_gate"],
        "if (decision.mode == static_cast<std::uint32_t>(\n"
        "            plugin_selection::admission_v1::Mode::AdmittedSelection)) {",
        "if (decision.mode != static_cast<std::uint32_t>(\n"
        "            plugin_selection::admission_v1::Mode::AdmittedSelection)) {")
    expect_slice("D/TAA dormancy bypass requires a valid admitted strict selection", changed)

    changed = dict(sources)
    changed["intro_owner_gate"] = once(changed["intro_owner_gate"],
        "(diagnosticsOwnerAllowed || temporalAAOwnerAllowed)",
        "(diagnosticsOwnerAllowed && temporalAAOwnerAllowed)")
    expect_slice("either strict-selected D or TAA owner keeps its startup path", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "!strictSelectedVScreenOwner &&", "strictSelectedVScreenOwner &&")
    expect_slice("only strict-selected D/TAA ownership bypasses transport-only startup", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runIntroOwned(g_state && g_state->introOwnerAllowed, [&] {\n"
        "        g_introLifecycleRegistered = ensureIntroVideoLifecycle();",
        "runIntroOwned(true, [&] {\n"
        "        g_introLifecycleRegistered = ensureIntroVideoLifecycle();")
    expect_slice("Intro lifecycle factory remains behind its cached owner gate", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (!g_state || !g_state->introOwnerAllowed) return false;\n"
        "    return pluginRegistryRegisterLifecycle(introVideoLifecycleOps());",
        "if (!g_state || g_state->introOwnerAllowed) return false;\n"
        "    return pluginRegistryRegisterLifecycle(introVideoLifecycleOps());")
    expect_slice("Intro registry cannot construct or register its lifecycle while denied", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runIntroOwned(introOwnerAllowed, [&] {\n"
        "        enable(InterestId::IntroCurveObserve, introCurveDrawInterestConfigured());",
        "runIntroOwned(true, [&] {\n"
        "        enable(InterestId::IntroCurveObserve, introCurveDrawInterestConfigured());")
    expect_slice("Intro curve draw-interest configuration remains owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "s->introOwnerAllowed && kind == 'X' && count == 6 && introPanelWants()",
        "kind == 'X' && count == 6 && introPanelWants()")
    expect_slice("Intro panel resolver cannot bypass cached ownership", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runIntroOwned(g_state && g_state->introOwnerAllowed,\n"
        "                      [&] { holoBegin(self); });",
        "runIntroOwned(true,\n"
        "                      [&] { holoBegin(self); });")
    expect_slice("Intro begin forwarding remains owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runIntroOwned(g_state->introOwnerAllowed, [&] {\n"
        "        tkIntroPanel.run(",
        "runIntroOwned(true, [&] {\n"
        "        tkIntroPanel.run(")
    expect_slice("complete Intro frame bookkeeping remains owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "{\"legacy.intro-panel\", &drawGate_intro_panel, true, LegacyGateOwner::Intro}",
        "{\"legacy.intro-panel\", &drawGate_intro_panel, true, LegacyGateOwner::Core}")
    expect_slice("Intro draw subscription retains Intro ownership metadata", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "{\"legacy.panel-curve\", &drawGate_panel_curve, true, LegacyGateOwner::OnFootPanel}",
        "{\"legacy.panel-curve\", &drawGate_panel_curve, true, LegacyGateOwner::Core}")
    expect_slice("panel-curve observer retains On-foot ownership despite shared Intro runtime", changed)

    changed = dict(sources)
    changed["vscreen_predicate_fixture"] = once(changed["vscreen_predicate_fixture"],
        "okay &= testIntroOwnerDeniedVisitor(device, immediate);",
        "/* Intro WARP denial/admission visitor omitted */")
    expect_slice("real Intro denied/admitted visitor fixture stays invoked", changed)

    changed = dict(sources)
    changed["plugin_dispatch_fixture"] = once(changed["plugin_dispatch_fixture"],
        "testIntroStartupAdmissionSnapshot();",
        "/* Intro startup admission fixture omitted */")
    expect_slice("Intro startup strict/Legacy decision fixture stays invoked", changed)

    changed = dict(sources)
    changed["plugin_dispatch_fixture"] = once(changed["plugin_dispatch_fixture"],
        "testDiagnosticsTemporalStartupAdmissionPolicy();",
        "/* controlled strict D/TAA policy fixture omitted */")
    expect_slice("controlled strict D/TAA policy fixture stays invoked without claiming runtime positives", changed)

    changed = dict(sources)
    changed["vscreen_predicate_fixture"] = once(changed["vscreen_predicate_fixture"],
        "fssPanelSite, immediate, immediate, 0, true, true,\n"
        "                              &deniedScanner, true, false, 'X', 6, 1, 5, 6)",
        "fssPanelSite, immediate, immediate, 0, true, true,\n"
        "                              &deniedScanner, true, true, 'X', 6, 1, 5, 6)")
    expect_slice("real scanner visitor fixture uses denied Comfort state", changed)

    changed = dict(sources)
    changed["native_frame_fixture"] = once(changed["native_frame_fixture"],
        "nativeFrameComfortOwnerDenied();", "/* native-frame absence fixture omitted */")
    expect_slice("native-frame denied-owner fixture is invoked", changed)

    changed = dict(sources)
    changed["native_temporal_fixture"] = once(changed["native_temporal_fixture"],
        "nativeTemporalOwnerDenied();", "/* native Temporal-AA absence fixture omitted */")
    expect_slice("real native Temporal-AA absence fixture remains invoked", changed)

    changed = dict(sources)
    changed["native_temporal"] = once(changed["native_temporal"],
        "if(!edvr::temporalAAOwnerAllowed())return S_FALSE;",
        "if(false)return S_FALSE;")
    expect_slice("denied Temporal-AA native acquire skips pool/config/device work", changed)

    changed = dict(sources)
    changed["flat_runtime"] = once(changed["flat_runtime"],
        "const auto mode = temporalAAOwnerAllowed\n        ? Config::get().requestedTemporalMode()",
        "const auto mode = Config::get().requestedTemporalMode()")
    expect_slice("flat runtime never reads Temporal-AA mode without its cached owner", changed)

    changed = dict(sources)
    changed["flat_runtime"] = once(changed["flat_runtime"],
        "if(!manual&&!s.drawPackets.active())return;",
        "const int asked=Config::get().getInt(\"advanced.temporal_aa_jitter_phases\",8);\n"
        "        if(!manual&&!s.drawPackets.active())return;")
    expect_slice("AA-off flat capture returns before reading Temporal-AA jitter config", changed)

    changed = dict(sources)
    changed["flat_runtime"] = once(changed["flat_runtime"],
        "s.drawPackets.present(s.drawPacketContext.Get(),frame+1);",
        "/* Core draw-packet present omitted */")
    expect_slice("flat Core draw-packet capture remains active when Temporal-AA is absent", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (g_state->temporalAAOwnerAllowed)\n            tkEngineVelocity.run(",
        "if (true)\n            tkEngineVelocity.run(")
    expect_slice("vScreen does not dispatch EngineVelocity clock without Temporal-AA admission", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (temporalAAOwnerActive())\n        engineVelocityResourceWritten(resource);",
        "engineVelocityResourceWritten(resource);")
    expect_slice("shared resource writes do not invalidate EngineVelocity without the cached owner", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "return g_state && g_state->temporalAAOwnerAllowed;",
        "return true;")
    expect_slice("vScreen Temporal-AA accessor cannot bypass its cached owner decision", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (g_state->temporalAAOwnerAllowed)\n"
        "        tkCelestialStatus.run(tickCelestialStatus);",
        "tkCelestialStatus.run(tickCelestialStatus);")
    expect_slice("Celestial Status tick is skipped without Temporal-AA admission", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (g_state->temporalAAOwnerAllowed)\n            engineVelocityRememberVs(",
        "if (true)\n            engineVelocityRememberVs(")
    expect_slice("device shader capture remains cached-owner gated", changed)

    changed = dict(sources)
    changed["flat_runtime"] = once(changed["flat_runtime"],
        "if (!temporalAAOwnerAllowed()) return;",
        "/* TAA owner boundary removed */")
    expect_slice("flat shared draw hooks do not dispatch TAA substitution without admission", changed)

    changed = dict(sources)
    changed["exposure_fix"] = once(changed["exposure_fix"],
        "if (temporalAAOwnerAllowed() && engineVelocitySkinWanted() && skinChainBound())",
        "if (engineVelocitySkinWanted() && skinChainBound())")
    expect_slice("Comfort chain observer cannot sample the TAA skin path without its owner", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (g_state->temporalAAOwnerAllowed && mapData0 && type != D3D11_MAP_READ && celestialMotionAnyWatched()) {",
        "if (mapData0 && type != D3D11_MAP_READ && celestialMotionAnyWatched()) {")
    expect_slice("planet-patch Map tee stays owner-gated", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "if (!fromEdvr && g_state->temporalAAOwnerAllowed && out && *out) {\n"
        "                if (uiSurfacesWantsAtlas(*desc)) {",
        "if (!fromEdvr && out && *out) {\n"
        "                if (uiSurfacesWantsAtlas(*desc)) {")
    expect_slice("UI surface atlas/chain recognition remains owner-gated after game texture creation", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (g_state->temporalAAOwnerAllowed && type != D3D11_MAP_READ)\n"
        "        uiAtlasNoteWrite(res, 1);",
        "if (type != D3D11_MAP_READ)\n        uiAtlasNoteWrite(res, 1);")
    expect_slice("UI atlas write Map observer remains owner-gated", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (g_state->temporalAAOwnerAllowed) {\n"
        "        uiDepthConfigure(cfg);\n"
        "        uiLayerConfigure(cfg);",
        "if (true) {\n"
        "        uiDepthConfigure(cfg);\n"
        "        uiLayerConfigure(cfg);")
    expect_slice("TAA-only UI panel scaling is not configured without cached ownership", changed)

    changed = dict(sources)
    changed["native_fss"] = once(changed["native_fss"],
        "const bool ownerAllowed=edvr::comfortOwnerAllowed();",
        "const bool ownerAllowed=true;")
    expect_slice("native FSS snapshots published Comfort admission at Acquire", changed)

    changed = dict(sources)
    changed["native_fss"] = once(changed["native_fss"],
        "if(s->ownerAllowed)edvr::terrainCullCoverNoteFrusta(f->frusta);",
        "edvr::terrainCullCoverNoteFrusta(f->frusta);")
    expect_slice("shared Comfort frusta publication remains owner-gated", changed)

    changed = dict(sources)
    changed["native_fss_fixture"] = once(changed["native_fss_fixture"],
        "nativeFssPublishesComfortFrustaOnce();", "/* Comfort frusta positive control omitted */")
    expect_slice("native FSS retains its once-per-frame frusta control", changed)

    changed = dict(sources)
    changed["intro_owner_gate"] = once(changed["intro_owner_gate"],
        "admission.strictModeSelection &&",
        "admission.strictModeSelection ||")
    expect_slice("admitted Diagnostics cannot be stranded in transport-only startup", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "const bool diagnosticsAllowed = diagnosticsOwnerAllowed();",
        "const bool diagnosticsAllowed = false;")
    expect_slice("D owner snapshot is taken before dormant startup selection", changed)

    changed = dict(sources)
    changed["intro_owner_gate"] = once(changed["intro_owner_gate"],
        "if (decision.mode == static_cast<std::uint32_t>(\n"
        "            plugin_selection::admission_v1::Mode::AdmittedSelection)) {",
        "if (decision.mode != static_cast<std::uint32_t>(\n"
        "            plugin_selection::admission_v1::Mode::AdmittedSelection)) {")
    expect_slice("D/TAA dormancy bypass requires a valid admitted strict selection", changed)

    changed = dict(sources)
    changed["intro_owner_gate"] = once(changed["intro_owner_gate"],
        "(diagnosticsOwnerAllowed || temporalAAOwnerAllowed)",
        "(diagnosticsOwnerAllowed && temporalAAOwnerAllowed)")
    expect_slice("either strict-selected D or TAA owner keeps its startup path", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "!strictSelectedVScreenOwner &&", "strictSelectedVScreenOwner &&")
    expect_slice("only strict-selected D/TAA ownership bypasses transport-only startup", changed)

    changed = dict(sources)
    changed["proxy"] = once(changed["proxy"],
        "edvr::hookDevice(device);\n        if (swapChain)",
        "edvr::hookSwapChain(swapChain);\n        if (swapChain)")
    expect_slice("device owner publication precedes Present-capable swapchain hooks", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "bool onFootPanelOwnerAllowed = false;", "bool onFootPanelOwnerAllowed = true;")
    expect_slice("On-foot device cache is fail-closed before publication", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "plugins::kPluginOnFootPanel);", "plugins::kPluginPerformance);")
    expect_slice("device startup snapshots the On-foot manifest owner", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "runOnFootPanelOwned(s.onFootPanelOwnerAllowed, [&] {\n            Config& cfg = Config::get();",
        "runOnFootPanelOwned(true, [&] {\n            Config& cfg = Config::get();")
    expect_slice("initial On-foot panel resolution remains denied without admission", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "const bool onFootPanelOwnerAllowed = edvr::onFootPanelOwnerAllowed();",
        "const bool onFootPanelOwnerAllowed = true;")
    expect_slice("VScreen copies the cached On-foot owner instead of opening the path", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runOnFootPanelOwned(onFootPanelOwnerAllowed, [&] {\n        wantVoid = cfg.getBool(\"fix.black_void\", true);",
        "runOnFootPanelOwned(true, [&] {\n        wantVoid = cfg.getBool(\"fix.black_void\", true);")
    expect_slice("cold On-foot startup config stays behind cached admission", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runOnFootPanelOwned(s->onFootPanelOwnerAllowed, [&] {\n        s->blackVoid = cfg.getBool(\"fix.black_void\", true);",
        "runOnFootPanelOwned(true, [&] {\n        s->blackVoid = cfg.getBool(\"fix.black_void\", true);")
    expect_slice("On-foot reload config cannot run for an absent owner", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        '{"legacy.panel-distance", &drawGate_panel_distance, true, LegacyGateOwner::OnFootPanel}',
        '{"legacy.panel-distance", &drawGate_panel_distance, true, LegacyGateOwner::Core}')
    expect_slice("On-foot panel-distance registry row retains owner filtering", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (!legacyDrawGateOwnerAllowed(registration, state)) continue;",
        "if (false) continue;")
    expect_slice("denied On-foot registry callbacks are filtered before registration", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "if (s->onFootPanelOwnerAllowed && panelCurveWants() &&",
        "if (panelCurveWants() &&")
    expect_slice("panel-curve observer preserves trace but skips absent-owner module work", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "runVScreenFootprintOwned(g_state, [&] {\n            tkVScreenFootprint.run",
        "runVScreenFootprintOwned(nullptr, [&] {\n            tkVScreenFootprint.run")
    expect_slice("footprint capture and frame work remain cached-owner gated", changed)

    changed = dict(sources)
    changed["screen_motion"] = once(changed["screen_motion"],
        "if (onFootPanelOwnerAllowed) {\n        const bool weapon=cfg.getBool(\"fix.weapon_stability\",true);",
        "if (true) {\n        const bool weapon=cfg.getBool(\"fix.weapon_stability\",true);")
    expect_slice("ScreenMotion does not read weapon config without On-foot admission", changed)

    changed = dict(sources)
    changed["screen_motion"] = once(changed["screen_motion"],
        "if (g.onFootPanelOwnerAllowed)\n        weaponMotionFrameBoundary(ctx);\n    if (!g.temporalAAOwnerAllowed) return;",
        "weaponMotionFrameBoundary(ctx);\n    if (!g.temporalAAOwnerAllowed) return;")
    expect_slice("weapon frame work stays owner-gated independently of Temporal-AA", changed)

    changed = dict(sources)
    changed["native_frame"] = once(changed["native_frame"],
        "if (state->onFootPanelOwnerAllowed) {\n        const bool weaponStability",
        "if (true) {\n        const bool weaponStability")
    expect_slice("native-frame weapon pacing uses the provider's cached owner", changed)

    changed = dict(sources)
    changed["temporal_pass"] = once(changed["temporal_pass"],
        "auto* weapon=onFootPanelOwnerAllowed() ? weaponMotionView() : nullptr;",
        "auto* weapon=weaponMotionView();")
    expect_slice("Temporal-AA does not consume weapon maps while On-foot is absent", changed)

    changed = dict(sources)
    changed["vr_world_route"] = once(changed["vr_world_route"],
        "ID3D11ShaderResourceView* map = onFootPanelOwnerAllowed()\n        ? weaponMotionView() : nullptr;",
        "ID3D11ShaderResourceView* map = weaponMotionView();")
    expect_slice("VR world route does not consume weapon maps while On-foot is absent", changed)

    changed = dict(sources)
    changed["menu"] = once(changed["menu"],
        "if (onFootPanelOwnerAllowed())\n                    resolveVScreenTargetResolution(Config::get(), &vw, &vh, temporalAAOwnerAllowed(),",
        "if (true)\n                    resolveVScreenTargetResolution(Config::get(), &vw, &vh, temporalAAOwnerAllowed(),")
    expect_slice("menu hint avoids On-foot settings reads when the owner is absent", changed)

    changed = dict(sources)
    changed["vscreen_res"] = once(changed["vscreen_res"],
        "if (temporalAAOwnerAllowed) {",
        "if (true) {")
    expect_slice("vScreen fit resolver does not read cold Temporal-AA keys when owner is denied", changed)

    changed = dict(sources)
    changed["intro_upscale"] = once(changed["intro_upscale"],
        "resolveVScreenTargetResolution(cfg, &vw, &vh, temporalAAOwnerAllowed(),",
        "resolveVScreenTargetResolution(cfg, &vw, &vh,")
    expect_slice("Intro fit call passes the published Temporal-AA owner decision", changed)

    changed = dict(sources)
    changed["menu"] = once(changed["menu"],
        "if (!temporalAAOwnerAllowed()) return true;\n    if (runtimeFlatProfile()) return !flatDlssSelected();",
        "/* preset row reads mode without owner */\n    if (runtimeFlatProfile()) return !flatDlssSelected();")
    expect_slice("menu preset-row dimming does not read Temporal-AA mode without owner", changed)

    changed = dict(sources)
    changed["menu"] = once(changed["menu"],
        'if (!temporalAAOwnerAllowed()) {\n        statusLine(c, "Temporal AA", "owner not admitted");\n    } else {',
        'if (false) {\n        statusLine(c, "Temporal AA", "owner not admitted");\n    } else {')
    expect_slice("menu status preserves owner-denied cold mode branch", changed)

    changed = dict(sources)
    changed["menu"] = once(changed["menu"],
        "if (dlssRow && temporalAAOwnerAllowed()) {",
        "if (dlssRow) {")
    expect_slice("menu preset row label does not read Temporal-AA mode when owner is absent", changed)

    changed = dict(sources)
    changed["menu"] = once(changed["menu"],
        "const bool refusing = temporalAAOwnerAllowed() &&",
        "const bool refusing = true &&")
    expect_slice("flat warning does not inspect refusal or requested mode without TAA owner", changed)

    changed = dict(sources)
    changed["menu"] = once(changed["menu"],
        "if (refusing || s.open || s.flatSettingsForce) {",
        "if (refusing) {")
    expect_slice("flat settings polling remains available to open/forced Core menu without TAA", changed)

    changed = dict(sources)
    changed["menu"] = once(changed["menu"],
        "if (runtimeFlatProfile() && temporalAAOwnerAllowed()) {\n            const char* wrapper = contextWrapperFile();",
        "if (runtimeFlatProfile()) {\n            const char* wrapper = contextWrapperFile();")
    expect_slice("flat wrapper note probe and requested mode remain TAA-owned", changed)

    changed = dict(sources)
    changed["device"] = once(changed["device"],
        "s.temporalAAOwnerAllowed ? 1u : 0u",
        "temporalAAOwnerAllowed() ? 1u : 0u")
    expect_slice("Performance sampler receives the cached post-commit TAA snapshot", changed)

    changed = dict(sources)
    changed["texture_sampler_policy"] = once(changed["texture_sampler_policy"],
        "context->temporalAAOwnerAllowed <= 1u",
        "true")
    expect_slice("sampler policy validates the cached Temporal-AA owner context before using it", changed)

    changed = dict(sources)
    changed["texture_sampler_policy"] = once(changed["texture_sampler_policy"],
        'const bool temporal = temporalOwnerAllowed && temporalModeEnabled(\n'
        '        Config::get().getString("fix.temporal_aa", "off"));',
        'const bool temporal = temporalOwnerAllowed;')
    expect_slice("admitted TAA ownership does not enable the auto LOD default when the configured mode is off", changed)

    changed = dict(sources)
    changed["vscreen_predicate_fixture"] = once(changed["vscreen_predicate_fixture"],
        "okay &= testOnFootOwnerDeniedFootprint(device, immediate);",
        "/* On-foot footprint denial fixture omitted */")
    expect_slice("real footprint absence and admitted-copy fixture stays invoked", changed)


def expect(label: str, sources: tuple[str, ...], changed: tuple[str, ...]) -> None:
    global MUTATION_COUNT
    MUTATION_COUNT += 1
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
    expect("Scanner denied-owner lifecycle selection", s, s[:6] + (replace_body(s[6], "bool ensureScannerLifecycle()", "if (!g_state || !g_state->comfortOwnerAllowed) return false;", "if (g_state && g_state->comfortOwnerAllowed) return false;"),) + s[7:])
    expect("Comfort denied-owner lifecycle selection", s, s[:6] + (replace_body(s[6], "bool ensureComfortLifecycle()", "if (!g_state || !g_state->comfortOwnerAllowed) return false;", "if (g_state && g_state->comfortOwnerAllowed) return false;"),) + s[7:])
    expect("Scanner reveal frame boundary scope", s, s[:6] + (replace_body(s[6], "void vScreenFrameBoundary()", "frameScannerLifecycleStage(\n                plugins::scanners::kScannerLifecycleRevealFrame, s->frameNo,\n                fssRevealFrameBoundary);", "fssRevealFrameBoundary();"),) + s[7:])
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
    slice_sources = {name: path.read_text(encoding="utf-8")
                     for name, path in SLICE_PATHS.items()}
    check(*sources)
    check_module_slices(slice_sources)
    if args.self_test:
        self_test(sources)
        self_test_slices(slice_sources)
        print(f"PASS: grouped lifecycle wiring and module boundaries ({MUTATION_COUNT} mutations)")
    else:
        print("DRY RUN: grouped lifecycle and module-boundary pins passed; no files written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
