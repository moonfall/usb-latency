/*
 * Board layer for the Waveshare ESP32-S3-Zero: BOOT button (GPIO0,
 * active-low) as the input, onboard WS2812 (GPIO21) as the indicator.
 *
 * The LED encodes the mode as a colour (red/green/blue) and is only lit
 * while the button is held. Modes are otherwise invisible on this board,
 * which is why boot flashes the active mode's colour briefly — without it
 * you'd have to press the button to find out what the device is.
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

static inline void showModeColor(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  setPixel(40, 0, 0); break;  // red
    case MODE_KEYBOARD: setPixel(0, 40, 0); break;  // green
    case MODE_MOUSE:    setPixel(0, 0, 40); break;  // blue
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

void boardShowPress(bool pressed, Mode active, int64_t atMicros) {
  (void)atMicros;  // nothing here to measure the machine's response with
  if (pressed) {
    showModeColor(active);
  } else {
    setPixel(0, 0, 0);
  }
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
