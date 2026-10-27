// Hides gamepad-class devices from DirectInput 8 enumeration using MinHook.
// Call once, from a normal (non-loader-lock) context such as the INIT_ONCE callback.
#pragma once

// Returns true if the DirectInput hooks were installed.
bool InstallDInputHooks(bool debug);