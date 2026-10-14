// Finding Elite Dangerous, however it was installed.
//
// The README's install table lists three paths and then says: if none of those
// match, find EliteDangerous64.exe yourself. That last sentence is the part
// people get wrong -- the Frontier launcher alone has moved its Products folder
// between %LOCALAPPDATA% and Program Files, Steam libraries live on whichever
// drive had room, and Epic writes its own manifests somewhere neither of the
// others look.
//
// So nothing here trusts a fixed path. Each store is asked where it put the
// game in its own terms (Steam's library index, Epic's manifests, the
// uninstall registry, the launcher's own folders), every answer is then
// CONFIRMED by finding EliteDangerous64.exe underneath it, and anything that
// cannot be confirmed is not offered. The user can always point at a folder
// themselves, and that path goes through exactly the same confirmation.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

struct GameInstall {
    std::wstring dir;        // the folder holding EliteDangerous64.exe
    std::wstring openvrDir;  // the folder holding the game's openvr_api.dll; empty if not found
    std::wstring source;     // "Steam", "Epic", "Frontier launcher", "Folder you chose"
    std::wstring product;    // the Products\ folder name, e.g. elite-dangerous-odyssey-64
    bool odyssey = false;    // Odyssey rather than the Horizons build
};

// Every install this machine can be shown to have, Odyssey first. Confirmed by
// the executable, deduplicated by path.
std::vector<GameInstall> findInstalls();

// Does this folder hold EliteDangerous64.exe?
bool gameDirLooksRight(const std::wstring& dir);

// Build the record for a folder the user picked. `dir` may be the game folder
// itself or a launcher root above it; the game folder underneath is found when
// there is exactly one.
bool describeDir(const std::wstring& dir, const std::wstring& source, GameInstall* out);

// Whichever of Openvr\win64, Openvr or the game folder holds the game's
// openvr_api.dll (or the openvr_api_orig.dll a previous install left). Empty
// when none of them do -- which is a fact the planner has to report, not
// paper over.
std::wstring findOpenvrDir(const std::wstring& gameDir);

// Is Elite Dangerous running, and does that stop us?
//
// Every file this installer touches is one the game holds open while it runs
// -- but only in the folder it was launched from. A machine with two installs
// (the Frontier launcher's and Steam's) plays one while the other is patched,
// and asking by executable NAME alone refused the second for the first one's
// sake.
enum class GameRunState {
    NotRunning,   // no EliteDangerous64.exe anywhere on this machine
    OtherFolder,  // running, and every copy of it came from some other install
    ThisFolder,   // running from the folder about to be written into
    // The process list could not be read (or not to its end), so "stopped" is
    // not something anything here can say. Not "not running": that reading
    // let a failed snapshot walk the installer into a folder the game had open.
    // A caller treats it as a refusal, like ThisFolder, and says why.
    Unknown,
};

// EliteDangerous64.exe, weighed against the folder about to be written into.
GameRunState gameRunState(const std::wstring& gameDir);

// The same question about any executable name. It takes the name rather than
// fixing it because that is the only way to test the answer: no build machine
// has Elite running, and every one of them has the test process itself.
GameRunState runStateOf(const wchar_t* exeName, const std::wstring& dir);

// runStateOf over a process snapshot somebody else took (a TH32CS_SNAPPROCESS
// handle, passed as void* so this header needs no windows.h). Takes ownership:
// the handle is closed. INVALID_HANDLE_VALUE or null -- CreateToolhelp32Snapshot
// having failed -- is Unknown, which is the one branch a test cannot reach by
// asking Windows for a snapshot.
GameRunState runStateOfSnapshot(void* snapshot, const wchar_t* exeName, const std::wstring& dir);

// ---- the rig (Pre-flight reads these; nothing here writes) -------------------

// The PCI vendor ids DXGI reports, as the upscaler rule keys on them.
constexpr unsigned kVendorNvidia = 0x10DE;
constexpr unsigned kVendorAmd = 0x1002;
constexpr unsigned kVendorIntel = 0x8086;
constexpr unsigned kVendorMicrosoft = 0x1414;  // Microsoft Basic Render Driver

struct GpuAdapter {
    unsigned           vendorId = 0;
    std::wstring       name;               // DXGI's Description, e.g. NVIDIA GeForce RTX 4070 Ti
    unsigned long long dedicatedVram = 0;  // bytes
    unsigned long long luid = 0;           // (HighPart << 32) | LowPart
};

struct GpuList {
    std::vector<GpuAdapter> adapters;  // every real adapter, in DXGI's order
    // Index into `adapters` of the card the headset is most likely on: the one
    // Windows ranks first for high performance (IDXGIFactory6) when it can be
    // asked, else the one with the most dedicated memory. -1 when there are none.
    int  primary = -1;
    bool primaryFromWindows = false;  // true when Windows chose it, false when memory did
};

// Every DXGI adapter that is a graphics card. A software adapter
// (DXGI_ADAPTER_FLAG_SOFTWARE) and the Microsoft Basic Render Driver are not.
GpuList gpuAdapters();

// The two decisions inside gpuAdapters, apart from DXGI so a test can drive them.
// `dxgiFlags` is DXGI_ADAPTER_DESC1::Flags.
bool countsAsGraphicsCard(unsigned vendorId, unsigned dxgiFlags);
// The index of the adapter whose LUID Windows ranked first when it was asked
// (`haveWindowsChoice`), else of the one with the most dedicated memory (the
// first of equals). -1 for an empty list. A Windows choice no adapter matches is
// ignored rather than trusted.
int pickPrimaryAdapter(const std::vector<GpuAdapter>& adapters, bool haveWindowsChoice,
                       unsigned long long windowsChoiceLuid);

// Windows' active OpenXR runtime: what the loader EDVR bundles will pick.
// XR_RUNTIME_JSON when it is set, else HKLM\SOFTWARE\Khronos\OpenXR\1
// ActiveRuntime read from the 64-bit registry view.
enum class OpenXrRuntimeState {
    NotSet,           // neither names a manifest
    ManifestMissing,  // one is named, and the file is not there
    Ok,
};
struct OpenXrRuntime {
    OpenXrRuntimeState state = OpenXrRuntimeState::NotSet;
    std::wstring       manifest;          // the path that was named
    bool               fromEnvironment = false;  // XR_RUNTIME_JSON rather than the registry
    // The manifest's runtime.name, else the manifest's file stem; empty when
    // nothing is set.
    std::string        name;
};
OpenXrRuntime activeOpenXrRuntime();
// The half of the above that does not depend on this machine's registry: a
// manifest path, however it was found.
OpenXrRuntime openXrRuntimeAt(const std::wstring& manifestPath, bool fromEnvironment);

// runtime.name out of a manifest's JSON; empty when the document is malformed,
// has no runtime object, or the object has no string name.
std::string openXrRuntimeNameFromManifest(const std::string& json);
// "oculus_openxr_64" from ...\oculus_openxr_64.json.
std::string openXrRuntimeNameFromPath(const std::wstring& manifestPath);

struct OpenXrRuntimeLabel {
    std::string display;  // a friendly name for the well-known runtimes, else `name` as it came
    std::string hint;     // where to change it: "change it in SteamVR"; or, for an unknown one, "Your headset's own app sets it."
    bool        known = false;
};
OpenXrRuntimeLabel describeOpenXrRuntime(const std::string& name, const std::wstring& manifestPath);

// Is a process of this name running, in any folder, spelled in any case? Taken
// from the same process snapshot gameRunState uses. A snapshot that could not
// be read answers false: this is for a warning, which has no business inventing
// a process it could not see (gameRunState treats the same failure as a refusal,
// because there a wrong "stopped" costs files).
bool processRunning(const wchar_t* exeName);

// Is there an NVIDIA graphics adapter in this machine, by DXGI's vendor id,
// software adapters skipped? NVIDIA's DLSS runtime is placed only where
// there is one. The first such adapter's name goes to `name` when given. A
// machine with an AMD integrated GPU beside an NVIDIA card counts.
bool nvidiaAdapterPresent(std::wstring* name);

// Path and text helpers shared with the rest of the installer.
std::wstring joinPath(const std::wstring& dir, const std::wstring& leaf);
std::wstring leafOf(const std::wstring& path);
bool         fileExists(const std::wstring& path);
bool         dirExists(const std::wstring& path);
std::wstring canonicalPath(const std::wstring& path);
std::string  toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

// The whole file, or an empty string. Text only: everything read this way is
// an ini, a manifest or a launcher's index.
std::string readTextFile(const std::wstring& path, size_t limit = 4u << 20);

}  // namespace edvr::installer
