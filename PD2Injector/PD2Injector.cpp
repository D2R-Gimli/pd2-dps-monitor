// =====================================================================================
//  PD2Injector.exe - loads PD2DpsHook.dll into the running game (Game.exe).
//
//  Usage:  PD2Injector.exe [--wait <seconds>] [--process <name.exe>] [--quiet]
//    --wait     wait up to this many seconds for the game to start (default 120)
//    --process  name of the game process (default Game.exe)
//    --quiet    do not wait for Enter at the end
//
//  The DLL is always taken from the folder of this exe (not the current directory), so a
//  shortcut with any "Start in" folder works. Every running Game.exe that does not have the
//  DLL loaded yet gets it injected (PD2 allows several game instances).
//  Must be built as 32 bit (Win32) like the game.
// =====================================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>

static const wchar_t* kDllName = L"PD2DpsHook.dll";

static void Print(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    vwprintf(format, args);
    va_end(args);
    fflush(stdout);
}

static std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

static std::vector<DWORD> FindProcesses(const std::wstring& name) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static bool HasModule(DWORD pid, const wchar_t* module) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, module) == 0) found = true;
        } while (!found && Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static void EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(token);   // works without it for normal (non elevated) games
}

// Classic LoadLibraryW injection: write the DLL path into the game and start a thread there.
static bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        Print(L"  ERROR: cannot open the game process (error %u).\n", GetLastError());
        Print(L"  If the game runs as administrator, start the injector as administrator too.\n");
        return false;
    }

    BOOL wow64 = FALSE;
    IsWow64Process(process, &wow64);
#ifndef _WIN64
    BOOL selfWow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfWow64);
    if (wow64 != selfWow64) {
        Print(L"  ERROR: the game is not a 32 bit process - wrong Game.exe?\n");
        CloseHandle(process);
        return false;
    }
#endif

    SIZE_T size = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (remote && WriteProcessMemory(process, remote, dllPath.c_str(), size, nullptr)) {
        auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (thread) {
            WaitForSingleObject(thread, 15000);
            DWORD result = 0;
            GetExitCodeThread(thread, &result);
            CloseHandle(thread);
            ok = result != 0;
            if (!ok) Print(L"  ERROR: LoadLibrary failed inside the game (missing file or blocked by antivirus?).\n");
        } else {
            Print(L"  ERROR: CreateRemoteThread failed (error %u).\n", GetLastError());
        }
    } else {
        Print(L"  ERROR: could not write into the game process (error %u).\n", GetLastError());
    }
    if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring processName = L"Game.exe";
    int waitSeconds = 120;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--wait" && i + 1 < argc) waitSeconds = _wtoi(argv[++i]);
        else if (a == L"--process" && i + 1 < argc) processName = argv[++i];
        else if (a == L"--quiet") quiet = true;
        else {
            Print(L"Usage: PD2Injector.exe [--wait <seconds>] [--process <name.exe>] [--quiet]\n");
            return 2;
        }
    }

    SetConsoleTitleW(L"PD2 DPS Meter - Injector");
    Print(L"PD2 DPS Meter injector\n======================\n");

    std::wstring dllPath = ExeDirectory() + kDllName;
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Print(L"ERROR: %s not found next to the injector:\n  %s\n", kDllName, dllPath.c_str());
        if (!quiet) { Print(L"\nPress Enter to close..."); getwchar(); }
        return 1;
    }
    Print(L"DLL: %s\n", dllPath.c_str());
    EnableDebugPrivilege();

    std::vector<DWORD> pids = FindProcesses(processName);
    if (pids.empty() && waitSeconds > 0) {
        Print(L"Waiting for %s (up to %d s, start the game now)...\n", processName.c_str(), waitSeconds);
        for (int t = 0; t < waitSeconds * 2 && pids.empty(); ++t) {
            Sleep(500);
            pids = FindProcesses(processName);
        }
        if (!pids.empty()) Sleep(3000);   // give the game a moment to load its DLLs
    }
    if (pids.empty()) {
        Print(L"ERROR: %s is not running.\n", processName.c_str());
        if (!quiet) { Print(L"\nPress Enter to close..."); getwchar(); }
        return 1;
    }

    int injected = 0, failed = 0;
    for (DWORD pid : pids) {
        Print(L"%s (PID %u): ", processName.c_str(), pid);
        if (HasModule(pid, kDllName)) {
            Print(L"meter already running.\n");
            continue;
        }
        Print(L"injecting...\n");
        if (Inject(pid, dllPath)) {
            Print(L"  OK - the meter appears in a moment. Right-click it for options.\n");
            ++injected;
        } else {
            ++failed;
        }
    }

    Print(L"\nSettings: %sPD2DpsMeter.ini   Logs: %slogs\\\n", ExeDirectory().c_str(), ExeDirectory().c_str());
    if (failed) {
        if (!quiet) { Print(L"\nPress Enter to close..."); getwchar(); }
        return 1;
    }
    if (!quiet) {
        Print(L"\nThis window closes in 5 seconds (it is not needed while playing).\n");
        Sleep(5000);
    }
    return 0;
}
