// Elite's own graphics settings, read for Pre-flight.
//
// They live under the Windows profile, not the game folder (see
// src/common/elite_graphics_folder.h):
//   %LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics
//     Settings.xml                 <PresetName> names the active preset
//     Custom.<major>.<minor>.fxcfg the Custom preset's values, one file per game version
//   <game folder>\OptionDefaults\<Preset>.fxcfg
//                                  a stock preset's values (Low ... VRUltra)
//
// Which file holds the values of the ACTIVE preset is decided the way the runtime
// decides it for the terrain checkerboard notice and the flat F8 panel: Custom is the
// highest Custom.<major>.<minor>.fxcfg, a stock preset is its OptionDefaults file.
// The folder listing, that pick, the whole-file read and the stock-name test are those
// readers' own helpers (src/d3d11/flat_elite_settings.h and
// terrain_checkerboard_reader.h), included and not copied: a second picker that drifted
// from them would read a file the game never does. tools\installer_test checks the two
// agree on fixture folders.
//
// READ ONLY in this phase. "Not found" is a normal outcome (Proton and Wine put the
// folder elsewhere, and a fresh install has never started Elite), never an error.
//
// elementText is the one piece of XML handling here, and it is deliberately narrow: it
// answers only when the element occurs exactly once. Writing Elite's files (a later
// phase) will build its edit on that rule -- an element that is missing or repeated is a
// file whose shape EDVR does not know, and a file like that is left alone.
#pragma once

#include <optional>
#include <string>

namespace edvr::installer {

// The text of <tag>...</tag>, trimmed of white space, when the element occurs exactly
// once outside comments (<!-- ... -->) and is closed by </tag> straight after its text.
// std::nullopt when it is absent, repeated, or not closed. Only the plain form <tag> is
// an element here: <tag attr="x"> is not, and nor is a commented-out one. Pure.
std::optional<std::string> elementText(const std::string& xml, const std::string& tag);

// How many <tag> elements the text holds outside comments. Pure.
int elementCount(const std::string& xml, const std::string& tag);

// "true"/"1" or "false"/"0", in any case. False when it is neither. Pure.
bool parseEliteBool(const std::string& text, bool* on);
// A whole decimal number ("0.750000", "1"), nothing after it. Pure.
bool parseEliteNumber(const std::string& text, double* out);

// One value, and where it came from.
struct EliteValue {
    bool        found = false;  // the element was there exactly once, with text
    std::string text;           // as written, trimmed
    std::string file;           // "Custom.4.4.fxcfg" or "OptionDefaults\VRHigh.fxcfg"
};

struct EliteSettings {
    bool         folderFound = false;  // Options\Graphics exists
    std::wstring folder;               // ... and this is it (empty if LocalAppData could not be resolved)
    bool         presetKnown = false;  // Settings.xml gave a PresetName
    std::string  preset;               // as written: Custom, VRHigh ...
    std::string  file;                 // the preset file the values were read from; empty when none was
    std::string  why;                  // when `file` is empty: the reason, in words

    EliteValue hmdQuality;           // HMDRenderTargetMultiplier
    EliteValue supersampling;        // SSAAMultiplier
    EliteValue aaMode;               // AAMode
    EliteValue terrainCheckerboard;  // TerrainCheckerboardRenderingEnabled

    // A preset file was found and read. Individual values can still be missing from it.
    bool usable() const { return !file.empty(); }
};

// The Options\Graphics folder under this user's LocalAppData (the shell's answer, the
// environment variable as the fallback). Empty if neither gives one.
std::wstring eliteGraphicsFolderNow();

// Settings for the game in `gameDir`, from this user's profile.
EliteSettings readEliteSettings(const std::wstring& gameDir);
// The same over a graphics folder somebody names (a fixture folder in the rig).
EliteSettings readEliteSettingsFrom(const std::wstring& graphicsFolder, const std::wstring& gameDir);

}  // namespace edvr::installer
