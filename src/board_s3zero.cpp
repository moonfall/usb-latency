/*
 * Board layer for the Waveshare ESP32-S3-Zero: BOOT button (GPIO0,
 * active-low) as the input, onboard WS2812 (GPIO21) as the indicator.
 *
 * The LED encodes the mode as a colour and is only lit while the button
 * is held. Modes are otherwise invisible on this board, which is why boot
 * flashes the active mode's colour briefly — without it you'd have to
 * press the button to find out what the device is.
 *
 * Six modes and one RGB LED means the colours are the three primaries for
 * the USB family and the three secondaries for the BLE one:
 *
 *   GAMEPAD  red      BLE GAMEPAD   yellow
 *   KEYBOARD green    BLE KEYBOARD  cyan
 *   MOUSE    blue     BLE MOUSE     magenta
 *
 * The pairing is arbitrary — there is no colour that means "the same
 * thing but over the radio" — but six distinguishable colours on a device
 * with no other output is worth more than a mnemonic. The table above is
 * the reference; it is also in CLAUDE.md.
 *
 * BLE on this board has no pairing gesture and no link display, which is
 * a deliberate refusal rather than an omission. See boardShowLink() below
 * for the whole argument, and note the one thing the LED does say: a
 * press that went nowhere (a BLE mode with no host listening) lights the
 * mode colour DIM instead of full, so "nothing happened" and "nothing
 * could have happened" are not the same picture.
 *
 * Do not press/hold BOOT while plugging in via a normal (non-flashing)
 * USB session; GPIO0 low at reset is how the ROM bootloader is asked for
 * download mode.
 */

#include <Arduino.h>

#include "board.h"

static const uint8_t BOOT_BUTTON_PIN = 0;   // GPIO0 on the S3-Zero
static const uint8_t RGB_LED_PIN = 21;      // onboard WS2812
static const uint32_t MODE_SWITCH_FLASH_MS = 500;  // white flash on the first cycle of a hold
static const uint32_t BOOT_FLASH_MS = 200;  // active-mode colour flash on boot

// This board's onboard WS2812 shows red where rgbLedWrite()'s green_val
// argument is nonzero and green where its red_val argument is nonzero —
// i.e. its red/green channels are swapped relative to the library's
// assumed wire order. blue_val is unaffected. Swap red/green here so
// every other call site can use normal, intuitive (r, g, b) values.
static inline void setPixel(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(RGB_LED_PIN, g, r, b);
}

// `level` is the per-channel value a lit channel gets: LEVEL_ON normally,
// LEVEL_DIM for a press in a BLE mode that had nowhere to go. Dim rather
// than off, and rather than some fourth colour, because the useful
// message is "this press, in this mode, reached nobody" — keeping the hue
// keeps the mode readable while the brightness carries the rest.
static const uint8_t LEVEL_ON = 40;
static const uint8_t LEVEL_DIM = 5;

static inline void showModeColor(Mode mode, uint8_t level = LEVEL_ON) {
  switch (mode) {
    case MODE_GAMEPAD:      setPixel(level, 0, 0); break;      // red
    case MODE_KEYBOARD:     setPixel(0, level, 0); break;      // green
    case MODE_MOUSE:        setPixel(0, 0, level); break;      // blue
    case MODE_BLE_GAMEPAD:  setPixel(level, level, 0); break;  // yellow
    case MODE_BLE_KEYBOARD: setPixel(0, level, level); break;  // cyan
    case MODE_BLE_MOUSE:    setPixel(level, 0, level); break;  // magenta
    default: break;
  }
}

void boardBegin() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
}

// No Grove port and no screen, so there is nothing to measure with and
// nowhere to put a reading. The hold ladder collapses to its one original
// rung, and the menu functions below are all no-ops since there's no
// screen to draw one on.
bool boardHasSensor() {
  return false;
}

void boardResetStats() {}

void boardShowHoldHint(HoldRung next) {
  (void)next;  // one rung, and a single LED can't spell it out anyway
}

// No screen, so no menu to open — the button stays exactly what it
// always was: short press sends the action, holding cycles the pending
// mode. main.cpp checks boardHasSensor() before ever asking about the
// menu, but these are still needed to satisfy the shared board.h
// contract that board_atoms3r.cpp implements for real.
bool boardMenuActive() { return false; }
void boardEnterMenu() {}
void boardMenuTap() {}
void boardMenuSelect() {}

// No sensor means no measurement to be busy with, and no menu means the
// automated test can never be started here in the first place.
bool boardMeasurementBusy() { return false; }
void boardShowFullTest(uint8_t step, uint8_t total) {
  (void)step;
  (void)total;  // no screen; and with no sensor, no full test ever starts here
}
void boardShowAutoTest(uint16_t done, uint16_t total) {
  (void)done;
  (void)total;
}

bool boardButtonPressed() {
  return digitalRead(BOOT_BUTTON_PIN) == LOW;  // active-low
}

void boardShowBoot(Mode active, Mode pending) {
  (void)pending;  // a single LED can't usefully show both at once
  showModeColor(active);
  delay(BOOT_FLASH_MS);
  setPixel(0, 0, 0);
}

void boardShowPress(bool pressed, Mode active, int64_t atMicros, bool sent) {
  (void)atMicros;  // nothing here to measure the machine's response with
  if (pressed) {
    // The only use this board has for `sent`: a BLE press with no host
    // listening lights dim rather than full. There is no measurement to
    // suppress here (no sensor), so that is the whole of it.
    showModeColor(active, sent ? LEVEL_ON : LEVEL_DIM);
  } else {
    setPixel(0, 0, 0);
  }
}

// Nothing to show it on, and deliberately so.
//
// The temptation is to light the LED continuously in a BLE mode to say
// "connected" — and it would be wrong twice over. The LED is lit only
// while the button is held, which is what makes it readable at all on a
// board whose entire output is one pixel; turning it into a status lamp
// would cost that and gain a second, contradictory meaning for the same
// colour. And the state it would report is the one the next press
// reports anyway, more usefully, via `sent` above.
//
// Pairing, likewise, has no gesture here. This board's BLE story is
// simply: it advertises from boot, it re-advertises whenever a host goes
// away, and any host that has not bonded with it can pair. That covers
// every case except a stale bond — a host that has forgotten the device
// while the device still holds a key for it — for which the way out is
// to forget the device on the host, or, failing that, to erase flash.
// The alternative was inventing a second hold gesture on a button that
// already carries press-to-send and hold-to-cycle-mode, on a board with
// no way to show that the gesture had been recognised. One gesture more
// than the indicator can explain is worse than none.
void boardShowLink(LinkState state) {
  (void)state;
}

void boardShowPending(Mode active, Mode pending, bool firstOfHold) {
  (void)active;
  // Flash white once, on the first cycle of a given hold, so holding
  // through several modes shows one flash and then direct colour-to-colour
  // jumps rather than a strobe.
  if (firstOfHold) {
    setPixel(40, 40, 40);
    delay(MODE_SWITCH_FLASH_MS);
  }
  showModeColor(pending);
}

// No sensor means no sample ever reaches main.cpp from here, and no menu
// means no automated run can be started in the first place — so this can
// only ever be called with an empty run, and there is no filesystem on
// this board's 4MB layout to put one in anyway. Same shape as the menu
// no-ops above: present to satisfy the shared board.h contract that
// board_atoms3r.cpp implements for real.
void boardWriteRun(const RunRecord &run) {
  (void)run;
}

// This board's 4MB layout is stock default.csv — one app, no data
// partition of any kind — so there is nothing here to hand a host, and
// MODE_STORAGE is unreachable by construction. boardHasStorage() saying
// so is what main.cpp gates on before it even reads the persisted
// armed flag, so a device that somehow carried one over in NVS still
// comes up as the HID mode it was left in rather than as a drive with no
// blocks behind it.
bool boardHasStorage() {
  return false;
}

bool boardStorageBegin(uint32_t *blockCount, uint16_t *blockSize) {
  (void)blockCount;
  (void)blockSize;
  return false;
}

int32_t boardStorageRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  (void)lba;
  (void)offset;
  (void)buffer;
  (void)size;
  return -1;  // no medium; the SCSI command fails rather than reads zeros
}

int32_t boardStorageWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t size) {
  (void)lba;
  (void)offset;
  (void)buffer;
  (void)size;
  return -1;
}

void boardShowStorageEjected() {}
