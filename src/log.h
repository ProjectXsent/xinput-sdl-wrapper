// Tiny thread-safe file logger -> xinput_sdl.log (next to the wrapper DLL).
#pragma once

#include <cstdarg>

namespace xlog {

enum class Level { Debug, Info, Warn, Error };

struct Options {
    bool enabled = true;       // write xinput_sdl.log at all
    bool verbose = false;      // include Debug lines (SDL verbose log, rumble, ...)
    bool debugOutput = false;  // also mirror to OutputDebugString (DebugView)
};

// Safe to call from DllMain (kernel32 only). 'dir' = folder with trailing '\\'.
// The log is overwritten on every start. If 'dir' is not writable the log is
// created in %TEMP% instead (the chosen path is logged on the first line).
void Init(const char* dir, const Options& opt);
void Shutdown();

bool Enabled();
bool Verbose();

void Write(Level lvl, const char* fmt, ...);
void WriteV(Level lvl, const char* fmt, va_list ap);

}  // namespace xlog

#define LOGD(...) do { if (xlog::Verbose()) xlog::Write(xlog::Level::Debug, __VA_ARGS__); } while (0)
#define LOGI(...) xlog::Write(xlog::Level::Info,  __VA_ARGS__)
#define LOGW(...) xlog::Write(xlog::Level::Warn,  __VA_ARGS__)
#define LOGE(...) xlog::Write(xlog::Level::Error, __VA_ARGS__)

// Logs once per call site, e.g. LOG_ONCE("first call of %s", name);
#define LOG_ONCE(...)                                                  \
    do {                                                               \
        static volatile long s_once_ = 0;                              \
        if (InterlockedExchange(&s_once_, 1) == 0) LOGI(__VA_ARGS__);  \
    } while (0)
