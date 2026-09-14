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

// How many modes this board's hold-to-cycle rotation offers. MODE_LIGHT is
// last in the enum, so a board with no light sensor returns MODE_LIGHT and
// gets the three HID modes only; one with a sensor returns MODE_COUNT.
// main.cpp also validates the NVS-stored mode against this, so a mode
// saved on one board can't strand the other.
uint8_t boardModeCount();

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
// showing the new pending mode.
void boardShowPending(Mode active, Mode pending, bool firstOfHold);
