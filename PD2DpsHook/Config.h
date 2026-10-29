#pragma once
// =====================================================================================
//  Settings from PD2DpsMeter.ini (next to the DLL). The file is created with commented
//  defaults on first start. Everything changed in the meter UI (mode, position, colours,
//  always-on-top, hide-when-inactive) is written back, so the next start looks the same.
// =====================================================================================
#include "pch.h"

enum class MeterMode {
    Hidden = 0,   // no window, only logging + shared memory (e.g. for the Python meter)
    Overlay = 1,  // transparent overlay glued to the game window
    Window = 2,   // normal application window (selectable in OBS "Window Capture")
};

struct Config {
    // [Meter]
    MeterMode mode = MeterMode::Overlay;
    bool alwaysOnTop = true;            // window mode: keep above other windows
    bool hideWhenGameInactive = true;   // overlay mode: hide while another program is in front
    int overlayOffsetX = 20;            // overlay mode: position relative to the game client area
    int overlayOffsetY = 20;
    int windowX = 100;                  // window mode: screen position
    int windowY = 100;
    double scale = 1.0;                 // size of fonts/layout

    // [Look]
    COLORREF background = RGB(0, 255, 0);  // window mode background (use as OBS colour key)
    COLORREF textColor = RGB(255, 255, 255);
    COLORREF titleColor = RGB(199, 179, 119);
    COLORREF shadowColor = RGB(0, 0, 0);
    bool shadow = true;
    std::wstring fontName = L"Exocet";
    std::wstring fallbackFont = L"Tahoma";
    int fontSize = 20;
    std::wstring logo = L"logo.png";    // BMP/PNG/JPG/GIF, relative to the DLL folder; empty = none
    int logoSize = 50;

    // [Keys]
    UINT resetModifiers = MOD_CONTROL | MOD_ALT;
    UINT resetKey = 'R';
    std::wstring resetHotkeyText = L"Ctrl+Alt+R";
    UINT modeModifiers = MOD_CONTROL | MOD_ALT;   // cycles overlay -> window -> hidden
    UINT modeKey = 'M';
    std::wstring modeHotkeyText = L"Ctrl+Alt+M";

    // [Filter]
    bool monstersOnly = true;           // count only damage taken by monsters (unit type 1)
    bool serverUnitsOnly = true;        // only units with field 0xD0 == 11 (avoids double counting)
    int minLossRaw = 0;                 // ignore HP losses <= this (raw 1/256 HP units; 256 = 1 HP)
    std::vector<uint32_t> classIds;     // only these MonStats ids (empty = all monsters)

    // [Log]
    bool logHits = true;                // write every counted hit to logs\hits_*.csv
    double autoSessionGap = 0.0;        // >0: a pause longer than this (s) starts a new session
};

extern Config g_config;

std::wstring ModuleDirectory();                 // folder of PD2DpsHook.dll (with trailing '\')
std::wstring ConfigPath();
void LoadConfig();
void SaveSettings();                            // writes mode, positions, colours back to the ini
std::wstring ColorToText(COLORREF c);           // "R,G,B"
bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& key);
