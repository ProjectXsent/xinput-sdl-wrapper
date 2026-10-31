// xinput-sdl-wrapper
// A drop-in XInput DLL (xinput1_3 / xinput1_4 / xinput9_1_0) that sources its
// data from SDL3's Gamepad API, so any SDL-supported pad works in games that
// only speak XInput. Optionally hides DirectInput8 game controllers from the
// game (MinHook) and logs to input_sdl.log.

#include "xinput_types.h"
#include "log.h"
#include "dinput_hide.h"

#include <SDL3/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kMaxPads = 4;

struct Slot {
    SDL_Gamepad*   pad = nullptr;
    SDL_JoystickID id = 0;
    DWORD          packet = 0;
    XINPUT_GAMEPAD last = {};
    WORD           lastLow = 0, lastHigh = 0;
    int            reported = -1;  // last connection state written to the log (-1 = none yet)
};

struct Config {
    bool swapAB = false;
    bool rumble = true;
    bool hideDInput = true;

    bool logEnabled = true;
    bool logVerbose = false;
    bool logDebugOutput = false;

    bool sdlXInput = false;     // let SDL load a real xinput DLL (see ini)
    int  hintRawInput = -1;     // -1 = leave SDL default, 0/1 = force
    int  hintWGI = -1;
    int  hintHIDAPI = -1;
    int  hintDInput = -1;
    int  hintThread = -1;
};

HMODULE          g_module = nullptr;
INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_lock;
bool             g_sdlOk = false;
Config           g_cfg;
Slot             g_slots[kMaxPads];
LARGE_INTEGER    g_freq = {}, g_lastPump = {};
char             g_dir[MAX_PATH] = {};

void ModuleDir(char* out, size_t n) {
    GetModuleFileNameA(g_module, out, (DWORD)n);
    char* slash = strrchr(out, '\\');
    if (slash) slash[1] = '\0';
}

// ---- config (read in DllMain; only kernel32 calls) --------------------------

int ReadTri(const char* ini, const char* section, const char* key) {
    char buf[16] = {};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), ini);
    if (!buf[0]) return -1;
    return atoi(buf) != 0 ? 1 : 0;
}

void LoadConfig(const char* ini) {
    auto b = [&](const char* sec, const char* key, int def) {
        return GetPrivateProfileIntA(sec, key, def, ini) != 0;
    };
    g_cfg.swapAB     = b("General", "SwapAB", 0);
    g_cfg.rumble     = b("General", "Rumble", 1);
    g_cfg.hideDInput = b("General", "HideDInput", 1);

    g_cfg.logEnabled     = b("Logging", "Enabled", 1);
    g_cfg.logVerbose     = b("Logging", "Verbose", 0);
    g_cfg.logDebugOutput = b("Logging", "DebugOutput", 0);

    g_cfg.sdlXInput    = b("SDL", "XInput", 0);
    g_cfg.hintRawInput = ReadTri(ini, "SDL", "RawInput");
    g_cfg.hintWGI      = ReadTri(ini, "SDL", "WGI");
    g_cfg.hintHIDAPI   = ReadTri(ini, "SDL", "HIDAPI");
    g_cfg.hintDInput   = ReadTri(ini, "SDL", "DirectInput");
    g_cfg.hintThread   = ReadTri(ini, "SDL", "JoystickThread");
}

void LogBanner(const char* ini) {
    char exe[MAX_PATH] = {}, dll[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    GetModuleFileNameA(g_module, dll, sizeof(dll));
    LOGI("==== xinput-sdl-wrapper (SDL3) starting ====");
    LOGI("process: %s (pid %lu, %u-bit)", exe, GetCurrentProcessId(), (unsigned)(sizeof(void*) * 8));
    LOGI("wrapper: %s", dll);
    LOGI("ini: %s (%s)", ini, GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? "found" : "not found, using defaults");
    LOGI("config: SwapAB=%d Rumble=%d HideDInput=%d | Log: verbose=%d debugOutput=%d",
         g_cfg.swapAB, g_cfg.rumble, g_cfg.hideDInput, g_cfg.logVerbose, g_cfg.logDebugOutput);
    LOGI("config: SDL XInput=%d RawInput=%d WGI=%d HIDAPI=%d DirectInput=%d JoystickThread=%d (-1 = SDL default)",
         g_cfg.sdlXInput, g_cfg.hintRawInput, g_cfg.hintWGI, g_cfg.hintHIDAPI, g_cfg.hintDInput, g_cfg.hintThread);
}

// Runs from DllMain: no SDL here (loader lock), but logging and the DirectInput
// hooks must be up before the game creates its IDirectInput8 object.
void Bootstrap() {
    ModuleDir(g_dir, sizeof(g_dir));
    char ini[MAX_PATH + 32];
    snprintf(ini, sizeof(ini), "%sxinput_sdl.ini", g_dir);
    LoadConfig(ini);

    xlog::Options lo;
    lo.enabled = g_cfg.logEnabled;
    lo.verbose = g_cfg.logVerbose;
    lo.debugOutput = g_cfg.logDebugOutput;
    xlog::Init(g_dir, lo);

    dinput_hide::Init();
    LogBanner(ini);

    // Works when the game links dinput8.dll statically (already mapped now). If
    // it is not loaded yet, InitOnceFn retries (with LoadLibrary) on the first
    // XInput call.
    if (g_cfg.hideDInput) {
        if (!dinput_hide::Install(false))
            LOGI("DirectInput: dinput8.dll not loaded yet; will retry on first XInput call");
    } else {
        LOGI("DirectInput hiding disabled (HideDInput=0)");
    }
}

// ---- SDL logging bridge -----------------------------------------------------

void SDLCALL SdlLogOut(void* /*userdata*/, int category, SDL_LogPriority prio, const char* msg) {
    switch (prio) {
        case SDL_LOG_PRIORITY_ERROR:
        case SDL_LOG_PRIORITY_CRITICAL: LOGE("[SDL cat %d] %s", category, msg); break;
        case SDL_LOG_PRIORITY_WARN:     LOGW("[SDL cat %d] %s", category, msg); break;
        case SDL_LOG_PRIORITY_INFO:     LOGI("[SDL cat %d] %s", category, msg); break;
        default:                        LOGD("[SDL cat %d] %s", category, msg); break;
    }
}

void SetTriHint(const char* name, int v) {
    if (v < 0) return;
    SDL_SetHint(name, v ? "1" : "0");
    LOGI("SDL hint %s=%d", name, v);
}

// ---- slot management (call with g_lock held) ------------------------------

int FindSlotById(SDL_JoystickID id) {
    for (int i = 0; i < kMaxPads; ++i)
        if (g_slots[i].pad && g_slots[i].id == id) return i;
    return -1;
}

void OpenDevice(SDL_JoystickID id) {
    if (FindSlotById(id) >= 0) return;  // already tracked
    if (!SDL_IsGamepad(id)) return;

    dinput_hide::Bypass bypass;  // SDL's own DirectInput backend must see real devices
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        LOGE("SDL_OpenGamepad(%u) failed: %s", (unsigned)id, SDL_GetError());
        return;
    }
    for (int i = 0; i < kMaxPads; ++i) {
        if (!g_slots[i].pad) {
            g_slots[i] = Slot{};
            g_slots[i].pad = pad;
            g_slots[i].id = id;
            const char* path = SDL_GetGamepadPath(pad);
            LOGI("gamepad connected -> slot %d: '%s' vid=%04X pid=%04X type=%s id=%u path=%s",
                 i, SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?",
                 (unsigned)SDL_GetGamepadVendor(pad), (unsigned)SDL_GetGamepadProduct(pad),
                 SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)), (unsigned)id,
                 path ? path : "(none)");
            if (xlog::Verbose()) {
                char* mapping = SDL_GetGamepadMapping(pad);
                LOGD("mapping: %s", mapping ? mapping : "(none)");
                SDL_free(mapping);
            }
            return;
        }
    }
    LOGW("no free slot for '%s'", SDL_GetGamepadName(pad));
    SDL_CloseGamepad(pad);
}

void CloseInstance(SDL_JoystickID id) {
    for (int i = 0; i < kMaxPads; ++i) {
        if (g_slots[i].pad && g_slots[i].id == id) {
            LOGI("gamepad disconnected: slot %d '%s'", i, SDL_GetGamepadName(g_slots[i].pad));
            dinput_hide::Bypass bypass;
            SDL_CloseGamepad(g_slots[i].pad);
            g_slots[i] = Slot{};
        }
    }
}

void OpenAllConnected() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    LOGI("%d gamepad(s) present at startup", n);
    if (ids) {
        for (int i = 0; i < n; ++i) OpenDevice(ids[i]);
        SDL_free(ids);
    }
}

// Pump SDL (hot-plug + state refresh), at most every ~2 ms.
void Pump(bool force = false) {
    if (!g_sdlOk) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!force && g_lastPump.QuadPart &&
        (now.QuadPart - g_lastPump.QuadPart) * 1000 < g_freq.QuadPart * 2)
        return;
    g_lastPump = now;

    dinput_hide::Bypass bypass;  // SDL's DirectInput hot-plug detection runs in here
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_GAMEPAD_ADDED:
                OpenDevice(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                CloseInstance(e.gdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_ADDED:
                if (!SDL_IsGamepad(e.jdevice.which)) {
                    const char* name = SDL_GetJoystickNameForID(e.jdevice.which);
                    LOGW("joystick '%s' (vid=%04X pid=%04X) has no gamepad mapping, ignored",
                         name ? name : "?",
                         (unsigned)SDL_GetJoystickVendorForID(e.jdevice.which),
                         (unsigned)SDL_GetJoystickProductForID(e.jdevice.which));
                }
                break;
            default:
                break;
        }
    }
    SDL_UpdateGamepads();
}

// ---- one-time init --------------------------------------------------------

BOOL CALLBACK InitOnceFn(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);

    // Fallback for games that load dinput8.dll dynamically after us.
    if (g_cfg.hideDInput) dinput_hide::Install(true);

    dinput_hide::Bypass bypass;  // SDL_Init enumerates DirectInput devices itself

    char db[MAX_PATH + 32];
    snprintf(db, sizeof(db), "%sgamecontrollerdb.txt", g_dir);

    SDL_SetLogOutputFunction(SdlLogOut, nullptr);
    SDL_SetLogPriorities(g_cfg.logVerbose ? SDL_LOG_PRIORITY_VERBOSE : SDL_LOG_PRIORITY_WARN);

    // CRITICAL (default): stop SDL from loading xinput1_x.dll itself. It would
    // find *this* DLL in the game folder and recurse into us.
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, g_cfg.sdlXInput ? "1" : "0");
    // We don't own a window; keep delivering input when the game is unfocused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SetTriHint(SDL_HINT_JOYSTICK_RAWINPUT, g_cfg.hintRawInput);
    SetTriHint(SDL_HINT_JOYSTICK_WGI, g_cfg.hintWGI);
    SetTriHint(SDL_HINT_JOYSTICK_HIDAPI, g_cfg.hintHIDAPI);
    SetTriHint(SDL_HINT_JOYSTICK_DIRECTINPUT, g_cfg.hintDInput);
    SetTriHint(SDL_HINT_JOYSTICK_THREAD, g_cfg.hintThread);

    int ver = SDL_GetVersion();
    LOGI("SDL %d.%d.%d (%s)", SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver),
         SDL_VERSIONNUM_MICRO(ver), SDL_GetRevision());

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        g_sdlOk = false;
        return TRUE;
    }
    g_sdlOk = true;
    LOGI("SDL initialised");

    // Optional user-supplied mappings (https://github.com/mdqinc/SDL_GameControllerDB)
    if (GetFileAttributesA(db) != INVALID_FILE_ATTRIBUTES) {
        int n = SDL_AddGamepadMappingsFromFile(db);
        if (n < 0) LOGE("gamecontrollerdb.txt: load failed: %s", SDL_GetError());
        else       LOGI("gamecontrollerdb.txt: %d mapping(s) added", n);
    }

    EnterCriticalSection(&g_lock);
    OpenAllConnected();
    Pump(true);  // also picks up pads via ADDED events (duplicates are ignored)
    LeaveCriticalSection(&g_lock);
    return TRUE;
}

inline void EnsureInit() { InitOnceExecuteOnce(&g_once, InitOnceFn, nullptr, nullptr); }

struct Lock {
    Lock()  { EnsureInit(); EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

// ---- translation ----------------------------------------------------------

inline SHORT InvertAxis(Sint16 v) { return (SHORT)(v == -32768 ? 32767 : -v); }
inline BYTE  TriggerToByte(Sint16 v) { return (BYTE)(v <= 0 ? 0 : v >> 7); }

void Translate(SDL_Gamepad* c, bool withGuide, XINPUT_GAMEPAD& g) {
    struct Map { SDL_GamepadButton sdl; WORD xi; };
    static const Map map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP,       XINPUT_GAMEPAD_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN,     XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,     XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    XINPUT_GAMEPAD_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START,         XINPUT_GAMEPAD_START},
        {SDL_GAMEPAD_BUTTON_BACK,          XINPUT_GAMEPAD_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK,    XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK,   XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,XINPUT_GAMEPAD_RIGHT_SHOULDER},
    };
    WORD b = 0;
    for (const Map& m : map)
        if (SDL_GetGamepadButton(c, m.sdl)) b |= m.xi;

    // SDL3 names face buttons by position: SOUTH=A, EAST=B, WEST=X, NORTH=Y.
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
    g.bLeftTrigger  = TriggerToByte(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    g.bRightTrigger = TriggerToByte(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
}

DWORD GetStateImpl(DWORD idx, XINPUT_STATE* st, bool withGuide) {
    if (!st || idx >= kMaxPads) return ERROR_BAD_ARGUMENTS;
    Lock lock;
    Pump();
    Slot& s = g_slots[idx];

    int connected = s.pad ? 1 : 0;
    if (s.reported != connected) {  // log transitions only, not every poll
        LOGI("XInputGetState(%lu): %s", idx, connected ? "pad connected" : "no pad (ERROR_DEVICE_NOT_CONNECTED)");
        s.reported = connected;
    }
    if (!s.pad) return ERROR_DEVICE_NOT_CONNECTED;

    XINPUT_GAMEPAD cur = {};
    Translate(s.pad, withGuide, cur);
    if (std::memcmp(&cur, &s.last, sizeof(cur)) != 0) {
        s.last = cur;
        ++s.packet;
    }
    st->dwPacketNumber = s.packet;
    st->Gamepad = cur;
    return ERROR_SUCCESS;
}

}  // namespace

// ---- exports --------------------------------------------------------------
// Names/ordinals are assigned in src/def/*.def (no __declspec(dllexport)).

extern "C" {

DWORD WINAPI XInputGetState(DWORD idx, XINPUT_STATE* st) {
    LOG_ONCE("first call: XInputGetState");
    return GetStateImpl(idx, st, false);
}

DWORD WINAPI XInputGetStateEx(DWORD idx, XINPUT_STATE* st) {
    LOG_ONCE("first call: XInputGetStateEx (ordinal 100)");
    return GetStateImpl(idx, st, true);
}

DWORD WINAPI XInputSetState(DWORD idx, XINPUT_VIBRATION* v) {
    LOG_ONCE("first call: XInputSetState");
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
    bool on = v->wLeftMotorSpeed || v->wRightMotorSpeed;
    bool ok = SDL_RumbleGamepad(s.pad, v->wLeftMotorSpeed, v->wRightMotorSpeed, on ? 0xFFFF : 0);
    LOGD("rumble slot %lu low=%u high=%u -> %s%s%s", idx, (unsigned)v->wLeftMotorSpeed,
         (unsigned)v->wRightMotorSpeed, ok ? "ok" : "failed", ok ? "" : ": ", ok ? "" : SDL_GetError());
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetCapabilities(DWORD idx, DWORD /*flags*/, XINPUT_CAPABILITIES* caps) {
    LOG_ONCE("first call: XInputGetCapabilities");
    if (idx >= kMaxPads || !caps) return ERROR_BAD_ARGUMENTS;
    Lock lock;
    Pump();
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

void WINAPI XInputEnable(BOOL enable) {
    LOG_ONCE("first call: XInputEnable(%d)", (int)enable);
}

DWORD WINAPI XInputGetBatteryInformation(DWORD idx, BYTE /*devType*/, XINPUT_BATTERY_INFORMATION* b) {
    LOG_ONCE("first call: XInputGetBatteryInformation");
    if (idx >= kMaxPads || !b) return ERROR_BAD_ARGUMENTS;
    Lock lock;
    if (!g_slots[idx].pad) return ERROR_DEVICE_NOT_CONNECTED;
    b->BatteryType = BATTERY_TYPE_WIRED;
    b->BatteryLevel = BATTERY_LEVEL_FULL;
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetKeystroke(DWORD idx, DWORD /*reserved*/, XINPUT_KEYSTROKE* /*k*/) {
    LOG_ONCE("first call: XInputGetKeystroke (not implemented, returns ERROR_EMPTY)");
    if (idx >= kMaxPads && idx != 0xFF) return ERROR_BAD_ARGUMENTS;
    return ERROR_EMPTY;  // not implemented; "no pending keystrokes"
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD, GUID*, GUID*) {
    LOG_ONCE("first call: XInputGetDSoundAudioDeviceGuids");
    return ERROR_DEVICE_NOT_CONNECTED;
}

// Undocumented ordinals 101-103
DWORD WINAPI XInputWaitForGuideButton(DWORD, DWORD, void*) {
    LOG_ONCE("first call: XInputWaitForGuideButton (not implemented)");
    return ERROR_CALL_NOT_IMPLEMENTED;
}
DWORD WINAPI XInputCancelGuideButtonWait(DWORD) { return ERROR_SUCCESS; }
DWORD WINAPI XInputPowerOffController(DWORD)    { return ERROR_SUCCESS; }

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
        // No SDL work here (loader lock): SDL is initialised lazily. Only the
        // logger and the DirectInput hooks start now, so the hooks are in place
        // before the game creates its IDirectInput8 object.
        Bootstrap();
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // FreeLibrary (not process exit): detours must not outlive our code.
        dinput_hide::Shutdown();
        xlog::Shutdown();
    }
    return TRUE;
}
