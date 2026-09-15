/*
 * The host-facing identities this firmware can boot as, and the strings
 * that name them.
 *
 * Shared by main.cpp (which owns the mode state machine, USB and the
 * radio) and by the per-board feedback code in board_*.cpp (which paints
 * the mode onto whatever indicator the board has). Header-only: these are
 * pure lookup tables, not state.
 */
#pragma once

#include <stdint.h>

// A Mode is a host-facing identity and nothing else, which is why it is
// fixed for the life of a boot. The light meter deliberately is NOT one
// of these: it touches no USB or BLE state, so making it a mode would
// have saddled it with a reboot it doesn't need. It's a view the board
// layer toggles live instead — from its own menu, on boards that have
// one (see board.h).
//
// There are two families of three. The USB three enumerate as a single
// USB HID device and send their report down the wire; the BLE three
// enumerate as nothing at all over USB (it is CDC-only for debug output)
// and send the equivalent HID-over-GATT notification to a paired host
// instead. The action is the same in both families — gamepad "X", Space,
// left click — precisely so the two transports can be compared with one
// instrument, and everything downstream of sendPress() (the measurement,
// the stats, the histogram, the automated test, the run files) does not
// know or care which family is running.
//
// ORDER IS A STORED FORMAT. These values go into NVS, so 0..2 have to
// keep meaning what they always meant — an existing device must come back
// up as the USB mode it was left in rather than as something new. The BLE
// three were therefore appended after MOUSE rather than interleaved with
// their USB counterparts, which would have read better here and silently
// changed what every deployed device was. The reverse direction is worth
// knowing too: a device left in a BLE mode (3..5) and then downgraded to
// a firmware that predates them fails the `stored < MODE_COUNT` check in
// main.cpp and falls back to gamepad, which is the right failure.
//
// MODE_STORAGE sits deliberately PAST MODE_COUNT, and the gap is the
// whole design. MODE_COUNT is two things at once: the number of
// identities the mode rotation cycles through, and a value no real mode
// ever takes (board_atoms3r.cpp uses it as its "nothing drawn yet"
// sentinel). It grew from 3 to 6 when the BLE modes landed — which is
// exactly what puts them in the rotation and is the only change the
// rotation needed — while `(pendingMode + 1) % MODE_COUNT` still
// structurally cannot land on storage: you cannot hold the button into a
// drive, and the S3-Zero, which has no storage partition at all, cannot
// reach it even by accident. But storage IS a genuine seventh identity —
// it enumerates as a mass-storage device, with its own product name,
// fixed for the boot exactly like the other six — so it is a Mode and
// not a flag hung off the side of one. That is what keeps it out of
// main.cpp's press path for free: sendPress()/sendRelease() already end
// in `default:`, which is precisely "this identity sends nothing".
// What a *future* boot will be is persisted separately — see the
// storage-armed flag in main.cpp — because that has to remember the HID
// mode to come back to as well.
enum Mode : uint8_t {
  MODE_GAMEPAD = 0,
  MODE_KEYBOARD,
  MODE_MOUSE,
  MODE_BLE_GAMEPAD,
  MODE_BLE_KEYBOARD,
  MODE_BLE_MOUSE,
  MODE_COUNT,    // cycleable identities, and the "no mode" sentinel
  MODE_STORAGE,  // outside the rotation on purpose; see above
};

// True for the three identities that talk over the radio instead of the
// wire. The one place the distinction is drawn, so nothing else has to
// spell out a range of enum values: main.cpp branches on it to decide
// whether to build a HID device or a GATT server, and the board layer
// uses it to decide whether a link state is even a meaningful thing to
// show.
inline bool modeIsBle(Mode mode) {
  return mode == MODE_BLE_GAMEPAD || mode == MODE_BLE_KEYBOARD ||
         mode == MODE_BLE_MOUSE;
}

// Name of the mode itself, for on-screen display. Kept short enough to
// fit the AtomS3R's 128px mode line in Font0 (21 characters) with room to
// spare, since the BLE ones are the longest.
inline const char *modeName(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:      return "GAMEPAD";
    case MODE_KEYBOARD:     return "KEYBOARD";
    case MODE_MOUSE:        return "MOUSE";
    case MODE_BLE_GAMEPAD:  return "BLE GAMEPAD";
    case MODE_BLE_KEYBOARD: return "BLE KEYBOARD";
    case MODE_BLE_MOUSE:    return "BLE MOUSE";
    case MODE_STORAGE:      return "STORAGE";
    default:                return "?";
  }
}

// What a press actually sends in that mode. Deliberately identical
// between a USB mode and its BLE counterpart: the whole point of having
// both is that the only difference between the two numbers is the
// transport.
inline const char *modeAction(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:
    case MODE_BLE_GAMEPAD:  return "\"X\" button";
    case MODE_KEYBOARD:
    case MODE_BLE_KEYBOARD: return "Space";
    case MODE_MOUSE:
    case MODE_BLE_MOUSE:    return "Left click";
    case MODE_STORAGE:      return "nothing";  // a drive has no button to press
    default:                return "";
  }
}

// The USB product string. Set in every mode, BLE ones included — a BLE
// boot still enumerates as a CDC serial port for debug output, and a
// debug port that names the mode it belongs to beats an anonymous one.
//
// Note what this is NOT used for: anything the Bluetooth side shows. See
// modeBleName() below, which is bounded by limits USB does not have.
inline const char *modeProductName(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:      return "USB Latency Tester - Gamepad";
    case MODE_KEYBOARD:     return "USB Latency Tester - Keyboard";
    case MODE_MOUSE:        return "USB Latency Tester - Mouse";
    case MODE_BLE_GAMEPAD:  return "USB Latency Tester - BLE Gamepad";
    case MODE_BLE_KEYBOARD: return "USB Latency Tester - BLE Keyboard";
    case MODE_BLE_MOUSE:    return "USB Latency Tester - BLE Mouse";
    case MODE_STORAGE:      return "USB Latency Tester - Storage";
    default:                return "USB Latency Tester";
  }
}

// The Bluetooth name: both the GAP device name (characteristic 0x2A00,
// which a host reads over GATT) and the complete local name broadcast in
// the advertisement, so a host shows the same string in its picker and
// after connecting.
//
// A separate, shorter set of strings from modeProductName() because BLE
// has two length limits USB does not, and "USB Latency Tester - BLE
// Keyboard" (33 bytes) is over both of them:
//
//   31 bytes  the GAP device name. NimBLE's buffer for it is
//             CONFIG_BT_NIMBLE_GAP_DEVICE_NAME_MAX_LEN + 1, and
//             ble_svc_gap_device_name_set() returns BLE_HS_EINVAL for
//             anything longer — i.e. the name is silently NOT set and
//             the device advertises as whatever the default was. That
//             macro is not #ifndef-guarded in nimconfig.h, so it cannot
//             simply be raised with a -D.
//   29 bytes  the advertisement's name field (BLE_HS_ADV_MAX_FIELD_SZ).
//             NimBLE does not refuse an over-long one here; it truncates
//             and downgrades the AD type from "complete local name" to
//             "shortened local name", which would have put
//             "USB Latency Tester - BLE Keyb" in every picker.
//
// So the "USB " prefix goes — which is the right thing to cut anyway on
// a device that, in these three modes, is not a USB anything. The
// longest of these is 27 bytes, leaving slack against both limits rather
// than sitting exactly on one.
inline const char *modeBleName(Mode mode) {
  switch (mode) {
    case MODE_BLE_GAMEPAD:  return "Latency Tester BLE Gamepad";   // 26 bytes
    case MODE_BLE_KEYBOARD: return "Latency Tester BLE Keyboard";  // 27 bytes
    case MODE_BLE_MOUSE:    return "Latency Tester BLE Mouse";     // 24 bytes
    default:                return "Latency Tester";
  }
}
