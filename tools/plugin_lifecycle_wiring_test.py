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
    "native_fov_startup": ROOT / "src/openxr/native_fov_startup.h",
    "fov_facade": ROOT / "src/openxr/native_fov_trim.h",
    "trim_fixture": ROOT / "tools/native_trim_test/native_trim_test.cpp",
    "relay_header": ROOT / "src/d3d11/scheduler_relay.h",
    "relay_host": ROOT / "src/d3d11/kinematic_eval_hook.cpp",
    "vscreen": ROOT / "src/d3d11/vscreen.cpp",
    "device": ROOT / "src/d3d11/device_hook.cpp",
    "registry": ROOT / "src/d3d11/plugin_registry.cpp",
    "explorer_owner_gate": ROOT / "src/d3d11/explorer_cam_owner_gate.h",
    "explorer_fixture": ROOT / "tools/explorer_cam_test/explorer_cam_test.cpp",
    "cockpit_owner_gate": ROOT / "src/d3d11/cockpit_visuals_owner_gate.h",
    "cockpit_api_fixture": ROOT / "tools/cockpit_api_test/cockpit_api_test.cpp",
    "vscreen_predicate_fixture": ROOT / "tools/vscreen_predicate_test/vscreen_predicate_test.cpp",
    "sunglare": ROOT / "src/d3d11/sunglare_fix.cpp",
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
    registry = sources["registry"]
    build = sources["build"]
    explorer_owner_gate = sources["explorer_owner_gate"]
    explorer_fixture = sources["explorer_fixture"]
    cockpit_owner_gate = sources["cockpit_owner_gate"]
    cockpit_api_fixture = sources["cockpit_api_fixture"]
    vscreen_predicate_fixture = sources["vscreen_predicate_fixture"]
    sunglare = sources["sunglare"]
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
    ordered(sched_config_host, "pluginRegistryRegisterLifecycle(",
            "pluginRegistryConfigureLifecycle(", "return;",
            "edvrDiagnosticsSchedulerConfigure(edvrDiagnosticsSchedulerState(),")
    sched_shutdown_host = body(vscreen, "void shutdownSchedulerLifecycle()")
    ordered(sched_shutdown_host, "pluginRegistryHasLifecycle(",
            "pluginRegistryShutdownLifecycle(", "return;",
            "edvrDiagnosticsSchedulerShutdown(edvrDiagnosticsSchedulerState())")
    device_tick = body(device, "tkSchedulerProbe.run([] {")
    ordered(device_tick, "g_state->frameCounter", "pluginRegistryHasLifecycle(",
            "pluginRegistryFrameLifecycle(", "} else {", "edvrDiagnosticsSchedulerFrame(")
    if "schedulerStackProbe" in device_tick or "SchedulerStackProbe" in device_tick:
        raise AssertionError("core Present host must dispatch Diagnostics through scalar lifecycle callbacks")
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
    if "if (!ownerAllowed) return false;\n    std::forward<Callback>(callback)();\n    return true;" not in cockpit_owner_gate:
        raise AssertionError("denied Cockpit owner helper must return without invoking its callback")
    if vscreen.count("ownerAllowedForStartup(") != 1:
        raise AssertionError("Cockpit ownership must be queried once at VScreen startup")
    if "ownerAllowedForStartup" in body(vscreen, "void vScreenFrameBoundary()") or \
            "ownerAllowedForStartup" in body(vscreen, "void shutdownVScreenFixes()"):
        raise AssertionError("Cockpit Present and shutdown must use the cached startup decision")
    startup = body(vscreen, "void installVScreenFixes(")
    query_start = startup.find("ownerAllowedForStartup(")
    query_end = startup.find(");", query_start)
    query = startup[query_start:query_end + 2] if query_start >= 0 and query_end >= 0 else ""
    ordered(startup, "ownerAllowedForStartup(",
            "runCockpitVisualsOwned(cockpitVisualsOwnerAllowed, [&] {")
    if "plugins::kPluginCockpitVisuals" not in query:
        raise AssertionError("VScreen startup must sample the Cockpit admission decision")
    startup_gate = body(startup,
                         "runCockpitVisualsOwned(cockpitVisualsOwnerAllowed, [&] {")
    if "cockpitVisualsPluginOps()" not in startup_gate:
        raise AssertionError("denied Cockpit ownership must skip its optional factory/registration")
    if startup.count("cockpitVisualsPluginOps()") != 1 or \
            startup.count("ownerAllowedForStartup(") != 1:
        raise AssertionError("Cockpit startup factory/query must each have one gated callsite")
    if "(cockpitVisualsOwnerAllowed || !exposureDampingActive())" not in startup:
        raise AssertionError("Comfort glare-train hook exception must apply only when Cockpit is denied")

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
    if "{\"legacy.sun-glare\", &drawGate_sun_glare, true}" not in vscreen:
        raise AssertionError("shared Comfort glare observation lost its legacy draw-gate subscription")
    sun_gate_start = vscreen.find("EDVR_GATE_STATE(sun_glare,")
    sun_gate_end = vscreen.find("EDVR_GATE_OWNED(particles", sun_gate_start)
    sun_gate = vscreen[sun_gate_start:sun_gate_end] if sun_gate_start >= 0 and sun_gate_end >= 0 else ""
    if not sun_gate:
        raise AssertionError("sun-glare draw gate must preserve admitted Legacy behavior and use only Comfort damping when denied")
    ordered(sun_gate, "s->cockpitVisualsOwnerAllowed", "? sunglareWantsDraws()",
            ": exposureDampingActive()")
    if "drawGateSubscribed(State* s) { return pluginRegistryWantsDraws(s); }" not in vscreen or \
            "(void)registerLegacyDrawGates(g_state);" not in startup:
        raise AssertionError("shared glare observer is not connected to the live VScreen draw boundary")
    if "{\"legacy.sun-glare\", &drawGate_sun_glare, true}" not in vscreen:
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
                      "billboard", "g_sunglareWorld", "g_sunglareMode", "g_mode", "g_keep"):
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


def expect_slice(label: str, sources: dict[str, str]) -> None:
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
    changed["sunglare"] = replace_body(changed["sunglare"],
        "void sunglareTrackExposureGlareTrain(",
        "bool damping = false;",
        "const auto cockpitMode = g_mode; (void)cockpitMode; bool damping = false;")
    expect_slice("shared Comfort glare observer avoids Cockpit mode state", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        ": exposureDampingActive())", ": sunglareWantsDraws())")
    expect_slice("denied sun-glare subscription avoids Cockpit draw state", changed)

    changed = dict(sources)
    changed["vscreen"] = once(changed["vscreen"],
        "(cockpitVisualsOwnerAllowed || !exposureDampingActive())", "true")
    expect_slice("dormant hook exception is limited to denied Cockpit ownership", changed)

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
    slice_sources = {name: path.read_text(encoding="utf-8")
                     for name, path in SLICE_PATHS.items()}
    check(*sources)
    check_module_slices(slice_sources)
    if args.self_test:
        self_test(sources)
        self_test_slices(slice_sources)
        print("PASS: grouped lifecycle wiring and module boundaries (71 mutations)")
    else:
        print("DRY RUN: grouped lifecycle and module-boundary pins passed; no files written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
