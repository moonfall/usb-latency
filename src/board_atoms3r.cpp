/*
 * Board layer for the M5Stack AtomS3R: the screen button (GPIO41,
 * active-low — the whole LCD face is the button) as the input, and the
 * 0.85" 128x128 LCD behind it as the indicator.
 *
 * Unlike the S3-Zero's single LED, the screen can show the mode
 * permanently, so it does — there's no need to press anything to find
 * out what the device currently is. The layout, top to bottom:
 *
 *     USB LATENCY          <- fixed title
 *      GAMEPAD             <- mode this boot enumerated as, in its colour
 *   [ "X" button ]         <- what a press sends; lights up while held
 *    hold 3s: next mode    <- hint, or the pending mode + "press reset"
 *
 * The button is read straight off GPIO41 rather than through M5Unified's
 * M5.BtnA: M5Unified debounces on a trailing edge (10ms by default),
 * which would add exactly the kind of latency this project exists to
 * measure. M5GFX is used on its own for the display — it autodetects the
 * AtomS3R panel (GC9107 or ST7735S, depending on batch) and drives the
 * backlight through the board's LP5562, neither of which is worth
 * reimplementing here.
 */

#include <Arduino.h>
#include <M5GFX.h>

#include "board.h"

static const uint8_t SCREEN_BUTTON_PIN = 41;  // the LCD face is the button
static const uint8_t BRIGHTNESS = 160;

// Layout (the panel is 128x128).
static const int SCREEN_W = 128;
static const int BOX_X = 6;
static const int BOX_Y = 60;
static const int BOX_W = SCREEN_W - 2 * BOX_X;
static const int BOX_H = 34;
static const int BOX_R = 8;

static M5GFX display;
static bool displayReady = false;
static bool heldNow = false;  // so a redraw mid-hold keeps the box lit

static uint16_t modeColor(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return display.color565(255,  72,  56);  // red
    case MODE_KEYBOARD: return display.color565( 48, 214,  96);  // green
    case MODE_MOUSE:    return display.color565( 72, 150, 255);  // blue
    default:            return display.color565(255, 255, 255);
  }
}

// The action box: outlined when idle, filled with the mode colour while
// the button is held. Redrawn on its own for press/release, so a press
// doesn't cost a full-screen repaint.
static void drawActionBox(Mode active, bool pressed) {
  uint16_t color = modeColor(active);
  uint16_t black = display.color565(0, 0, 0);

  if (pressed) {
    display.fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, color);
    display.setTextColor(black, color);
  } else {
    display.fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, black);
    display.drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, color);
    display.setTextColor(color, black);
  }

  display.setFont(&fonts::Font2);
  display.setTextDatum(textdatum_t::middle_center);
  display.drawString(modeAction(active), SCREEN_W / 2, BOX_Y + BOX_H / 2);
}

static void drawFrame(Mode active, Mode pending) {
  uint16_t black = display.color565(0, 0, 0);
  uint16_t dim = display.color565(110, 110, 110);
  uint16_t amber = display.color565(255, 190, 40);

  display.startWrite();
  display.fillScreen(black);

  display.setTextDatum(textdatum_t::top_center);
  display.setFont(&fonts::Font2);
  display.setTextColor(dim, black);
  display.drawString("USB LATENCY", SCREEN_W / 2, 3);

  // The mode name is the headline. Font4 fits all three names at this
  // width, but fall back a size rather than clip if that ever changes.
  display.setTextDatum(textdatum_t::middle_center);
  display.setTextColor(modeColor(active), black);
  display.setFont(&fonts::Font4);
  if (display.textWidth(modeName(active)) > SCREEN_W - 8) {
    display.setFont(&fonts::Font2);
  }
  display.drawString(modeName(active), SCREEN_W / 2, 40);

  drawActionBox(active, heldNow);

  // Footer: either how to change the mode, or the change already queued
  // up and waiting on a reboot (see main.cpp for why it has to wait).
  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  if (pending == active) {
    display.setTextColor(dim, black);
    display.drawString("hold 3s: next mode", SCREEN_W / 2, 104);
  } else {
    display.setTextColor(amber, black);
    display.drawString(modeName(pending), SCREEN_W / 2, 102);
    display.setTextColor(dim, black);
    display.drawString("on next reset", SCREEN_W / 2, 114);
  }

  display.endWrite();
}

void boardBegin() {
  pinMode(SCREEN_BUTTON_PIN, INPUT_PULLUP);
}

bool boardButtonPressed() {
  return digitalRead(SCREEN_BUTTON_PIN) == LOW;  // active-low
}

void boardShowBoot(Mode active, Mode pending) {
  // Deliberately not in boardBegin(): panel autodetect probes SPI and
  // brings up the backlight over I2C, and none of that should sit
  // between power-up and USB.begin() delaying enumeration.
  displayReady = display.init();
  if (!displayReady) return;

  display.setBrightness(BRIGHTNESS);
  drawFrame(active, pending);
}

void boardShowPress(bool pressed, Mode active) {
  heldNow = pressed;
  if (!displayReady) return;

  display.startWrite();
  drawActionBox(active, pressed);
  display.endWrite();
}

void boardShowPending(Mode active, Mode pending, bool firstOfHold) {
  (void)firstOfHold;  // the footer changing is cue enough on a screen
  if (!displayReady) return;

  drawFrame(active, pending);
}
