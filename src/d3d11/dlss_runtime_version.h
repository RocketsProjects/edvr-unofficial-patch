#pragma once
// The DLSS runtime's own file version, as a log line (issue 90: people swap nvngx_dlss.dll with DLSS Swapper, and a log should say
// which DLSS it came from without asking). Pure: no Win32, no NGX. dlaa.cpp finds the module and reads VS_FIXEDFILEINFO; this
// turns what it found into text, so the formatting is held by a rig (tools\dlss_runtime_version_test).
#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {

struct DlssRuntimeVersion {
    bool     read = false;      // the file carried a version resource that was read
    uint16_t major = 0, minor = 0, build = 0, revision = 0;
};

// VS_FIXEDFILEINFO's dwFileVersionMS / dwFileVersionLS: (major << 16 | minor) and (build << 16 | revision).
inline DlssRuntimeVersion dlssVersionFromFixed(uint32_t fileVersionMS, uint32_t fileVersionLS) {
    DlssRuntimeVersion v;
    v.read = true;
    v.major = static_cast<uint16_t>(fileVersionMS >> 16);
    v.minor = static_cast<uint16_t>(fileVersionMS & 0xFFFFu);
    v.build = static_cast<uint16_t>(fileVersionLS >> 16);
    v.revision = static_cast<uint16_t>(fileVersionLS & 0xFFFFu);
    return v;
}

// "310.9.16.0": the four fields as the file's own properties page prints them; "unknown" when none was read.
inline std::string dlssVersionText(const DlssRuntimeVersion& v) {
    if (!v.read) return "unknown";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(v.major), static_cast<unsigned>(v.minor),
                  static_cast<unsigned>(v.build), static_cast<unsigned>(v.revision));
    return buf;
}

// Where the file the version came from was found.
enum class DlssRuntimeSource {
    kLoaded,       // the module nvngx_dlss.dll mapped in this process (the one NGX is running)
    kBesideGame,   // not mapped (yet): the copy beside the game's executable, where NGX looks first
    kNone,         // neither
};

// The one log line, said once per session, after the first DLSS feature was asked for (the DLL is mapped by then). `path` is the file
// the version was read from (empty for kNone). Never claims more than it saw: a copy beside the game is not called the loaded one.
inline std::string dlssRuntimeLine(DlssRuntimeSource source, const DlssRuntimeVersion& version, const std::string& path) {
    std::string out = "dlaa: DLSS runtime ";
    switch (source) {
        case DlssRuntimeSource::kLoaded:
            out += "loaded: nvngx_dlss.dll " + (version.read ? "file version " + dlssVersionText(version) : std::string("(file version could not be read)")) +
                   " (" + path + ")";
            break;
        case DlssRuntimeSource::kBesideGame:
            out += "not loaded yet; the nvngx_dlss.dll beside the game " +
                   (version.read ? "has file version " + dlssVersionText(version) : std::string("has no readable file version")) + " (" + path + ")";
            break;
        case DlssRuntimeSource::kNone:
            out += "not loaded and none beside the game: NGX is using a copy from elsewhere, or none";
            break;
    }
    out += ". The graphics driver's version is not read by this build.";
    return out;
}

}  // namespace edvr
