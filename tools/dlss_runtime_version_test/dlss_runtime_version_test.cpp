// Pure-function rig for src\d3d11\dlss_runtime_version.h: the log line that names the loaded nvngx_dlss.dll's file version
// (issue 90, Tomcatt: people swap the DLL with DLSS Swapper and a log should say which DLSS it came from). No Win32, no NGX, no
// files. dlaa.cpp's logDlssRuntimeVersionOnce finds the module and reads VS_FIXEDFILEINFO; this holds the formatting.
#include "../../src/d3d11/dlss_runtime_version.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace edvr;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
}
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("dlss_runtime_version_test: dry-run (no files, no Win32)");
        return 0;
    }
    if (std::strcmp(argv[1], "--self-test")) return 2;

    // 310.9.16.0 is the file version the reporter's DLSS Swapper copy carries; the build ships 310.9.1.
    const DlssRuntimeVersion swapped = dlssVersionFromFixed((310u << 16) | 9u, (16u << 16) | 0u);
    check(swapped.read && swapped.major == 310 && swapped.minor == 9 && swapped.build == 16 && swapped.revision == 0,
          "V1: VS_FIXEDFILEINFO's MS/LS words split into major.minor / build.revision");
    check(dlssVersionText(swapped) == "310.9.16.0", "V2: four fields, dot-separated, as the properties page prints them");
    check(dlssVersionText(dlssVersionFromFixed((310u << 16) | 9u, (1u << 16) | 0u)) == "310.9.1.0", "V3: the shipped 310.9.1 is told apart from 310.9.16");
    check(dlssVersionText(dlssVersionFromFixed(0xFFFFFFFFu, 0xFFFFFFFFu)) == "65535.65535.65535.65535", "V4: the widest value prints whole, no sign or overflow");
    check(dlssVersionText(dlssVersionFromFixed(0, 0)) == "0.0.0.0" && dlssVersionText(DlssRuntimeVersion{}) == "unknown",
          "V5: a read zero version is 0.0.0.0, an unread one is 'unknown' -- the two are not confused");

    const std::string path = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64\\nvngx_dlss.dll";
    const std::string loaded = dlssRuntimeLine(DlssRuntimeSource::kLoaded, swapped, path);
    check(loaded == "dlaa: DLSS runtime loaded: nvngx_dlss.dll file version 310.9.16.0 (" + path +
                        "). The graphics driver's version is not read by this build.",
          "L1: the loaded module's line: the version, the path it was read from, and the driver caveat, exactly");
    check(loaded.find('\n') == std::string::npos, "L2: one line");
    check(has(loaded, "310.9.16.0") && !has(loaded, "310.9.1.0)") && !has(loaded, "unknown"), "L3: the version is the one read");

    const std::string unreadable = dlssRuntimeLine(DlssRuntimeSource::kLoaded, DlssRuntimeVersion{}, path);
    check(has(unreadable, "loaded") && has(unreadable, "could not be read") && !has(unreadable, "file version 0") && !has(unreadable, "unknown"),
          "L4: a loaded module with no readable version says so and prints no number");

    const std::string beside = dlssRuntimeLine(DlssRuntimeSource::kBesideGame, swapped, path);
    check(has(beside, "not loaded yet") && has(beside, "beside the game") && has(beside, "has file version 310.9.16.0") && !has(beside, "loaded:"),
          "L5: a copy beside the game that is not mapped is called that, never the loaded one");
    const std::string besideBad = dlssRuntimeLine(DlssRuntimeSource::kBesideGame, DlssRuntimeVersion{}, path);
    check(has(besideBad, "no readable file version") && !has(besideBad, "has file version"), "L6: an unreadable copy beside the game says so");

    const std::string none = dlssRuntimeLine(DlssRuntimeSource::kNone, DlssRuntimeVersion{}, std::string());
    check(has(none, "not loaded and none beside the game") && !has(none, "()") && !has(none, "file version"),
          "L7: neither: said so, no empty path in brackets, no version claimed");
    check(none.rfind("dlaa: DLSS runtime ", 0) == 0 && loaded.rfind("dlaa: DLSS runtime ", 0) == 0 && beside.rfind("dlaa: DLSS runtime ", 0) == 0,
          "L8: every case starts with the prefix the first-asked line points readers at ('dlaa: DLSS runtime')");

    std::printf("dlss_runtime_version_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
