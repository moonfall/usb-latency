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

bool boardButtonPressed() {
  return digitalRead(BOOT_BUTTON_PIN) == LOW;  // active-low
}

void boardShowBoot(Mode active, Mode pending) {
  (void)pending;  // a single LED can't usefully show both at once
  showModeColor(active);
  delay(BOOT_FLASH_MS);
  setPixel(0, 0, 0);
}

void boardShowPress(bool pressed, Mode active) {
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
