#include "preflight.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "../common/iniedit.h"
#include "probe.h"
#include "state.h"

namespace edvr::installer {
namespace {

const char* const kDot = " \xC2\xB7 ";   // " . " with U+00B7, the separator the Setup tab uses

const char* const kKeyUpscaler = "fix.temporal_aa";
const char* const kLeafVr = "edvr.ini";
const char* const kLeafFlat = "edvr-flat.ini";

// Advice that sends the user to Elite's own menu says so in these words.
const char* const kSetInElite = "Set it in Elite's Graphics options.";

std::string lowerTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    std::string out = s.substr(a, b - a);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// An absent key reads as its shipped default, which for the upscaler is off.
std::string upscalerValue(const std::string& raw) {
    const std::string v = lowerTrim(raw);
    return v.empty() ? std::string("off") : v;
}

std::string fixed2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

std::string gbText(unsigned long long bytes) {
    const unsigned long long gb = (bytes + (512ull << 20)) >> 30;   // to the nearest GiB
    return std::to_string(gb) + " GB";
}

// "change it in SteamVR" -> "Change it in SteamVR."
std::string sentence(std::string s) {
    if (s.empty()) return s;
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    const char last = s.back();
    if (last != '.' && last != '!' && last != '?') s += '.';
    return s;
}

// A value as the window names it: the upscaler's names for its key, the file's text for any other.
std::string displayValue(const std::string& key, const std::string& value) {
    return key == kKeyUpscaler ? upscalerName(value) : value;
}

bool contains(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

PreflightRow rigRow(const char* id, const char* label) {
    PreflightRow r;
    r.id = id;
    r.group = RowGroup::Rig;
    r.label = label;
    r.kind = RowKind::Info;
    return r;
}

// An Elite row whose value could not be read from the file.
PreflightRow unknownEliteRow(const char* id, const char* label) {
    PreflightRow r;
    r.id = id;
    r.label = label;
    r.kind = RowKind::Info;
    r.tone = RowTone::Muted;
    r.now = "unknown";
    r.why = "Elite's file does not say, so there is nothing to advise.";
    return r;
}

// A row that is advice for Elite's menu: Good when the value is fine, else Warn with the
// value to set.
PreflightRow eliteAdvice(const char* id, const char* label, bool fine, const std::string& now,
                         const std::string& after, const std::string& why) {
    PreflightRow r;
    r.id = id;
    r.label = label;
    r.kind = RowKind::EliteAdvice;
    r.now = now;
    r.why = why;
    r.alreadyGood = fine;
    r.tone = fine ? RowTone::Good : RowTone::Warn;
    r.after = fine ? std::string("already fine") : after;
    return r;
}

PreflightRow upscalerRow(const char* id, const char* label, const char* file,
                         const UpscalerChoice& rec, const std::string& current,
                         const std::string& shipped) {
    PreflightRow r;
    r.id = id;
    r.label = label;
    r.kind = RowKind::EdvrWrite;
    r.file = file;
    r.key = kKeyUpscaler;
    r.current = current;
    r.value = rec.value;
    r.now = upscalerName(current);
    r.why = rec.why;
    if (current == rec.value) {
        r.alreadyGood = true;
        r.tone = RowTone::Good;
        r.after = "already set";
        return r;
    }
    r.checkable = true;
    r.after = upscalerName(rec.value);
    // Ticked only while the value is one the user never chose: the one this version shipped.
    // A value they set themselves is shown, can be ticked, and starts unticked.
    r.tickedByDefault = current == shipped;
    r.tone = r.tickedByDefault ? RowTone::Warn : RowTone::Muted;
    if (!r.tickedByDefault) r.why += " (your own choice, so not ticked)";
    return r;
}

}  // namespace

UpscalerChoice recommendUpscaler(unsigned vendorId, bool payloadHasDlss, bool payloadHasXess) {
    UpscalerChoice c;
    switch (vendorId) {
        case kVendorNvidia:
            if (payloadHasDlss) {
                c.value = "dlss";
                c.why = "NVIDIA's DLSS: steadier and faster on your card.";
            } else {
                c.value = "fsr";
                c.why = "FSR (this installer carries no DLSS runtime).";
            }
            break;
        case kVendorAmd:
            c.value = "fsr";
            c.why = "AMD's FSR: steadier and faster on your card.";
            break;
        case kVendorIntel:
            if (payloadHasXess) {
                c.value = "xess";
                c.why = "Intel's XeSS: steadier and faster on your card.";
            } else {
                c.value = "fsr";
                c.why = "FSR (this installer carries no XeSS runtime).";
            }
            break;
        default:
            break;   // any other vendor, or none: no recommendation
    }
    return c;
}

std::string upscalerName(const std::string& value) {
    const std::string v = upscalerValue(value);
    if (v == "off") return "Off";
    if (v == "on") return "TAA";
    if (v == "dlss") return "DLSS";
    if (v == "dlaa") return "DLAA";
    if (v == "fsr") return "FSR";
    if (v == "xess") return "XeSS";
    return value;
}

std::vector<PreflightRow> recommendPreflight(const PreflightInputs& in) {
    std::vector<PreflightRow> rows;
    const bool vr = scopeHasVrIni(in.scope);
    const bool flat = scopeHasFlatIni(in.scope);

    // ---- YOUR RIG --------------------------------------------------------
    {
        PreflightRow r = rigRow("rig.gpu", "Graphics card");
        if (in.haveGpu) {
            r.now = toUtf8(in.gpu.name);
            if (in.gpu.dedicatedVram > 0) r.now += std::string(kDot) + gbText(in.gpu.dedicatedVram);
            r.tone = RowTone::Good;
        } else {
            r.now = "none found";
            r.why = "Windows lists no graphics card.";
            r.tone = RowTone::Muted;
        }
        rows.push_back(r);
    }
    if (vr) {
        PreflightRow r = rigRow("rig.runtime", "VR runtime");
        const OpenXrRuntimeLabel label = describeOpenXrRuntime(in.runtime.name, in.runtime.manifest);
        switch (in.runtime.state) {
            case OpenXrRuntimeState::Ok:
                r.now = label.display.empty() ? std::string("an OpenXR runtime") : label.display;
                r.why = sentence(label.hint);
                r.tone = RowTone::Good;
                break;
            case OpenXrRuntimeState::ManifestMissing:
                r.now = label.display + " (file missing)";
                r.why = sentence(label.hint);
                r.tone = RowTone::Bad;
                break;
            case OpenXrRuntimeState::NotSet:
                r.now = "not set";
                r.why = "Windows has no active runtime. " + sentence(label.hint);
                r.tone = RowTone::Bad;
                break;
        }
        rows.push_back(r);

        PreflightRow h = rigRow("rig.headset", "Headset");
        h.now = "Known after your first flight";
        h.tone = RowTone::Muted;
        rows.push_back(h);
    }

    // ---- BEFORE YOU FLY --------------------------------------------------
    const UpscalerChoice rec = recommendUpscaler(in.haveGpu ? in.gpu.vendorId : 0, in.payloadHasDlss,
                                                 in.payloadHasXess);
    const bool upscalerRecommended = !rec.value.empty();

    const std::string vrNow = upscalerValue(in.vrTemporalNow);
    const std::string vrBase = upscalerValue(in.vrTemporalBase);
    // While edvr-flat.ini is missing the flat profile reads edvr.ini, so that is the value it has.
    const std::string flatNow = in.flatIniPresent ? upscalerValue(in.flatTemporalNow) : vrNow;
    const std::string flatBase = !lowerTrim(in.flatTemporalBase).empty() ? upscalerValue(in.flatTemporalBase) : vrBase;

    if (!upscalerRecommended) {
        PreflightRow r;
        r.id = "upscaler.none";
        r.label = "Upscaler";
        r.kind = RowKind::Info;
        r.tone = RowTone::Muted;
        r.now = upscalerName(vr ? vrNow : flatNow);
        r.why = in.haveGpu ? "No recommendation for this card. Pick one in Settings if you want one."
                           : "No graphics card was found, so no upscaler is recommended.";
        rows.push_back(r);
    } else {
        const bool both = vr && flat;
        if (vr) rows.push_back(upscalerRow("vr.upscaler", both ? "Upscaler (VR)" : "Upscaler", kLeafVr, rec, vrNow, vrBase));
        if (flat)
            rows.push_back(upscalerRow("flat.upscaler", both ? "Upscaler (flat)" : "Upscaler", kLeafFlat, rec, flatNow, flatBase));
    }

    if (!in.elite.usable()) {
        PreflightRow r;
        r.id = "elite.missing";
        r.label = "Elite's graphics settings were not found";
        r.kind = RowKind::Info;
        r.tone = RowTone::Muted;
        r.why = "Start Elite once so it creates them; then this list shows what to set there.";
        rows.push_back(r);
    } else {
        const EliteSettings& e = in.elite;
        double number = 0.0;
        bool on = false;

        if (vr && upscalerRecommended) {
            if (e.hmdQuality.found && parseEliteNumber(e.hmdQuality.text, &number)) {
                // Below 0.5 is under DLSS's floor (docs\anti-aliasing.md), so it is not fine either.
                rows.push_back(eliteAdvice("elite.hmd_quality", "HMD Image Quality", number >= 0.5 && number < 0.95,
                                           fixed2(number), "0.75",
                                           std::string("Smaller render, restored by the upscaler. ") + kSetInElite));
            } else {
                rows.push_back(unknownEliteRow("elite.hmd_quality", "HMD Image Quality"));
            }
        }
        if (upscalerRecommended) {
            if (e.aaMode.found && parseEliteNumber(e.aaMode.text, &number)) {
                // The numbers behind AAMode's names are not known here, so it is On or Off and no finer.
                const bool off = number == 0.0;
                rows.push_back(eliteAdvice("elite.aa", "Anti-aliasing", off, off ? "Off" : "On", "Off",
                                           std::string("The upscaler does this job. ") + kSetInElite));
            } else {
                rows.push_back(unknownEliteRow("elite.aa", "Anti-aliasing"));
            }
        }
        if (vr) {
            if (e.terrainCheckerboard.found && parseEliteBool(e.terrainCheckerboard.text, &on)) {
                rows.push_back(eliteAdvice("elite.terrain_checkerboard", "Terrain checkerboard", !on, on ? "On" : "Off", "Off",
                                           std::string("Distant terrain shimmers in VR with it on. ") + kSetInElite));
            } else {
                rows.push_back(unknownEliteRow("elite.terrain_checkerboard", "Terrain checkerboard"));
            }
        }
        if (flat) {
            if (e.supersampling.found && parseEliteNumber(e.supersampling.text, &number)) {
                rows.push_back(eliteAdvice("elite.supersampling", "Supersampling", number <= 1.001, fixed2(number), "1.00",
                                           std::string("The flat upscaler needs Elite at native size. ") + kSetInElite));
            } else {
                rows.push_back(unknownEliteRow("elite.supersampling", "Supersampling"));
            }
        }
    }

    if (in.rtssRunning) {
        PreflightRow r;
        r.id = "warn.rtss";
        r.label = "RivaTuner is running.";
        r.why = "It has crashed Elite with EDVR: close it before you start Elite.";
        r.kind = RowKind::Warn;
        r.tone = RowTone::Warn;
        rows.push_back(r);
    }
    return rows;
}

std::vector<std::string> defaultTicks(const std::vector<PreflightRow>& rows) {
    std::vector<std::string> ids;
    for (const PreflightRow& r : rows)
        if (r.kind == RowKind::EdvrWrite && r.checkable && r.tickedByDefault) ids.push_back(r.id);
    return ids;
}

int pendingCount(const std::vector<PreflightRow>& rows) {
    int n = 0;
    for (const PreflightRow& r : rows)
        if (r.tone == RowTone::Warn || r.tone == RowTone::Bad) ++n;
    return n;
}

std::string preflightSummary(const std::vector<PreflightRow>& rows) {
    const int n = pendingCount(rows);
    if (n == 0) return "Ready to fly";
    return std::to_string(n) + (n == 1 ? " thing to set" : " things to set");
}

std::string preflightText(const std::vector<PreflightRow>& rows, const std::vector<std::string>& ticked) {
    std::string out;
    bool inFly = false;
    out += "YOUR RIG\r\n";
    for (const PreflightRow& r : rows) {
        if (r.group == RowGroup::Fly && !inFly) {
            inFly = true;
            out += "\r\nBEFORE YOU FLY\r\n";
        }
        std::string mark = "    ";
        if (r.kind == RowKind::EdvrWrite) mark = r.checkable ? (contains(ticked, r.id) ? "[x] " : "[ ] ") : " ok ";
        else if (r.kind == RowKind::EliteAdvice) mark = r.alreadyGood ? " ok " : " -> ";
        else if (r.kind == RowKind::Warn) mark = " !  ";
        out += "  " + mark + r.label;
        if (!r.now.empty()) out += "   " + r.now;
        if (!r.after.empty()) out += (r.alreadyGood ? "  (" + r.after + ")" : "  ->  " + r.after);
        out += "\r\n";
        if (!r.why.empty()) out += "        " + r.why + "\r\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// the plan
// ---------------------------------------------------------------------------

namespace {

std::wstring settingsPathForLeaf(const std::wstring& gameDir, const std::string& leaf) {
    return settingsPathFor(gameDir, leaf == kLeafFlat ? "flat" : "vr");
}

// The text a settings file starts from: its own, or -- for edvr-flat.ini while it does not
// exist -- the edvr.ini the flat profile reads in its place (SettingsModel starts the flat
// file from it on the first change). `from` is the file the text came from, `seeded` says
// it is not the target's own.
struct Source {
    std::string  text;
    std::wstring from;
    bool         targetPresent = false;
    bool         seeded = false;
};

Source sourceFor(const Survey& s, const std::string& leaf) {
    Source src;
    if (leaf == kLeafFlat) {
        if (s.flatIniPresent) {
            src.text = s.flatIniText;
            src.from = settingsPathFor(s.game.dir, "flat");
            src.targetPresent = true;
        } else if (s.iniPresent) {
            src.text = s.iniText;
            src.from = settingsPathFor(s.game.dir, "vr");
            src.seeded = true;
        }
    } else if (s.iniPresent) {
        src.text = s.iniText;
        src.from = settingsPathFor(s.game.dir, "vr");
        src.targetPresent = true;
    }
    return src;
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

// One file's edit: the same single-key merge the settings window runs, writing into the line
// where the setting already lives and touching nothing else.
std::string editedText(const std::string& source, const Pairs& pairs) {
    return mergeIni(source, source, &source, pairs, nullptr);
}

void addBackupAndWrite(Plan* plan, const Survey& s, const std::string& leaf, const Source& src,
                       const std::string& updated, const std::string& why) {
    const std::wstring target = settingsPathForLeaf(s.game.dir, leaf);
    const std::string sha = sha256Bytes(src.text.data(), src.text.size());
    if (src.targetPresent) {
        Step backup;
        backup.action = Action::Backup;
        backup.from = target;
        backup.to = joinPath(plan->backupDir, fromUtf8(leaf));
        backup.expectSha = sha;
        backup.required = true;
        backup.why = "your " + leaf + ", before it is changed";
        plan->steps.push_back(backup);
    }
    Step write;
    write.action = Action::WriteText;
    write.from = src.from;      // the file the text was made from: the stale-plan guard checks it still is
    write.to = target;
    write.text = updated;
    write.expectSha = sha;
    write.why = why;
    plan->steps.push_back(write);
}

// The steps that open a plan (the backup folder) and close it (the record), added once the
// file steps are known.
void frame(Plan* plan, const Survey& s, bool anyBackup) {
    std::vector<Step> opening;
    if (anyBackup) {
        Step mk;
        mk.action = Action::MakeDir;
        mk.to = plan->backupDir;
        mk.why = "keeps a copy of everything changed";
        opening.push_back(mk);
    }
    Step mkState;
    mkState.action = Action::MakeDir;
    mkState.to = stateDirPath(s.game.dir);
    mkState.why = "holds the install record";
    opening.push_back(mkState);
    plan->steps.insert(plan->steps.begin(), opening.begin(), opening.end());

    Step rec;
    rec.action = Action::WriteText;
    if (!s.stateSha.empty()) {
        rec.from = statePath(s.game.dir);
        rec.expectSha = s.stateSha;
    }
    rec.to = statePath(s.game.dir);
    rec.text = serializeState(plan->nextState);
    rec.why = "records what was changed, so it can be put back";
    plan->steps.push_back(rec);
}

}  // namespace

Plan planPreflight(const Survey& s, const std::vector<PreflightRow>& rows,
                   const std::vector<std::string>& ticked, const Options& o) {
    Plan plan;
    plan.backupDir = joinPath(backupRootPath(s.game.dir),
                              L"preflight-" + (o.backupStamp.empty() ? std::wstring(L"backup") : o.backupStamp));
    plan.nextState = s.state;

    std::vector<const PreflightRow*> chosen;
    for (const PreflightRow& r : rows) {
        if (r.kind != RowKind::EdvrWrite || !r.checkable || !contains(ticked, r.id)) continue;
        chosen.push_back(&r);
    }
    if (chosen.empty()) {
        plan.nothingToDo = true;
        plan.notes.push_back("Nothing is ticked that needs setting. Nothing to do.");
        return plan;
    }

    bool anyBackup = false;
    std::vector<PreflightChange> made;
    for (const char* leaf : {kLeafVr, kLeafFlat}) {
        Pairs pairs;
        std::vector<const PreflightRow*> mine;
        for (const PreflightRow* r : chosen) {
            if (r->file != leaf) continue;
            mine.push_back(r);
        }
        if (mine.empty()) continue;

        const Source src = sourceFor(s, leaf);
        if (src.text.empty()) {
            // The settings window says the same: there is nothing to edit until the install has written the file.
            plan.problems.push_back(std::string(leaf) + " is not there yet -- install EDVR into this folder first.");
            plan.blocked = true;
            continue;
        }
        std::vector<PreflightChange> fileChanges;
        for (const PreflightRow* r : mine) {
            // What the file says now, read again from the text the edit starts from.
            const std::string before = lowerTrim(iniValue(src.text, r->key, r->current));
            if (before == r->value) {
                plan.notes.push_back(r->key + " is already " + r->value + " in " + leaf + ". Left alone.");
                continue;
            }
            pairs.emplace_back(r->key, r->value);
            fileChanges.push_back({leaf, r->key, before.empty() ? r->current : before, r->value});
        }
        if (pairs.empty()) continue;

        const std::string updated = editedText(src.text, pairs);
        bool took = true;
        for (const auto& kv : pairs)
            if (lowerTrim(iniValue(updated, kv.first)) != kv.second) took = false;
        if (!took || updated == src.text) {
            plan.problems.push_back(std::string("The edit to ") + leaf + " did not take, so it was not written.");
            plan.blocked = true;
            continue;
        }

        addBackupAndWrite(&plan, s, leaf, src, updated, std::string("sets ") + pairs.front().first + " in " + leaf);
        anyBackup = anyBackup || src.targetPresent;
        for (size_t i = 0; i < fileChanges.size(); ++i) {
            const PreflightRow* r = nullptr;
            for (const PreflightRow* m : mine)
                if (m->key == fileChanges[i].key) r = m;
            plan.notes.push_back("Set " + (r ? r->label : fileChanges[i].key) + " to " +
                                 displayValue(fileChanges[i].key, fileChanges[i].after) + " in " + leaf + " (it reads " +
                                 displayValue(fileChanges[i].key, fileChanges[i].before) + " now).");
            made.push_back(fileChanges[i]);
        }
        if (src.seeded)
            plan.notes.push_back(std::string(leaf) + " does not exist yet: it is started from your edvr.ini with this one change, and edvr.ini is left exactly as it is.");
    }

    if (plan.blocked) {
        plan.steps.clear();
        return plan;
    }
    if (made.empty()) {
        plan.nothingToDo = true;
        plan.notes.push_back("Everything ticked is already set. Nothing to do.");
        return plan;
    }

    // The record. A value changed before keeps the first `before` it was given: Put my old settings
    // back then goes to the settings the user had, not to the ones the previous run left.
    PreflightRecord& record = plan.nextState.preflight;
    for (const PreflightChange& c : made) {
        bool merged = false;
        for (PreflightChange& old : record.changes) {
            if (old.file == c.file && old.key == c.key) {
                old.after = c.after;
                merged = true;
            }
        }
        if (!merged) record.changes.push_back(c);
    }
    record.appliedUtc = o.nowUtc;

    frame(&plan, s, anyBackup);
    plan.notes.push_back("Elite's own settings are not changed here; the rows that name them say what to set in Elite's Graphics options.");
    return plan;
}

Plan planPreflightUndo(const Survey& s, const Options& o) {
    Plan plan;
    plan.backupDir = joinPath(backupRootPath(s.game.dir),
                              L"preflight-" + (o.backupStamp.empty() ? std::wstring(L"backup") : o.backupStamp));
    plan.nextState = s.state;

    const PreflightRecord& record = s.state.preflight;
    if (!record.present()) {
        plan.nothingToDo = true;
        plan.notes.push_back("There is nothing to put back: Set up for me has not changed anything here.");
        return plan;
    }

    bool anyBackup = false;
    for (const char* leaf : {kLeafVr, kLeafFlat}) {
        // Only the file itself: a missing edvr-flat.ini is not seeded to undo something it never held.
        const bool flatLeaf = std::string(leaf) == kLeafFlat;
        const bool present = flatLeaf ? s.flatIniPresent : s.iniPresent;
        const std::string& text = flatLeaf ? s.flatIniText : s.iniText;

        Pairs pairs;
        for (const PreflightChange& c : record.changes) {
            if (c.file != leaf) continue;
            if (!present || text.empty()) {
                plan.notes.push_back(std::string(leaf) + " is not there any more, so " + c.key + " was left alone.");
                continue;
            }
            const std::string now = lowerTrim(iniValue(text, c.key, "\x01"));
            if (now == c.after) {
                pairs.emplace_back(c.key, c.before);
                plan.notes.push_back("Put " + c.key + " back to " + c.before + " in " + leaf + " (it reads " + c.after + " now).");
            } else if (now == c.before) {
                plan.notes.push_back(c.key + " in " + std::string(leaf) + " already reads " + c.before + ". Left alone.");
            } else {
                plan.notes.push_back("Left " + c.key + " alone in " + std::string(leaf) + ": it reads " +
                                     (now == "\x01" ? std::string("nothing") : now) + " now, not the " + c.after +
                                     " that Set up for me wrote, so you changed it since.");
            }
        }
        if (pairs.empty()) continue;

        Source src;
        src.text = text;
        src.from = settingsPathFor(s.game.dir, flatLeaf ? "flat" : "vr");
        src.targetPresent = true;
        const std::string updated = editedText(text, pairs);
        bool took = true;
        for (const auto& kv : pairs)
            if (lowerTrim(iniValue(updated, kv.first)) != lowerTrim(kv.second)) took = false;
        if (!took || updated == text) {
            plan.problems.push_back(std::string("The edit to ") + leaf + " did not take, so it was not written.");
            plan.blocked = true;
            continue;
        }
        addBackupAndWrite(&plan, s, leaf, src, updated, std::string("puts back what Set up for me changed in ") + leaf);
        anyBackup = true;
    }
    for (const PreflightChange& c : record.changes) {
        if (c.file != kLeafVr && c.file != kLeafFlat)
            plan.notes.push_back("The record names " + c.file + ", which is not a settings file EDVR keeps, so " + c.key + " was left alone.");
    }

    if (plan.blocked) {
        plan.steps.clear();
        return plan;
    }
    // Cleared whether or not anything could be put back: what is left was changed by the user, and a
    // record that kept naming it would offer to undo their own edit.
    plan.nextState.preflight = PreflightRecord();
    frame(&plan, s, anyBackup);
    return plan;
}

}  // namespace edvr::installer
