/*
 * USB HID Gamepad / Keyboard / Mouse (single active device)
 * -------------------------------------------------------------------------
 * Emulates exactly ONE USB HID device at a time — gamepad ("X" button),
 * keyboard (Space), or mouse (left click) — selected by the board's one
 * button:
 * On a board with a sensor (a screen to put a menu on), the button works
 * in two entirely different ways depending on whether the menu is open:
 *
 *   Normal operation
 *     - Short press: send the active mode's action, and time the
 *       display's response to it.
 *     - Hold 1s: reset the measurement statistics.
 *     - Hold 2s: open the menu. From here on, presses are menu input,
 *       not HID input — see below — until the menu exits on its own.
 *
 *   Inside the menu
 *     - Short press: advance — move the highlighted item, or, inside the
 *       mode picker, advance the candidate mode.
 *     - Hold 1s: trigger the highlighted item's action.
 *   The menu holds the light meter (a live view, toggled instantly),
 *   the automated test (see appStartAutoTest()) and changing the pending
 *   mode (see appAdvancePendingMode()). The first and last used to be
 *   hold-ladder rungs of their own; moving them into a proper menu is
 *   what let short-press-to-advance and hold-to-select behave the same
 *   way at every level, instead of every feature inventing its own hold
 *   duration to remember — and left somewhere obvious to put the third.
 *
 *   While an automated run is going, the button does one thing only:
 *   stop it. Nothing else — no HID report, no hold rungs — because the
 *   act of stopping a run must not land in that run's own data.
 *
 * A board with no sensor (no screen to draw a menu on) never has one:
 * the button stays exactly what it always was — short press sends the
 * action, and holding advances the pending mode one step per
 * MODE_HOLD_MS (3s) for as long as it's held.
 *
 * Note what is NOT part of any of this: the light meter is not a Mode. A
 * Mode is a USB identity, which is why it's stuck until a reboot — and
 * the meter touches no USB state whatsoever, so tying it to one would
 * have cost a reboot for nothing. It's a view the board layer flips at
 * runtime. (Outside the menu, presses keep sending their HID report
 * while the meter view is up, which is exactly what you want when aiming
 * the sensor: press the button, watch the thing you're pointing at
 * change, and watch the bar move. Inside the menu, no press sends HID at
 * all — see above.)
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
#include <esp_random.h>
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

// Normal-operation ladder, board-with-a-sensor only: two one-shot rungs,
// each firing once per hold as the threshold is crossed.
static const uint32_t STATS_HOLD_MS = 1000;  // hold 1s -> reset stats
static const uint32_t MENU_HOLD_MS = 2000;   // hold 2s -> open the menu

// Inside the menu: how long a hold has to be to trigger the highlighted
// item, as opposed to a short press-and-release advancing it instead.
static const uint32_t MENU_SELECT_HOLD_MS = 1000;

// Board-with-no-sensor only: the one original rung, repeating at this
// interval for as long as the button stays held.
static const uint32_t MODE_HOLD_MS = 3000;

// --- Automated test ---------------------------------------------------
// An unattended run of presses in the current mode, started from the
// menu, so a distribution worth reading can be collected without
// standing there tapping a button hundreds of times. At 500 presses and
// a ~200-500ms gap each, a run takes roughly three to five minutes —
// abortable at any point with a press. 500 also splits to ~250 samples
// per direction, comfortably inside the board's HIST_CAPACITY (500 per
// direction) so a whole run stays in the histogram.
static const uint16_t AUTO_TEST_ITERATIONS = 500;
static const uint32_t AUTO_HOLD_MS = 50;      // plausible press length
static const uint32_t AUTO_GAP_MIN_MS = 200;  // gap between releases and the next press
static const uint32_t AUTO_GAP_MAX_MS = 500;
// Hard ceiling on how long an automated press may stay down. Normally the
// measurement gates the release and this is never reached — it only bites
// if a measurement somehow never finishes, and it exists because a stuck
// HID button is a far worse thing to inflict on the host than one lost
// sample. Comfortably above the board's own measurement timeout.
static const uint32_t AUTO_HOLD_MAX_MS = 1000;

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
static uint32_t lastTriggerMs = 0;   // also this press's start time, while held
static uint32_t nextCycleMs = 0;     // no-sensor board only: next mode-cycle rung
static bool cycledThisHold = false;  // true once this hold has advanced the mode at least once

// Normal-operation ladder (board with a sensor only). Each rung fires at
// most once per hold, as the threshold is crossed.
static bool statsRungAhead = false;
static uint32_t statsAtMs = 0;
static bool menuRungAhead = false;
static uint32_t menuAtMs = 0;

// Automated-test sequencer. AUTO_GAP is "waiting to start the next
// press", AUTO_HOLDING is "pressed, waiting to release".
enum AutoPhase : uint8_t { AUTO_OFF, AUTO_GAP, AUTO_HOLDING };
static AutoPhase autoPhase = AUTO_OFF;
static uint16_t autoDone = 0;
static uint32_t autoPressedAtMs = 0;
static uint32_t autoResumeAtMs = 0;
// volatile: set by appStartAutoTest() from the board's UI task on core 0,
// consumed by loop() on core 1.
static volatile bool autoStartRequested = false;
static bool pressAbortedAuto = false;  // this press stopped a run and sends nothing

// Captured once, at the press edge, rather than re-checked live: if a
// hold is the one that opens the menu partway through (crossing
// MENU_HOLD_MS below), its own eventual release must still read as an
// ordinary HID release — not get mistaken for the menu's first
// navigation tap, which is what a live boardMenuActive() check at
// release time would do, since the menu is open by then.
static bool wasMenuActiveAtPress = false;
static bool menuActionTaken = false;  // this hold already triggered the highlighted item

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
static void doAdvancePendingMode(bool firstOfHold) {
  pendingMode = static_cast<Mode>((pendingMode + 1) % MODE_COUNT);
  prefs.putUChar(PREFS_KEY, pendingMode);
  boardShowPending(activeMode, pendingMode, firstOfHold);
}

// Board-with-no-sensor path: one continuous hold can cycle through
// several modes, so firstOfHold tracks whether this is the first
// advance within it (see board_s3zero.cpp for what it does with that).
static void advancePendingMode() {
  doAdvancePendingMode(!cycledThisHold);
  cycledThisHold = true;
}

// Menu-driven path (board with a sensor): each advance is its own
// discrete tap, not a step within a continuous hold, so there is no
// "first of hold" to report.
void appAdvancePendingMode() {
  doAdvancePendingMode(false);
}

// Random rather than fixed, and this is the whole point of the gap: a
// constant interval can alias with the display's refresh cadence, parking
// every press at the same phase within a frame and quietly biasing the
// very distribution this tool exists to measure. 200-500ms also keeps a
// 100-press run to about a minute.
static uint32_t autoGapMs() {
  return AUTO_GAP_MIN_MS + (esp_random() % (AUTO_GAP_MAX_MS - AUTO_GAP_MIN_MS + 1));
}

static void stopAutoTest() {
  // If the run was aborted mid-press, release — otherwise the host is
  // left holding a button nobody is pressing.
  if (autoPhase == AUTO_HOLDING) sendRelease();
  autoPhase = AUTO_OFF;
  boardShowAutoTest(0, 0);
}

void appStartAutoTest() {
  autoStartRequested = true;
}

// Drives the automated run. Called every loop() pass, including while
// the button is released — which is the normal case, since a run happens
// with nobody touching the device.
static void serviceAutoTest(uint32_t now) {
  if (autoStartRequested) {
    // Wait for the button to come up first. The menu item that requests
    // a run is itself selected by a 1s hold, so the button is still down
    // at that moment; starting immediately would fire the first press
    // while the user is still holding, and that press would also be the
    // one that gets debounced against their release.
    if (stableState) return;
    autoStartRequested = false;
    autoDone = 0;
    autoPhase = AUTO_GAP;
    // A normal gap before the first press too, rather than firing the
    // instant the button comes up: the button is the screen face, inches
    // from wherever the sensor is aimed, so letting go of it is exactly
    // the moment not to be taking a reading.
    autoResumeAtMs = now + autoGapMs();
    boardShowAutoTest(0, AUTO_TEST_ITERATIONS);
  }

  switch (autoPhase) {
    case AUTO_OFF:
      return;

    case AUTO_GAP:
      if ((int32_t)(now - autoResumeAtMs) < 0) return;
      {
        // Deliberately the same three lines the manual press path runs,
        // timestamp and ordering included: an automated sample has to
        // measure the same thing a real button press does, or the two
        // can't be compared against each other.
        int64_t edgeMicros = esp_timer_get_time();
        sendPress();
        boardShowPress(true, activeMode, edgeMicros);
      }
      autoPressedAtMs = now;
      autoPhase = AUTO_HOLDING;
      return;

    case AUTO_HOLDING:
      // Hold for a plausible press length, but never release before the
      // measurement this press triggered has finished: the next press
      // would overwrite the t0 that measurement is still timing against.
      if ((now - autoPressedAtMs) < AUTO_HOLD_MS) return;
      if (boardMeasurementBusy() && (now - autoPressedAtMs) < AUTO_HOLD_MAX_MS) return;

      sendRelease();
      boardShowPress(false, activeMode, esp_timer_get_time());
      autoDone++;

      if (autoDone >= AUTO_TEST_ITERATIONS) {
        autoPhase = AUTO_OFF;
        boardShowAutoTest(0, 0);
      } else {
        autoPhase = AUTO_GAP;
        autoResumeAtMs = now + autoGapMs();
        boardShowAutoTest(autoDone, AUTO_TEST_ITERATIONS);
      }
      return;
  }
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
  boardShowHoldHint(RUNG_STATS);
}

void loop() {
  bool raw = boardButtonPressed();
  uint32_t now = millis();

  // Outside the lockout window, any change is a real edge: act on it
  // immediately, then start the lockout to swallow bounce.
  if ((now - lastTriggerMs) >= DEBOUNCE_MS && raw != stableState) {
    // t0 for latency measurement: the edge itself, before the report is
    // queued, so the HID send counts as part of what's being measured.
    // Meaningless when the menu is about to own this press, but cheap
    // enough that skipping it isn't worth the branch.
    int64_t edgeMicros = esp_timer_get_time();
    stableState = raw;
    lastTriggerMs = now;

    if (stableState) {
      // Three mutually exclusive things a press can be, decided here once
      // and remembered for the rest of this hold (see wasMenuActiveAtPress
      // above for why the decision can't be re-made at release time).
      pressAbortedAuto = (autoPhase != AUTO_OFF);
      wasMenuActiveAtPress = !pressAbortedAuto && boardHasSensor() && boardMenuActive();

      if (pressAbortedAuto) {
        // Abort gesture, nothing else: no HID report, no hold ladder, or
        // the act of stopping a run would land in that run's own data.
        stopAutoTest();
      } else if (wasMenuActiveAtPress) {
        // Presses inside the menu are navigation only — no HID report,
        // no measurement. Tap vs. hold-to-select is resolved on release
        // or by the timer below; nothing fires at the press edge itself.
        menuActionTaken = false;
      } else {
        // HID report first, feedback second — never the other way round.
        sendPress();
        boardShowPress(true, activeMode, edgeMicros);
        cycledThisHold = false;
        if (boardHasSensor()) {
          statsRungAhead = true;
          menuRungAhead = true;
          statsAtMs = now + STATS_HOLD_MS;
          menuAtMs = now + MENU_HOLD_MS;
        } else {
          nextCycleMs = now + MODE_HOLD_MS;
        }
      }
    } else {
      if (pressAbortedAuto) {
        // Consumed by the abort. stopAutoTest() already sent whatever
        // release the run itself still owed, so there is nothing here.
      } else if (wasMenuActiveAtPress) {
        // A release before the select threshold fired is a tap; one
        // that already fired (menuActionTaken) needs nothing further —
        // see boardMenuSelect() below.
        if (!menuActionTaken) boardMenuTap();
      } else {
        sendRelease();
        boardShowPress(false, activeMode, edgeMicros);
        statsRungAhead = menuRungAhead = false;
        boardShowHoldHint(RUNG_STATS);
      }
    }
  }

  // Runs whether or not the button is down — a run proceeds with nobody
  // touching the device, so this has to come before the released-early-out.
  serviceAutoTest(now);

  if (!stableState) return;

  if (pressAbortedAuto) return;  // this hold's only job was stopping the run

  if (wasMenuActiveAtPress) {
    if (!menuActionTaken && (now - lastTriggerMs) >= MENU_SELECT_HOLD_MS) {
      menuActionTaken = true;
      boardMenuSelect();
    }
    return;
  }

  if (boardHasSensor()) {
    // Two one-shot rungs; nothing repeats after them — continuing to
    // hold past MENU_HOLD_MS just waits for release, since the menu
    // (already open by then) is what the rest of the hold belongs to.
    if (statsRungAhead && now >= statsAtMs) {
      statsRungAhead = false;
      boardResetStats();
      boardShowHoldHint(RUNG_MENU);
    }
    if (menuRungAhead && now >= menuAtMs) {
      menuRungAhead = false;
      boardEnterMenu();
    }
  } else {
    // No sensor, no menu: the one original rung, repeating for as long
    // as the button stays down.
    if (now >= nextCycleMs) {
      advancePendingMode();
      nextCycleMs = now + MODE_HOLD_MS;
    }
  }
}
