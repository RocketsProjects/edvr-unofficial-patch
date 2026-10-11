"""Source pins for the production texture sampler policy adapter.

The policy rig covers policy math. The separate hook rig includes the same
production hook body as device_hook.cpp and exercises its forwarding and
disabled/absent paths without a live D3D vtable.
"""

import argparse
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOST_PATH = ROOT / "src/d3d11/device_hook.cpp"
HOOK_PATH = ROOT / "src/d3d11/sampler_create_hook.inl"
STARTUP_PATH = ROOT / "src/d3d11/sampler_startup.h"
POLICY_PATH = ROOT / "src/plugins/performance/texture_sampler_policy.cpp"
HEADER_PATH = ROOT / "src/plugins/performance/texture_sampler_policy.h"
RIG_PATH = ROOT / "tools/performance_sampler_test/texture_sampler_policy_test.cpp"
HOOK_RIG_PATH = ROOT / "tools/performance_sampler_hook_test/sampler_hook_test.cpp"


def code_only(source):
    out = list(source)
    state = "code"
    i = 0
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and n == "/":
                out[i] = out[i + 1] = " "
                state = "line"
                i += 2
                continue
            if c == "/" and n == "*":
                out[i] = out[i + 1] = " "
                state = "block"
                i += 2
                continue
            if c in ('"', "'"):
                out[i] = " "
                state = c
        elif state == "line":
            if c == "\n":
                state = "code"
            else:
                out[i] = " "
        elif state == "block":
            if c == "*" and n == "/":
                out[i] = out[i + 1] = " "
                state = "code"
                i += 2
                continue
            if c != "\n":
                out[i] = " "
        else:
            if c == "\\" and n:
                out[i] = " "
                if n != "\n":
                    out[i + 1] = " "
                i += 2
                continue
            if c == state:
                state = "code"
            if c != "\n":
                out[i] = " "
        i += 1
    return "".join(out)


def function_body(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", source)
    if not match:
        raise AssertionError("missing function " + name)
    opening = source.find("{", match.start(), match.end())
    depth = 0
    for end in range(opening, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:end]
    raise AssertionError("unclosed function " + name)


def block_end(source, opening):
    depth = 0
    for end in range(opening, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return end
    raise AssertionError("unclosed source block")


def ordered(source, *patterns):
    offset = 0
    for pattern in patterns:
        match = re.search(pattern, source[offset:])
        if not match:
            return False
        offset += match.end()
    return True


def inspect(host_text=None, policy_text=None, header_text=None,
            hook_text=None, hook_rig_text=None, startup_text=None,
            startup_rig_text=None):
    host_raw = HOST_PATH.read_text(encoding="utf-8") if host_text is None else host_text
    hook_raw = HOOK_PATH.read_text(encoding="utf-8") if hook_text is None else hook_text
    startup_raw = (STARTUP_PATH.read_text(encoding="utf-8")
                   if startup_text is None else startup_text)
    policy_raw = POLICY_PATH.read_text(encoding="utf-8") if policy_text is None else policy_text
    header_raw = HEADER_PATH.read_text(encoding="utf-8") if header_text is None else header_text
    rig_raw = RIG_PATH.read_text(encoding="utf-8")
    hook_rig_raw = (HOOK_RIG_PATH.read_text(encoding="utf-8")
                    if hook_rig_text is None else hook_rig_text)
    startup_rig = hook_rig_raw if startup_rig_text is None else startup_rig_text
    host = code_only(host_raw)
    hook_source = code_only(hook_raw)
    startup_source = code_only(startup_raw)
    policy = code_only(policy_raw)
    header = code_only(header_raw)
    policy_compact = re.sub(r"\s+", " ", policy_raw)
    failures = []

    def require(condition, message):
        if not condition:
            failures.append(message)

    require(host_raw.count('#include "sampler_create_hook.inl"') == 1 and
            hook_rig_raw.count('#include "../../src/d3d11/sampler_create_hook.inl"') == 1 and
            "HRESULT STDMETHODCALLTYPE hookedCreateSamplerState(" not in host_raw and
            "HRESULT STDMETHODCALLTYPE hookedCreateSamplerState(" not in hook_rig_raw,
            "host and hook rig must include the one production hook body without a duplicate")

    try:
        hook = function_body(hook_source, "hookedCreateSamplerState")
        configure = function_body(policy, "resolveTextureSamplerSettingsV1")
        resolve = function_body(policy, "resolveTextureSamplerSettingsFromValuesV1")
        rewrite = function_body(policy, "rewriteTextureSamplerV1")
    except AssertionError as exc:
        return [str(exc)]

    require("if (!desc || self != s.device) return s.realCreateSamplerState(self, desc, out);" in hook,
            "null descriptors and foreign devices must forward the original descriptor")
    require(len(re.findall(r"\bs\.realCreateSamplerState\s*\(", hook)) == 2,
            "the host hook must have one bypass forward and one final real call")
    require(ordered(hook,
                    r"if\s*\(\s*s\.samplerSettings\.rewriteEnabled\s*&&\s*s\.samplerPolicy\s*&&",
                    r"TextureSamplerDescV1\s+input",
                    r"s\.samplerPolicy->rewrite\s*\(",
                    r"if\s*\(\s*rewritten\s*\)"),
            "disabled or absent policy must return before POD construction and callback")
    require(set(re.findall(r"\bd\.(\w+)\s*=", hook)) ==
            {"Filter", "MaxAnisotropy", "MipLODBias"},
            "the host may apply only the three sampler fields owned by policy")
    require("s.deviceHook.replace(kDevCreateSamplerState, &hookedCreateSamplerState" in host,
            "the sampler hook remains present")
    require(host_raw.count('#include "sampler_startup.h"') == 1 and
            startup_rig.count('#include "../../src/d3d11/sampler_startup.h"') == 1,
            "host and fixture include the same production startup boundary")
    try:
        startup = function_body(startup_source, "samplerStartupConfigure")
    except AssertionError as exc:
        return [str(exc)]
    require(ordered(startup,
                    r"\*policyOut\s*=\s*nullptr",
                    r"memset\s*\(settingsOut,\s*0",
                    r"if\s*\(!factory\)\s*return false",
                    r"const [\w:]*TextureSamplerPolicyOpsV1\* ops\s*=\s*factory\(\)",
                    r"if\s*\(!ops\s*\|\|[\s\S]*?!ops->configure\)",
                    r"\*policyOut\s*=\s*ops",
                    r"ops->configure\s*\(",
                    r"return settingsOut->biasAutoRequested && !settingsOut->autoSourceAvailable"),
            "startup clears absent state, validates Ops, then configures and returns the historical warning condition")
    require("samplerStartupConfigure(" in host and
            "&plugins::performance::textureSamplerPolicyOpsV1" in host and
            "&samplerReadEliteHmdMultiplier" in host and
            "if (warnAutoSourceUnavailable)" in host and
            "s.deviceHook.replace(kDevCreateSamplerState, &hookedCreateSamplerState" in host,
            "current device startup uses the private helper and retains sampler hook installation")
    require("s.samplerPolicy->configure" not in host_raw and
            "advanced.texture_lod_bias = auto, but Elite's graphics" in host_raw,
            "configuration moved behind the helper and startup warning text remains literal")
    require(ordered(host,
                    r"samplerStartupConfigure\s*\(",
                    r"s\.deviceHook\.replace\(kDevCreateSamplerState,\s*&hookedCreateSamplerState"),
            "settings must resolve before the hook can receive sampler creates")
    require("testSamplerStartupBoundary();" in startup_rig and
            "absent factory clears stale enabled state" in startup_rig and
            "valid installed policy configures the current default-off settings" in startup_rig and
            "invalid Ops is absent-safe" in startup_rig and
            "available auto source suppresses the startup warning" in startup_rig,
            "native hook fixture exercises absent, valid and invalid production startup paths")
    try:
        independent_gates = function_body(startup_rig, "testIndependentNoRewriteGates")
        absent_through_hook = function_body(startup_rig, "testAbsentStartupForwardsThroughHook")
    except AssertionError as exc:
        return [str(exc)]
    require('{"absent policy with stale enabled settings", nullptr, 1}' in
            independent_gates and
            "state.samplerSettings.rewriteEnabled = scenario.enabled;" in
            independent_gates and
            "samplerStartupConfigure(" not in independent_gates,
            "independent null-Ops hook gate must retain stale enabled state")
    require(ordered(absent_through_hook,
                    r"samplerStartupConfigure\s*\(",
                    r"hookedCreateSamplerState\s*\(") and
            "g_startup.factoryCalls == 0" in absent_through_hook and
            "g_startup.configureCalls == 0" in absent_through_hook and
            "g_startup.readerCalls == 0" in absent_through_hook and
            "state.censusCalls == 1" in absent_through_hook and
            "state.samplerCensusPrints == 1" in absent_through_hook and
            "g_original.calls == 1" in absent_through_hook and
            "sameDescriptor(g_original.desc, desc)" in absent_through_hook and
            "g_original.outAddress == &out" in absent_through_hook and
            "got == S_FALSE" in absent_through_hook and
            "testAbsentStartupForwardsThroughHook();" in startup_rig,
            "separate absent-startup fixture must reach the real hook and check forwarding and census")
    require("input.addressU = static_cast<uint32_t>(desc->AddressU)" in hook and
            "input.addressV = static_cast<uint32_t>(desc->AddressV)" in hook and
            "input.addressW = static_cast<uint32_t>(desc->AddressW)" in hook and
            "input.comparisonFunc = static_cast<uint32_t>(desc->ComparisonFunc)" in hook and
            "input.minLod = desc->MinLOD" in hook and "input.maxLod = desc->MaxLOD" in hook and
            "input.borderColor[i] = desc->BorderColor[i]" in hook,
            "the complete descriptor policy facts must be copied into the fixed-width POD")
    require(ordered(configure,
                    r"getIntInRange\s*\(",
                    r"getString\s*\(",
                    r"getString\s*\(",
                    r"resolveTextureSamplerSettingsFromValuesV1"),
            "module owns the sampler config reads and resolution order")
    require('getIntInRange( "advanced.texture_anisotropy", 0, 0, 16)' in policy_compact and
            'getString("fix.temporal_aa", "off")' in policy_compact and
            'getString( "advanced.texture_lod_bias", temporal ? "auto" : "0")' in policy_compact,
            "module owns exact keys, anisotropy bounds and temporal-dependent default")
    require('if (_stricmp(biasKey, "auto") == 0)' in policy_compact and
            "readMultiplier(hostContext" in resolve and "log2f(multiplier)" in resolve,
            "only the auto branch asks the host for Elite's multiplier and computes log2")
    require("if (!(bias > -4.0f)) bias = -4.0f" in resolve and
            "if (!(bias < 4.0f)) bias = 4.0f" in resolve and "atof(biasKey)" in resolve,
            "manual atof and historical ordered bias clamps stay in policy")
    require("settings->rewriteEnabled = anisotropy > 0 || bias != 0.0f" in resolve,
            "the cached interest bit represents the default-off fast path")
    accepted = function_body(policy, "samplerFilterAccepted")
    require("*output = *input" in rewrite and
            "filter == kFilterLinear || filter == kFilterAnisotropic" in accepted,
            "policy starts from an exact descriptor copy and admits only the two exact filters")
    require("0x15u" in policy and "0x55u" in policy and
            "output->mipLodBias = input->mipLodBias + settings->bias" in rewrite,
            "filter constants and additive bias match the production D3D11 policy")
    require("struct TextureSamplerPolicyOpsV1" in header and
            "ConfigureTextureSamplerV1 configure;" in header and
            "RewriteTextureSamplerV1 rewrite;" in header and "void* context;" in header,
            "the private callback boundary is a versioned POD ops record with opaque context")
    require("sizeof(TextureSamplerDescV1) == 52" in header and
            "offsetof(TextureSamplerDescV1, borderColor) == 28" in header and
            "sizeof(TextureSamplerSettingsV1) == 256" in header and
            "offsetof(TextureSamplerSettingsV1, biasWhy) == 36" in header and
            "sizeof(TextureSamplerPolicyOpsV1) == 32" in header and
            "offsetof(TextureSamplerPolicyOpsV1, rewrite) == 24" in header and
            "uint32_t fileCapacity" in header,
            "POD extents, critical offsets and the host callback's fixed capacity stay pinned")
    require('auto configuredOff = configure("0", "0", "off");' in rig_raw and
            "!configuredOff.rewriteEnabled" in rig_raw,
            "the production fixture pins the cached disabled flag for explicitly off settings")
    dry_start = rig_raw.find('if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0)')
    dry_return = rig_raw.find("return 0;", dry_start)
    invalid_start = rig_raw.find('if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0)')
    self_test_start = rig_raw.find("testUnsetDefaultsAndAuto();", invalid_start)
    require(dry_start >= 0 and dry_return > dry_start and
            not any(token in rig_raw[dry_start:dry_return]
                    for token in ("Config::get", "configure(", "readMultiplier")),
            "native rig dry-run returns before config, policy or multiplier access")
    require(invalid_start > dry_return and "return 2;" in rig_raw[invalid_start:self_test_start] and
            self_test_start > invalid_start,
            "native rig rejects invalid CLI arguments before running the fixture")
    # The sampler install remains outside profile-specific hook branches. The
    # profile-dependent values are Config's existing read behavior, fixture-pinned in C++.
    require("s.deviceHook.replace(kDevCreateSamplerState, &hookedCreateSamplerState" in host,
            "the sampler hook remains profile-independent as in the host baseline")
    device_setup = function_body(host, "hookDevice")
    flat_branch = re.search(r"if\s*\(\s*runtimeFlatProfile\s*\(\s*\)\s*\)\s*\{",
                            device_setup)
    if flat_branch:
        branch_open = device_setup.find("{", flat_branch.start(), flat_branch.end())
        branch_close = block_end(device_setup, branch_open)
        sampler_setup = device_setup.find(
            "s.deviceHook.replace(kDevCreateSamplerState, &hookedCreateSamplerState")
        require(not (flat_branch.start() < sampler_setup < branch_close),
                "sampler hook install must stay outside the flat-only device hook group")
    return failures


def self_test():
    errors = inspect()
    if errors:
        raise AssertionError("\n".join(errors))
    host = HOST_PATH.read_text(encoding="utf-8")
    hook = HOOK_PATH.read_text(encoding="utf-8")
    hook_rig = HOOK_RIG_PATH.read_text(encoding="utf-8")
    # Negative mutation: removing the disabled guard must make this source
    # gate fail before the expensive native fixture or any flight is needed.
    mutated = hook.replace("s.samplerSettings.rewriteEnabled && s.samplerPolicy",
                           "s.samplerPolicy", 1)
    if mutated == hook or not inspect(hook_text=mutated):
        raise AssertionError("negative mutation escaped the disabled-fast-path pin")
    mutated = hook.replace("s.samplerSettings.rewriteEnabled && s.samplerPolicy",
                           "s.samplerSettings.rewriteEnabled", 1)
    if mutated == hook or not inspect(hook_text=mutated):
        raise AssertionError("negative mutation escaped the absent-policy pin")
    mutated = host.replace('#include "sampler_create_hook.inl"', "", 1)
    if mutated == host or not inspect(host_text=mutated):
        raise AssertionError("negative mutation escaped the production include pin")
    mutated = hook_rig.replace(
        '#include "../../src/d3d11/sampler_create_hook.inl"', "", 1)
    if mutated == hook_rig or not inspect(hook_rig_text=mutated):
        raise AssertionError("negative mutation escaped the fixture include pin")
    startup = STARTUP_PATH.read_text(encoding="utf-8")
    mutated = startup.replace("if (!factory) return false;", "if (factory) return false;", 1)
    if mutated == startup or not inspect(startup_text=mutated):
        raise AssertionError("negative mutation escaped the absent-factory boundary pin")
    mutated = startup.replace("std::memset(settingsOut, 0, sizeof(*settingsOut));", "", 1)
    if mutated == startup or not inspect(startup_text=mutated):
        raise AssertionError("negative mutation escaped the stale-settings reset pin")
    mutated = hook_rig.replace(
        '#include "../../src/d3d11/sampler_startup.h"', "", 1)
    if mutated == hook_rig or not inspect(startup_rig_text=mutated):
        raise AssertionError("negative mutation escaped the startup fixture include pin")
    mutated = hook_rig.replace(
        '{"absent policy with stale enabled settings", nullptr, 1},', "", 1)
    if mutated == hook_rig or not inspect(startup_rig_text=mutated):
        raise AssertionError("negative mutation escaped the independent null-Ops pin")
    mutated = hook_rig.replace(
        "state.samplerSettings.rewriteEnabled = scenario.enabled;",
        "state.samplerSettings.rewriteEnabled = scenario.enabled;\n"
        "        samplerStartupConfigure(nullptr, nullptr, nullptr, "
        "&state.samplerPolicy, &state.samplerSettings);", 1)
    if mutated == hook_rig or not inspect(startup_rig_text=mutated):
        raise AssertionError("negative mutation escaped the independent-gate contamination pin")
    mutated = hook_rig.replace("testAbsentStartupForwardsThroughHook();", "", 1)
    if mutated == hook_rig or not inspect(startup_rig_text=mutated):
        raise AssertionError("negative mutation escaped the absent-through-hook fixture pin")
    return 10


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="run read-only production source pins")
    args = parser.parse_args()
    if not args.self_test:
        parser.error("--self-test is required")
    count = self_test()
    print(f"performance sampler source pins: {count} negative mutations passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
