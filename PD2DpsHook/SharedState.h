#pragma once
// =====================================================================================
//  Live statistics shared with external programs (e.g. python/pd2dps_meter.py).
//
//  The DLL publishes this struct in a named shared memory block:
//      Local\PD2DpsMeter_<process id of Game.exe>
//
//  Readers must use the sequence counter like a seqlock: read 'sequence', copy the
//  struct, read 'sequence' again; the copy is consistent if both values are equal and
//  even. Writers set commandReset = 1 to request a reset (the DLL clears it).
//
//  Keep this layout in sync with SHARED_FORMAT in python/pd2dps_meter.py and bump
//  kSharedVersion whenever it changes.
// =====================================================================================
#include <cstdint>

constexpr uint32_t kSharedMagic = 0x53504432;   // "2DPS"
constexpr uint32_t kSharedVersion = 1;

#pragma pack(push, 4)
struct SharedStats {
    uint32_t magic;            // kSharedMagic
    uint32_t version;          // kSharedVersion
    uint32_t sequence;         // odd while the DLL is writing
    uint32_t commandReset;     // set to 1 by a reader to request a reset

    double dps1;               // damage of the last 1 s
    double dps5;               // average of the last 5 s
    double dps10;              // average of the last 10 s
    double dps20;              // average of the last 20 s
    double sessionDps;         // total damage / session time
    double sessionSeconds;     // time from first hit to now (or to last hit when idle)
    double totalDamage;        // since the last reset
    double lastHit;            // damage of the most recent hit
    double maxHit;             // biggest hit since reset
    double hitsPerSecond;      // hits in the last 2 s / 2
    double lastKillSeconds;    // time-to-kill of the last killed target (0 = none yet)
    double targetHp;           // remaining life of the last hit target

    uint32_t hits;             // number of counted hits since reset
    uint32_t kills;            // targets that reached 0 life since reset
    uint32_t targetClassId;    // MonStats row (hcIdx) of the last hit target
    uint32_t sessionNumber;    // increments on every reset
};
#pragma pack(pop)
