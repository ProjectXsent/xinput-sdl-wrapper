// xinput-sdl-wrapper
// A drop-in XInput DLL (xinput1_3 / xinput1_4 / xinput9_1_0) that sources its
// data from SDL3's gamepad API, so any SDL-supported pad works in games
// that only speak XInput.

#include "xinput_types.h"
#include "dinput_hide.h"
#include <SDL3/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kMaxPads = 4;

struct Slot {
    SDL_Gamepad*   pad = nullptr;
    SDL_JoystickID id = 0;          // 0 is never a valid SDL3 instance ID
    DWORD               packet = 0;
    XINPUT_GAMEPAD      last = {};      // latest state, always translated WITH the guide bit
    WORD                lastLow = 0, lastHigh = 0;
};

struct Config {
    bool swapAB = false;
    bool rumble = true;
    bool debug = false;
    int  sdlXInput = -1;   // -1 auto, 0 off, 1 on (SDL's own use of the real XInput)
};

HMODULE          g_module = nullptr;
INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_lock;
bool             g_sdlOk = false;
Config           g_cfg;
Slot             g_slots[kMaxPads];
LARGE_INTEGER    g_freq = {};
HANDLE           g_ready = nullptr;      // signalled when the worker finished init
char             g_logPath[MAX_PATH + 32] = {};
bool             g_loggedFirstCall = false;

}  // namespace

// Shared by dinput_hide.cpp. Appends to xinput_sdl.log and the debugger channel.
void LogWrite(const char* line) {
    OutputDebugStringA(line);
    if (!g_logPath[0]) return;
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(h, line, (DWORD)strlen(line), &n, nullptr);
    CloseHandle(h);
}

namespace {

void Log(const char* fmt, ...) {
    if (!g_cfg.debug) return;
    char msg[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[560];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [xinput-sdl] %s\n",
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    LogWrite(line);
}

void ModuleDir(char* out, size_t n) {
    GetModuleFileNameA(g_module, out, (DWORD)n);
    char* slash = strrchr(out, '\\');
    if (slash) slash[1] = '\0';
}

// ---- slot management (call with g_lock held) ------------------------------

int FindSlotByPad(SDL_Gamepad* p) {
    for (int i = 0; i < kMaxPads; ++i)
        if (g_slots[i].pad == p) return i;
    return -1;
}

// In SDL3 the ADDED event carries the joystick instance ID directly.
void OpenDevice(SDL_JoystickID instanceId) {
    SDL_Gamepad* pad = SDL_OpenGamepad(instanceId);
    if (!pad) { Log("SDL_OpenGamepad(%u) failed: %s", (unsigned)instanceId, SDL_GetError()); return; }
    if (FindSlotByPad(pad) >= 0) return;  // already tracked (open is ref-counted)
    for (int i = 0; i < kMaxPads; ++i) {
        if (!g_slots[i].pad) {
            g_slots[i] = Slot{};
            g_slots[i].pad = pad;
            g_slots[i].id = instanceId;
            Log("'%s' -> slot %d", SDL_GetGamepadName(pad), i);
            return;
        }
    }
    Log("No free slot for '%s'", SDL_GetGamepadName(pad));
    SDL_CloseGamepad(pad);
}

void CloseInstance(SDL_JoystickID id) {
    for (int i = 0; i < kMaxPads; ++i) {
        if (g_slots[i].pad && g_slots[i].id == id) {
            Log("slot %d disconnected", i);
            SDL_CloseGamepad(g_slots[i].pad);
            g_slots[i] = Slot{};
        }
    }
}

// Log calls that took suspiciously long (capped so the log stays readable).
void LogIfSlow(const char* what, LARGE_INTEGER start) {
    static LONG count = 0;
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
    if (ms >= 8.0 && InterlockedIncrement(&count) <= 100)
        Log("SLOW: %s took %.1f ms (thread %lu)", what, ms, GetCurrentThreadId());
}

void Translate(SDL_Gamepad* c, bool withGuide, XINPUT_GAMEPAD& g);

// Pump SDL (hot-plug + state refresh). Called only from the worker thread,
// with g_lock held.
void Pump() {
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_JOYSTICK_ADDED:
            Log("JOYSTICK_ADDED id=%u '%s' isGamepad=%d", (unsigned)e.jdevice.which,
                SDL_GetJoystickNameForID(e.jdevice.which) ? SDL_GetJoystickNameForID(e.jdevice.which) : "?",
                (int)SDL_IsGamepad(e.jdevice.which));
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            Log("JOYSTICK_REMOVED id=%u", (unsigned)e.jdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            Log("GAMEPAD_ADDED id=%u", (unsigned)e.gdevice.which);
            OpenDevice(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            Log("GAMEPAD_REMOVED id=%u", (unsigned)e.gdevice.which);
            CloseInstance(e.gdevice.which);
            break;
        default: break;
        }
    }
    SDL_UpdateGamepads();

    // Refresh the cached XInput state of every slot.
    for (int i = 0; i < kMaxPads; ++i) {
        Slot& s = g_slots[i];
        if (!s.pad) continue;
        XINPUT_GAMEPAD cur = {};
        Translate(s.pad, true, cur);
        if (std::memcmp(&cur, &s.last, sizeof(cur)) != 0) {
            s.last = cur;
            ++s.packet;
        }
    }
}

bool ModuleIsXInput14() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_module, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    return _stricmp(base, "xinput1_4.dll") == 0;
}

void DumpDevices() {
    int n = 0;
    SDL_JoystickID* js = SDL_GetJoysticks(&n);
    Log("SDL sees %d joystick(s)", n);
    for (int i = 0; i < n; ++i) {
        const char* nm = SDL_GetJoystickNameForID(js[i]);
        Log("  joystick id=%u '%s' isGamepad=%d", (unsigned)js[i], nm ? nm : "?", (int)SDL_IsGamepad(js[i]));
    }
    SDL_free(js);
    SDL_JoystickID* gs = SDL_GetGamepads(&n);
    Log("SDL sees %d gamepad(s)", n);
    for (int i = 0; i < n; ++i) OpenDevice(gs[i]);   // no-op if already tracked
    SDL_free(gs);
}

// ---- SDL worker thread ----------------------------------------------------
// All SDL work (init, event pumping, device polling) runs on this private thread,
// so SDL never touches the game's message queue and the game's thread is never
// blocked by SDL / HID driver calls. XInputGetState just reads a cached snapshot.
// NOTE: this thread must NOT call EnsureInit()/Lock (the creator is waiting on us).

void SetOptionalHint(const char* iniKey, const char* sdlHint, const char* ini) {
    int v = (int)GetPrivateProfileIntA("General", iniKey, -1, ini);
    if (v >= 0) {
        SDL_SetHint(sdlHint, v ? "1" : "0");
        Log("hint %s = %d (ini %s)", sdlHint, v, iniKey);
    }
}

char g_ini[MAX_PATH + 32], g_db[MAX_PATH + 32];

DWORD WINAPI WorkerMain(LPVOID) {
    // SDL's XInput backend is how Xbox pads are normally reached on Windows, so
    // keep it on. It must only be off when *we* are xinput1_4.dll, because SDL
    // tries to load XInput1_4.dll first and would find this wrapper (recursion).
    bool sdlXInput = (g_cfg.sdlXInput >= 0) ? (g_cfg.sdlXInput != 0) : !ModuleIsXInput14();
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, sdlXInput ? "1" : "0");
    Log("SDL_HINT_XINPUT_ENABLED = %d (wrapper is %s)", (int)sdlXInput,
        ModuleIsXInput14() ? "xinput1_4" : "not xinput1_4");
    // We don't own a window; keep delivering input when the game is unfocused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Optional experiments (ini: RawInput / WGI / HIDAPI = 0 or 1; absent = SDL default)
    SetOptionalHint("RawInput", SDL_HINT_JOYSTICK_RAWINPUT, g_ini);
    SetOptionalHint("WGI",      SDL_HINT_JOYSTICK_WGI,      g_ini);
    SetOptionalHint("HIDAPI",   SDL_HINT_JOYSTICK_HIDAPI,   g_ini);

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {  // SDL3: returns true on success
        Log("SDL_Init failed: %s", SDL_GetError());
        g_sdlOk = false;
        SetEvent(g_ready);
        return 0;
    }
    g_sdlOk = true;
    {
        int v = SDL_GetVersion();
        Log("SDL_Init ok, SDL %d.%d.%d (worker thread %lu)", SDL_VERSIONNUM_MAJOR(v),
            SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), GetCurrentThreadId());
    }

    // Hide gamepad-class devices from the game's DirectInput enumeration so it
    // only sees the pad through XInput. SDL's own DirectInput use is exempted by
    // caller module inside the hooks. Done before signalling ready, so the hooks
    // are in place before the game's first XInput call returns.
    bool hooked = InstallDInputHooks(g_cfg.debug);
    Log("DirectInput hooks %s", hooked ? "installed" : "NOT installed");

    // Optional user-supplied mappings (https://github.com/mdqinc/SDL_GameControllerDB)
    if (GetFileAttributesA(g_db) != INVALID_FILE_ATTRIBUTES) {
        int added = SDL_AddGamepadMappingsFromFile(g_db);
        Log("gamecontrollerdb.txt: %d mapping(s) added", added);
    }

    EnterCriticalSection(&g_lock);
    Pump();          // picks up already-connected pads via ADDED events
    DumpDevices();   // log what SDL sees; also opens any gamepad the events missed
    Pump();          // fill the state cache for anything just opened
    LeaveCriticalSection(&g_lock);
    SetEvent(g_ready);

    // ~500 Hz polling. Prefer a high-resolution timer (Win10 1803+); otherwise Sleep(1).
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                       0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    for (;;) {
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -20000;  // 2 ms, relative, 100 ns units
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 50);
        } else {
            Sleep(1);
        }
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        EnterCriticalSection(&g_lock);
        Pump();
        LeaveCriticalSection(&g_lock);
        LogIfSlow("Pump", t0);
    }
    return 0;
}

// ---- one-time init --------------------------------------------------------

BOOL CALLBACK InitOnceFn(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);

    char dir[MAX_PATH];
    ModuleDir(dir, sizeof(dir));
    snprintf(g_ini, sizeof(g_ini), "%sxinput_sdl.ini", dir);
    snprintf(g_db, sizeof(g_db), "%sgamecontrollerdb.txt", dir);

    g_cfg.swapAB = GetPrivateProfileIntA("General", "SwapAB", 0, g_ini) != 0;
    g_cfg.rumble = GetPrivateProfileIntA("General", "Rumble", 1, g_ini) != 0;
    g_cfg.debug  = GetPrivateProfileIntA("General", "Debug", 0, g_ini) != 0;
    // SDLXInput: -1 = auto (default), 0 = never, 1 = always
    g_cfg.sdlXInput = (int)GetPrivateProfileIntA("General", "SDLXInput", -1, g_ini);

    if (g_cfg.debug) {
        snprintf(g_logPath, sizeof(g_logPath), "%sxinput_sdl.log", dir);
        // start a fresh log each run
        HANDLE h = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("init: host exe = %s", exe);
        Log("init: wrapper dir = %s (ini %s)", dir,
            GetFileAttributesA(g_ini) != INVALID_FILE_ATTRIBUTES ? "found" : "NOT found");
        Log("init: calling thread %lu", GetCurrentThreadId());
    }

    g_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    if (!th || !g_ready) {
        Log("failed to start SDL worker thread (error %lu)", GetLastError());
        g_sdlOk = false;
        return TRUE;
    }
    CloseHandle(th);
    if (WaitForSingleObject(g_ready, 10000) == WAIT_TIMEOUT)
        Log("WARNING: SDL init still running after 10 s; continuing without waiting");
    return TRUE;
}

inline void EnsureInit() { InitOnceExecuteOnce(&g_once, InitOnceFn, nullptr, nullptr); }

struct Lock {
    Lock()  { EnsureInit(); EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

// ---- translation ----------------------------------------------------------

inline SHORT InvertAxis(Sint16 v) { return (SHORT)(v == -32768 ? 32767 : -v); }

void Translate(SDL_Gamepad* c, bool withGuide, XINPUT_GAMEPAD& g) {
    struct Map { SDL_GamepadButton sdl; WORD xi; };
    static const Map map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP,       XINPUT_GAMEPAD_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN,     XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,     XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    XINPUT_GAMEPAD_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START,         XINPUT_GAMEPAD_START},
        {SDL_GAMEPAD_BUTTON_BACK,          XINPUT_GAMEPAD_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK,     XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK,    XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
    };
    WORD b = 0;
    for (const Map& m : map)
        if (SDL_GetGamepadButton(c, m.sdl)) b |= m.xi;

    // SDL3 names buttons by position: SOUTH=A, EAST=B, WEST=X, NORTH=Y (Xbox labels).
    SDL_GamepadButton bA = SDL_GAMEPAD_BUTTON_SOUTH, bB = SDL_GAMEPAD_BUTTON_EAST;
    SDL_GamepadButton bX = SDL_GAMEPAD_BUTTON_WEST,  bY = SDL_GAMEPAD_BUTTON_NORTH;
    if (g_cfg.swapAB) { bA = SDL_GAMEPAD_BUTTON_EAST;  bB = SDL_GAMEPAD_BUTTON_SOUTH;
                        bX = SDL_GAMEPAD_BUTTON_NORTH; bY = SDL_GAMEPAD_BUTTON_WEST; }
    if (SDL_GetGamepadButton(c, bA)) b |= XINPUT_GAMEPAD_A;
    if (SDL_GetGamepadButton(c, bB)) b |= XINPUT_GAMEPAD_B;
    if (SDL_GetGamepadButton(c, bX)) b |= XINPUT_GAMEPAD_X;
    if (SDL_GetGamepadButton(c, bY)) b |= XINPUT_GAMEPAD_Y;
    if (withGuide && SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_GUIDE))
        b |= XINPUT_GAMEPAD_GUIDE;

    g.wButtons      = b;
    g.sThumbLX      = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTX);
    g.sThumbLY      = InvertAxis(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTY));
    g.sThumbRX      = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTX);
    g.sThumbRY      = InvertAxis(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTY));
    g.bLeftTrigger  = (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  >> 7);
    g.bRightTrigger = (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
}

DWORD GetStateImpl(DWORD idx, XINPUT_STATE* st, bool withGuide) {
    if (!st || idx >= kMaxPads) return ERROR_BAD_ARGUMENTS;
    EnsureInit();
    if (!g_loggedFirstCall) { g_loggedFirstCall = true; Log("first XInputGetState call by the game (thread %lu)", GetCurrentThreadId()); }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    Lock lock;
    LogIfSlow("waiting for state lock", t0);
    const Slot& s = g_slots[idx];
    if (!s.pad) return ERROR_DEVICE_NOT_CONNECTED;

    st->dwPacketNumber = s.packet;
    st->Gamepad = s.last;
    if (!withGuide) st->Gamepad.wButtons &= (WORD)~XINPUT_GAMEPAD_GUIDE;
    return ERROR_SUCCESS;
}

}  // namespace

// ---- exports --------------------------------------------------------------
// Names/ordinals are assigned in src/def/*.def (no __declspec(dllexport)).

extern "C" {

DWORD WINAPI XInputGetState(DWORD idx, XINPUT_STATE* st)   { return GetStateImpl(idx, st, false); }
DWORD WINAPI XInputGetStateEx(DWORD idx, XINPUT_STATE* st) { return GetStateImpl(idx, st, true); }

DWORD WINAPI XInputSetState(DWORD idx, XINPUT_VIBRATION* v) {
    if (idx >= kMaxPads || !v) return ERROR_BAD_ARGUMENTS;
    Lock lock;
    Slot& s = g_slots[idx];
    if (!s.pad) return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_cfg.rumble) return ERROR_SUCCESS;
    if (v->wLeftMotorSpeed == s.lastLow && v->wRightMotorSpeed == s.lastHigh)
        return ERROR_SUCCESS;  // avoid re-sending identical commands every frame
    s.lastLow = v->wLeftMotorSpeed;
    s.lastHigh = v->wRightMotorSpeed;
    // XInput: left = low-frequency (big) motor, right = high-frequency (small) motor.
    SDL_RumbleGamepad(s.pad, v->wLeftMotorSpeed, v->wRightMotorSpeed,
                             (v->wLeftMotorSpeed || v->wRightMotorSpeed) ? 0xFFFF : 0);
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetCapabilities(DWORD idx, DWORD /*flags*/, XINPUT_CAPABILITIES* caps) {
    if (idx >= kMaxPads || !caps) return ERROR_BAD_ARGUMENTS;
    Lock lock;
    if (!g_slots[idx].pad) return ERROR_DEVICE_NOT_CONNECTED;
    std::memset(caps, 0, sizeof(*caps));
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Flags = 0;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY =
    caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
    caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFF;
    return ERROR_SUCCESS;
}

void WINAPI XInputEnable(BOOL /*enable*/) {}

DWORD WINAPI XInputGetBatteryInformation(DWORD idx, BYTE /*devType*/, XINPUT_BATTERY_INFORMATION* b) {
    if (idx >= kMaxPads || !b) return ERROR_BAD_ARGUMENTS;
    Lock lock;
    if (!g_slots[idx].pad) return ERROR_DEVICE_NOT_CONNECTED;
    b->BatteryType = BATTERY_TYPE_WIRED;
    b->BatteryLevel = BATTERY_LEVEL_FULL;
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetKeystroke(DWORD idx, DWORD /*reserved*/, XINPUT_KEYSTROKE* /*k*/) {
    if (idx >= kMaxPads && idx != 0xFF) return ERROR_BAD_ARGUMENTS;
    return ERROR_EMPTY;  // not implemented; "no pending keystrokes"
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD, GUID*, GUID*) { return ERROR_DEVICE_NOT_CONNECTED; }

// Undocumented ordinals 101-103
DWORD WINAPI XInputWaitForGuideButton(DWORD, DWORD, void*) { return ERROR_CALL_NOT_IMPLEMENTED; }
DWORD WINAPI XInputCancelGuideButtonWait(DWORD)           { return ERROR_SUCCESS; }
DWORD WINAPI XInputPowerOffController(DWORD)              { return ERROR_SUCCESS; }

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    // Intentionally no SDL work here (loader lock). SDL is initialised lazily.
    return TRUE;
}