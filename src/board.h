/*
 * Board I/O abstraction: the button the user presses, and whatever the
 * board has to show state on.
 *
 * Exactly one board_*.cpp is compiled per PlatformIO env (selected with
 * build_src_filter in platformio.ini), so there is no runtime dispatch
 * and no board #ifdefs anywhere in main.cpp:
 *
 *   env:esp32-s3-zero    -> board_s3zero.cpp   (BOOT button, WS2812 LED)
 *   env:m5stack-atoms3r  -> board_atoms3r.cpp  (screen button, 128x128 LCD)
 *
 * Everything mode-related is passed in rather than kept here, so the
 * board layer holds no copy of state main.cpp already owns. Nothing in a
 * board_*.cpp touches USB or the radio — those belong to main.cpp alone,
 * which is why anything BLE-shaped in this contract (boardShowLink(),
 * appBlePairingMode()) is a value being pushed down or a request being
 * passed up, never a call into NimBLE.
 */
#pragma once

#include "mode.h"

// First thing in setup(), before any USB configuration. Must only set up
// the button: anything slow (an LCD) belongs in boardShowBoot() instead,
// so it doesn't sit between power-up and USB.begin().
void boardBegin();

// True if this board can time the display's response to a press — i.e. it
// has both a light sensor and somewhere to show the result. This is what
// decides whether the hold ladder in main.cpp has its reset-stats and
// toggle-meter rungs at all; a board without a sensor keeps the plain
// one-rung ladder it always had.
bool boardHasSensor();

// Throw away the accumulated measurement statistics and start counting
// again. Called from the hold ladder; a no-op without a sensor.
void boardResetStats();

// What continuing to hold the button would do next, in normal (non-menu)
// operation. main.cpp owns the ladder's timing and pushes this whenever
// the answer changes (including on release, to reset the hint), so the
// board never duplicates the thresholds — it just renders the label.
// Meaningless, and never sent, once boardMenuActive() is true: the menu
// has its own on-screen "tap: next, hold: select" hint instead.
enum HoldRung : uint8_t {
  RUNG_STATS,  // keep holding -> reset stats
  RUNG_MENU,   // keep holding -> open the menu
};
void boardShowHoldHint(HoldRung next);

// --- The menu ----------------------------------------------------------
// A board with a sensor gets a small on-screen menu for things that used
// to be hold-ladder rungs of their own (the light meter, changing the
// pending mode): opened by a 2s hold from normal operation, after which
// every press is menu input instead of a HID send — until the menu picks
// its own way back out. main.cpp decides, once per press edge, whether
// to run the normal HID/measurement path or this one instead; see the
// "wasMenuActiveAtPress" comment in main.cpp for why that decision is
// captured once per press rather than re-checked live.
//
// True while the menu owns the button. A board without a sensor has no
// menu to open and always returns false, so main.cpp's normal path runs
// unconditionally there, exactly as before this existed.
//
// A MODE_STORAGE boot is the other extreme: it returns true from the
// first loop() pass and never goes back, because the drive screen is the
// only screen that boot has. That one answer is what makes storage mode
// need no special case in main.cpp's press path at all — every press is
// already routed to boardMenuTap()/boardMenuSelect() instead of to HID
// and the measurement, the hold ladder is already skipped, and the
// automated test is already unreachable (its menu item cannot be
// highlighted from a screen that has no items).
bool boardMenuActive();

// Fired once, when a hold crosses the "open the menu" threshold during
// normal operation. Takes the button away from HID/measurement duty
// until the menu exits on its own.
void boardEnterMenu();

// A short press-and-release while the menu is open: advance — move the
// highlighted item, or, inside the mode-picker, advance the candidate
// mode via appAdvancePendingMode() below.
void boardMenuTap();

// A hold of a second or more while the menu is open: trigger the
// highlighted item's action. What that means depends on the item —
// entering a submenu, toggling something and returning to normal
// operation, or just leaving the menu.
void boardMenuSelect();

// True from the moment a press hands a measurement over until that
// measurement has finished — i.e. it covers the whole span, unlike the
// board's internal "a measurement is queued" flag. The automated test
// uses it to pace itself one press per *completed* measurement instead
// of guessing an interval long enough to cover the worst case. Always
// false on a board with no sensor, which has no measurement to be busy
// with.
bool boardMeasurementBusy();

// Progress readout while the automated test runs: `done` out of `total`.
// total == 0 means "not running", which puts the normal display back.
// A no-op on a board with no screen to show it on.
void boardShowAutoTest(uint16_t done, uint16_t total);

// Raw, undebounced button state; true while held. main.cpp does the
// debouncing, so this should be a plain read with no delay in it.
bool boardButtonPressed();

// End of setup(), after USB.begin(). Brings up the display (if any) and
// shows which mode this boot actually enumerated as — or, on a board that
// renders from its own task, starts that task and lets it do the work.
void boardShowBoot(Mode active, Mode pending);

// Press/release feedback. Called after the HID report has already been
// sent, so it is never in the latency path. Implementations are free to
// either draw inline (the LED, which is microseconds) or just hand the
// state to another core and return (the LCD, which is milliseconds).
//
// atMicros is esp_timer_get_time() sampled at the button edge itself,
// before the HID report was queued — i.e. t0 for any measurement of how
// long the machine takes to respond. A board with a light sensor starts
// its clock from this; one without just ignores it.
//
// `sent` is false when the press produced no report at all, which in
// practice means a BLE mode with no host connected (see boardShowLink()
// below). It matters because the honest response to it is to NOT start a
// measurement: nothing left the device, so nothing is going to change on
// the display, and timing that would only add a guaranteed
// MEASURE_TIMEOUT_MS sample to the statistics and a `timeout` row to any
// run file. A board should show the press somehow and skip the clock.
void boardShowPress(bool pressed, Mode active, int64_t atMicros, bool sent);

// --- BLE link state ----------------------------------------------------
// Only meaningful in one of the three BLE modes; main.cpp pushes it
// whenever the radio's state changes, which includes from inside NimBLE's
// own callbacks on the host task — so an implementation must be as cheap
// as every other cross-core entry point here (one store and a poke), not
// draw anything itself.
//
// Why the board needs it at all: in a BLE mode a press only does
// something when a host is connected, and "nothing happened" is otherwise
// indistinguishable from a broken button. The state is shown
// continuously rather than as a reaction to a press, because on the one
// board that has a screen the screen cannot repaint promptly anyway (see
// the measurement comment in board_atoms3r.cpp).
enum LinkState : uint8_t {
  LINK_NONE,         // not a BLE mode; nothing to show
  LINK_ADVERTISING,  // radio up, waiting for a host to connect
  LINK_PAIRING,      // as above, but bonds were just cleared (see appBlePairingMode)
  LINK_CONNECTED,    // a host is connected; presses go somewhere
};
void boardShowLink(LinkState state);

// Implemented in main.cpp; called by the board's menu "Pairing" item,
// which only exists in a BLE mode. See main.cpp for what pairing mode
// concretely does — the short version is that it forgets every bonded
// host and starts advertising again, because a stale bond, not a lack of
// discoverability, is what actually stops a new host connecting.
void appBlePairingMode();

// Called after a hold has advanced the pending mode — the mode a future
// reboot will come up in, which is not the one running now.
// firstOfHold is true only for the first advance within a given hold, for
// boards that want a one-off "something changed" cue before settling into
// showing the new pending mode. Always false when driven by the menu's
// mode picker (appAdvancePendingMode()) — a discrete tap, not a hold, so
// "first within a hold" doesn't apply there.
void boardShowPending(Mode active, Mode pending, bool firstOfHold);

// Implemented in main.cpp; called by the board's mode-picker submenu on
// each advancing tap. Advances and persists pendingMode exactly as the
// S3-Zero's hold-to-cycle gesture always has, and reports the result
// back through boardShowPending() above so the picker can show it. A
// board with no menu never calls this.
void appAdvancePendingMode();

// Implemented in main.cpp; called by the menu's "Auto test" item. Starts
// an unattended run of AUTO_TEST_ITERATIONS presses in the current mode,
// spaced by a random gap, each one going through exactly the same
// send-and-measure path a real button press does. It lives in main.cpp
// rather than the board layer for the same reason the mode picker does:
// only main.cpp may touch USB, and an automated press has to be a real
// HID report or it isn't measuring the same thing. A board with no menu
// never calls this.
void appStartAutoTest();

// Implemented in main.cpp; called by the menu's "Validate" item. Same
// machinery as appStartAutoTest() but VALIDATE_ITERATIONS presses with a
// much longer settling gap — the pacing half of the validation mode whose
// observation half (idle-crossing counting, one-change-per-press
// checking) the board runs itself.
void appStartValidation();

// Implemented in main.cpp; the threshold calibration's one input event.
// The calibration needs the display under test to change state between
// its two captures, and the firmware now makes that happen itself
// instead of asking the user to. Only main.cpp may touch USB or the
// radio, so only main.cpp can send it.
//
// Asynchronous, because the send has to happen on core 1 while the board
// is blocked in its calibration on core 0: appCalPress() only requests
// one, and appCalPressBusy() stays true until the release has gone out.
// It is deliberately NOT a measurement — boardShowPress() is not called
// for it, nothing is timed, and no sample exists. See main.cpp.
void appCalPress();
bool appCalPressBusy();

// Implemented in main.cpp; "would a press actually reach a host right
// now?". True in every USB mode, and in a BLE mode only once a host has
// subscribed. The board asks before starting a calibration, because the
// press above is the entire mechanism by which the display changes: if
// it goes nowhere, both captures measure the same state and the report
// blames the sensor for it.
bool appCanSendInput();

// --- Recording an automated run ----------------------------------------
// Every automated run is written out as a CSV file on the board's flash
// filesystem, so a distribution can be looked at properly afterwards
// instead of being read off a 128x128 screen while it happens. (The same
// partition is what a MODE_STORAGE boot hands to the host raw — see the
// USB mass storage block below — which is why the on-flash format is
// plain FAT with 8.3 names; see board_atoms3r.cpp.)
//
// The two halves of a sample are known by different owners, which is the
// whole reason this needs a contract rather than one file doing it all:
// main.cpp paces the run and therefore knows the gap it left before each
// press, while the board did the measuring and therefore knows what came
// back. The board pushes its half up through appRecordSample() below,
// main.cpp joins the two and buffers the result in RAM, and the finished
// run comes back down to boardWriteRun() to be stored.
//
// Nothing here writes to flash while a run is in progress, and that is
// not an implementation detail to be optimised away later: an erase or a
// program cycle stalls the flash cache, which stalls code execution on
// *both* cores, which would land squarely inside some other iteration's
// measurement. One write, after the last sample is in.
struct RunSample {
  uint32_t latencyUs;  // meaningless, and not written out, when timedOut
  uint16_t gapMs;      // idle time main.cpp left before this press
  bool rise;           // true = the sensor crossed the threshold upward
  bool timedOut;       // nothing crossed before the board gave up
};

// A whole run, as handed to the board for storage. `planned` is the
// iteration count the run set out to do and `count` is how many samples
// actually came back, so the two differing is the interesting part: a
// run stopped early has aborted set, and a run that finished can still
// be short by the odd sample a wedged measurement swallowed.
struct RunRecord {
  Mode mode;                   // the USB identity every sample was taken in
  uint16_t planned;            // iterations the run set out to do
  uint16_t count;              // samples actually collected
  bool aborted;                // stopped early by a press
  const RunSample *samples;    // count entries, oldest first
};

// Store a finished (or aborted) run. Called from loop() on core 1 once
// the run is over and no measurement is outstanding — never during one.
// Expected to block for as long as the write takes; there is nothing
// left to disturb by then, which is exactly why the call is deferred to
// this point rather than made per sample.
//
// A board with no filesystem — and, having no sensor, no way to have
// produced a sample in the first place — does nothing here.
void boardWriteRun(const RunRecord &run);

// --- USB mass storage --------------------------------------------------
// A seventh identity (MODE_STORAGE), on boards that have a filesystem
// partition worth exposing: instead of a HID device, the board enumerates
// as a small thumb drive whose blocks ARE the `ffat` partition, so the
// host mounts the run CSVs directly with no firmware in the loop.
//
// The split of duties is the same one as everywhere else, just applied to
// a new pair of things: only main.cpp may touch USB, so main.cpp
// constructs USBMSC and owns its callbacks; only the board knows where
// its flash is, so the board owns the partition and the block I/O. The
// three functions below are the seam between them.
//
// The hard rule that makes any of it safe: in a MODE_STORAGE boot the
// firmware must NOT also have FFat mounted on that partition. Two writers
// on one FAT volume — the host's cached view and FatFs's — corrupt it,
// and neither side has any way to notice. So the board skips its mount
// entirely when it comes up in MODE_STORAGE, which also makes run
// recording inert for that boot (nothing to write to, and nothing that
// could start a run anyway — see boardMenuActive() below).

// True if this board has a partition to expose. Gates everything else
// here, including whether main.cpp consults the persisted armed flag at
// all — a board with no storage partition must never boot into a mode it
// cannot implement, however its NVS got written.
bool boardHasStorage();

// Prepare the partition for raw block access and report its geometry, in
// the units USB mass storage speaks. Called from setup() on a
// MODE_STORAGE boot only, BEFORE USB.begin(), and never on a boot where
// the filesystem is mounted normally.
//
// Returning false is not fatal: main.cpp falls back to the persisted HID
// mode for that boot, on the grounds that a working latency tester beats
// a device that enumerates as nothing.
bool boardStorageBegin(uint32_t *blockCount, uint16_t *blockSize);

// Raw block I/O, wired straight to the MSC read/write callbacks — so
// these run on the TinyUSB task, not loop(), and `size` may cover less
// than a whole block (`offset` says where within it). Return the number
// of bytes handled, or a negative value to fail the SCSI command.
//
// Blocking on flash is fine here and nowhere else in this firmware:
// nothing is being timed in a MODE_STORAGE boot, so the cache stall a
// write costs lands in no measurement.
int32_t boardStorageRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size);
int32_t boardStorageWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t size);

// The host issued START STOP UNIT with eject set — it has flushed its
// caches and let go of the volume. Purely a display cue: it is the one
// moment the device can honestly say "safe to unplug now". Called from
// the TinyUSB task, so implementations must be as cheap as the other
// cross-core pokes here.
void boardShowStorageEjected();

// Implemented in main.cpp; the persisted "next boot is a drive" flag,
// which lives with the rest of the mode state rather than in the board
// layer because it is the same kind of thing pendingMode is: a choice a
// manual reboot applies. Deliberately NOT folded into the stored mode —
// that has to go on remembering which HID identity to come back to, so
// two values were needed either way.
//
// Called from the board's menu, both ways: to arm a drive from an
// ordinary boot, and to disarm one from the drive screen itself.
void appSetStorageArmed(bool armed);
bool appStorageArmed();

// Implemented in main.cpp; called by the board's measurement code as
// soon as a measurement resolves, and before the board drops the busy
// flag boardMeasurementBusy() reports — so a run that is waiting for
// that flag can rely on the sample already being there.
//
// Called for every measurement, including the manual presses that have
// nothing to do with a run; main.cpp drops the ones that arrive while no
// run is in progress. Keeping the filter there rather than here is what
// lets the board layer stay ignorant of runs entirely.
//
// latencyUs is meaningless when timedOut is true.
void appRecordSample(uint32_t latencyUs, bool rise, bool timedOut);
