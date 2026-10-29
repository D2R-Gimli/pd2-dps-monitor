#pragma once
// =====================================================================================
//  The meter window (runs in its own UI thread inside the game process).
//
//  Modes (see [Meter] Mode in the ini, switchable at runtime via right-click menu or hotkey):
//    Overlay  borderless, transparent (colour key 1,0,1), glued to the game picture; follows
//             the game window when it is moved, hides when the game is minimised/inactive.
//    Window   normal window with title "PD2 DPS Meter" and a taskbar entry, solid background
//             colour. OBS: "Window Capture" -> [Game.exe]: PD2 DPS Meter, then add a
//             "Color Key" filter with the Background colour.
//    Hidden   no window; logging, hotkeys and the shared memory keep working.
// =====================================================================================
#include "pch.h"

void StartOverlay();
