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
 * The view toggles from the menu (below), not a hold rung of its own — it
 * touches no USB state, so unlike a Mode it costs no reboot. Measurements
 * keep running and keep accumulating underneath the meter, so switching
 * back shows the stats and histogram they built up.
 *
 * Normal-operation layout, top to bottom:
 *
 *     24.38 ms                <- last measurement, coloured by direction
 *   R6 24.9  F7 41.2           <- per-direction n/mean
 *      GAMEPAD                 <- mode this boot came up as
 *   hold: reset stats          <- next hold rung, or the pending mode
 *   [ histogram, R/F stacked,  <- last HIST_CAPACITY samples of each
 *     min/max labelled ]          direction, same colour scheme as above
 *
 * In a BLE mode the mode line carries a second field, right-aligned,
 * because a press there only does something when a host is listening and
 * "nothing happened" would otherwise be indistinguishable from a broken
 * button:
 *
 *   BLE KEYBOARD      LINK     <- connected; presses go somewhere
 *   BLE KEYBOARD       ADV     <- advertising, nobody connected
 *   BLE KEYBOARD      PAIR     <- as above, just after bonds were dropped
 *
 * It lives on the mode line rather than the line below because that line
 * is already contended (automated run > armed drive > pending mode >
 * hold hint) and because the link is a property of the identity, which is
 * what the mode line is for. The press-time half of the same story is the
 * headline: a press with no link shows "not connected" there and starts
 * NO measurement, since nothing left the device and timing it would only
 * bank a guaranteed MEASURE_TIMEOUT_MS into the statistics.
 *
 * There is no separate action box or press indicator: this board has no
 * accessible RGB LED (see the gotcha in CLAUDE.md — the LP5562 exists and
 * drives the LCD backlight, but its R/G/B channels reach nothing usable),
 * and the on-screen box that used to fill in on a press couldn't repaint
 * until the measurement finished anyway, so it was never truly
 * immediate. The recent-measurement line and the histogram are the
 * feedback now, same as they always ended up being in practice.
 *
 * A 2s hold from normal operation replaces all of the above with a menu
 * (see board.h for the full normal/menu button contract — this file just
 * implements it):
 *
 *      MENU                    MENU_TOP: five items, or six in a BLE
 *    > Light meter              mode, cycled by tap and triggered by a
 *      Auto test                1s+ hold. Light meter flips meterView
 *      USB drive                and exits; Auto test and Pairing hand
 *      Change mode              off to main.cpp (which owns USB and the
 *      Pairing                  radio, so it owns both) and exit; USB
 *      Exit                     drive and Change mode drop into the two
 *   tap: next  hold: select     pickers below; Exit just leaves.
 *
 *                               Pairing exists only in a BLE mode — see
 *                               buildTopMenu() below, and
 *                               appBlePairingMode() in main.cpp for what
 *                               it does. In a USB mode the item is not
 *                               hidden-but-present, it is simply not in
 *                               the list, so it cannot be cycled past
 *                               either.
 *
 *   CHANGE MODE                MENU_MODE: tap advances the candidate via
 *     KEYBOARD                  appAdvancePendingMode() — the same NVS
 *   on next reset                write the old hold-to-cycle gesture did,
 *   tap: next  hold: confirm    just tap-driven now. Hold exits the menu;
 *                                the choice is already persisted per tap,
 *                                so there is nothing left to "confirm".
 *
 *   USB DRIVE                  MENU_STORAGE: the same shape again, for
 *     ARMED                     the one thing that is not a Mode you can
 *   on next reset                cycle to — tap toggles whether the next
 *   tap: toggle  hold: done      boot enumerates as a mass-storage device
 *                                instead of a HID one (persisted per tap
 *                                via appSetStorageArmed()), hold leaves.
 *
 * While the menu is open, no press reaches the HID/measurement path at
 * all (see main.cpp) — every press is menu input until MENU_TOP's Exit,
 * or a picker's confirm, returns menuState to MENU_NONE.
 *
 * A MODE_STORAGE boot leans on exactly that. It starts in MENU_DRIVE and
 * never leaves, so boardMenuActive() is true from the first loop() pass
 * onward and main.cpp's existing "the menu owns the button" branch does
 * all the work: no HID report (there is no HID device that boot), no
 * measurement started, no hold ladder, and no way to reach the automated
 * test — with not one line of storage-awareness in main.cpp's press path.
 * The screen is the whole UI for that boot:
 *
 *   USB DRIVE                  MENU_DRIVE: READY until the host ejects,
 *     READY                     then EJECTED. The line under it says what
 *   run files on host           a reset would do, so leaving is a hold
 *   next reset: drive           (which toggles the armed flag, disarming
 *   hold: toggle                 it) followed by a manual reset.
 *
 * And the rule that makes it safe: this file does NOT mount FFat in a
 * MODE_STORAGE boot. The host has the volume; a second FatFs with its own
 * cache on the same blocks would corrupt it, and neither side could tell.
 * Run recording is inert that boot as a consequence — storageReady stays
 * false — which costs nothing, because no run can be started anyway.
 *
 * Every automated run started from that menu is also written out, as one
 * CSV file per run on the board's `ffat` partition — see the run-storage
 * block below for the format and boardWriteRun() for the write itself.
 * Nothing is written while a run is going: this file's half of each
 * sample goes up to main.cpp through appRecordSample(), main.cpp buffers
 * the whole run in RAM, and the file is written once at the end. A flash
 * erase stalls the cache and therefore code execution on both cores, so
 * a per-sample write would land inside some other sample's measurement.
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
#include <stdarg.h>

#include <FFat.h>
#include <M5GFX.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <wear_levelling.h>

#include "board.h"

static const uint8_t SCREEN_BUTTON_PIN = 41;  // the LCD face is the button
static const uint8_t BRIGHTNESS = 160;

// --- The light sensor ----------------------------------------------------
// Default wiring: an M5Stack Unit Light (U012: photoresistor + LM393) on
// the Grove port. The unit's two signal wires are yellow = digital
// (comparator output, thresholded by the pot on the unit) and white =
// analog. On a Port A Grove connector yellow is the SDA line and white is
// the SCL line, and on the AtomS3R those are GPIO2 and GPIO1 respectively
// — so the analog output lands on GPIO1, which is ADC1_CH0. Only the
// analog side is read here; the digital side is left alone.
//
// Both the sense pin and an optional "virtual ground" pin are build-time
// knobs, so a different sensor can be wired to different pads without
// touching this file. The case that motivated them: a bare BPW34
// photodiode in photovoltaic mode straddling the bottom expansion pads —
// its 5.08mm lead pitch lands exactly on G6 and G8 (two 2.54mm pads
// apart, legs clearing G7 in between), neither of which is a ground. So:
//
//     -DLIGHT_SENSOR_PIN=6 -DLIGHT_GND_PIN=8 -DLIGHT_THRESHOLD=300
//
// wires anode -> G6 (sense, ADC1_CH5), cathode -> G8, with G8 driven LOW
// at task startup as the diode's ground. A GPIO held low is a perfectly
// good ground at photodiode currents: microamps across a few tens of
// ohms of Rds(on) is microvolts of error. Either pin works in either
// role (both are ADC1-capable, neither is a strapping pin) — if the
// meter view pins near zero under bright light, the diode is backwards;
// swap the two flags rather than resoldering. The low threshold is the
// other half of the story: photovoltaic mode tops out around 0.35-0.45V
// (~300-500 counts), nowhere near the Unit Light's 3000.
#ifndef LIGHT_SENSOR_PIN
#define LIGHT_SENSOR_PIN 1
#endif
#ifndef LIGHT_GND_PIN
#define LIGHT_GND_PIN -1  // -1: no virtual ground; the sensor has a real one
#endif
static const uint8_t LIGHT_ANALOG_PIN = LIGHT_SENSOR_PIN;
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

// How long each of the threshold calibration's two captures samples for
// (see runThresholdCal()). Unlike the measurement poll below, this loop
// yields every sample, so it is not watchdog-bound and 10s is fine.
#ifndef SENSOR_CAPTURE_MS
#define SENSOR_CAPTURE_MS 10000
#endif

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

// --- Run storage -------------------------------------------------------
// Finished automated runs are written to the `ffat` partition (see
// partitions_atoms3r_8MB.csv) as one CSV file each. FAT rather than
// LittleFS/SPIFFS on purpose: USB drive mode (see the MSC block near the
// bottom of this file) exposes this partition raw, so the host mounts the
// flash itself and reads the files with no firmware in the loop — which
// only works if what's down there really is a FAT volume. That also fixes the naming: 8.3, uppercase,
// no long-filename entries.
//
// One file looks like this:
//
//   # usb-latency run 42
//   # mode,GAMEPAD
//   # planned,500
//   # recorded,500
//   # aborted,no
//   seq,gap_ms,dir,latency_us,status
//   1,342,R,24381,ok
//   2,208,F,41002,ok
//   3,455,R,,timeout
//
// The mode is a metadata line rather than a sixth column repeated 500
// times: it is a property of the run, not of a sample, and every reader
// worth using (pandas' comment='#', R's read.csv(comment.char='#'))
// skips the '#' block for free. latency_us is left empty on a timeout
// and the reason moves to `status`, so the column stays purely numeric
// instead of needing a sentinel parsed out of it. dir is R or F, the
// same rise/fall split the on-screen stats keep — see the file header
// on why that distinction is worth carrying all the way to the file.
static const char *RUN_PREFS_NAMESPACE = "runlog";  // separate from main.cpp's "usbmode"
static const char *RUN_PREFS_KEY = "next";
// Filenames are RUNnnnnn.CSV, so the counter has to wrap before it needs
// a sixth digit or it would stop being 8.3. 99999 runs at three to five
// minutes each is several years of continuous testing; wrapping (and
// overwriting) is a better failure than silently writing RUN100000.CSV
// and having FAT mangle it into something with a tilde in.
static const uint32_t RUN_NUMBER_WRAP = 100000;
// How much of a run file to accumulate before handing it to FatFS. See
// runEmit() below for why the file isn't written a row at a time.
static const size_t RUN_WRITE_CHUNK = 1024;

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
// Set once by the UI task at boot, read by boardWriteRun() on core 1.
// Mounting is the only thing that ever touches it, and that happens long
// before any run can be started from the menu.
static volatile bool storageReady = false;

// Written from loop() (core 1), read by uiTask (core 0). All are single
// aligned words, so a torn read isn't possible; the task re-reads them
// every pass and redraws whatever no longer matches the screen.
static volatile Mode wantActive = MODE_GAMEPAD;
static volatile Mode wantPending = MODE_GAMEPAD;
static volatile int64_t pressMicros = 0;
static volatile bool measurePending = false;
// Covers the whole span a measurement occupies, unlike measurePending
// which the task clears the moment it picks the work up. Set on core 1
// in boardShowPress(), cleared on core 0 once runMeasurement() returns;
// boardMeasurementBusy() is what the automated test paces itself on.
static volatile bool measureBusy = false;
static volatile bool resetRequested = false;
static volatile bool enterMenuRequested = false;
static volatile bool menuTapRequested = false;
static volatile bool menuSelectRequested = false;
static volatile HoldRung wantHint = RUNG_STATS;

// Automated-test progress, pushed in from main.cpp (core 1) which owns
// the run. autoTotal == 0 means no run is in progress.
static volatile uint16_t autoDone = 0;
static volatile uint16_t autoTotal = 0;

// The menu's own state — see the file header for what each level looks
// like. menuState is volatile because boardMenuActive() (below) is
// called from main.cpp on core 1; topIndex never is, so it doesn't need
// to be.
// MENU_DRIVE is the odd one out: it is not reached from MENU_TOP at all,
// it is where a MODE_STORAGE boot starts and stays. Being a menu state is
// the point — it makes boardMenuActive() true for that whole boot, which
// is what keeps every press away from the HID/measurement path without
// main.cpp knowing storage exists. See the file header.
// MENU_CAPTURE covers the threshold calibration's whole flow — both
// captures, the tap-to-continue between them, and the results screen. A
// menu state for the same reason MENU_DRIVE is one: while it is up,
// boardMenuActive() keeps every press (the continue tap, the dismissing
// tap, anything stray mid-capture) from also being a HID press.
enum MenuState : uint8_t { MENU_NONE, MENU_TOP, MENU_MODE, MENU_STORAGE, MENU_DRIVE, MENU_CAPTURE };
static volatile MenuState menuState = MENU_NONE;
static int topIndex = 0;

// The top menu's items are identified by these rather than by their row,
// because the row a given item sits on now depends on the boot: Pairing
// is in the list only in a BLE mode, where it is the one thing the user
// may actually need and where, in a USB mode, it would be an item that
// does nothing. Selecting by identity means the switch below cannot
// silently start doing the wrong thing when the list changes length.
enum TopItem : uint8_t {
  ITEM_METER,
  ITEM_AUTO,
  ITEM_DRIVE,
  ITEM_MODE,
  ITEM_PAIR,   // BLE modes only
  ITEM_EXIT,
  ITEM_KINDS,
};
static const char *TOP_ITEM_LABELS[ITEM_KINDS] = {
  "Light meter", "Auto test", "USB drive", "Change mode", "Pairing", "Exit"
};
// Built once per boot by buildTopMenu(), from the mode. Fixed for the
// life of the boot, exactly like the mode it is derived from, so nothing
// has to think about the list changing under a live topIndex.
static TopItem topItems[ITEM_KINDS];
static int numTopItems = 0;

// The BLE link, as pushed down from main.cpp (see boardShowLink()).
// LINK_NONE in every non-BLE mode, which is what keeps every BLE-shaped
// branch in this file inert on the other four.
static volatile LinkState wantLink = LINK_NONE;
// Set by boardShowPress() when a press produced no report at all, and
// consumed by uiTaskFn. Not a measurement, deliberately: see the file
// header.
static volatile bool noLinkPress = false;

// Set by boardShowStorageEjected() from the TinyUSB task, read by uiTask.
// Only meaningful in a MODE_STORAGE boot, and only ever goes one way: the
// host has flushed and let go, so the screen can say so.
static volatile bool storageEjected = false;

// Measurement results. Touched only by uiTask, so no synchronisation.
// Kept as two separate populations, not pooled — see the file header on
// why a photoresistor's two directions are not expected to be the same.
static const int32_t LAT_NONE = -1;     // nothing measured yet
static const int32_t LAT_TIMEOUT = -2;  // the light never crossed
// A press in a BLE mode with no host listening. Distinct from a timeout
// on purpose: a timeout means the report went out and the display didn't
// answer, which is a result about the machine under test; this means
// nothing went out at all, which is a result about the device.
static const int32_t LAT_NOLINK = -3;
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
// from within the menu's "Light meter" item (see menuSelectRequested
// handling in uiTaskFn).
static bool meterView = false;

static uint16_t black() { return display.color565(0, 0, 0); }
static uint16_t riseColor() { return display.color565(120, 220, 230); }  // cyan
static uint16_t fallColor() { return display.color565(255, 130, 220); }  // magenta
static uint16_t dimColor() { return display.color565(110, 110, 110); }

// Same three hues for the same three actions, with the BLE family shifted
// to the secondaries — matching the S3-Zero's LED table exactly, so a
// colour means the same thing on both boards. See board_s3zero.cpp.
static uint16_t modeColor(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:      return display.color565(255,  72,  56);  // red
    case MODE_KEYBOARD:     return display.color565( 48, 214,  96);  // green
    case MODE_MOUSE:        return display.color565( 72, 150, 255);  // blue
    case MODE_BLE_GAMEPAD:  return display.color565(255, 190,  40);  // yellow
    case MODE_BLE_KEYBOARD: return display.color565( 90, 224, 214);  // cyan
    case MODE_BLE_MOUSE:    return display.color565(230, 110, 230);  // magenta
    default:                return display.color565(255, 255, 255);
  }
}

// The right-hand field of the mode line in a BLE mode. Four characters at
// most: Font0 is a 6px cell, the mode name can be twelve characters
// ("BLE KEYBOARD"), and 12 + 4 plus a gap is all 21 cells of the panel
// will take.
static const char *linkTag(LinkState link) {
  switch (link) {
    case LINK_CONNECTED:   return "LINK";
    case LINK_PAIRING:     return "PAIR";
    case LINK_ADVERTISING: return "ADV";
    default:               return "";
  }
}

static uint16_t linkColor(LinkState link) {
  switch (link) {
    case LINK_CONNECTED:   return display.color565( 48, 214,  96);  // green
    case LINK_PAIRING:     return display.color565(255, 190,  40);  // amber
    default:               return display.color565(110, 110, 110);  // dim
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
      // Third destination for the same sample, after the running stats
      // and the histogram: main.cpp, which pairs it with the gap that
      // preceded the press and buffers it for the run's file. Handing it
      // over from in here rather than from the caller is what guarantees
      // it lands before measureBusy drops — see board.h.
      appRecordSample(us, waitForRise, false);
      return;
    }
    if (now >= deadline) {
      lastLatencyUs = LAT_TIMEOUT;
      // A timeout is a result, not a missing one: which direction was
      // being waited for is still worth recording, because a run full of
      // timeouts in one direction says something quite different from a
      // run full of them in both.
      appRecordSample(0, waitForRise, true);
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
  } else if (lastLatencyUs == LAT_NOLINK) {
    snprintf(top, sizeof(top), "not connected");
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

// The mode name, in its colour — plus, in a BLE mode only, the link state
// right-aligned beside it. Two draws rather than one string because the
// two fields want two colours: the mode keeps its own identity colour
// whatever the radio is doing, and the tag carries green/amber/dim for
// connected/pairing/advertising.
//
// Only ever drawn as part of a full frame (a mode change and a link
// change are both conditions that trigger one), so it doesn't need to
// manage its own incremental redraw.
static void drawModeLine(Mode active, LinkState link) {
  display.setFont(&fonts::Font0);

  if (!modeIsBle(active)) {
    display.setTextDatum(textdatum_t::top_center);
    display.setTextColor(modeColor(active), black());
    display.setTextPadding(SCREEN_W);
    display.drawString(modeName(active), SCREEN_W / 2, MODE_Y);
    display.setTextPadding(0);
    return;
  }

  // Two fields means neither can clear the line with text padding without
  // erasing the other, so the line is cleared once up front instead.
  display.fillRect(0, MODE_Y, SCREEN_W, 8, black());
  display.setTextPadding(0);
  display.setTextDatum(textdatum_t::top_left);
  display.setTextColor(modeColor(active), black());
  display.drawString(modeName(active), 2, MODE_Y);
  display.setTextDatum(textdatum_t::top_right);
  display.setTextColor(linkColor(link), black());
  display.drawString(linkTag(link), SCREEN_W - 2, MODE_Y);
}

// Either the mode change already queued up and waiting on a reboot (see
// main.cpp for why it has to wait), which outranks everything because
// it's the one thing needing action elsewhere — or else what carrying on
// holding the button would do next. Drawn on its own so the hint can
// change mid-hold without a full repaint.
static void drawNextLine(Mode active, Mode pending, HoldRung hint) {
  char buf[32];
  uint16_t color;

  if (autoTotal > 0) {
    // An automated run outranks the other two: it's the only one of the
    // three that changes second to second, and while it runs the button
    // means "stop" rather than any of the hold rungs.
    //
    // Width: Font0 is a 6px cell, so 21 characters is the whole 128px
    // panel. At AUTO_TEST_ITERATIONS = 500 the widest this ever renders
    // is "AUTO 499/500 tap=stop" — exactly 21, exactly full width, no
    // margin left. (Displayed done never reaches total: the run swaps
    // back to the normal display on its last release.) Raising the
    // iteration count to four digits would push this over and clip the
    // tail, so shorten the hint if that ever happens.
    snprintf(buf, sizeof(buf), "AUTO %u/%u tap=stop",
             (unsigned)autoDone, (unsigned)autoTotal);
    color = riseColor();
  } else if (appStorageArmed()) {
    // Outranks a queued mode change because it is what the next reset
    // will actually do: the drive boot ignores the HID mode entirely and
    // comes back to it only once the drive is disarmed again.
    snprintf(buf, sizeof(buf), "-> USB DRIVE");
    color = display.color565(255, 190, 40);  // amber
  } else if (pending != active) {
    snprintf(buf, sizeof(buf), "-> %s", modeName(pending));
    color = display.color565(255, 190, 40);  // amber
  } else {
    const char *text = "";
    switch (hint) {
      case RUNG_STATS: text = "hold: reset stats"; break;
      case RUNG_MENU:  text = "hold: open menu"; break;
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

// MENU_TOP: the items of this boot's menu, one per line, with the current
// selection picked out in amber. A tap (below) cycles topIndex; a hold
// (below) triggers whichever item is highlighted.
static void drawMenuTop() {
  uint16_t dim = dimColor();
  uint16_t hi = display.color565(255, 190, 40);  // amber, matches the pending-mode colour

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("MENU", SCREEN_W / 2, 6);

  // Six items (a BLE mode's list) at the previous 14px pitch from y=22
  // would put the last row at y=92, overlapping the "tap: next" hint at
  // y=96. 12px from y=20 lands it at y=80 with eight clear pixels below,
  // and Font0 is 8px tall so a 12px pitch still leaves a visible gap
  // between rows. That is the panel full: a seventh item needs a
  // scrolling menu, not another row.
  const int rowH = 12;
  const int top = 20;
  char buf[24];
  for (int i = 0; i < numTopItems; i++) {
    bool selected = (i == topIndex);
    const char *label = TOP_ITEM_LABELS[topItems[i]];
    // In the meter view the Auto test slot runs the sensor capture
    // instead (see the select handler), so it says so.
    if (topItems[i] == ITEM_AUTO && meterView) label = "Find threshold";
    snprintf(buf, sizeof(buf), selected ? "> %s" : "%s", label);
    display.setTextColor(selected ? hi : dim, black());
    display.drawString(buf, SCREEN_W / 2, top + i * rowH);
  }

  display.setTextColor(dim, black());
  display.drawString("tap: next", SCREEN_W / 2, 96);
  display.drawString("hold: select", SCREEN_W / 2, 108);
}

// MENU_MODE: the mode picker. pendingMode is the candidate a tap here
// advances (via appAdvancePendingMode(), which persists it immediately —
// there is nothing left for the hold to "confirm" beyond leaving the
// menu), shown big and in its own colour, same as the old headline used
// to be for the active mode.
static void drawMenuMode(Mode active, Mode pending) {
  uint16_t dim = dimColor();

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("CHANGE MODE", SCREEN_W / 2, 6);

  display.setTextDatum(textdatum_t::middle_center);
  display.setTextColor(modeColor(pending), black());
  display.setFont(&fonts::Font4);
  if (display.textWidth(modeName(pending)) > SCREEN_W - 8) {
    display.setFont(&fonts::Font2);
  }
  display.drawString(modeName(pending), SCREEN_W / 2, 52);

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  if (pending != active) {
    display.drawString("on next reset", SCREEN_W / 2, 76);
  }
  display.drawString("tap: next", SCREEN_W / 2, 96);
  display.drawString("hold: confirm", SCREEN_W / 2, 108);
}

// MENU_STORAGE: the arm/disarm picker, deliberately built to the same
// pattern as drawMenuMode() above — a big candidate, "on next reset"
// underneath it, and a tap that persists immediately so the hold has
// nothing left to do but leave. Storage is not a Mode you can cycle to
// (see mode.h on why MODE_STORAGE sits outside the rotation), so it needs
// its own picker; making that picker look and behave identically is the
// next best thing to it being one.
static void drawMenuStorage(Mode pending) {
  uint16_t dim = dimColor();
  bool armed = appStorageArmed();

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("USB DRIVE", SCREEN_W / 2, 6);

  display.setTextDatum(textdatum_t::middle_center);
  display.setFont(&fonts::Font4);
  display.setTextColor(armed ? display.color565(255, 190, 40) : dim, black());
  display.drawString(armed ? "ARMED" : "OFF", SCREEN_W / 2, 52);

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  if (armed) {
    display.drawString("on next reset", SCREEN_W / 2, 76);
  } else {
    // Say what it falls back to, so the two lines answer the same
    // question ("what will the next reset be?") either way round.
    char buf[24];
    snprintf(buf, sizeof(buf), "stays %s", modeName(pending));
    display.drawString(buf, SCREEN_W / 2, 76);
  }
  display.drawString("tap: toggle", SCREEN_W / 2, 96);
  display.drawString("hold: done", SCREEN_W / 2, 108);
}

// MENU_DRIVE: the entire UI of a MODE_STORAGE boot. No items, no way out
// except a hold and a manual reset — which is the honest shape of it,
// since the USB descriptor is fixed for the boot (see main.cpp) and
// nothing on this screen can change what the host is currently mounting.
// The bottom line is therefore the important one: it always states what
// the next reset will do, so "how do I get my gamepad back" is answered
// on screen rather than remembered.
static void drawMenuDrive(Mode pending) {
  uint16_t dim = dimColor();
  uint16_t amber = display.color565(255, 190, 40);
  bool ejected = storageEjected;
  bool armed = appStorageArmed();

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("USB DRIVE", SCREEN_W / 2, 6);

  display.setTextDatum(textdatum_t::middle_center);
  display.setFont(&fonts::Font4);
  const char *headline = ejected ? "EJECTED" : "READY";
  if (display.textWidth(headline) > SCREEN_W - 8) display.setFont(&fonts::Font2);
  display.setTextColor(ejected ? amber : riseColor(), black());
  display.drawString(headline, SCREEN_W / 2, 48);

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString(ejected ? "safe to unplug" : "run files on host",
                     SCREEN_W / 2, 70);

  char buf[28];
  if (armed) {
    snprintf(buf, sizeof(buf), "next reset: drive");
    display.setTextColor(dim, black());
  } else {
    snprintf(buf, sizeof(buf), "next reset: %s", modeName(pending));
    display.setTextColor(amber, black());
  }
  display.drawString(buf, SCREEN_W / 2, 90);

  display.setTextColor(dim, black());
  display.drawString("hold: toggle", SCREEN_W / 2, 106);
}

// --- Threshold calibration (meter view's replacement for Auto test) ----
// Two labelled captures, SENSOR_CAPTURE_MS of ~1kHz raw ADC sampling
// each: the user sets the display to one state before selecting the menu
// item, the first capture runs, a prompt asks them to set the other
// state and tap once, and the second capture runs. Labelling the two
// sets beats clustering one mixed capture (the previous, Otsu-based
// design): there is no mixture to unpick, and the threshold can be
// placed dead-centre in the actually-measured gap between the sets, with
// the margin reported as a number instead of inferred.
//
// The whole flow owns the button — menuState is MENU_CAPTURE throughout,
// so no press during it is ever a HID send. That is deliberate and new
// relative to the old design: the user arranges the display state
// themselves between phases, and a press that also sent a click would
// flip the very state they just set up.
//
// Sampling yields every iteration (vTaskDelay(1)), unlike the
// measurement's tight poll — a 10s unyielding spin would trip the core-0
// task watchdog at 5s, and level statistics don't need microsecond
// cadence anyway.
struct CapSet {
  uint32_t n = 0;
  uint16_t minV = 0, maxV = 0;
  float mu = 0, sd = 0;
};
struct CalResult {
  bool done = false;      // a calibration has run this boot
  bool clean = false;     // the two sets don't overlap
  CapSet dim, bright;     // ordered by mean, not by capture order
  int thr = 0;            // suggested threshold
  int margin = 0;         // counts of clear air each side of thr (clean only)
  uint32_t overlap = 0;   // samples on the wrong side of thr (overlap only)
};
static CalResult cal;

// One histogram per phase, deliberately static: 8KB each is nothing to
// this chip's RAM but far too much for the UI task's stack.
static uint16_t capHistA[4096];
static uint16_t capHistB[4096];

static void drawCaptureProgress(int phase, uint32_t elapsedMs, uint16_t curMin, uint16_t curMax) {
  display.startWrite();
  display.fillScreen(black());
  display.setFont(&fonts::Font2);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(riseColor(), black());
  char buf[28];
  snprintf(buf, sizeof(buf), "CAPTURE %d/2  %lus/%us", phase,
           (unsigned long)(elapsedMs / 1000), (unsigned)(SENSOR_CAPTURE_MS / 1000));
  display.drawString(buf, SCREEN_W / 2, 20);
  display.setFont(&fonts::Font0);
  display.setTextColor(dimColor(), black());
  snprintf(buf, sizeof(buf), "seen %u - %u", curMin, curMax);
  display.drawString(buf, SCREEN_W / 2, 52);
  display.drawString("hold the display steady", SCREEN_W / 2, 76);
  display.endWrite();
}

static void drawCapturePrompt(const CapSet &first) {
  display.startWrite();
  display.fillScreen(black());
  display.setFont(&fonts::Font2);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(display.color565(255, 190, 40), black());
  display.drawString("CAPTURE 1 DONE", SCREEN_W / 2, 16);
  display.setFont(&fonts::Font0);
  display.setTextColor(dimColor(), black());
  char buf[28];
  snprintf(buf, sizeof(buf), "mean %.0f  range %u-%u", first.mu, first.minV, first.maxV);
  display.drawString(buf, SCREEN_W / 2, 44);
  display.drawString("switch the display to", SCREEN_W / 2, 66);
  display.drawString("the OTHER state, then", SCREEN_W / 2, 78);
  display.setTextColor(riseColor(), black());
  display.drawString("tap: start capture 2", SCREEN_W / 2, 100);
  display.endWrite();
}

// One phase: fill `hist`, summarise into `out`. Streaming min/max feed
// the progress screen; mean/sd come off the histogram afterwards.
static void capturePhase(int phase, uint16_t *hist, CapSet &out) {
  memset(hist, 0, 4096 * sizeof(uint16_t));
  uint16_t mn = 4095, mx = 0;
  uint32_t n = 0;
  uint32_t start = millis(), lastDraw = 0;
  drawCaptureProgress(phase, 0, 0, 0);  // immediately, not 500ms late

  for (;;) {
    uint32_t elapsed = millis() - start;
    if (elapsed >= SENSOR_CAPTURE_MS) break;
    int v = analogRead(LIGHT_ANALOG_PIN);
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    hist[v]++;
    n++;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    if (elapsed - lastDraw >= 500) {
      lastDraw = elapsed;
      drawCaptureProgress(phase, elapsed, mn, mx);
    }
    vTaskDelay(1);  // yield: feeds the idle task, sets the ~1kHz cadence
  }

  out.n = n;
  out.minV = mn;
  out.maxV = mx;
  double sum = 0, sq = 0;
  for (int i = mn; i <= mx && n; i++) {
    if (!hist[i]) continue;
    sum += (double)hist[i] * i;
    sq += (double)hist[i] * i * i;
  }
  if (n) {
    out.mu = sum / n;
    out.sd = sqrt(fmax(0.0, sq / n - out.mu * out.mu));
  }
}

// The whole calibration, run linearly on the UI task from the menu's
// select handler. Blocking in here is fine — this task owns the screen
// and the sensor, and everything it would otherwise be doing is exactly
// what this flow is doing. The inter-phase wait polls the tap request
// flag that loop() (core 1) sets, which is also why the flags are
// cleared before the wait: a tap queued during phase 1 must not start
// phase 2 on its own.
static void runThresholdCal() {
  CapSet a, b;
  capturePhase(1, capHistA, a);

  drawCapturePrompt(a);
  menuTapRequested = false;
  menuSelectRequested = false;
  while (!menuTapRequested && !menuSelectRequested) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  menuTapRequested = false;
  menuSelectRequested = false;

  capturePhase(2, capHistB, b);

  cal = CalResult();
  cal.done = true;
  const bool aDim = a.mu <= b.mu;
  cal.dim = aDim ? a : b;
  cal.bright = aDim ? b : a;
  const uint16_t *dimHist = aDim ? capHistA : capHistB;
  const uint16_t *brightHist = aDim ? capHistB : capHistA;

  if (cal.bright.minV > cal.dim.maxV) {
    // Clean gap: the threshold goes dead-centre in it, and the margin —
    // the clear air between the threshold and the nearest sample either
    // side — is the number that says how much room for error there is.
    cal.clean = true;
    cal.thr = (cal.dim.maxV + cal.bright.minV) / 2;
    cal.margin = (cal.bright.minV - cal.dim.maxV) / 2;
  } else {
    // The sets overlap. Cut at the midpoint of the means and count the
    // samples on the wrong side — an honest "this placement/sensor can't
    // cleanly separate these two states" number, not a fake margin.
    cal.clean = false;
    cal.thr = (int)((cal.dim.mu + cal.bright.mu) / 2);
    for (int i = cal.thr + 1; i < 4096; i++) cal.overlap += dimHist[i];
    for (int i = 0; i <= cal.thr; i++) cal.overlap += brightHist[i];
  }
}

// The calibration's report. Suggested threshold big and amber, both
// sets' level and spread, and either the margin (clean) or the overlap
// count (not) — so "how safe is this threshold" is a number on the
// screen, not a feeling.
static void drawCaptureResult() {
  uint16_t dim = dimColor();
  uint16_t amber = display.color565(255, 190, 40);
  uint16_t warn = display.color565(255, 140, 60);
  char buf[32];

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("THRESHOLD CAL", SCREEN_W / 2, 4);

  if (!cal.done) return;

  display.setTextColor(amber, black());
  display.setFont(&fonts::Font4);
  snprintf(buf, sizeof(buf), "%d", cal.thr);
  display.drawString(buf, SCREEN_W / 2, 22);

  display.setFont(&fonts::Font0);
  if (cal.clean) {
    display.setTextColor(cal.margin >= 3 * (int)fmax(cal.dim.sd, cal.bright.sd) ? dim : warn, black());
    snprintf(buf, sizeof(buf), "margin %d counts", cal.margin);
  } else {
    display.setTextColor(warn, black());
    snprintf(buf, sizeof(buf), "OVERLAP: %lu wrong side", (unsigned long)cal.overlap);
  }
  display.drawString(buf, SCREEN_W / 2, 48);

  display.setTextColor(dim, black());
  snprintf(buf, sizeof(buf), "dark %.0f s%.1f %u-%u", cal.dim.mu, cal.dim.sd,
           cal.dim.minV, cal.dim.maxV);
  display.drawString(buf, SCREEN_W / 2, 64);
  snprintf(buf, sizeof(buf), "brite %.0f s%.1f %u-%u", cal.bright.mu, cal.bright.sd,
           cal.bright.minV, cal.bright.maxV);
  display.drawString(buf, SCREEN_W / 2, 74);
  snprintf(buf, sizeof(buf), "current threshold %d", lightThreshold);
  display.drawString(buf, SCREEN_W / 2, 88);
  display.drawString("tap: done", SCREEN_W / 2, 112);
}

static void drawFrame(Mode active, Mode pending, int raw, uint32_t mv, HoldRung hint,
                      LinkState link) {
  display.startWrite();
  display.fillScreen(black());

  if (menuState == MENU_TOP) {
    drawMenuTop();
  } else if (menuState == MENU_MODE) {
    drawMenuMode(active, pending);
  } else if (menuState == MENU_STORAGE) {
    drawMenuStorage(pending);
  } else if (menuState == MENU_DRIVE) {
    drawMenuDrive(pending);
  } else if (menuState == MENU_CAPTURE) {
    drawCaptureResult();
  } else {
    drawTopStrip(raw, mv);
    drawModeLine(active, link);
    drawNextLine(active, pending, hint);
    drawHistogram();
  }

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
#if LIGHT_GND_PIN >= 0
  // The sensor's ground is a GPIO held low (see the sensor knobs above).
  // Done once, here, and no code may call pinMode() on this pin again —
  // same peripheral-manager rule as the old TEPT4400 pull-up (see the
  // CLAUDE.md gotcha): a later pinMode() would reconfigure the pin and
  // silently drop the drive. Until this line runs the pin floats and the
  // sensor reads garbage; that window ends before the first press can be
  // measured, since this task starts before loop()'s first pass matters.
  pinMode(LIGHT_GND_PIN, OUTPUT);
  digitalWrite(LIGHT_GND_PIN, LOW);
#endif

  // Mount here and nowhere else. This is the one moment in the firmware's
  // life when a flash operation is harmless: USB is already up (main.cpp
  // called USB.begin() before boardShowBoot() started this task), nothing
  // has been pressed yet, and no measurement can be running. Doing it in
  // setup() would put it before USB.begin(); doing it lazily on the first
  // run would put an erase inside a measurement window.
  //
  // formatOnFail: a device flashed with this partition table for the
  // first time has an `ffat` partition full of whatever was there before,
  // which will not mount — so the first boot formats it, once, and every
  // boot after that mounts in milliseconds. Note the consequence for that
  // first boot only: the screen stays black for however long the format
  // takes, because this task is the one that draws.
  //
  // Skipped outright in a MODE_STORAGE boot, and this is the single most
  // important line in that feature: the host is about to mount these very
  // blocks. A FatFs instance here would keep its own cache of a volume
  // the host is also writing, and the two would diverge silently until
  // the directory table stopped making sense. So the firmware simply does
  // not have a filesystem that boot — boardStorageBegin() took the
  // partition raw instead, back in setup(), and nothing here needs one:
  // no run can be started from a screen with no menu items on it.
  if (wantActive != MODE_STORAGE) storageReady = FFat.begin(true);

  // What the screen currently shows, so only real changes are repainted.
  Mode shownActive = MODE_COUNT;  // MODE_COUNT != any real mode, forcing
  Mode shownPending = MODE_COUNT; // the first pass to draw a full frame
  bool shownMeter = false;
  HoldRung shownHint = RUNG_STATS;
  MenuState shownMenuState = MENU_TOP;  // != MENU_NONE, same forcing trick
  int shownTopIndex = -1;
  // 0xFF is not a LinkState, so the first pass draws — same forcing trick
  // again, and it has to be one: LINK_NONE is a real value here (every
  // non-BLE mode sits in it forever).
  LinkState shownLink = (LinkState)0xFF;
  uint16_t shownAutoDone = 0xFFFF;      // != any real count, same trick again
  uint16_t shownAutoTotal = 0xFFFF;
  int shownRaw = -1;
  // The two storage screens draw from state that isn't menuState or
  // topIndex, so they need their own "what's on the panel" mirrors or a
  // toggle would never repaint.
  bool shownArmed = false;
  bool shownEjected = false;

  int raw = 0;
  uint32_t mv = 0;
  uint32_t lastSampleMs = 0;

  for (;;) {
    Mode active = wantActive;
    Mode pending = wantPending;
    HoldRung hint = wantHint;
    // wantLink is read below, AFTER the menu requests are serviced, not
    // here with the others: selecting the Pairing item changes the link
    // state inside that block, and a snapshot taken up here would draw
    // the frame that leaves the menu with the old tag still on it.

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
    // Menu requests, likewise deferred here rather than acted on where
    // they're raised (main.cpp, on core 1) — see boardEnterMenu() etc.
    // below. All three read-modify-write menuState/topIndex, so keeping
    // them on this one core is what makes that safe without a lock.
    if (enterMenuRequested) {
      enterMenuRequested = false;
      menuState = MENU_TOP;
      topIndex = 0;
    }
    if (menuTapRequested) {
      menuTapRequested = false;
      if (menuState == MENU_TOP) {
        topIndex = (topIndex + 1) % numTopItems;
      } else if (menuState == MENU_MODE) {
        appAdvancePendingMode();
      } else if (menuState == MENU_STORAGE) {
        // Persisted on the spot, exactly like the mode picker's tap —
        // so the hold that follows is only ever "I'm done looking".
        appSetStorageArmed(!appStorageArmed());
      }
      else if (menuState == MENU_CAPTURE) {
        menuState = MENU_NONE;  // the report has one job: be read, then leave
      }
      // MENU_DRIVE deliberately ignores taps: a stray brush of the screen
      // face while the host is copying files must not change what the
      // next reset does. Only the 1s hold below counts there.
    }
    if (menuSelectRequested) {
      menuSelectRequested = false;
      if (menuState == MENU_TOP) {
        switch (topItems[topIndex]) {
          case ITEM_METER: meterView = !meterView; menuState = MENU_NONE; break;
          // Close the menu on the way out so the run is visible as it
          // goes; main.cpp waits for the button to come up before its
          // first press, so this hold can't leak into the data.
          case ITEM_AUTO:
            if (meterView) {
              // The meter view's version of the auto test: calibrate the
              // threshold from two labelled captures instead of timing
              // the display. menuState goes to MENU_CAPTURE *before* the
              // flow starts and stays there throughout — every press
              // during it is menu input, never a HID send, because the
              // user arranges the display state by hand between phases
              // and a press that also clicked would flip the very state
              // they just set up. runThresholdCal() blocks right here
              // through both captures and the tap-to-continue between
              // them; the stale-request clearing it does internally is
              // what keeps a phase-1 tap from starting phase 2.
              menuState = MENU_CAPTURE;
              runThresholdCal();
              menuTapRequested = false;   // a tap queued during phase 2
              menuSelectRequested = false; // must not dismiss the report
            } else {
              appStartAutoTest();
              menuState = MENU_NONE;
            }
            break;
          case ITEM_DRIVE: menuState = MENU_STORAGE; break;
          case ITEM_MODE:  menuState = MENU_MODE; break;
          // Pairing has no picker of its own and nothing to confirm: it
          // is a single irreversible-ish act (bonds gone, advertising
          // restarted) whose result shows up on the mode line's link tag
          // a moment later, so the menu just gets out of the way. Note it
          // erases NVS and therefore stalls the flash cache on both
          // cores — harmless precisely here, with no run going and no
          // measurement outstanding, and the same licence boardWriteRun()
          // has.
          case ITEM_PAIR:  appBlePairingMode(); menuState = MENU_NONE; break;
          case ITEM_EXIT:  menuState = MENU_NONE; break;
          default: break;
        }
      } else if (menuState == MENU_MODE || menuState == MENU_STORAGE ||
                 menuState == MENU_CAPTURE) {
        // The pickers persist per tap and the capture report is only a
        // report — in all three, a hold just leaves.
        menuState = MENU_NONE;
      } else if (menuState == MENU_DRIVE) {
        // The only gesture a storage boot has. It cannot change what the
        // host is mounting right now — the USB descriptor was fixed
        // before enumeration — so all it does is decide what the next
        // manual reset comes up as. Toggling rather than only disarming
        // means an accidental hold is undoable with another one.
        appSetStorageArmed(!appStorageArmed());
      }
    }

    LinkState link = wantLink;  // see the note where the others are read

    bool inMenu = (menuState != MENU_NONE);
    bool storageScreen = (menuState == MENU_STORAGE || menuState == MENU_DRIVE);
    bool armed = storageScreen && appStorageArmed();
    bool ejected = storageEjected;
    bool newFrame = (active != shownActive || pending != shownPending ||
                     meterView != shownMeter ||
                     menuState != shownMenuState || topIndex != shownTopIndex ||
                     armed != shownArmed || ejected != shownEjected ||
                     link != shownLink);
    // A clear only shows up in the measure view's top strip; the meter
    // view is live anyway and will repaint on its own cadence. The
    // histogram is visible in both views, so it always needs redrawing.
    // Neither applies while the menu is up — it draws over both.
    bool topDirty = statsCleared && !meterView && !inMenu;
    bool histDirty = statsCleared && !inMenu;

    if (newFrame) shownRaw = -1;  // whatever is cached belongs to the old view

    // Measure BEFORE drawing anything. A repaint here would delay the
    // first sample by a millisecond or two of SPI, and — much worse —
    // could straddle the very change being timed. This runs in the meter
    // view too: the measurement is what the press is for either way.
    if (measurePending) {
      measurePending = false;
      runMeasurement(pressMicros);
      measureBusy = false;  // releases the automated test's next press
      histDirty = true;
      if (!meterView) topDirty = true;
    }

    // A press that produced no report — a BLE mode with nobody listening.
    // It deliberately does not go through runMeasurement(): nothing left
    // the device, so nothing is going to change on the display, and the
    // MEASURE_TIMEOUT_MS the poll would burn would land in the stats and
    // in any run file as a timeout that says something quite untrue about
    // the machine under test. The headline says so instead, and the
    // statistics are left exactly as they were.
    if (noLinkPress) {
      noLinkPress = false;
      lastLatencyUs = LAT_NOLINK;
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
        drawFrame(active, pending, raw, mv, hint, link);
        shownActive = active;
        shownLink = link;
        shownPending = pending;
        shownMeter = meterView;
        shownHint = hint;
        shownMenuState = menuState;
        shownTopIndex = topIndex;
        shownArmed = armed;
        shownEjected = ejected;
        shownAutoDone = autoDone;
        shownAutoTotal = autoTotal;
        shownRaw = raw;
      } else if (!inMenu) {
        // The menu redraws only via newFrame above (any change to
        // menuState/topIndex is one of its conditions) — nothing here
        // applies while it's up.
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
        // The same line carries the hold hint and the automated run's
        // progress, so any of the three moving redraws it.
        if (hint != shownHint || autoDone != shownAutoDone ||
            autoTotal != shownAutoTotal) {
          display.startWrite();
          drawNextLine(active, pending, hint);
          display.endWrite();
          shownHint = hint;
          shownAutoDone = autoDone;
          shownAutoTotal = autoTotal;
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
// this board gets the full normal-operation ladder and the menu built on
// top of it (see the file header and board.h).
bool boardHasSensor() {
  return true;
}

void boardResetStats() {
  resetRequested = true;
  nudgeUi();
}

void boardShowHoldHint(HoldRung next) {
  wantHint = next;
  nudgeUi();
}

bool boardMeasurementBusy() {
  return measureBusy;
}

void boardShowAutoTest(uint16_t done, uint16_t total) {
  autoDone = done;
  autoTotal = total;
  nudgeUi();
}

// Allocate this run's file number. Bumped before the file is written
// rather than after: losing a number to a failed write costs nothing,
// whereas handing the same number out twice would have the second run
// silently overwrite the first.
static uint32_t nextRunNumber() {
  Preferences runPrefs;
  // A namespace of its own rather than another key in main.cpp's
  // "usbmode": that one is owned by the mode state machine on core 1 and
  // this is written from core 1 too, but by an unrelated feature — and
  // separate namespaces mean an erase of either can never take the other
  // with it.
  if (!runPrefs.begin(RUN_PREFS_NAMESPACE, false)) return 0;
  uint32_t n = runPrefs.getUInt(RUN_PREFS_KEY, 0);
  runPrefs.putUInt(RUN_PREFS_KEY, (n + 1) % RUN_NUMBER_WRAP);
  runPrefs.end();
  return n;
}

// Rows are built up in this buffer and written in batches rather than one
// f.print() per line: every write is a VFS + FatFS round trip, and 500 of
// them is 500 chances to touch flash where a handful would do. File scope
// rather than a local so a kilobyte of it isn't sitting on loop()'s
// stack; boardWriteRun() below is the only caller, and it is called from
// one core at a time with no run in progress, so there is nothing to
// share it with.
static char runBuf[RUN_WRITE_CHUNK + 64];
static size_t runBufUsed = 0;

static void runFlush(File &f) {
  if (runBufUsed == 0) return;
  f.write((const uint8_t *)runBuf, runBufUsed);
  runBufUsed = 0;
}

static void runEmit(File &f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(runBuf + runBufUsed, sizeof(runBuf) - runBufUsed, fmt, ap);
  va_end(ap);
  if (n > 0) runBufUsed += (size_t)n;
  // The slack past RUN_WRITE_CHUNK is what makes this safe to check
  // after the fact rather than before: a row is a few dozen bytes and the
  // buffer has 64 spare, so the longest possible row still fits whatever
  // the previous flush left behind.
  if (runBufUsed >= RUN_WRITE_CHUNK) runFlush(f);
}

// Write a finished run out as RUNnnnnn.CSV. Called from loop() on core 1
// with the run already over and no measurement outstanding (main.cpp's
// finishRun() guarantees both), so this is free to block for as long as
// the flash takes.
void boardWriteRun(const RunRecord &run) {
  if (!storageReady || run.count == 0) return;

  uint32_t number = nextRunNumber();
  char path[16];  // "/RUN99999.CSV" plus NUL, comfortably
  snprintf(path, sizeof(path), "/RUN%05u.CSV", (unsigned)number);

  File f = FFat.open(path, FILE_WRITE);
  if (!f) return;
  runBufUsed = 0;

  runEmit(f, "# usb-latency run %u\n", (unsigned)number);
  runEmit(f, "# mode,%s\n", modeName(run.mode));
  runEmit(f, "# planned,%u\n", (unsigned)run.planned);
  runEmit(f, "# recorded,%u\n", (unsigned)run.count);
  runEmit(f, "# aborted,%s\n", run.aborted ? "yes" : "no");
  runEmit(f, "seq,gap_ms,dir,latency_us,status\n");

  for (uint16_t i = 0; i < run.count; i++) {
    const RunSample &s = run.samples[i];
    if (s.timedOut) {
      // latency_us deliberately left empty rather than filled with a
      // sentinel, so the column parses as a number everywhere and the
      // reason lives in `status` instead.
      runEmit(f, "%u,%u,%c,,timeout\n", (unsigned)(i + 1), (unsigned)s.gapMs,
              s.rise ? 'R' : 'F');
    } else {
      runEmit(f, "%u,%u,%c,%u,ok\n", (unsigned)(i + 1), (unsigned)s.gapMs,
              s.rise ? 'R' : 'F', (unsigned)s.latencyUs);
    }
  }

  runFlush(f);
  f.close();
}

// --- USB mass storage: the ffat partition as raw blocks ----------------
// A MODE_STORAGE boot hands this partition to the host instead of
// mounting it (see the file header, and boardStorageBegin() for the
// mutual exclusion). The mapping is not an approximation of what FatFs
// does on top of wear levelling — it is deliberately the same arithmetic,
// copied from ESP-IDF's own fatfs/diskio/diskio_wl.c:
//
//   sector count  wl_size(h) / wl_sector_size(h)
//   sector N      lives at byte N * wl_sector_size(h)
//   writing one   wl_erase_range() that sector, then wl_write() it whole
//
// so a block the host reads as LBA N is byte-for-byte the sector FatFs
// wrote as sector N. Using wl_sector_size() as the USB block size rather
// than the conventional 512 is what buys that: the FAT volume down there
// was formatted with 4096-byte logical sectors (CONFIG_WL_SECTOR_SIZE),
// and a 512-byte block size would leave the host's FAT driver reconciling
// a 4096-byte BPB against 512-byte blocks for no gain.
static wl_handle_t wlHandle = WL_INVALID_HANDLE;
static size_t wlSector = 0;   // bytes per wear-levelling sector == USB block
static uint32_t wlBlocks = 0;
// One sector, staged. wl_write() needs its sector erased first, so a
// partial-block write cannot be passed straight through — it would have
// to erase what the earlier fragments just wrote. Chunks are accumulated
// here and committed only once a whole sector is present. With this
// core's CFG_TUD_MSC_EP_BUFSIZE (4096) equal to the block size the host
// always fills it in one call, but the SCSI contract permits fragments
// and correctness here is not worth gambling on a build-time constant.
static uint8_t *wlStage = nullptr;

bool boardHasStorage() {
  return true;
}

bool boardStorageBegin(uint32_t *blockCount, uint16_t *blockSize) {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  if (!part) return false;

  // Note what is NOT happening anywhere near here: FFat.begin(). This is
  // the only claim on the partition for the whole boot — uiTaskFn checks
  // the mode and skips its mount — because two writers on one FAT volume
  // is data loss with no symptom until it is far too late.
  if (wl_mount(part, &wlHandle) != ESP_OK) {
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  wlSector = wl_sector_size(wlHandle);
  // USB block size is a uint16_t on the wire, so an oversized sector is a
  // configuration this code cannot express — better to say so and let
  // main.cpp fall back to a HID boot than to enumerate a broken drive.
  if (wlSector == 0 || wlSector > UINT16_MAX) {
    wl_unmount(wlHandle);
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  wlStage = (uint8_t *)malloc(wlSector);
  if (!wlStage) {
    wl_unmount(wlHandle);
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  wlBlocks = (uint32_t)(wl_size(wlHandle) / wlSector);
  *blockCount = wlBlocks;
  *blockSize = (uint16_t)wlSector;
  return wlBlocks > 0;
}

int32_t boardStorageRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  if (wlHandle == WL_INVALID_HANDLE) return -1;
  // Out of range is an error, not a short read: returning 0 would tell
  // TinyUSB "not ready, ask again" and spin the host forever.
  if (lba >= wlBlocks || offset > wlSector || size > wlSector - offset) return -1;
  if (wl_read(wlHandle, (size_t)lba * wlSector + offset, buffer, size) != ESP_OK) return -1;
  return (int32_t)size;
}

int32_t boardStorageWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t size) {
  if (wlHandle == WL_INVALID_HANDLE || !wlStage) return -1;
  if (lba >= wlBlocks || offset > wlSector || size > wlSector - offset) return -1;

  memcpy(wlStage + offset, buffer, size);
  // Still short of a whole sector: report the bytes taken and wait for
  // the rest of the block before touching flash at all.
  if (offset + size < wlSector) return (int32_t)size;

  size_t addr = (size_t)lba * wlSector;
  if (wl_erase_range(wlHandle, addr, wlSector) != ESP_OK) return -1;
  if (wl_write(wlHandle, addr, wlStage, wlSector) != ESP_OK) return -1;
  return (int32_t)size;
}

void boardShowStorageEjected() {
  // Runs on the TinyUSB task, so it does what every other cross-core
  // entry point here does and no more: one store and a poke.
  storageEjected = true;
  nudgeUi();
}

bool boardMenuActive() {
  return menuState != MENU_NONE;
}

void boardEnterMenu() {
  enterMenuRequested = true;
  nudgeUi();
}

void boardMenuTap() {
  menuTapRequested = true;
  nudgeUi();
}

void boardMenuSelect() {
  menuSelectRequested = true;
  nudgeUi();
}

bool boardButtonPressed() {
  return digitalRead(SCREEN_BUTTON_PIN) == LOW;  // active-low
}

// Which items this boot's top menu has. Called once, from boardShowBoot()
// on core 1 before uiTaskFn exists, so there is nobody to race: the mode
// is fixed for the life of the boot (see mode.h), so the list derived
// from it is too, and topIndex can never be pointed at an item that
// stopped existing.
//
// Pairing is in only in a BLE mode. Not greyed out, not present-but-inert
// — absent, so tapping through the menu in a USB mode is exactly the menu
// it always was. Nothing else is conditional: USB drive and Auto test are
// as useful over the radio as over the wire, which is rather the point of
// having BLE modes at all.
static void buildTopMenu(Mode active) {
  numTopItems = 0;
  topItems[numTopItems++] = ITEM_METER;
  topItems[numTopItems++] = ITEM_AUTO;
  topItems[numTopItems++] = ITEM_DRIVE;
  topItems[numTopItems++] = ITEM_MODE;
  if (modeIsBle(active)) topItems[numTopItems++] = ITEM_PAIR;
  topItems[numTopItems++] = ITEM_EXIT;
}

void boardShowBoot(Mode active, Mode pending) {
  wantActive = active;
  wantPending = pending;
  buildTopMenu(active);
  // Set here, on core 1, rather than left for the task to notice: main.cpp
  // starts calling boardMenuActive() on its very first loop() pass, which
  // can beat a freshly created task to its first iteration. A storage boot
  // that lost that race would route its first press into the HID path —
  // where there is no HID device to send on.
  if (active == MODE_STORAGE) menuState = MENU_DRIVE;
  // Core 0: loop() has core 1 (ARDUINO_RUNNING_CORE=1) to itself. Priority
  // 1 matches the Arduino loop task and stays below the USB task, so
  // neither the HID path nor enumeration can be held up by a repaint or
  // by a measurement. Stack is a bit larger than the bare minimum: the
  // histogram builds two 32-entry bin arrays plus a few format buffers on
  // this task's own stack per redraw.
  xTaskCreatePinnedToCore(uiTaskFn, "atoms3r-ui", 8192, nullptr, 1, &uiTask, 0);
}

void boardShowPress(bool pressed, Mode active, int64_t atMicros, bool sent) {
  wantActive = active;
  if (pressed) {
    if (!sent) {
      // Nothing was transmitted, so there is nothing to time. Raise the
      // headline flag instead of the measurement one, and in particular
      // do NOT set measureBusy — the automated test paces itself on that
      // flag, and a run that set it here would wait out a measurement
      // that is never going to be started.
      noLinkPress = true;
      nudgeUi();
      return;
    }
    // Hand the edge timestamp over and let the task start its clock from
    // it. Note the consequence: the on-screen numbers don't update until
    // the measurement finishes, because the task must not be painting
    // while it is sampling. That's the measurement duration — tens of
    // milliseconds normally, MEASURE_TIMEOUT_MS at worst.
    pressMicros = atMicros;
    // Before measurePending, so the task can never pick the work up and
    // finish it in the window between the two and leave busy stuck true.
    measureBusy = true;
    measurePending = true;
  }
  nudgeUi();
}

// Called from main.cpp, which is itself often inside a NimBLE callback on
// the host task when it calls this — a third core-crossing on top of the
// two this file already has. Same treatment as all of them: one store and
// a poke, nothing drawn here.
void boardShowLink(LinkState state) {
  wantLink = state;
  nudgeUi();
}

void boardShowPending(Mode active, Mode pending, bool firstOfHold) {
  (void)firstOfHold;  // the next-line changing is cue enough on a screen
  wantActive = active;
  wantPending = pending;
  nudgeUi();
}
