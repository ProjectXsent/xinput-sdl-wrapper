// xinput_test: loads an XInput DLL by path and prints live state for slots 0-3.
//   xinput_test.exe [path\to\xinput1_3.dll]
// Must be built with the same bitness as the DLL. Run it from the DLL's folder
// so SDL3.dll is found. Press Ctrl+C to exit.
#include "xinput_types.h"
#include <cstdio>

typedef DWORD(WINAPI* GetStateFn)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* SetStateFn)(DWORD, XINPUT_VIBRATION*);

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "xinput1_3.dll";
    HMODULE m = LoadLibraryA(path);
    if (!m) { std::printf("LoadLibrary(%s) failed: %lu\n", path, GetLastError()); return 1; }
    auto getState = (GetStateFn)GetProcAddress(m, "XInputGetState");
    auto setState = (SetStateFn)GetProcAddress(m, "XInputSetState");
    if (!getState) { std::printf("XInputGetState not exported\n"); return 1; }
    std::printf("Loaded %s. Hold LT+RT to rumble slot 0. Ctrl+C quits.\n\n", path);

    for (;;) {
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE s = {};
            DWORD r = getState(i, &s);
            if (r == ERROR_SUCCESS) {
                const XINPUT_GAMEPAD& g = s.Gamepad;
                std::printf("[%lu] pkt=%-6lu btn=%04X LT=%3u RT=%3u L=(%6d,%6d) R=(%6d,%6d)   \n",
                            i, s.dwPacketNumber, g.wButtons, g.bLeftTrigger, g.bRightTrigger,
                            g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
                if (i == 0 && setState) {
                    XINPUT_VIBRATION v = {(WORD)(g.bLeftTrigger * 257), (WORD)(g.bRightTrigger * 257)};
                    setState(0, &v);
                }
            } else {
                std::printf("[%lu] not connected                                                    \n", i);
            }
        }
        std::printf("\x1b[4A");  // move cursor up 4 lines (Windows 10+ terminals)
        Sleep(16);
    }
}
