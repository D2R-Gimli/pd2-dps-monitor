// =====================================================================================
//  Fake Game.exe: simulates fights so PD2DpsHook.dll can be tested without Diablo II.
//
//  fake_game.exe [seconds] [--no-inject]
//    - opens a window of class "Diablo II" (the overlay attaches to it)
//    - loads D2COMMON.dll (fake_d2common) and, unless --no-inject, PD2DpsHook.dll from ..\..\bin\Release
//      (use --no-inject to test PD2Injector.exe / pd2dps_meter.py --inject instead)
//    - hits a monster ~10x per second for 1000 +-50% damage; the monster dies at 0 life and
//      respawns with a new unit id; every 7th event damages a "player" and every 5th event
//      happens on a client-side copy (field 0xD0 != 11) - both must NOT be counted.
// =====================================================================================
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

struct FakeUnit {
    DWORD type;        // +0x00
    DWORD classId;     // +0x04
    DWORD unused;      // +0x08
    DWORD unitId;      // +0x0C
    BYTE pad[0xD0 - 0x10];
    DWORD fieldD0;     // +0xD0
};
struct FakeStatList {
    BYTE pad[0x44];
    FakeUnit* owner;   // +0x44
};

using ApplyLifeFn = void(__cdecl*)(int*, void*, int, DWORD*);

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

int wmain(int argc, wchar_t** argv) {
    int seconds = argc > 1 ? _wtoi(argv[1]) : 20;
    bool inject = !(argc > 2 && std::wstring(argv[2]) == L"--no-inject");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Diablo II";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(L"Diablo II", L"Diablo II (fake)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             200, 150, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);

    HMODULE common = LoadLibraryW(L"D2COMMON.dll");
    if (!common) { wprintf(L"D2COMMON.dll (fake) not found\n"); return 1; }
    auto applyLife = (ApplyLifeFn)GetProcAddress(common, "ApplyLife");

    if (inject) {
        HMODULE hook = LoadLibraryW(L"..\\..\\bin\\Release\\PD2DpsHook.dll");
        if (!hook) { wprintf(L"PD2DpsHook.dll not found (build Release first)\n"); return 1; }
    }
    // give the DLL time to find the pattern and patch it (or the injector time to inject)
    for (int i = 0; i < (inject ? 30 : 100); ++i) { Pump(); Sleep(100); }

    static FakeUnit monster = {}, monsterClient = {}, player = {};
    monster.type = 1; monster.classId = 1285; monster.unitId = 1000; monster.fieldD0 = 11;
    monsterClient = monster; monsterClient.fieldD0 = 0;
    player.type = 0; player.classId = 0; player.unitId = 1; player.fieldD0 = 11;
    FakeStatList monsterList = {}, clientList = {}, playerList = {};
    monsterList.owner = &monster;
    clientList.owner = &monsterClient;
    playerList.owner = &player;

    DWORD lifeId = 6;
    const int maxLife = 20000 * 256;
    int monsterLife[2] = { 0, maxLife };   // entry layout: [0] = stat id/layer, [1] = value
    int clientLife[2] = { 0, maxLife };
    int playerLife[2] = { 0, 5000 * 256 };

    double expected = 0;
    int counted = 0, kills = 0;
    srand(1234);
    DWORD end = GetTickCount() + seconds * 1000;
    for (int n = 1; GetTickCount() < end; ++n) {
        Pump();
        int damage = 500 + rand() % 1001;
        if (n % 7 == 0) {
            applyLife(playerLife, &playerList, -damage * 256, &lifeId);   // player hit: not counted
        } else if (n % 5 == 0) {
            applyLife(clientLife, &clientList, -damage * 256, &lifeId);   // client copy: not counted
        } else {
            int raw = damage * 256;
            if (raw > monsterLife[1]) raw = monsterLife[1];
            applyLife(monsterLife, &monsterList, -raw, &lifeId);
            expected += raw / 256.0;
            ++counted;
            if (monsterLife[1] <= 0) {
                ++kills;
                monsterLife[1] = maxLife;
                ++monster.unitId;
            }
        }
        Sleep(100);
    }
    wprintf(L"simulated: %d counted hits, %.0f damage, %d kills\n", counted, expected, kills);
    for (int i = 0; i < 20; ++i) { Pump(); Sleep(100); }   // let the meter catch up
    DestroyWindow(wnd);
    return 0;
}
