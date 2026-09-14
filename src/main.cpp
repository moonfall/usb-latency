/*
 * USB HID Gamepad / Keyboard / Mouse (single active device)
 * -------------------------------------------------------------------------
 * Emulates exactly ONE USB HID device at a time — gamepad ("X" button),
 * keyboard (Space), or mouse (left click) — selected by the board's one
 * button:
 * The one button carries everything, as a ladder of hold durations:
 *   - Short press: send the active mode's action (and, on a board with a
 *     light sensor, time the display's response to it).
 *   - Hold 2s: reset the measurement statistics.
 *   - Hold 4s: toggle between the latency readout and the raw light
 *     meter. Live — no reboot, no change to the USB identity.
 *   - Hold 7s, and every MODE_HOLD_MS after: select the next mode as
 *     "pending" and persist it to NVS. This does NOT reboot or change
 *     what's active this session; a manual reboot (reset button, or
 *     unplug/replug) is required for it to take effect.
 * The first two rungs only exist on a board with a sensor. Without one
 * the ladder collapses to its single original rung, with the mode cycle
 * back at MODE_HOLD_MS (3s) where it has always been.
 *
 * Note what is NOT on that ladder: the light meter is not a Mode. A Mode
 * is a USB identity, which is why it's stuck until a reboot — and the
 * meter touches no USB state whatsoever, so tying it to one would have
 * cost a reboot for nothing. It's a view the board layer flips at
 * runtime, and presses keep sending their HID report while it's up, which
 * is exactly what you want when aiming the sensor: press the button,
 * watch the thing you're pointing at change, and watch the bar move.
 *
 * Two boards are supported, one PlatformIO env each. Everything specific
 * to a board — which pin the button is on, and how state is shown — lives
 * behind board.h, with exactly one board_*.cpp compiled per env:
 *
 *   env:esp32-s3-zero    Waveshare ESP32-S3-Zero. BOOT button (GPIO0);
 *                        the mode shows as a colour on the onboard WS2812,
 *                        briefly at boot and while the button is held.
 *   env:m5stack-atoms3r  M5Stack AtomS3R. Screen button (GPIO41); the mode
 *                        is shown permanently on the 128x128 LCD.
 *
 * Nothing below this point is board-specific, and nothing in board_*.cpp
 * touches USB.
 *
 * Why a reboot at all, and why it must be manual: the ESP32 Arduino
 * core's TinyUSB HID wrapper registers a device's report descriptor
 * once, in that device class's *constructor* (USBHIDGamepad::USBHIDGamepad()
 * etc. all call hid.addDevice(this, ...) unconditionally) — merely
 * skipping .begin() on the other two doesn't stop them contributing their
 * report collection to the composite descriptor. Constructing all three,
 * like a normal composite-HID sketch does, is exactly what made the
 * device enumerate as a multi-collection gadget (which macOS then splits
 * into what look like several devices). To present as a single, genuine
 * gamepad *or* keyboard *or* mouse, only the active mode's class must
 * ever be constructed — which this sketch does by `new`-ing only one of
 * them, at boot, based on the mode persisted in NVS (`Preferences`). That
 * choice can't be changed without re-running setup() from scratch, i.e.
 * a reboot. An earlier version called `ESP.restart()` automatically once
 * a hold crossed the threshold, but on the S3-Zero GPIO0 doubles as the
 * chip's boot-mode strapping pin: since the hold-to-switch gesture means
 * the button is, by definition, still held down at that instant, the
 * reboot frequently landed the chip in the ROM download bootloader
 * instead of back in this firmware. Requiring a manual reboot — at a time
 * the user chooses, button released — sidesteps that hazard entirely.
 *
 * This also means every USB identity/descriptor call (constructing the
 * one HID device, USB.productName(), etc.) MUST happen in setup(), before
 * this sketch's own USB.begin() — which is exactly why
 * ARDUINO_USB_CDC_ON_BOOT must be 0 (see platformio.ini and the CLAUDE.md
 * gotcha): with it set to 1, the Arduino core's app_main() calls
 * USB.begin() itself before setup() ever runs, locking in a USB
 * descriptor that doesn't know about our HID device or product name at
 * all. With CDC_ON_BOOT off, `USBSerial` below is begun manually instead,
 * so Serial-style debug output over USB remains available.
 *
 * The USB product name includes the active mode (see modeProductName()),
 * so the host's device picker/System Information shows e.g. "USB Latency
 * Tester - Keyboard" rather than a generic, mode-less name. It only
 * updates after a reboot, same as the mode itself.
 *
 * Debounce strategy: leading-edge / "lockout" debounce (act immediately
 * on the edge, then ignore further changes for DEBOUNCE_MS) to keep
 * added latency at zero for the actual button action; only the
 * mode-cycling feedback is deliberately slow. For the same reason, the
 * HID report is always sent before any board feedback is drawn, so
 * lighting an LED or repainting a screen is never in the latency path.
 *
 * Requires ARDUINO_USB_MODE=0 (native USB-OTG / TinyUSB) — set in
 * platformio.ini.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <USB.h>
#include <USBCDC.h>
#include <USBHIDGamepad.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>

#include "board.h"
#include "mode.h"

// --- Configuration ---------------------------------------------------
static const uint32_t DEBOUNCE_MS = 25;     // lockout window after a trigger

// The hold ladder. The first two rungs exist only where boardHasSensor();
// the mode rung then follows MODE_HOLD_MS after the last rung that does
// exist, and repeats at that interval for as long as the button is held.
// So: 2s / 4s / 7s / 10s ... with a sensor, and plain 3s / 6s ... without.
static const uint32_t STATS_HOLD_MS = 2000;
static const uint32_t METER_HOLD_MS = 4000;
static const uint32_t MODE_HOLD_MS = 3000;

static const char *PREFS_NAMESPACE = "usbmode";
static const char *PREFS_KEY = "mode";

static Preferences prefs;
static Mode activeMode = MODE_GAMEPAD;   // mode this boot actually enumerated as
static Mode pendingMode = MODE_GAMEPAD;  // mode a reboot would pick up; dialed in by holding

// Manual CDC serial (see file header on why ARDUINO_USB_CDC_ON_BOOT is 0).
static USBCDC USBSerial;

// Only the active mode's device is ever constructed (see file header) —
// the other two pointers stay null for the life of this boot.
static USBHIDGamepad *gamepad = nullptr;
static USBHIDKeyboard *keyboard = nullptr;
static USBHIDMouse *mouse = nullptr;

static bool stableState = false;   // false = released, true = pressed
static uint32_t lastTriggerMs = 0;
static uint32_t nextCycleMs = 0;   // when the current hold next advances pendingMode
static bool cycledThisHold = false;  // true once this hold has advanced the mode at least once

// Rungs still ahead in the current hold, with the time each one fires.
static bool statsRungAhead = false;
static uint32_t statsAtMs = 0;
static bool meterRungAhead = false;
static uint32_t meterAtMs = 0;

// The rung a fresh hold would reach first — also what the hint falls back
// to on release.
static inline HoldRung firstRung() {
  return boardHasSensor() ? RUNG_STATS : RUNG_MODE;
}

static inline void sendPress() {
  switch (activeMode) {
    case MODE_GAMEPAD:  gamepad->pressButton(BUTTON_X); break;
    case MODE_KEYBOARD: keyboard->press(' '); break;
    case MODE_MOUSE:    mouse->press(MOUSE_LEFT); break;
    default: break;
  }
}

static inline void sendRelease() {
  switch (activeMode) {
    case MODE_GAMEPAD:  gamepad->releaseButton(BUTTON_X); break;
    case MODE_KEYBOARD: keyboard->release(' '); break;
    case MODE_MOUSE:    mouse->release(MOUSE_LEFT); break;
    default: break;
  }
}

// Advances pendingMode by one and persists it — takes effect on the next
// manual reboot, not this session.
static void advancePendingMode() {
  pendingMode = static_cast<Mode>((pendingMode + 1) % MODE_COUNT);
  prefs.putUChar(PREFS_KEY, pendingMode);

  boardShowPending(activeMode, pendingMode, !cycledThisHold);
  cycledThisHold = true;
}

void setup() {
  boardBegin();

  prefs.begin(PREFS_NAMESPACE, false);
  uint8_t stored = prefs.getUChar(PREFS_KEY, MODE_GAMEPAD);
  activeMode = (stored < MODE_COUNT) ? static_cast<Mode>(stored) : MODE_GAMEPAD;
  pendingMode = activeMode;

  // Construct the one active-mode device, and set the product name to
  // match, before USB.begin() — both are rejected as no-ops afterwards.
  switch (activeMode) {
    case MODE_GAMEPAD:  gamepad = new USBHIDGamepad();  gamepad->begin();  break;
    case MODE_KEYBOARD: keyboard = new USBHIDKeyboard(); keyboard->begin(); break;
    case MODE_MOUSE:    mouse = new USBHIDMouse();      mouse->begin();    break;
    default: break;
  }
  USB.productName(modeProductName(activeMode));
  USBSerial.begin();
  USB.begin();

  // Only now that USB is up is it safe to spend time on the indicator.
  boardShowBoot(activeMode, pendingMode);
  boardShowHoldHint(firstRung());
}

void loop() {
  bool raw = boardButtonPressed();
  uint32_t now = millis();

  // Outside the lockout window, any change is a real edge: act on it
  // immediately, then start the lockout to swallow bounce.
  if ((now - lastTriggerMs) >= DEBOUNCE_MS && raw != stableState) {
    // t0 for latency measurement: the edge itself, before the report is
    // queued, so the HID send counts as part of what's being measured.
    int64_t edgeMicros = esp_timer_get_time();
    stableState = raw;
    lastTriggerMs = now;

    // HID report first, feedback second — never the other way round.
    if (stableState) {
      sendPress();
      // Lay out this hold's ladder, skipping the rungs the board has no
      // use for so the mode rung stays where it has always been on a
      // board without a sensor.
      cycledThisHold = false;
      statsRungAhead = meterRungAhead = boardHasSensor();
      statsAtMs = now + STATS_HOLD_MS;
      meterAtMs = now + METER_HOLD_MS;
      nextCycleMs = (boardHasSensor() ? meterAtMs : now) + MODE_HOLD_MS;
    } else {
      sendRelease();
      statsRungAhead = meterRungAhead = false;
      boardShowHoldHint(firstRung());
    }
    boardShowPress(stableState, activeMode, edgeMicros);
  }

  // Walk the ladder for as long as the button stays down. Each rung fires
  // once per hold, then hands the hint on to the next one.
  if (stableState) {
    if (statsRungAhead && now >= statsAtMs) {
      statsRungAhead = false;
      boardResetStats();
      boardShowHoldHint(RUNG_METER);
    }
    if (meterRungAhead && now >= meterAtMs) {
      meterRungAhead = false;
      boardToggleMeter();
      boardShowHoldHint(RUNG_MODE);
    }
    // The top rung repeats: see the file header for why it only selects a
    // mode for the next manual reboot, rather than switching live.
    if (now >= nextCycleMs) {
      advancePendingMode();
      nextCycleMs = now + MODE_HOLD_MS;
    }
  }
}
