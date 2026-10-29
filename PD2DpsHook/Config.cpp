#include "pch.h"
#include "Config.h"
#include "Log.h"

Config g_config;

extern HMODULE g_hModule;

// Default ini written on first start. Keep it in sync with Config.h and the README.
static const char* kDefaultIni =
"; =====================================================================\r\n"
";  PD2 DPS Meter settings. Changes take effect after re-injecting, or\r\n"
";  right-click the meter -> \"Reload settings\".\r\n"
"; =====================================================================\r\n"
"\r\n"
"[Meter]\r\n"
"; Mode: overlay = transparent overlay that follows the game window\r\n"
";       window  = normal window (taskbar entry, selectable in OBS \"Window Capture\")\r\n"
";       hidden  = no window (logging only, e.g. when using the Python meter)\r\n"
"Mode=overlay\r\n"
"; window mode: keep the meter above all other windows (1/0)\r\n"
"AlwaysOnTop=1\r\n"
"; overlay mode: hide the overlay while another program is in the foreground (1/0)\r\n"
"HideWhenGameInactive=1\r\n"
"; overlay mode: position relative to the top-left corner of the game picture\r\n"
"OverlayOffsetX=20\r\n"
"OverlayOffsetY=20\r\n"
"; window mode: screen position\r\n"
"WindowX=100\r\n"
"WindowY=100\r\n"
"; overall size factor (e.g. 0.8 or 1.25)\r\n"
"Scale=1.0\r\n"
"\r\n"
"[Look]\r\n"
"; Colours as R,G,B or #RRGGBB. Easier: right-click the meter -> \"Background colour...\" etc.\r\n"
"; Background of the window mode. Pick a colour that does not occur in the text and use\r\n"
"; it in OBS as \"Color Key\" filter (default pure green).\r\n"
"Background=0,255,0\r\n"
"; TextColor = the values, TitleColor = title and labels\r\n"
"TextColor=255,255,255\r\n"
"TitleColor=199,179,119\r\n"
"ShadowColor=0,0,0\r\n"
"Shadow=1\r\n"
"; Diablo font (installed with the dummy package); FallbackFont is used if it is missing\r\n"
"Font=Exocet\r\n"
"FallbackFont=Tahoma\r\n"
"FontSize=20\r\n"
"; Logo image (BMP, PNG, JPG or GIF) relative to the DLL folder or an absolute path.\r\n"
"; It is scaled to LogoSize x LogoSize pixels. Leave empty for no logo.\r\n"
"; In overlay mode pixels with colour 1,0,1 are fully transparent.\r\n"
"Logo=logo.png\r\n"
"LogoSize=50\r\n"
"\r\n"
"[Keys]\r\n"
"; Global hotkey to reset the statistics (e.g. Ctrl+Alt+R, Ctrl+Shift+F12). Empty = off.\r\n"
"Reset=Ctrl+Alt+R\r\n"
"; Global hotkey to switch the meter: overlay -> window -> hidden -> overlay. Empty = off.\r\n"
"Mode=Ctrl+Alt+M\r\n"
"\r\n"
"[Filter]\r\n"
"; Count only damage dealt to monsters (1) or every life loss incl. players (0)\r\n"
"MonstersOnly=1\r\n"
"; Count each hit only once (server-side unit, field 0xD0 = 11). Set 0 if nothing is counted.\r\n"
"ServerUnitsOnly=1\r\n"
"; Ignore life losses up to this amount in 1/256 life points (256 = 1 life). 0 = count all,\r\n"
"; including small poison/burn ticks. The original tool used 256.\r\n"
"MinLossRaw=0\r\n"
"; Only count these monster ids (MonStats hcIdx, see the dummy build report), comma\r\n"
"; separated, ranges allowed: 1285-1292. Empty = all monsters.\r\n"
"ClassIds=\r\n"
"\r\n"
"[Log]\r\n"
"; Write every counted hit to logs\\hits_<date>.csv (for analyze_dps.py)\r\n"
"LogHits=1\r\n"
"; Start a new session automatically after a pause longer than this many seconds (0 = off,\r\n"
"; sessions then only change on RESET)\r\n"
"AutoSessionGap=0\r\n";

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    std::wstring dir(path);
    size_t pos = dir.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L".\\" : dir.substr(0, pos + 1);
}

std::wstring ConfigPath() {
    return ModuleDirectory() + L"PD2DpsMeter.ini";
}

// ---------------------------------------------------------------------------------
//  small parsing helpers
// ---------------------------------------------------------------------------------
static std::wstring ReadString(const wchar_t* section, const wchar_t* key, const std::wstring& def) {
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(section, key, def.c_str(), buf, 512, ConfigPath().c_str());
    std::wstring s(buf);
    // strip trailing comments / whitespace
    size_t c = s.find(L';');
    if (c != std::wstring::npos) s = s.substr(0, c);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}

static int ReadInt(const wchar_t* section, const wchar_t* key, int def) {
    std::wstring s = ReadString(section, key, std::to_wstring(def));
    return s.empty() ? def : _wtoi(s.c_str());
}

static bool ReadBool(const wchar_t* section, const wchar_t* key, bool def) {
    return ReadInt(section, key, def ? 1 : 0) != 0;
}

static double ReadDouble(const wchar_t* section, const wchar_t* key, double def) {
    wchar_t d[32];
    swprintf_s(d, L"%g", def);
    std::wstring s = ReadString(section, key, d);
    return s.empty() ? def : _wtof(s.c_str());
}

static COLORREF ReadColor(const wchar_t* section, const wchar_t* key, COLORREF def) {
    std::wstring s = ReadString(section, key, L"");
    if (s.empty()) return def;
    unsigned r = 0, g = 0, b = 0;
    if (s[0] == L'#' && s.size() == 7) {
        unsigned v = wcstoul(s.c_str() + 1, nullptr, 16);
        return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    }
    if (swscanf_s(s.c_str(), L"%u , %u , %u", &r, &g, &b) == 3) return RGB(r & 255, g & 255, b & 255);
    LogLine(L"Invalid colour for %s: %s", key, s.c_str());
    return def;
}

// "1285-1292, 1300" -> list of ids
static std::vector<uint32_t> ParseIdList(const std::wstring& text) {
    std::vector<uint32_t> ids;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find(L',', i);
        std::wstring part = text.substr(i, end == std::wstring::npos ? std::wstring::npos : end - i);
        unsigned a = 0, b = 0;
        if (swscanf_s(part.c_str(), L" %u - %u", &a, &b) == 2) {
            for (unsigned v = a; v <= b && v - a < 4096; ++v) ids.push_back(v);
        } else if (swscanf_s(part.c_str(), L" %u", &a) == 1) {
            ids.push_back(a);
        }
        if (end == std::wstring::npos) break;
        i = end + 1;
    }
    return ids;
}

bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& key) {
    modifiers = 0;
    key = 0;
    size_t i = 0;
    while (i <= text.size()) {
        size_t end = text.find(L'+', i);
        std::wstring part = text.substr(i, end == std::wstring::npos ? std::wstring::npos : end - i);
        while (!part.empty() && iswspace(part.back())) part.pop_back();
        while (!part.empty() && iswspace(part.front())) part.erase(part.begin());
        for (auto& ch : part) ch = towupper(ch);
        if (part == L"CTRL" || part == L"CONTROL") modifiers |= MOD_CONTROL;
        else if (part == L"ALT") modifiers |= MOD_ALT;
        else if (part == L"SHIFT") modifiers |= MOD_SHIFT;
        else if (part == L"WIN") modifiers |= MOD_WIN;
        else if (part.size() == 1 && iswalnum(part[0])) key = part[0];
        else if (part.size() >= 2 && part[0] == L'F' && _wtoi(part.c_str() + 1) >= 1 && _wtoi(part.c_str() + 1) <= 24)
            key = VK_F1 + _wtoi(part.c_str() + 1) - 1;
        else if (!part.empty()) return false;
        if (end == std::wstring::npos) break;
        i = end + 1;
    }
    return key != 0;
}

// ---------------------------------------------------------------------------------
void LoadConfig() {
    const std::wstring path = ConfigPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, kDefaultIni, (DWORD)strlen(kDefaultIni), &written, nullptr);
            CloseHandle(f);
            LogLine(L"Created default settings file %s", path.c_str());
        }
    }

    Config c;
    std::wstring mode = ReadString(L"Meter", L"Mode", L"overlay");
    for (auto& ch : mode) ch = towlower(ch);
    c.mode = mode == L"window" ? MeterMode::Window : mode == L"hidden" ? MeterMode::Hidden : MeterMode::Overlay;
    c.alwaysOnTop = ReadBool(L"Meter", L"AlwaysOnTop", c.alwaysOnTop);
    c.hideWhenGameInactive = ReadBool(L"Meter", L"HideWhenGameInactive", c.hideWhenGameInactive);
    c.overlayOffsetX = ReadInt(L"Meter", L"OverlayOffsetX", c.overlayOffsetX);
    c.overlayOffsetY = ReadInt(L"Meter", L"OverlayOffsetY", c.overlayOffsetY);
    c.windowX = ReadInt(L"Meter", L"WindowX", c.windowX);
    c.windowY = ReadInt(L"Meter", L"WindowY", c.windowY);
    c.scale = ReadDouble(L"Meter", L"Scale", c.scale);
    if (c.scale < 0.5) c.scale = 0.5;
    if (c.scale > 3.0) c.scale = 3.0;

    c.background = ReadColor(L"Look", L"Background", c.background);
    c.textColor = ReadColor(L"Look", L"TextColor", c.textColor);
    c.titleColor = ReadColor(L"Look", L"TitleColor", c.titleColor);
    c.shadowColor = ReadColor(L"Look", L"ShadowColor", c.shadowColor);
    c.shadow = ReadBool(L"Look", L"Shadow", c.shadow);
    c.fontName = ReadString(L"Look", L"Font", c.fontName);
    c.fallbackFont = ReadString(L"Look", L"FallbackFont", c.fallbackFont);
    c.fontSize = ReadInt(L"Look", L"FontSize", c.fontSize);
    c.logo = ReadString(L"Look", L"Logo", c.logo);
    c.logoSize = ReadInt(L"Look", L"LogoSize", c.logoSize);

    c.resetHotkeyText = ReadString(L"Keys", L"Reset", c.resetHotkeyText);
    if (c.resetHotkeyText.empty()) {
        c.resetKey = 0;
    } else if (!ParseHotkey(c.resetHotkeyText, c.resetModifiers, c.resetKey)) {
        LogLine(L"Invalid reset hotkey \"%s\" - hotkey disabled", c.resetHotkeyText.c_str());
        c.resetKey = 0;
    }

    c.modeHotkeyText = ReadString(L"Keys", L"Mode", c.modeHotkeyText);
    if (c.modeHotkeyText.empty()) {
        c.modeKey = 0;
    } else if (!ParseHotkey(c.modeHotkeyText, c.modeModifiers, c.modeKey)) {
        LogLine(L"Invalid mode hotkey \"%s\" - hotkey disabled", c.modeHotkeyText.c_str());
        c.modeKey = 0;
    }

    c.monstersOnly = ReadBool(L"Filter", L"MonstersOnly", c.monstersOnly);
    c.serverUnitsOnly = ReadBool(L"Filter", L"ServerUnitsOnly", c.serverUnitsOnly);
    c.minLossRaw = ReadInt(L"Filter", L"MinLossRaw", c.minLossRaw);
    c.classIds = ParseIdList(ReadString(L"Filter", L"ClassIds", L""));

    c.logHits = ReadBool(L"Log", L"LogHits", c.logHits);
    c.autoSessionGap = ReadDouble(L"Log", L"AutoSessionGap", c.autoSessionGap);

    g_config = c;
    LogLine(L"Settings loaded: mode=%s monstersOnly=%d serverUnitsOnly=%d minLossRaw=%d classIds=%u",
            mode.c_str(), c.monstersOnly, c.serverUnitsOnly, c.minLossRaw, (unsigned)c.classIds.size());
}

std::wstring ColorToText(COLORREF c) {
    return std::to_wstring(GetRValue(c)) + L"," + std::to_wstring(GetGValue(c)) + L"," + std::to_wstring(GetBValue(c));
}

// Writes everything that can be changed in the meter UI back to the ini (comments are kept).
void SaveSettings() {
    const std::wstring path = ConfigPath();
    auto put = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
    };
    put(L"Meter", L"Mode", g_config.mode == MeterMode::Window ? L"window"
                         : g_config.mode == MeterMode::Hidden ? L"hidden" : L"overlay");
    put(L"Meter", L"AlwaysOnTop", g_config.alwaysOnTop ? L"1" : L"0");
    put(L"Meter", L"HideWhenGameInactive", g_config.hideWhenGameInactive ? L"1" : L"0");
    put(L"Meter", L"OverlayOffsetX", std::to_wstring(g_config.overlayOffsetX));
    put(L"Meter", L"OverlayOffsetY", std::to_wstring(g_config.overlayOffsetY));
    put(L"Meter", L"WindowX", std::to_wstring(g_config.windowX));
    put(L"Meter", L"WindowY", std::to_wstring(g_config.windowY));
    put(L"Look", L"Background", ColorToText(g_config.background));
    put(L"Look", L"TextColor", ColorToText(g_config.textColor));
    put(L"Look", L"TitleColor", ColorToText(g_config.titleColor));
    put(L"Look", L"ShadowColor", ColorToText(g_config.shadowColor));
    put(L"Look", L"Shadow", g_config.shadow ? L"1" : L"0");
}
