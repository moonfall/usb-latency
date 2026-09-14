/*
 * The device modes — three HID ones plus a light meter — and the strings
 * that name them.
 *
 * Shared by main.cpp (which owns the mode state machine and the USB
 * identity) and by the per-board feedback code in board_*.cpp (which
 * paints the mode onto whatever indicator the board has). Header-only:
 * these are pure lookup tables, not state.
 */
#pragma once

#include <stdint.h>

// MODE_LIGHT must stay last: boards without a light sensor report their
// mode count as MODE_LIGHT, which drops it off the end of the rotation.
enum Mode : uint8_t {
  MODE_GAMEPAD = 0,
  MODE_KEYBOARD,
  MODE_MOUSE,
  MODE_LIGHT,
  MODE_COUNT,
};

// MODE_LIGHT is a meter, not an input device: it constructs no HID class
// and a press sends nothing. Every other mode both sends a report and, on
// a board with a sensor, times the display's response to it.
inline bool modeSendsHid(Mode mode) { return mode != MODE_LIGHT; }

// Name of the mode itself, for on-screen display.
inline const char *modeName(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "GAMEPAD";
    case MODE_KEYBOARD: return "KEYBOARD";
    case MODE_MOUSE:    return "MOUSE";
    case MODE_LIGHT:    return "LIGHT";
    default:            return "?";
  }
}

// What a press actually sends in that mode.
inline const char *modeAction(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "\"X\" button";
    case MODE_KEYBOARD: return "Space";
    case MODE_MOUSE:    return "Left click";
    case MODE_LIGHT:    return "light meter";
    default:            return "";
  }
}

// USB product string, so the host's device picker identifies the active
// mode by name rather than showing a generic, mode-less device.
inline const char *modeProductName(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "USB Latency Tester - Gamepad";
    case MODE_KEYBOARD: return "USB Latency Tester - Keyboard";
    case MODE_MOUSE:    return "USB Latency Tester - Mouse";
    case MODE_LIGHT:    return "USB Latency Tester - Light";
    default:            return "USB Latency Tester";
  }
}
