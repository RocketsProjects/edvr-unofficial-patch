#include "elite_settings.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdlib>
#include <vector>

#include "detect.h"
#include "../common/elite_graphics_folder.h"
#include "../d3d11/terrain_checkerboard_reader.h"

namespace edvr::installer {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Where the next <!-- ... --> starts and ends, from `from`. false when there is none; an
// unterminated one runs to the end of the text, as a reader would take it.
bool nextComment(const std::string& xml, size_t from, size_t* begin, size_t* end) {
    const size_t b = xml.find("<!--", from);
    if (b == std::string::npos) return false;
    const size_t e = xml.find("-->", b + 4);
    *begin = b;
    *end = e == std::string::npos ? xml.size() : e + 3;
    return true;
}

struct Scan {
    int         count = 0;      // <tag> opens outside comments
    bool        closed = true;  // the first of them is closed by </tag> straight after its text
    std::string text;           // the first one's text, trimmed
};

Scan scan(const std::string& xml, const std::string& tag) {
    Scan result;
    if (tag.empty()) return result;
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    size_t commentBegin = 0, commentEnd = 0;
    bool haveComment = nextComment(xml, 0, &commentBegin, &commentEnd);
    size_t i = 0;
    while (i < xml.size()) {
        const size_t at = xml.find(open, i);
        if (at == std::string::npos) break;
        // A comment that ends before this open is behind us; one that holds it hides it.
        while (haveComment && commentEnd <= at) haveComment = nextComment(xml, commentEnd, &commentBegin, &commentEnd);
        if (haveComment && commentBegin <= at && at < commentEnd) {
            i = commentEnd;
            continue;
        }
        ++result.count;
        const size_t from = at + open.size();
        const size_t lt = xml.find('<', from);
        const bool thisClosed = lt != std::string::npos && xml.compare(lt, close.size(), close) == 0;
        if (result.count == 1) {
            result.closed = thisClosed;
            if (thisClosed) {
                size_t a = from, b = lt;
                while (a < b && isSpace(xml[a])) ++a;
                while (b > a && isSpace(xml[b - 1])) --b;
                result.text = xml.substr(a, b - a);
            }
        }
        i = thisClosed ? lt + close.size() : from;
    }
    return result;
}

std::wstring widen(const std::string& s) { return fromUtf8(s); }

}  // namespace

std::optional<std::string> elementText(const std::string& xml, const std::string& tag) {
    const Scan s = scan(xml, tag);
    if (s.count != 1 || !s.closed) return std::nullopt;
    return s.text;
}

int elementCount(const std::string& xml, const std::string& tag) { return scan(xml, tag).count; }

bool parseEliteBool(const std::string& text, bool* on) {
    std::string t;
    for (char c : text) t += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (t == "true" || t == "1") {
        if (on) *on = true;
        return true;
    }
    if (t == "false" || t == "0") {
        if (on) *on = false;
        return true;
    }
    return false;
}

bool parseEliteNumber(const std::string& text, double* out) {
    if (text.empty()) return false;
    const char* begin = text.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin || *end != '\0') return false;
    // strtod takes "inf", "nan" and hex; none of them is a setting.
    for (char c : text)
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) return false;
    if (out) *out = v;
    return true;
}

std::wstring eliteGraphicsFolderNow() {
    PWSTR path = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        base = path;
        CoTaskMemFree(path);
    } else {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) base = env;
    }
    return edvr::eliteGraphicsFolderUnder(base);
}

EliteSettings readEliteSettings(const std::wstring& gameDir) {
    const std::wstring folder = eliteGraphicsFolderNow();
    if (folder.empty()) {
        EliteSettings none;
        none.why = "Windows would not say where your profile's local app data is";
        return none;
    }
    return readEliteSettingsFrom(folder, gameDir);
}

EliteSettings readEliteSettingsFrom(const std::wstring& folder, const std::wstring& gameDir) {
    EliteSettings out;
    out.folder = folder;

    const edvr::EliteFolderListing listing = edvr::flatListEliteGraphics(folder);
    if (!listing.folder) {
        out.why = "Elite's graphics settings folder was not found";
        return out;
    }
    out.folderFound = true;

    std::string settings;
    if (!listing.settings || !edvr::flatReadSmallFile(joinPath(folder, L"Settings.xml"), &settings)) {
        out.why = "Settings.xml is missing or unreadable, so the active preset is not known";
        return out;
    }
    // Settings.xml is asked for the preset's name and nothing else: it carries tags of the
    // same names as the values below, and they are not the ones the game reads.
    const std::optional<std::string> preset = elementText(settings, "PresetName");
    if (!preset || preset->empty()) {
        out.why = "Settings.xml names no single preset, so the active preset is not known";
        return out;
    }
    out.presetKnown = true;
    out.preset = *preset;

    std::string file;
    std::wstring path;
    if (_stricmp(preset->c_str(), "Custom") == 0) {
        // Only names of exactly the Custom.<major>.<minor>.fxcfg shape are the Custom
        // preset's; the picker's fallback for an unversioned name would choose a lookalike
        // the game never reads, so nothing unversioned is handed to it.
        std::vector<edvr::EliteFxcfg> versioned;
        for (const edvr::EliteFxcfg& f : listing.fxcfg) {
            unsigned major = 0, minor = 0;
            if (edvr::flatCustomFxcfgVersion(f.name, &major, &minor)) versioned.push_back(f);
        }
        const int pick = edvr::flatPickCustomFxcfg(versioned);
        if (pick < 0) {
            out.why = "the preset is Custom but the folder has no Custom.<major>.<minor>.fxcfg";
            return out;
        }
        file = versioned[static_cast<size_t>(pick)].name;
        path = joinPath(folder, widen(file));
    } else {
        if (!edvr::tcn::plausibleStockPreset(*preset)) {
            out.why = "the preset " + *preset + " is not Custom or a stock preset, so no file is read for it";
            return out;
        }
        file = "OptionDefaults\\" + *preset + ".fxcfg";
        if (gameDir.empty()) {
            out.why = "no game folder to find " + file + " in";
            return out;
        }
        path = joinPath(joinPath(gameDir, L"OptionDefaults"), widen(*preset + ".fxcfg"));
    }

    std::string text;
    if (!edvr::flatReadSmallFile(path, &text)) {
        out.why = file + " could not be read";
        return out;
    }
    out.file = file;

    auto take = [&](const char* tag, EliteValue* value) {
        const std::optional<std::string> t = elementText(text, tag);
        if (!t || t->empty()) return;
        value->found = true;
        value->text = *t;
        value->file = file;
    };
    take("HMDRenderTargetMultiplier", &out.hmdQuality);
    take("SSAAMultiplier", &out.supersampling);
    take("AAMode", &out.aaMode);
    take("TerrainCheckerboardRenderingEnabled", &out.terrainCheckerboard);
    return out;
}

}  // namespace edvr::installer
