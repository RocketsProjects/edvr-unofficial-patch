// Pre-flight: what to set for a good first flight, worked out from the rig.
//
// People install the mod and start Elite with the upscaler off, a render size meant
// for a monitor, Elite's own anti-aliasing on, and sometimes a program that crashes the
// game running beside it. The setup that works is documented across several places and
// applied by nobody. Pre-flight reads the machine, says what it would change and why, and
// -- only for EDVR's own settings, only when asked -- changes it, with a copy of every
// file first and an undo that puts back one value at a time.
//
// recommendPreflight is PURE: everything it needs comes in through PreflightInputs,
// which the caller fills from the machine (app.cpp), and what comes out is a list of
// rows. That is what lets tools\installer_test drive every rule -- the vendor rule, the
// tick rule, the scope filter -- on made-up machines. planPreflight and planPreflightUndo
// are pure the same way: a Survey in, a Plan out, nothing written (the Plan is then
// shown, confirmed and applied like any other, by apply.cpp, with its stale-plan guard,
// atomic writes and rollback).
//
// Elite's own files are NOT written in this phase. The Elite rows are advice with the
// current value read from the game's files; the row text tells the user to set it in
// Elite's Graphics options.
#pragma once

#include <string>
#include <vector>

#include "detect.h"
#include "elite_settings.h"
#include "plan.h"

namespace edvr::installer {

// The dot a row gets, with the Setup tab's meanings: good (done, fine), warn (worth
// doing), bad (broken), muted (nothing to say or nothing to do).
enum class RowTone { Good, Warn, Bad, Muted };

enum class RowKind {
    EdvrWrite,    // changes EDVR's own settings file when ticked
    EliteAdvice,  // a setting in Elite's own Graphics options; read here, set by the user
    Info,         // a fact about the rig
    Warn,         // something running that should not be
};

enum class RowGroup { Rig, Fly };  // YOUR RIG / BEFORE YOU FLY

struct PreflightRow {
    std::string id;     // rig.gpu, rig.runtime, rig.headset, vr.upscaler, flat.upscaler, upscaler.none,
                        // elite.hmd_quality, elite.aa, elite.terrain_checkerboard, elite.supersampling,
                        // elite.missing, warn.rtss
    RowGroup    group = RowGroup::Fly;
    std::string label;
    std::string why;    // one line; for the rig rows, the note beside the value
    std::string now;
    std::string after;  // empty for a row with nothing to change; "already set" when it is done
    RowTone     tone = RowTone::Muted;
    RowKind     kind = RowKind::Info;

    bool checkable = false;        // a checkbox: an EDVR row whose value differs from the recommendation
    bool tickedByDefault = false;  // ... and the user never chose the value it has now
    bool alreadyGood = false;      // the value is already the recommended one (tone Good)

    // EdvrWrite rows: what the plan writes.
    std::string file;     // edvr.ini | edvr-flat.ini
    std::string key;      // fix.temporal_aa
    std::string current;  // the value the file reads now (an absent key reads as its default)
    std::string value;    // the value to set
};

// The upscaler rule (decided: DLSS on NVIDIA, FSR on AMD, XeSS on Intel), with the
// fallbacks for a payload that lacks the vendor's runtime file. `value` is a
// fix.temporal_aa value, or empty for "no recommendation" (any other vendor, or none).
struct UpscalerChoice {
    std::string value;  // dlss | fsr | xess | empty
    std::string why;    // one line for the row
};
UpscalerChoice recommendUpscaler(unsigned vendorId, bool payloadHasDlss, bool payloadHasXess);

// The names fix.temporal_aa values go by in the window: off -> Off, on -> TAA, dlss -> DLSS,
// dlaa -> DLAA, fsr -> FSR, xess -> XeSS. Anything else as written.
std::string upscalerName(const std::string& value);

struct PreflightInputs {
    std::string scope = "both";  // both | vr | flat: what the install covers

    bool       haveGpu = false;
    GpuAdapter gpu;              // the primary adapter

    OpenXrRuntime runtime;
    bool          rtssRunning = false;

    EliteSettings elite;         // elite.usable() false means "not found"

    // fix.temporal_aa as edvr.ini reads it now, and as the version installed shipped it
    // (edvr_install\edvr.ini.base). Empty means the key is not there, which reads as its
    // default, off.
    std::string vrTemporalNow, vrTemporalBase;
    // The same for edvr-flat.ini. While that file does not exist the flat profile reads
    // edvr.ini, and so does this: flatTemporalNow is then ignored for edvr.ini's value.
    bool        flatIniPresent = false;
    std::string flatTemporalNow, flatTemporalBase;

    bool payloadHasDlss = false;  // this installer carries nvngx_dlss.dll
    bool payloadHasXess = false;  // ... and libxess.dll
};

// The rows, in the order the window and --preflight show them.
std::vector<PreflightRow> recommendPreflight(const PreflightInputs& in);

// The ids of the rows ticked when nothing has been touched: EDVR rows that are checkable
// and whose current value is the one the version shipped (the user never chose it).
std::vector<std::string> defaultTicks(const std::vector<PreflightRow>& rows);

// Rows that still ask the user for something: tone Warn or Bad.
int pendingCount(const std::vector<PreflightRow>& rows);
// "Ready to fly", "1 thing to set", "3 things to set".
std::string preflightSummary(const std::vector<PreflightRow>& rows);

// The rows as plain text, for --preflight. `ticked` marks the checkboxes.
std::string preflightText(const std::vector<PreflightRow>& rows, const std::vector<std::string>& ticked);

// The plan for the ticked EDVR rows: for each settings file, a Backup then a WriteText whose
// text is the single-key edit the settings window makes (mergeIni with the key forced), then
// the install record with the new [preflight] entries. Merged with an earlier record: a
// value changed twice keeps its first `before`, so Put my old settings back goes to the
// settings the user had, not to the ones from the previous run.
//
// Ids that are not checkable EDVR rows are ignored. Nothing ticked is nothingToDo. A settings
// file that is not there is a problem and blocks the plan (install EDVR first). The plan
// writes nothing and creates nothing; its backupDir is only named.
Plan planPreflight(const Survey& survey, const std::vector<PreflightRow>& rows,
                   const std::vector<std::string>& ticked, const Options& options);

// Put the recorded values back: for each change, `before` is written only where the file
// still says `after`. A value the user changed since is left alone and said in the notes. The
// record is cleared either way. Nothing recorded is nothingToDo.
Plan planPreflightUndo(const Survey& survey, const Options& options);

}  // namespace edvr::installer
