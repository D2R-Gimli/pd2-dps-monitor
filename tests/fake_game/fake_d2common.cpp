// =====================================================================================
//  Fake D2COMMON.dll for testing PD2DpsHook.dll without Diablo II.
//
//  ApplyLife() executes exactly the 15 byte instruction sequence the hook looks for, with
//  the same register/stack layout as the real game:
//      eax = stat entry (value at +4), ebx = stat list (owner unit at +0x44),
//      [esp+14h] = delta, [esp+18h] = pointer to the stat id
// =====================================================================================
#include <windows.h>

extern "C" __declspec(dllexport) __declspec(naked) void __cdecl ApplyLife(int* /*entry*/, void* /*statList*/,
                                                                           int /*delta*/, DWORD* /*statId*/) {
    __asm {
        push ebx
        push ebp
        push edi
        push esi
        mov  eax, [esp + 20]   // entry
        mov  ebx, [esp + 24]   // stat list
        mov  ecx, [esp + 28]   // delta
        mov  edx, [esp + 32]   // stat id pointer
        push edx               // -> [esp+18h] at the pattern
        push ecx               // -> [esp+14h] at the pattern
        sub  esp, 14h
        // mov ebp,[eax+4] / mov ecx,[esp+14h] / lea edi,[ecx+ebp] / test edi,edi / mov [eax+4],edi
        _emit 0x8B
        _emit 0x68
        _emit 0x04
        _emit 0x8B
        _emit 0x4C
        _emit 0x24
        _emit 0x14
        _emit 0x8D
        _emit 0x3C
        _emit 0x29
        _emit 0x85
        _emit 0xFF
        _emit 0x89
        _emit 0x78
        _emit 0x04
        add  esp, 1Ch
        pop  esi
        pop  edi
        pop  ebp
        pop  ebx
        ret
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
