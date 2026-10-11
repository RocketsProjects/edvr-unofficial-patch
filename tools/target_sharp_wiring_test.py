#!/usr/bin/env python3
"""Source-order pins for the private TargetSharp module boundary.

The WARP fixtures execute parity and null-Ops behavior. These pins only guard
the host seam/order and production source dependency; mutants stay in memory.
"""
import argparse
from pathlib import Path
import sys


DEFAULT_ROOT = Path(__file__).resolve().parents[1]


def body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function scope: {signature}")
    opening = source.find("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:pos]
    raise AssertionError(f"unterminated function scope: {signature}")


def ordered(text: str, *needles: str) -> None:
    positions = [text.find(item) for item in needles]
    if any(pos < 0 for pos in positions) or positions != sorted(positions):
        raise AssertionError(f"missing/reordered source fragments: {needles}")


def require(text: str, *needles: str) -> None:
    for item in needles:
        if item not in text:
            raise AssertionError(f"missing source anchor: {item}")


def check(root: Path, mutate: str = "") -> None:
    facade = (root / "src/d3d11/target_sharp.cpp").read_text(encoding="utf-8-sig")
    module = (root / "src/plugins/cockpit_visuals/target_sharp_policy.cpp").read_text(encoding="utf-8-sig")
    header = (root / "src/plugins/cockpit_visuals/target_sharp_policy.h").read_text(encoding="utf-8-sig")
    vscreen = (root / "src/d3d11/vscreen.cpp").read_text(encoding="utf-8-sig")
    build = (root / "build.bat").read_text(encoding="utf-8-sig")
    api_fixture = (root / "tools/cockpit_api_test/cockpit_api_test.cpp").read_text(encoding="utf-8-sig")
    predicate_fixture = (root / "tools/vscreen_predicate_test/vscreen_predicate_test.cpp").read_text(encoding="utf-8-sig")
    cost_fixture = (root / "tools/plugin_cost_test/plugin_cost_test.cpp").read_text(encoding="utf-8-sig")

    if mutate == "configure_absence":
        facade = facade.replace("if (!ops) { clearFast(); return; }", "if (!ops) { return; }", 1)
    elif mutate == "candidate_gate":
        facade = facade.replace("if (!wantsDraws<Observe>(observation, true)) return false;", "// bypass gate", 1)
    elif mutate == "match_hash":
        module = module.replace("return h == configuredHash;", "return true;", 1)
    elif mutate == "production_source":
        build = build.replace('    "src\\plugins\\cockpit_visuals\\target_sharp_policy.cpp"\n', "", 1)
    elif mutate == "absence_fixture":
        api_fixture = api_fixture.replace("g_targetConfigReads == 0", "true", 1)
    elif mutate == "observed_route":
        vscreen = vscreen.replace("targetSharpOnEyeDrawObserved(", "bypassedTargetSharpObserved(", 1)
    elif mutate == "setter_teardown":
        facade = facade.replace("old->shutdown(old->context, &released);", "/* skipped shutdown */", 1)
    elif mutate == "pending_bracket":
        facade = facade.replace("if (g_bracketPending) return;  // Never overwrite a PS restoration still owed.",
                                "// pending bracket ignored", 1)
    elif mutate == "end_retry":
        facade = facade.replace("if (!validFast(next) || next.engaged) return;",
                                "// clear even if restoration failed", 1)

    require(header,
            "sizeof(TargetSharpFastStateV1) == 32",
            "offsetof(TargetSharpFastStateV1, vertexShaderHash) == 24",
            "sizeof(TargetSharpPolicyOpsV1) == 56",
            "bool (*matches)(void*, ID3D11DeviceContext*, char, uint32_t, uint32_t,")

    configure = body(facade, "void targetSharpConfigure(Config& cfg)")
    ordered(configure, "const TargetSharpPolicyOpsV1* ops = activeOps();",
            "if (!ops) { clearFast(); return; }", "ops->configure(", "acceptFast(next)")
    candidate = body(facade, "bool candidate(ID3D11DeviceContext* ctx")
    ordered(candidate, "wantsDraws<Observe>(observation, true)",
            "const TargetSharpPolicyOpsV1* ops = activeOps();", "ops->matches(")
    interest = body(facade, "std::size_t targetSharpDrawInterestFilters(")
    ordered(interest, "if (!detail::g_targetSharpSharp) return 0;",
            "out[0] =", "g_fast.vertexShaderHash")
    begin = body(facade, "void targetSharpBegin(ID3D11DeviceContext* ctx)")
    ordered(begin, "if (g_bracketPending) return;",
            "if (!detail::g_targetSharpSharp || detail::g_targetSharpFailed) return;",
            "g_bracketOps = *ops;", "g_bracketPending = true;", "ops->begin(")
    require(begin, "catch (...) {", "g_bracketOps.end(",
            "if (validFast(restored) && !restored.engaged)", "clearBracket();", "throw;")
    end = body(facade, "void targetSharpEnd(ID3D11DeviceContext* ctx)")
    ordered(end, "if (!g_bracketPending) return;", "g_bracketOps.end(",
            "if (!validFast(next) || next.engaged) return;", "clearBracket()")
    clear_bracket = body(facade, "void clearBracket()")
    require(clear_bracket, "g_bracketPending = false;", "g_bracketContext = nullptr;",
            "g_bracketOps = {};")
    setter = body(facade, "bool targetSharpSetPolicyOpsV1ForTest(")
    ordered(setter, "if (g_bracketPending || g_fast.engaged) return false;",
            "if (ops && !validOps(ops)) return false;", "const TargetSharpPolicyOpsV1* old = activeOps();",
            "old->shutdown(old->context, &released);", "const bool builtin = ops && ops == targetSharpPolicyOpsV1();",
            "g_hasOverride = !builtin;", "if (ops && !builtin) g_override = *ops;")
    require(setter, "catch (...) {", "return false;")

    matcher = body(module, "bool targetSharpOnEyeDrawImpl(TargetSharpState& state,")
    ordered(matcher, "if (!sharp) return false;", "if (failed) return false;",
            "if (kind != kKind", "bindingGet(BindSlot::PsSrv0)",
            "bindingResolve(srv0, &surf)", "vScreenIsEyeSized",
            "bindingGet(BindSlot::PsSrv1)", "bindingGet(BindSlot::PsSrv2)",
            "bindingGet(BindSlot::PsSrv3)", "ctx->VSGetShader", "lookupShaderHash(vs)",
            "return h == configuredHash;")
    config = body(module, "void builtinConfigure(void* opaque, Config* cfg,")
    ordered(config, 'cfg->getString("experimental.target_indicator",',
            'if (mode == "stock")', 'else if (mode == "sharp")',
            'target_indicator \\"%s\\" is not stock or sharp')
    if "constexpr float g_sharpen = 0.25f" not in module:
        raise AssertionError("fixed TargetSharp RCAS value changed")

    require(vscreen, "targetSharpOnEyeDrawObserved(", "targetSharpOnEyeDraw(self",
            "targetSharpBegin(self)", "targetSharpEnd(self)")
    if vscreen.count("targetSharpConfigure(cfg);") != 2:
        raise AssertionError("both VScreen configure/reload sites must route through the facade")
    require(vscreen, "targetSharpDrawInterestConfigured()",
            "targetSharpDrawInterestFilters")

    plugin_start = build.find('if not exist "%OBJ%\\plugins\\cockpit_visuals"')
    plugin_end = build.find('if not exist "%OBJ%\\plugins\\on_foot_panel"', plugin_start)
    api_start = build.find(":rig_cockpit_api_test")
    api_end = build.find(":rig_draw_ladder_test", api_start)
    screen_start = build.find(":rig_vscreen_predicate_test")
    screen_end = build.find("\n:", screen_start + 1)
    if min(plugin_start, plugin_end, api_start, api_end, screen_start, screen_end) < 0:
        raise AssertionError("missing TargetSharp production/fixture build scopes")
    plugin_recipe = build[plugin_start:plugin_end]
    api_recipe = build[api_start:api_end]
    screen_recipe = build[screen_start:screen_end]
    require(plugin_recipe, '"src\\plugins\\cockpit_visuals\\target_sharp_policy.cpp"',
            '"%OBJ%\\plugins\\cockpit_visuals\\target_sharp_policy.obj"')
    require(api_recipe,
            '"src\\d3d11\\target_sharp.cpp" "src\\plugins\\cockpit_visuals\\target_sharp_policy.cpp"')
    require(screen_recipe,
            '"src\\d3d11\\target_sharp.cpp" "src\\plugins\\cockpit_visuals\\target_sharp_policy.cpp"',
            '"%OBJ%\\vscreenpredicate\\target_sharp_policy.obj"')
    require(build,
            'python "tools\\target_sharp_wiring_test.py" --dry-run',
            'python "tools\\target_sharp_wiring_test.py" --self-test')
    require(api_fixture,
            "fakeTargetSharpMatches", "fake.configure == 1", "fake.matches == 1",
            "fake.begin == 1 && fake.end == 1", "fake.shutdown == 1",
            "g_targetConfigReads == 0", "g_compileCalls == 0",
            "g_bindingGetCalls == 0 && g_bindingResolveCalls == 0",
            "observed == stock", "g_compileCalls == 2",
            "fake.shutdown == 2", "invalid Ops metadata",
            "fake.begin == 2 && fake.end == 2",
            "throwing begin/end retains its copied bracket")
    require(predicate_fixture,
            "absent TargetSharp publishes no draw interest or shader filter",
            "observed WARP route records absent gate",
            "legacy WARP route exits the absent TargetSharp gate",
            "targetSharpPolicyOpsV1())")
    require(cost_fixture,
            'leafApiSourceChecks("src/plugins/cockpit_visuals/target_sharp_policy.cpp"',
            'readSource("src/plugins/cockpit_visuals/target_sharp_policy.cpp")',
            'readSource("src/d3d11/target_sharp.cpp")',
            'functionBody(sharp, "void builtinBegin(")',
            'functionBody(sharp, "void builtinEnd(")',
            'const bool costSample = state.costSample;')


def self_test(root: Path) -> None:
    check(root)
    mutations = ("configure_absence", "candidate_gate", "match_hash",
                 "production_source", "absence_fixture", "observed_route",
                 "setter_teardown", "pending_bracket", "end_retry")
    for mutation in mutations:
        try:
            check(root, mutation)
        except AssertionError:
            continue
        raise AssertionError(f"source gate missed mutation: {mutation}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        check(args.root)
        if args.self_test:
            self_test(args.root)
            print("target_sharp_wiring_test: PASS (9 in-memory mutations)")
        else:
            print("target_sharp_wiring_test: source pins valid; dry-run wrote nothing")
        return 0
    except (AssertionError, OSError) as exc:
        print(f"target_sharp_wiring_test: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
