#include "pch.h"
#include "Overlay.h"
#include "Config.h"
#include "Hook.h"
#include "Log.h"
#include "Stats.h"

#include <commdlg.h>   // ChooseColor

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

extern HMODULE g_hModule;

// ---------------------------------------------------------------------------------
//  constants
// ---------------------------------------------------------------------------------
static const wchar_t* kClassName = L"PD2DpsMeterWindow";
static const wchar_t* kTitle = L"PD2 DPS Meter";
static const COLORREF kColorKey = RGB(1, 0, 1);   // transparent colour of the overlay mode

constexpr UINT_PTR kTimerRefresh = 1;
constexpr int kHotkeyReset = 1;
constexpr int kHotkeyMode = 2;
constexpr UINT WM_APP_RECREATE = WM_APP + 1;      // switch mode / reload look

enum MenuId : UINT {
    kMenuReset = 100, kMenuOverlay, kMenuWindow, kMenuHidden, kMenuTopmost, kMenuHideInactive,
    kMenuBackground, kMenuLabelColor, kMenuValueColor, kMenuShadow,
    kMenuLogs, kMenuEditIni, kMenuReload, kMenuHelp,
};

// ---------------------------------------------------------------------------------
//  UI state (only touched by the UI thread)
// ---------------------------------------------------------------------------------
static HWND g_wnd = nullptr;
static HWND g_gameWnd = nullptr;
static HFONT g_fontTitle = nullptr;
static HFONT g_fontText = nullptr;
static HFONT g_fontButton = nullptr;
static Gdiplus::Bitmap* g_logo = nullptr;
static bool g_dragging = false;

struct Layout {
    int pad, logo, textX, labelW, valueW, lineH, titleH, width, height;
    RECT button;    // RESET button
    RECT logoRect;  // drag handle in overlay mode
};
static Layout g_layout = {};

static int S(double v) { return (int)(v * g_config.scale + 0.5); }

// ---------------------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------------------
static std::wstring FormatNumber(double v) {
    wchar_t raw[64];
    swprintf_s(raw, L"%.0f", v < 0 ? 0 : v);
    std::wstring s(raw), out;
    int n = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (n && n % 3 == 0) out.insert(out.begin(), L',');
        out.insert(out.begin(), *it);
        ++n;
    }
    return out;
}

static std::wstring FormatSeconds(double s) {
    wchar_t buf[32];
    if (s < 60) swprintf_s(buf, L"%.1f s", s);
    else swprintf_s(buf, L"%d:%04.1f", (int)(s / 60), s - 60 * (int)(s / 60));
    return buf;
}

static int CALLBACK FontExistsProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *(bool*)found = true;
    return 0;
}

static bool FontInstalled(const std::wstring& face) {
    if (face.empty()) return false;
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FontExistsProc, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static void LoadLook() {
    if (g_fontTitle) DeleteObject(g_fontTitle);
    if (g_fontText) DeleteObject(g_fontText);
    if (g_fontButton) DeleteObject(g_fontButton);
    delete g_logo;
    g_logo = nullptr;

    std::wstring face = FontInstalled(g_config.fontName) ? g_config.fontName : g_config.fallbackFont;
    if (face != g_config.fontName) LogLine(L"Font \"%s\" is not installed, using \"%s\"", g_config.fontName.c_str(), face.c_str());
    g_fontTitle = CreateFontW(-S(g_config.fontSize + 4), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face.c_str());
    g_fontText = CreateFontW(-S(g_config.fontSize), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face.c_str());
    g_fontButton = CreateFontW(-S(g_config.fontSize * 0.7), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face.c_str());

    if (!g_config.logo.empty()) {
        std::wstring path = g_config.logo;
        if (path.size() < 2 || path[1] != L':') path = ModuleDirectory() + path;
        auto* bmp = Gdiplus::Bitmap::FromFile(path.c_str());
        if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
            g_logo = bmp;
        } else {
            delete bmp;
            LogLine(L"Logo not loaded: %s", path.c_str());
        }
    }
}

// The label/value lines of the meter. Index 0 is used to measure the layout.
static void BuildLines(const SharedStats& s, std::vector<std::pair<std::wstring, std::wstring>>& lines) {
    lines.clear();
    lines.emplace_back(L"DPS 1s", FormatNumber(s.dps1));
    lines.emplace_back(L"DPS 5s", FormatNumber(s.dps5));
    lines.emplace_back(L"DPS 20s", FormatNumber(s.dps20));
    lines.emplace_back(L"Average", FormatNumber(s.sessionDps));
    lines.emplace_back(L"Time", FormatSeconds(s.sessionSeconds));
    lines.emplace_back(L"Total", FormatNumber(s.totalDamage));
    wchar_t buf[96];
    swprintf_s(buf, L"%u  (%.1f/s)", s.hits, s.hitsPerSecond);
    lines.emplace_back(L"Hits", buf);
    lines.emplace_back(L"Last / Max", FormatNumber(s.lastHit) + L" / " + FormatNumber(s.maxHit));
    if (s.kills) swprintf_s(buf, L"%u  (TTK %s)", s.kills, FormatSeconds(s.lastKillSeconds).c_str());
    else swprintf_s(buf, L"0");
    lines.emplace_back(L"Kills", buf);
}

static void ComputeLayout() {
    Layout& l = g_layout;
    l.pad = S(6);
    l.logo = g_logo ? S(g_config.logoSize) : 0;
    l.textX = l.pad + (l.logo ? l.logo + S(10) : 0);

    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, g_fontText);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"Last / Max  ", 12, &sz);
    l.labelW = sz.cx;
    l.lineH = sz.cy + S(1);
    GetTextExtentPoint32W(dc, L"9,999,999 / 9,999,999", 21, &sz);
    l.valueW = sz.cx;
    SelectObject(dc, g_fontTitle);
    GetTextExtentPoint32W(dc, L"PD2 DPS", 7, &sz);
    l.titleH = sz.cy + S(2);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);

    const int lineCount = 9;
    l.width = l.textX + l.labelW + l.valueW + l.pad;
    int textBottom = l.pad + l.titleH + lineCount * l.lineH;
    int buttonW = l.logo ? l.logo : S(70);
    int buttonH = S(22);
    if (l.logo) {
        l.logoRect = { l.pad, l.pad, l.pad + l.logo, l.pad + l.logo };
        l.button = { l.pad, l.logoRect.bottom + S(8), l.pad + buttonW, l.logoRect.bottom + S(8) + buttonH };
        l.height = (std::max)(textBottom, (int)l.button.bottom) + l.pad;
    } else {
        l.logoRect = { l.pad, l.pad, l.textX + l.labelW, l.pad + l.titleH };   // title acts as handle
        l.button = { l.textX, textBottom + S(4), l.textX + buttonW, textBottom + S(4) + buttonH };
        l.height = l.button.bottom + l.pad;
    }
}

static void DrawLabel(HDC dc, int x, int y, const std::wstring& text, COLORREF color) {
    if (g_config.shadow) {
        SetTextColor(dc, g_config.shadowColor);
        TextOutW(dc, x + 1, y + 1, text.c_str(), (int)text.size());
    }
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text.c_str(), (int)text.size());
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const bool overlay = g_config.mode == MeterMode::Overlay;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(overlay ? kColorKey : g_config.background);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    const Layout& l = g_layout;
    if (g_logo) {
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(g_logo, l.pad, l.pad, l.logo, l.logo);
    }

    SharedStats s = GetStatsSnapshot();
    std::vector<std::pair<std::wstring, std::wstring>> lines;
    BuildLines(s, lines);

    HGDIOBJ oldFont = SelectObject(mem, g_fontTitle);
    std::wstring title = L"PD2 DPS";
    if (s.sessionNumber > 1) title += L"  #" + std::to_wstring(s.sessionNumber);
    DrawLabel(mem, l.textX, l.pad, title, g_config.titleColor);
    SelectObject(mem, g_fontText);
    int y = l.pad + l.titleH;
    for (const auto& line : lines) {
        DrawLabel(mem, l.textX, y, line.first, g_config.titleColor);
        DrawLabel(mem, l.textX + l.labelW, y, line.second, g_config.textColor);
        y += l.lineH;
    }

    // RESET button
    HPEN pen = CreatePen(PS_SOLID, (std::max)(1, S(1)), g_config.titleColor);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
    RoundRect(mem, l.button.left, l.button.top, l.button.right, l.button.bottom, S(6), S(6));
    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(pen);
    RECT br = l.button;
    SelectObject(mem, g_fontButton);
    SetTextColor(mem, g_config.textColor);
    DrawTextW(mem, L"RESET", -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(mem, oldFont);
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------------
//  game window tracking (overlay mode)
// ---------------------------------------------------------------------------------
static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM result) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || hwnd == g_wnd || !IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;   // skip dialogs/tool windows
    RECT r;
    GetClientRect(hwnd, &r);
    if (r.right < 320 || r.bottom < 200) return TRUE;
    *(HWND*)result = hwnd;
    return FALSE;
}

static HWND GameWindow() {
    if (g_gameWnd && IsWindow(g_gameWnd)) return g_gameWnd;
    g_gameWnd = FindWindowW(L"Diablo II", nullptr);
    if (g_gameWnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(g_gameWnd, &pid);
        if (pid != GetCurrentProcessId()) g_gameWnd = nullptr;   // another D2 instance
    }
    if (!g_gameWnd) EnumWindows(FindGameWindowProc, (LPARAM)&g_gameWnd);
    return g_gameWnd;
}

static POINT GameClientOrigin(HWND game) {
    POINT p = { 0, 0 };
    ClientToScreen(game, &p);
    return p;
}

static bool IsOwnProcessForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Keeps the overlay glued to the game picture and hides it when the game is not visible.
static void TrackGameWindow() {
    if (g_config.mode != MeterMode::Overlay || g_dragging) return;
    HWND game = GameWindow();
    if (!game) return;

    bool visible = !IsIconic(game) && (!g_config.hideWhenGameInactive || IsOwnProcessForeground());
    if (!visible) {
        if (IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_HIDE);
        return;
    }
    POINT origin = GameClientOrigin(game);
    RECT r;
    GetWindowRect(g_wnd, &r);
    int x = origin.x + g_config.overlayOffsetX, y = origin.y + g_config.overlayOffsetY;
    if (r.left != x || r.top != y || !IsWindowVisible(g_wnd)) {
        SetWindowPos(g_wnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// ---------------------------------------------------------------------------------
//  window creation / mode switching
// ---------------------------------------------------------------------------------
static void RegisterHotkeys(HWND hwnd) {
    if (g_config.resetKey && !RegisterHotKey(hwnd, kHotkeyReset, g_config.resetModifiers | MOD_NOREPEAT, g_config.resetKey))
        LogLine(L"Reset hotkey %s is already used by another program", g_config.resetHotkeyText.c_str());
    if (g_config.modeKey && !RegisterHotKey(hwnd, kHotkeyMode, g_config.modeModifiers | MOD_NOREPEAT, g_config.modeKey))
        LogLine(L"Mode hotkey %s is already used by another program", g_config.modeHotkeyText.c_str());
}

static void CreateMeterWindow() {
    if (g_wnd) {
        HWND old = g_wnd;
        g_wnd = nullptr;
        DestroyWindow(old);
    }
    LoadLook();
    ComputeLayout();
    const Layout& l = g_layout;

    DWORD style, exStyle;
    int x, y, w = l.width, h = l.height;
    if (g_config.mode == MeterMode::Window) {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        exStyle = WS_EX_APPWINDOW | (g_config.alwaysOnTop ? WS_EX_TOPMOST : 0);
        RECT r = { 0, 0, w, h };
        AdjustWindowRectEx(&r, style, FALSE, exStyle);
        w = r.right - r.left;
        h = r.bottom - r.top;
        x = g_config.windowX;
        y = g_config.windowY;
        // keep the window reachable if the saved position is off-screen
        RECT check = { x, y, x + w, y + 40 };
        if (!MonitorFromRect(&check, MONITOR_DEFAULTTONULL)) x = y = 100;
    } else {
        style = WS_POPUP;
        exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        HWND game = GameWindow();
        POINT origin = game ? GameClientOrigin(game) : POINT{ 0, 0 };
        x = origin.x + g_config.overlayOffsetX;
        y = origin.y + g_config.overlayOffsetY;
    }

    g_wnd = CreateWindowExW(exStyle, kClassName, kTitle, style, x, y, w, h, nullptr, nullptr, g_hModule, nullptr);
    if (!g_wnd) {
        LogLine(L"CreateWindowEx failed (%u)", GetLastError());
        return;
    }
    if (g_config.mode == MeterMode::Overlay) SetLayeredWindowAttributes(g_wnd, kColorKey, 0, LWA_COLORKEY);

    RegisterHotkeys(g_wnd);
    SetTimer(g_wnd, kTimerRefresh, 100, nullptr);
    if (g_config.mode == MeterMode::Window) ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    else if (g_config.mode == MeterMode::Overlay) TrackGameWindow();
    // Hidden: the window exists (for hotkeys) but is never shown
}

static void SwitchMode(MeterMode mode) {
    g_config.mode = mode;
    SaveSettings();
    PostMessageW(g_wnd, WM_APP_RECREATE, 0, 0);
}

static void ShowHelp(HWND hwnd) {
    std::wstring text =
        L"PD2 DPS Meter\n\n"
        L"Drag the logo (overlay) or anywhere (window) to move the meter.\n"
        L"Right-click for this menu.\n\n"
        L"Reset statistics:  " + (g_config.resetKey ? g_config.resetHotkeyText : std::wstring(L"(off)")) + L"\n"
        L"Switch mode:  " + (g_config.modeKey ? g_config.modeHotkeyText : std::wstring(L"(off)")) + L"\n\n"
        L"OBS: set Mode = Window, add a \"Window Capture\" of\n"
        L"\"[Game.exe]: PD2 DPS Meter\" and a \"Color Key\" filter with the\n"
        L"Background colour from PD2DpsMeter.ini.\n\n"
        L"Settings: " + ConfigPath() + L"\nLogs: " + LogDirectory();
    MessageBoxW(hwnd, text.c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
}

// Windows colour dialog; returns false if cancelled. The 16 custom colours are kept while
// the game runs, the chosen colour itself is saved to the ini.
static bool PickColor(HWND hwnd, COLORREF& color) {
    static COLORREF custom[16] = { RGB(0, 255, 0), RGB(255, 0, 255), RGB(0, 0, 255), RGB(1, 0, 1),
                                   RGB(255, 255, 255), RGB(199, 179, 119), RGB(0, 0, 0) };
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = hwnd;
    cc.rgbResult = color;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    color = cc.rgbResult;
    return true;
}

static void ShowMenu(HWND hwnd, POINT screen) {
    HMENU menu = CreatePopupMenu();
    std::wstring reset = L"Reset statistics";
    if (g_config.resetKey) reset += L"\t" + g_config.resetHotkeyText;
    AppendMenuW(menu, MF_STRING, kMenuReset, reset.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_config.mode == MeterMode::Overlay ? MF_CHECKED : 0), kMenuOverlay, L"Mode: overlay on the game");
    AppendMenuW(menu, MF_STRING | (g_config.mode == MeterMode::Window ? MF_CHECKED : 0), kMenuWindow, L"Mode: window (for OBS)");
    AppendMenuW(menu, MF_STRING, kMenuHidden, L"Hide meter (logging continues)");
    if (g_config.mode == MeterMode::Window)
        AppendMenuW(menu, MF_STRING | (g_config.alwaysOnTop ? MF_CHECKED : 0), kMenuTopmost, L"Always on top");
    if (g_config.mode == MeterMode::Overlay)
        AppendMenuW(menu, MF_STRING | (g_config.hideWhenGameInactive ? MF_CHECKED : 0), kMenuHideInactive,
                    L"Hide while the game is not active");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU colors = CreatePopupMenu();
    AppendMenuW(colors, MF_STRING | (g_config.mode == MeterMode::Overlay ? MF_GRAYED : 0), kMenuBackground,
                L"Background colour... (window mode, OBS colour key)");
    AppendMenuW(colors, MF_STRING, kMenuLabelColor, L"Title / label colour...");
    AppendMenuW(colors, MF_STRING, kMenuValueColor, L"Value colour...");
    AppendMenuW(colors, MF_STRING | (g_config.shadow ? MF_CHECKED : 0), kMenuShadow, L"Text shadow");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)colors, L"Colours");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuLogs, L"Open log folder");
    AppendMenuW(menu, MF_STRING, kMenuEditIni, L"Edit settings");
    AppendMenuW(menu, MF_STRING, kMenuReload, L"Reload settings");
    AppendMenuW(menu, MF_STRING, kMenuHelp, L"Help");

    SetForegroundWindow(hwnd);   // required so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case kMenuReset: RequestReset(); break;
    case kMenuOverlay: SwitchMode(MeterMode::Overlay); break;
    case kMenuWindow: SwitchMode(MeterMode::Window); break;
    case kMenuHidden: SwitchMode(MeterMode::Hidden); break;
    case kMenuTopmost:
        g_config.alwaysOnTop = !g_config.alwaysOnTop;
        SetWindowPos(hwnd, g_config.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SaveSettings();
        break;
    case kMenuHideInactive:
        g_config.hideWhenGameInactive = !g_config.hideWhenGameInactive;
        SaveSettings();
        break;
    case kMenuBackground:
        if (PickColor(hwnd, g_config.background)) { SaveSettings(); InvalidateRect(hwnd, nullptr, FALSE); }
        break;
    case kMenuLabelColor:
        if (PickColor(hwnd, g_config.titleColor)) { SaveSettings(); InvalidateRect(hwnd, nullptr, FALSE); }
        break;
    case kMenuValueColor:
        if (PickColor(hwnd, g_config.textColor)) { SaveSettings(); InvalidateRect(hwnd, nullptr, FALSE); }
        break;
    case kMenuShadow:
        g_config.shadow = !g_config.shadow;
        SaveSettings();
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case kMenuLogs: ShellExecuteW(nullptr, L"open", LogDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case kMenuEditIni: ShellExecuteW(nullptr, L"open", L"notepad.exe", ConfigPath().c_str(), nullptr, SW_SHOWNORMAL); break;
    case kMenuReload:
        LoadConfig();
        ApplyFilterConfig();
        PostMessageW(hwnd, WM_APP_RECREATE, 0, 0);
        break;
    case kMenuHelp: ShowHelp(hwnd); break;
    }
}

// ---------------------------------------------------------------------------------
//  window procedure
// ---------------------------------------------------------------------------------
static bool InRect(const RECT& r, POINT p) { return PtInRect(&r, p) != FALSE; }

static LRESULT CALLBACK MeterWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        TrackGameWindow();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit != HTCLIENT) return hit;   // real caption / borders of the window mode
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &p);
        if (InRect(g_layout.button, p)) return HTCLIENT;
        if (g_config.mode == MeterMode::Window) return HTCAPTION;   // drag anywhere
        return InRect(g_layout.logoRect, p) ? HTCAPTION : HTCLIENT;
    }

    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (InRect(g_layout.button, p)) RequestReset();
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &p);
        ShowMenu(hwnd, p);
        return 0;
    }

    case WM_NCRBUTTONUP:
        ShowMenu(hwnd, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_ENTERSIZEMOVE:
        g_dragging = true;
        return 0;

    case WM_EXITSIZEMOVE: {
        g_dragging = false;
        RECT r;
        GetWindowRect(hwnd, &r);
        if (g_config.mode == MeterMode::Overlay) {
            HWND game = GameWindow();
            POINT origin = game ? GameClientOrigin(game) : POINT{ 0, 0 };
            g_config.overlayOffsetX = r.left - origin.x;
            g_config.overlayOffsetY = r.top - origin.y;
        } else {
            g_config.windowX = r.left;
            g_config.windowY = r.top;
        }
        SaveSettings();
        return 0;
    }

    case WM_HOTKEY:
        if (wParam == kHotkeyReset) {
            RequestReset();
        } else if (wParam == kHotkeyMode) {
            MeterMode next = g_config.mode == MeterMode::Overlay ? MeterMode::Window
                           : g_config.mode == MeterMode::Window ? MeterMode::Hidden : MeterMode::Overlay;
            SwitchMode(next);
        }
        return 0;

    case WM_CLOSE:   // window mode "X": hide instead of destroying (logging keeps running)
        SwitchMode(MeterMode::Hidden);
        return 0;

    case WM_APP_RECREATE:
        CreateMeterWindow();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerRefresh);
        UnregisterHotKey(hwnd, kHotkeyReset);
        UnregisterHotKey(hwnd, kHotkeyMode);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------------
//  UI thread
// ---------------------------------------------------------------------------------
static DWORD WINAPI OverlayThread(LPVOID) {
    // Per-monitor DPI awareness for this thread only: crisp text and correct positions even
    // with Windows display scaling (the game itself stays DPI unaware).
    using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto set = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MeterWndProc;
    wc.hInstance = g_hModule;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        LogLine(L"RegisterClassEx failed (%u)", GetLastError());
        return 0;
    }

    CreateMeterWindow();
    LogLine(L"Meter window created.");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete g_logo;
    g_logo = nullptr;
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}

void StartOverlay() {
    HANDLE h = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else LogLine(L"Failed to create the overlay thread (%u)", GetLastError());
}
