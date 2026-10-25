#pragma once

// The XInput gamepad layout the game reads (XamInputGetState fills the Xbox 360's own copy of
// these structures, the same as the PC's <xinput.h>). Every input source (XInput, the PlayStation
// HID reader, SDL gamepads, the keyboard, test scripts) is turned into this.
#if defined(_WIN32) && !defined(CRASH_NO_WIN32_HEADERS) // the define: portability checks on Windows
#include <windows.h>
#include <xinput.h>
#else
#include <cstdint>

struct XINPUT_GAMEPAD
{
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};

struct XINPUT_STATE
{
    uint32_t dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};

struct XINPUT_VIBRATION
{
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};

struct XINPUT_CAPABILITIES
{
    uint8_t Type;
    uint8_t SubType;
    uint16_t Flags;
    XINPUT_GAMEPAD Gamepad;
    XINPUT_VIBRATION Vibration;
};

#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000

#define XINPUT_DEVTYPE_GAMEPAD 0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD 0x01
#endif
