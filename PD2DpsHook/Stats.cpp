#include "pch.h"
#include "Stats.h"
#include "Hook.h"
#include "Config.h"
#include "Log.h"

// ---------------------------------------------------------------------------------
//  State owned by the worker thread
// ---------------------------------------------------------------------------------
struct Session {
    uint32_t number = 1;
    bool started = false;        // true after the first hit
    double firstHit = 0, lastHit = 0;   // seconds (QPC based)
    double total = 0;
    double maxHit = 0, lastDamage = 0;
    double peakDps1 = 0;
    uint32_t hits = 0;
    uint32_t kills = 0;
    double lastTtk = 0;
    SYSTEMTIME startWall = {};
};

static Session g_session;
static std::deque<std::pair<double, double>> g_recent;        // (time, damage) of the last 20 s
static std::unordered_map<uint32_t, double> g_firstHitByUnit;  // unit id -> time of first hit
static uint32_t g_lastTargetClass = 0;
static double g_lastTargetHp = 0;

static LARGE_INTEGER g_qpcFreq = {};
static LARGE_INTEGER g_qpcStart = {};
static FILETIME g_wallStart = {};

static HANDLE g_hitLog = INVALID_HANDLE_VALUE;
static std::atomic<bool> g_resetRequested{false};
static std::atomic<bool> g_running{false};

// published snapshot (for the overlay of this DLL) + shared memory (for other programs)
static std::mutex g_snapshotMutex;
static SharedStats g_snapshot = {};
static HANDLE g_sharedMapping = nullptr;
static SharedStats* g_shared = nullptr;

// ---------------------------------------------------------------------------------
//  time helpers
// ---------------------------------------------------------------------------------
static double QpcToSeconds(LONGLONG qpc) {
    return double(qpc - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart);
}

static double NowSeconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return QpcToSeconds(now.QuadPart);
}

// wall clock of a QPC based time stamp
static SYSTEMTIME WallTime(double seconds) {
    ULARGE_INTEGER t;
    t.LowPart = g_wallStart.dwLowDateTime;
    t.HighPart = g_wallStart.dwHighDateTime;
    t.QuadPart += (ULONGLONG)(seconds * 10000000.0);
    FILETIME ft, local;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st;
    FileTimeToSystemTime(&local, &st);
    return st;
}

// ---------------------------------------------------------------------------------
//  files
// ---------------------------------------------------------------------------------
static void WriteText(HANDLE f, const char* text) {
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, (DWORD)strlen(text), &written, nullptr);
}

static void OpenHitLog() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"hits_%04u%02u%02u_%02u%02u%02u.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring path = LogDirectory() + name;
    g_hitLog = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_hitLog == INVALID_HANDLE_VALUE) {
        LogLine(L"Could not create hit log %s (%u)", path.c_str(), GetLastError());
        return;
    }
    char header[1024];
    sprintf_s(header,
        "# PD2 DPS Meter hit log, format 2\r\n"
        "# filter: monsters_only=%d server_units_only=%d min_loss_raw=%d class_filter=%u ids\r\n"
        "# time = local wall clock, t = seconds since the session's first hit,\r\n"
        "# damage/life in life points (raw game value / 256), killed = life reached 0\r\n"
        "time,session,t,damage,life_before,life_after,killed,unit_type,class_id,unit_id,field_d0\r\n",
        g_config.monstersOnly, g_config.serverUnitsOnly, g_config.minLossRaw, (unsigned)g_config.classIds.size());
    WriteText(g_hitLog, header);
    LogLine(L"Hit log: %s", path.c_str());
}

static void AppendSessionSummary(const Session& s) {
    if (!s.started || s.hits == 0) return;
    std::wstring path = LogDirectory() + L"sessions.csv";
    bool isNew = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    if (isNew) WriteText(f, "start,session,duration_s,total_damage,avg_dps,peak_dps_1s,hits,hits_per_s,avg_hit,max_hit,kills,last_ttk_s\r\n");
    double duration = s.lastHit - s.firstHit;
    double dur = duration > 1.0 ? duration : 1.0;
    char line[512];
    sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u,%u,%.3f,%.1f,%.1f,%.1f,%u,%.2f,%.1f,%.1f,%u,%.3f\r\n",
              s.startWall.wYear, s.startWall.wMonth, s.startWall.wDay, s.startWall.wHour, s.startWall.wMinute,
              s.startWall.wSecond, s.number, duration, s.total, s.total / dur, s.peakDps1, s.hits, s.hits / dur,
              s.total / s.hits, s.maxHit, s.kills, s.lastTtk);
    WriteText(f, line);
    CloseHandle(f);
}

// ---------------------------------------------------------------------------------
//  processing
// ---------------------------------------------------------------------------------
static void Reset() {
    AppendSessionSummary(g_session);
    uint32_t next = g_session.number + 1;
    g_session = Session();
    g_session.number = next;
    g_recent.clear();
    g_firstHitByUnit.clear();
    g_lastTargetClass = 0;
    g_lastTargetHp = 0;
    if (g_hitLog != INVALID_HANDLE_VALUE) {
        char line[64];
        sprintf_s(line, "# reset -> session %u\r\n", next);
        WriteText(g_hitLog, line);
    }
}

static void ProcessHit(const HitEvent& ev) {
    double t = QpcToSeconds(ev.qpc);
    double damage = (ev.lifeBefore - ev.lifeAfter) / 256.0;
    bool killed = ev.lifeAfter <= 0;

    // optional automatic session split after a long pause
    if (g_session.started && g_config.autoSessionGap > 0 && t - g_session.lastHit > g_config.autoSessionGap) Reset();

    if (!g_session.started) {
        g_session.started = true;
        g_session.firstHit = t;
        g_session.startWall = WallTime(t);
    }
    g_session.lastHit = t;
    g_session.total += damage;
    g_session.lastDamage = damage;
    if (damage > g_session.maxHit) g_session.maxHit = damage;
    ++g_session.hits;
    g_recent.emplace_back(t, damage);

    auto first = g_firstHitByUnit.find(ev.unitId);
    if (first == g_firstHitByUnit.end()) first = g_firstHitByUnit.emplace(ev.unitId, t).first;
    if (killed) {
        ++g_session.kills;
        g_session.lastTtk = t - first->second;
        g_firstHitByUnit.erase(first);
    }
    g_lastTargetClass = ev.classId;
    g_lastTargetHp = ev.lifeAfter > 0 ? ev.lifeAfter / 256.0 : 0.0;

    if (g_config.logHits && g_hitLog != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st = WallTime(t);
        char line[256];
        sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u.%03u,%u,%.3f,%.3f,%.3f,%.3f,%d,%u,%u,%u,%u\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  g_session.number, t - g_session.firstHit, damage, ev.lifeBefore / 256.0, ev.lifeAfter / 256.0,
                  killed ? 1 : 0, ev.unitType, ev.classId, ev.unitId, ev.fieldD0);
        WriteText(g_hitLog, line);
    }
}

static double WindowDamage(double now, double seconds) {
    double sum = 0;
    for (auto it = g_recent.rbegin(); it != g_recent.rend() && now - it->first <= seconds; ++it) sum += it->second;
    return sum;
}

static void Publish(double now) {
    while (!g_recent.empty() && now - g_recent.front().first > 20.0) g_recent.pop_front();

    SharedStats s = {};
    s.magic = kSharedMagic;
    s.version = kSharedVersion;
    s.dps1 = WindowDamage(now, 1.0);
    s.dps5 = WindowDamage(now, 5.0) / 5.0;
    s.dps10 = WindowDamage(now, 10.0) / 10.0;
    s.dps20 = WindowDamage(now, 20.0) / 20.0;
    if (s.dps1 > g_session.peakDps1) g_session.peakDps1 = s.dps1;

    if (g_session.started) {
        bool idle = now - g_session.lastHit > kIdleSeconds;
        s.sessionSeconds = (idle ? g_session.lastHit : now) - g_session.firstHit;
        s.sessionDps = g_session.total / (s.sessionSeconds > 1.0 ? s.sessionSeconds : 1.0);
    }
    s.totalDamage = g_session.total;
    s.lastHit = g_session.lastDamage;
    s.maxHit = g_session.maxHit;
    double hits2 = 0;
    for (auto it = g_recent.rbegin(); it != g_recent.rend() && now - it->first <= 2.0; ++it) ++hits2;
    s.hitsPerSecond = hits2 / 2.0;
    s.lastKillSeconds = g_session.lastTtk;
    s.targetHp = g_lastTargetHp;
    s.hits = g_session.hits;
    s.kills = g_session.kills;
    s.targetClassId = g_lastTargetClass;
    s.sessionNumber = g_session.number;

    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshot = s;
    }

    if (g_shared) {
        // seqlock: odd sequence while writing
        uint32_t seq = g_shared->sequence;
        InterlockedExchange((volatile LONG*)&g_shared->sequence, (LONG)(seq + 1));
        uint32_t reset = g_shared->commandReset;
        s.sequence = seq + 1;
        s.commandReset = reset;
        *g_shared = s;
        InterlockedExchange((volatile LONG*)&g_shared->sequence, (LONG)(seq + 2));
    }
}

static DWORD WINAPI StatsThread(LPVOID) {
    std::vector<HitEvent> events;
    events.reserve(4096);
    while (g_running) {
        Sleep(50);

        if (g_shared && InterlockedExchange((volatile LONG*)&g_shared->commandReset, 0) != 0) g_resetRequested = true;
        if (g_resetRequested.exchange(false)) Reset();

        events.clear();
        uint32_t dropped = DrainHitEvents(events);
        if (dropped) LogLine(L"Warning: %u hit events dropped (buffer full)", dropped);
        for (const HitEvent& ev : events) ProcessHit(ev);

        Publish(NowSeconds());
    }
    return 0;
}

// ---------------------------------------------------------------------------------
//  public API
// ---------------------------------------------------------------------------------
void StartStats() {
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    GetSystemTimePreciseAsFileTime(&g_wallStart);

    wchar_t name[64];
    swprintf_s(name, L"Local\\PD2DpsMeter_%u", GetCurrentProcessId());
    g_sharedMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedStats), name);
    if (g_sharedMapping) {
        g_shared = (SharedStats*)MapViewOfFile(g_sharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedStats));
        if (g_shared) {
            memset(g_shared, 0, sizeof(SharedStats));
            LogLine(L"Shared memory: %s", name);
        }
    }

    if (g_config.logHits) OpenHitLog();

    g_running = true;
    HANDLE h = CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else LogLine(L"Failed to create the stats thread (%u)", GetLastError());
}

void StopStats() {
    g_running = false;
    AppendSessionSummary(g_session);
    if (g_hitLog != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hitLog);
        g_hitLog = INVALID_HANDLE_VALUE;
    }
}

void RequestReset() {
    g_resetRequested = true;
}

SharedStats GetStatsSnapshot() {
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    return g_snapshot;
}
