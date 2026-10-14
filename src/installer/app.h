// The bits both faces of the installer share: the command line, the plain-text
// description of what is in a game folder, and the elevation hand-off.
//
// The window is the product; the command line exists because an installer that
// can only be driven by hand cannot be tested, cannot be scripted by whoever
// packages EDVR for a community, and cannot tell you what it WOULD do without
// doing it (--dry-run, which is how every case in tools/installer_test was
// first checked against a real folder).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "../common/install_scope.h"
#include "plan.h"
#include "preflight.h"

namespace edvr::installer {

struct AppArgs {
    enum class Act { None, Install, Repair, Uninstall, CollectLogs, Preflight };

    Act          action = Act::None;
    std::wstring dir;                    // --dir, the game folder
    std::string  scope;                  // --scope both|vr|flat; empty means the folder's own, else both
    bool         keepSettings = true;    // --replace-settings
    bool         removeSettings = false; // --remove-settings (uninstall)
    bool         dryRun = false;         // --dry-run
    // --preflight: print the rig and what to set. With --apply, set the EDVR rows that are ticked by default (or
    // only those named by --rows); with --undo, put back what the last --apply changed.
    bool         preflightApply = false;
    bool         preflightUndo = false;
    bool         rowsGiven = false;       // --rows a,b: the row ids to set instead of the defaults
    std::vector<std::string> rows;
    bool         convertProfile = false; // --convert-profile: accepted, and no longer needed
    bool         autorun = false;        // set on the elevated relaunch: already confirmed
    bool         help = false;
    bool         badArg = false;
    std::wstring badArgText;
};

AppArgs parseArgs(int argc, wchar_t** argv);
std::string usageText();

Options optionsFor(const AppArgs& args, bool repair);

// The scope a run installs: the one asked for, else the folder's own, else both.
std::string scopeFor(const AppArgs& args, const Survey& survey);

// Elite's 3D mode, from the Settings.xml the game keeps under the Windows profile.
// Unknown when the file is missing or unreadable.
edvr::ModeRead eliteGameMode();

// What is in this folder right now, in the words the user needs: which halves
// are installed, which other mod is in the chain, whether the game's original
// runtime is safe.
std::string statusReport(const Survey& survey, const PayloadInfo& payload, const std::string& scope);

// Everything the plan will do, then everything the merge decided.
std::string planReport(const Plan& plan);

// What Pre-flight needs to know, read from this machine and this folder: the primary graphics card, the active
// OpenXR runtime, whether RivaTuner is running, Elite's graphics files, and fix.temporal_aa as each settings
// file reads it now and as the installed version shipped it. `scope` is the install scope the rows are for.
PreflightInputs collectPreflightInputs(const Survey& survey, const PayloadInfo& payload, const std::string& scope);

// A Pre-flight plan that would write a settings file the user has made read-only is refused, as the settings
// list refuses it: whoever ticked Read-only meant nothing to change the file, and applyPlan -- which clears the
// attribute for an install -- would quietly undo that. Empty when no such file is in the plan; otherwise one line
// naming it, which the caller reports as the plan's problem.
std::string readOnlySettingsProblem(const Plan& plan);

// Does this run need administrator rights for that folder?
bool needsElevationFor(const Survey& survey);

// Relaunch ourselves elevated, carrying the same arguments plus --autorun.
// Returns false if the user declined the UAC prompt.
bool relaunchElevated(const AppArgs& args);

bool isElevated();

int runConsole(const AppArgs& args);

// --preflight over rows somebody else worked out (runConsole passes the machine's; the rig passes made-up ones).
// Prints the rows, or with --apply / --undo plans, reports and carries it out; --dry-run stops after the report.
int runPreflight(const AppArgs& args, const Survey& survey, const std::vector<PreflightRow>& rows);
int runGui(HINSTANCE instance, const AppArgs& args);

}  // namespace edvr::installer
