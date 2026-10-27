# xinput-sdl-wrapper

A drop-in replacement for `xinput1_3.dll` (also `xinput1_4.dll` and
`xinput9_1_0.dll`) that reads controllers through **SDL3's GameController API**
and presents them to the game as Xbox 360 pads. Use it to make PlayStation,
Switch, generic DirectInput and other SDL-supported controllers work in games
that only understand XInput.

```
Game  --XInputGetState-->  xinput1_3.dll (this project)  --SDL3-->  your controller
```

## Features

- Up to 4 pads, hot-plug aware, stable slot assignment
- Full button/stick/trigger mapping, rumble (low/high-frequency motors)
- `XInputGetStateEx` (ordinal 100) with Guide button
- Optional `gamecontrollerdb.txt` for extra mappings
- Optional `xinput_sdl.ini` (swap A/B, disable rumble, debug logging)
- Packet number only changes when the state changes (like real XInput)
- No work in `DllMain`; lazy, thread-safe init
- Static CRT: no VC++ redistributable required

## Quick start (using a release/CI build)

1. Find out what the game loads and its bitness (see below).
2. Copy the matching DLL (`xinput1_3.dll`, ...) **and `SDL3.dll`** next to the
   game's `.exe`. 32-bit game -> 32-bit build; 64-bit game -> 64-bit build.
3. Optionally copy `xinput_sdl.ini` and `gamecontrollerdb.txt` there too.
4. Launch the game.

### Which DLL name does the game use?

Check the exe's imports:

```
dumpbin /imports Game.exe | findstr /i xinput
```
(or open it in Dependencies / PE-bear). Common values: `XINPUT1_3.dll`
(DX9-era games), `XINPUT1_4.dll` (Win8+ era), `XINPUT9_1_0.dll`. If unsure,
place copies under several names; unused ones are harmless.

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

Output lands in `build*/bin/Release/` (DLL, `SDL3.dll`, `xinput_sdl.ini`,
`xinput_test.exe`).

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

## Testing without a game

Run from the folder containing the DLL and `SDL3.dll`:

```
xinput_test.exe xinput1_3.dll
```

It prints live state for slots 0-3 and rumbles slot 0 proportionally to the
triggers. (`xinput_test.exe` must have the same bitness as the DLL.)

## Configuration (`xinput_sdl.ini`)

| Key | Default | Description |
|---|---|---|
| `SwapAB` | `0` | Swap A/B and X/Y (Nintendo layout) |
| `Rumble` | `1` | `0` ignores game rumble |
| `Debug` | `0` | Log to `OutputDebugString` (use Sysinternals DebugView) |

`gamecontrollerdb.txt` (from
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)) placed
next to the DLL is loaded automatically.

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

## Limitations

- Not implemented: `XInputGetKeystroke` (returns `ERROR_EMPTY`), audio device
  GUIDs, headset, real battery levels.
- Games that detect Xbox pads through Raw Input / HID device paths (`IG_`) or
  load XInput by absolute path bypass this approach.
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
| Need logs | `Debug=1` + DebugView |

## License

MIT, see `LICENSE`.
