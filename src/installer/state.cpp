#include "state.h"

#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "detect.h"
#include "../common/iniedit.h"

namespace edvr::installer {
using edvr::plugin_selection::decodePluginSelectionRecord;
using edvr::plugin_selection::isSupportedPluginSelectionRecord;
using edvr::plugin_selection::isPluginSectionName;

std::wstring stateDirPath(const std::wstring& gameDir) {
    return joinPath(gameDir, L"edvr_install");
}
std::wstring statePath(const std::wstring& gameDir) {
    return joinPath(stateDirPath(gameDir), L"state.ini");
}
std::wstring baseIniPath(const std::wstring& gameDir) {
    return joinPath(stateDirPath(gameDir), L"edvr.ini.base");
}
std::wstring backupRootPath(const std::wstring& gameDir) {
    return joinPath(gameDir, L"edvr_backup");
}

const wchar_t* settingsLeafFor(const std::string& profile) {
    return profile == "flat" ? L"edvr-flat.ini" : L"edvr.ini";
}
std::wstring settingsPathFor(const std::wstring& gameDir, const std::string& profile) {
    return joinPath(gameDir, settingsLeafFor(profile));
}

std::string utcNow() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32];
    sprintf_s(buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
              st.wMinute, st.wSecond);
    return std::string(buf);
}

std::wstring timestampName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}

InstallState parseState(const std::string& text) {
    InstallState s;
    if (text.empty()) return s;
    const IniDoc doc = iniParse(text);

    auto get = [&](const char* section, const char* key) -> std::string {
        const IniLine* l = doc.findKey(section, key);
        return l ? l->value : std::string();
    };

    s.edvrVersion = get("edvr", "version");
    s.installedUtc = get("edvr", "installed_utc");
    s.profile = get("edvr", "profile");
    if (s.profile.empty()) s.profile = "vr";
    s.descriptorSha = get("edvr", "descriptor_sha256");
    s.components = get("edvr", "components");
    s.openvrDir = fromUtf8(get("edvr", "openvr_dir"));
    s.pluginSelection = decodePluginSelectionRecord(doc);

    s.d3d11Sha = get("d3d11", "sha256");
    s.d3d11Installed = !s.d3d11Sha.empty();
    s.chainTarget = fromUtf8(get("d3d11", "chain_target"));
    s.chainMod = fromUtf8(get("d3d11", "chain_mod"));

    s.openvrSha = get("openvr", "sha256");
    s.openvrInstalled = !s.openvrSha.empty();
    s.openvrOrigName = fromUtf8(get("openvr", "orig_name"));
    s.openvrOrigSha = get("openvr", "orig_sha256");

    s.ngxSha = get("ngx", "sha256");
    s.ngxInstalled = !s.ngxSha.empty();

    s.iniSha = get("ini", "sha256");
    s.nativeGraphicsSha = get("native", "graphics_sha256");
    s.nativeRuntimeSha = get("native", "runtime_sha256");
    s.openxrLoaderSha = get("native", "loader_sha256");
    s.openxrLicenseSha = get("native", "license_sha256");
    s.nativeConfigSha = get("native", "config_sha256");
    s.nativeOriginalName = fromUtf8(get("native", "original_name"));
    s.nativeOriginalSha = get("native", "original_sha256");
    s.nativeInstalled = !s.nativeRuntimeSha.empty();

    // A record with no version is not a record; it is a file that happens to
    // parse. Everything downstream keys off `present`, so it has to mean
    // "written by this installer", not "the parse did not fail".
    s.present = !s.edvrVersion.empty() || s.d3d11Installed || s.openvrInstalled || s.nativeInstalled;
    return s;
}

InstallState readState(const std::wstring& gameDir) {
    InstallState s = parseState(readTextFile(statePath(gameDir)));
    s.hasBaseIni = fileExists(baseIniPath(gameDir));
    return s;
}

std::string serializeState(const InstallState& state) {
    std::string out;
    out += "# EDVR install record. Written by the installer; read by it on the next run.\r\n";
    out += "# Deleting this file loses nothing the game needs -- it only makes the\r\n";
    out += "# installer fall back to guessing what a previous run did.\r\n";
    out += "\r\n[edvr]\r\n";
    out += "version = " + state.edvrVersion + "\r\n";
    out += "installed_utc = " + state.installedUtc + "\r\n";
    out += "profile = " + state.profile + "\r\n";
    out += "descriptor_sha256 = " + state.descriptorSha + "\r\n";
    out += "components = " + state.components + "\r\n";
    out += "openvr_dir = " + toUtf8(state.openvrDir) + "\r\n";

    out += "\r\n[d3d11]\r\n";
    out += "sha256 = " + (state.d3d11Installed ? state.d3d11Sha : std::string()) + "\r\n";
    out += "chain_target = " + toUtf8(state.chainTarget) + "\r\n";
    out += "chain_mod = " + toUtf8(state.chainMod) + "\r\n";

    out += "\r\n[openvr]\r\n";
    out += "sha256 = " + (state.openvrInstalled ? state.openvrSha : std::string()) + "\r\n";
    out += "orig_name = " + toUtf8(state.openvrOrigName) + "\r\n";
    out += "orig_sha256 = " + state.openvrOrigSha + "\r\n";

    out += "\r\n[ngx]\r\n";
    out += "sha256 = " + (state.ngxInstalled ? state.ngxSha : std::string()) + "\r\n";

    out += "\r\n[ini]\r\n";
    out += "sha256 = " + state.iniSha + "\r\n";
    out += "\r\n[native]\r\n";
    out += "graphics_sha256 = " + state.nativeGraphicsSha + "\r\n";
    out += "runtime_sha256 = " + state.nativeRuntimeSha + "\r\n";
    out += "loader_sha256 = " + state.openxrLoaderSha + "\r\n";
    out += "license_sha256 = " + state.openxrLicenseSha + "\r\n";
    out += "config_sha256 = " + state.nativeConfigSha + "\r\n";
    out += "original_name = " + toUtf8(state.nativeOriginalName) + "\r\n";
    out += "original_sha256 = " + state.nativeOriginalSha + "\r\n";
    if (isSupportedPluginSelectionRecord(state.pluginSelection)) {
        out += "\r\n[plugins]\r\n";
        out += "selection_schema_version = 1\r\n";
        out += "catalog_schema_version = 2\r\n";
        out += "profile = " + state.pluginSelection.profile + "\r\n";
        out += "selected_ids = ";
        for (size_t i = 0; i < state.pluginSelection.selectedIds.size(); ++i) {
            if (i) out += ',';
            out += state.pluginSelection.selectedIds[i];
        }
        out += "\r\n";
    } else if (state.pluginSelection.state == PluginSelectionRecordState::Opaque) {
        if (state.pluginSelection.opaqueText.empty())
            throw std::invalid_argument("cannot serialize an empty opaque plugin selection record");
        // A broken [plugins header is a comment to IniDoc, so its following
        // keys would otherwise inherit [native] from the generated record.
        // The valid header becomes part of opaqueText on the next parse, which
        // makes subsequent rewrites byte-stable without adding more guards.
        const IniDoc opaqueDoc = iniParse(state.pluginSelection.opaqueText);
        if (opaqueDoc.lines.empty() || opaqueDoc.lines.front().kind != LineKind::Section ||
            !isPluginSectionName(opaqueDoc.lines.front().section)) {
            out += "[plugins]\r\n";
        }
        out += state.pluginSelection.opaqueText;
    } else if (state.pluginSelection.state == PluginSelectionRecordState::Supported) {
        throw std::invalid_argument("cannot serialize an invalid supported plugin selection record");
    }
    return out;
}

}  // namespace edvr::installer
