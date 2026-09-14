/*
 * Board layer for the M5Stack AtomS3R: the screen button (GPIO41,
 * active-low — the whole LCD face is the button) as the input, the 0.85"
 * 128x128 LCD behind it as the indicator, and an M5Stack Unit Light on
 * the Grove port as the response sensor.
 *
 * A press always starts a clock at the button edge and stops it when the
 * sensor crosses LIGHT_THRESHOLD — i.e. it times button-down all the way
 * through USB, the host, the compositor and the panel, to light actually
 * changing. That happens in every mode and regardless of which view is
 * up; all the meter view changes is what the top of the screen shows:
 *
 *   measure view  The last measurement plus a running count / minimum /
 *                 mean. The default, and what you watch while testing.
 *
 *   meter view    The live sensor reading, as a number and as a bar with
 *                 the threshold ticked on it. For aiming the sensor at
 *                 the spot that will change and confirming the change
 *                 really does cross the threshold. Presses still send
 *                 their HID report here, so you can make the display do
 *                 its thing and watch the bar move while you aim.
 *
 * The view toggles live, on a 4s hold — it touches no USB state, so
 * unlike a Mode it costs no reboot. Measurements keep running and keep
 * accumulating underneath the meter, so switching back shows the stats
 * they built up.
 *
 * Layout, top to bottom:
 *
 *     24.38 ms   |  2145  1.73V      <- measurement, or the live reading
 *   n12 lo21.4 av24.9 | [==|====]    <- stats, or the bar + threshold tick
 *      GAMEPAD                       <- mode this boot enumerated as
 *   [ "X" button ]                   <- what a press sends; lit while held
 *    hold: reset stats               <- next hold rung, or pending mode
 *
 * NOTHING here draws from loop(). All panel access, all ADC sampling and
 * the whole measurement loop happen in one task pinned to core 0
 * (`uiTaskFn`), while loop() runs on core 1 — so none of it can stretch a
 * loop iteration and push out the moment the next button edge is noticed.
 * The board entry points called from loop() only store a value and poke
 * the task: xTaskNotifyGive() is a few microseconds and, because the task
 * it wakes is on the other core at a lower priority than USB, it cannot
 * preempt anything on core 1.
 *
 * That poke is also what keeps press feedback prompt despite the 100ms
 * metering period: the task blocks in ulTaskNotifyTake() with a
 * LIGHT_PERIOD_MS timeout, so it wakes either on a press/release edge or
 * on the timeout.
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
#include <esp_timer.h>

#include "board.h"

static const uint8_t SCREEN_BUTTON_PIN = 41;  // the LCD face is the button
static const uint8_t BRIGHTNESS = 160;

// --- Unit Light (U012: photoresistor + LM393) on the Grove port --------
// The unit's two signal wires are yellow = digital (comparator output,
// thresholded by the pot on the unit) and white = analog. On a Port A
// Grove connector yellow is the SDA line and white is the SCL line, and
// on the AtomS3R those are GPIO2 and GPIO1 respectively — so the analog
// output lands on GPIO1, which is ADC1_CH0. Only the analog side is read
// here; the digital side is left alone.
static const uint8_t LIGHT_ANALOG_PIN = 1;
static const int ADC_MAX = 4095;              // 12-bit, the Arduino default
static const uint32_t LIGHT_PERIOD_MS = 100;  // metering cadence in the meter view
static const int LIGHT_DEADBAND = 8;          // counts of ADC noise not worth a repaint

// The level the reading has to cross for a measurement to stop. Override
// from platformio.ini with -DLIGHT_THRESHOLD=<counts> if 3000 doesn't
// sit between your display's two states; the meter view draws it on the bar
// so you can see where it falls.
#ifndef LIGHT_THRESHOLD
#define LIGHT_THRESHOLD 3000
#endif
static const int lightThreshold = LIGHT_THRESHOLD;

// How long to wait for the display to respond before giving up. Also the
// longest this task can hold core 0 without letting its idle task run —
// keep it well under the 5s task watchdog.
static const uint32_t MEASURE_TIMEOUT_MS = 500;

// --- Layout (the panel is 128x128) -------------------------------------
static const int SCREEN_W = 128;
static const int TOP_TEXT_Y = 1;   // Font2, 16 tall
static const int TOP_SUB_Y = 20;   // Font0, 8 tall / the bar
static const int BAR_X = 4;
static const int BAR_W = SCREEN_W - 2 * BAR_X;
static const int BAR_H = 7;
static const int MODE_Y = 45;
static const int BOX_X = 6;
static const int BOX_Y = 62;
static const int BOX_W = SCREEN_W - 2 * BOX_X;
static const int BOX_H = 32;
static const int BOX_R = 8;
static const int FOOT1_Y = 100;
static const int FOOT2_Y = 112;

static M5GFX display;
static bool displayReady = false;
static TaskHandle_t uiTask = nullptr;

// Written from loop() (core 1), read by uiTask (core 0). All are single
// aligned words, so a torn read isn't possible; the task re-reads them
// every pass and redraws whatever no longer matches the screen.
static volatile bool wantPressed = false;
static volatile Mode wantActive = MODE_GAMEPAD;
static volatile Mode wantPending = MODE_GAMEPAD;
static volatile int64_t pressMicros = 0;
static volatile bool measurePending = false;
static volatile bool resetRequested = false;
static volatile bool meterToggleRequested = false;
static volatile HoldRung wantHint = RUNG_STATS;

// Measurement results. Touched only by uiTask, so no synchronisation.
static const int32_t LAT_NONE = -1;     // nothing measured yet
static const int32_t LAT_TIMEOUT = -2;  // the light never crossed
static int32_t lastLatencyUs = LAT_NONE;
static uint32_t measCount = 0;
static uint32_t measMinUs = 0;
static uint64_t measSumUs = 0;

// Which of the two top-strip views is up. Owned by uiTask, flipped only
// in response to meterToggleRequested.
static bool meterView = false;

static uint16_t black() { return display.color565(0, 0, 0); }

static uint16_t modeColor(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return display.color565(255,  72,  56);  // red
    case MODE_KEYBOARD: return display.color565( 48, 214,  96);  // green
    case MODE_MOUSE:    return display.color565( 72, 150, 255);  // blue
    default:            return display.color565(255, 255, 255);
  }
}

// Times a press through to the display responding to it. t0 is when
// loop() saw the button edge, on the other core, before the HID report
// was even queued — so the returned figure covers the whole chain.
//
// The direction of the crossing is taken from a baseline sampled here
// rather than configured: whichever side of the threshold the sensor
// starts on, the clock stops on the first sample that has reached the
// other side. So a dark screen flashing bright and a bright screen going
// dark both work, with the one threshold value.
//
// Deliberately a tight poll with no yield — analogRead() is tens of
// microseconds, so this resolves far finer than the millisecond, and
// MEASURE_TIMEOUT_MS bounds how long core 0's idle task goes unserviced.
// loop() is on core 1 and is untouched throughout; the USB task sits at a
// higher priority and still preempts this freely.
static void runMeasurement(int64_t t0) {
  int baseline = analogRead(LIGHT_ANALOG_PIN);
  bool waitForRise = baseline < lightThreshold;
  int64_t deadline = t0 + (int64_t)MEASURE_TIMEOUT_MS * 1000;

  for (;;) {
    int v = analogRead(LIGHT_ANALOG_PIN);
    int64_t now = esp_timer_get_time();

    if (waitForRise ? (v >= lightThreshold) : (v < lightThreshold)) {
      uint32_t us = (uint32_t)(now - t0);
      lastLatencyUs = (int32_t)us;
      if (measCount == 0 || us < measMinUs) measMinUs = us;
      measSumUs += us;
      measCount++;
      return;
    }
    if (now >= deadline) {
      lastLatencyUs = LAT_TIMEOUT;
      return;
    }
  }
}

// The meter view's top strip: the raw ADC count and its voltage, plus the
// same value as a bar with the threshold ticked on it, so the sensor can
// be aimed and the threshold sanity-checked at a glance.
static void drawLightStrip(int raw, uint32_t mv) {
  uint16_t fg = display.color565(120, 220, 230);
  uint16_t track = display.color565(40, 40, 40);
  uint16_t tickColor = display.color565(255, 190, 40);

  char buf[24];
  snprintf(buf, sizeof(buf), "%4d  %u.%02uV", raw,
           (unsigned)(mv / 1000), (unsigned)((mv % 1000) / 10));

  display.setFont(&fonts::Font2);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(fg, black());
  display.setTextPadding(SCREEN_W);
  display.drawString(buf, SCREEN_W / 2, TOP_TEXT_Y);
  display.setTextPadding(0);

  int fill = (raw * BAR_W) / ADC_MAX;
  if (fill < 0) fill = 0;
  if (fill > BAR_W) fill = BAR_W;
  display.fillRect(BAR_X, TOP_SUB_Y, fill, BAR_H, fg);
  display.fillRect(BAR_X + fill, TOP_SUB_Y, BAR_W - fill, BAR_H, track);

  int tick = BAR_X + (lightThreshold * BAR_W) / ADC_MAX;
  display.drawFastVLine(tick, TOP_SUB_Y - 2, BAR_H + 4, tickColor);
}

// The measure view's top strip: the last measurement, and a running
// count / minimum / mean underneath it. One sample of a latency chain is
// close to meaningless on its own, so the summary earns its line.
static void drawLatencyStrip() {
  uint16_t fg = display.color565(120, 220, 230);
  uint16_t dim = display.color565(110, 110, 110);
  uint16_t warn = display.color565(255, 140, 60);

  char top[24];
  char sub[32];
  uint16_t topColor = fg;

  if (lastLatencyUs == LAT_NONE) {
    snprintf(top, sizeof(top), "-- ms");
    topColor = dim;
  } else if (lastLatencyUs == LAT_TIMEOUT) {
    snprintf(top, sizeof(top), "no change");
    topColor = warn;
  } else {
    uint32_t us = (uint32_t)lastLatencyUs;
    snprintf(top, sizeof(top), "%u.%02u ms", us / 1000, (us % 1000) / 10);
  }

  if (measCount == 0) {
    snprintf(sub, sizeof(sub), "press to measure");
  } else {
    uint32_t avgUs = (uint32_t)(measSumUs / measCount);
    snprintf(sub, sizeof(sub), "n%u lo%u.%u av%u.%u", (unsigned)measCount,
             measMinUs / 1000, (measMinUs % 1000) / 100,
             avgUs / 1000, (avgUs % 1000) / 100);
  }

  display.setTextDatum(textdatum_t::top_center);
  display.setTextPadding(SCREEN_W);  // clears its own line, so no flicker

  display.setFont(&fonts::Font2);
  display.setTextColor(topColor, black());
  display.drawString(top, SCREEN_W / 2, TOP_TEXT_Y);

  display.setFont(&fonts::Font0);
  display.setTextColor(dim, black());
  display.drawString(sub, SCREEN_W / 2, TOP_SUB_Y);

  display.setTextPadding(0);
}

static void drawTopStrip(int raw, uint32_t mv) {
  if (meterView) {
    drawLightStrip(raw, mv);
  } else {
    drawLatencyStrip();
  }
}

// The action box: outlined when idle, filled with the mode colour while
// the button is held. Redrawn on its own for press/release, so a press
// doesn't cost a full-screen repaint.
static void drawActionBox(Mode active, bool pressed) {
  uint16_t color = modeColor(active);

  if (pressed) {
    display.fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, color);
    display.setTextColor(black(), color);
  } else {
    display.fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, black());
    display.drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, color);
    display.setTextColor(color, black());
  }

  display.setFont(&fonts::Font2);
  display.setTextDatum(textdatum_t::middle_center);
  display.drawString(modeAction(active), SCREEN_W / 2, BOX_Y + BOX_H / 2);
}

// Footer: either the mode change already queued up and waiting on a
// reboot (see main.cpp for why it has to wait), which outranks everything
// because it's the one thing needing action elsewhere — or else what
// carrying on holding the button would do next. Drawn on its own so the
// hint can change mid-hold without a full repaint.
static void drawFooter(Mode active, Mode pending, HoldRung hint) {
  uint16_t dim = display.color565(110, 110, 110);
  uint16_t amber = display.color565(255, 190, 40);

  display.fillRect(0, FOOT1_Y, SCREEN_W, SCREEN_W - FOOT1_Y, black());
  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);

  if (pending != active) {
    display.setTextColor(amber, black());
    display.drawString(modeName(pending), SCREEN_W / 2, FOOT1_Y);
    display.setTextColor(dim, black());
    display.drawString("on next reset", SCREEN_W / 2, FOOT2_Y);
    return;
  }

  const char *text = "";
  switch (hint) {
    case RUNG_STATS: text = "hold: reset stats"; break;
    case RUNG_METER: text = meterView ? "hold: latency view" : "hold: light meter"; break;
    case RUNG_MODE:  text = "hold: next mode"; break;
  }
  display.setTextColor(dim, black());
  display.drawString(text, SCREEN_W / 2, FOOT1_Y);
}

static void drawFrame(Mode active, Mode pending, bool pressed, int raw, uint32_t mv,
                      HoldRung hint) {
  display.startWrite();
  display.fillScreen(black());

  drawTopStrip(raw, mv);

  // The mode name is the headline. Font4 fits all three names at this
  // width, but fall back a size rather than clip if that ever changes.
  display.setTextDatum(textdatum_t::middle_center);
  display.setTextColor(modeColor(active), black());
  display.setFont(&fonts::Font4);
  if (display.textWidth(modeName(active)) > SCREEN_W - 8) {
    display.setFont(&fonts::Font2);
  }
  display.drawString(modeName(active), SCREEN_W / 2, MODE_Y);

  drawActionBox(active, pressed);
  drawFooter(active, pending, hint);

  display.endWrite();
}

// Everything that touches the panel, the ADC or the clock runs here, on
// core 0.
static void uiTaskFn(void *) {
  // Deliberately not in setup(): panel autodetect probes SPI and brings
  // the backlight up over I2C, and none of that belongs on the core that
  // is about to start polling the button.
  displayReady = display.init();
  if (displayReady) display.setBrightness(BRIGHTNESS);
  analogSetPinAttenuation(LIGHT_ANALOG_PIN, ADC_11db);  // full ~0-3.3V span

  // What the screen currently shows, so only real changes are repainted.
  Mode shownActive = MODE_COUNT;  // MODE_COUNT != any real mode, forcing
  Mode shownPending = MODE_COUNT; // the first pass to draw a full frame
  bool shownPressed = false;
  bool shownMeter = false;
  HoldRung shownHint = RUNG_STATS;
  int shownRaw = -1;

  int raw = 0;
  uint32_t mv = 0;
  uint32_t lastSampleMs = 0;

  for (;;) {
    Mode active = wantActive;
    Mode pending = wantPending;
    bool pressed = wantPressed;
    HoldRung hint = wantHint;

    // Requests from loop() on the other core. Both are deferred to here
    // so that the stats and the view stay single-threaded on core 0.
    bool statsCleared = false;
    if (resetRequested) {
      resetRequested = false;
      lastLatencyUs = LAT_NONE;
      measCount = 0;
      measMinUs = 0;
      measSumUs = 0;
      statsCleared = true;
    }
    if (meterToggleRequested) {
      meterToggleRequested = false;
      meterView = !meterView;
    }

    bool newFrame = (active != shownActive || pending != shownPending ||
                     meterView != shownMeter);
    // A clear only shows up in the measure view; the meter view is live
    // anyway and will repaint on its own cadence.
    bool topDirty = statsCleared && !meterView;

    if (newFrame) shownRaw = -1;  // whatever is cached belongs to the old view

    // Measure BEFORE drawing anything. A repaint here would delay the
    // first sample by a millisecond or two of SPI, and — much worse —
    // could straddle the very change being timed. This runs in the meter
    // view too: the measurement is what the press is for either way.
    if (measurePending) {
      measurePending = false;
      runMeasurement(pressMicros);
      if (!meterView) topDirty = true;
    }

    if (meterView) {
      uint32_t now = millis();
      if (shownRaw < 0 || (uint32_t)(now - lastSampleMs) >= LIGHT_PERIOD_MS) {
        lastSampleMs = now;
        raw = analogRead(LIGHT_ANALOG_PIN);
        mv = analogReadMilliVolts(LIGHT_ANALOG_PIN);
        // Repaint only once the reading has actually moved — the bottom
        // few ADC counts are noise, and repainting on every one of them
        // would just make the number flicker.
        if (shownRaw < 0 || abs(raw - shownRaw) > LIGHT_DEADBAND) topDirty = true;
      }
    }

    if (displayReady) {
      if (newFrame) {
        drawFrame(active, pending, pressed, raw, mv, hint);
        shownActive = active;
        shownPending = pending;
        shownPressed = pressed;
        shownMeter = meterView;
        shownHint = hint;
        shownRaw = raw;
      } else {
        if (pressed != shownPressed) {
          display.startWrite();
          drawActionBox(active, pressed);
          display.endWrite();
          shownPressed = pressed;
        }
        if (topDirty) {
          display.startWrite();
          drawTopStrip(raw, mv);
          display.endWrite();
          shownRaw = raw;
        }
        if (hint != shownHint) {
          display.startWrite();
          drawFooter(active, pending, hint);
          display.endWrite();
          shownHint = hint;
        }
      }
    }

    // Wake on the next press/release edge or hold rung, or after
    // LIGHT_PERIOD_MS to re-meter the sensor — whichever comes first.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LIGHT_PERIOD_MS));
  }
}

static inline void nudgeUi() {
  if (uiTask) xTaskNotifyGive(uiTask);
}

void boardBegin() {
  pinMode(SCREEN_BUTTON_PIN, INPUT_PULLUP);
}

// Light sensor on the Grove port plus a screen to put the answer on, so
// this board gets the full hold ladder.
bool boardHasSensor() {
  return true;
}

void boardResetStats() {
  resetRequested = true;
  nudgeUi();
}

void boardToggleMeter() {
  meterToggleRequested = true;
  nudgeUi();
}

void boardShowHoldHint(HoldRung next) {
  wantHint = next;
  nudgeUi();
}

bool boardButtonPressed() {
  return digitalRead(SCREEN_BUTTON_PIN) == LOW;  // active-low
}

void boardShowBoot(Mode active, Mode pending) {
  wantActive = active;
  wantPending = pending;
  wantPressed = false;
  // Core 0: loop() has core 1 (ARDUINO_RUNNING_CORE=1) to itself. Priority
  // 1 matches the Arduino loop task and stays below the USB task, so
  // neither the HID path nor enumeration can be held up by a repaint or
  // by a measurement.
  xTaskCreatePinnedToCore(uiTaskFn, "atoms3r-ui", 6144, nullptr, 1, &uiTask, 0);
}

void boardShowPress(bool pressed, Mode active, int64_t atMicros) {
  wantActive = active;
  if (pressed) {
    // Hand the edge timestamp over and let the task start its clock from
    // it. Note the consequence: in a measuring mode the box doesn't light
    // up until the measurement finishes, because the task must not be
    // painting while it is sampling. That's the measurement duration —
    // tens of milliseconds normally, MEASURE_TIMEOUT_MS at worst.
    pressMicros = atMicros;
    measurePending = true;
  }
  wantPressed = pressed;
  nudgeUi();
}

void boardShowPending(Mode active, Mode pending, bool firstOfHold) {
  (void)firstOfHold;  // the footer changing is cue enough on a screen
  wantActive = active;
  wantPending = pending;
  nudgeUi();
}
