// xinput-sdl-wrapper
// A drop-in XInput DLL (xinput1_3 / xinput1_4 / xinput9_1_0) that sources its
// data from SDL3's Gamepad API, so any SDL-supported pad works in games that
// only speak XInput. Optionally hides DirectInput8 game controllers from the
// game (MinHook) and logs to xinput_sdl.log.
//
// Threading model
// ---------------
// SDL is initialised, pumped and shut down on ONE background worker thread. The
// game's threads never call into SDL: XInputGetState copies a cached, already
// translated XINPUT_GAMEPAD, and XInputSetState only records the requested motor
// speeds (the worker applies them). Previously SDL_Init ran inside the game's first
// XInput call (window/device creation = lost focus + busy cursor) and SDL_PumpEvents
// ran on the game's thread every poll (multi-ms stalls with some drivers).
//
// The worker thread holds a dinput_hide::Bypass for its whole lifetime, so SDL's own
// DirectInput enumeration is never filtered by our hooks.

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

// SDL rumble effects have a finite duration; XInput's do not. Request a short effect
// and refresh it while the game keeps it non-zero. A short duration also bounds how
// long a pad keeps vibrating if the game exits or crashes mid-rumble.
constexpr Uint32    kRumbleDurationMs = 2000;
constexpr ULONGLONG kRumbleRefreshMs  = 1000;
constexpr Uint32    kPollDelayMs      = 2;
constexpr DWORD     kReadyWaitMs      = 1000;  // max the game's first XInput call waits for SDL

struct Slot {
    // Worker-thread only (the game thread never touches SDL objects).
    SDL_Gamepad*   pad = nullptr;
    SDL_JoystickID id = 0;

    // Shared cache, guarded by g_lock.
    bool           connected = false;
    DWORD          packet = 0;
    XINPUT_GAMEPAD cur = {};          // translated state, GUIDE bit included
    WORD           wantLow = 0, wantHigh = 0;  // last rumble request from the game
    bool           rumbleDirty = false;
    ULONGLONG      rumbleTick = 0;

    // Game thread only (guarded by g_lock as well).
    int            reported = -1;  // last connection state written to the log (-1 = none yet)
};

struct Config {
    bool swapAB = false;
    bool rumble = true;
    bool hideDInput = true;
    bool realXInput = true;     // forward pads the real XInput DLL serves (Xbox) to it; SDL fills the rest
    bool ignoreXbox = true;     // never let SDL take Xbox-type pads (real XInput owns them)

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
CRITICAL_SECTION g_lock;                 // guards every shared Slot field
HANDLE           g_readyEvent = nullptr; // signalled once the first device scan is done (or SDL failed)
volatile LONG    g_ready = 0;            // 1 once the worker finished its first scan
volatile LONG    g_getStateCalls = 0;    // diagnostics: XInputGetState/Ex calls made by the game
Config           g_cfg;
Slot             g_slots[kMaxPads];
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
    g_cfg.realXInput = b("General", "RealXInput", 1);
    g_cfg.ignoreXbox = b("General", "IgnoreXboxPads", 1);

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
    LOGI("config: RealXInput=%d IgnoreXboxPads=%d", g_cfg.realXInput, g_cfg.ignoreXbox);
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

// ---- locking ----------------------------------------------------------------

// Plain lock for the worker thread. It must NOT go through EnsureInit(): the game
// thread may be inside InitOnceFn waiting for this very thread.
struct RawLock {
    RawLock()  { EnterCriticalSection(&g_lock); }
    ~RawLock() { LeaveCriticalSection(&g_lock); }
};

// ---- the genuine XInput DLL -------------------------------------------------
// Xbox pads belong to the real driver. When RealXInput=1 we load the system copy of
// *this DLL's own name* (xinput1_3 / xinput1_4 / xinput9_1_0) from the system folder and
// ask it first; SDL answers only for slots where no real pad is connected.

namespace real {

typedef DWORD (WINAPI *PFN_GetState)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI *PFN_SetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD (WINAPI *PFN_GetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void  (WINAPI *PFN_Enable)(BOOL);
typedef DWORD (WINAPI *PFN_Battery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD (WINAPI *PFN_Keystroke)(DWORD, DWORD, XINPUT_KEYSTROKE*);

HMODULE       g_mod = nullptr;
PFN_GetState  p_GetState = nullptr, p_GetStateEx = nullptr;
PFN_SetState  p_SetState = nullptr;
PFN_GetCaps   p_GetCaps = nullptr;
PFN_Enable    p_Enable = nullptr;
PFN_Battery   p_Battery = nullptr;
PFN_Keystroke p_Keystroke = nullptr;
volatile LONG g_seen[kMaxPads];  // 0 = unseen, 1 = not connected, 2 = connected (log transitions only)

bool Active() { return g_mod != nullptr; }

// Called from InitOnceFn (never DllMain: LoadLibrary under loader lock).
void Load() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(g_module, self, MAX_PATH);
    const wchar_t* name = wcsrchr(self, L'\\');
    name = name ? name + 1 : self;
    if (_wcsnicmp(name, L"xinput", 6) != 0) {
        LOGW("RealXInput: this DLL is not named xinput*.dll (%ls); not loading a real one", name);
        return;
    }
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for a 32-bit process
    if (n == 0 || n + 1 + lstrlenW(name) >= MAX_PATH) return;
    lstrcatW(path, L"\\");
    lstrcatW(path, name);
    g_mod = LoadLibraryW(path);
    if (!g_mod) { LOGW("RealXInput: could not load %ls (error %lu)", path, GetLastError()); return; }
    p_GetState   = (PFN_GetState)  GetProcAddress(g_mod, "XInputGetState");
    p_GetStateEx = (PFN_GetState)  GetProcAddress(g_mod, MAKEINTRESOURCEA(100));
    p_SetState   = (PFN_SetState)  GetProcAddress(g_mod, "XInputSetState");
    p_GetCaps    = (PFN_GetCaps)   GetProcAddress(g_mod, "XInputGetCapabilities");
    p_Enable     = (PFN_Enable)    GetProcAddress(g_mod, "XInputEnable");
    p_Battery    = (PFN_Battery)   GetProcAddress(g_mod, "XInputGetBatteryInformation");
    p_Keystroke  = (PFN_Keystroke) GetProcAddress(g_mod, "XInputGetKeystroke");
    if (!p_GetState) { g_mod = nullptr; LOGW("RealXInput: %ls has no XInputGetState; disabled", path); return; }
    LOGI("RealXInput: forwarding Xbox pads to %ls (GetStateEx %s)", path, p_GetStateEx ? "available" : "n/a");
}

// Real pad state for `idx`; ERROR_SUCCESS only when a real pad is connected.
DWORD GetState(DWORD idx, XINPUT_STATE* st, bool ex) {
    if (!g_mod) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD r = (ex && p_GetStateEx) ? p_GetStateEx(idx, st) : p_GetState(idx, st);
    LONG now = (r == ERROR_SUCCESS) ? 2 : 1;
    if (InterlockedExchange(&g_seen[idx], now) != now)
        LOGI("real XInput user %lu: %s", idx, now == 2 ? "pad connected" : "no pad");
    return r;
}

bool Connected(int idx) {
    XINPUT_STATE tmp;
    return g_mod && p_GetState(idx, &tmp) == ERROR_SUCCESS;
}

}  // namespace real

// ---- translation (worker thread only: calls SDL) ----------------------------

inline SHORT InvertAxis(Sint16 v) { return (SHORT)(v == -32768 ? 32767 : -v); }
inline BYTE  TriggerToByte(Sint16 v) { return (BYTE)(v <= 0 ? 0 : v >> 7); }

void Translate(SDL_Gamepad* c, XINPUT_GAMEPAD& g) {
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
        {SDL_GAMEPAD_BUTTON_GUIDE,         XINPUT_GAMEPAD_GUIDE},  // stripped for XInputGetState
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

    g.wButtons      = b;
    g.sThumbLX      = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTX);
    g.sThumbLY      = InvertAxis(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTY));
    g.sThumbRX      = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTX);
    g.sThumbRY      = InvertAxis(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTY));
    g.bLeftTrigger  = TriggerToByte(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    g.bRightTrigger = TriggerToByte(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
}

// ---- slot management (worker thread only) -----------------------------------
// Only the worker changes Slot::pad / Slot::id, so it may read them without the lock.

int FindSlotById(SDL_JoystickID id) {
    for (int i = 0; i < kMaxPads; ++i)
        if (g_slots[i].pad && g_slots[i].id == id) return i;
    return -1;
}

void OpenDevice(SDL_JoystickID id) {
    if (FindSlotById(id) >= 0) return;  // already tracked
    if (!SDL_IsGamepad(id)) return;

    if (g_cfg.ignoreXbox && real::Active()) {
        SDL_GamepadType t = SDL_GetGamepadTypeForID(id);
        if (t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE) {
            LOGI("ignoring Xbox-type pad id=%u (served by the real XInput)", (unsigned)id);
            return;
        }
    }

    // Prefer a free slot that no genuine XInput pad is using; else any free slot.
    int slot = -1;
    for (int i = 0; i < kMaxPads && slot < 0; ++i)
        if (!g_slots[i].pad && !real::Connected(i)) slot = i;
    for (int i = 0; i < kMaxPads && slot < 0; ++i)
        if (!g_slots[i].pad) slot = i;

    SDL_Gamepad* pad = SDL_OpenGamepad(id);  // outside the lock: may take a while
    if (!pad) {
        LOGE("SDL_OpenGamepad(%u) failed: %s", (unsigned)id, SDL_GetError());
        return;
    }
    if (slot < 0) {
        LOGW("no free slot for '%s'", SDL_GetGamepadName(pad));
        SDL_CloseGamepad(pad);
        return;
    }
    {
        RawLock l;
        g_slots[slot] = Slot{};
        g_slots[slot].pad = pad;
        g_slots[slot].id = id;
        g_slots[slot].connected = true;
    }
    const char* path = SDL_GetGamepadPath(pad);
    LOGI("gamepad connected -> slot %d: '%s' vid=%04X pid=%04X type=%s id=%u path=%s",
         slot, SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?",
         (unsigned)SDL_GetGamepadVendor(pad), (unsigned)SDL_GetGamepadProduct(pad),
         SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)), (unsigned)id,
         path ? path : "(none)");
    if (xlog::Verbose()) {
        char* mapping = SDL_GetGamepadMapping(pad);
        LOGD("mapping: %s", mapping ? mapping : "(none)");
        SDL_free(mapping);
    }
}

void CloseInstance(SDL_JoystickID id) {
    int s = FindSlotById(id);
    if (s < 0) return;
    SDL_Gamepad* pad = g_slots[s].pad;
    LOGI("gamepad disconnected: slot %d '%s'", s, SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?");
    {
        RawLock l;               // unpublish first so the game thread can no longer see it
        g_slots[s] = Slot{};
    }
    SDL_CloseGamepad(pad);
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

// One iteration of the worker: pump SDL (hot-plug + state refresh), publish the
// translated state of every slot, and push pending rumble requests.
void Tick() {
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

    const ULONGLONG now = GetTickCount64();

    // Diagnostics: every 5 s, say whether the game is still polling XInput at all. A game
    // that checked once at startup and never again will never see a pad plugged in later.
    static ULONGLONG lastBeat = now;
    static LONG lastCalls = 0;
    if (now - lastBeat >= 5000) {
        lastBeat = now;
        LONG calls = InterlockedCompareExchange(&g_getStateCalls, 0, 0);
        if (calls != lastCalls) { LOGI("heartbeat: game made %ld XInputGetState call(s) in the last 5 s", calls - lastCalls); lastCalls = calls; }
        else                    { LOGI("heartbeat: game made NO XInputGetState calls in the last 5 s (total %ld)", calls); }
    }
    for (int i = 0; i < kMaxPads; ++i) {
        SDL_Gamepad* pad = g_slots[i].pad;  // only this thread changes it
        if (!pad) continue;

        XINPUT_GAMEPAD cur = {};
        Translate(pad, cur);
        const bool connected = SDL_GamepadConnected(pad);

        WORD lo, hi;
        bool push;
        {
            RawLock l;
            Slot& s = g_slots[i];
            if (std::memcmp(&cur, &s.cur, sizeof(cur)) != 0) {
                s.cur = cur;
                ++s.packet;
            }
            s.connected = connected;
            lo = s.wantLow;
            hi = s.wantHigh;
            push = s.rumbleDirty || ((lo || hi) && now - s.rumbleTick > kRumbleRefreshMs);
            if (push) { s.rumbleDirty = false; s.rumbleTick = now; }
        }
        if (push) {
            // XInput: left = low-frequency (big) motor, right = high-frequency (small) motor.
            bool ok = (lo == 0 && hi == 0) ? SDL_RumbleGamepad(pad, 0, 0, 0)
                                           : SDL_RumbleGamepad(pad, lo, hi, kRumbleDurationMs);
            LOGD("rumble slot %d low=%u high=%u -> %s%s%s", i, (unsigned)lo, (unsigned)hi,
                 ok ? "ok" : "failed", ok ? "" : ": ", ok ? "" : SDL_GetError());
        }
    }
}

// ---- worker thread ----------------------------------------------------------

DWORD WINAPI Worker(LPVOID) {
    // SDL's own DirectInput backend (SDL_Init, device enumeration, hot-plug) runs on this
    // thread, so it must see the real devices. Lives for the whole thread.
    dinput_hide::Bypass bypass;

    char db[MAX_PATH + 32];
    snprintf(db, sizeof(db), "%sgamecontrollerdb.txt", g_dir);

    SDL_SetLogOutputFunction(SdlLogOut, nullptr);
    SDL_SetLogPriorities(g_cfg.logVerbose ? SDL_LOG_PRIORITY_VERBOSE : SDL_LOG_PRIORITY_WARN);

    // CRITICAL (default): stop SDL from loading xinput1_x.dll itself. It would
    // find *this* DLL in the game folder and recurse into us.
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, g_cfg.sdlXInput ? "1" : "0");
    // We don't own a window; keep delivering input when the game is unfocused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (g_cfg.realXInput && g_cfg.ignoreXbox) {
        // Xbox pads belong to the real XInput driver; keep SDL's own Xbox drivers off them.
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "0");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX_360", "0");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX_ONE", "0");
    }
    SetTriHint(SDL_HINT_JOYSTICK_RAWINPUT, g_cfg.hintRawInput);
    SetTriHint(SDL_HINT_JOYSTICK_WGI, g_cfg.hintWGI);
    SetTriHint(SDL_HINT_JOYSTICK_HIDAPI, g_cfg.hintHIDAPI);
    SetTriHint(SDL_HINT_JOYSTICK_DIRECTINPUT, g_cfg.hintDInput);
    SetTriHint(SDL_HINT_JOYSTICK_THREAD, g_cfg.hintThread);

    int ver = SDL_GetVersion();
    LOGI("SDL %d.%d.%d (%s) on worker thread %lu", SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver),
         SDL_VERSIONNUM_MICRO(ver), SDL_GetRevision(), GetCurrentThreadId());

    ULONGLONG t0 = GetTickCount64();
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        SetEvent(g_readyEvent);  // unblock the game's first XInput call; every slot stays "not connected"
        return 1;
    }
    LOGI("SDL initialised (%llu ms)", (unsigned long long)(GetTickCount64() - t0));

    // Optional user-supplied mappings (https://github.com/mdqinc/SDL_GameControllerDB)
    if (GetFileAttributesA(db) != INVALID_FILE_ATTRIBUTES) {
        int n = SDL_AddGamepadMappingsFromFile(db);
        if (n < 0) LOGE("gamecontrollerdb.txt: load failed: %s", SDL_GetError());
        else       LOGI("gamecontrollerdb.txt: %d mapping(s) added", n);
    }

    OpenAllConnected();
    Tick();  // fill the state cache (and pick up pads via ADDED events; duplicates are ignored)
    InterlockedExchange(&g_ready, 1);
    SetEvent(g_readyEvent);
    LOGI("SDL backend ready; game threads no longer touch SDL");

    for (;;) {
        Tick();
        SDL_Delay(kPollDelayMs);
    }
}

// ---- one-time init --------------------------------------------------------

BOOL CALLBACK InitOnceFn(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);  // before the worker exists: it uses RawLock

    // Fallback for games that load dinput8.dll dynamically after us.
    if (g_cfg.hideDInput) dinput_hide::Install(true);

    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
    if (g_cfg.realXInput) real::Load();  // before the worker starts: it probes real pads for slot choice

    // The worker runs code from this DLL for the rest of the process, so a FreeLibrary of
    // the wrapper must never unmap it (the thread would crash). Pin it.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&g_once), &pinned);  // any address inside this module

    HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!h) {
        LOGE("could not start the SDL worker thread (error %lu)", GetLastError());
        return TRUE;  // every slot stays "not connected"
    }
    CloseHandle(h);

    // SDL runs on its own thread, but the game usually polls pads once at startup. Wait
    // (bounded, plain wait: no SDL, no window messages) for the first scan so a pad that
    // is already plugged in is present on that first poll. On timeout the pad simply
    // appears once the worker is done.
    ULONGLONG t0 = GetTickCount64();
    WaitForSingleObject(g_readyEvent, kReadyWaitMs);
    LOGI("first XInput call waited %llu ms for SDL (%s)", (unsigned long long)(GetTickCount64() - t0),
         InterlockedCompareExchange(&g_ready, 0, 0) ? "ready" : "not ready yet");
    return TRUE;
}

inline void EnsureInit() { InitOnceExecuteOnce(&g_once, InitOnceFn, nullptr, nullptr); }

// Lock for game threads.
struct Lock {
    Lock()  { EnsureInit(); EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

DWORD GetStateImpl(DWORD idx, XINPUT_STATE* st, bool withGuide) {
    if (!st || idx >= kMaxPads) return ERROR_BAD_ARGUMENTS;
    InterlockedIncrement(&g_getStateCalls);
    EnsureInit();
    if (real::GetState(idx, st, withGuide) == ERROR_SUCCESS) return ERROR_SUCCESS;  // a real Xbox pad wins
    Lock lock;
    Slot& s = g_slots[idx];

    int connected = s.connected ? 1 : 0;
    if (s.reported != connected) {  // log transitions only, not every poll
        LOGI("XInputGetState(%lu): %s", idx, connected ? "pad connected" : "no pad (ERROR_DEVICE_NOT_CONNECTED)");
        s.reported = connected;
    }
    if (!connected) return ERROR_DEVICE_NOT_CONNECTED;

    XINPUT_GAMEPAD cur = s.cur;
    if (!withGuide) cur.wButtons &= (WORD)~XINPUT_GAMEPAD_GUIDE;
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
    EnsureInit();
    if (real::Active() && real::p_SetState && real::p_SetState(idx, v) == ERROR_SUCCESS) return ERROR_SUCCESS;
    Lock lock;
    Slot& s = g_slots[idx];
    if (!s.connected) return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_cfg.rumble) return ERROR_SUCCESS;
    if (v->wLeftMotorSpeed == s.wantLow && v->wRightMotorSpeed == s.wantHigh)
        return ERROR_SUCCESS;  // identical to the last request; the worker already handles refreshing
    s.wantLow = v->wLeftMotorSpeed;
    s.wantHigh = v->wRightMotorSpeed;
    s.rumbleDirty = true;      // applied by the SDL worker within ~2 ms; never blocks the game
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetCapabilities(DWORD idx, DWORD flags, XINPUT_CAPABILITIES* caps) {
    LOG_ONCE("first call: XInputGetCapabilities");
    if (idx >= kMaxPads || !caps) return ERROR_BAD_ARGUMENTS;
    EnsureInit();
    if (real::Active() && real::p_GetCaps && real::p_GetCaps(idx, flags, caps) == ERROR_SUCCESS) return ERROR_SUCCESS;
    Lock lock;
    if (!g_slots[idx].connected) return ERROR_DEVICE_NOT_CONNECTED;
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
    EnsureInit();
    if (real::Active() && real::p_Enable) real::p_Enable(enable);
}

DWORD WINAPI XInputGetBatteryInformation(DWORD idx, BYTE devType, XINPUT_BATTERY_INFORMATION* b) {
    LOG_ONCE("first call: XInputGetBatteryInformation");
    if (idx >= kMaxPads || !b) return ERROR_BAD_ARGUMENTS;
    EnsureInit();
    if (real::Active() && real::p_Battery && real::p_Battery(idx, devType, b) == ERROR_SUCCESS) return ERROR_SUCCESS;
    Lock lock;
    if (!g_slots[idx].connected) return ERROR_DEVICE_NOT_CONNECTED;
    b->BatteryType = BATTERY_TYPE_WIRED;
    b->BatteryLevel = BATTERY_LEVEL_FULL;
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetKeystroke(DWORD idx, DWORD reserved, XINPUT_KEYSTROKE* k) {
    LOG_ONCE("first call: XInputGetKeystroke (real pads only; SDL pads return ERROR_EMPTY)");
    if (idx >= kMaxPads && idx != 0xFF) return ERROR_BAD_ARGUMENTS;
    EnsureInit();
    if (real::Active() && real::p_Keystroke) {
        DWORD r = real::p_Keystroke(idx, reserved, k);
        if (r != ERROR_DEVICE_NOT_CONNECTED) return r;
    }
    return ERROR_EMPTY;  // no pending keystrokes
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
