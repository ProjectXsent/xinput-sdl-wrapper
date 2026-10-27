// xinput-sdl-wrapper
// A drop-in XInput DLL (xinput1_3 / xinput1_4 / xinput9_1_0) that sources its
// data from SDL3's gamepad API, so any SDL-supported pad works in games
// that only speak XInput.

#include "xinput_types.h"
#include "dinput_hide.h"
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr int kMaxPads = 4;

struct Slot {
    SDL_Gamepad*   pad = nullptr;
    SDL_JoystickID id = 0;          // 0 is never a valid SDL3 instance ID
    DWORD               packet = 0;
    XINPUT_GAMEPAD      last = {};
    WORD                lastLow = 0, lastHigh = 0;
};

struct Config {
    bool swapAB = false;
    bool rumble = true;
    bool debug = false;
};

HMODULE          g_module = nullptr;
INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_lock;
bool             g_sdlOk = false;
Config           g_cfg;
Slot             g_slots[kMaxPads];
LARGE_INTEGER    g_freq = {}, g_lastPump = {};

void Log(const char* fmt, ...) {
    if (!g_cfg.debug) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
    OutputDebugStringA("[xinput-sdl] ");
    OutputDebugStringA(buf);
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
    if (!pad) return;
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

// Pump SDL (hot-plug + state refresh), at most every ~2 ms.
void Pump(bool force = false) {
    if (!g_sdlOk) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!force && g_lastPump.QuadPart &&
        (now.QuadPart - g_lastPump.QuadPart) * 1000 < g_freq.QuadPart * 2)
        return;
    g_lastPump = now;

    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED)        OpenDevice(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) CloseInstance(e.gdevice.which);
    }
    SDL_UpdateGamepads();
}

// ---- one-time init --------------------------------------------------------

BOOL CALLBACK InitOnceFn(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);

    char dir[MAX_PATH], ini[MAX_PATH + 32], db[MAX_PATH + 32];
    ModuleDir(dir, sizeof(dir));
    snprintf(ini, sizeof(ini), "%sxinput_sdl.ini", dir);
    snprintf(db, sizeof(db), "%sgamecontrollerdb.txt", dir);

    g_cfg.swapAB = GetPrivateProfileIntA("General", "SwapAB", 0, ini) != 0;
    g_cfg.rumble = GetPrivateProfileIntA("General", "Rumble", 1, ini) != 0;
    g_cfg.debug  = GetPrivateProfileIntA("General", "Debug", 0, ini) != 0;

    // CRITICAL: stop SDL from loading xinput1_x.dll itself. It would find *this*
    // DLL in the game folder and recurse into us.
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    // We don't own a window; keep delivering input when the game is unfocused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {  // SDL3: returns true on success
        Log("SDL_Init failed: %s", SDL_GetError());
        g_sdlOk = false;
        return TRUE;
    }
    g_sdlOk = true;

    // Hide gamepad-class devices from the game's DirectInput enumeration so it
    // only sees the pad through XInput. Safe here (not under loader lock); SDL's
    // own DirectInput use is exempted by caller module inside the hooks.
    InstallDInputHooks(g_cfg.debug);

    // Optional user-supplied mappings (https://github.com/mdqinc/SDL_GameControllerDB)
    if (GetFileAttributesA(db) != INVALID_FILE_ATTRIBUTES)
        SDL_AddGamepadMappingsFromFile(db);

    EnterCriticalSection(&g_lock);
    Pump(true);  // picks up already-connected pads via DEVICEADDED events
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
    Lock lock;
    Pump();
    Slot& s = g_slots[idx];
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