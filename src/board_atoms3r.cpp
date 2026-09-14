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
 *   measure view  The last measurement plus running stats, kept as two
 *                 separate populations — R for a rise through
 *                 LIGHT_THRESHOLD, F for a fall — rather than one pooled
 *                 average. Bucketing by direction rather than averaging
 *                 them together is deliberate: a photoresistor's rise and
 *                 fall response times are not generally equal, so if the
 *                 two populations differ noticeably, that asymmetry is
 *                 itself information about the sensor, not the thing
 *                 being measured. Which of R/F corresponds to your
 *                 display's dark->light vs light->dark isn't asserted
 *                 here — watch which way the bar moves in the meter view
 *                 below to find out for your setup. The default view, and
 *                 what you watch while testing.
 *
 *   meter view    The live sensor reading, as a number and as a bar with
 *                 the threshold ticked on it, in place of the measure
 *                 view's top two lines only — everything below (mode,
 *                 pending mode, histogram) still shows as normal. For
 *                 aiming the sensor at the spot that will change and
 *                 confirming the change really does cross the threshold.
 *                 Presses still send their HID report here, so you can
 *                 make the display do its thing and watch the bar move
 *                 while you aim.
 *
 * The view toggles live, on a 4s hold — it touches no USB state, so
 * unlike a Mode it costs no reboot. Measurements keep running and keep
 * accumulating underneath the meter, so switching back shows the stats
 * and histogram they built up.
 *
 * Layout, top to bottom:
 *
 *     24.38 ms                <- last measurement, coloured by direction
 *   R6 24.9  F7 41.2           <- per-direction n/mean
 *      GAMEPAD                 <- mode this boot enumerated as
 *   hold: reset stats          <- next hold rung, or the pending mode
 *   [ histogram, R/F stacked,  <- last HIST_CAPACITY samples of each
 *     min/max labelled ]          direction, same colour scheme as above
 *
 * There is no separate action box or press indicator: this board has no
 * accessible RGB LED (see the gotcha in CLAUDE.md — the LP5562 exists and
 * drives the LCD backlight, but its R/G/B channels reach nothing usable),
 * and the on-screen box that used to fill in on a press couldn't repaint
 * until the measurement finished anyway, so it was never truly
 * immediate. The recent-measurement line and the histogram are the
 * feedback now, same as they always ended up being in practice.
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

// How many of the most recent samples, per direction, the histogram is
// built from. Override with -DHIST_CAPACITY=<n> if 500 is more or less
// than you want; memory cost is 4 bytes/sample/direction (2*4*n), so 500
// is 4KB total — trivial against this chip's 320KB of RAM.
#ifndef HIST_CAPACITY
#define HIST_CAPACITY 500
#endif

// --- Layout (the panel is 128x128) -------------------------------------
static const int SCREEN_W = 128;
static const int TOP_TEXT_Y = 0;    // Font2, 16 tall
static const int TOP_SUB_Y = 17;    // Font0, 8 tall / the bar
static const int BAR_X = 4;
static const int BAR_W = SCREEN_W - 2 * BAR_X;
static const int BAR_H = 7;
static const int MODE_Y = 26;       // Font0, 8 tall
static const int NEXT_Y = 35;       // Font0, 8 tall
static const int HIST_TOP = 45;
static const int HIST_H = 60;
static const int HIST_BINS = 32;    // 128px / 32 = 4px per bin, exactly
static const int HIST_LABEL_Y = HIST_TOP + HIST_H + 2;

static M5GFX display;
static bool displayReady = false;
static TaskHandle_t uiTask = nullptr;

// Written from loop() (core 1), read by uiTask (core 0). All are single
// aligned words, so a torn read isn't possible; the task re-reads them
// every pass and redraws whatever no longer matches the screen.
static volatile Mode wantActive = MODE_GAMEPAD;
static volatile Mode wantPending = MODE_GAMEPAD;
static volatile int64_t pressMicros = 0;
static volatile bool measurePending = false;
static volatile bool resetRequested = false;
static volatile bool meterToggleRequested = false;
static volatile HoldRung wantHint = RUNG_STATS;

// Measurement results. Touched only by uiTask, so no synchronisation.
// Kept as two separate populations, not pooled — see the file header on
// why a photoresistor's two directions are not expected to be the same.
static const int32_t LAT_NONE = -1;     // nothing measured yet
static const int32_t LAT_TIMEOUT = -2;  // the light never crossed
static int32_t lastLatencyUs = LAT_NONE;
static bool lastLatencyWasRise = false;  // which bucket lastLatencyUs is in

struct DirStats {
  uint32_t count = 0;
  uint32_t minUs = 0;
  uint64_t sumUs = 0;

  void record(uint32_t us) {
    if (count == 0 || us < minUs) minUs = us;
    sumUs += us;
    count++;
  }
};
static DirStats statsRise;  // ADC crossed upward through lightThreshold
static DirStats statsFall;  // ADC crossed downward through lightThreshold

// The last HIST_CAPACITY samples of one direction, as a ring buffer. Once
// full, a new sample overwrites the oldest — so the histogram always
// reflects only the most recent window, not the whole session (which the
// R/F summary line above it already covers).
struct History {
  uint32_t buf[HIST_CAPACITY];
  uint16_t count = 0;  // valid entries so far, saturates at HIST_CAPACITY
  uint16_t next = 0;   // ring write cursor

  void push(uint32_t us) {
    buf[next] = us;
    next = (next + 1) % HIST_CAPACITY;
    if (count < HIST_CAPACITY) count++;
  }
};
static History histRise;
static History histFall;

// Which of the two top-strip views is up. Owned by uiTask, flipped only
// in response to meterToggleRequested.
static bool meterView = false;

static uint16_t black() { return display.color565(0, 0, 0); }
static uint16_t riseColor() { return display.color565(120, 220, 230); }  // cyan
static uint16_t fallColor() { return display.color565(255, 130, 220); }  // magenta
static uint16_t dimColor() { return display.color565(110, 110, 110); }

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
      lastLatencyWasRise = waitForRise;
      if (waitForRise) {
        statsRise.record(us);
        histRise.push(us);
      } else {
        statsFall.record(us);
        histFall.push(us);
      }
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
// be aimed and the threshold sanity-checked at a glance. Takes over the
// measure view's top two lines only — everything else on screen (mode,
// pending mode, histogram) is unaffected by which view is up.
static void drawLightStrip(int raw, uint32_t mv) {
  uint16_t fg = riseColor();
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

// One direction's contribution to the sub line: "R6 24.9" or, with
// nothing recorded yet, "R--".
static void formatDirStats(char *out, size_t outLen, char letter, const DirStats &s) {
  if (s.count == 0) {
    snprintf(out, outLen, "%c--", letter);
  } else {
    uint32_t avgUs = (uint32_t)(s.sumUs / s.count);
    snprintf(out, outLen, "%c%u %u.%u", letter, (unsigned)s.count,
             avgUs / 1000, (avgUs % 1000) / 100);
  }
}

// The measure view's top strip: the last measurement — coloured by which
// direction it was, cyan for a rise and magenta for a fall, so the split
// is visible before you even read the sub line — and, underneath it, the
// count and mean for each direction kept separately. See the file header
// for why they're not pooled into one average.
static void drawLatencyStrip() {
  uint16_t warn = display.color565(255, 140, 60);

  char top[24];
  char sub[32];
  uint16_t topColor = dimColor();

  if (lastLatencyUs == LAT_NONE) {
    snprintf(top, sizeof(top), "-- ms");
  } else if (lastLatencyUs == LAT_TIMEOUT) {
    snprintf(top, sizeof(top), "no change");
    topColor = warn;
  } else {
    uint32_t us = (uint32_t)lastLatencyUs;
    snprintf(top, sizeof(top), "%u.%02u ms", us / 1000, (us % 1000) / 10);
    topColor = lastLatencyWasRise ? riseColor() : fallColor();
  }

  if (statsRise.count == 0 && statsFall.count == 0) {
    snprintf(sub, sizeof(sub), "press to measure");
  } else {
    char r[16], f[16];
    formatDirStats(r, sizeof(r), 'R', statsRise);
    formatDirStats(f, sizeof(f), 'F', statsFall);
    snprintf(sub, sizeof(sub), "%s  %s", r, f);
  }

  display.setTextDatum(textdatum_t::top_center);
  display.setTextPadding(SCREEN_W);  // clears its own line, so no flicker

  display.setFont(&fonts::Font2);
  display.setTextColor(topColor, black());
  display.drawString(top, SCREEN_W / 2, TOP_TEXT_Y);

  display.setFont(&fonts::Font0);
  display.setTextColor(dimColor(), black());
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

// The mode name, in its colour. Only ever drawn as part of a full frame
// (a mode change is one of the conditions that triggers one), so it
// doesn't need to manage its own incremental redraw.
static void drawModeLine(Mode active) {
  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(modeColor(active), black());
  display.setTextPadding(SCREEN_W);
  display.drawString(modeName(active), SCREEN_W / 2, MODE_Y);
  display.setTextPadding(0);
}

// Either the mode change already queued up and waiting on a reboot (see
// main.cpp for why it has to wait), which outranks everything because
// it's the one thing needing action elsewhere — or else what carrying on
// holding the button would do next. Drawn on its own so the hint can
// change mid-hold without a full repaint.
static void drawNextLine(Mode active, Mode pending, HoldRung hint) {
  char buf[24];
  uint16_t color;

  if (pending != active) {
    snprintf(buf, sizeof(buf), "-> %s", modeName(pending));
    color = display.color565(255, 190, 40);  // amber
  } else {
    const char *text = "";
    switch (hint) {
      case RUNG_STATS: text = "hold: reset stats"; break;
      case RUNG_METER: text = meterView ? "hold: latency view" : "hold: light meter"; break;
      case RUNG_MODE:  text = "hold: next mode"; break;
    }
    snprintf(buf, sizeof(buf), "%s", text);
    color = dimColor();
  }

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextPadding(SCREEN_W);
  display.setTextColor(color, black());
  display.drawString(buf, SCREEN_W / 2, NEXT_Y);
  display.setTextPadding(0);
}

// The last HIST_CAPACITY samples of each direction, as a stacked bar
// histogram sharing one time axis — fall bars from the baseline up,
// rise bars stacked on top of them — so a rise cluster and a fall
// cluster at different points on the axis are immediately visible as
// separate humps, and any further split *within* one direction (the
// thing this whole view exists to catch) shows up as multiple humps in
// one colour. minUs/maxUs across both buffers set the axis, and are
// labelled at its ends so a bar's position can be read as a real value.
static void drawHistogram() {
  // Redrawn on every new sample, and bin heights can shrink as well as
  // grow, so the whole area is cleared first rather than only drawn over.
  display.fillRect(0, HIST_TOP, SCREEN_W, HIST_LABEL_Y + 8 - HIST_TOP, black());

  if (histRise.count == 0 && histFall.count == 0) {
    display.setFont(&fonts::Font0);
    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(dimColor(), black());
    display.drawString("no samples yet", SCREEN_W / 2, HIST_TOP + HIST_H / 2);
    return;
  }

  uint32_t lo = UINT32_MAX, hi = 0;
  for (uint16_t i = 0; i < histRise.count; i++) {
    uint32_t v = histRise.buf[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  for (uint16_t i = 0; i < histFall.count; i++) {
    uint32_t v = histFall.buf[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  uint32_t span = (hi > lo) ? (hi - lo) : 1;

  uint16_t riseBins[HIST_BINS] = {0};
  uint16_t fallBins[HIST_BINS] = {0};
  for (uint16_t i = 0; i < histRise.count; i++) {
    uint32_t b = (histRise.buf[i] - lo) * HIST_BINS / (span + 1);
    riseBins[b < HIST_BINS ? b : HIST_BINS - 1]++;
  }
  for (uint16_t i = 0; i < histFall.count; i++) {
    uint32_t b = (histFall.buf[i] - lo) * HIST_BINS / (span + 1);
    fallBins[b < HIST_BINS ? b : HIST_BINS - 1]++;
  }

  uint16_t maxTotal = 1;
  for (int i = 0; i < HIST_BINS; i++) {
    uint16_t t = riseBins[i] + fallBins[i];
    if (t > maxTotal) maxTotal = t;
  }

  int barW = SCREEN_W / HIST_BINS;
  int yBase = HIST_TOP + HIST_H;
  for (int i = 0; i < HIST_BINS; i++) {
    int x = i * barW;
    int fallH = fallBins[i] * HIST_H / maxTotal;
    int riseH = riseBins[i] * HIST_H / maxTotal;
    if (fallH > 0) display.fillRect(x, yBase - fallH, barW, fallH, fallColor());
    if (riseH > 0) display.fillRect(x, yBase - fallH - riseH, barW, riseH, riseColor());
  }

  char loBuf[12], hiBuf[12];
  snprintf(loBuf, sizeof(loBuf), "%u.%u", lo / 1000, (lo % 1000) / 100);
  snprintf(hiBuf, sizeof(hiBuf), "%u.%u", hi / 1000, (hi % 1000) / 100);

  display.setFont(&fonts::Font0);
  display.setTextColor(dimColor(), black());
  display.setTextDatum(textdatum_t::top_left);
  display.drawString(loBuf, 1, HIST_LABEL_Y);
  display.setTextDatum(textdatum_t::top_right);
  display.drawString(hiBuf, SCREEN_W - 1, HIST_LABEL_Y);
}

static void drawFrame(Mode active, Mode pending, int raw, uint32_t mv, HoldRung hint) {
  display.startWrite();
  display.fillScreen(black());

  drawTopStrip(raw, mv);
  drawModeLine(active);
  drawNextLine(active, pending, hint);
  drawHistogram();

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
  bool shownMeter = false;
  HoldRung shownHint = RUNG_STATS;
  int shownRaw = -1;

  int raw = 0;
  uint32_t mv = 0;
  uint32_t lastSampleMs = 0;

  for (;;) {
    Mode active = wantActive;
    Mode pending = wantPending;
    HoldRung hint = wantHint;

    // Requests from loop() on the other core. Both are deferred to here
    // so that the stats and the view stay single-threaded on core 0.
    bool statsCleared = false;
    if (resetRequested) {
      resetRequested = false;
      lastLatencyUs = LAT_NONE;
      statsRise = DirStats();
      statsFall = DirStats();
      histRise = History();
      histFall = History();
      statsCleared = true;
    }
    if (meterToggleRequested) {
      meterToggleRequested = false;
      meterView = !meterView;
    }

    bool newFrame = (active != shownActive || pending != shownPending ||
                     meterView != shownMeter);
    // A clear only shows up in the measure view's top strip; the meter
    // view is live anyway and will repaint on its own cadence. The
    // histogram is visible in both views, so it always needs redrawing.
    bool topDirty = statsCleared && !meterView;
    bool histDirty = statsCleared;

    if (newFrame) shownRaw = -1;  // whatever is cached belongs to the old view

    // Measure BEFORE drawing anything. A repaint here would delay the
    // first sample by a millisecond or two of SPI, and — much worse —
    // could straddle the very change being timed. This runs in the meter
    // view too: the measurement is what the press is for either way.
    if (measurePending) {
      measurePending = false;
      runMeasurement(pressMicros);
      histDirty = true;
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
        drawFrame(active, pending, raw, mv, hint);
        shownActive = active;
        shownPending = pending;
        shownMeter = meterView;
        shownHint = hint;
        shownRaw = raw;
      } else {
        if (topDirty) {
          display.startWrite();
          drawTopStrip(raw, mv);
          display.endWrite();
          shownRaw = raw;
        }
        if (histDirty) {
          display.startWrite();
          drawHistogram();
          display.endWrite();
        }
        if (hint != shownHint) {
          display.startWrite();
          drawNextLine(active, pending, hint);
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
  // Core 0: loop() has core 1 (ARDUINO_RUNNING_CORE=1) to itself. Priority
  // 1 matches the Arduino loop task and stays below the USB task, so
  // neither the HID path nor enumeration can be held up by a repaint or
  // by a measurement. Stack is a bit larger than the bare minimum: the
  // histogram builds two 32-entry bin arrays plus a few format buffers on
  // this task's own stack per redraw.
  xTaskCreatePinnedToCore(uiTaskFn, "atoms3r-ui", 8192, nullptr, 1, &uiTask, 0);
}

void boardShowPress(bool pressed, Mode active, int64_t atMicros) {
  wantActive = active;
  if (pressed) {
    // Hand the edge timestamp over and let the task start its clock from
    // it. Note the consequence: the on-screen numbers don't update until
    // the measurement finishes, because the task must not be painting
    // while it is sampling. That's the measurement duration — tens of
    // milliseconds normally, MEASURE_TIMEOUT_MS at worst.
    pressMicros = atMicros;
    measurePending = true;
  }
  nudgeUi();
}

void boardShowPending(Mode active, Mode pending, bool firstOfHold) {
  (void)firstOfHold;  // the next-line changing is cue enough on a screen
  wantActive = active;
  wantPending = pending;
  nudgeUi();
}
