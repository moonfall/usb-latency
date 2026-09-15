/*
 * The USB identities this firmware can boot as, and the strings that name
 * them.
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
//
// MODE_STORAGE sits deliberately PAST MODE_COUNT, and the gap is the
// whole design. MODE_COUNT is two things at once: the number of HID
// identities the mode rotation cycles through, and a value no real mode
// ever takes (board_atoms3r.cpp uses it as its "nothing drawn yet"
// sentinel). Keeping it at 3 means `(pendingMode + 1) % MODE_COUNT`
// structurally cannot land on storage — you cannot hold the button into
// a drive, and the S3-Zero, which has no storage partition at all, cannot
// reach it even by accident. But storage IS a genuine fourth USB identity
// — it enumerates as a mass-storage device, with its own product name,
// fixed for the boot exactly like the other three — so it is a Mode and
// not a flag hung off the side of one. That is what keeps it out of
// main.cpp's press path for free: sendPress()/sendRelease() already end
// in `default: break;`, which is precisely "this identity sends nothing".
// What a *future* boot will be is persisted separately — see the
// storage-armed flag in main.cpp — because that has to remember the HID
// mode to come back to as well.
enum Mode : uint8_t {
  MODE_GAMEPAD = 0,
  MODE_KEYBOARD,
  MODE_MOUSE,
  MODE_COUNT,    // cycleable HID identities, and the "no mode" sentinel
  MODE_STORAGE,  // outside the rotation on purpose; see above
};

// Name of the mode itself, for on-screen display.
inline const char *modeName(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "GAMEPAD";
    case MODE_KEYBOARD: return "KEYBOARD";
    case MODE_MOUSE:    return "MOUSE";
    case MODE_STORAGE:  return "STORAGE";
    default:            return "?";
  }
}

// What a press actually sends in that mode.
inline const char *modeAction(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "\"X\" button";
    case MODE_KEYBOARD: return "Space";
    case MODE_MOUSE:    return "Left click";
    case MODE_STORAGE:  return "nothing";  // a drive has no button to press
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
    case MODE_STORAGE:  return "USB Latency Tester - Storage";
    default:            return "USB Latency Tester";
  }
}
