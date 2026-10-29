#include "pch.h"
#include "Hook.h"
#include "Config.h"
#include "Log.h"

// ---------------------------------------------------------------------------------
//  Filter settings used inside the game thread (plain values, no locking needed)
// ---------------------------------------------------------------------------------
static volatile bool g_monstersOnly = true;
static volatile bool g_serverUnitsOnly = true;
static volatile int32_t g_minLossRaw = 0;
static volatile bool g_useClassFilter = false;
static bool g_classAllowed[kMaxClassId] = {};

void ApplyFilterConfig() {
    g_useClassFilter = false;
    memset(g_classAllowed, 0, sizeof(g_classAllowed));
    for (uint32_t id : g_config.classIds)
        if (id < kMaxClassId) g_classAllowed[id] = true;
    g_useClassFilter = !g_config.classIds.empty();
    g_monstersOnly = g_config.monstersOnly;
    g_serverUnitsOnly = g_config.serverUnitsOnly;
    g_minLossRaw = g_config.minLossRaw;
}

// ---------------------------------------------------------------------------------
//  Event buffer: ring buffer protected by a tiny spin lock (the game may update stats
//  from more than one thread; the critical section is a few instructions long)
// ---------------------------------------------------------------------------------
constexpr uint32_t kRingSize = 16384;
static HitEvent g_ring[kRingSize];
static uint32_t g_ringHead = 0;     // next write position
static uint32_t g_ringCount = 0;
static uint32_t g_ringDropped = 0;
static volatile LONG g_ringLock = 0;

static void Lock() {
    while (InterlockedExchange(&g_ringLock, 1) != 0) YieldProcessor();
}
static void Unlock() {
    InterlockedExchange(&g_ringLock, 0);
}

uint32_t DrainHitEvents(std::vector<HitEvent>& out) {
    Lock();
    uint32_t start = (g_ringHead + kRingSize - g_ringCount) % kRingSize;
    for (uint32_t i = 0; i < g_ringCount; ++i) out.push_back(g_ring[(start + i) % kRingSize]);
    g_ringCount = 0;
    uint32_t dropped = g_ringDropped;
    g_ringDropped = 0;
    Unlock();
    return dropped;
}

// ---------------------------------------------------------------------------------
//  Called from the hook stub for every stat change. Must be fast and must never throw.
// ---------------------------------------------------------------------------------
extern "C" void __cdecl OnStatChange(const uint8_t* statList, int32_t oldRaw, int32_t newRaw, const uint32_t* statId) {
    __try {
        if (!statId || *statId != kStatLife) return;
        if (oldRaw - newRaw <= g_minLossRaw || oldRaw <= newRaw) return;  // only losses

        HitEvent ev = {};
        ev.lifeBefore = oldRaw;
        ev.lifeAfter = newRaw;
        ev.unitType = 0xFFFFFFFF;

        // the stat list keeps a pointer to its owner unit at +0x44
        const uint8_t* unit = nullptr;
        if ((uintptr_t)statList > 0x10000) unit = *(const uint8_t* const*)(statList + 0x44);
        if ((uintptr_t)unit > 0x10000) {
            ev.unitType = *(const uint32_t*)(unit + 0x00);
            ev.classId = *(const uint32_t*)(unit + 0x04);
            ev.unitId = *(const uint32_t*)(unit + 0x0C);
            ev.fieldD0 = *(const uint32_t*)(unit + 0xD0);
        }

        if (g_monstersOnly && ev.unitType != kUnitTypeMonster) return;
        if (g_serverUnitsOnly && ev.fieldD0 != kServerUnitMarker) return;
        if (g_useClassFilter && (ev.classId >= kMaxClassId || !g_classAllowed[ev.classId])) return;

        QueryPerformanceCounter((LARGE_INTEGER*)&ev.qpc);

        Lock();
        if (g_ringCount < kRingSize) {
            g_ring[g_ringHead] = ev;
            g_ringHead = (g_ringHead + 1) % kRingSize;
            ++g_ringCount;
        } else {
            ++g_ringDropped;
        }
        Unlock();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // a stale pointer must never crash the game - just skip this event
    }
}

// ---------------------------------------------------------------------------------
//  Patching
// ---------------------------------------------------------------------------------
static const unsigned char kPattern[] = {
    0x8B, 0x68, 0x04,             // mov  ebp, [eax+4]
    0x8B, 0x4C, 0x24, 0x14,       // mov  ecx, [esp+14h]
    0x8D, 0x3C, 0x29,             // lea  edi, [ecx+ebp]
    0x85, 0xFF,                   // test edi, edi
    0x89, 0x78, 0x04              // mov  [eax+4], edi
};
constexpr size_t kHookLen = sizeof(kPattern);   // 15 bytes

static uint8_t* g_hookAddress = nullptr;
static uint8_t g_originalBytes[kHookLen] = {};
static void* g_returnAddress = nullptr;

// Re-executes the overwritten instructions, reports the change, then continues in the game.
__declspec(naked) static void HookStub() {
    __asm {
        mov  ebp, [eax + 4]          // old value
        mov  ecx, [esp + 14h]        // delta
        lea  edi, [ecx + ebp]        // new value

        pushfd                       // keep every register and flag of the game
        pushad                       // (pushfd + pushad = 0x24 bytes on the stack)
        mov  edx, [esp + 24h + 18h]  // stat id pointer (was [esp+18h] before our pushes)
        push edx
        push edi
        push ebp
        push ebx                     // stat list
        call OnStatChange
        add  esp, 16
        popad
        popfd

        test edi, edi                // original flags for the code that follows
        mov  [eax + 4], edi
        jmp  dword ptr [g_returnAddress]
    }
}

static uint8_t* FindPattern(HMODULE module) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto* base = reinterpret_cast<uint8_t*>(module);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    for (DWORD i = 0; i + kHookLen <= size; ++i) {
        if (memcmp(base + i, kPattern, kHookLen) == 0) return base + i;
    }
    return nullptr;
}

bool InstallHook() {
    HMODULE d2common = nullptr;
    // D2Common may be loaded a moment after the game process starts
    for (int i = 0; i < 100 && !d2common; ++i) {
        d2common = GetModuleHandleW(L"D2COMMON.dll");
        if (!d2common) Sleep(100);
    }
    if (!d2common) {
        LogLine(L"D2COMMON.dll not found - is this Diablo II / Project Diablo 2?");
        return false;
    }

    g_hookAddress = FindPattern(d2common);
    if (!g_hookAddress) {
        LogLine(L"Life update code not found in D2COMMON.dll (already hooked, or unsupported game version).");
        return false;
    }
    LogLine(L"Hook location D2COMMON.dll+0x%X", (unsigned)(g_hookAddress - reinterpret_cast<uint8_t*>(d2common)));

    g_returnAddress = g_hookAddress + kHookLen;
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_hookAddress, kHookLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogLine(L"VirtualProtect failed (%u)", GetLastError());
        return false;
    }
    memcpy(g_originalBytes, g_hookAddress, kHookLen);

    // Only the first 5 bytes become "jmp HookStub". The remaining original bytes stay in
    // place: they are never reached through the jump (the stub returns behind them), and a
    // game thread that is just executing them continues with valid code.
    uint8_t jump[5];
    jump[0] = 0xE9;
    *reinterpret_cast<int32_t*>(jump + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HookStub) - reinterpret_cast<uintptr_t>(g_hookAddress) - 5);
    memcpy(g_hookAddress, jump, 5);

    VirtualProtect(g_hookAddress, kHookLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), g_hookAddress, kHookLen);
    LogLine(L"Hook installed.");
    return true;
}

void RemoveHook() {
    if (!g_hookAddress) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_hookAddress, kHookLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(g_hookAddress, g_originalBytes, kHookLen);
        VirtualProtect(g_hookAddress, kHookLen, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), g_hookAddress, kHookLen);
    }
    g_hookAddress = nullptr;
}
