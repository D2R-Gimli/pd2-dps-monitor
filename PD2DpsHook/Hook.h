#pragma once
// =====================================================================================
//  Hook on the D2Common stat update that changes a unit's life.
//
//  D2Common.dll contains (inside the "add value to stat" routine):
//      8B 68 04        mov  ebp, [eax+4]      ; old value (life is 24.8 fixed point)
//      8B 4C 24 14     mov  ecx, [esp+14h]    ; delta
//      8D 3C 29        lea  edi, [ecx+ebp]    ; new value
//      85 FF           test edi, edi
//      89 78 04        mov  [eax+4], edi      ; store
//  Registers at this point: ebx = stat list (owner unit at +0x44),
//  [esp+18h] = pointer to the stat id (6 = life).
//  These 15 bytes are replaced by a jump to our stub, which re-executes them and reports
//  every life loss to OnLifeLoss() (filtered according to [Filter] in the ini).
// =====================================================================================
#include "pch.h"

constexpr uint32_t kStatLife = 6;          // ItemStatCost id of "hitpoints"
constexpr uint32_t kUnitTypeMonster = 1;
constexpr uint32_t kServerUnitMarker = 11; // value of unit field 0xD0 on the counted (server) unit
constexpr uint32_t kMaxClassId = 8192;

// One counted life loss, produced inside the game thread, consumed by the stats thread.
struct HitEvent {
    LONGLONG qpc;          // QueryPerformanceCounter timestamp
    int32_t lifeBefore;    // raw (1/256)
    int32_t lifeAfter;     // raw (1/256)
    uint32_t unitType;     // 0 player, 1 monster, ... (0xFFFFFFFF = unknown)
    uint32_t classId;      // MonStats hcIdx for monsters
    uint32_t unitId;       // unique id of the unit in this game
    uint32_t fieldD0;      // raw unit field 0xD0 (diagnostics)
};

bool InstallHook();        // pattern scan + patch, returns false if D2Common is not found
void RemoveHook();
void ApplyFilterConfig();  // copy the [Filter] settings into the lock-free filter used by the hook

// Moves all pending events into 'out'. Returns the number of events dropped because the
// buffer was full (should stay 0).
uint32_t DrainHitEvents(std::vector<HitEvent>& out);
