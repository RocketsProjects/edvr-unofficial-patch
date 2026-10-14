// The window.
//
// One screen per job, no wizard: which install, which halves, and a pane that
// says exactly what is about to happen -- because the thing being modified is
// somebody's game folder, and an installer that does its work behind a progress
// bar is asking for trust it has not earned. Every button shows the plan and
// waits for a yes before a file moves.
//
// The look is drawn rather than inherited; see ui.h for why and how. What is
// still native here is deliberate: the install picker is a real combo box and
// the report is a real edit control, because a dropdown and a scrolling text
// view are the two things a hand-rolled widget gets subtly wrong -- keyboard
// selection, mouse wheel, text selection for copying into a bug report.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <map>
#include <string>
#include <vector>

#include "app.h"
#include "apply.h"
#include "logbundle.h"
#include "mirror.h"
#include "payload.h"
#include "preflight.h"
#include "settings.h"
#include "settings_view.h"
#include "ui.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace edvr::installer {
namespace {

enum : int {
    kIdCombo = 1001,
    kIdBrowse = 1002,
    kIdChkKeep = 1012,
    kIdInstall = 1020,
    kIdRepair = 1021,
    kIdUninstall = 1022,
    kIdCollectLogs = 1024,
    kIdReport = 1030,
    kIdKofi = 1040,
    kIdDiscord = 1041,
    kIdTabInstall = 1050,
    kIdTabSettings = 1051,
    kIdTabPreflight = 1052,
    kIdSearch = 1060,
    kIdSettingsList = 1061,
    kIdSettingsVr = 1062,
    kIdSettingsFlat = 1063,
    kIdScope = 1070,
    kIdPfSetUp = 1082,
    kIdPfUndo = 1083,
    kIdPfBox = 1090,   // kPfBoxCount ids from here: one checkbox per EDVR row on the Pre-flight tab
};

const UINT kMsgFirstScan = WM_APP + 1;

const wchar_t* kTipUrl = L"https://ko-fi.com/seancharacterecho";
const wchar_t* kDiscordUrl = L"https://discord.gg/ynkdf6Gdua";
const wchar_t* kTipText = L"EDVR is free. If it makes your flight more enjoyable, you can";
constexpr int kTipTop = 658;  // the tip line's top: the painted sentence and both links share it

// The client area and every control position below are in 96-dpi units, scaled
// once at layout time.
const int kClientWidth = 720;
const int kClientHeight = 688;
const int kMargin = 24;
const int kCardPad = 20;

// The scope prompt's three choices, in the order the combo lists them. The
// default is the folder's own scope, else both.
const wchar_t* kScopeChoices[] = {L"VR and flat (recommended)", L"VR only", L"Flat only"};

// The Pre-flight tab's geometry, in 96-dpi units (the window is fixed at 720x688 and the tab does not scroll).
// The tallest list is both scopes with every Elite row and RivaTuner: 3 rig rows, 6 two-line rows and 1 warning,
// which ends at 552, above the result line.
const int kPfLeft = kMargin + kCardPad;                  // the checkbox and dot column
const int kPfRight = kClientWidth - kMargin - kCardPad;
const int kPfCardBottom = 648;
const int kPfRigHeadingY = 200;
const int kPfRigRow = 22;
const int kPfFlyRow = 35;     // the label and its numbers, then the why line under them
const int kPfWarnRow = 22;
const int kPfNowX = 400;      // the "now" column; the arrow and the "after" column follow
const int kPfArrowX = 478;
const int kPfAfterX = 504;
const int kPfResultY = 564;   // what the last button did
const int kPfButtonY = 588;
const int kPfCaptionY = 626;

enum class Screen { Install, Preflight, Settings };

enum class Tone { Good, Warn, Bad, Muted };

struct StatusRow {
    std::wstring label;
    std::wstring value;
    Tone         tone = Tone::Muted;
};

struct Place {
    HWND hwnd = nullptr;
    int  x = 0, y = 0, w = 0, h = 0;
    Screen screen = Screen::Install;
    bool bothScreens = false;
};

// The Pre-flight tab. Its rows are painted (dots, columns, the why line); only the checkboxes and the two
// buttons are controls. Checkboxes come from a small pool bound to the EDVR rows that can be ticked.
constexpr size_t kPfBoxCount = 8;

struct Gui {
    HWND window = nullptr;
    HWND combo = nullptr;
    HWND scopeCombo = nullptr;
    HWND report = nullptr;
    HWND chkKeep = nullptr;
    HWND install = nullptr;
    HWND repair = nullptr;
    HWND uninstall = nullptr;
    HWND collectLogs = nullptr;
    HWND tabInstall = nullptr;
    HWND tabSettings = nullptr;
    HWND tabPreflight = nullptr;
    HWND pfSetUp = nullptr;                 // Set up for me
    HWND pfUndo = nullptr;                  // Put my old settings back
    std::vector<HWND> pfBoxes;              // the checkbox pool
    std::vector<int>  pfBoxRow;             // the row each pool box is bound to, -1 when it is hidden
    std::vector<PreflightRow> pfRows;       // what recommendPreflight said when the tab was last refreshed
    std::map<std::string, bool> pfTicks;    // the user's ticks by row id, kept across refreshes
    std::wstring pfResult;                  // one line under the rows: what the last button did
    RowTone      pfResultTone = RowTone::Muted;   // that line's colour
    int          pfStatusRow = -1;          // the Setup tab's Pre-flight row, for a click on it
    HWND settingsVr = nullptr;
    HWND settingsFlat = nullptr;
    HWND tip = nullptr;
    HWND discord = nullptr;
    HWND search = nullptr;
    HWND settingsList = nullptr;
    SettingsModel settings;
    std::string   settingsEdition = "vr";   // the file the settings screen edits: "vr" (edvr.ini) or "flat" (edvr-flat.ini)
    edvr::ModeRead mode = edvr::readGameMode(nullptr);  // Elite's 3D mode, read when the folder is surveyed
    UINT dpi = 96;

    std::vector<Place>      places;
    std::vector<StatusRow>  status;
    std::wstring            statusPath;   // the full folder, under the picker
    std::wstring            modeLine;     // "Elite's 3D mode is ..." under the status rows
    std::wstring            scopeLine;    // "This install does not include ..." when the scope leaves the mode out

    std::vector<GameInstall> installs;
    Survey                   survey;
    bool                     haveSurvey = false;
    Screen                   screen = Screen::Install;
    AppArgs                  args;
    bool                     reportDrag = false;   // slim-scrollbar drag state
    int                      reportDragOffset = 0;
};

Gui g;

int dp(int v) { return ui::dp(v, g.dpi); }

void setText(HWND control, const std::string& utf8) {
    SetWindowTextW(control, fromUtf8(utf8).c_str());
}

// The report keeps no native scrollbar (an EDIT's bar is the one piece of
// stock chrome the drawn look could not restyle); the parent paints the
// same slim thumb the settings list uses, in the report card's right
// gutter, and drives the EDIT by lines when it is dragged.
const RECT kReportCard{kMargin, 524, kClientWidth - kMargin, 650};

RECT reportCardRect() {
    return RECT{dp(kReportCard.left), dp(kReportCard.top), dp(kReportCard.right),
                dp(kReportCard.bottom)};
}

int reportLineHeight() { return ui::textHeightPx(ui::fonts().body); }

// content/page/pos of the report in device pixels, for the slim thumb.
void reportScrollState(int* content, int* page, int* pos) {
    RECT client{};
    GetClientRect(g.report, &client);
    const int lineHeight = reportLineHeight();
    *content = static_cast<int>(SendMessageW(g.report, EM_GETLINECOUNT, 0, 0)) * lineHeight;
    *page = client.bottom - client.top;
    *pos = static_cast<int>(SendMessageW(g.report, EM_GETFIRSTVISIBLELINE, 0, 0)) * lineHeight;
}

void invalidateReportGutter() {
    const RECT card = reportCardRect();
    const RECT gutter{card.right - dp(12), card.top, card.right, card.bottom};
    InvalidateRect(g.window, &gutter, FALSE);
}

// The EDIT scrolls itself on wheel, keys and selection drags; the thumb is
// painted by the parent and has to follow.
LRESULT CALLBACK reportProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT_PTR /*id*/, DWORD_PTR /*data*/) {
    // A multiline EDIT scrolls on the wheel only when it has WS_VSCROLL, and
    // this one deliberately has no scroll bar -- the card draws its own slim
    // gutter instead. So the control ignores the tick however it is
    // delivered, and routing it correctly (which the pump now does) was
    // necessary but not sufficient: two separate faults, one symptom, and
    // the first fix looked like it had simply failed.
    //
    // So scroll it here. EM_LINESCROLL is what the EDIT would have done for
    // itself with the style it does not have.
    if (message == WM_MOUSEWHEEL) {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == 0) lines = 3;          // "do not scroll" -- still scroll
        if (lines > 24) lines = 24;         // WHEEL_PAGESCROLL is 0xFFFFFFFF
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int step = -delta * static_cast<int>(lines) / WHEEL_DELTA;
        if (step != 0) SendMessageW(hwnd, EM_LINESCROLL, 0, step);
        invalidateReportGutter();
        return 0;
    }
    const LRESULT out = DefSubclassProc(hwnd, message, wparam, lparam);
    switch (message) {
        case WM_KEYDOWN:
        case WM_MOUSEMOVE:
        case WM_TIMER:   // the EDIT's own drag-select autoscroll
            invalidateReportGutter();
            break;
        default:
            break;
    }
    return out;
}

void setReport(const std::string& utf8) {
    setText(g.report, utf8);
    // Scrolled to the END: outcomes are appended below the plan, and the
    // pane is the only place they are said now -- the freshest line must
    // be the visible one.
    const LRESULT len = SendMessageW(g.report, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(g.report, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(g.report, EM_SCROLLCARET, 0, 0);
    invalidateReportGutter();
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

void place(HWND hwnd, int x, int y, int w, int h, Screen screen, bool bothScreens = false) {
    Place p;
    p.hwnd = hwnd;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.screen = screen;
    p.bothScreens = bothScreens;
    g.places.push_back(p);
}

// The scope the prompt names: both, vr or flat. The combo is the only record of it.
std::string scopeChoice() {
    if (!g.scopeCombo) return "both";
    switch (SendMessageW(g.scopeCombo, CB_GETCURSEL, 0, 0)) {
        case 1: return "vr";
        case 2: return "flat";
        default: return "both";
    }
}

int scopeIndex(const std::string& scope) {
    return scope == "vr" ? 1 : scope == "flat" ? 2 : 0;
}

// The settings file the screen edits for a scope: the one scope that installs only one of
// them, else the file the game's 3D mode runs on (flat for modes 0 and 1, VR otherwise and when
// the mode is unknown).
std::string editionFor(const std::string& scope) {
    if (scope == "vr") return "vr";
    if (scope == "flat") return "flat";
    return g.mode.mode == edvr::GameMode::Flat ? "flat" : "vr";
}

// The VR/Flat selector offers only the files the scope installs, and shows the one in use.
void syncSettingsSelector() {
    if (!g.settingsVr || !g.settingsFlat) return;
    const std::string scope = scopeChoice();
    ui::setButtonStyle(g.settingsVr, g.settingsEdition == "vr" ? ui::ButtonStyle::TabActive : ui::ButtonStyle::Tab);
    ui::setButtonStyle(g.settingsFlat, g.settingsEdition == "flat" ? ui::ButtonStyle::TabActive : ui::ButtonStyle::Tab);
    const bool settingsShown = g.screen == Screen::Settings;
    ShowWindow(g.settingsVr, settingsShown && scope != "flat" ? SW_SHOW : SW_HIDE);
    ShowWindow(g.settingsFlat, settingsShown && scope != "vr" ? SW_SHOW : SW_HIDE);
}

void layoutPreflight();   // the Pre-flight tab's own controls, placed from its rows (below)

// Everything moved and shown or hidden for the current screen and scale.
//
// The first version of this took the window's size from GetDpiForSystem and the
// control positions from GetDpiForWindow. On a 150% display those are different
// numbers, so the controls were laid out half as big again as the window meant
// to hold them, and the right-hand third was off the edge.
void applyLayout() {
    for (const Place& p : g.places) {
        const bool visible = p.bothScreens || p.screen == g.screen;
        ShowWindow(p.hwnd, visible ? SW_SHOW : SW_HIDE);
        if (visible) MoveWindow(p.hwnd, dp(p.x), dp(p.y), dp(p.w), dp(p.h), TRUE);
    }

    // The tip link finishes a sentence that is painted, not a control, so where
    // it goes depends on how wide that sentence renders -- which changes with
    // the font, the scale and the language of the machine it is running on.
    // Both links are sized to their measured labels and sit on the sentence's own line (its rect's top, kTipTop), so the
    // link continues the sentence after one ordinary space, and "Ask on Discord" ends at the right margin.
    if (g.tip || g.discord) {
        HDC dc = GetDC(g.window);
        const HFONT caption = ui::fonts().caption;
        const int prefix = ui::textWidth(dc, kTipText, caption);
        const int space = ui::textWidth(dc, L" ", caption);
        const int tipWidth = ui::textWidth(dc, L"leave a tip", caption) + dp(2);
        const int discordWidth = ui::textWidth(dc, L"Ask on Discord", caption) + dp(2);
        ReleaseDC(g.window, dc);
        if (g.tip) MoveWindow(g.tip, dp(kMargin) + prefix + space, dp(kTipTop), tipWidth, dp(20), TRUE);
        if (g.discord)
            MoveWindow(g.discord, dp(kClientWidth - kMargin) - discordWidth, dp(kTipTop), discordWidth, dp(20), TRUE);
    }

    // A combo's field height is its item height plus its own frame, not
    // what MoveWindow asked for. The frame is measured and the item height
    // set to land the field at exactly the 30 units the Browse button
    // beside it gets; the text centres itself within the item.
    // The scope prompt's combo is sized the same way.
    for (HWND combo : {g.combo, g.scopeCombo}) {
        if (!combo) continue;
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, dp(24));  // dropdown rows
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), dp(22));
        RECT rc{};
        GetWindowRect(combo, &rc);
        const int frame = (rc.bottom - rc.top) - dp(22);
        const int fieldItem = dp(30) - frame;
        if (fieldItem > 0) {
            SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), fieldItem);
        }
    }

    // Single-line EDITs draw their text at the top of the control, so the
    // search box is sized to its text and centred within the frame painted
    // around it (250..282 in design units) instead of filling the frame with
    // the text riding high -- the same treatment the settings list gives its
    // inline value editor.
    if (g.search) {
        const int textH = ui::textHeightPx(ui::fonts().body);
        const int frameTop = dp(250);
        const int frameH = dp(282) - frameTop;
        const int y = frameTop + (frameH > textH ? (frameH - textH) / 2 : 0);
        MoveWindow(g.search, dp(kMargin + kCardPad), y, dp(310), textH, TRUE);
    }

    // Shown or hidden by the scope, not by the screen alone, so it runs after the loop above.
    syncSettingsSelector();
    layoutPreflight();
    InvalidateRect(g.window, nullptr, TRUE);
}

void sizeWindowToLayout() {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.window, GWL_STYLE));
    RECT wanted{0, 0, dp(kClientWidth), dp(kClientHeight)};
    AdjustWindowRectExForDpi(&wanted, style, FALSE, 0, g.dpi);
    SetWindowPos(g.window, nullptr, 0, 0, wanted.right - wanted.left, wanted.bottom - wanted.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// what the folder looks like, as rows rather than as a paragraph
// ---------------------------------------------------------------------------

Tone toneForOurs(DllKind kind) {
    switch (kind) {
        case DllKind::Edvr: return Tone::Good;
        case DllKind::Absent: return Tone::Muted;
        case DllKind::Unreadable: return Tone::Bad;
        default: return Tone::Warn;
    }
}

void rebuildStatus() {
    g.status.clear();
    g.modeLine.clear();
    g.scopeLine.clear();
    g.pfStatusRow = -1;
    if (!g.haveSurvey) {
        g.statusPath.clear();
        return;
    }
    const Survey& s = g.survey;
    g.statusPath = s.game.dir;

    StatusRow fixes;
    fixes.label = L"The fixes";
    fixes.tone = toneForOurs(s.d3d11.kind);
    // A record older than the files says nothing about what is installed, its version included.
    const bool stale = installRecordStale(s);
    if (s.d3d11.kind == DllKind::Edvr) {
        fixes.value = L"EDVR installed as d3d11.dll";
        if (!s.state.edvrVersion.empty() && !stale) fixes.value += L" \x00b7 " + fromUtf8(s.state.edvrVersion);
    } else if (s.d3d11.kind == DllKind::Absent) {
        fixes.value = L"not installed";
    } else {
        fixes.value = describeDll(s.d3d11) + L" holds the d3d11.dll name";
    }
    g.status.push_back(fixes);

    StatusRow vr;
    vr.label = L"Flash fix, Explorer Cam";
    if (!s.haveOpenvrDir) {
        vr.tone = Tone::Warn;
        vr.value = L"no Openvr folder found under this install";
    } else if (s.openvrCurrent.kind == DllKind::Edvr) {
        vr.tone = Tone::Good;
        vr.value = L"EDVR installed as openvr_api.dll";
    } else if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
        vr.tone = Tone::Muted;
        vr.value = L"not installed \x00b7 the game's own runtime is in place";
    } else {
        vr.tone = toneForOurs(s.openvrCurrent.kind);
        vr.value = describeDll(s.openvrCurrent);
    }
    g.status.push_back(vr);

    if (stale) {
        StatusRow record;
        record.label = L"Install record";
        record.tone = Tone::Warn;
        record.value = L"older than the files here; Update refreshes it";
        g.status.push_back(record);
    }

    // The game's runtime depends on the scope: flat expects the game's own openvr_api.dll, and both and vr
    // run EDVR's own runtime as openvr_api.dll.
    const std::string scope = scopeChoice();
    if (s.haveOpenvrDir) {
        StatusRow orig;
        orig.label = L"The game's runtime";
        if (scope == "flat") {
            if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Good;
                orig.value = L"the game's own runtime is in place";
            } else if (s.openvrOrig.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Good;
                orig.value = L"safe, renamed openvr_api_orig.dll";
            } else if (s.openvrCurrent.kind == DllKind::Edvr) {
                orig.tone = Tone::Warn;
                orig.value = L"EDVR's runtime is in place; flat restores the game's own";
            } else {
                orig.tone = Tone::Muted;
                orig.value = L"not found";
            }
        } else {
            if (s.openvrCurrent.kind == DllKind::Edvr) {
                orig.tone = Tone::Good;
                orig.value = L"EDVR's own runtime is in place";
            } else if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Muted;
                orig.value = L"the game's own runtime; the install replaces it";
            } else if (s.openvrOrig.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Good;
                orig.value = L"safe, renamed openvr_api_orig.dll";
            } else {
                orig.tone = Tone::Muted;
                orig.value = L"not found";
            }
        }
        g.status.push_back(orig);
    }

    // One row for the scope's settings files, each with whether it is here and kept. The flat file is read
    // from edvr-flat.ini and falls back to the shared edvr.ini until it has its own.
    StatusRow settings;
    settings.label = L"Your settings";
    std::wstring parts;
    bool allHere = true;
    if (scope != "flat") {
        parts += s.iniPresent ? L"edvr.ini kept" : L"edvr.ini not yet";
        allHere = allHere && s.iniPresent;
    }
    if (scope == "both") parts += L"; ";
    if (scope != "vr") {
        parts += s.flatIniPresent ? L"edvr-flat.ini kept"
                                  : (s.iniPresent ? L"edvr-flat.ini not yet (edvr.ini is copied into it)"
                                                  : L"edvr-flat.ini not yet");
        allHere = allHere && s.flatIniPresent;
    }
    settings.value = parts;
    settings.tone = allHere ? Tone::Good : Tone::Muted;
    g.status.push_back(settings);

    if (!s.state.chainTarget.empty()) {
        StatusRow chain;
        chain.label = L"Alongside";
        chain.tone = Tone::Good;
        chain.value = (s.state.chainMod.empty() ? std::wstring(L"another mod")
                                                : s.state.chainMod) +
                      L", chained through " + s.state.chainTarget;
        g.status.push_back(chain);
    }

    if (s.gameRunningHere) {
        StatusRow running;
        running.label = L"Elite Dangerous";
        running.tone = Tone::Bad;
        running.value = L"running \x2014 close it before installing anything";
        g.status.push_back(running);
    } else if (s.gameRunningElsewhere) {
        // Not a warning: the copy running belongs to another install and this
        // one is free. Said anyway, because somebody who can see the game on
        // their other monitor is owed the reason the buttons are still live.
        StatusRow running;
        running.label = L"Elite Dangerous";
        running.tone = Tone::Muted;
        running.value = L"running from a different folder \x2014 not this install";
        g.status.push_back(running);
    } else if (s.gameRunStateUnknown) {
        // The buttons stay live on purpose: a click re-surveys, which may well
        // succeed the second time, and when it does not the plan says why it
        // refuses rather than the buttons being greyed with no reason given.
        StatusRow running;
        running.label = L"Elite Dangerous";
        running.tone = Tone::Warn;
        running.value = L"could not check whether it is running \x2014 nothing will be installed";
        g.status.push_back(running);
    }

    // Pre-flight: one row saying whether anything is left to set, and a click on it opens the tab. Only for a
    // folder that has EDVR's settings to set (the rows are read from the Pre-flight tab's own refresh).
    if (!g.pfRows.empty() && (s.iniPresent || s.flatIniPresent)) {
        StatusRow pre;
        pre.label = L"Pre-flight";
        const int pending = pendingCount(g.pfRows);
        pre.value = fromUtf8(preflightSummary(g.pfRows)) + (pending > 0 ? L" \x2014 open the Pre-flight tab" : L"");
        pre.tone = pending > 0 ? Tone::Warn : Tone::Good;
        g.pfStatusRow = static_cast<int>(g.status.size());
        g.status.push_back(pre);
    }

    // Elite's 3D mode decides which half of EDVR runs. Said in a sentence under the rows, and
    // when the scope leaves that half out, said again: somebody who installed VR only and
    // then turned 3D off is owed the reason the flat fixes are not there.
    if (!g.mode.known) {
        g.modeLine = L"Elite's 3D mode is unknown: EDVR starts in VR.";
    } else if (g.mode.mode == edvr::GameMode::Flat) {
        g.modeLine = L"Elite's 3D mode is off: EDVR runs its flat fixes.";
    } else {
        g.modeLine = L"Elite's 3D mode is on: EDVR runs its VR fixes.";
    }
    if (g.mode.mode == edvr::GameMode::Flat && scope == "vr") {
        g.scopeLine = L"This install does not include flat: EDVR stays out of the way.";
    } else if (g.mode.mode == edvr::GameMode::Vr && scope == "flat") {
        g.scopeLine = L"This install does not include VR: EDVR stays out of the way.";
    }
}

COLORREF colourFor(Tone tone) {
    const ui::Theme& t = ui::theme();
    switch (tone) {
        case Tone::Good: return t.good;
        case Tone::Warn: return t.warn;
        case Tone::Bad: return t.bad;
        default: return t.muted;
    }
}

Tone toneOf(RowTone tone) {
    switch (tone) {
        case RowTone::Good: return Tone::Good;
        case RowTone::Warn: return Tone::Warn;
        case RowTone::Bad: return Tone::Bad;
        default: return Tone::Muted;
    }
}

// The status card's rows are 20 apart; with the Pre-flight row there can be eight, and eight at 20 would push the
// two lines under them out of the card, so they close up to 18.
int statusPitch() { return g.status.size() >= 8 ? 18 : 20; }

// Is this client point on the Setup tab's Pre-flight row? The rows are painted, not controls, so the hit test is
// the paint's own arithmetic (paintInstallScreen: rows from y=202, statusPitch() apart).
bool pfStatusRowAt(int x, int y) {
    if (g.pfStatusRow < 0) return false;
    const int top = 202 + g.pfStatusRow * statusPitch();
    return x >= dp(kMargin + kCardPad) && x < dp(kClientWidth - kMargin - kCardPad) && y >= dp(top) &&
           y < dp(top + statusPitch());
}

// ---------------------------------------------------------------------------
// Pre-flight: the rows behind the tab, and where they sit
// ---------------------------------------------------------------------------

// The scope the rows are for: what is installed here, else what the Setup tab is about to install.
std::string pfScope() {
    const std::string installed = installedScope(g.survey);
    return installed.empty() ? scopeChoice() : installed;
}

bool pfAnyTicked() {
    for (const auto& kv : g.pfTicks)
        if (kv.second) return true;
    return false;
}

void updatePreflightButtons() {
    if (g.pfSetUp) EnableWindow(g.pfSetUp, g.haveSurvey && pfAnyTicked());
}

// Reads the machine again and rebinds the checkboxes. The user's ticks survive a refresh (by row id); a row that
// is not tickable any more forgets its tick, so it starts from its default if it comes back.
void refreshPreflight() {
    g.pfRows.clear();
    g.pfBoxRow.assign(g.pfBoxes.size(), -1);
    if (!g.haveSurvey) {
        g.pfTicks.clear();
        updatePreflightButtons();
        return;
    }
    g.pfRows = recommendPreflight(collectPreflightInputs(g.survey, payloadInfo(), pfScope()));

    std::map<std::string, bool> kept;
    size_t box = 0;
    for (size_t i = 0; i < g.pfRows.size() && box < g.pfBoxes.size(); ++i) {
        const PreflightRow& r = g.pfRows[i];
        if (r.kind != RowKind::EdvrWrite || !r.checkable) continue;
        const auto earlier = g.pfTicks.find(r.id);
        const bool on = earlier != g.pfTicks.end() ? earlier->second : r.tickedByDefault;
        kept[r.id] = on;
        g.pfBoxRow[box] = static_cast<int>(i);
        SetWindowTextW(g.pfBoxes[box], fromUtf8(r.label).c_str());
        ui::setCheckbox(g.pfBoxes[box], on);
        InvalidateRect(g.pfBoxes[box], nullptr, TRUE);
        ++box;
    }
    g.pfTicks = kept;
    updatePreflightButtons();
}

struct PfSlot {
    size_t index = 0;   // into g.pfRows
    int    y = 0;
    int    height = 0;
};
struct PfLayout {
    std::vector<PfSlot> slots;
    int flyHeadingY = 0;
};

PfLayout pfLayout() {
    PfLayout layout;
    int y = kPfRigHeadingY + 20;
    for (size_t i = 0; i < g.pfRows.size(); ++i) {
        if (g.pfRows[i].group != RowGroup::Rig) continue;
        layout.slots.push_back({i, y, kPfRigRow});
        y += kPfRigRow;
    }
    y += 10;
    layout.flyHeadingY = y;
    y += 24;
    for (size_t i = 0; i < g.pfRows.size(); ++i) {
        if (g.pfRows[i].group != RowGroup::Fly) continue;
        const int height = g.pfRows[i].kind == RowKind::Warn ? kPfWarnRow : kPfFlyRow;
        layout.slots.push_back({i, y, height});
        y += height;
    }
    return layout;
}

// The tab's own controls: each bound checkbox beside its row's label, and the undo link only when there is
// something to undo. Called from applyLayout and again whenever the rows change.
void layoutPreflight() {
    const bool shown = g.screen == Screen::Preflight && g.haveSurvey;
    const PfLayout layout = pfLayout();
    for (size_t b = 0; b < g.pfBoxes.size(); ++b) {
        const int row = b < g.pfBoxRow.size() ? g.pfBoxRow[b] : -1;
        const PfSlot* slot = nullptr;
        for (const PfSlot& s : layout.slots)
            if (row >= 0 && s.index == static_cast<size_t>(row)) slot = &s;
        if (shown && slot) {
            MoveWindow(g.pfBoxes[b], dp(kPfLeft), dp(slot->y), dp(kPfNowX - 8 - kPfLeft), dp(20), TRUE);
            ShowWindow(g.pfBoxes[b], SW_SHOW);
        } else {
            ShowWindow(g.pfBoxes[b], SW_HIDE);
        }
    }
    if (g.pfUndo) {
        const bool offered = shown && g.survey.state.preflight.present();
        if (offered) {
            HDC dc = GetDC(g.window);
            const int width = ui::textWidth(dc, L"Put my old settings back", ui::fonts().caption) + dp(2);
            ReleaseDC(g.window, dc);
            MoveWindow(g.pfUndo, dp(kPfLeft + 156), dp(kPfButtonY + 8), width, dp(20), TRUE);
        }
        ShowWindow(g.pfUndo, offered ? SW_SHOW : SW_HIDE);
    }
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------

// The install picker and the folder it names sit above both screens -- and
// above the TABS: the settings being edited are the settings of whichever
// install is selected, and a Settings tab that does not say which folder it
// is writing to is asking to be used on the wrong one.
void paintInstallPicker(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    if (g.statusPath.empty()) return;
    RECT path{dp(kMargin), dp(122), dp(kClientWidth - kMargin), dp(140)};
    ui::drawText(dc, g.statusPath, path, f.caption, t.subtext,
                 DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
}

void paintInstallScreen(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();

    // The window is not taller than it was, so the status card, the actions card
    // and the report card are fitted into the space the one-install layout left.
    RECT card{dp(kMargin), dp(186), dp(kClientWidth - kMargin), dp(384)};
    ui::paintCard(dc, card, g.dpi);

    int y = 202;
    for (const StatusRow& row : g.status) {
        const int centre = dp(y + 10);
        ui::fillCircle(dc, dp(kMargin + kCardPad + 5), centre, dp(4), colourFor(row.tone));

        RECT label{dp(kMargin + kCardPad + 20), dp(y), dp(kMargin + kCardPad + 180), dp(y + 20)};
        ui::drawText(dc, row.label, label, f.body, t.subtext,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT value{dp(kMargin + kCardPad + 190), dp(y), dp(kClientWidth - kMargin - kCardPad),
                   dp(y + 20)};
        ui::drawText(dc, row.value, value, f.body, t.text,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        // 20 apart, not 22: seven rows and the two lines under them fit the card at the window's fixed height.
        y += statusPitch();
    }

    if (!g.modeLine.empty()) {
        RECT mode{dp(kMargin + kCardPad), dp(y + 2), dp(kClientWidth - kMargin - kCardPad), dp(y + 20)};
        ui::drawText(dc, g.modeLine, mode, f.caption, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!g.scopeLine.empty()) {
            RECT scope{dp(kMargin + kCardPad), dp(y + 20), dp(kClientWidth - kMargin - kCardPad),
                       dp(y + 38)};
            ui::drawText(dc, g.scopeLine, scope, f.caption, colourFor(Tone::Warn),
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // The scope prompt sits on the tab row, to the right of the tabs.
    RECT scopeLabel{dp(372), dp(146), dp(484), dp(176)};
    ui::drawText(dc, L"Install EDVR for:", scopeLabel, f.body, t.subtext, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT actions{dp(kMargin), dp(394), dp(kClientWidth - kMargin), dp(512)};
    ui::paintCard(dc, actions, g.dpi);

    RECT reassure{dp(kMargin + kCardPad), dp(478), dp(kClientWidth - kMargin - kCardPad), dp(498)};
    ui::drawText(dc,
                 L"Nothing is changed until you confirm it, and every file replaced is copied "
                 L"into edvr_backup\\ first.",
                 reassure, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    const RECT report = reportCardRect();
    ui::paintCard(dc, report, g.dpi);

    if (g.report) {
        int content = 0, page = 0, pos = 0;
        reportScrollState(&content, &page, &pos);
        ui::drawSlimScrollbar(dc, report, content, page, pos, g.dpi, g.reportDrag);
    }
}

// One Pre-flight row. A rig row is a dot, a label and a value with a note beside it. A row EDVR can set has a
// checkbox (a control, bound to it) where the dot would be; the rest have the dot. Then the columns:
// now, an arrow, after -- or, for a value that is already right, that said in the "after" place.
void paintPreflightRow(HDC dc, const PreflightRow& r, int top) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const COLORREF dot = colourFor(toneOf(r.tone));

    if (r.group == RowGroup::Rig) {
        ui::fillCircle(dc, dp(kMargin + kCardPad + 5), dp(top + 10), dp(4), dot);
        RECT label{dp(kPfLeft + 20), dp(top), dp(kPfLeft + 180), dp(top + 20)};
        ui::drawText(dc, fromUtf8(r.label), label, f.body, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::wstring value = fromUtf8(r.now);
        const int valueX = kPfLeft + 190;
        RECT valueRect{dp(valueX), dp(top), dp(kPfRight), dp(top + 20)};
        ui::drawText(dc, value, valueRect, f.body, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!r.why.empty()) {
            const int used = ui::textWidth(dc, value, f.body) + dp(12);
            RECT note{dp(valueX) + used, dp(top + 2), dp(kPfRight), dp(top + 20)};
            if (note.left < note.right)
                ui::drawText(dc, fromUtf8(r.why), note, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        return;
    }

    const bool boxed = r.kind == RowKind::EdvrWrite && r.checkable;
    if (!boxed) ui::fillCircle(dc, dp(kMargin + kCardPad + 5), dp(top + 10), dp(4), dot);

    if (r.kind == RowKind::Warn) {
        // "RivaTuner is running." in the warning colour, then what to do about it, on the same line.
        const std::wstring head = fromUtf8(r.label);
        RECT headRect{dp(kPfLeft + 28), dp(top), dp(kPfRight), dp(top + 20)};
        ui::drawText(dc, head, headRect, f.bodyBold, colourFor(Tone::Warn), DT_LEFT | DT_SINGLELINE);
        const int used = ui::textWidth(dc, head, f.bodyBold) + dp(8);
        RECT rest{dp(kPfLeft + 28) + used, dp(top), dp(kPfRight), dp(top + 20)};
        if (rest.left < rest.right)
            ui::drawText(dc, fromUtf8(r.why), rest, f.body, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    if (!boxed) {
        RECT label{dp(kPfLeft + 28), dp(top), dp(kPfNowX - 8), dp(top + 20)};
        ui::drawText(dc, fromUtf8(r.label), label, f.body, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (!r.now.empty()) {
        RECT now{dp(kPfNowX), dp(top), dp(kPfArrowX - 4), dp(top + 20)};
        ui::drawText(dc, fromUtf8(r.now), now, f.body, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (r.alreadyGood && !r.after.empty()) {
        RECT done{dp(kPfArrowX), dp(top), dp(kPfRight), dp(top + 20)};
        ui::drawText(dc, fromUtf8(r.after), done, f.body, t.good, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (!r.after.empty()) {
        RECT arrow{dp(kPfArrowX), dp(top), dp(kPfAfterX - 4), dp(top + 20)};
        ui::drawText(dc, L"\x2192", arrow, f.body, t.subtext, DT_LEFT | DT_SINGLELINE);
        RECT after{dp(kPfAfterX), dp(top), dp(kPfRight), dp(top + 20)};
        ui::drawText(dc, fromUtf8(r.after), after, f.bodyBold, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (!r.why.empty()) {
        RECT why{dp(kPfLeft + 28), dp(top + 18), dp(kPfRight), dp(top + 34)};
        ui::drawText(dc, fromUtf8(r.why), why, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void paintPreflightScreen(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    RECT card{dp(kMargin), dp(186), dp(kClientWidth - kMargin), dp(kPfCardBottom)};
    ui::paintCard(dc, card, g.dpi);

    if (!g.haveSurvey) {
        RECT note{dp(kPfLeft), dp(202), dp(kPfRight), dp(222)};
        ui::drawText(dc, L"Pick an install above first.", note, f.caption, t.subtext,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    const PfLayout layout = pfLayout();
    RECT rigHeading{dp(kPfLeft), dp(kPfRigHeadingY), dp(kPfRight), dp(kPfRigHeadingY + 16)};
    ui::drawText(dc, L"YOUR RIG", rigHeading, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE);

    RECT flyHeading{dp(kPfLeft), dp(layout.flyHeadingY), dp(kPfNowX - 8), dp(layout.flyHeadingY + 16)};
    ui::drawText(dc, L"BEFORE YOU FLY", flyHeading, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE);
    RECT nowHead{dp(kPfNowX), dp(layout.flyHeadingY), dp(kPfArrowX - 4), dp(layout.flyHeadingY + 16)};
    ui::drawText(dc, L"now", nowHead, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE);
    RECT afterHead{dp(kPfAfterX), dp(layout.flyHeadingY), dp(kPfRight), dp(layout.flyHeadingY + 16)};
    ui::drawText(dc, L"after", afterHead, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE);

    for (const PfSlot& slot : layout.slots) paintPreflightRow(dc, g.pfRows[slot.index], slot.y);

    if (!g.pfResult.empty()) {
        RECT result{dp(kPfLeft), dp(kPfResultY), dp(kPfRight), dp(kPfResultY + 20)};
        ui::drawText(dc, g.pfResult, result, f.body, colourFor(toneOf(g.pfResultTone)),
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RECT caption{dp(kPfLeft), dp(kPfCaptionY), dp(kPfRight), dp(kPfCaptionY + 18)};
    ui::drawText(dc, L"Everything changed is copied to edvr_backup\\ first.", caption, f.caption, t.subtext,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void paintSettingsScreen(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    RECT card{dp(kMargin), dp(186), dp(kClientWidth - kMargin), dp(648)};
    ui::paintCard(dc, card, g.dpi);

    // No "Settings" heading inside the card: the active tab above already
    // says it, and a heading repeating a tab is the same redundancy the
    // Setup tab was renamed to avoid. The note carries the useful part.
    RECT note{dp(kMargin + kCardPad), dp(202), dp(kClientWidth - kMargin - kCardPad), dp(222)};
    const std::wstring where =
        g.settings.loaded()
            ? L"Written into the " + std::wstring(settingsLeafFor(g.settingsEdition)) +
                  L" of the install above, and live within a second. The few that need a game "
                  L"restart are marked."
            : L"Pick an install above first.";
    ui::drawText(dc, where, note, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // The search box is a real edit control with its 3D border taken off, so
    // the frame around it is drawn here to match everything else.
    // Its right edge stops 8 units short of the VR/Flat selector at x=370, so the two read as separate controls.
    RECT search{dp(kMargin + kCardPad) - dp(6), dp(250), dp(kMargin + kCardPad) + dp(318),
                dp(282)};
    ui::fillRounded(dc, search, dp(6), t.control);
    ui::strokeRounded(dc, search, dp(6), t.controlBorder);
}

void paintWindow(HWND window) {
    PAINTSTRUCT ps;
    HDC screenDc = BeginPaint(window, &ps);

    RECT client;
    GetClientRect(window, &client);

    // Drawn into a bitmap and blitted: cards, text and dots painted straight
    // onto the window flicker visibly every time the report pane changes.
    HDC dc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, client.right, client.bottom);
    HBITMAP previous = static_cast<HBITMAP>(SelectObject(dc, bitmap));

    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    ui::fillRect(dc, client, t.windowBg);

    RECT title{dp(kMargin), dp(18), dp(400), dp(50)};
    ui::drawText(dc, L"EDVR", title, f.title, t.text, DT_LEFT | DT_SINGLELINE);

    const std::wstring subtitle = fromUtf8(payloadInfo().version) +
        L"  \x00b7  unofficial patch for Elite Dangerous in VR and flat";
    RECT sub{dp(kMargin), dp(52), dp(kClientWidth - kMargin), dp(70)};
    ui::drawText(dc, subtitle, sub, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    paintInstallPicker(dc);
    if (g.screen == Screen::Install)
        paintInstallScreen(dc);
    else if (g.screen == Screen::Preflight)
        paintPreflightScreen(dc);
    else
        paintSettingsScreen(dc);

    RECT tip{dp(kMargin), dp(kTipTop), dp(kClientWidth - kMargin), dp(kTipTop + 20)};
    ui::drawText(dc, kTipText, tip, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE);

    BitBlt(screenDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &ps);
}

// ---------------------------------------------------------------------------
// behaviour
// ---------------------------------------------------------------------------

void showSurvey() {
    // Read again on every change: the game's 3D switch is flipped outside this window.
    g.mode = eliteGameMode();
    // The Pre-flight rows first: the Setup tab's status card carries a row made from them.
    refreshPreflight();
    rebuildStatus();
    layoutPreflight();
    if (!g.haveSurvey) {
        EnableWindow(g.install, FALSE);
        EnableWindow(g.repair, FALSE);
        EnableWindow(g.uninstall, FALSE);
        EnableWindow(g.collectLogs, FALSE);
        InvalidateRect(g.window, nullptr, TRUE);
        return;
    }

    // This folder's copy, not the machine's. Another install being in a jump
    // has no hold on the files here.
    const bool running = g.survey.gameRunningHere;
    EnableWindow(g.install, !running);
    EnableWindow(g.repair, !running);
    EnableWindow(g.uninstall, !running);
    // Collecting logs reads files and writes a zip somewhere else entirely, so
    // it is the one action that stays available while the game is running --
    // which is exactly when somebody wants it.
    EnableWindow(g.collectLogs, TRUE);

    const bool installed = g.survey.d3d11.kind == DllKind::Edvr ||
                           g.survey.openvrCurrent.kind == DllKind::Edvr || g.survey.state.present;
    SetWindowTextW(g.install, installed ? L"Update" : L"Install");
    InvalidateRect(g.window, nullptr, TRUE);
}

// Loads one settings file into the settings screen. Before a folder is chosen there
// is nothing to load; the choice is kept for when there is.
void loadSettings(const std::string& edition) {
    g.settingsEdition = edition;
    if (!g.haveSurvey) return;
    g.settings.load(g.survey.game.dir, edition);
    settingsListSetModel(g.settingsList, &g.settings, mirrorDirFor(g.survey.game));
}

void selectInstall(size_t index) {
    if (index >= g.installs.size()) {
        g.haveSurvey = false;
        showSurvey();
        return;
    }
    g.survey = surveyTarget(g.installs[index]);
    g.haveSurvey = true;
    g.pfTicks.clear();      // another folder starts from its own defaults, and its own last result
    g.pfResult.clear();
    // The scope this folder gets: the one the run was started with, else the folder's own, else both.
    SendMessageW(g.scopeCombo, CB_SETCURSEL, scopeIndex(g.args.scope.empty() ? defaultScope(g.survey) : g.args.scope), 0);
    g.mode = eliteGameMode();
    loadSettings(editionFor(scopeChoice()));
    showSurvey();
}

// The scope prompt changed. A settings screen on a file the new scope does not install moves to
// the one it does; the status and the selector follow.
void scopeChanged() {
    const std::string scope = scopeChoice();
    if (scope != "both" && scope != g.settingsEdition) loadSettings(scope);
    showSurvey();
    syncSettingsSelector();
}

void settingsEditionPicked(const std::string& edition) {
    if (edition == g.settingsEdition) return;
    loadSettings(edition);
    syncSettingsSelector();
    InvalidateRect(g.window, nullptr, TRUE);
}

void addInstall(const GameInstall& game) {
    for (size_t i = 0; i < g.installs.size(); ++i) {
        if (_wcsicmp(g.installs[i].dir.c_str(), game.dir.c_str()) == 0) {
            SendMessageW(g.combo, CB_SETCURSEL, i, 0);
            selectInstall(i);
            return;
        }
    }
    g.installs.push_back(game);
    const std::wstring label = game.source + L"  \x00b7  " + game.dir;
    SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    const size_t index = g.installs.size() - 1;
    SendMessageW(g.combo, CB_SETCURSEL, index, 0);
    selectInstall(index);
}

void scanForInstalls() {
    SendMessageW(g.combo, CB_RESETCONTENT, 0, 0);
    g.installs.clear();
    for (const GameInstall& game : findInstalls()) {
        g.installs.push_back(game);
        const std::wstring label = game.source + L"  \x00b7  " + game.dir;
        SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    if (g.installs.empty()) {
        g.haveSurvey = false;
        setReport(
                "No Elite Dangerous install was found.\r\n\r\n"
                "Press Browse and point at the folder that holds EliteDangerous64.exe. For a "
                "Frontier launcher install that is usually a folder ending in\r\n"
                "    Products\\elite-dangerous-odyssey-64");
        showSurvey();
        return;
    }

    // With more than one install -- and three storefronts on one machine is not
    // unusual -- the one that already has EDVR in it is the one being updated,
    // and a better guess than whichever store answered first.
    size_t pick = 0;
    for (size_t i = 0; i < g.installs.size(); ++i) {
        const Survey candidate = surveyTarget(g.installs[i]);
        if (candidate.state.present || candidate.d3d11.kind == DllKind::Edvr ||
            candidate.openvrCurrent.kind == DllKind::Edvr) {
            pick = i;
            break;
        }
    }
    SendMessageW(g.combo, CB_SETCURSEL, pick, 0);
    selectInstall(pick);
}

void browseForFolder() {
    // The modern picker, which shows the path and lets it be pasted in --
    // SHBrowseForFolder's tree is not a reasonable way to find a folder eight
    // levels down a Steam library.
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Pick the folder that holds EliteDangerous64.exe");

    std::wstring picked;
    if (SUCCEEDED(dialog->Show(g.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (picked.empty()) return;

    GameInstall game;
    if (!describeDir(picked, L"Folder you chose", &game)) {
        MessageBoxW(g.window,
                    L"There is no EliteDangerous64.exe in that folder, or in a Products folder "
                    L"under it.\n\nPick the folder that holds EliteDangerous64.exe itself.",
                    L"EDVR installer", MB_OK | MB_ICONWARNING);
        return;
    }
    addInstall(game);
}

std::wstring confirmText(const Plan& plan, const wchar_t* verb) {
    std::wstring text = std::wstring(verb) + L"\n\n";
    for (const std::string& note : plan.notes) text += L"\x2022 " + fromUtf8(note) + L"\n";
    if (!plan.problems.empty()) {
        text += L"\nWorth knowing:\n";
        for (const std::string& p : plan.problems) text += L"! " + fromUtf8(p) + L"\n";
    }
    text += L"\nA copy of every file replaced goes into edvr_backup\\.\n\nGo ahead?";
    return text;
}

void showScreen(Screen screen);

void runAction(AppArgs::Act action) {
    if (!g.haveSurvey) return;

    // The folder can change between the survey and the button -- the game gets
    // started, another installer runs. Ask the disk again rather than acting on
    // a picture that may be minutes old.
    g.survey = surveyTarget(g.survey.game);

    // A first install, as opposed to an update, repair or uninstall: the one the Pre-flight tab opens after.
    const bool freshInstall = action == AppArgs::Act::Install && g.survey.d3d11.kind != DllKind::Edvr &&
                              g.survey.openvrCurrent.kind != DllKind::Edvr && !g.survey.state.present;

    AppArgs args = g.args;
    args.action = action;
    args.dir = g.survey.game.dir;
    args.keepSettings = ui::checkboxChecked(g.chkKeep);
    args.removeSettings = false;
    args.scope = scopeChoice();

    const std::wstring mirrorDir = mirrorDirFor(g.survey.game);
    const bool canMirror = action != AppArgs::Act::Uninstall;

    // A folder with no settings file at all but a mirror outside it is exactly
    // the folder a game update just wiped. Asked about, not assumed -- unlike the
    // command line there is a window right here to ask in -- and only when
    // "keep my settings" is still checked; unchecking it says fresh defaults
    // are wanted, and old settings reappearing anyway would be worse than not
    // finding the mirror at all.
    //
    // Asked in the chosen scope's terms: each file the scope installs that the
    // folder lacks, if the mirror holds it. A file the folder has is never restored over.
    if (canMirror && args.keepSettings) {
        const MirrorInfo mirror = readMirror(mirrorDir);
        const std::vector<std::string> missing = settingsToRestore(g.survey.game.dir, mirror, args.scope);
        if (!missing.empty()) {
            const std::wstring wanted = fromUtf8(settingsNamesOf(missing));
            const std::wstring text =
                L"This folder has no " + wanted + L", but settings from an earlier install of it "
                L"were found outside the game folder, last saved " +
                fromUtf8(mirror.savedUtc) +
                L".\n\nRestore them before installing? They are kept at\n" + mirror.dir +
                L"\nprecisely so a game update wiping this folder cannot take them too.";
            const int answer = MessageBoxW(g.window, text.c_str(), L"EDVR installer",
                                           MB_YESNO | MB_ICONQUESTION);
            if (answer == IDYES) {
                std::vector<std::string> notes;
                const bool restored = restoreFromMirror(g.survey.game.dir, mirror, args.scope, &notes);
                if (!restored) {
                    // Said, not swallowed: some of the saved settings did not come
                    // back, and carrying on quietly would install over the gap.
                    std::wstring why = L"Not all of your saved settings could be restored:\n\n";
                    for (const std::string& n : notes) why += L"\x2022 " + fromUtf8(n) + L"\n";
                    why += L"\nThe install carries on with what is in the folder now.";
                    MessageBoxW(g.window, why.c_str(), L"EDVR installer", MB_OK | MB_ICONWARNING);
                }
                // Whatever did come back is what the plan is for.
                g.survey = surveyTarget(g.survey.game);
            }
        }
    }
    // A scope change is planned and shown in the report like any other install:
    // there is no conversion prompt, and the planner's own proofs gate it.
    const Options options = optionsFor(args, action == AppArgs::Act::Repair);
    const Plan plan = action == AppArgs::Act::Uninstall
                          ? planUninstall(g.survey, options, payloadInfo())
                          : planInstall(g.survey, options, payloadInfo());

    setReport(planReport(plan));

    // Outcomes land in the report pane, not a message box: the pane is
    // right there, it scrolls to the freshest line, and it can be read
    // again after the fact. Only QUESTIONS get a box from here on.
    if (plan.blocked || plan.nothingToDo) {
        // Nothing changed in the folder, but this may be the first run of an
        // installer new enough to mirror at all -- an install already at the
        // latest build must not have to wait for its next update to get one.
        if (canMirror && plan.nothingToDo) updateMirror(g.survey.game.dir, plan.backupDir, mirrorDir);
        setReport(planReport(plan) + "\r\n----\r\n" + planSummary(plan) + "\r\n");
        showSurvey();
        return;
    }

    const wchar_t* verb = action == AppArgs::Act::Uninstall ? L"About to remove EDVR:"
                          : action == AppArgs::Act::Repair  ? L"About to repair this install:"
                                                            : L"About to install EDVR:";
    if (!args.autorun) {
        // No is the default button, deliberately. Install is this window's
        // default push button, so Return reaches it -- and if the confirmation
        // took Return as yes, two stray keystrokes at a window that had just
        // taken the foreground would be enough to modify a game folder. That
        // is not hypothetical: it happened twice during development, to a real
        // install, while windows were being opened for screenshots.
        const int answer = MessageBoxW(g.window, confirmText(plan, verb).c_str(), L"EDVR installer",
                                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
    }

    if (needsElevationFor(g.survey) && !isElevated()) {
        const int answer = MessageBoxW(
            g.window,
            L"This game folder can only be written to by an administrator (it is usually under "
            L"Program Files).\n\nRestart the installer with administrator rights and carry on?",
            L"EDVR installer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        AppArgs elevated = args;
        elevated.autorun = true;
        if (relaunchElevated(elevated)) {
            PostMessageW(g.window, WM_CLOSE, 0, 0);
        } else {
            setReport(planReport(plan) +
                      "\r\n----\r\nWindows would not start the installer with "
                      "administrator rights, so nothing was changed.\r\n");
        }
        return;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);

    std::string text = planReport(plan);
    text += "\r\n----\r\n";
    for (const std::string& line : result.done) text += "  " + line + "\r\n";
    if (result.ok) {
        text += "\r\nDone. Start Elite Dangerous; EDVR writes what it managed to install into "
                "edvr_logs\\ next to the game.\r\n";
        if (canMirror) {
            const MirrorResult m = updateMirror(g.survey.game.dir, plan.backupDir, mirrorDir);
            if (m.ok) {
                text += "\r\nMirrored ";
                for (size_t i = 0; i < m.saved.size(); ++i) {
                    if (i) text += ", ";
                    text += m.saved[i];
                }
                text += " to " + toUtf8(mirrorDir) +
                        " (outside the game folder, so a game update wiping this folder cannot "
                        "take it too).\r\n";
            }
        }
    } else {
        text += "\r\n" + result.error + "\r\n";
        if (result.rolledBack) {
            text += "Installed files and metadata were restored; recovery backups were kept.\r\n";
        }
    }
    setReport(text);

    g.survey = surveyTarget(g.survey.game);
    showSurvey();
    // The next job after a first install is making the first flight good; the tab is right there with every
    // row ticked. Not after an update or a failure: those people know where they are.
    if (result.ok && freshInstall) showScreen(Screen::Preflight);
}

// One zip on the Desktop with the logs, the breadcrumbs, any fatal note, the
// settings file and the install record -- the four things the README asks a
// reporter to attach, from three different folders, chosen from the right
// session.
void saveLogs() {
    if (!g.haveSurvey) return;
    const LogBundle bundle = collectLogs(g.survey.game.dir, desktopFolder());

    std::string text;
    if (bundle.ok) {
        text = "Saved " + toUtf8(bundle.zipPath) + "\r\n\r\nIt contains:\r\n";
        for (const std::wstring& name : bundle.included) text += "  " + toUtf8(name) + "\r\n";
        if (!bundle.notes.empty()) {
            text += "\r\n";
            for (const std::string& note : bundle.notes) text += "  " + note + "\r\n";
        }
        text += "\r\nAttach it to a GitHub issue, or drop it on Discord. It holds EDVR's own "
                "logs and settings and nothing else.\r\n";
        if (!bundle.dumpZipPath.empty()) {
            // The dump is a separate file for a reason: it is a copy of the game's
            // memory, which is not for a public issue.
            text += "\r\nThe game crashed in that session, so its crash dump is saved apart from the logs: " +
                    toUtf8(bundle.dumpZipPath) +
                    "\r\nIt holds a copy of the game's memory. Do not attach it to a public issue; "
                    "send it to the developer privately if asked.\r\n";
        }
    } else {
        text = bundle.error + "\r\n";
        for (const std::string& note : bundle.notes) text += "  " + note + "\r\n";
    }
    setReport(text);
}
void setPreflightResult(const std::wstring& text, RowTone tone) {
    g.pfResult = text;
    g.pfResultTone = tone;
    InvalidateRect(g.window, nullptr, FALSE);
}

// Set up for me (undo false) and Put my old settings back (undo true): the install flow in miniature. Ask the
// disk again, plan, say what is about to happen and wait for a yes (No is the default button, for the reason
// runAction gives), ask for administrator rights only if the folder needs them, apply, mirror the settings,
// and say what happened in one line under the rows.
//
// `confirmed` is the elevated relaunch, which was confirmed before it started; `onlyRows` is the rows that
// copy was told to set (the user's ticks travel with it), null otherwise.
void runPreflightAction(bool undo, bool confirmed, const std::vector<std::string>* onlyRows) {
    if (!g.haveSurvey) return;
    g.survey = surveyTarget(g.survey.game);
    const std::string scope = pfScope();
    const std::vector<PreflightRow> rows =
        recommendPreflight(collectPreflightInputs(g.survey, payloadInfo(), scope));
    std::vector<std::string> ticked;
    if (onlyRows) {
        ticked = *onlyRows;
    } else {
        for (const auto& kv : g.pfTicks)
            if (kv.second) ticked.push_back(kv.first);
    }

    AppArgs args = g.args;
    args.action = AppArgs::Act::Preflight;
    args.dir = g.survey.game.dir;
    args.scope = scope;
    args.preflightApply = !undo;
    args.preflightUndo = undo;
    args.rowsGiven = !undo;
    args.rows = ticked;

    const Options options = optionsFor(args, false);
    const Plan plan = undo ? planPreflightUndo(g.survey, options) : planPreflight(g.survey, rows, ticked, options);
    if (plan.blocked || plan.nothingToDo) {
        const std::string& first = plan.blocked ? (plan.problems.empty() ? std::string("That could not be planned.") : plan.problems.front())
                                                : (plan.notes.empty() ? std::string("Nothing to do.") : plan.notes.front());
        setPreflightResult(fromUtf8(first), plan.blocked ? RowTone::Bad : RowTone::Muted);
        showSurvey();
        return;
    }
    const std::string readOnly = readOnlySettingsProblem(plan);
    if (!readOnly.empty()) {
        setPreflightResult(fromUtf8(readOnly), RowTone::Bad);
        return;
    }

    if (!confirmed) {
        const wchar_t* verb = undo ? L"About to put your old settings back:" : L"About to set up EDVR for your first flight:";
        const int answer = MessageBoxW(g.window, confirmText(plan, verb).c_str(), L"EDVR installer",
                                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
    }

    // Only the game folder is written: the backup, the record and the settings files all live in it.
    if (!canWriteInto(g.survey.game.dir) && !isElevated()) {
        const int answer = MessageBoxW(
            g.window,
            L"This game folder can only be written to by an administrator (it is usually under "
            L"Program Files).\n\nRestart the installer with administrator rights and carry on?",
            L"EDVR installer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        AppArgs elevated = args;
        elevated.autorun = true;
        if (relaunchElevated(elevated)) {
            PostMessageW(g.window, WM_CLOSE, 0, 0);
        } else {
            setPreflightResult(L"Windows would not start the installer with administrator rights, so nothing was changed.",
                               RowTone::Bad);
        }
        return;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);
    if (!result.ok) {
        std::string why = result.error;
        if (result.rolledBack) why += " Everything was put back as it was.";
        setPreflightResult(fromUtf8(why), RowTone::Bad);
        return;
    }
    updateMirrorIni(g.survey.game.dir, mirrorDirFor(g.survey.game));

    // Ticks start again from the defaults: what was set is "already set" now, and what was put back is a
    // default again.
    g.pfTicks.clear();
    g.survey = surveyTarget(g.survey.game);
    showSurvey();

    if (undo) {
        bool leftAlone = false;
        for (const std::string& n : plan.notes)
            if (n.rfind("Left ", 0) == 0) leftAlone = true;
        setPreflightResult(leftAlone ? L"Your old settings are back. Anything you changed since was left alone."
                                     : L"Your old settings are back.",
                           RowTone::Good);
    } else {
        // The rows the plan set, not the ticks: a tick kept from an earlier refresh can name a row that has
        // nothing left to set.
        size_t changed = 0;
        for (const PreflightRow& r : rows)
            if (r.kind == RowKind::EdvrWrite && r.checkable &&
                std::find(ticked.begin(), ticked.end(), r.id) != ticked.end())
                ++changed;
        const std::wstring count = std::to_wstring(changed);
        setPreflightResult(L"Done. " + count + (changed == 1 ? L" setting" : L" settings") +
                               L" changed; the old values are copied to edvr_backup\\.",
                           RowTone::Good);
    }
}

void showScreen(Screen screen) {
    g.screen = screen;
    ui::setButtonStyle(g.tabInstall,
                       screen == Screen::Install ? ui::ButtonStyle::TabActive : ui::ButtonStyle::Tab);
    ui::setButtonStyle(g.tabPreflight, screen == Screen::Preflight ? ui::ButtonStyle::TabActive
                                                                   : ui::ButtonStyle::Tab);
    ui::setButtonStyle(g.tabSettings, screen == Screen::Settings ? ui::ButtonStyle::TabActive
                                                                 : ui::ButtonStyle::Tab);
    // The rig can have changed since the tab was last drawn (RivaTuner started, a runtime chosen).
    if (screen == Screen::Preflight) refreshPreflight();
    applyLayout();
}

void createControls(HWND window) {
    g.dpi = GetDpiForWindow(window);
    ui::buildFonts(g.dpi);
    const ui::Fonts& f = ui::fonts();

    // The tabs sit UNDER the install picker, not beside the title: the
    // screens are two views of whichever install is selected above, and the
    // reading order should say so. "Setup" rather than "Install", so the tab
    // names the screen and only the button names the action.
    g.tabInstall = ui::makeButton(window, L"Setup", kIdTabInstall, ui::ButtonStyle::TabActive,
                                  f.body);
    g.tabSettings = ui::makeButton(window, L"Settings", kIdTabSettings, ui::ButtonStyle::Tab,
                                   f.body);
    // Pre-flight sits between Setup and Settings: install, then make the first flight good, then tune.
    g.tabPreflight = ui::makeButton(window, L"Pre-flight", kIdTabPreflight, ui::ButtonStyle::Tab, f.body);
    place(g.tabInstall, kMargin, 146, 84, 30, Screen::Install, true);
    place(g.tabPreflight, kMargin + 92, 146, 104, 30, Screen::Install, true);
    place(g.tabSettings, kMargin + 204, 146, 92, 30, Screen::Install, true);

    // The scope prompt, on the same row as the tabs and drawn beside it as
    // "Install EDVR for:". Owner-drawn like the install combo, sized the same way.
    g.scopeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                       CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                   0, 0, 10, 10, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdScope)), nullptr,
                                   nullptr);
    SendMessageW(g.scopeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    for (const wchar_t* choice : kScopeChoices) {
        SendMessageW(g.scopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
    }
    SendMessageW(g.scopeCombo, CB_SETCURSEL, 0, 0);
    place(g.scopeCombo, 490, 146, 206, 30, Screen::Install);

    // The settings screen's file selector, two tab-like buttons. Which of them shows is
    // the scope's call (syncSettingsSelector), not the screen's alone.
    g.settingsVr = ui::makeButton(window, L"VR", kIdSettingsVr, ui::ButtonStyle::TabActive, f.body);
    g.settingsFlat = ui::makeButton(window, L"Flat", kIdSettingsFlat, ui::ButtonStyle::Tab, f.body);
    place(g.settingsVr, 370, 250, 150, 32, Screen::Settings);
    place(g.settingsFlat, 526, 250, 150, 32, Screen::Settings);

    // Still a real combo -- keyboard selection and the wheel are the two
    // things a hand-rolled dropdown gets subtly wrong -- but owner-drawn,
    // so the field and the list are painted in the house font and colours,
    // and its field height is converged on the Browse button's in
    // applyLayout (a combo sizes itself from its item height, not from
    // MoveWindow).
    g.combo = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                  CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                              0, 0, 10, 10, window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCombo)), nullptr,
                              nullptr);
    SendMessageW(g.combo, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    place(g.combo, kMargin, 88, 512, 30, Screen::Install, true);

    HWND browse = ui::makeButton(window, L"Browse...", kIdBrowse, ui::ButtonStyle::Secondary,
                                 f.body);
    place(browse, 568, 88, 108, 30, Screen::Install, true);

    // Seeded from the command line, because the elevated relaunch carries
     // --replace-settings and this checkbox is what runAction reads. Defaulting
     // it to checked threw that flag away without a word.
    g.chkKeep = ui::makeCheckbox(window, L"Keep the settings I have changed", kIdChkKeep,
                                 g.args.keepSettings, f.body);
    place(g.chkKeep, kMargin + kCardPad, 404, 420, 24, Screen::Install);

    g.install = ui::makeButton(window, L"Install", kIdInstall, ui::ButtonStyle::Primary, f.body);
    g.repair = ui::makeButton(window, L"Repair", kIdRepair, ui::ButtonStyle::Secondary, f.body);
    g.uninstall = ui::makeButton(window, L"Uninstall", kIdUninstall, ui::ButtonStyle::Secondary,
                                 f.body);
    g.collectLogs = ui::makeButton(window, L"Save logs", kIdCollectLogs,
                                   ui::ButtonStyle::Secondary, f.body);
    place(g.install, kMargin + kCardPad, 436, 124, 34, Screen::Install);
    place(g.repair, 172, 436, 104, 34, Screen::Install);
    place(g.uninstall, 284, 436, 104, 34, Screen::Install);
    place(g.collectLogs, 396, 436, 116, 34, Screen::Install);
    // The Pre-flight tab: Set up for me, the undo link (shown only when there is something to undo), and a pool of
    // checkboxes that refreshPreflight binds to the rows EDVR can set. Their places are the rows', not fixed ones,
    // so layoutPreflight positions them; they start hidden.
    g.pfSetUp = ui::makeButton(window, L"Set up for me", kIdPfSetUp, ui::ButtonStyle::Primary, f.body);
    place(g.pfSetUp, kPfLeft, kPfButtonY, 140, 34, Screen::Preflight);
    g.pfUndo = ui::makeButton(window, L"Put my old settings back", kIdPfUndo, ui::ButtonStyle::CardLink, f.caption);
    place(g.pfUndo, kPfLeft + 156, kPfButtonY + 8, 160, 20, Screen::Preflight);
    for (size_t i = 0; i < kPfBoxCount; ++i) {
        HWND box = ui::makeCheckbox(window, L"", kIdPfBox + static_cast<int>(i), false, f.body);
        ShowWindow(box, SW_HIDE);
        g.pfBoxes.push_back(box);
    }
    g.pfBoxRow.assign(g.pfBoxes.size(), -1);

    // No Close button: it was the one control at list height marked
    // both-screens, so it painted through the settings page -- and the
    // window's own close box already does the job.

    g.search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0,
                               10, 10, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)), nullptr,
                               nullptr);
    SendMessageW(g.search, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    SendMessageW(g.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search settings"));
    // 318 wide, so the box ends 8 units short of the VR button at x=370.
    place(g.search, kMargin + kCardPad, 252, 310, 28, Screen::Settings);

    g.settingsList = createSettingsList(window, kIdSettingsList, g.dpi);
    place(g.settingsList, kMargin + 2, 292, kClientWidth - 2 * kMargin - 4, 350, Screen::Settings);

    g.report = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY, 0,
                               0, 10, 10, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReport)), nullptr,
                               nullptr);
    SendMessageW(g.report, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    SetWindowSubclass(g.report, reportProc, 1, 0);
    place(g.report, kMargin + kCardPad - 4, 536, 636, 102, Screen::Install);

    g.tip = ui::makeButton(window, L"leave a tip", kIdKofi, ui::ButtonStyle::Link, f.caption);
    place(g.tip, kMargin, kTipTop, 76, 20, Screen::Install, true);  // x and width fixed up in applyLayout

    g.discord = ui::makeButton(window, L"Ask on Discord", kIdDiscord, ui::ButtonStyle::Link,
                               f.caption);
    place(g.discord, kClientWidth - kMargin - 110, kTipTop, 110, 20, Screen::Install, true);  // sized in applyLayout

    // Dark mode does not reach the native controls on its own. These two theme
    // names are what File Explorer uses for exactly this, and on a light theme
    // (or an older Windows) they simply do nothing.
    if (ui::theme().dark) {
        SetWindowTheme(g.combo, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.scopeCombo, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.report, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g.search, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.settingsList, L"DarkMode_Explorer", nullptr);
    }

    // Deliberately not an explanation of what Install, Repair and Uninstall do.
    // A first reader of this window already knows, and the pane is where the
    // plan goes -- filling it with a lecture beforehand made the one thing it
    // is for look like more of the same text. (Field feedback, on the first
    // build anybody but its author had seen.)
    setReport("What each button is about to do appears here, before anything is changed.");

    showScreen(Screen::Install);
    sizeWindowToLayout();
}

void rescale(HWND window, UINT dpi, const RECT* suggested) {
    g.dpi = dpi;
    ui::buildFonts(dpi);
    const ui::Fonts& f = ui::fonts();
    for (const Place& p : g.places) {
        SendMessageW(p.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    }
    SendMessageW(g.report, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    for (HWND box : g.pfBoxes) SendMessageW(box, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    settingsListRescale(g.settingsList, dpi);
    applyLayout();
    if (suggested) {
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    sizeWindowToLayout();
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            g.window = window;
            ui::applyWindowChrome(window);
            createControls(window);
            PostMessageW(window, kMsgFirstScan, 0, 0);
            return 0;

        case WM_PAINT:
            paintWindow(window);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // the paint above covers every pixel

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (item->CtlID == kIdCombo || item->CtlID == kIdScope) {
                ui::drawComboItem(item, g.dpi);
                return TRUE;
            }
            if (ui::drawOwnerDrawn(item, g.dpi)) return TRUE;
            break;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure->CtlID == kIdCombo || measure->CtlID == kIdScope) {
                measure->itemHeight = static_cast<UINT>(dp(24));
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (id == kIdSearch && code == EN_CHANGE) {
                wchar_t buffer[256]{};
                GetWindowTextW(g.search, buffer, 256);
                settingsListSetFilter(g.settingsList, buffer);
                return 0;
            }
            if (id == kIdCombo && code == CBN_SELCHANGE) {
                const LRESULT index = SendMessageW(g.combo, CB_GETCURSEL, 0, 0);
                if (index != CB_ERR) selectInstall(static_cast<size_t>(index));
                return 0;
            }
            if (id == kIdScope && code == CBN_SELCHANGE) {
                scopeChanged();
                return 0;
            }
            if (code != BN_CLICKED) return 0;
            switch (id) {
                case kIdBrowse: browseForFolder(); return 0;
                case kIdSettingsVr: settingsEditionPicked("vr"); return 0;
                case kIdSettingsFlat: settingsEditionPicked("flat"); return 0;
                case kIdInstall: runAction(AppArgs::Act::Install); return 0;
                case kIdRepair: runAction(AppArgs::Act::Repair); return 0;
                case kIdUninstall: runAction(AppArgs::Act::Uninstall); return 0;
                case kIdCollectLogs: saveLogs(); return 0;
                case kIdTabInstall: showScreen(Screen::Install); return 0;
                case kIdTabPreflight: showScreen(Screen::Preflight); return 0;
                case kIdTabSettings: showScreen(Screen::Settings); return 0;
                case kIdPfSetUp: runPreflightAction(false, false, nullptr); return 0;
                case kIdPfUndo: runPreflightAction(true, false, nullptr); return 0;
                case kIdKofi:
                    ShellExecuteW(window, L"open", kTipUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdDiscord:
                    ShellExecuteW(window, L"open", kDiscordUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdChkKeep:
                    ui::toggleCheckbox(reinterpret_cast<HWND>(lparam));
                    return 0;
                default:
                    if (id >= kIdPfBox && id < kIdPfBox + static_cast<int>(kPfBoxCount)) {
                        // A Pre-flight checkbox: flip it, and remember the choice against its row.
                        HWND box = reinterpret_cast<HWND>(lparam);
                        ui::toggleCheckbox(box);
                        const int row = g.pfBoxRow[static_cast<size_t>(id - kIdPfBox)];
                        if (row >= 0 && static_cast<size_t>(row) < g.pfRows.size())
                            g.pfTicks[g.pfRows[static_cast<size_t>(row)].id] = ui::checkboxChecked(box);
                        updatePreflightButtons();
                    }
                    return 0;
            }
        }

        // The report card's slim scrollbar is parent territory (the EDIT
        // keeps no bar of its own): a press in the right gutter grabs or
        // jumps the thumb, and the drag drives the EDIT by whole lines.
        case WM_LBUTTONDOWN: {
            if (g.screen == Screen::Install && pfStatusRowAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                showScreen(Screen::Preflight);
                return 0;
            }
            if (g.screen != Screen::Install || !g.report) break;
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const RECT card = reportCardRect();
            if (x < card.right - dp(12) || x >= card.right || y < card.top || y >= card.bottom)
                break;
            int content = 0, page = 0, pos = 0;
            reportScrollState(&content, &page, &pos);
            const RECT thumb = ui::slimThumb(card, content, page, pos, g.dpi);
            if (thumb.right <= thumb.left) break;
            if (y >= thumb.top && y < thumb.bottom) {
                g.reportDragOffset = y - thumb.top;
            } else {
                g.reportDragOffset = (thumb.bottom - thumb.top) / 2;
            }
            g.reportDrag = true;
            SetCapture(window);
            // fall through to the move handler's math via a synthetic move
        }
            [[fallthrough]];
        case WM_MOUSEMOVE: {
            if (!g.reportDrag) break;
            int content = 0, page = 0, pos = 0;
            reportScrollState(&content, &page, &pos);
            const int wanted =
                ui::slimPosFromThumbTop(reportCardRect(), content, page,
                                        GET_Y_LPARAM(lparam) - g.reportDragOffset, g.dpi);
            const int lineHeight = reportLineHeight();
            const int delta = wanted / lineHeight - pos / lineHeight;
            if (delta != 0) SendMessageW(g.report, EM_LINESCROLL, 0, delta);
            invalidateReportGutter();
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g.reportDrag) {
                g.reportDrag = false;
                if (message == WM_LBUTTONUP) ReleaseCapture();
                invalidateReportGutter();
            }
            break;

        // WM_MOUSEWHEEL goes to whichever control has keyboard focus, not
        // the one under the cursor -- and nobody clicks a read-only status
        // pane before trying to scroll it. The settings list gets away with
        // focus-routing because its rows take focus on click; the report
        // needs the tick forwarded here so hovering it is enough.
        case WM_MOUSEWHEEL: {
            if (g.screen != Screen::Install || !g.report) break;
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};  // screen coords
            ScreenToClient(window, &pt);
            const RECT card = reportCardRect();
            if (!PtInRect(&card, pt)) break;
            return SendMessageW(g.report, WM_MOUSEWHEEL, wparam, lparam);
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX: {
            // The report pane sits inside a card and has to be the same colour
            // as it, or it reads as a sunken 1990s text box. The search box sits
            // inside a frame painted behind it, and takes that frame's fill.
            HDC dc = reinterpret_cast<HDC>(wparam);
            const ui::Theme& t = ui::theme();
            const bool isSearch = reinterpret_cast<HWND>(lparam) == g.search;
            const COLORREF fill = isSearch ? t.control : t.cardBg;
            SetTextColor(dc, t.text);
            SetBkColor(dc, fill);
            static HBRUSH brushes[2] = {nullptr, nullptr};
            static COLORREF colours[2] = {0, 0};
            const int slot = isSearch ? 1 : 0;
            if (!brushes[slot] || colours[slot] != fill) {
                if (brushes[slot]) DeleteObject(brushes[slot]);
                brushes[slot] = CreateSolidBrush(fill);
                colours[slot] = fill;
            }
            return reinterpret_cast<LRESULT>(brushes[slot]);
        }

        case WM_SETCURSOR:
            // A hand over the Setup tab's Pre-flight row, which opens the tab.
            if (g.screen == Screen::Install && LOWORD(lparam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(window, &pt);
                if (pfStatusRowAt(pt.x, pt.y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;

        case WM_ACTIVATE:
            // Back from closing RivaTuner, choosing a runtime or setting Elite's options: the Pre-flight rows are read
            // again so the tab shows the rig as it is now, not as it was when the window last had the screen.
            if (LOWORD(wparam) != WA_INACTIVE && g.screen == Screen::Preflight && g.haveSurvey) {
                refreshPreflight();
                layoutPreflight();
                InvalidateRect(window, nullptr, FALSE);
            }
            break;

        case WM_SETTINGCHANGE:
            // The system switched between light and dark while we were open.
            if (lparam && wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
                ui::refreshTheme();
                ui::applyWindowChrome(window);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        case WM_DPICHANGED:
            rescale(window, HIWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            return 0;

        case kMsgFirstScan: {
            // After the window is up, so the first paint is not held behind a
            // registry and filesystem sweep.
            if (!g.args.dir.empty()) {
                GameInstall game;
                if (describeDir(g.args.dir, L"Folder you chose", &game))
                    addInstall(game);
                else
                    scanForInstalls();
            } else {
                scanForInstalls();
            }
            if (g.args.autorun && g.args.action == AppArgs::Act::Preflight && g.haveSurvey) {
                // The elevated copy of a Pre-flight button: the plan was confirmed before it started, and the rows
                // are the ones that were ticked.
                showScreen(Screen::Preflight);
                runPreflightAction(g.args.preflightUndo, true, g.args.rowsGiven ? &g.args.rows : nullptr);
            } else if (g.args.autorun && g.args.action != AppArgs::Act::None && g.haveSurvey) {
                runAction(g.args.action);
            } else if (g.args.action == AppArgs::Act::Preflight) {
                // --preflight that found no console to print to: the window opens on the tab it asked about.
                showScreen(Screen::Preflight);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            ui::releaseFonts();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int runGui(HINSTANCE instance, const AppArgs& args) {
    g.args = args;

    const INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ui::startup();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;  // painted, not erased
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpszClassName = L"EdvrInstallerWindow";
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);

    HWND window = CreateWindowExW(0, cls.lpszClassName, L"EDVR installer",
                                  (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) |
                                      WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, kClientWidth, kClientHeight,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // A wheel tick is delivered to whatever holds keyboard FOCUS, not to
        // whatever the cursor is over. Every wheel handler in this program
        // sits in a window proc, so unless that window happens to be focused
        // its handler never runs at all -- which is why the report pane's own
        // fix did nothing in the field: the tick was going to whichever
        // button had focus and stopping there.
        //
        // Redirecting here, before dispatch, is the one place that fixes it
        // for every control at once. Anything outside our own window tree is
        // left alone, and WindowFromPoint skips hidden and disabled children,
        // so a pane in either state still falls through to the parent's
        // handler.
        if (message.message == WM_MOUSEWHEEL) {
            const POINT pt{GET_X_LPARAM(message.lParam),
                           GET_Y_LPARAM(message.lParam)};   // screen coords
            const HWND under = WindowFromPoint(pt);
            if (under && (under == window || IsChild(window, under))) {
                message.hwnd = under;
            }
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ui::shutdown();
    CoUninitialize();
    return 0;
}

}  // namespace edvr::installer
