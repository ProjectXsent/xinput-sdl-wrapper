// Minimal, self-contained copies of the XInput ABI types.
// We deliberately do NOT include <xinput.h>: its declarations differ between
// Windows SDK / MinGW versions and would clash with our exported definitions.
#pragma once
#include <windows.h>

#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_GUIDE          0x0400   // only via XInputGetStateEx (ordinal 100)
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000

#define XINPUT_DEVTYPE_GAMEPAD        0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD     0x01

#define XINPUT_FLAG_GAMEPAD           0x00000001

#define BATTERY_DEVTYPE_GAMEPAD       0x00
#define BATTERY_TYPE_WIRED            0x01
#define BATTERY_LEVEL_FULL            0x03

struct XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};

struct XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

struct XINPUT_CAPABILITIES {
    BYTE             Type;
    BYTE             SubType;
    WORD             Flags;
    XINPUT_GAMEPAD   Gamepad;
    XINPUT_VIBRATION Vibration;
};

struct XINPUT_BATTERY_INFORMATION {
    BYTE BatteryType;
    BYTE BatteryLevel;
};

struct XINPUT_KEYSTROKE {
    WORD  VirtualKey;
    WCHAR Unicode;
    WORD  Flags;
    BYTE  UserIndex;
    BYTE  HidCode;
};
