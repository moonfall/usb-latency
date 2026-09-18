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
 *      MENU                    MENU_TOP: the items of this boot, cycled
 *    > Auto test                by tap and triggered by a 1s+ hold.
 *      Change mode              Light meter flips meterView and exits;
 *      Light meter              Auto test and Pairing hand off to
 *      Pairing                  main.cpp (which owns USB and the radio,
 *      Validate                 so it owns both) and exit; Change mode
 *      Sensor                   and Sensor drop into the pickers below;
 *      Exit                     Exit just leaves.
 *   tap: next  hold: select
 *                               Ordered by reach, not by age — see
 *                               buildTopMenu() below. Seven rows is the
 *                               most this boot can show and the most the
 *                               12px pitch allows; see drawMenuTop().
 *
 *                               Pairing exists only in a BLE mode — see
 *                               buildTopMenu() below, and
 *                               appBlePairingMode() in main.cpp for what
 *                               it does. In a USB mode the item is not
 *                               hidden-but-present, it is simply not in
 *                               the list, so it cannot be cycled past
 *                               either.
 *
 *   CHANGE MODE                MENU_MODE: tap advances the candidate and
 *     KEYBOARD                  persists it on the spot — the same NVS
 *   on next reset                write the old hold-to-cycle gesture did,
 *   tap: next  hold: confirm    just tap-driven now. Hold exits the menu;
 *                                the choice is already persisted per tap,
 *                                so there is nothing left to "confirm".
 *
 *                               The list is the six modes plus USB DRIVE
 *                                — one picker for "what does the next
 *                                reset come up as", which is the actual
 *                                question, rather than one picker for the
 *                                six and a second for the seventh. Only
 *                                six of the seven are a Mode; the drive
 *                                is the armed flag instead (see
 *                                pickerCandidate() below).
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
// Unit Light wiring, for the record: the module's two Grove signal wires
// are yellow = digital (LM393 comparator, thresholded by the module's
// pot) and white = analog; on a Port A connector yellow is SDA and white
// is SCL, which on the AtomS3R are GPIO2 and GPIO1 — so the analog
// output lands on GPIO1 (ADC1_CH0), and only the analog side is read.
//
// Sensor configurations, selectable at runtime from the menu ("Sensor")
// and persisted in NVS — the -DLIGHT_SENSOR_PIN / -DLIGHT_GND_PIN build
// knobs this replaces required a reflash to swap sensors, which turned
// out to be exactly the wrong shape once two sensors were physically
// wired at once.
//
//  [0] Unit Light (default): the Grove module, analog on GPIO1. The
//      default because it is the sensor a fresh device is assumed to
//      have — a stored selection of a sensor that isn't wired reads a
//      floating pin, so the safe one has to be what NVS-less boots get.
//  [1] BPW34: bare photodiode in photovoltaic mode straddling the
//      bottom pads — 5.08mm lead pitch lands on G6 (anode, sense) and
//      G8 (cathode) exactly, legs clearing G7. G8 is driven LOW as the
//      diode's ground when this config activates: a GPIO held low is a
//      real ground at photodiode currents (uA across tens of ohms of
//      Rds(on) = uV of error). Photovoltaic swing tops out ~0.35-0.45V,
//      hence the ~250-count default threshold; wired backwards it pins
//      near zero in the meter view — swap the legs.
//
// Each config carries its own default threshold AND its own calibrated
// NVS slot ("thr0"/"thr1", see activateSensor()): a threshold is a
// property of a sensor circuit, and one global value would let
// calibrating one sensor silently clobber the other's.
struct SensorConfig {
  const char *name;   // picker + reports
  const char *tag;    // meter view prefix
  uint8_t pin;        // ADC sense pin
  int8_t gndPin;      // driven LOW while active; -1 = sensor has a real ground
  int defaultThr;     // used when no calibration is stored for this config
};
#ifndef LIGHT_THRESHOLD
#define LIGHT_THRESHOLD 3000  // Unit Light default; per-config, see the table
#endif
#ifndef BPW34_THRESHOLD
#define BPW34_THRESHOLD 250
#endif
static const SensorConfig SENSORS[] = {
  { "Unit Light", "UL", 1, -1, LIGHT_THRESHOLD },
  { "BPW34",      "PD", 6,  8, BPW34_THRESHOLD },
};
static const uint8_t SENSOR_COUNT = sizeof(SENSORS) / sizeof(SENSORS[0]);
// Which config is live. Read everywhere the sensor is touched (all UI
// task), written by the picker and at boot (also UI task) — plain, safe.
static uint8_t activeSensor = 0;
static inline uint8_t sensorPin() { return SENSORS[activeSensor].pin; }

static const int ADC_MAX = 4095;              // 12-bit, the Arduino default
static const uint32_t LIGHT_PERIOD_MS = 100;  // metering cadence in the meter view
static const int LIGHT_DEADBAND = 8;          // counts of ADC noise not worth a repaint

// Not const: follows the active sensor config, and a threshold
// calibration (see runThresholdCal()) can replace it at runtime — the
// replacement is persisted per config ("thr0"/"thr1") and re-loaded by
// activateSensor(), so a stored calibration OUTRANKS the config's
// default from then on. Read and written only on the UI task
// (measurement, meter tick, calibration report, activation), so a plain
// int is safe. 0 in NVS means "nothing stored, use the default".
static int lightThreshold = LIGHT_THRESHOLD;
static const char *SENSOR_PREFS_NAMESPACE = "sensor";  // own namespace, same reasoning as "runlog"
static const char *SENSOR_PREFS_CFG_KEY = "cfg";       // active config index

static void sensorThrKey(uint8_t idx, char *out, size_t outLen) {
  snprintf(out, outLen, "thr%u", (unsigned)idx);
}

// Make one config live: pins, attenuation, threshold (stored calibration
// if any, the config's default otherwise), and — unless this is the boot
// pass replaying a stored choice — persist the selection. UI task only.
static void activateSensor(uint8_t idx, bool persist) {
  if (idx >= SENSOR_COUNT) idx = 0;
  activeSensor = idx;
  const SensorConfig &c = SENSORS[idx];
  analogSetPinAttenuation(c.pin, ADC_11db);  // full ~0-3.3V span
  if (c.gndPin >= 0) {
    // The sensor's ground is a GPIO held low (see the config table).
    // Re-asserted on every activation — cheap, and it means switching
    // away and back never leaves the diode floating. Nothing else may
    // call pinMode() on this pin (the peripheral-manager rule from the
    // old TEPT4400 pull-up gotcha still applies).
    pinMode(c.gndPin, OUTPUT);
    digitalWrite(c.gndPin, LOW);
  }

  lightThreshold = c.defaultThr;
  char key[8];
  sensorThrKey(idx, key, sizeof(key));
  Preferences p;
  if (p.begin(SENSOR_PREFS_NAMESPACE, false)) {
    int stored = (int)p.getUInt(key, 0);
    if (stored > 0 && stored < 4096) lightThreshold = stored;
    if (persist) p.putUChar(SENSOR_PREFS_CFG_KEY, idx);
    p.end();
  }
}

// How long each of the threshold calibration's two captures samples for
// (see runThresholdCal()). Unlike the measurement poll below, this loop
// yields every sample, so it is not watchdog-bound and 10s is fine.
#ifndef SENSOR_CAPTURE_MS
#define SENSOR_CAPTURE_MS 10000
#endif

// Positioning time between the hold that selects the calibration and the
// first sample of capture 1. The button is the screen face, so at the
// instant the flow starts the user's hand is still on the device they
// now have to aim a sensor at; without this, capture 1's opening second
// is a measurement of a finger moving away rather than of the display
// state being characterised. Same reason as the automated test's
// AUTO_START_DELAY_MS in main.cpp, minus the aliasing argument — this
// capture is a level statistic, not a timing one, so a fixed wait costs
// it nothing.
static const uint32_t CAL_START_DELAY_MS = 1000;

// How long to wait after the calibration's own press has gone out before
// capture 2 starts sampling. The press is the thing that changes the
// display, and the change is not instant: the host has to notice the HID
// report, the page has to repaint, the panel has to finish its own
// transition, and the sensor has its own response time on top. Sampling
// through any of that would put the transition itself into the set that
// is supposed to characterise the steady state — which is precisely the
// overlap the report warns about. Half a second is generous against
// every one of those and costs a capture nothing.
static const uint32_t CAL_SETTLE_MS = 500;

// How long to wait for the display to respond before giving up. Also the
// longest this task can hold core 0 without letting its idle task run —
// keep it well under the 5s task watchdog.
static const uint32_t MEASURE_TIMEOUT_MS = 500;

// Consecutive samples that must agree before a threshold crossing counts,
// in the measurement, the validation's watchers, everywhere. Exists
// because single-sample ADC noise blips near a low threshold produced
// impossible sub-millisecond "measurements". NOTE: this protection (and
// the averaged baseline in runMeasurement()) was first added in the
// SFH309 era, then lost when that sensor's code was shelved to the
// tept4400 branch — the fast-detection complaints that led to the
// validation mode were, in part, this fix being missing. It is
// deliberately part of main again now.
static const int MEASURE_CONFIRM = 3;

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

// Full-test progress, same push-from-core-1 pattern. fullTotal == 0 means
// no full test in progress.
static volatile uint8_t fullStepShown = 0;
static volatile uint8_t fullTotalShown = 0;

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
enum MenuState : uint8_t { MENU_NONE, MENU_TOP, MENU_MODE, MENU_DRIVE, MENU_CAPTURE, MENU_VAL, MENU_SENSOR };
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
  ITEM_VALIDATE,
  ITEM_SENSOR,
  ITEM_MODE,
  ITEM_PAIR,   // BLE modes only
  ITEM_EXIT,
  ITEM_FULL,   // deliberately BELOW Exit: a ~15-minute six-reboot run is
               // the last thing a stray extra tap should land on
  ITEM_KINDS,
};
static const char *TOP_ITEM_LABELS[ITEM_KINDS] = {
  "Light meter", "Auto test", "Validate", "Sensor", "Change mode", "Pairing", "Exit", "Full test"
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

// Shared by the reset-stats hold and by switching sensor configs — both
// mean "everything measured so far no longer applies".
static void clearStats() {
  lastLatencyUs = LAT_NONE;
  statsRise = DirStats();
  statsFall = DirStats();
  histRise = History();
  histFall = History();
}

// Which of the two top-strip views is up. Owned by uiTask, flipped only
// from within the menu's "Light meter" item (see menuSelectRequested
// handling in uiTaskFn).
static bool meterView = false;

static const uint32_t VAL_POSTWATCH_MS = 300;
static const uint32_t VAL_FAST_US = 1000;

struct ValStats {
  uint16_t presses = 0, timeouts = 0, fastCnt = 0, multiCnt = 0, cleanCnt = 0;
  uint32_t idleFlips = 0;   // confirmed crossings with no input in flight
  uint16_t flipsSincePress = 0;
  uint32_t minUs = 0, sumUs = 0, latCnt = 0;
};
static ValStats val;
static bool validationActive = false;  // set/cleared on the UI task only

// Idle-watch side tracking, persistent across watch slices so a flip
// straddling two slices still counts once. -1 = unknown, re-learn.
static int valIdleSide = -1;
static int valIdleRun = 0;

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
  // Averaged baseline: a single read sits within ADC noise of a low
  // threshold, and a noise-displaced baseline picks the wrong crossing
  // direction. ~100us of reads, none of it in the reported figure — t0
  // is already fixed.
  int32_t baselineSum = 0;
  for (int i = 0; i < 4; i++) baselineSum += analogRead(sensorPin());
  int baseline = baselineSum / 4;
  bool waitForRise = baseline < lightThreshold;
  int64_t deadline = t0 + (int64_t)MEASURE_TIMEOUT_MS * 1000;

  // A crossing counts only after MEASURE_CONFIRM consecutive agreeing
  // reads — but the reported time is the FIRST read of the run, so the
  // confirmation rejects one-sample noise blips without adding a
  // microsecond to the figure.
  int64_t crossAt = 0;
  int run = 0;
  for (;;) {
    int v = analogRead(sensorPin());
    int64_t now = esp_timer_get_time();

    if (waitForRise ? (v >= lightThreshold) : (v < lightThreshold)) {
      if (run == 0) crossAt = now;
      // No deadline check while confirming: staying crossed confirms
      // within MEASURE_CONFIRM samples (microseconds), and dropping back
      // resets run and falls through to the deadline check below — so
      // the loop cannot fail to terminate.
      if (++run < MEASURE_CONFIRM) continue;
      uint32_t us = (uint32_t)(crossAt - t0);
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
  snprintf(buf, sizeof(buf), "%s %4d %u.%02uV", SENSORS[activeSensor].tag, raw,
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

  if (fullTotalShown > 0 && autoTotal == 0) {
    // A full test between runs: settling after a reboot, or waiting on a
    // BLE host. The step counter is the useful part; "settling" says why
    // nothing appears to be happening.
    snprintf(buf, sizeof(buf), "FULL %u/%u settling...",
             (unsigned)fullStepShown, (unsigned)fullTotalShown);
    color = riseColor();
  } else if (autoTotal > 0) {
    // An automated run outranks the other two: it's the only one of the
    // three that changes second to second, and while it runs the button
    // means "stop" rather than any of the hold rungs.
    //
    // Width: Font0 is a 6px cell, so 21 characters is the whole 128px
    // panel. The widest of the three run forms is the full-test one,
    // "F3/6 499/500 tap=stop" — exactly 21, exactly full width, no
    // margin left. (Displayed done never reaches total: the run swaps
    // back to the normal display on its last release.) Four-digit
    // iteration counts would clip the tail; shorten the hint then.
    if (fullTotalShown > 0) {
      snprintf(buf, sizeof(buf), "F%u/%u %u/%u tap=stop",
               (unsigned)fullStepShown, (unsigned)fullTotalShown,
               (unsigned)autoDone, (unsigned)autoTotal);
    } else {
      snprintf(buf, sizeof(buf), "%s %u/%u tap=stop",
               validationActive ? "VAL" : "AUTO",
               (unsigned)autoDone, (unsigned)autoTotal);
    }
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

  // Seven items at most now (a BLE mode's list, with Pairing) — USB
  // drive left for the mode picker, which is where the question it
  // answers actually belongs. That bought back the pixel the eighth row
  // had cost: the pitch is 12px again, rows from y=8, so the seventh
  // lands at y=80 and clears the hints at 96/108 by eight pixels, with
  // Font0's 8px cell leaving four clear pixels per gap instead of
  // three. Back to the 11px pitch (third visit — see the ceiling gotcha's
  // pitch history): Full test is the eighth row in a BLE mode, and eight
  // rows fit only at 11px, ending at y=85 with the hints at 96/108.
  // Seven fits at the comfier 12px; a NINTH needs a scrolling menu at
  // any pitch, and that remains the hard ceiling.
  const int rowH = 11;
  const int top = 8;
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

// MENU_MODE: the picker for "what does the next reset come up as". Its
// list is the six rotation modes plus one virtual seventh entry, USB
// DRIVE — which is not a Mode the mode key can hold (see mode.h on why
// MODE_STORAGE sits past MODE_COUNT) but is the other thing a reset can
// produce, so it belongs in the same list rather than behind a second,
// identical-looking picker of its own. MODE_COUNT stands in for that
// entry here: it is already the value no real mode takes, which keeps
// the list a plain 0..MODE_COUNT range.
static const int PICK_DRIVE = MODE_COUNT;

// Where the picker is currently sitting — derived from the truth every
// time rather than tracked in a variable of its own, so the screen
// cannot drift from what a reset would actually do. The precedence is
// boot's: the armed flag outranks pendingMode (main.cpp ignores the mode
// key entirely on a drive boot), so an armed drive IS the candidate,
// whatever pendingMode still remembers.
static int pickerCandidate(Mode pending) {
  return appStorageArmed() ? PICK_DRIVE : (int)pending;
}

// A tap advances that candidate, persisting the result immediately —
// there is nothing left for the hold to "confirm" beyond leaving the
// menu. The candidate is shown big and in its own colour, same as the
// old headline used to be for the active mode.
static void drawMenuMode(Mode active, Mode pending) {
  uint16_t dim = dimColor();
  bool drive = (pickerCandidate(pending) == PICK_DRIVE);
  const char *name = drive ? "USB DRIVE" : modeName(pending);

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("CHANGE MODE", SCREEN_W / 2, 6);

  display.setTextDatum(textdatum_t::middle_center);
  // Amber for the drive, matching the "-> USB DRIVE" the normal screen's
  // next line shows for the same state; the six modes keep their own
  // colours.
  display.setTextColor(drive ? display.color565(255, 190, 40) : modeColor(pending),
                       black());
  // Font2 for every candidate, unconditionally. This used to be Font4
  // with a width-conditional fall back to Font2, which meant the SIZE of
  // the text encoded nothing but the length of the name: "MOUSE" (90px
  // in Font4) stayed big while "GAMEPAD" (122px) and every BLE name fell
  // back, so tapping through the list made the headline jump between two
  // sizes for no reason the user could act on. One size for all seven
  // candidates is the fix, and Font2 is the one that fits them all —
  // the widest, "BLE KEYBOARD", is 93px against the 120px usable width,
  // so nothing here can ever need a fallback again.
  display.setFont(&fonts::Font2);
  display.drawString(name, SCREEN_W / 2, 48);

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  // An armed drive is an "on next reset" condition in its own right —
  // pendingMode can equal activeMode and the next boot still be a drive.
  if (drive || pending != active) {
    // Nudged up with the candidate above it: Font2 is ten pixels shorter
    // than Font4 was, and leaving both lines where they sat left the
    // block low in the free band between the heading and the hints.
    display.drawString("on next reset", SCREEN_W / 2, 70);
  }
  display.drawString("tap: next", SCREEN_W / 2, 96);
  display.drawString("hold: confirm", SCREEN_W / 2, 108);
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
// each, with the display changed between them by the firmware's own
// press: the user aims the sensor and selects the item, capture 1 runs,
// one real HID/BLE press goes out and flips the test page to its other
// state, and capture 2 runs. Labelling the two sets beats clustering one
// mixed capture (the previous, Otsu-based design): there is no mixture
// to unpick, and the threshold can be placed dead-centre in the
// actually-measured gap between the sets, with the margin reported as a
// number instead of inferred.
//
// The whole flow still owns the button — menuState is MENU_CAPTURE
// throughout, so no press *of the user's* is ever a HID send. What that
// rule protects has changed shape, though, and the new split is worth
// stating plainly:
//
//   * the firmware's press is the one input event the flow wants. It is
//     made by main.cpp on core 1 (appCalPress(), which this file waits
//     on through appCalPressBusy()), because only that file may touch
//     USB or the radio, and it is deliberately not a measurement.
//   * the user's button is the abort. It was the "I've set the other
//     state, go" tap in the manual design; with no manual step left,
//     there is nothing for it to mean but stop — and a press that also
//     clicked would flip the display halfway through a capture, which is
//     the same hazard the old design was avoiding, from the other end.
//
// Refused outright in a BLE mode with no subscribed host (appCanSendInput()):
// the press is the entire mechanism here, so without one the two
// captures would characterise the same state and the report would blame
// the sensor for an overlap the radio caused.
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
  bool done = false;      // a calibration ran to completion, and thr is usable
  // The two ways it can end with nothing to apply. Both leave done
  // false, which is what stops the report's hold from writing a stale
  // threshold from some earlier run.
  bool cancelled = false; // a user press ended it early
  bool noLink = false;    // refused to start: nothing would have received the press
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
  // The button's only job during the flow now — worth saying, since it
  // used to be how the user advanced it.
  display.drawString("tap: cancel", SCREEN_W / 2, 100);
  display.endWrite();
}

// The positioning pause before capture 1. A frame of its own rather than
// a state inside drawCaptureProgress(): nothing is being sampled yet, and
// a "0s/10s" line on screen would be saying otherwise.
static void drawCaptureReady() {
  display.startWrite();
  display.fillScreen(black());
  display.setFont(&fonts::Font2);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(display.color565(255, 190, 40), black());
  display.drawString("GET READY", SCREEN_W / 2, 20);
  display.setFont(&fonts::Font0);
  display.setTextColor(dimColor(), black());
  display.drawString("aim the sensor at the", SCREEN_W / 2, 52);
  display.drawString("display and let go", SCREEN_W / 2, 68);
  display.setTextColor(riseColor(), black());
  display.drawString("capture 1 in 1s", SCREEN_W / 2, 90);
  display.endWrite();
}

// Between the two captures, in place of the old "switch the display and
// tap" prompt: the press is going out and the display is being given
// CAL_SETTLE_MS to actually change. A frame of its own for the same
// reason GET READY is one — nothing is being sampled yet, and the
// progress screen would be claiming otherwise. Capture 1's numbers used
// to be shown here; they are not, because there is no longer any
// decision for the user to make against them, and the report a few
// seconds later carries both sets properly.
static void drawCaptureSwitching(const CapSet &first) {
  display.startWrite();
  display.fillScreen(black());
  display.setFont(&fonts::Font2);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(display.color565(255, 190, 40), black());
  display.drawString("SWITCHING", SCREEN_W / 2, 16);
  display.setFont(&fonts::Font0);
  display.setTextColor(dimColor(), black());
  char buf[28];
  snprintf(buf, sizeof(buf), "capture 1 mean %.0f", first.mu);
  display.drawString(buf, SCREEN_W / 2, 44);
  display.drawString("sending one press to", SCREEN_W / 2, 66);
  display.drawString("flip the display", SCREEN_W / 2, 78);
  display.setTextColor(riseColor(), black());
  display.drawString("capture 2 next", SCREEN_W / 2, 100);
  display.endWrite();
}

// Any user press during the flow means stop. There is no step left that
// wants one — the firmware makes its own input event now — so the only
// thing the button can usefully mean here is "abandon this", and both
// gestures count: a tap and a hold are equally that. Checked everywhere
// the flow waits or samples, so an abort never has to wait out a
// ten-second capture to take effect.
static bool calAbortRequested() {
  return menuTapRequested || menuSelectRequested;
}

// One phase: fill `hist`, summarise into `out`. Streaming min/max feed
// the progress screen; mean/sd come off the histogram afterwards.
// Returns false if the user asked to stop partway, in which case `out`
// is meaningless and the caller abandons the whole calibration.
static bool capturePhase(int phase, uint16_t *hist, CapSet &out) {
  memset(hist, 0, 4096 * sizeof(uint16_t));
  uint16_t mn = 4095, mx = 0;
  uint32_t n = 0;
  uint32_t start = millis(), lastDraw = 0;
  drawCaptureProgress(phase, 0, 0, 0);  // immediately, not 500ms late

  for (;;) {
    if (calAbortRequested()) return false;
    uint32_t elapsed = millis() - start;
    if (elapsed >= SENSOR_CAPTURE_MS) break;
    int v = analogRead(sensorPin());
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
  return true;
}

// The whole calibration, run linearly on the UI task from the menu's
// select handler. Blocking in here is fine — this task owns the screen
// and the sensor, and everything it would otherwise be doing is exactly
// what this flow is doing. It is also why the mid-flow press has to be
// asked of main.cpp and waited on rather than simply called: the send
// belongs on core 1, and this task is parked in here for twenty-odd
// seconds.
//
// Every wait in here is a vTaskDelay poll, never a spin: the flow lasts
// far longer than the 5s task watchdog would tolerate being held. Each
// one also checks calAbortRequested(), so the user's press gets out of
// any stage — including the middle of a capture — rather than being
// noticed whenever the next one ends.
static void runThresholdCal() {
  CapSet a, b;

  // Cleared up front rather than just before the results are filled in:
  // every early return below leaves this struct as the report, and a
  // previous run's numbers surviving an abort would be worse than no
  // report at all — the hold would then apply a threshold belonging to
  // some other display state entirely.
  cal = CalResult();

  // Let go and aim before anything is sampled. vTaskDelay rather than a
  // busy wait for the same reason capturePhase() yields every sample:
  // this task must not hold core 0 solid, and there is nothing to do
  // here but wait.
  drawCaptureReady();
  vTaskDelay(pdMS_TO_TICKS(CAL_START_DELAY_MS));

  if (!capturePhase(1, capHistA, a)) {
    cal.cancelled = true;
    return;
  }

  // The step that used to be the user's. One real press in the active
  // mode, made by main.cpp on the other core, which clicks whatever the
  // test page is and flips it to its other state — the same report a
  // finger on the button would have sent, which is the point: if this
  // device can change the display at all, this is how.
  drawCaptureSwitching(a);
  appCalPress();
  while (appCalPressBusy()) {
    if (calAbortRequested()) {
      // main.cpp still owes the release and will send it on its own
      // pacing; leaving it to do that is what keeps an abort from
      // handing the host a stuck button.
      cal.cancelled = true;
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  // Then let the display and the sensor finish arriving at the new state
  // before measuring it. See CAL_SETTLE_MS.
  uint32_t settleStart = millis();
  while ((uint32_t)(millis() - settleStart) < CAL_SETTLE_MS) {
    if (calAbortRequested()) {
      cal.cancelled = true;
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (!capturePhase(2, capHistB, b)) {
    cal.cancelled = true;
    return;
  }

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

// --- Validation run (the menu's "Validate" item) ------------------------
// A short auto test (VALIDATE_ITERATIONS presses, long settling gaps —
// main.cpp paces it, see appStartValidation()) whose product is a
// verdict, not a distribution. What it adds over a normal run:
//
//  * Idle watching. Between presses the UI task samples the sensor at
//    ~1kHz and counts confirmed threshold crossings. During a settling
//    gap no input has been sent, so every crossing counted there is a
//    light change nobody asked for — backlight PWM, pixel-inversion
//    flicker, mains-flickering room lights, a marginal threshold. This
//    is the number that explains impossibly-fast measurements: a source
//    of unrequested crossings will happily stop a real measurement's
//    clock a few hundred microseconds after the press.
//  * One-change-per-press checking. After a measurement's crossing, the
//    watcher keeps going for VAL_POSTWATCH_MS and counts any further
//    crossings — a press should change the light exactly once.
//  * A physical floor. Anything under VAL_FAST_US (1ms — a full-speed
//    USB device cannot even get its report polled faster than that) is
//    counted as impossible rather than banked as a latency.
//
// Validation samples never touch the stats, the histogram, or a run
// file — appRecordSample() is not called for them. The report is the
// product.

// One ~LIGHT_PERIOD_MS slice of idle watching, in place of the UI task's
// normal blocking wait. Returns early the moment any request lands so
// menu/press handling stays as responsive as the notify path it replaces.
static void validationIdleWatch() {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < LIGHT_PERIOD_MS) {
    if (measurePending || resetRequested || enterMenuRequested ||
        menuTapRequested || menuSelectRequested) return;
    int side = (analogRead(sensorPin()) >= lightThreshold) ? 1 : 0;
    if (valIdleSide < 0) {
      valIdleSide = side;
      valIdleRun = 0;
    } else if (side != valIdleSide) {
      if (++valIdleRun >= MEASURE_CONFIRM) {  // same noise gate as a measurement
        valIdleSide = side;
        valIdleRun = 0;
        val.idleFlips++;
        val.flipsSincePress++;
      }
    } else {
      valIdleRun = 0;
    }
    vTaskDelay(1);
  }
}

// The validation flavour of runMeasurement(): same baseline, same
// confirmed crossing, same timestamping — plus the post-crossing watch
// and the classification. Deliberately does NOT record into the stats,
// the histogram, or via appRecordSample().
static void runValidatedMeasurement(int64_t t0) {
  const uint16_t flipsBefore = val.flipsSincePress;
  val.flipsSincePress = 0;
  val.presses++;

  int32_t baselineSum = 0;
  for (int i = 0; i < 4; i++) baselineSum += analogRead(sensorPin());
  int baseline = baselineSum / 4;
  bool waitForRise = baseline < lightThreshold;
  int64_t deadline = t0 + (int64_t)MEASURE_TIMEOUT_MS * 1000;

  int64_t crossAt = 0;
  int run = 0;
  bool crossed = false;
  uint32_t us = 0;
  for (;;) {
    int v = analogRead(sensorPin());
    int64_t now = esp_timer_get_time();
    if (waitForRise ? (v >= lightThreshold) : (v < lightThreshold)) {
      if (run == 0) crossAt = now;
      if (++run >= MEASURE_CONFIRM) {
        us = (uint32_t)(crossAt - t0);
        crossed = true;
        break;
      }
    } else {
      run = 0;
    }
    if (now >= deadline) break;
  }

  if (!crossed) {
    val.timeouts++;
    valIdleSide = -1;
    return;
  }

  // The one-change-per-press check: stay on watch past the crossing and
  // count any further confirmed flips. Yields per sample — this loop
  // runs for VAL_POSTWATCH_MS, far past the tight poll's watchdog
  // licence.
  int extras = 0;
  int side = waitForRise ? 1 : 0;
  int erun = 0;
  uint32_t postStart = millis();
  while ((uint32_t)(millis() - postStart) < VAL_POSTWATCH_MS) {
    int v = analogRead(sensorPin());
    int es = (v >= lightThreshold) ? 1 : 0;
    if (es != side) {
      if (++erun >= MEASURE_CONFIRM) {
        side = es;
        erun = 0;
        extras++;
      }
    } else {
      erun = 0;
    }
    vTaskDelay(1);
  }

  bool fast = us < VAL_FAST_US;
  if (fast) val.fastCnt++;
  if (extras > 0) val.multiCnt++;
  if (!fast && extras == 0 && flipsBefore == 0) val.cleanCnt++;
  if (val.latCnt == 0 || us < val.minUs) val.minUs = us;
  val.sumUs += us;
  val.latCnt++;
  valIdleSide = side;  // the watcher's notion of "current side" stays fresh
  valIdleRun = 0;
}

// The validation report: counts first, verdict last — and the verdict
// names the likeliest culprit rather than leaving the counts to be
// decoded, because the whole point of the mode is to say what is wrong.
static void drawValReport() {
  uint16_t dim = dimColor();
  uint16_t warn = display.color565(255, 140, 60);
  uint16_t good = display.color565(120, 220, 130);
  char buf[32];

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("VALIDATION", SCREEN_W / 2, 4);

  snprintf(buf, sizeof(buf), "presses %u  timeout %u", val.presses, val.timeouts);
  display.drawString(buf, SCREEN_W / 2, 20);
  display.setTextColor(val.cleanCnt == val.presses ? good : dim, black());
  snprintf(buf, sizeof(buf), "clean %u/%u", val.cleanCnt, val.presses);
  display.drawString(buf, SCREEN_W / 2, 32);
  display.setTextColor((val.fastCnt || val.multiCnt) ? warn : dim, black());
  snprintf(buf, sizeof(buf), "fast<1ms %u  multi %u", val.fastCnt, val.multiCnt);
  display.drawString(buf, SCREEN_W / 2, 44);
  display.setTextColor(val.idleFlips ? warn : dim, black());
  snprintf(buf, sizeof(buf), "idle flips %lu", (unsigned long)val.idleFlips);
  display.drawString(buf, SCREEN_W / 2, 56);
  if (val.latCnt) {
    display.setTextColor(dim, black());
    uint32_t avg = val.sumUs / val.latCnt;
    snprintf(buf, sizeof(buf), "lat min %lu.%lu avg %lu.%lu",
             (unsigned long)(val.minUs / 1000), (unsigned long)((val.minUs % 1000) / 100),
             (unsigned long)(avg / 1000), (unsigned long)((avg % 1000) / 100));
    display.drawString(buf, SCREEN_W / 2, 68);
  }

  // The verdict, most damning evidence first: unrequested changes make
  // every other number unreliable, so they outrank everything.
  display.setTextColor(warn, black());
  if (val.idleFlips > 0) {
    display.drawString("light changes w/o input!", SCREEN_W / 2, 86);
    display.drawString("PWM/flicker or thin margin", SCREEN_W / 2, 96);
  } else if (val.fastCnt > 0) {
    display.drawString("impossibly fast crossings", SCREEN_W / 2, 86);
    display.drawString("during presses: re-cal thr", SCREEN_W / 2, 96);
  } else if (val.multiCnt > 0) {
    display.drawString("multiple changes per press", SCREEN_W / 2, 86);
  } else if (val.presses && val.cleanCnt == val.presses) {
    display.setTextColor(good, black());
    display.drawString("all clean", SCREEN_W / 2, 88);
  }

  display.setTextColor(dim, black());
  display.drawString("tap: done", SCREEN_W / 2, 112);
}

// The calibration's report. Suggested threshold big and amber, both
// sets' level and spread, and either the margin (clean) or the overlap
// count (not) — so "how safe is this threshold" is a number on the
// screen, not a feeling. Or, when the flow produced no threshold at all,
// the reason: an abort and a refusal both land here rather than dropping
// silently back to the meter, because a flow that just vanished would
// read as a crash.
static void drawCaptureResult() {
  uint16_t dim = dimColor();
  uint16_t amber = display.color565(255, 190, 40);
  uint16_t warn = display.color565(255, 140, 60);
  char buf[32];

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("THRESHOLD CAL", SCREEN_W / 2, 4);

  if (cal.noLink) {
    display.setFont(&fonts::Font2);
    display.setTextColor(warn, black());
    display.drawString("NO LINK", SCREEN_W / 2, 30);
    display.setFont(&fonts::Font0);
    display.setTextColor(dim, black());
    display.drawString("no host is listening,", SCREEN_W / 2, 62);
    display.drawString("so nothing would flip", SCREEN_W / 2, 74);
    display.drawString("tap: done", SCREEN_W / 2, 112);
    return;
  }
  if (cal.cancelled) {
    display.setFont(&fonts::Font2);
    display.setTextColor(warn, black());
    display.drawString("CANCELLED", SCREEN_W / 2, 30);
    display.setFont(&fonts::Font0);
    display.setTextColor(dim, black());
    display.drawString("no threshold measured", SCREEN_W / 2, 66);
    display.drawString("tap: done", SCREEN_W / 2, 112);
    return;
  }

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
  display.setTextColor(amber, black());
  snprintf(buf, sizeof(buf), "hold: use %d", cal.thr);
  display.drawString(buf, SCREEN_W / 2, 100);
  display.setTextColor(dim, black());
  display.drawString("tap: done", SCREEN_W / 2, 112);
}

// MENU_SENSOR: the sensor picker. Tap applies AND persists the next
// config on the spot — pins re-driven, threshold reloaded from that
// config's own calibration slot — so the hold that follows is only ever
// "I'm done looking". Switching clears the stats and histogram, since
// one sensor's numbers mean nothing against another's; that side effect
// is printed on the screen rather than left to be discovered.
static void drawMenuSensor() {
  uint16_t dim = dimColor();
  char buf[28];

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  display.drawString("SENSOR", SCREEN_W / 2, 6);

  display.setTextDatum(textdatum_t::middle_center);
  display.setTextColor(riseColor(), black());
  // Fixed Font2, same reasoning as the mode picker above. Both current
  // sensor names happen to fit Font4 ("Unit Light" 107px, "BPW34" 84px),
  // so this screen was not yet showing the two-sizes-in-one-list problem
  // — but it was one longer name away from it, and the two pickers
  // reading the same is worth more than the extra height.
  display.setFont(&fonts::Font2);
  display.drawString(SENSORS[activeSensor].name, SCREEN_W / 2, 48);

  display.setFont(&fonts::Font0);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextColor(dim, black());
  snprintf(buf, sizeof(buf), "pin %u  threshold %d",
           SENSORS[activeSensor].pin, lightThreshold);
  display.drawString(buf, SCREEN_W / 2, 66);
  display.drawString("switching clears stats", SCREEN_W / 2, 80);
  display.drawString("tap: next", SCREEN_W / 2, 96);
  display.drawString("hold: done", SCREEN_W / 2, 108);
}

static void drawFrame(Mode active, Mode pending, int raw, uint32_t mv, HoldRung hint,
                      LinkState link) {
  display.startWrite();
  display.fillScreen(black());

  if (menuState == MENU_TOP) {
    drawMenuTop();
  } else if (menuState == MENU_MODE) {
    drawMenuMode(active, pending);
  } else if (menuState == MENU_DRIVE) {
    drawMenuDrive(pending);
  } else if (menuState == MENU_CAPTURE) {
    drawCaptureResult();
  } else if (menuState == MENU_VAL) {
    drawValReport();
  } else if (menuState == MENU_SENSOR) {
    drawMenuSensor();
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
  {
    // Replay the persisted sensor selection — pins, ground, threshold,
    // the lot — before the first press could possibly be measured. The
    // default (index 0, Unit Light) is what a fresh device gets, which
    // is why the safe-with-nothing-wired config has to be index 0.
    uint8_t storedCfg = 0;
    Preferences p;
    if (p.begin(SENSOR_PREFS_NAMESPACE, true)) {
      storedCfg = p.getUChar(SENSOR_PREFS_CFG_KEY, 0);
      p.end();
    }
    activateSensor(storedCfg, false);
  }

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
  uint8_t shownSensor = 0xFF;  // != any real config, forces the first draw
  uint8_t shownFullStep = 0xFF, shownFullTotal = 0xFF;  // full-test progress line
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
      clearStats();
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
        // One list, two kinds of thing: the six modes in rotation order,
        // then USB DRIVE, then round to the first mode again.
        int cand = (pickerCandidate(pending) + 1) % (PICK_DRIVE + 1);
        if (cand == PICK_DRIVE) {
          // pendingMode is deliberately left where it was. It is
          // irrelevant while the drive is armed (boot ignores it), and
          // it is what the drive screen offers as the way back out.
          appSetStorageArmed(true);
        } else {
          // Picking a real mode MUST disarm: the armed flag wins at
          // boot, so leaving it set would make the mode just chosen look
          // ignored on the next reset.
          if (appStorageArmed()) appSetStorageArmed(false);
          appSetPendingMode((Mode)cand);
        }
      } else if (menuState == MENU_SENSOR) {
        // Applied and persisted per tap, same idiom as the other
        // pickers. Stats go with the old sensor — one circuit's numbers
        // mean nothing against another's — and statsCleared makes the
        // main screen redraw honest once the picker is left.
        activateSensor((activeSensor + 1) % SENSOR_COUNT, true);
        clearStats();
        statsCleared = true;
      }
      else if (menuState == MENU_CAPTURE || menuState == MENU_VAL) {
        menuState = MENU_NONE;  // the reports have one job: be read, then leave
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
              // flow starts and stays there throughout, so every press
              // of the user's during it is menu input and never a HID
              // send — the flow's own input event comes from main.cpp
              // instead (see runThresholdCal()), and the user's button
              // is the abort.
              menuState = MENU_CAPTURE;
              if (!appCanSendInput()) {
                // Nothing would receive the press that changes the
                // display, so the captures would be of one state twice.
                // Refusing and saying why beats spending twenty seconds
                // to produce a report blaming the sensor — the same
                // judgement main.cpp makes about starting an auto test
                // with no link.
                cal = CalResult();
                cal.noLink = true;
              } else {
                // Blocks right here for the whole flow: two captures,
                // the press between them and the settle after it.
                runThresholdCal();
              }
              // Whatever ended the flow — finished, aborted or refused —
              // the button state it ended on belongs to that flow, not
              // to the report now on screen. Clearing both is what stops
              // the very press that cancelled a calibration from also
              // dismissing the screen explaining that it did.
              menuTapRequested = false;
              menuSelectRequested = false;
            } else {
              validationActive = false;  // a stale flag must not colour this run
              appStartAutoTest();
              menuState = MENU_NONE;
            }
            break;
          case ITEM_VALIDATE:
            // Fresh counters per run; the report is only ever about the
            // run that just happened. validationActive arms the board's
            // half (idle watching, validated measurements) — and is also
            // what routes the run-end signal to the report screen.
            val = ValStats();
            valIdleSide = -1;
            validationActive = true;
            appStartValidation();
            menuState = MENU_NONE;
            break;
          case ITEM_SENSOR: menuState = MENU_SENSOR; break;
          case ITEM_FULL:
            // Six auto tests, five reboots, unattended — main.cpp owns
            // all of it from here; any press stops the whole thing.
            appStartFullTest();
            menuState = MENU_NONE;
            break;
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
      } else if (menuState == MENU_MODE || menuState == MENU_VAL ||
                 menuState == MENU_SENSOR) {
        // The pickers persist per tap and the validation report is only
        // a report — in all three, a hold just leaves.
        menuState = MENU_NONE;
      } else if (menuState == MENU_CAPTURE) {
        // On the report, hold means "use it": the calibrated threshold
        // becomes the live one and is persisted, so it survives reboots
        // and outranks the build flag from now on (see lightThreshold's
        // comment; tap remains leave-without-applying). The NVS write
        // stalls the flash cache on both cores — harmless precisely
        // here, mid-menu, with no run going and no measurement
        // outstanding; the same licence the pairing item has.
        if (cal.done && cal.thr > 0 && cal.thr < 4096) {
          lightThreshold = cal.thr;
          char key[8];
          sensorThrKey(activeSensor, key, sizeof(key));  // a calibration belongs to its sensor
          Preferences p;
          if (p.begin(SENSOR_PREFS_NAMESPACE, false)) {
            p.putUInt(key, (uint32_t)cal.thr);
            p.end();
          }
        }
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
    // Tracked unconditionally: the armed flag is now visible on three
    // screens — the mode picker (as its seventh candidate), the drive
    // screen, and the normal screen's next line ("-> USB DRIVE") — so
    // "which screen are we on" is no longer a useful filter on it.
    bool armed = appStorageArmed();
    bool ejected = storageEjected;
    bool newFrame = (active != shownActive || pending != shownPending ||
                     meterView != shownMeter ||
                     menuState != shownMenuState || topIndex != shownTopIndex ||
                     armed != shownArmed || ejected != shownEjected ||
                     link != shownLink || activeSensor != shownSensor ||
                     fullStepShown != shownFullStep || fullTotalShown != shownFullTotal);
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
      if (validationActive && autoTotal > 0) {
        // A validation run's press: measured the same way, then watched
        // past the crossing, classified, and kept out of the stats.
        runValidatedMeasurement(pressMicros);
      } else {
        runMeasurement(pressMicros);
        histDirty = true;
        if (!meterView) topDirty = true;
      }
      measureBusy = false;  // releases the automated test's next press
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
        raw = analogRead(sensorPin());
        mv = analogReadMilliVolts(sensorPin());
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
        shownSensor = activeSensor;
        shownFullStep = fullStepShown;
        shownFullTotal = fullTotalShown;
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

    // A validation run ending (or aborting) is detected here, as the
    // autoTotal handoff from main.cpp dropping back to zero, and lands
    // on the report screen. Detected on this task rather than acted on
    // in boardShowAutoTest() because menuState belongs to this task.
    {
      static uint16_t prevAutoTotal = 0;
      uint16_t curTotal = autoTotal;
      if (validationActive && prevAutoTotal > 0 && curTotal == 0) {
        validationActive = false;
        menuState = MENU_VAL;
      }
      prevAutoTotal = curTotal;
    }

    // Wake on the next press/release edge or hold rung, or after
    // LIGHT_PERIOD_MS to re-meter the sensor — whichever comes first.
    // During a validation run's gaps the wait is spent watching the
    // sensor instead of blocking: an idle crossing found there is the
    // whole reason the mode exists. The watch polls the same request
    // flags the notify path would wake for, so responsiveness holds.
    if (validationActive && autoTotal > 0 && !measurePending) {
      validationIdleWatch();
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LIGHT_PERIOD_MS));
    }
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

void boardShowFullTest(uint8_t step, uint8_t total) {
  fullStepShown = step;
  fullTotalShown = total;
  nudgeUi();
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
  if (run.fullTotal > 0) {
    // Full-test membership, so the six files of one test can be grouped
    // after the fact. Unknown "# key,value" lines are ignored by the
    // analyzer's parser by construction, so old tooling stays happy.
    runEmit(f, "# fulltest,%u\n", (unsigned)run.fullTestId);
    runEmit(f, "# fullstep,%u/%u\n", (unsigned)run.fullStep, (unsigned)run.fullTotal);
  }
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
// it always was. Nothing else is conditional: Auto test and the mode
// picker (whose list carries the USB drive) are as useful over the radio
// as over the wire, which is rather the point of having BLE modes at
// all.
static void buildTopMenu(Mode active) {
  // Ordered by how often a session reaches for them, not by when they
  // were written: Auto test and Change mode are what the menu is opened
  // for most, so they are the two rows a tap-driven list should cost
  // least to reach. Pairing keeps its place among the frequent items
  // (in a BLE mode it is the thing you need when nothing works), and
  // Validate/Sensor/Exit — setup and diagnostics, reached deliberately
  // rather than often — sit at the bottom.
  numTopItems = 0;
  topItems[numTopItems++] = ITEM_AUTO;
  topItems[numTopItems++] = ITEM_MODE;
  topItems[numTopItems++] = ITEM_METER;
  if (modeIsBle(active)) topItems[numTopItems++] = ITEM_PAIR;
  topItems[numTopItems++] = ITEM_VALIDATE;
  topItems[numTopItems++] = ITEM_SENSOR;
  topItems[numTopItems++] = ITEM_EXIT;
  // Below Exit on purpose — the bottom of the list, per the enum comment.
  topItems[numTopItems++] = ITEM_FULL;
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
