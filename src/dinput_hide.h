// Hides DirectInput8 game controllers from the host game (MinHook).
//
// XInput games that *also* enumerate DirectInput see the physical pad twice
// (once through us, once through DirectInput). Dropping the DirectInput copy
// avoids double input / wrong layouts.
//
// Hooked:
//   dinput8!DirectInput8Create            (to reach the IDirectInput8A/W vtables)
//   IDirectInput8A/W::EnumDevices         (game controllers are filtered out)
//   IDirectInput8A/W::EnumDevicesBySemantics (same filter; used by action-mapping games)
//   IDirectInput8A/W::CreateDevice        (hidden instance GUIDs are refused)
//
// Not hooked: FindDevice, legacy dinput.dll, winmm joy*, RawInput.
#pragma once

namespace dinput_hide {

// Call once from DllMain: allocates the thread-local used by Bypass.
void Init();

// Installs the hooks. Idempotent. With allowLoadLibrary == false it only hooks
// dinput8.dll if it is already loaded (the only safe mode under loader lock);
// with true it will LoadLibrary("dinput8.dll") when needed.
// Returns true once the DirectInput8Create hook is in place.
bool Install(bool allowLoadLibrary);

// Removes the hooks (only meaningful when the DLL is unloaded via FreeLibrary).
void Shutdown();

// While alive on a thread, DirectInput calls made from that thread are passed
// through unfiltered. Wrap every SDL call that may touch DirectInput
// (SDL_Init, SDL_PumpEvents/PollEvent, SDL_OpenGamepad): SDL3's own joystick
// backend uses DirectInput8 and would otherwise be blinded by our hooks. The SDL
// worker thread simply holds one Bypass for its entire lifetime.
struct Bypass {
    Bypass();
    ~Bypass();
    Bypass(const Bypass&) = delete;
    Bypass& operator=(const Bypass&) = delete;
};

}  // namespace dinput_hide
