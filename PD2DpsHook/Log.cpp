#include "pch.h"
#include "Log.h"
#include "Config.h"

static std::mutex g_logMutex;

std::wstring LogDirectory() {
    std::wstring dir = ModuleDirectory() + L"logs\\";
    CreateDirectoryW(dir.c_str(), nullptr);  // fails harmlessly if it already exists
    return dir;
}

void LogLine(const wchar_t* format, ...) {
    wchar_t text[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1400];
    int n = sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u.%03u  %ls\r\n",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text);
    if (n <= 0) return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wstring path = LogDirectory() + L"PD2DpsMeter.log";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, (DWORD)n, &written, nullptr);
    CloseHandle(f);
}
