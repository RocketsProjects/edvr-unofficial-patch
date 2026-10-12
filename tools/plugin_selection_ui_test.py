#!/usr/bin/env python3
"""Source and mutation gates for installer plugin selection wiring."""

import argparse
from pathlib import Path
import sys


def source_map(root):
    paths = (
        "build.bat", "src/installer/app.cpp", "src/installer/app.h",
        "src/installer/gui.cpp", "src/installer/plan.cpp", "src/installer/plan.h",
        "src/installer/plugin_components.cpp", "src/installer/plugin_components.h",
        "tools/installer_test/installer_test.cpp",
    )
    return {name: (root / name).read_text(encoding="utf-8") for name in paths}


def failures(files):
    gui = files["src/installer/gui.cpp"]
    choose_start = gui.find("bool choosePlugins(HWND owner)")
    choose_end = gui.find("// One zip on the Desktop", choose_start)
    choose = gui[choose_start:choose_end] if choose_start >= 0 and choose_end > choose_start else ""
    cancel_at = choose.find("if (answer == IDCANCEL) return false;")
    selection_writes = [choose.find(token) for token in (
        "g.args.pluginSelectionSpecified = true;", "g.args.noPlugins = ids.empty();",
        "g.args.pluginIds = std::move(ids);")]
    checks = {
        "shared production and fixture component source":
            '"src\\installer\\plugin_components.cpp"' in files["build.bat"] and
            '"src\\installer\\plugin_components.cpp"' in files["build.bat"].split(":rig_installer_test", 1)[-1],
        "source test and mutant gates": files["build.bat"].count('tools\\plugin_selection_ui_test.py') == 2,
        "whole-profile readiness gate": 'if (!ready)' in files["src/installer/plugin_components.cpp"] and
            'profileSelectionReady' in files["src/installer/plugin_components.cpp"],
        "hard dependency closure": 'dep.kind != 1' in files["src/installer/plugin_components.cpp"] and
            'addHard(static_cast<std::uint32_t>(depIndex))' in files["src/installer/plugin_components.cpp"],
        "conflict and advisory soft dependency policy": 'row.conflictCount' in files["src/installer/plugin_components.cpp"] and
            'dep.kind != 2' in files["src/installer/plugin_components.cpp"] and
            'softNotices.emplace_back' in files["src/installer/plugin_components.cpp"],
        "repeatable strict CLI forms": 'arg == L"--plugin"' in files["src/installer/app.cpp"] and
            'arg.rfind(L"--plugin=", 0) == 0' in files["src/installer/app.cpp"] and
            'argv[i + 1][0] == L\'-\'' in files["src/installer/app.cpp"],
        "explicit empty and mixed selection rejection": 'L"--no-plugins"' in files["src/installer/app.cpp"] and
            'a.noPlugins && !a.pluginIds.empty()' in files["src/installer/app.cpp"],
        "validated elevation forwarding": 'pluginSelectionValidated' in files["src/installer/app.cpp"] and
            '!args.pluginSelectionValidated' in files["src/installer/app.cpp"],
        "GUI lists catalog names and selection choices": all(token in files["src/installer/gui.cpp"] for token in
            ("row.name", "row.description", "Recommended", "Default", "Installed", "TaskDialogIndirect",
             "(row.recommendedProfileMask & bit)", "(row.defaultProfileMask & bit)")),
        "GUI recommendation/default tags use the active profile":
            files["src/installer/gui.cpp"].count("(row.recommendedProfileMask & bit)") >= 2 and
            files["src/installer/gui.cpp"].count("(row.defaultProfileMask & bit)") >= 2,
        "selection-only plan writes metadata": 'changed = selectionChanged' in files["src/installer/plan.cpp"] and
            'next.pluginSelection = desiredSelection' in files["src/installer/plan.cpp"] and
            'if (o.repair || s.baseIniText != p.iniText)' in files["src/installer/plan.cpp"],
        "legacy absent receipt is preserved except on fresh install or conversion":
            '(!s.state.present || conversion)' in files["src/installer/plan.cpp"],
        "plan reports complete resulting selection with catalog names":
            'Resulting plugin selection' in files["src/installer/plan.cpp"] and
            'remains bound to the ' in files["src/installer/plan.cpp"] and
            'pluginSelectionNames(catalog, selectedProfile' in files["src/installer/plan.cpp"] and
            'if (ids.empty()) return "none";' in files["src/installer/plan.cpp"],
        "typed catalog injection with production wrapper": 'plugin_components::compiledCatalog()' in files["src/installer/plan.cpp"] and
            'plugin_components::CatalogView catalog' in files["src/installer/plan.h"] and
            '#include "plugin_manifest.inc"' in files["src/installer/plugin_components.cpp"],
        "GUI reopens pending choices and distinguishes them from installed plugins":
            'if (pendingChoice)' in files["src/installer/gui.cpp"] and
            'installed = g.args.pluginIds;' in files["src/installer/gui.cpp"] and
            '  [Selected]' in files["src/installer/gui.cpp"] and
            'Selected for this install.' in files["src/installer/gui.cpp"],
        "per-plugin chooser cancel returns before any selection commit":
            'MB_YESNOCANCEL | MB_ICONQUESTION' in choose and
            choose.find('for (const auto& row : rows)') < cancel_at < min(
                (position for position in selection_writes if position >= 0), default=-1),
        "uninstall strips selection flags before any elevated relaunch":
            'if (action == AppArgs::Act::Uninstall)' in files["src/installer/gui.cpp"] and
            'args.pluginSelectionSpecified = false;' in files["src/installer/gui.cpp"] and
            'args.noPlugins = false;' in files["src/installer/gui.cpp"] and
            'args.pluginIds.clear();' in files["src/installer/gui.cpp"],
    }
    return [name for name, ok in checks.items() if not ok]


def self_test(root):
    bad = failures(source_map(root))
    if bad:
        for item in bad:
            print("FAIL:", item)
        return 1
    print("plugin_selection_ui_test: installer selection source checks passed")
    return 0


def mutant_test(root):
    original = source_map(root)
    cases = (
        ("profile readiness", "if (!ready) {", "if (ready) {", "src/installer/plugin_components.cpp"),
        ("hard dependency closure", "if (dep.kind != 1) continue;", "if (dep.kind == 1) continue;", "src/installer/plugin_components.cpp"),
        ("conflict rejection", "row.conflictCount", "row.rigCount", "src/installer/plugin_components.cpp"),
        ("missing CLI value", "argv[i + 1][0] == L'-'", "argv[i + 1][0] == L'+'", "src/installer/app.cpp"),
        ("elevation validation", "!args.pluginSelectionValidated", "args.pluginSelectionValidated", "src/installer/app.cpp"),
        ("selection-only metadata write", "changed = selectionChanged", "changed = false", "src/installer/plan.cpp"),
        ("unchanged base defaults", "if (o.repair || s.baseIniText != p.iniText)", "if (true)", "src/installer/plan.cpp"),
        ("legacy absent receipt", "(!s.state.present || conversion)", "true", "src/installer/plan.cpp"),
        ("selection result names", "pluginSelectionNames(catalog, selectedProfile", "selectedProfile", "src/installer/plan.cpp"),
        ("profile-filtered recommendation label", "(row.recommendedProfileMask & bit)", "row.recommendedProfileMask", "src/installer/gui.cpp"),
        ("pending GUI choice", "installed = g.args.pluginIds;", "installed.clear();", "src/installer/gui.cpp"),
        ("uninstall elevated flags", "args.pluginSelectionSpecified = false;", "args.pluginSelectionSpecified = true;", "src/installer/gui.cpp"),
    )
    for name, before, after, path in cases:
        files = dict(original)
        if before not in files[path]:
            print("FAIL: mutant anchor missing:", name)
            return 1
        files[path] = files[path].replace(before, after, 1)
        if not failures(files):
            print("FAIL: source checks accepted mutant:", name)
            return 1
    cancel_mutant = dict(original)
    gui_path = "src/installer/gui.cpp"
    cancel_check = "if (answer == IDCANCEL) return false;\n"
    final_write = "g.args.pluginIds = std::move(ids);"
    if cancel_check not in cancel_mutant[gui_path] or final_write not in cancel_mutant[gui_path]:
        print("FAIL: mutant anchor missing: cancel ordering")
        return 1
    cancel_mutant[gui_path] = cancel_mutant[gui_path].replace(cancel_check, "", 1)
    cancel_mutant[gui_path] = cancel_mutant[gui_path].replace(
        final_write, final_write + "\n            " + cancel_check.rstrip(), 1)
    if not failures(cancel_mutant):
        print("FAIL: source checks accepted mutant: cancel after selection commit")
        return 1
    print("plugin_selection_ui_test: all source mutants rejected")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--self-test", action="store_true")
    group.add_argument("--mutants", action="store_true")
    args = parser.parse_args(argv)
    return self_test(args.root) if args.self_test else mutant_test(args.root)


if __name__ == "__main__":
    sys.exit(main())
