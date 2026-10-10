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
    "relay_header": ROOT / "src/d3d11/scheduler_relay.h",
    "relay_host": ROOT / "src/d3d11/kinematic_eval_hook.cpp",
    "vscreen": ROOT / "src/d3d11/vscreen.cpp",
    "device": ROOT / "src/d3d11/device_hook.cpp",
    "registry": ROOT / "src/d3d11/plugin_registry.cpp",
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
    relay_h = sources["relay_header"]
    relay = sources["relay_host"]
    vscreen = sources["vscreen"]
    device = sources["device"]
    registry = sources["registry"]
    build = (ROOT / "build.bat").read_text(encoding="utf-8")
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
    ordered(rem_configure_host, "pluginRegistryRegisterLifecycle(",
            "pluginRegistryConfigureLifecycle(", "return;", "remlokConfigure(cfg);")
    rem_frame_host = body(vscreen, "void frameRemlokLifecycle(uint32_t sceneFrame)")
    ordered(rem_frame_host, "pluginRegistryHasLifecycle(",
            "pluginRegistryFrameLifecycle(", "return;", "remlokFrameBoundary();")
    rem_shutdown_host = body(vscreen, "void shutdownRemlokLifecycle()")
    ordered(rem_shutdown_host, "pluginRegistryHasLifecycle(",
            "pluginRegistryShutdownLifecycle(", "return;", "remlokShutdown();")
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
    )
    for path in required_build_sources:
        if build.count(path) < 2:
            raise AssertionError(f"build must compile/archive and preserve direct rig source coverage for {path}")
    if "plugin_performance.lib" not in build or "plugin_performance.lib" not in flat_proxy:
        raise AssertionError("Performance archive must feed production and flat SDK bench links")
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


def expect_slice(label: str, sources: dict[str, str]) -> None:
    try:
        check_module_slices(sources)
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"mutation not detected: {label}")


def self_test_slices(sources: dict[str, str]) -> None:
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
        print("PASS: grouped lifecycle wiring and module boundaries (32 mutations)")
    else:
        print("DRY RUN: grouped lifecycle and module-boundary pins passed; no files written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
