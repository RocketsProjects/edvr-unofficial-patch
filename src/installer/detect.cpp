#include "detect.h"

#include <windows.h>
#include <tlhelp32.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <vector>

namespace edvr::installer {
namespace {

const wchar_t* kGameExe = L"EliteDangerous64.exe";

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring env(const wchar_t* name) {
    wchar_t buf[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    if (n == 0 || n >= 1024) return std::wstring();
    return std::wstring(buf, n);
}

std::string readTextFileImpl(const std::wstring& path, size_t limit = 4u << 20) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 ||
        static_cast<size_t>(size.QuadPart) > limit) {
        CloseHandle(f);
        return std::string();
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(f);
    // A short read is a failure, not a shorter file. config.cpp refuses one for
    // the same reason and records what it cost: a truncated ini looks exactly
    // like a file somebody deleted settings from, and the merge would write
    // that reading back to disk as though they had.
    if (!ok || read < text.size()) return std::string();
    return text;
}

std::wstring regString(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD extraFlags) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | extraFlags, &key) != ERROR_SUCCESS)
        return std::wstring();
    wchar_t buf[1024]{};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS st =
        RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::wstring();
    return std::wstring(buf);
}

// Every subdirectory of `dir`, or nothing if it cannot be listed.
std::vector<std::wstring> subdirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::wstring> filesMatching(const std::wstring& dir, const std::wstring& pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// A store told us where it put the game. Confirm it, and add every product
// folder underneath that really holds the executable.
//
// "Underneath" is one level of Products\ and no more. A recursive search of a
// Steam library would take minutes and could wander into somebody's backups.
void addFromRoot(const std::wstring& root, const std::wstring& source,
                 std::vector<GameInstall>* out) {
    if (root.empty()) return;

    auto push = [&](const std::wstring& dir, const std::wstring& product) {
        GameInstall gi;
        gi.dir = canonicalPath(dir);
        gi.source = source;
        gi.product = product;
        gi.odyssey = lower(product).find(L"odyssey") != std::wstring::npos;
        gi.openvrDir = findOpenvrDir(gi.dir);
        out->push_back(gi);
    };

    if (gameDirLooksRight(root)) {
        // The root IS a product folder: name it by its own leaf.
        const size_t slash = root.find_last_of(L"\\/");
        push(root, slash == std::wstring::npos ? root : root.substr(slash + 1));
        return;
    }

    // Store roots point at either the launcher folder or the game folder above
    // Products\. Both shapes appear in the wild, and Steam adds one more level
    // (the library, then "Elite Dangerous").
    const std::wstring candidates[] = {
        root,
        joinPath(root, L"Elite Dangerous"),
        joinPath(root, L"EDLaunch"),
    };
    for (const std::wstring& c : candidates) {
        const std::wstring products = joinPath(c, L"Products");
        if (!dirExists(products)) continue;
        for (const std::wstring& p : subdirs(products)) {
            const std::wstring dir = joinPath(products, p);
            if (gameDirLooksRight(dir)) push(dir, p);
        }
    }
}

// ---- Steam ---------------------------------------------------------------
//
// The library index, not a guessed path: people move Elite to whichever drive
// has room, and libraryfolders.vdf is where Steam records that.
void findSteam(std::vector<GameInstall>* out) {
    std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
    if (steam.empty())
        steam = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath",
                          KEY_WOW64_32KEY);
    if (steam.empty()) return;
    for (wchar_t& c : steam) {
        if (c == L'/') c = L'\\';
    }

    std::vector<std::wstring> libraries{steam};
    const std::string vdf = readTextFileImpl(joinPath(steam, L"steamapps\\libraryfolders.vdf"));
    // Deliberately not a VDF parser: every entry of interest is a "path" key
    // whose value is a quoted, backslash-escaped Windows path, and that is all
    // this needs to read. A wrong or stale entry costs nothing -- it simply
    // fails the executable check below.
    size_t p = 0;
    while ((p = vdf.find("\"path\"", p)) != std::string::npos) {
        const size_t open = vdf.find('"', p + 6);
        if (open == std::string::npos) break;
        const size_t close = vdf.find('"', open + 1);
        if (close == std::string::npos) break;
        std::string raw = vdf.substr(open + 1, close - open - 1);
        std::string unescaped;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') ++i;
            unescaped += raw[i];
        }
        libraries.push_back(fromUtf8(unescaped));
        p = close + 1;
    }

    for (const std::wstring& lib : libraries) {
        if (lib.empty()) continue;
        addFromRoot(joinPath(lib, L"steamapps\\common\\Elite Dangerous"), L"Steam", out);
    }
}

// ---- Epic ----------------------------------------------------------------
//
// Epic writes one .item manifest per installed game, as JSON. Again no parser:
// InstallLocation is the only field that matters and DisplayName is only used
// to tell Elite's manifest from the other twenty.
void findEpic(std::vector<GameInstall>* out) {
    const std::wstring manifests =
        joinPath(env(L"ProgramData"), L"Epic\\EpicGamesLauncher\\Data\\Manifests");
    if (!dirExists(manifests)) return;

    for (const std::wstring& name : filesMatching(manifests, L"*.item")) {
        const std::string json = readTextFileImpl(joinPath(manifests, name), 1u << 20);
        if (json.empty()) continue;

        auto field = [&](const char* key) -> std::string {
            const std::string needle = std::string("\"") + key + "\"";
            size_t k = json.find(needle);
            if (k == std::string::npos) return std::string();
            const size_t colon = json.find(':', k + needle.size());
            if (colon == std::string::npos) return std::string();
            const size_t open = json.find('"', colon);
            if (open == std::string::npos) return std::string();
            std::string value;
            for (size_t i = open + 1; i < json.size(); ++i) {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    ++i;
                    value += json[i];
                    continue;
                }
                if (json[i] == '"') break;
                value += json[i];
            }
            return value;
        };

        const std::wstring display = lower(fromUtf8(field("DisplayName")));
        const std::wstring location = fromUtf8(field("InstallLocation"));
        if (location.empty()) continue;
        if (display.find(L"elite") == std::wstring::npos &&
            lower(location).find(L"elite") == std::wstring::npos) {
            continue;
        }
        addFromRoot(location, L"Epic", out);
    }
}

// ---- Frontier launcher ---------------------------------------------------
void findFrontier(std::vector<GameInstall>* out) {
    // Where the launcher itself keeps Products these days.
    addFromRoot(joinPath(env(L"LOCALAPPDATA"), L"Frontier_Developments"), L"Frontier launcher", out);

    // The uninstall registry, both views: whatever the launcher's installer
    // recorded is the authoritative answer for an install that moved.
    const wchar_t* uninstallRoots[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };
    for (const wchar_t* uroot : uninstallRoots) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, uroot, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t sub[512]{};
            DWORD len = 512;
            if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            const std::wstring path = std::wstring(uroot) + L"\\" + sub;
            const std::wstring name = lower(regString(HKEY_LOCAL_MACHINE, path.c_str(),
                                                      L"DisplayName", 0));
            if (name.find(L"elite dangerous") == std::wstring::npos &&
                name.find(L"frontier") == std::wstring::npos) {
                continue;
            }
            addFromRoot(regString(HKEY_LOCAL_MACHINE, path.c_str(), L"InstallLocation", 0),
                        L"Frontier launcher", out);
        }
        RegCloseKey(key);
    }

    // And the places it has installed itself to by default, on every fixed
    // drive. Cheap: three directory probes per drive, no walking.
    std::vector<std::wstring> roots;
    const std::wstring progFiles86 = env(L"ProgramFiles(x86)");
    const std::wstring progFiles = env(L"ProgramFiles");
    if (!progFiles86.empty()) roots.push_back(joinPath(progFiles86, L"Frontier"));
    if (!progFiles.empty()) roots.push_back(joinPath(progFiles, L"Frontier"));

    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        const std::wstring d(root);
        roots.push_back(d + L"Frontier");
        roots.push_back(d + L"Games\\Frontier");
        roots.push_back(d + L"Program Files (x86)\\Frontier");
    }
    for (const std::wstring& r : roots) addFromRoot(r, L"Frontier launcher", out);
}

// ---- who is running ------------------------------------------------------

// The full path of a running process's executable, or nothing.
//
// PROCESS_QUERY_LIMITED_INFORMATION is the right to ask for: it is granted
// across integrity levels, where the older PROCESS_QUERY_INFORMATION is
// refused for anything running higher than the asker -- and a game launched by
// a Steam that runs elevated, beside an installer that does not, is exactly
// that shape. When it is refused anyway the caller reads the silence as the
// dangerous answer, which is the whole reason this can return nothing.
std::wstring processImagePath(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return std::wstring();
    wchar_t buf[1024]{};
    DWORD chars = 1024;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, buf, &chars);
    CloseHandle(proc);
    if (!ok || chars == 0 || chars >= 1024) return std::wstring();
    return std::wstring(buf, chars);
}

// The folder a path names a file in. A path with nothing above it but a drive
// keeps its separator: "C:" on its own means the current directory on C:,
// which is not the folder anybody asked about.
std::wstring parentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    if (slash == 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, slash);
}

// The same folder, however each side spelled it: case, trailing separators and
// a "." or ".." on the way are differences Windows does not have.
//
// What this does not undo is a junction or an 8.3 short name -- two routes to
// one folder still read as two folders. Both sides of every comparison here
// come from a store's own record of where it put the game, or from the folder
// the user picked, and neither arrives in those shapes.
bool sameDir(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(canonicalPath(a).c_str(), canonicalPath(b).c_str()) == 0;
}

}  // namespace

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr,
                                         0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], need, nullptr,
                        nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need =
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], need);
    return out;
}

std::string readTextFile(const std::wstring& path, size_t limit) {
    return readTextFileImpl(path, limit);
}

std::wstring leafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    if (leaf.empty()) return dir;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += leaf;
    return out;
}

bool fileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring canonicalPath(const std::wstring& path) {
    wchar_t buf[1024]{};
    const DWORD n = GetFullPathNameW(path.c_str(), 1024, buf, nullptr);
    std::wstring out = (n > 0 && n < 1024) ? std::wstring(buf, n) : path;
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

bool gameDirLooksRight(const std::wstring& dir) {
    return !dir.empty() && fileExists(joinPath(dir, kGameExe));
}

std::wstring findOpenvrDir(const std::wstring& gameDir) {
    // Both layouts are in the field, and a previous install leaves the renamed
    // original as the only openvr file in the folder -- so the search goes by
    // FILE, not by folder name, and accepts either name.
    const std::wstring candidates[] = {
        joinPath(gameDir, L"Openvr\\win64"),
        joinPath(gameDir, L"Openvr"),
        gameDir,
    };
    for (const std::wstring& c : candidates) {
        if (!dirExists(c)) continue;
        if (fileExists(joinPath(c, L"openvr_api.dll")) ||
            fileExists(joinPath(c, L"openvr_api_orig.dll"))) {
            return c;
        }
    }
    return std::wstring();
}

bool describeDir(const std::wstring& dir, const std::wstring& source, GameInstall* out) {
    std::vector<GameInstall> found;
    addFromRoot(canonicalPath(dir), source, &found);
    if (found.empty()) return false;
    *out = found.front();
    return true;
}

GameRunState runStateOfSnapshot(void* snapshot, const wchar_t* exeName, const std::wstring& dir) {
    HANDLE snap = static_cast<HANDLE>(snapshot);
    // This used to return NotRunning: a snapshot that could not be taken read as
    // a machine with no game on it, and the installer went on to replace files
    // the game had open. Not knowing is not the same answer as "stopped", and
    // the caller is told which it got.
    if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return GameRunState::Unknown;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    GameRunState state = GameRunState::NotRunning;
    bool listed = false;  // the list was read to its end, or the answer no longer depends on the rest
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) != 0) continue;

            // The name matched, which is where this used to stop. Every
            // install on the machine carries the same one; only the folder a
            // running copy was launched from has its files held open.
            const std::wstring image = processImagePath(pe.th32ProcessID);

            // A process that cannot be read is not a process that can be
            // cleared -- it could be this very folder's, started elevated or
            // already on its way out -- and a caller with no folder to compare
            // against has asked the older question. Both read as the refusal,
            // because a refusal is the direction somebody can recover from.
            if (image.empty() || dir.empty() || sameDir(parentOf(image), dir)) {
                state = GameRunState::ThisFolder;
                listed = true;
                break;  // nothing a later process says softens this
            }
            state = GameRunState::OtherFolder;
        } while (Process32NextW(snap, &pe));
        // Read straight after the loop that ended it: ERROR_NO_MORE_FILES is the
        // list running out; anything else is the list failing part-way, and a
        // process in the part not read could be the game.
        if (!listed) listed = GetLastError() == ERROR_NO_MORE_FILES;
    }
    CloseHandle(snap);
    return listed ? state : GameRunState::Unknown;
}

GameRunState runStateOf(const wchar_t* exeName, const std::wstring& dir) {
    return runStateOfSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), exeName, dir);
}

GameRunState gameRunState(const std::wstring& gameDir) { return runStateOf(kGameExe, gameDir); }

std::vector<GameInstall> findInstalls() {
    std::vector<GameInstall> found;
    findSteam(&found);
    findEpic(&found);
    findFrontier(&found);

    // One install can be reachable through more than one store record (an Epic
    // manifest and the uninstall registry can name the same folder). Keep the
    // first, which is the more specific source.
    std::vector<GameInstall> unique;
    std::set<std::wstring> seen;
    for (const GameInstall& gi : found) {
        if (!seen.insert(lower(gi.dir)).second) continue;
        unique.push_back(gi);
    }

    // Odyssey first: it is what EDVR is for, and on a machine with both
    // products the Horizons folder is the wrong answer offered by default.
    std::stable_sort(unique.begin(), unique.end(),
                     [](const GameInstall& a, const GameInstall& b) {
                         return a.odyssey && !b.odyssey;
                     });
    return unique;
}

bool processRunning(const wchar_t* exeName) {
    if (!exeName || !*exeName) return false;
    // With no folder to compare against a name match is a match (runStateOfSnapshot
    // reads the empty folder as "any"), which is the question asked here.
    const GameRunState state = runStateOf(exeName, std::wstring());
    return state == GameRunState::ThisFolder || state == GameRunState::OtherFolder;
}

// ---------------------------------------------------------------------------
// the graphics cards
// ---------------------------------------------------------------------------

bool countsAsGraphicsCard(unsigned vendorId, unsigned dxgiFlags) {
    // The render-only "Basic Render Driver" is flagged as software; its vendor id is
    // checked as well, since a driver that forgot the flag would otherwise pass for a card.
    if (dxgiFlags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
    return vendorId != kVendorMicrosoft;
}

int pickPrimaryAdapter(const std::vector<GpuAdapter>& adapters, bool haveWindowsChoice,
                       unsigned long long windowsChoiceLuid) {
    if (adapters.empty()) return -1;
    if (haveWindowsChoice) {
        for (size_t i = 0; i < adapters.size(); ++i)
            if (adapters[i].luid == windowsChoiceLuid) return static_cast<int>(i);
    }
    size_t best = 0;
    for (size_t i = 1; i < adapters.size(); ++i)
        if (adapters[i].dedicatedVram > adapters[best].dedicatedVram) best = i;
    return static_cast<int>(best);
}

namespace {

GpuAdapter adapterFrom(const DXGI_ADAPTER_DESC1& desc) {
    GpuAdapter a;
    a.vendorId = desc.VendorId;
    a.name = desc.Description;
    a.dedicatedVram = static_cast<unsigned long long>(desc.DedicatedVideoMemory);
    a.luid = (static_cast<unsigned long long>(static_cast<unsigned long>(desc.AdapterLuid.HighPart)) << 32) |
             static_cast<unsigned long long>(desc.AdapterLuid.LowPart);
    return a;
}

// DXGI rather than WMI or the registry: it is the same enumeration the game's D3D11
// device comes from, it needs no COM apartment, and a software adapter (the
// render-only "Basic Render Driver") is flagged as such rather than passing for a
// card. `askWindows` adds the high-performance ranking (IDXGIFactory6, Windows 10
// 1803 and later; absent on anything older, which then falls back to memory).
GpuList enumerateAdapters(bool askWindows) {
    GpuList list;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        !factory) {
        return list;
    }
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && countsAsGraphicsCard(desc.VendorId, desc.Flags))
            list.adapters.push_back(adapterFrom(desc));
        adapter->Release();
    }

    bool haveChoice = false;
    unsigned long long choiceLuid = 0;
    if (askWindows && !list.adapters.empty()) {
        IDXGIFactory6* factory6 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6),
                                              reinterpret_cast<void**>(&factory6))) &&
            factory6) {
            for (UINT i = 0; !haveChoice; ++i) {
                IDXGIAdapter1* adapter = nullptr;
                if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         __uuidof(IDXGIAdapter1),
                                                         reinterpret_cast<void**>(&adapter)) != S_OK ||
                    !adapter) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                // The first adapter Windows ranks that is a card at all: its list can
                // open with the software one.
                if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                    countsAsGraphicsCard(desc.VendorId, desc.Flags)) {
                    haveChoice = true;
                    choiceLuid = adapterFrom(desc).luid;
                }
                adapter->Release();
            }
            factory6->Release();
        }
    }
    factory->Release();

    list.primary = pickPrimaryAdapter(list.adapters, haveChoice, choiceLuid);
    list.primaryFromWindows =
        haveChoice && list.primary >= 0 &&
        list.adapters[static_cast<size_t>(list.primary)].luid == choiceLuid;
    return list;
}

}  // namespace

GpuList gpuAdapters() { return enumerateAdapters(true); }

bool nvidiaAdapterPresent(std::wstring* name) {
    const GpuList list = enumerateAdapters(false);
    for (const GpuAdapter& a : list.adapters) {
        if (a.vendorId != kVendorNvidia) continue;
        if (name) *name = a.name;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// the active OpenXR runtime
// ---------------------------------------------------------------------------

namespace {

// Just enough JSON to find runtime.name in a runtime's manifest. Strict about the
// whole document: a manifest that does not parse names nothing, because the loader
// would not read it either.
class ManifestReader {
public:
    explicit ManifestReader(const std::string& text) : t_(text) {}

    std::string runtimeName() {
        if (t_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;   // a byte order mark is allowed in front
        std::string name;
        skipSpace();
        if (!object(0, &name)) return std::string();
        skipSpace();
        if (i_ != t_.size()) return std::string();
        return name;
    }

private:
    static const int kMaxDepth = 32;
    const std::string& t_;
    size_t i_ = 0;

    char peek() const { return i_ < t_.size() ? t_[i_] : '\0'; }
    bool eat(char c) {
        if (peek() != c || i_ >= t_.size()) return false;
        ++i_;
        return true;
    }
    void skipSpace() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\r' || t_[i_] == '\n')) ++i_;
    }

    static void appendUtf8(std::string* out, unsigned long cp) {
        if (cp < 0x80) {
            *out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            *out += static_cast<char>(0xC0 | (cp >> 6));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *out += static_cast<char>(0xE0 | (cp >> 12));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            *out += static_cast<char>(0xF0 | (cp >> 18));
            *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned long* out) {
        if (i_ + 4 > t_.size()) return false;
        unsigned long v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_ + static_cast<size_t>(k)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned long>(c - 'A' + 10);
            else return false;
        }
        i_ += 4;
        *out = v;
        return true;
    }

    bool string(std::string* out) {
        if (!eat('"')) return false;
        std::string s;
        for (;;) {
            if (i_ >= t_.size()) return false;
            const unsigned char c = static_cast<unsigned char>(t_[i_++]);
            if (c == '"') break;
            if (c < 0x20) return false;
            if (c != '\\') {
                s += static_cast<char>(c);
                continue;
            }
            if (i_ >= t_.size()) return false;
            const char e = t_[i_++];
            switch (e) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {   // a high surrogate: its low half follows
                        unsigned long low = 0;
                        if (!eat('\\') || !eat('u') || !hex4(&low) || low < 0xDC00 || low > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return false;
                    }
                    appendUtf8(&s, cp);
                    break;
                }
                default: return false;
            }
        }
        *out = s;
        return true;
    }

    bool skipValue(int depth) {
        if (depth > kMaxDepth) return false;
        skipSpace();
        const char c = peek();
        if (c == '"') {
            std::string ignored;
            return string(&ignored);
        }
        if (c == '{') {
            ++i_;
            skipSpace();
            if (eat('}')) return true;
            for (;;) {
                skipSpace();
                std::string key;
                if (!string(&key)) return false;
                skipSpace();
                if (!eat(':')) return false;
                if (!skipValue(depth + 1)) return false;
                skipSpace();
                if (eat(',')) continue;
                return eat('}');
            }
        }
        if (c == '[') {
            ++i_;
            skipSpace();
            if (eat(']')) return true;
            for (;;) {
                if (!skipValue(depth + 1)) return false;
                skipSpace();
                if (eat(',')) continue;
                return eat(']');
            }
        }
        // A number, true, false or null: the characters those are made of, and at least one.
        const size_t from = i_;
        while (i_ < t_.size() && ((t_[i_] >= '0' && t_[i_] <= '9') || (t_[i_] >= 'a' && t_[i_] <= 'z') ||
                                  (t_[i_] >= 'A' && t_[i_] <= 'Z') || t_[i_] == '+' || t_[i_] == '-' ||
                                  t_[i_] == '.'))
            ++i_;
        return i_ > from;
    }

    // level 0 is the document, level 1 the "runtime" object inside it; everything else is skipped.
    bool object(int level, std::string* name) {
        if (!eat('{')) return false;
        skipSpace();
        if (eat('}')) return true;
        for (;;) {
            skipSpace();
            std::string key;
            if (!string(&key)) return false;
            skipSpace();
            if (!eat(':')) return false;
            skipSpace();
            if (level == 0 && key == "runtime" && peek() == '{') {
                if (!object(1, name)) return false;
            } else if (level == 1 && key == "name" && peek() == '"') {
                std::string value;
                if (!string(&value)) return false;
                *name = value;   // the last of a repeated key, as a JSON reader keeps it
            } else if (!skipValue(level + 1)) {
                return false;
            }
            skipSpace();
            if (eat(',')) continue;
            return eat('}');
        }
    }
};

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// Environment variables can be long (a runtime manifest path is a full path); asked for at its own size.
std::wstring envAnySize(const wchar_t* name) {
    const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return std::wstring();
    std::wstring value(need, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, &value[0], need);
    if (got == 0 || got >= need) return std::wstring();
    value.resize(got);
    return value;
}

}  // namespace

std::string openXrRuntimeNameFromManifest(const std::string& json) {
    std::string name = trimmed(ManifestReader(json).runtimeName());
    if (name.size() > 80) name.resize(80);
    return name;
}

std::string openXrRuntimeNameFromPath(const std::wstring& manifestPath) {
    std::wstring leaf = leafOf(manifestPath);
    const size_t dot = leaf.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) leaf.resize(dot);
    return toUtf8(leaf);
}

OpenXrRuntime openXrRuntimeAt(const std::wstring& manifestPath, bool fromEnvironment) {
    OpenXrRuntime rt;
    if (manifestPath.empty()) return rt;
    rt.manifest = manifestPath;
    rt.fromEnvironment = fromEnvironment;
    if (!fileExists(manifestPath)) {
        rt.state = OpenXrRuntimeState::ManifestMissing;
        rt.name = openXrRuntimeNameFromPath(manifestPath);
        return rt;
    }
    rt.state = OpenXrRuntimeState::Ok;
    rt.name = openXrRuntimeNameFromManifest(readTextFile(manifestPath, 1u << 20));
    if (rt.name.empty()) rt.name = openXrRuntimeNameFromPath(manifestPath);
    return rt;
}

OpenXrRuntime activeOpenXrRuntime() {
    // The loader's own order: the environment variable overrides the registry.
    const std::wstring fromEnv = envAnySize(L"XR_RUNTIME_JSON");
    if (!fromEnv.empty()) return openXrRuntimeAt(fromEnv, true);
    // The 64-bit view: this installer is 64-bit today, but the key is the 64-bit
    // loader's, and a 32-bit process would otherwise be redirected away from it.
    const std::wstring registered =
        regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY);
    return openXrRuntimeAt(registered, false);
}

OpenXrRuntimeLabel describeOpenXrRuntime(const std::string& name, const std::wstring& manifestPath) {
    OpenXrRuntimeLabel label;
    const std::string hay = lowerAscii(name + " " + toUtf8(manifestPath));
    auto has = [&](const char* needle) { return hay.find(needle) != std::string::npos; };
    // The wording names the app and nothing finer: where in it the switch lives
    // moves between versions, and a menu path that was wrong would cost more than
    // none.
    if (has("steamvr") || has("steamxr")) {
        label.display = "SteamVR";
        label.hint = "change it in SteamVR";
        label.known = true;
    } else if (has("virtual desktop") || has("virtualdesktop") || has("vdxr")) {
        label.display = "Virtual Desktop";
        label.hint = "change it in Virtual Desktop";
        label.known = true;
    } else if (has("pimax") || has("piopenxr")) {
        label.display = has("pimaxxr") ? "PimaxXR" : "Pimax";
        label.hint = "change it in Pimax Play";
        label.known = true;
    } else if (has("oculus") || has("meta quest") || has("meta horizon")) {
        // Meta's PC app has been called Oculus, Meta Quest Link and Meta Horizon Link; the runtime's
        // manifest still lives under Oculus, so it is the file that is matched and "Link" that is said.
        label.display = "Oculus (Meta)";
        label.hint = "change it in Meta's Link app";
        label.known = true;
    } else if (has("mixed reality") || has("mixedreality")) {
        label.display = "Windows Mixed Reality";
        label.hint = "change it in OpenXR Tools for Windows Mixed Reality";
        label.known = true;
    } else {
        label.display = name;
        label.hint = "Your headset's own app sets it.";
    }
    return label;
}

}  // namespace edvr::installer
