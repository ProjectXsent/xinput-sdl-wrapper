#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace xlog {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE  g_file = INVALID_HANDLE_VALUE;
bool    g_enabled = false, g_verbose = false, g_dbgOut = false;
char    g_path[MAX_PATH + 32] = {};

const char* kFileName = "xinput_sdl.log";

const char* Tag(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        default:           return "ERROR";
    }
}

HANDLE OpenLog(const char* path) {
    return CreateFileA(path, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

void Init(const char* dir, const Options& opt) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE || g_enabled) {  // already initialised
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_enabled = opt.enabled;
    g_verbose = opt.verbose;
    g_dbgOut  = opt.debugOutput;

    bool usedTemp = false;
    if (g_enabled) {
        snprintf(g_path, sizeof(g_path), "%s%s", dir ? dir : "", kFileName);
        g_file = OpenLog(g_path);
        if (g_file == INVALID_HANDLE_VALUE) {
            char tmp[MAX_PATH];
            DWORD n = GetTempPathA(sizeof(tmp), tmp);
            if (n > 0 && n < sizeof(tmp)) {
                snprintf(g_path, sizeof(g_path), "%s%s", tmp, kFileName);
                g_file = OpenLog(g_path);
                usedTemp = (g_file != INVALID_HANDLE_VALUE);
            }
        }
        if (g_file == INVALID_HANDLE_VALUE) g_enabled = g_dbgOut;  // keep OutputDebugString only
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (g_file != INVALID_HANDLE_VALUE) {
        LOGI("log opened: %s%s", g_path, usedTemp ? " (game folder not writable, using %TEMP%)" : "");
    } else if (opt.enabled) {
        OutputDebugStringA("[input_sdl] could not create xinput_sdl.log\n");
    }
}

void Shutdown() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_enabled = false;
    ReleaseSRWLockExclusive(&g_lock);
}

bool Enabled() { return g_enabled; }
bool Verbose() { return g_enabled && g_verbose; }

void WriteV(Level lvl, const char* fmt, va_list ap) {
    if (!g_enabled) return;
    if (lvl == Level::Debug && !g_verbose) return;

    char line[1280];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(line, sizeof(line) - 3, "%02u:%02u:%02u.%03u [%5lu] %s ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentThreadId(), Tag(lvl));
    if (n < 0) n = 0;
    int cap = (int)sizeof(line) - 3 - n;
    int m = vsnprintf(line + n, (size_t)cap, fmt, ap);
    if (m < 0) m = 0;
    if (m >= cap) m = cap - 1;
    int len = n + m;
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = '\0';

    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, (DWORD)len, &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (g_dbgOut) OutputDebugStringA(line);
}

void Write(Level lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV(lvl, fmt, ap);
    va_end(ap);
}

}  // namespace xlog
