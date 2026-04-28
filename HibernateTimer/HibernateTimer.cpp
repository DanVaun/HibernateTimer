// HibernateTimer
// Author: DanVaun (https://github.com/DanVaun)
// Exposes the hibernate timeout hidden by Windows on Modern Standby (S0) systems.
// https://github.com/DanVaun/HibernateTimer
// Also includes screen/sleep timeouts and lid/power button controls.
//
// Registry mappings confirmed via Process Monitor:
// Both power button (7648efa3) and lid (5ca83367) use identical index mapping:
//   0 = Do nothing, 1 = Sleep, 2 = Hibernate, 3 = Shut down

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <sdkddkver.h>
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <powrprof.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, \
    "/manifestdependency:\"type='win32' "\
    "name='Microsoft.Windows.Common-Controls' "\
    "version='6.0.0.0' processorArchitecture='*' "\
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── GUIDs ─────────────────────────────────────────────────────────────────────
// Screen timeout
static const GUID G_VIDEO_SUB = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
static const GUID G_SCREEN_TO = { 0x3c0bc021,0xc8a8,0x4e07,{0xa9,0x73,0x6b,0x14,0xcb,0xcb,0x2b,0x7e} };
// Sleep / hibernate
static const GUID G_SLEEP_SUB = { 0x238c9fa8,0x0aad,0x41ed,{0x83,0xf4,0x97,0xbe,0x24,0x2c,0x8f,0x20} };
static const GUID G_SLEEP_TO = { 0x29f6c1db,0x86da,0x48c5,{0x9f,0xdb,0xf2,0xb6,0x7b,0x1f,0x44,0xda} };
static const GUID G_HIBER_TO = { 0x9d7815a6,0x7ee4,0x497e,{0x88,0x88,0x51,0x5a,0x05,0xf0,0x23,0x64} };
// Buttons and lid subgroup
static const GUID G_BTN_SUB = { 0x4f971e89,0xeebd,0x4455,{0xa8,0xde,0x9e,0x59,0x04,0x0e,0x73,0x47} };
// Physical power button (confirmed via Process Monitor)
static const GUID G_PWR_BTN = { 0x7648efa3,0xdd9c,0x4e3e,{0xb5,0x66,0x50,0xf9,0x29,0x38,0x62,0x80} };
// Lid close action (confirmed via Process Monitor)
static const GUID G_LID = { 0x5ca83367,0x6e45,0x459f,{0xa2,0x7b,0x47,0x6b,0x1d,0x01,0xc9,0x36} };

// ── Action mapping — identical for both power button and lid ──────────────────
// Confirmed via Process Monitor: 0=Do nothing 1=Sleep 2=Hibernate 3=Shut down
static const wchar_t* kActions[] = {
    L"Do nothing", L"Sleep", L"Hibernate", L"Shut down"
};
static const int kActionCount = 4;

// ── Time options ──────────────────────────────────────────────────────────────
struct TOpt { const wchar_t* label; DWORD sec; };
static const TOpt kTime[] = {
    {L"Never",      0    },{L"1 minute",   60   },{L"2 minutes",  120  },
    {L"3 minutes",  180  },{L"5 minutes",  300  },{L"10 minutes", 600  },
    {L"15 minutes", 900  },{L"20 minutes", 1200 },{L"25 minutes", 1500 },
    {L"30 minutes", 1800 },{L"45 minutes", 2700 },{L"1 hour",     3600 },
    {L"2 hours",    7200 },{L"3 hours",    10800},{L"4 hours",    14400},
    {L"5 hours",    18000},
};
static const int kTimeN = (int)(sizeof(kTime) / sizeof(kTime[0]));

// ── Colours ───────────────────────────────────────────────────────────────────
#define CLR_PAGE    RGB(243,243,243)
#define CLR_CARD    RGB(255,255,255)
#define CLR_BORDER  RGB(218,218,218)
#define CLR_TEXT    RGB(0,0,0)
#define CLR_SUB     RGB(96,96,96)
#define CLR_WARN_BG RGB(255,244,206)
#define CLR_WARN_BR RGB(200,160,0)

// ── Control IDs ───────────────────────────────────────────────────────────────
#define ID_SAVE        100
#define ID_ENABLE_HIB  101
#define ID_SCREEN_AC   200
#define ID_SCREEN_DC   201
#define ID_SLEEP_AC    202
#define ID_SLEEP_DC    203
#define ID_HIBER_AC    204
#define ID_HIBER_DC    205
#define ID_BTN_AC      220
#define ID_BTN_DC      221
#define ID_LID_AC      222
#define ID_LID_DC      223
#define SCROLL_TIMER   42

// ── Globals ───────────────────────────────────────────────────────────────────
HWND  g_hwnd = NULL;
HWND  g_inner = NULL;
HFONT g_fTitle = NULL;
HFONT g_fSec = NULL;
HFONT g_fLbl = NULL;
HFONT g_fSub = NULL;
HFONT g_fBold = NULL;
GUID* g_scheme = NULL;
int   g_innerH = 0;
int   g_scrollPos = 0;
int   g_scrollTgt = 0;
bool  g_hibEnabled = false;

static std::vector<RECT>             g_cards;
static std::vector<bool>             g_cardWarn; // true = paint as warning banner
static std::unordered_map<int, DWORD> g_orig;
static HBRUSH g_hbCard = NULL;
static HBRUSH g_hbPage = NULL;

// ── Fonts ─────────────────────────────────────────────────────────────────────
static HFONT MakeFont(int pt, int weight, const wchar_t* face = L"Segoe UI") {
    HDC hdc = GetDC(NULL);
    int h = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(h, 0, 0, 0, weight, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}
static void SF(HWND h, HFONT f) {
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

// ── GUID to registry string ───────────────────────────────────────────────────
static std::wstring GuidStr(const GUID& g) {
    wchar_t buf[64];
    swprintf_s(buf,
        L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1],
        g.Data4[2], g.Data4[3], g.Data4[4],
        g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

// ── Registry read/write for button and lid settings ───────────────────────────
// Reads ACSettingIndex or DCSettingIndex from the per-scheme registry path.
// This is the only reliable method for these settings on S0 systems.
static DWORD RegReadAction(const GUID& scheme, const GUID& sub,
    const GUID& setting, bool ac) {
    std::wstring path;
    path = L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes\\";
    path += L"{"; path += GuidStr(scheme);  path += L"}\\";
    path += L"{"; path += GuidStr(sub);     path += L"}\\";
    path += L"{"; path += GuidStr(setting); path += L"}";
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
        0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0; // default to Do nothing if key missing
    DWORD val = 0, sz = sizeof(DWORD);
    RegQueryValueExW(hk,
        ac ? L"ACSettingIndex" : L"DCSettingIndex",
        NULL, NULL, (LPBYTE)&val, &sz);
    RegCloseKey(hk);
    return val;
}

static void RegWriteAction(const GUID& scheme, const GUID& sub,
    const GUID& setting, bool ac, DWORD val) {
    std::wstring path;
    path = L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes\\";
    path += L"{"; path += GuidStr(scheme);  path += L"}\\";
    path += L"{"; path += GuidStr(sub);     path += L"}\\";
    path += L"{"; path += GuidStr(setting); path += L"}";
    HKEY hk;
    // Create key if it doesn't exist
    DWORD disp;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
        0, NULL, 0, KEY_SET_VALUE, NULL,
        &hk, &disp) != ERROR_SUCCESS)
        return;
    RegSetValueExW(hk,
        ac ? L"ACSettingIndex" : L"DCSettingIndex",
        0, REG_DWORD, (LPBYTE)&val, sizeof(DWORD));
    RegCloseKey(hk);
}

// ── Power API wrappers for screen/sleep/hibernate ─────────────────────────────
static DWORD RdAC(const GUID* sg, const GUID* s) {
    DWORD v = 0;
    PowerReadACValueIndex(NULL, g_scheme, sg, s, &v);
    return v;
}
static DWORD RdDC(const GUID* sg, const GUID* s) {
    DWORD v = 0;
    PowerReadDCValueIndex(NULL, g_scheme, sg, s, &v);
    return v;
}
static void WrAC(const GUID* sg, const GUID* s, DWORD v) {
    PowerWriteACValueIndex(NULL, g_scheme, sg, s, v);
}
static void WrDC(const GUID* sg, const GUID* s, DWORD v) {
    PowerWriteDCValueIndex(NULL, g_scheme, sg, s, v);
}

// ── Hibernate check and enable ────────────────────────────────────────────────
static bool CheckHibernateEnabled() {
    HKEY hk;
    DWORD val = 0, sz = sizeof(DWORD);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Power",
        0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExW(hk, L"HibernateEnabled",
            NULL, NULL, (LPBYTE)&val, &sz);
        RegCloseKey(hk);
    }
    return val != 0;
}

static void EnableHibernate() {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = L"powercfg.exe";
    sei.lpParameters = L"/hibernate on";
    sei.nShow = SW_HIDE;
    ShellExecuteExW(&sei);
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000);
        CloseHandle(sei.hProcess);
    }
}

// ── Time combo helpers ────────────────────────────────────────────────────────
static int ClosestTimeIdx(DWORD sec) {
    int best = 0; DWORD bd = 0xFFFFFFFF;
    for (int i = 0; i < kTimeN; i++) {
        DWORD d = sec >= kTime[i].sec
            ? sec - kTime[i].sec
            : kTime[i].sec - sec;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static void FillTimeCombo(HWND cb, DWORD currentSec) {
    int best = ClosestTimeIdx(currentSec);
    for (int i = 0; i < kTimeN; i++)
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kTime[i].label);
    SendMessageW(cb, CB_SETCURSEL, best, 0);
}
static DWORD GetTimeFromCombo(HWND cb) {
    int s = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    return (s >= 0 && s < kTimeN) ? kTime[s].sec : 0;
}
static DWORD GetComboSel(HWND p, int id) {
    return (DWORD)SendMessageW(GetDlgItem(p, id), CB_GETCURSEL, 0, 0);
}

// ── Layout ────────────────────────────────────────────────────────────────────
static const int M = 20;  // page margin
static const int RH = 58;  // row height
static const int SHH = 40;  // sub-header height
static const int HH = 50;  // card header height
static const int GC = 10;  // gap between cards
static const int CBW = 178; // combo width
static const int CBH = 26;  // combo height

// ── Build UI ──────────────────────────────────────────────────────────────────
static void BuildUI(HWND p, int winW) {
    g_cards.clear();
    g_cardWarn.clear();
    g_orig.clear();

    int cx = M;
    int cw = winW - M * 2;
    int cy = M;

    // ── Helpers ───────────────────────────────────────────────────────────────
    auto Div = [&]() {
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            cx + 1, cy, cw - 2, 1, p, NULL, NULL, NULL);
        cy += 1;
        };

    auto CardHdr = [&](const wchar_t* title) {
        HWND h = CreateWindowExW(0, L"STATIC", title,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 16, cy + 14, cw - 32, 22, p, NULL, NULL, NULL);
        SF(h, g_fSec);
        cy += HH;
        Div();
        };

    auto SubHdr = [&](const wchar_t* text) {
        HWND h = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 16, cy + 11, 220, 20, p, NULL, NULL, NULL);
        SF(h, g_fBold);
        cy += SHH;
        };

    // Standard row: label + optional sublabel + combo right-aligned
    auto Row = [&](const wchar_t* lbl, const wchar_t* sub, int cbId) -> HWND {
        HWND hL = CreateWindowExW(0, L"STATIC", lbl,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 16, cy + 10, cw - CBW - 36, 20, p, NULL, NULL, NULL);
        SF(hL, g_fLbl);
        if (sub) {
            HWND hS = CreateWindowExW(0, L"STATIC", sub,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                cx + 16, cy + 32, cw - CBW - 36, 18, p, NULL, NULL, NULL);
            SF(hS, g_fSub);
        }
        HWND cb = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + cw - CBW - M, cy + RH / 2 - CBH / 2, CBW, 220,
            p, (HMENU)(INT_PTR)cbId, NULL, NULL);
        SF(cb, g_fLbl);
        cy += RH;
        return cb;
        };

    // Action row: label + action combo (Do nothing/Sleep/Hibernate/Shut down)
    // val is the raw registry DWORD (0-3), stored directly as combo index
    auto ActionRow = [&](const wchar_t* lbl, int cbId, DWORD val) {
        HWND hL = CreateWindowExW(0, L"STATIC", lbl,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 16, cy + 19, cw - CBW - 36, 20, p, NULL, NULL, NULL);
        SF(hL, g_fLbl);
        HWND cb = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + cw - CBW - M, cy + RH / 2 - CBH / 2, CBW, 220,
            p, (HMENU)(INT_PTR)cbId, NULL, NULL);
        SF(cb, g_fLbl);
        // Populate all 4 options — same mapping for both power button and lid
        for (int i = 0; i < kActionCount; i++)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kActions[i]);
        // val IS the combo index (0=Do nothing, 1=Sleep, 2=Hibernate, 3=Shut down)
        DWORD sel = (val < (DWORD)kActionCount) ? val : 0;
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
        g_orig[cbId] = sel;
        cy += RH;
        };

    // ── Page title ────────────────────────────────────────────────────────────
    {
        HWND h = CreateWindowExW(0, L"STATIC", L"Hibernate Timer",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy, cw, 34, p, NULL, NULL, NULL);
        SF(h, g_fTitle);
        cy += 40;

        HWND hs = CreateWindowExW(0, L"STATIC",
            L"Exposes the hibernate timeout hidden by Windows on Modern Standby (S0) systems.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy, cw, 18, p, NULL, NULL, NULL);
        SF(hs, g_fSub);
        cy += 28;
    }

    // ── Hibernate status ──────────────────────────────────────────────────────
    g_hibEnabled = CheckHibernateEnabled();
    if (!g_hibEnabled) {
        int top = cy;
        HWND hW = CreateWindowExW(0, L"STATIC",
            L"\u26A0  Hibernate is currently disabled. "
            L"The timer below will have no effect until hibernate is enabled.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx + 14, cy + 12, cw - 160, 40, p, NULL, NULL, NULL);
        SF(hW, g_fLbl);
        HWND hBtn = CreateWindowExW(0, L"BUTTON", L"Enable hibernate",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + cw - 140, cy + 16, 136, 28,
            p, (HMENU)(INT_PTR)ID_ENABLE_HIB, NULL, NULL);
        SF(hBtn, g_fLbl);
        cy += 64;
        g_cards.push_back({ cx, top, cx + cw, cy });
        g_cardWarn.push_back(true);
        cy += GC;
    }
    else {
        HWND h = CreateWindowExW(0, L"STATIC",
            L"\u2714  Hibernate is enabled",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy, cw, 18, p, NULL, NULL, NULL);
        SF(h, g_fSub);
        cy += 26;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // CARD 1 — Screen, sleep & hibernate timeouts
    // ══════════════════════════════════════════════════════════════════════════
    {
        int top = cy;
        CardHdr(L"Screen, sleep, & hibernate timeouts");

        // Read current values
        DWORD scAC = RdAC(&G_VIDEO_SUB, &G_SCREEN_TO);
        DWORD scDC = RdDC(&G_VIDEO_SUB, &G_SCREEN_TO);
        DWORD slAC = RdAC(&G_SLEEP_SUB, &G_SLEEP_TO);
        DWORD slDC = RdDC(&G_SLEEP_SUB, &G_SLEEP_TO);
        DWORD hbAC = RdAC(&G_SLEEP_SUB, &G_HIBER_TO);
        DWORD hbDC = RdDC(&G_SLEEP_SUB, &G_HIBER_TO);

        // Plugged in
        SubHdr(L"Plugged in");

        HWND hScAC = Row(L"Turn my screen off after", nullptr, ID_SCREEN_AC);
        FillTimeCombo(hScAC, scAC);
        g_orig[ID_SCREEN_AC] = ClosestTimeIdx(scAC);
        Div();

        HWND hHbAC = Row(L"Make my device hibernate after",
            L"Set shorter than sleep to ensure hibernate can activate",
            ID_HIBER_AC);
        FillTimeCombo(hHbAC, hbAC);
        g_orig[ID_HIBER_AC] = ClosestTimeIdx(hbAC);
        Div();

        HWND hSlAC = Row(L"Make my device sleep after",
            L"Set longer than hibernate, or hibernate will never activate",
            ID_SLEEP_AC);
        FillTimeCombo(hSlAC, slAC);
        g_orig[ID_SLEEP_AC] = ClosestTimeIdx(slAC);
        Div();

        // On battery
        SubHdr(L"On battery");

        HWND hScDC = Row(L"Turn my screen off after", nullptr, ID_SCREEN_DC);
        FillTimeCombo(hScDC, scDC);
        g_orig[ID_SCREEN_DC] = ClosestTimeIdx(scDC);
        Div();

        HWND hHbDC = Row(L"Make my device hibernate after",
            L"Set shorter than sleep to ensure hibernate can activate",
            ID_HIBER_DC);
        FillTimeCombo(hHbDC, hbDC);
        g_orig[ID_HIBER_DC] = ClosestTimeIdx(hbDC);
        Div();

        HWND hSlDC = Row(L"Make my device sleep after",
            L"Set longer than hibernate, or hibernate will never activate",
            ID_SLEEP_DC);
        FillTimeCombo(hSlDC, slDC);
        g_orig[ID_SLEEP_DC] = ClosestTimeIdx(slDC);

        g_cards.push_back({ cx, top, cx + cw, cy });
        g_cardWarn.push_back(false);
        cy += GC;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // CARD 2 — Lid & power button controls
    // ══════════════════════════════════════════════════════════════════════════
    {
        int top = cy;
        CardHdr(L"Lid & power button controls");

        // Read via Power API — handles permissions correctly on all systems
        // Confirmed mapping: 0=Do nothing 1=Sleep 2=Hibernate 3=Shut down
        DWORD btnAC = g_scheme ? RdAC(&G_BTN_SUB, &G_PWR_BTN) : 0;
        DWORD btnDC = g_scheme ? RdDC(&G_BTN_SUB, &G_PWR_BTN) : 0;
        DWORD lidAC = g_scheme ? RdAC(&G_BTN_SUB, &G_LID) : 0;
        DWORD lidDC = g_scheme ? RdDC(&G_BTN_SUB, &G_LID) : 0;

        SubHdr(L"Plugged in");
        ActionRow(L"Pressing the power button", ID_BTN_AC, btnAC);
        Div();
        ActionRow(L"Closing the lid", ID_LID_AC, lidAC);
        Div();

        SubHdr(L"On battery");
        ActionRow(L"Pressing the power button", ID_BTN_DC, btnDC);
        Div();
        ActionRow(L"Closing the lid", ID_LID_DC, lidDC);

        g_cards.push_back({ cx, top, cx + cw, cy });
        g_cardWarn.push_back(false);
        cy += GC;
    }

    // ── Save button ───────────────────────────────────────────────────────────
    {
        HWND hB = CreateWindowExW(0, L"BUTTON", L"Save changes",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            cx + cw - 148, cy, 148, 34,
            p, (HMENU)(INT_PTR)ID_SAVE, NULL, NULL);
        SF(hB, g_fLbl);
        cy += 34 + M;
    }

    g_innerH = cy;
}

// ── Save — only write changed values ─────────────────────────────────────────
static void SaveAll(HWND p) {
    if (!g_scheme) return;
    int saved = 0;

    // Returns true if combo selection differs from original
    auto Changed = [&](int id) -> bool {
        DWORD cur = GetComboSel(p, id);
        auto it = g_orig.find(id);
        return it == g_orig.end() || it->second != cur;
        };

    // Screen timeout
    if (Changed(ID_SCREEN_AC)) {
        WrAC(&G_VIDEO_SUB, &G_SCREEN_TO, GetTimeFromCombo(GetDlgItem(p, ID_SCREEN_AC)));
        saved++;
    }
    if (Changed(ID_SCREEN_DC)) {
        WrDC(&G_VIDEO_SUB, &G_SCREEN_TO, GetTimeFromCombo(GetDlgItem(p, ID_SCREEN_DC)));
        saved++;
    }

    // Sleep timeout
    DWORD slAC = GetTimeFromCombo(GetDlgItem(p, ID_SLEEP_AC));
    DWORD slDC = GetTimeFromCombo(GetDlgItem(p, ID_SLEEP_DC));
    if (Changed(ID_SLEEP_AC)) { WrAC(&G_SLEEP_SUB, &G_SLEEP_TO, slAC); saved++; }
    if (Changed(ID_SLEEP_DC)) { WrDC(&G_SLEEP_SUB, &G_SLEEP_TO, slDC); saved++; }

    // Hibernate timeout — warn if hibernate >= sleep since sleep fires first
    DWORD hbAC = GetTimeFromCombo(GetDlgItem(p, ID_HIBER_AC));
    DWORD hbDC = GetTimeFromCombo(GetDlgItem(p, ID_HIBER_DC));
    if (slAC > 0 && hbAC > 0 && hbAC >= slAC)
        MessageBoxW(g_hwnd,
            L"Warning (plugged in): hibernate is set longer than or equal to sleep.\n"
            L"Sleep will activate first and hibernate will never trigger.",
            L"Hibernate Timer", MB_OK | MB_ICONWARNING);
    if (slDC > 0 && hbDC > 0 && hbDC >= slDC)
        MessageBoxW(g_hwnd,
            L"Warning (on battery): hibernate is set longer than or equal to sleep.\n"
            L"Sleep will activate first and hibernate will never trigger.",
            L"Hibernate Timer", MB_OK | MB_ICONWARNING);
    if (Changed(ID_HIBER_AC)) { WrAC(&G_SLEEP_SUB, &G_HIBER_TO, hbAC); saved++; }
    if (Changed(ID_HIBER_DC)) { WrDC(&G_SLEEP_SUB, &G_HIBER_TO, hbDC); saved++; }

    // Power button and lid actions
    // combo index == registry value (0=Do nothing 1=Sleep 2=Hibernate 3=Shut down)
    auto SaveAction = [&](int id, bool ac, const GUID& guid) {
        if (!Changed(id)) return;
        DWORD val = GetComboSel(p, id); // index IS the registry value
        RegWriteAction(*g_scheme, G_BTN_SUB, guid, ac, val);
        // Also write via Power API for compatibility
        if (ac) WrAC(&G_BTN_SUB, &guid, val);
        else    WrDC(&G_BTN_SUB, &guid, val);
        saved++;
        };
    SaveAction(ID_BTN_AC, true, G_PWR_BTN);
    SaveAction(ID_BTN_DC, false, G_PWR_BTN);
    SaveAction(ID_LID_AC, true, G_LID);
    SaveAction(ID_LID_DC, false, G_LID);

    // Commit the active scheme
    PowerSetActiveScheme(NULL, g_scheme);

    if (saved > 0) {
        // Update originals so next save is diff-based again
        for (int id : {ID_SCREEN_AC, ID_SCREEN_DC, ID_SLEEP_AC, ID_SLEEP_DC,
            ID_HIBER_AC, ID_HIBER_DC, ID_BTN_AC, ID_BTN_DC,
            ID_LID_AC, ID_LID_DC})
            g_orig[id] = GetComboSel(p, id);

        wchar_t msg[128];
        swprintf_s(msg, L"%d setting%s saved.", saved, saved == 1 ? L"" : L"s");
        MessageBoxW(g_hwnd, msg, L"Hibernate Timer", MB_OK | MB_ICONINFORMATION);
    }
    else {
        MessageBoxW(g_hwnd, L"No changes to save.",
            L"Hibernate Timer", MB_OK | MB_ICONINFORMATION);
    }
}

// ── Smooth scroll ─────────────────────────────────────────────────────────────
static void ApplyScroll(HWND hwnd, int pos) {
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(hwnd, SB_VERT, &si);
    int maxPos = max(0, (int)(si.nMax - (int)si.nPage + 1));
    pos = max(0, min(pos, maxPos));
    if (pos == g_scrollPos) return;
    int delta = g_scrollPos - pos;
    g_scrollPos = pos;
    si.fMask = SIF_POS; si.nPos = pos;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    ScrollWindowEx(hwnd, 0, delta, NULL, NULL, NULL, NULL,
        SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    UpdateWindow(hwnd);
}

// ── Inner panel ───────────────────────────────────────────────────────────────
LRESULT CALLBACK InnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        if (!g_hbPage) g_hbPage = CreateSolidBrush(CLR_PAGE);
        FillRect(hdc, &rc, g_hbPage);
        if (!g_hbCard) g_hbCard = CreateSolidBrush(CLR_CARD);
        for (int i = 0; i < (int)g_cards.size(); i++) {
            RECT dr = g_cards[i];
            OffsetRect(&dr, 0, -g_scrollPos);
            bool warn = i < (int)g_cardWarn.size() && g_cardWarn[i];
            HBRUSH hbFill = warn ? CreateSolidBrush(CLR_WARN_BG) : g_hbCard;
            COLORREF borderClr = warn ? CLR_WARN_BR : CLR_BORDER;
            HPEN hPen = CreatePen(PS_SOLID, 1, borderClr);
            HBRUSH oldB = (HBRUSH)SelectObject(hdc, hbFill);
            HPEN   oldP = (HPEN)SelectObject(hdc, hPen);
            RoundRect(hdc, dr.left, dr.top, dr.right, dr.bottom, 8, 8);
            SelectObject(hdc, oldB);
            SelectObject(hdc, oldP);
            DeleteObject(hPen);
            if (warn) DeleteObject(hbFill);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        if (!g_hbCard) g_hbCard = CreateSolidBrush(CLR_CARD);
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, CLR_TEXT);
        return (LRESULT)g_hbCard;
    }
    case WM_CTLCOLORBTN: {
        if (!g_hbCard) g_hbCard = CreateSolidBrush(CLR_CARD);
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)g_hbCard;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = g_scrollTgt;
        switch (LOWORD(wp)) {
        case SB_LINEUP:        pos -= 20; break;
        case SB_LINEDOWN:      pos += 20; break;
        case SB_PAGEUP:        pos -= si.nPage; break;
        case SB_PAGEDOWN:      pos += si.nPage; break;
        case SB_THUMBTRACK:    pos = si.nTrackPos; break;
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        g_scrollTgt = pos;
        SetTimer(hwnd, SCROLL_TIMER, 10, NULL);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL || lines == 0) lines = 3;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int px = (int)lines * 20 * (-delta) / WHEEL_DELTA;
        g_scrollTgt += px;
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int maxPos = max(0, (int)(si.nMax - (int)si.nPage + 1));
        g_scrollTgt = max(0, min(g_scrollTgt, maxPos));
        SetTimer(hwnd, SCROLL_TIMER, 10, NULL);
        return 0;
    }
    case WM_TIMER: {
        if (wp == SCROLL_TIMER) {
            int diff = g_scrollTgt - g_scrollPos;
            if (diff == 0) { KillTimer(hwnd, SCROLL_TIMER); return 0; }
            int step = diff / 4;
            if (step == 0) step = diff > 0 ? 1 : -1;
            ApplyScroll(hwnd, g_scrollPos + step);
            if (g_scrollPos == g_scrollTgt) KillTimer(hwnd, SCROLL_TIMER);
        }
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wp) == ID_SAVE) {
            SaveAll(hwnd);
        }
        else if (LOWORD(wp) == ID_ENABLE_HIB) {
            EnableHibernate();
            // Rebuild UI to reflect new hibernate state
            for (HWND c = GetWindow(hwnd, GW_CHILD); c;) {
                HWND next = GetWindow(c, GW_HWNDNEXT);
                DestroyWindow(c);
                c = next;
            }
            g_scrollPos = 0; g_scrollTgt = 0;
            RECT cr; GetClientRect(GetParent(hwnd), &cr);
            BuildUI(hwnd, cr.right);
            SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
            si.nMin = 0; si.nMax = g_innerH; si.nPage = cr.bottom; si.nPos = 0;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Main window ───────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_fTitle = MakeFont(20, FW_SEMIBOLD, L"Segoe UI Variable Display");
        g_fSec = MakeFont(11, FW_SEMIBOLD);
        g_fLbl = MakeFont(10, FW_NORMAL);
        g_fSub = MakeFont(9, FW_NORMAL);
        g_fBold = MakeFont(10, FW_BOLD);

        PowerGetActiveScheme(NULL, &g_scheme);

        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        WNDCLASSEXW wci = { sizeof(wci) };
        wci.lpfnWndProc = InnerProc;
        wci.hInstance = hInst;
        wci.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wci.lpszClassName = L"InnerPanel";
        wci.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wci);

        RECT cr; GetClientRect(hwnd, &cr);
        g_inner = CreateWindowExW(0, L"InnerPanel", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL,
            0, 0, cr.right, cr.bottom,
            hwnd, NULL, hInst, NULL);

        BuildUI(g_inner, cr.right);

        SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
        si.nMin = 0; si.nMax = g_innerH; si.nPage = cr.bottom; si.nPos = 0;
        SetScrollInfo(g_inner, SB_VERT, &si, TRUE);
        g_scrollPos = 0; g_scrollTgt = 0;
        return 0;
    }
    case WM_SIZE: {
        if (g_inner) {
            int w = LOWORD(lp), h = HIWORD(lp);
            SetWindowPos(g_inner, NULL, 0, 0, w, h, SWP_NOZORDER);
            SCROLLINFO si = { sizeof(si), SIF_PAGE };
            si.nPage = h;
            SetScrollInfo(g_inner, SB_VERT, &si, TRUE);
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_scheme) { LocalFree(g_scheme); g_scheme = NULL; }
        DeleteObject(g_fTitle); DeleteObject(g_fSec); DeleteObject(g_fLbl);
        DeleteObject(g_fSub);   DeleteObject(g_fBold);
        if (g_hbCard) DeleteObject(g_hbCard);
        if (g_hbPage) DeleteObject(g_hbPage);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry point ───────────────────────────────────────────────────────────────
int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE,
    _In_ LPWSTR, _In_ int nShow) {
    // Auto-elevate if not admin
        {
            BOOL admin = FALSE; HANDLE tok = NULL;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
                TOKEN_ELEVATION e; DWORD sz = sizeof(e);
                if (GetTokenInformation(tok, TokenElevation, &e, sz, &sz))
                    admin = e.TokenIsElevated;
                CloseHandle(tok);
            }
            if (!admin) {
                wchar_t path[MAX_PATH];
                GetModuleFileNameW(NULL, path, MAX_PATH);
                ShellExecuteW(NULL, L"runas", path, NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }
        }

        // Per-monitor DPI awareness v2
        typedef BOOL(WINAPI* SPDAC_t)(int);
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        SPDAC_t fnDpi = hUser32
            ? (SPDAC_t)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext")
            : nullptr;
        if (fnDpi) fnDpi(-4);
        else SetProcessDPIAware();

        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);

        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hbrBackground = CreateSolidBrush(CLR_PAGE);
        wc.lpszClassName = L"HibernateTimerApp";
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wc);

        g_hwnd = CreateWindowExW(0, L"HibernateTimerApp",
            L"Hibernate Timer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 620, 780,
            NULL, NULL, hInst, NULL);

        ShowWindow(g_hwnd, nShow);
        UpdateWindow(g_hwnd);

        MSG m;
        while (GetMessageW(&m, NULL, 0, 0)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        return (int)m.wParam;
}
