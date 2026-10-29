#pragma once
// =====================================================================================
//  Statistics: turns the raw hit events into DPS values, writes the CSV logs and
//  publishes everything to the shared memory block (SharedState.h).
//
//  Session = all hits since the last reset (button, hotkey, menu or external request).
//  Session time runs from the first hit to "now" while hits keep coming, and freezes at
//  the last hit once no hit arrived for kIdleSeconds.
//
//  Files (in <dll folder>\logs\):
//    hits_<date>_<time>.csv  one line per counted hit (written while LogHits=1)
//    sessions.csv            one summary line per finished session
// =====================================================================================
#include "pch.h"
#include "SharedState.h"

constexpr double kIdleSeconds = 3.0;

void StartStats();           // creates the shared memory and the worker thread
void StopStats();            // writes the final session summary (called at process exit)
void RequestReset();         // thread safe: resets on the next worker tick
SharedStats GetStatsSnapshot();
