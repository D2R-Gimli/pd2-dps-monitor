#pragma once
// Diagnostic log: logs\PD2DpsMeter.log next to the DLL (start-up, errors, settings).
// The per-hit data goes to separate CSV files, see Stats.cpp.
#include "pch.h"

std::wstring LogDirectory();                       // <dll folder>\logs\ (created on demand)
void LogLine(const wchar_t* format, ...);
