# xinput-sdl-wrapper

A drop-in replacement for `xinput1_3.dll` (also `xinput1_4.dll` and
`xinput9_1_0.dll`) that reads controllers through **SDL3's Gamepad API**
and presents them to the game as Xbox 360 pads. Use it to make PlayStation,
Switch, generic DirectInput and other SDL-supported controllers work in games
that only understand XInput.

The project also ships an optional companion, **`dinput8.dll` (dinput8-hide)**:
a DirectInput 8 proxy that hides gamepads from the game, so it sees each pad
only once, through XInput, instead of twice or through the wrong API.

```
Game  --XInputGetState-->  xinput1_3.dll (this project)  --SDL3-->  your controller

Game  --DirectInput8-->    dinput8.dll (dinput8-hide)  --> real dinput8.dll
                           (keyboard/mouse pass through; gamepads are filtered out)
```

## Features

**XInput wrapper**

- Up to 4 pads, hot-plug aware, stable slot assignment
- Full button/stick/trigger mapping, rumble (low/high-frequency motors)
- `XInputGetStateEx` (ordinal 100) with Guide button
- Optional `gamecontrollerdb.txt` for extra mappings
- Optional `xinput_sdl.ini` (swap A/B, disable rumble, debug logging)
- Packet number only changes when the state changes (like real XInput)
- No work in `DllMain`; lazy, thread-safe init
- Static CRT: no VC++ redistributable required

**dinput8-hide (optional)**

- Forwards everything to the real system `dinput8.dll`
- Wraps `IDirectInput8A` / `IDirectInput8W` so gamepad-class devices are
  removed from `EnumDevices` and `EnumDevicesBySemantics`
- Blocks `CreateDevice` (returns `DIERR_DEVICENOTREG`) and `GetDeviceStatus`
  (returns `DI_NOTATTACHED`) for hidden devices
- Keyboard, mouse and other non-pad devices are left untouched
- SDL and the XInput wrapper itself are exempt, so they still see the real devices
- No configuration of its own; reuses the `Debug` key from `xinput_sdl.ini`

## Quick start (using a release/CI build)

1. Find out what the game loads and its bitness (see below).
2. Copy the matching DLL (`xinput1_3.dll`, ...) **and `SDL3.dll`** next to the
   game's `.exe`. 32-bit game -> 32-bit build; 64-bit game -> 64-bit build.
3. Optionally copy `xinput_sdl.ini` and `gamecontrollerdb.txt` there too.
4. *(Optional)* If the game also enumerates the pad through DirectInput and
   gets confused (double input, wrong button layout, wrong player), copy
   `dinput8.dll` next to the exe as well. See [dinput8-hide](#dinput8-hide).
5. Launch the game.

### Which DLL name does the game use?

Check the exe's imports:

```
dumpbin /imports Game.exe | findstr /i "xinput dinput"
```
(or open it in Dependencies / PE-bear). Common values: `XINPUT1_3.dll`
(DX9-era games), `XINPUT1_4.dll` (Win8+ era), `XINPUT9_1_0.dll`. If unsure,
place copies under several names; unused ones are harmless.

If the exe imports `DINPUT8.dll` (or creates DirectInput 8 at runtime), the
`dinput8.dll` proxy is applicable.

## Building

Requirements: CMake >= 3.15, a C++17 compiler (MSVC recommended; MinGW-w64 works),
and SDL3 development files (SDL3 >= 3.4.0).

### MSVC + vcpkg

```bat
vcpkg install sdl3:x64-windows sdl3:x86-windows

:: 64-bit
cmake -S . -B build64 -A x64 -DXINPUT_DLL_NAME=xinput1_3 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build64 --config Release

:: 32-bit
cmake -S . -B build32 -A Win32 -DXINPUT_DLL_NAME=xinput1_3 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x86-windows
cmake --build build32 --config Release
```

Output lands in `build*/bin/Release/` (`xinput1_3.dll`, `dinput8.dll`,
`SDL3.dll`, `xinput_sdl.ini`, `xinput_test.exe`).

### MinGW-w64 (32-bit example)

```sh
cmake -S . -B build -G Ninja -DXINPUT_DLL_NAME=xinput1_3 \
  -DCMAKE_PREFIX_PATH=/path/to/SDL3-3.x.y/i686-w64-mingw32
cmake --build build
```

### Other options

| Option | Default | Meaning |
|---|---|---|
| `XINPUT_DLL_NAME` | `xinput1_3` | `xinput1_3`, `xinput1_4` or `xinput9_1_0` |
| `XINPUT_STATIC_SDL` | `OFF` | Link SDL3 statically (needs a static SDL3 package, e.g. `x64-windows-static`) |
| `XINPUT_BUILD_TEST` | `ON` | Build `xinput_test.exe` |
| `DINPUT8_BUILD_HIDE` | `ON` | Build the `dinput8.dll` proxy (dinput8-hide) |

`dinput8.dll` does not depend on SDL and has no import library dependency on
`dinput8.lib` or `dxguid.lib`.

## Testing without a game

Run from the folder containing the DLL and `SDL3.dll`:

```
xinput_test.exe xinput1_3.dll
```

It prints live state for slots 0-3 and rumbles slot 0 proportionally to the
triggers. (`xinput_test.exe` must have the same bitness as the DLL.)

## dinput8-hide

Many games probe both DirectInput and XInput. With a PlayStation or Switch pad
the game may then see the same physical controller twice (once as a DirectInput
joystick with a generic layout, once as an XInput pad), or pick DirectInput
and ignore XInput entirely. `dinput8.dll` removes the DirectInput side of that
so only the XInput wrapper is left.

### What is hidden

| DirectInput device type | Hidden? |
|---|---|
| Joystick, Gamepad, Driving, Flight, First-person | Yes |
| Supplemental (pedals, throttles, odd pads) | Yes |
| Generic device (`DI8DEVTYPE_DEVICE`, e.g. generic HID) | No (may not be a pad) |
| Keyboard, Mouse, Screen pointer, Remote, ... | No |

The policy is compiled in (`Config` in `src/dinput8_hide.cpp`). To change it,
flip the `hideJoystickClass`, `hideSupplemental` or `hideGeneric` constants and
rebuild.

### Install

Copy `dinput8.dll` (same bitness as the game) next to the game's `.exe`,
together with the XInput wrapper and `SDL3.dll`. The proxy loads the real
`dinput8.dll` from the Windows system directory (`SysWOW64` for 32-bit
processes on 64-bit Windows), never itself.

### Caller exemptions

When a module calls `DirectInput8Create`, the proxy checks the caller's DLL
name. Callers whose file name starts with `sdl` or `xinput` get the unwrapped
real interface. This matters because SDL can use DirectInput as one of its own
joystick backends, and SDL is what feeds the XInput wrapper; hiding pads from
it would defeat the whole setup. Everyone else, including the game exe, gets
the filtered wrapper.

## Configuration (`xinput_sdl.ini`)

| Key | Default | Used by | Description |
|---|---|---|---|
| `SwapAB` | `0` | xinput wrapper | Swap A/B and X/Y (Nintendo layout) |
| `Rumble` | `1` | xinput wrapper | `0` ignores game rumble |
| `Debug` | `0` | both | Log to `OutputDebugString` (use Sysinternals DebugView) |

All keys live in the `[General]` section. `dinput8.dll` reads only `Debug`; the
file is optional for it.

`gamecontrollerdb.txt` (from
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)) placed
next to the DLL is loaded automatically by the XInput wrapper.

## Design notes

- **SDL's own XInput backend is disabled** (`SDL_HINT_XINPUT_ENABLED=0`). If
  SDL loaded `xinput1_x.dll` from the game folder it would find *this* DLL and
  recurse. Real Xbox pads still work via SDL's HIDAPI/RawInput/WGI backends.
- **Exports** are defined in `src/def/*.def`, which gives undecorated names and
  fixed ordinals on both x86 (stdcall) and x64.
- **Own type definitions** (`src/xinput_types.h`) are used instead of
  `<xinput.h>` to avoid SDK/MinGW declaration clashes.
- **Events:** the wrapper polls SDL's event queue for hot-plug. If the game
  *also* uses the same `SDL3.dll` in the same process, the two would share one
  event queue; in that case build with `XINPUT_STATIC_SDL=ON`.
- **dinput8-hide wraps COM interfaces.** `IDirectInput8A` and `IDirectInput8W`
  are each wrapped; if a game queries the other charset variant through
  `QueryInterface`, that is wrapped too so it cannot be used to bypass the
  filter.
- **dinput8-hide never unloads.** `DllCanUnloadNow` always returns `S_FALSE`,
  since wrapper objects may outlive the host's idea of what is in use.
- **Loader lock:** neither DLL does real work in `DllMain`; both initialise
  lazily on first use.

## Limitations

- Not implemented: `XInputGetKeystroke` (returns `ERROR_EMPTY`), audio device
  GUIDs, headset, real battery levels.
- Games that detect Xbox pads through Raw Input / HID device paths (`IG_`) or
  load XInput by absolute path bypass this approach.
- dinput8-hide only covers **DirectInput 8**. Older DirectInput (`dinput.dll`),
  Raw Input, HID, WGI and SDL/GLFW running inside the game are not filtered. A
  game-side SDL whose DLL name starts with `sdl` is exempt by design, so it
  will still see the real pads.
- dinput8-hide does not handle aggregation (`DirectInput8Create` with a
  non-NULL outer object); that case is passed through unfiltered.
- **Do not use with online games / anti-cheat**; DLL proxying is commonly
  flagged and may get you banned. Intended for single-player use.
- Windows only.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Game ignores the DLL | Wrong DLL name or bitness; check imports |
| Game crashes at start | `SDL3.dll` missing/wrong bitness next to the exe |
| Pad not detected | Check it in `xinput_test.exe`; add a mapping to `gamecontrollerdb.txt` |
| Buttons mis-mapped | Fix SDL mapping, or set `SwapAB=1` |
| No rumble | Pad/driver lacks SDL rumble support; check `Rumble=1` |
| Pad appears twice / duplicate input | Add `dinput8.dll` (dinput8-hide); confirm the game imports DirectInput 8 |
| Game loses its keyboard/mouse | Should not happen; enable `Debug=1` and check which device was blocked |
| Pad still seen via DirectInput | Game uses old `dinput.dll`, Raw Input, or its own SDL (see Limitations) |
| Pad stops working after adding `dinput8.dll` | Caller exemption not matching; with `Debug=1` check the "DirectInput8Create from ..." lines |
| Need logs | `Debug=1` + DebugView (both DLLs log with `[xinput-sdl]` / `[dinput8-hide]` prefixes) |

## License

MIT, see `LICENSE`.
