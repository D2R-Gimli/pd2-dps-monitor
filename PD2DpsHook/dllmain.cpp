// =====================================================================================
//  PD2DpsHook.dll - DPS meter for Project Diablo 2 / Diablo II 1.13c
//
//  Injected into Game.exe by PD2Injector.exe (or python/pd2dps_meter.py --inject).
//  Start-up order (in a worker thread, never inside DllMain itself):
//    1. read PD2DpsMeter.ini            (Config.cpp)
//    2. start statistics + logging      (Stats.cpp)
//    3. patch the life update in D2Common (Hook.cpp)
//    4. create the meter window         (Overlay.cpp)
// =====================================================================================
#include "pch.h"
#include "Config.h"
#include "Hook.h"
#include "Log.h"
#include "Overlay.h"
#include "Stats.h"

HMODULE g_hModule = nullptr;

static DWORD WINAPI MainThread(LPVOID) {
    LogLine(L"---------------------------------------------------------------");
    LogLine(L"PD2DpsHook loaded into process %u", GetCurrentProcessId());
    LoadConfig();
    ApplyFilterConfig();
    StartStats();
    if (!InstallHook()) {
        LogLine(L"Hook not installed - the meter will show no damage.");
    }
    StartOverlay();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        break;
    }
    case DLL_PROCESS_DETACH:
        // reserved != nullptr: the game is exiting, the other threads are already gone.
        StopStats();
        if (reserved == nullptr) RemoveHook();   // explicit unload: restore the game code
        break;
    }
    return TRUE;
}
