#!/usr/bin/env python3
"""Offline source gate for installed-plugin visibility and reporting."""

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent


def self_test():
    menu = (ROOT / "src" / "d3d11" / "menu.cpp").read_text(encoding="utf-8")
    transport_h = (ROOT / "src" / "d3d11" / "plugin_admission_transport.h").read_text(encoding="utf-8")
    transport = (ROOT / "src" / "d3d11" / "plugin_admission_transport.cpp").read_text(encoding="utf-8")
    log_tool = (ROOT / "tools" / "edvr_log.py").read_text(encoding="utf-8")
    checks = [
        ("menu ownership follows generated config keys",
         "owner.ownedConfigKeys[k]" in menu and "menuRowInstalled" in menu),
        ("menu filtering uses production startup admission",
         "admission_v1::ownerAllowedForStartup(decision, index)" in menu),
        ("core-owned rows stay visible",
         "Core-owned settings have no plugin owner and remain editable." in menu),
        ("menu reads one immutable decision for page construction",
         "readDecisionForReporting(&selection)" in menu and
         "const SelectionDecision& decision" in menu),
        ("status distinguishes selected and legacy modes",
         'selected ? "selected" : "legacy"' in menu),
        ("startup report names mode, profile, set, reason and receipt state",
         '"plugin selection: mode=%s profile=%s plugins=%s reason=%s record=%s validation=0x%08X"' in transport),
        ("startup set uses generated manifest and production admission",
         "plugins::kManifest[i]" in transport and
         "admission_v1::ownerAllowedForStartup(decision, i)" in transport),
        ("log tool verifies an exact plugin set",
         "--expect-plugins" in log_tool and "verify_expected_plugins" in log_tool),
        ("log parser requires one structured startup line",
         "PLUGIN_SELECTION_RE" in log_tool and "multiple plugin selection startup lines" in log_tool),
        ("reporting decision read stays a memcpy query",
         "readDecisionForReporting" in transport_h and
         "QueryKindV1::Decision" in transport),
    ]
    failed = False
    for label, passed in checks:
        print("%s: %s" % ("PASS" if passed else "FAIL", label))
        failed |= not passed
    return 1 if failed else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if not args.self_test:
        parser.error("only --self-test is supported")
    status = self_test()
    print("plugin_visibility_test: %s" % ("FAILED" if status else "PASS"))
    return status


if __name__ == "__main__":
    sys.exit(main())
