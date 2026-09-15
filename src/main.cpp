/*
 * USB HID Gamepad / Keyboard / Mouse (single active device), or a drive
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
 *   the automated test (see appStartAutoTest()), arming the USB drive
 *   (see appSetStorageArmed()) and changing the pending mode (see
 *   appAdvancePendingMode()). The first and last used to be hold-ladder
 *   rungs of their own; moving them into a proper menu is what let
 *   short-press-to-advance and hold-to-select behave the same way at
 *   every level, instead of every feature inventing its own hold
 *   duration to remember — and left somewhere obvious to put the rest.
 *
 *   While an automated run is going, the button does one thing only:
 *   stop it. Nothing else — no HID report, no hold rungs — because the
 *   act of stopping a run must not land in that run's own data.
 *
 *   Every run — finished or stopped early — is written out as a CSV file
 *   on boards that have somewhere to put one. The samples are buffered
 *   here in RAM for the whole run and handed down to boardWriteRun() once
 *   at the end; see the recording block below for why they cannot be
 *   written as they arrive.
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
 *   The USB drive (MODE_STORAGE) is the opposite call, and worth
 *   contrasting with the meter for that reason. It IS a Mode: it changes
 *   what the device enumerates as, it needs the descriptor decided before
 *   USB.begin(), and so it cannot be entered or left without a reboot —
 *   every cost the meter was moved out of Mode-hood to avoid, this one
 *   genuinely incurs. On a board that has a filesystem partition, arming
 *   it (menu -> USB drive) makes the NEXT boot come up as a small
 *   mass-storage device whose blocks are the `ffat` partition itself, so
 *   the host mounts the run CSVs with no firmware in the loop. That boot
 *   has no HID device at all — a press sends nothing, which sendPress()
 *   already gets right for free via its `default:` arm — and, critically,
 *   the firmware does not mount FFat on that partition while the host has
 *   it (two writers on one FAT volume is silent corruption). It stays a
 *   drive across replugs until disarmed from the drive screen: a hold,
 *   then a manual reset. MODE_STORAGE deliberately sits outside the
 *   `% MODE_COUNT` rotation so no amount of holding can reach it — see
 *   mode.h.
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
#include <USBMSC.h>

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
// Whether the next boot should come up as a USB drive instead of a HID
// device. A key of its own rather than a fourth value in PREFS_KEY: the
// HID mode has to go on being remembered while storage is armed, or
// leaving storage mode would have nowhere to return to. Two values were
// needed either way, so they are two keys.
static const char *PREFS_STORAGE_KEY = "storage";

static Preferences prefs;
static Mode activeMode = MODE_GAMEPAD;   // mode this boot actually enumerated as
static Mode pendingMode = MODE_GAMEPAD;  // mode a reboot would pick up; dialed in by holding
// Sticky, not one-shot: a drive that reverted to a gamepad on every
// replug would be useless for the one thing it exists for — carrying run
// files to whichever machine you want to read them on. It stays a drive
// until someone says otherwise, and saying otherwise is one hold on the
// screen (see MENU_DRIVE in board_atoms3r.cpp).
static bool storageArmed = false;

// Manual CDC serial (see file header on why ARDUINO_USB_CDC_ON_BOOT is 0).
static USBCDC USBSerial;

// Only the active mode's device is ever constructed (see file header) —
// the other two pointers stay null for the life of this boot.
static USBHIDGamepad *gamepad = nullptr;
static USBHIDKeyboard *keyboard = nullptr;
static USBHIDMouse *mouse = nullptr;
// ...and the fourth identity, which is not a HID device at all. Same
// rule, same reason: USBMSC's constructor registers the mass-storage
// interface with TinyUSB the moment it runs, exactly as the HID classes
// register their report descriptors, so it may only be constructed on a
// boot that means to be a drive.
static USBMSC *msc = nullptr;

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

// --- Recording a run --------------------------------------------------
// Every run is written out as a file when it ends (see board.h's
// RunRecord contract and board_atoms3r.cpp for the format). The samples
// are buffered here, in RAM, for the whole run: 500 of them is 4KB
// against this chip's 320KB, and the alternative — appending to a file
// per iteration — would put a flash erase inside the measurement window
// of whichever press came next. Flash writes stall the cache and
// therefore both cores, so one write at the end is not a convenience,
// it's the only placement that doesn't corrupt the data being collected.
static RunSample runSamples[AUTO_TEST_ITERATIONS];
static uint16_t runCount = 0;     // samples collected in the run so far
static bool runRecording = false; // drop samples from ordinary manual presses
static bool runAborted = false;   // the run was stopped by a press, not finished
// Set when a run ends and cleared once the file is out. Deferred rather
// than written on the spot because an abort can land mid-measurement:
// the press that stops a run is handled on core 1 while core 0 may still
// be timing the previous one, and that last sample belongs in the file.
// See serviceAutoTest(), which flushes as soon as the board reports no
// measurement outstanding.
static bool runFlushPending = false;
// True only when the run ended with one of its own measurements still
// running — i.e. it was aborted mid-press. It is what stops the flush
// from waiting on, and then swallowing, a measurement belonging to some
// manual press the user made in the meantime: a run that ended normally
// already collected its last sample, so it never waits at all.
static bool runAwaitSample = false;
// The gap this press was preceded by, remembered from when it was rolled
// so it can be paired with the measurement that comes back.
static uint16_t runGapMs = 0;

// The board's half of a sample, handed up from its measurement code on
// core 0. One slot, not a queue: a measurement is strictly one at a time
// (the run won't start the next press until boardMeasurementBusy() goes
// false), and the board sets this before it drops that flag — so by the
// time core 1 is allowed to look, the value is already here. Same
// handover as boardShowPress()'s pressMicros, in the other direction.
static volatile uint32_t sampleLatencyUs = 0;
static volatile bool sampleRise = false;
static volatile bool sampleTimedOut = false;
static volatile bool sampleReady = false;  // set on core 0, cleared on core 1

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

// The storage flag's two accessors, called by the board's USB-drive
// picker and by the drive screen's disarm hold. They live here for the
// same reason the mode ones do: this file owns the "usbmode" namespace
// and the question of what a reboot will come up as. Note what they do
// NOT do — nothing about this boot changes, no device is created or torn
// down, and nothing reboots. Arming a drive is exactly as inert as
// queueing a mode change, and for exactly the same reason (see the file
// header on why the reboot has to be the user's).
void appSetStorageArmed(bool armed) {
  if (!boardHasStorage()) return;  // nothing to expose; refuse to remember one
  storageArmed = armed;
  prefs.putBool(PREFS_STORAGE_KEY, armed);
}

bool appStorageArmed() {
  return storageArmed;
}

// --- USB mass storage callbacks ---------------------------------------
// TinyUSB wants plain function pointers, and everything they need is the
// board's (which partition, which blocks) — so these are three-line
// trampolines and the real work is behind board.h. They run on the
// TinyUSB task, never from loop().
static int32_t mscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  return boardStorageRead(lba, offset, buffer, bufsize);
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  return boardStorageWrite(lba, offset, buffer, bufsize);
}

static bool mscStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  // The host finished with the volume: caches flushed, nothing more
  // coming. Dropping mediaPresent stops it polling a disk it has already
  // let go of, and the screen gets to say "safe to unplug" — which is
  // the only honest moment to say it, since a drive boot is left by
  // pulling the plug or pressing reset.
  if (load_eject && !start && msc) msc->mediaPresent(false);
  if (load_eject && !start) boardShowStorageEjected();
  return true;
}

// The board's half of one sample, arriving from its measurement code on
// core 0 the instant a measurement resolves — before the board drops the
// flag boardMeasurementBusy() reports, which is what makes it safe for
// core 1 to read this the moment it sees that flag clear.
//
// Every measurement lands here, including the manual presses that have
// nothing to do with a run. Nothing is filtered out at this end: the
// stash is a single slot that the next press overwrites anyway, and only
// the run sequencer below ever promotes one into the buffer. That keeps
// this function to four stores, which matters because it runs on the
// same core, and in the same breath, as the measurement itself.
void appRecordSample(uint32_t latencyUs, bool rise, bool timedOut) {
  sampleLatencyUs = latencyUs;
  sampleRise = rise;
  sampleTimedOut = timedOut;
  sampleReady = true;
}

// Move whatever the board left in the stash into the run's buffer,
// pairing it with the gap this press was preceded by — the half of a
// sample only this file knows. Core 1 only, and only ever with no
// measurement outstanding.
static void collectSample() {
  if (!sampleReady) return;  // e.g. a measurement the hold ceiling gave up on
  sampleReady = false;
  if (!runRecording || runCount >= AUTO_TEST_ITERATIONS) return;

  RunSample &s = runSamples[runCount++];
  s.latencyUs = sampleLatencyUs;
  s.gapMs = runGapMs;
  s.rise = sampleRise;
  s.timedOut = sampleTimedOut;
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
  // Aborting mid-press leaves that press's measurement running on the
  // other core; its result is a perfectly good sample and the file waits
  // for it.
  runAwaitSample = (autoPhase == AUTO_HOLDING);
  autoPhase = AUTO_OFF;
  boardShowAutoTest(0, 0);
  // Everything collected so far still gets written out; an aborted run
  // is usually the interesting one (you stopped it because you saw
  // something), so throwing its samples away would be exactly backwards.
  // The write itself waits — see runFlushPending, and finishRun() below.
  runAborted = true;
  runFlushPending = true;
}

void appStartAutoTest() {
  autoStartRequested = true;
}

// The one place a run's file is written. Deliberately not called from
// wherever a run happens to end: an abort is handled on core 1 at the
// press edge, which can be partway through core 0's measurement of the
// previous press, and that sample belongs in the file. So the end of a
// run only raises runFlushPending, and this waits for the board to
// report nothing outstanding before collecting the straggler and
// writing.
//
// The write blocks for as long as flash takes — tens of milliseconds,
// maybe more — and that is fine precisely here and nowhere else: the run
// is over, no measurement can be in flight, and the worst it costs is a
// button press going unnoticed while a file that is already fully
// determined gets stored.
static void finishRun() {
  if (runAwaitSample) {
    if (boardMeasurementBusy()) return;
    collectSample();
    runAwaitSample = false;
  }

  runFlushPending = false;
  runRecording = false;

  RunRecord rec;
  rec.mode = activeMode;
  rec.planned = AUTO_TEST_ITERATIONS;
  rec.count = runCount;
  rec.aborted = runAborted;
  rec.samples = runSamples;
  boardWriteRun(rec);
}

// Drives the automated run. Called every loop() pass, including while
// the button is released — which is the normal case, since a run happens
// with nobody touching the device.
static void serviceAutoTest(uint32_t now) {
  // Before anything else: a run that has ended still owes a file, and
  // nothing new may start until it's out.
  if (runFlushPending) {
    finishRun();
    if (runFlushPending) return;
  }

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
    runCount = 0;
    runAborted = false;
    runRecording = true;
    sampleReady = false;  // anything left over belongs to a manual press
    // A normal gap before the first press too, rather than firing the
    // instant the button comes up: the button is the screen face, inches
    // from wherever the sensor is aimed, so letting go of it is exactly
    // the moment not to be taking a reading.
    runGapMs = autoGapMs();
    autoResumeAtMs = now + runGapMs;
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
      // runGapMs is already the gap that just elapsed (rolled when this
      // phase was entered); it stays put until the next one is rolled, so
      // the measurement about to come back is paired with the right one.
      sampleReady = false;
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

      // Getting here means the measurement is done (or the hold ceiling
      // gave up on it, in which case there is nothing to collect and the
      // iteration simply contributes no row) — so the board's half of
      // this sample is already in the stash, still paired with the gap
      // that preceded this press.
      collectSample();

      if (autoDone >= AUTO_TEST_ITERATIONS) {
        autoPhase = AUTO_OFF;
        boardShowAutoTest(0, 0);
        runFlushPending = true;  // written on the next pass, by finishRun()
      } else {
        autoPhase = AUTO_GAP;
        runGapMs = autoGapMs();
        autoResumeAtMs = now + runGapMs;
        boardShowAutoTest(autoDone, AUTO_TEST_ITERATIONS);
      }
      return;
  }
}

void setup() {
  boardBegin();

  prefs.begin(PREFS_NAMESPACE, false);
  // `stored < MODE_COUNT` is the validation, and it keeps working
  // unchanged now that MODE_STORAGE exists — precisely because this key
  // only ever holds one of the three HID identities. Storage lives in its
  // own key, so a stored value of MODE_STORAGE here would still be
  // rejected as the corruption it would be.
  uint8_t stored = prefs.getUChar(PREFS_KEY, MODE_GAMEPAD);
  activeMode = (stored < MODE_COUNT) ? static_cast<Mode>(stored) : MODE_GAMEPAD;
  pendingMode = activeMode;
  // Gated on the board, not just on the flag: a board with no partition
  // to expose must never boot into a drive, whatever its NVS says — and
  // on such a board this is a compile-time false, since exactly one
  // board_*.cpp is linked.
  storageArmed = boardHasStorage() && prefs.getBool(PREFS_STORAGE_KEY, false);

  // Construct the one active-mode device, and set the product name to
  // match, before USB.begin() — both are rejected as no-ops afterwards.
  //
  // Storage first, because it is the identity that replaces the other
  // three rather than joining them. activeMode is only moved to
  // MODE_STORAGE once the partition is actually in hand: if the mount
  // fails there is nothing to expose, and coming up as the HID mode that
  // is still sitting in NVS beats enumerating a drive with no blocks
  // behind it. pendingMode is left pointing at that HID mode either way —
  // it is what the screen offers as the way back out.
  if (storageArmed) {
    uint32_t blockCount = 0;
    uint16_t blockSize = 0;
    if (boardStorageBegin(&blockCount, &blockSize)) {
      activeMode = MODE_STORAGE;
      msc = new USBMSC();
      msc->vendorID("USBLAT");
      msc->productID("Latency Runs");
      msc->productRevision("1.0");
      msc->onRead(mscRead);
      msc->onWrite(mscWrite);
      msc->onStartStop(mscStartStop);
      msc->mediaPresent(true);
      // Callbacks before begin(): begin() refuses if either is still
      // unset, and returns false rather than saying why.
      msc->begin(blockCount, blockSize);
    }
  }

  switch (activeMode) {
    case MODE_GAMEPAD:  gamepad = new USBHIDGamepad();  gamepad->begin();  break;
    case MODE_KEYBOARD: keyboard = new USBHIDKeyboard(); keyboard->begin(); break;
    case MODE_MOUSE:    mouse = new USBHIDMouse();      mouse->begin();    break;
    // MODE_STORAGE lands here, and landing here is the entire point: no
    // HID device is constructed, so nothing contributes a report
    // descriptor and the device enumerates as a plain mass-storage
    // gadget. Same `default:` arm that makes sendPress() send nothing.
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
