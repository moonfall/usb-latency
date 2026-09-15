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
 * board layer holds no copy of state main.cpp already owns.
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
void boardShowPress(bool pressed, Mode active, int64_t atMicros);

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
