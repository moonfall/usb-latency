/*
 * The three HID modes, and the strings that name them.
 *
 * Shared by main.cpp (which owns the mode state machine and the USB
 * identity) and by the per-board feedback code in board_*.cpp (which
 * paints the mode onto whatever indicator the board has). Header-only:
 * these are pure lookup tables, not state.
 */
#pragma once

#include <stdint.h>

// A Mode is a USB identity and nothing else, which is why it is fixed for
// the life of a boot. The light meter deliberately is NOT one of these:
// it touches no USB state, so making it a mode would have saddled it with
// a reboot it doesn't need. It's a view the board layer toggles live
// instead — from its own menu, on boards that have one (see board.h).
enum Mode : uint8_t {
  MODE_GAMEPAD = 0,
  MODE_KEYBOARD,
  MODE_MOUSE,
  MODE_COUNT,
};

// Name of the mode itself, for on-screen display.
inline const char *modeName(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "GAMEPAD";
    case MODE_KEYBOARD: return "KEYBOARD";
    case MODE_MOUSE:    return "MOUSE";
    default:            return "?";
  }
}

// What a press actually sends in that mode.
inline const char *modeAction(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "\"X\" button";
    case MODE_KEYBOARD: return "Space";
    case MODE_MOUSE:    return "Left click";
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
    default:            return "USB Latency Tester";
  }
}
