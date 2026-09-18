/*
 * HID Gamepad / Keyboard / Mouse over USB or BLE (one identity at a
 * time), or a drive
 * -------------------------------------------------------------------------
 * Emulates exactly ONE HID device at a time — gamepad ("X" button),
 * keyboard (Space), or mouse (left click) — over either USB or Bluetooth
 * LE, selected by the board's one button:
 *
 * The six identities are three actions times two transports, and that is
 * the whole reason the BLE half exists: the point is to compare BLE input
 * latency against USB with the same instrument, the same button, the same
 * sensor and the same statistics, so the only thing that differs between
 * a MODE_KEYBOARD number and a MODE_BLE_KEYBOARD one is how the report
 * got to the host. Everything downstream of sendPress() — the
 * measurement, the stats, the histogram, the automated test, the run
 * files — is transport-blind by construction. See the BLE block below for
 * how a BLE mode differs on the way in: USB is CDC-only (no HID device is
 * constructed at all), and the report becomes a HID-over-GATT
 * notification to a bonded host.
 *
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
 *   (see appSetStorageArmed()), changing the pending mode (see
 *   appAdvancePendingMode()) and — in a BLE mode only — pairing (see
 *   appBlePairingMode()). The first and last used to be hold-ladder
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
 * Mode is a host-facing identity, which is why it's stuck until a reboot
 * — and the meter touches neither USB nor the radio, so tying it to one
 * would have cost a reboot for nothing. It's a view the board layer
 * flips at runtime. (Outside the menu, presses keep sending their HID report
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
 * touches USB or the radio.
 *
 * A BLE mode is the same shape of thing for the same reason, just one
 * layer out: the GATT database, the report map and the advertised
 * identity are built once, in setup(), from the mode read out of NVS, and
 * NimBLE offers no more of a way to swap a HID report descriptor under a
 * bonded host than TinyUSB does under an enumerated one. So all six modes
 * share one rule — the identity is decided before anything is brought up,
 * and changing it is a reboot.
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
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
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

// The validation flavour of the run (see appStartValidation()): far
// fewer presses with far longer settling between them, because its
// product is not a distribution — it is a verdict on whether the setup
// can be trusted to produce one. The long gap is the point: it gives the
// board's idle-watching (see board_atoms3r.cpp) a wide window in which
// any light change is by definition unrequested.
static const uint16_t VALIDATE_ITERATIONS = 20;
static const uint32_t VALIDATE_GAP_MIN_MS = 1500;
static const uint32_t VALIDATE_GAP_MAX_MS = 3000;
static const uint32_t AUTO_HOLD_MS = 50;      // plausible press length
static const uint32_t AUTO_GAP_MIN_MS = 200;  // gap between releases and the next press
static const uint32_t AUTO_GAP_MAX_MS = 500;
// Hard ceiling on how long an automated press may stay down. Normally the
// measurement gates the release and this is never reached — it only bites
// if a measurement somehow never finishes, and it exists because a stuck
// HID button is a far worse thing to inflict on the host than one lost
// sample. Comfortably above the board's own measurement timeout.
static const uint32_t AUTO_HOLD_MAX_MS = 1000;

// Positioning time, added to the gap before the *first* press of a run
// (auto test and validation alike). The run is selected by a 1s hold on
// the device itself, so the moment it starts is the moment the user's
// hand is still on the thing they now have to aim at a display — a
// guaranteed second of doing nothing is what lets them let go, settle
// the sensor and get out of the way. It is *added* to the random first
// gap rather than replacing it: the randomness is there to stop presses
// aliasing with the display's refresh cadence (see the random-gap
// gotcha) and a fixed 1s start would put the first sample of every run
// at the same phase within a frame.
static const uint32_t AUTO_START_DELAY_MS = 1000;

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
static volatile bool autoIsValidation = false;  // set with autoStartRequested, same core
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

// --- Bluetooth LE HID -------------------------------------------------
// The three BLE modes are HID-over-GATT peripherals: the same three
// actions the USB modes send, carried as notifications on an input
// report characteristic instead of as reports on a USB endpoint.
//
// NimBLE and not the Arduino core's bundled `BLE` library, because the
// choice isn't open — this platform's prebuilt IDF libraries are built
// with CONFIG_BT_NIMBLE_ENABLED and no Bluedroid at all, so the host that
// the bundled library (and every ESP32-BLE-Gamepad/Keyboard/Mouse
// derivative) needs is simply not in libbt.a. NimBLEHIDDevice then gives
// the whole HoGP service — report map, input report plus its report
// reference descriptor, HID info, HID control point, protocol mode, and
// the Device Information and Battery services hosts expect alongside it —
// for the cost of handing it a report descriptor.
//
// Note what this shares with the USB half and what it doesn't. Shared:
// exactly one identity per boot, chosen from NVS before anything is
// brought up, so the GATT database contains one HID service with one
// report map and the host sees a single-purpose device. Not shared: a BLE
// mode still calls USB.begin(), because USBSerial is the only debug
// channel this firmware has — it just constructs no HID class, so the
// device enumerates as a plain CDC serial port and nothing else.

// Report IDs. One per mode, and always 1: a boot only ever builds one of
// the three report maps, so there is nothing for a second ID to
// disambiguate. It is present rather than omitted because a report map
// with no Report ID item and a report reference descriptor claiming ID 1
// is the classic HoGP mismatch, and hosts differ on which they believe.
static const uint8_t BLE_REPORT_ID = 1;

// Longest report any of the three modes sends (the keyboard's 8). One
// fixed buffer, filled in place on the press path, rather than a
// per-mode struct: the send path must not allocate.
static const size_t BLE_REPORT_MAX = 8;

// Connection interval we ask the host for, in 1.25ms units: 7.5ms to
// 15ms, slave latency 0, 2s supervision timeout. This is a real knob on
// the number being measured and it is deliberately set rather than left
// to the host's default, because it is what an actual BLE gamepad or
// mouse asks for — a tester that accepted a 30-60ms default would be
// measuring a device nobody ships. Two details matter more than the
// interval itself:
//
//   - Slave latency MUST be 0. Nonzero latency lets the peripheral skip
//     connection events when it has nothing to say, which is excellent
//     for battery and ruinous for input lag, and it would show up here
//     as a fat tail nothing in the firmware explains.
//   - The host may simply refuse. These are a *request*; the connection
//     interval the link actually settles on is the host's to decide, and
//     the measurement includes whatever it picked either way. See the
//     latency-honesty note further down: the wait for the next connection
//     event is part of what BLE costs and is not subtracted anywhere.
static const uint16_t BLE_CONN_ITVL_MIN = 6;   // 7.5ms
static const uint16_t BLE_CONN_ITVL_MAX = 12;  // 15ms
static const uint16_t BLE_CONN_LATENCY = 0;
static const uint16_t BLE_CONN_TIMEOUT = 200;  // 2s, in 10ms units

// Gamepad: 16 buttons plus X/Y axes. The axes are not used and are not
// decoration — a gamepad collection with buttons and no axes is accepted
// by some hosts and quietly ignored by others, and two spare bytes is a
// cheap way out of finding out which one is on the desk.
//
// Report: [buttons 0-7][buttons 8-15][X][Y], 4 bytes.
static const uint8_t bleReportMapGamepad[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Game Pad)
  0xA1, 0x01,        // Collection (Application)
  0x85, BLE_REPORT_ID,  //   Report ID
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (Button 1)
  0x29, 0x10,        //   Usage Maximum (Button 16)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x10,        //   Report Count (16)
  0x81, 0x02,        //   Input (Data,Var,Abs)
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x30,        //   Usage (X)
  0x09, 0x31,        //   Usage (Y)
  0x15, 0x81,        //   Logical Minimum (-127)
  0x25, 0x7F,        //   Logical Maximum (127)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x02,        //   Report Count (2)
  0x81, 0x02,        //   Input (Data,Var,Abs)
  0xC0,              // End Collection
};

// Keyboard: the standard 8-byte report, plus the LED output report. The
// output report is included, and getOutputReport() below creates the
// characteristic to match, because a keyboard that advertises no way to
// be told about caps lock is unusual enough that some hosts treat it as
// a malformed one.
//
// Report: [modifiers][reserved][key 1..6], 8 bytes.
static const uint8_t bleReportMapKeyboard[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, BLE_REPORT_ID,  //   Report ID
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,        //   Usage Minimum (Left Control)
  0x29, 0xE7,        //   Usage Maximum (Right GUI)
  0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08,
  0x81, 0x02,        //   Input (Data,Var,Abs)  -- modifier byte
  0x95, 0x01, 0x75, 0x08,
  0x81, 0x03,        //   Input (Cnst,Var,Abs)  -- reserved byte
  0x95, 0x05, 0x75, 0x01,
  0x05, 0x08,        //   Usage Page (LEDs)
  0x19, 0x01, 0x29, 0x05,
  0x91, 0x02,        //   Output (Data,Var,Abs) -- 5 LED bits
  0x95, 0x01, 0x75, 0x03,
  0x91, 0x03,        //   Output (Cnst,Var,Abs) -- LED padding
  0x95, 0x06, 0x75, 0x08,
  0x15, 0x00, 0x25, 0x65,
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0x00, 0x29, 0x65,
  0x81, 0x00,        //   Input (Data,Ary,Abs)  -- 6 keycodes
  0xC0,              // End Collection
};

// Mouse: 3 buttons, X/Y and a wheel — the shape every host has a driver
// for without thinking about it.
//
// Report: [buttons][X][Y][wheel], 4 bytes.
static const uint8_t bleReportMapMouse[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x85, BLE_REPORT_ID,  //   Report ID
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x05, 0x09,        //     Usage Page (Button)
  0x19, 0x01, 0x29, 0x03,
  0x15, 0x00, 0x25, 0x01,
  0x95, 0x03, 0x75, 0x01,
  0x81, 0x02,        //     Input (Data,Var,Abs)  -- 3 button bits
  0x95, 0x01, 0x75, 0x05,
  0x81, 0x03,        //     Input (Cnst,Var,Abs)  -- padding
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30, 0x09, 0x31, 0x09, 0x38,  // X, Y, Wheel
  0x15, 0x81, 0x25, 0x7F,
  0x75, 0x08, 0x95, 0x03,
  0x81, 0x06,        //     Input (Data,Var,Rel)
  0xC0,              //   End Collection
  0xC0,              // End Collection
};

// HID usage bytes for the one action each BLE mode sends. The gamepad's
// is worth a word: bit 3 is the same button index the USB gamepad's
// BUTTON_X resolves to (USBHIDGamepad.h names buttons after Linux input
// event codes, where BUTTON_X is 3 and not the SDL/XInput 2 — see the
// CLAUDE.md gotcha). Matching it is the point: the two transports must
// press the same thing or the host may not even route them to the same
// place.
static const uint8_t BLE_GAMEPAD_X_BIT = 0x08;  // button index 3
static const uint8_t BLE_KEY_SPACE = 0x2C;      // HID keyboard usage for Space
static const uint8_t BLE_MOUSE_LEFT = 0x01;     // button bit 0

// Only ever non-null in a BLE mode, the same way `gamepad` and friends
// are only ever non-null in their own USB mode.
static NimBLEServer *bleServer = nullptr;
static NimBLEHIDDevice *bleHid = nullptr;
static NimBLECharacteristic *bleInput = nullptr;
static uint8_t bleReport[BLE_REPORT_MAX];
static uint8_t bleReportLen = 0;

// Written from NimBLE's host task, read from loop() on core 1.
//
// bleReady, not "connected", is the flag the send path gates on, and the
// distinction is the honest one: a host that has connected but not yet
// subscribed to the input report will not receive a notification, so a
// press in that window is no more delivered than one with no host at
// all. Subscription is also the last step of the connect-pair-subscribe
// sequence, so waiting for it costs nothing real.
static volatile bool bleReady = false;
static volatile uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
// Set by appBlePairingMode(), cleared as soon as a host subscribes.
// Purely a display distinction — the radio does exactly the same thing in
// both states (advertise, connectable, discoverable) — so that the screen
// can say "pairing" right after bonds were dropped rather than leaving
// the user guessing whether the gesture did anything.
static volatile bool blePairingArmed = false;

// Push the link state down to the board. Called from the NimBLE host task
// as well as from setup(), and boardShowLink() is a store and a poke on
// both boards, so this stays inside what a callback may do.
static void bleUpdateLink() {
  if (!modeIsBle(activeMode)) {
    boardShowLink(LINK_NONE);
  } else if (bleReady) {
    boardShowLink(LINK_CONNECTED);
  } else {
    boardShowLink(blePairingArmed ? LINK_PAIRING : LINK_ADVERTISING);
  }
}

class BleCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
    bleConnHandle = info.getConnHandle();
    // Ask for the interval we actually want as soon as there is a link to
    // ask on. The host is free to say no; see BLE_CONN_ITVL_MIN above.
    server->updateConnParams(info.getConnHandle(), BLE_CONN_ITVL_MIN,
                             BLE_CONN_ITVL_MAX, BLE_CONN_LATENCY,
                             BLE_CONN_TIMEOUT);
    bleUpdateLink();
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info, int reason) override {
    (void)info;
    (void)reason;
    bleReady = false;
    bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
    // NimBLE re-advertises on disconnect by itself (advertiseOnDisconnect
    // defaults to true), which is exactly the behaviour wanted: a tester
    // that had to be power-cycled to be found again would be a nuisance
    // on the board with no screen especially. Nothing to do but say so.
    (void)server;
    bleUpdateLink();
  }

  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    // Nothing to gate on here — subscription is what the send path waits
    // for, and it comes after this. Kept for the link state only: a
    // failed pairing leaves the link unusable and the screen should not
    // go on claiming otherwise.
    if (!info.isEncrypted()) bleReady = false;
    bleUpdateLink();
  }
};

class BleInputCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *chr, NimBLEConnInfo &info, uint16_t subValue) override {
    (void)chr;
    // Bit 0 is "notifications enabled". Anything else (indications,
    // unsubscribe) means reports are not going to arrive the way this
    // firmware sends them.
    bool on = (subValue & 0x0001) != 0;
    bleReady = on;
    if (on) {
      bleConnHandle = info.getConnHandle();
      // The pairing gesture has done its job the moment a host is
      // actually listening.
      blePairingArmed = false;
    }
    bleUpdateLink();
  }
};

static BleCallbacks bleCallbacks;
static BleInputCallbacks bleInputCallbacks;

// --- One BLE identity, and one bond store, per BLE mode -----------------
//
// The three BLE modes present three different HID report maps, and a
// host caches both the GATT database and its pairing keys *per device
// address*. Give all three modes one address and switching modes breaks
// the device twice over: the host's cached report map no longer matches
// what the device serves (input goes dead until the host forgets the
// device), and on this side NimBLE's bond store is keyed by *peer* — so
// pairing the same host in a second mode overwrites the first mode's
// keys, and switching back fails encryption even though the host kept
// its half. The observed symptom of both together: forget-and-re-pair on
// every mode switch.
//
// Fixed by making each mode a genuinely different Bluetooth device:
//
//  * Identity: a per-mode static random address, derived from the chip's
//    BT MAC with the mode index mixed into the low byte (and the top two
//    bits forced to 0b11, which is what makes a random address "static").
//    Hosts then see three devices, each with its own cache and pairing.
//  * Bonds: NimBLE persists bonds as blobs in NVS namespace
//    "nimble_bond" (ble_store_nvs.c), with no per-identity separation —
//    so the whole namespace is banked per mode instead. On a BLE boot,
//    if the last BLE boot was a different mode, the live namespace is
//    copied out to that mode's bank, wiped, and the booting mode's bank
//    copied in. Non-BLE boots touch none of it. A pleasant consequence:
//    the pairing menu item's deleteAllBonds() now only ever clears the
//    active mode's bonds, which is exactly what "re-pair this mode"
//    should mean.
//
// The banking runs in setup(), strictly before NimBLEDevice::init() —
// the store must be settled before the host stack first reads it.

static const char *BLE_LIVE_NAMESPACE = "nimble_bond";

// Which mode's bonds currently sit in the live namespace. Lives beside
// the other persisted mode state. 0xFF = unknown (pre-banking firmware,
// or no BLE boot yet).
static const char *PREFS_BOND_OWNER_KEY = "bondowner";

static void bleBankNamespaceName(Mode mode, char *out, size_t outLen) {
  snprintf(out, outLen, "bondbank_%d", (int)mode);
}

// Copy every entry of one NVS namespace over another (destination is
// wiped first). The bond store is blob-only (verified against
// ble_store_nvs.c in the pinned library), but the copy handles all types
// blobs-included via the blob API only after checking, so a future store
// entry of another type fails loudly here rather than silently skewing.
static void bleCopyNamespace(const char *from, const char *to) {
  nvs_handle_t src, dst;
  if (nvs_open(from, NVS_READONLY, &src) != ESP_OK) return;  // nothing to copy
  if (nvs_open(to, NVS_READWRITE, &dst) != ESP_OK) {
    nvs_close(src);
    return;
  }
  nvs_erase_all(dst);

  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, from, NVS_TYPE_BLOB, &it);
  while (err == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    size_t len = 0;
    if (nvs_get_blob(src, info.key, nullptr, &len) == ESP_OK && len > 0) {
      uint8_t *buf = (uint8_t *)malloc(len);
      if (buf) {
        if (nvs_get_blob(src, info.key, buf, &len) == ESP_OK) {
          nvs_set_blob(dst, info.key, buf, len);
        }
        free(buf);
      }
    }
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  nvs_commit(dst);
  nvs_close(dst);
  nvs_close(src);
}

static void bleEraseNamespace(const char *ns) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

// Park the previous BLE mode's bonds and pull in this one's. Called only
// on a BLE boot; on a USB or storage boot the live namespace just keeps
// whatever it holds, still tagged with its owner.
static void bleBankSwitch(Mode mode) {
  uint8_t owner = prefs.getUChar(PREFS_BOND_OWNER_KEY, 0xFF);
  if (owner == (uint8_t)mode) return;  // already ours, nothing to move

  char bank[16];
  if (owner != 0xFF && modeIsBle((Mode)owner)) {
    // Park the previous owner's bonds in its bank.
    bleBankNamespaceName((Mode)owner, bank, sizeof(bank));
    bleCopyNamespace(BLE_LIVE_NAMESPACE, bank);
  }
  // 0xFF (firmware that predates banking, or a fresh chip): whatever is
  // in the live namespace was shared by all three modes and is stale for
  // at least two of them. Adopting it unparked would hand one mode keys
  // the host may associate with a *different* identity now that per-mode
  // addresses exist — so it is wiped rather than adopted, one final
  // re-pair per mode as the migration cost.
  bleEraseNamespace(BLE_LIVE_NAMESPACE);
  bleBankNamespaceName(mode, bank, sizeof(bank));
  bleCopyNamespace(bank, BLE_LIVE_NAMESPACE);
  prefs.putUChar(PREFS_BOND_OWNER_KEY, (uint8_t)mode);
}

// The per-mode identity address: the chip's BT MAC with the mode index
// mixed into the low byte, top two bits forced to 0b11 (the static
// random marker — hosts treat anything else in a random slot as
// malformed). NimBLE takes addresses little-endian, so out[5] is the
// most significant byte.
static void bleIdentityFor(Mode mode, uint8_t out[6]) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  for (int i = 0; i < 6; i++) out[i] = mac[5 - i];  // big->little endian
  out[0] ^= (uint8_t)mode;  // low byte: what makes the three differ
  out[5] |= 0xC0;           // static random address marker
}

// Build the GATT database for this boot's BLE identity and start
// advertising. Called once from setup(), only in a BLE mode — the same
// only-construct-what-this-boot-is rule the USB device classes follow,
// for the same reason: the report map is registered with the stack here
// and there is no supported way to swap it afterwards.
static void bleBegin(Mode mode) {
  const uint8_t *map = nullptr;
  size_t mapLen = 0;
  uint16_t appearance = 0;
  bool wantsOutputReport = false;

  switch (mode) {
    case MODE_BLE_GAMEPAD:
      map = bleReportMapGamepad; mapLen = sizeof(bleReportMapGamepad);
      appearance = HID_GAMEPAD; bleReportLen = 4;
      break;
    case MODE_BLE_KEYBOARD:
      map = bleReportMapKeyboard; mapLen = sizeof(bleReportMapKeyboard);
      appearance = HID_KEYBOARD; bleReportLen = 8;
      wantsOutputReport = true;
      break;
    case MODE_BLE_MOUSE:
      map = bleReportMapMouse; mapLen = sizeof(bleReportMapMouse);
      appearance = HID_MOUSE; bleReportLen = 4;
      break;
    default:
      return;  // not a BLE boot; the radio stays off entirely
  }

  // Bonds for this mode into the live store, previous mode's parked —
  // strictly before init(), which is when the host stack first reads it.
  bleBankSwitch(mode);

  // The GAP device name. Deliberately modeBleName() and not
  // modeProductName(): NimBLE caps this at 31 bytes and silently refuses
  // anything longer, which the USB product strings exceed. See mode.h.
  NimBLEDevice::init(modeBleName(mode));

  // This mode's own identity address, so hosts see three separate
  // devices (see the block comment above bleBankSwitch()). Order
  // matters: setOwnAddr() installs the random address,
  // setOwnAddrType() then validates that one is installed.
  uint8_t identity[6];
  bleIdentityFor(mode, identity);
  NimBLEDevice::setOwnAddr(identity);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

  // Bonding, no MITM, secure connections: "just works" pairing, which is
  // the only kind available on a device with one button and (on one
  // board) no display to show a passkey on. Bonding is not optional —
  // the input report characteristic is READ_ENC, and every host worth
  // testing against refuses to use a HID device it has not paired with.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&bleCallbacks, false);

  bleHid = new NimBLEHIDDevice(bleServer);
  bleHid->setManufacturer("usb-latency");
  // Vendor ID source 0x02 = USB Implementers Forum. The VID/PID are the
  // Espressif defaults this device already enumerates with over USB, so a
  // host that has seen both transports at least sees one vendor.
  bleHid->setPnp(0x02, 0x303A, 0x1001, 0x0100);
  // Country code 0 (not localised), flags 0x01 = remote wake. Remote wake
  // is what lets a press bring a sleeping host back, which is a thing a
  // real HID device does and one this one may well be pointed at.
  bleHid->setHidInfo(0x00, 0x01);
  bleHid->setReportMap(const_cast<uint8_t *>(map), mapLen);

  bleInput = bleHid->getInputReport(BLE_REPORT_ID);
  bleInput->setCallbacks(&bleInputCallbacks);
  if (wantsOutputReport) bleHid->getOutputReport(BLE_REPORT_ID);
  // Not a real measurement — there is no battery on either board, both
  // run off the USB cable that is also the debug channel. It is here
  // because hosts that find a Battery Service with no value read it
  // anyway and some log an error every time, and 100% is the least
  // misleading constant for a mains-powered device.
  bleHid->setBatteryLevel(100);

  bleServer->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  // enableScanResponse() before setName(): NimBLEAdvertising::setName()
  // puts the name in the scan response only if the scan response is
  // already enabled, and otherwise spends the advertisement's own bytes
  // on it. The advertisement holds flags, appearance, the HID service
  // UUID and the preferred connection interval; the name would not fit
  // alongside them.
  adv->enableScanResponse(true);
  adv->setName(modeBleName(mode));
  adv->setAppearance(appearance);
  adv->addServiceUUID(bleHid->getHidService()->getUUID());
  adv->setPreferredParams(BLE_CONN_ITVL_MIN, BLE_CONN_ITVL_MAX);
  adv->start();

  bleUpdateLink();
}

// One notification, on the press path. Everything it needs is already
// decided (which bytes, how many, which connection), so this is a memset,
// two stores and the notify.
//
// Not blocking, and that matters: NimBLECharacteristic::notify() with an
// explicit connection handle allocates an mbuf and hands it to
// ble_gattc_notify_custom(), which queues the ATT PDU on that
// connection's transmit queue and returns. It does not wait for the next
// connection event and it does not wait for an acknowledgement — that is
// what an *indication* would do, and why this is a notification. The
// handle is passed explicitly rather than left to default for the same
// reason: the no-handle form walks getPeerDevices(), which returns a
// std::vector by value, i.e. a heap allocation per press.
//
// The one thing it can block on is NimBLE's host mutex, if the host task
// happens to be mid-operation at that instant — microseconds, and on the
// other core (the NimBLE tasks are pinned to core 0, loop() has core 1).
// Untested on hardware; see CLAUDE.md.
static bool bleSend(bool pressed) {
  if (!bleInput || !bleReady) return false;

  memset(bleReport, 0, bleReportLen);
  if (pressed) {
    switch (activeMode) {
      case MODE_BLE_GAMEPAD:  bleReport[0] = BLE_GAMEPAD_X_BIT; break;
      case MODE_BLE_KEYBOARD: bleReport[2] = BLE_KEY_SPACE; break;
      case MODE_BLE_MOUSE:    bleReport[0] = BLE_MOUSE_LEFT; break;
      default: return false;
    }
  }
  // A release is the all-zero report the memset already produced.
  return bleInput->notify(bleReport, bleReportLen, bleConnHandle);
}

// Pairing mode, and what it concretely does: forget every bonded host,
// drop the current link if there is one, and advertise again.
//
// It is deliberately not "become discoverable for 30 seconds", because
// discoverability is not the scarce thing here — a BLE mode advertises
// from boot and re-advertises the moment a host goes away, so there is
// never a window a new host can't see the device. What actually stops a
// new host connecting is a *stale bond*: the device keeps a key for a
// machine that has since forgotten it (or has three of them, NimBLE's
// CONFIG_BT_NIMBLE_MAX_BONDS), and the pairing attempt fails in a way
// that looks from the outside like the device being broken. Dropping the
// bonds is the gesture that fixes that, and it is the same gesture a
// keyboard's "unpair" button performs.
//
// The host has its own half of that bond and this cannot reach it, so
// moving a device between machines usually also needs the old machine to
// forget it. Nothing here can help with that; see CLAUDE.md.
//
// Called from the board's menu, i.e. from the UI task on core 0.
// deleteAllBonds() writes NVS and therefore erases flash, which stalls
// the cache on both cores — fine exactly here, and for the same reason
// boardWriteRun() is: it is a deliberate menu action taken while nothing
// is being measured.
void appBlePairingMode() {
  if (!modeIsBle(activeMode)) return;

  blePairingArmed = true;
  bleReady = false;

  if (bleServer && bleConnHandle != BLE_HS_CONN_HANDLE_NONE) {
    bleServer->disconnect(bleConnHandle);
  }
  NimBLEDevice::deleteAllBonds();
  // Harmless if the disconnect above already restarted it; NimBLE
  // ignores a start on an already-advertising instance.
  NimBLEDevice::startAdvertising();

  bleUpdateLink();
}

// Returns whether anything actually left the device. False in a BLE mode
// with no host listening — see boardShowPress()'s `sent` in board.h for
// what the caller does with that, and note that MODE_STORAGE's `default:`
// arm returns false for free, which is the same "this identity sends
// nothing" it always meant.
static inline bool sendPress() {
  switch (activeMode) {
    case MODE_GAMEPAD:  gamepad->pressButton(BUTTON_X); return true;
    case MODE_KEYBOARD: keyboard->press(' '); return true;
    case MODE_MOUSE:    mouse->press(MOUSE_LEFT); return true;
    case MODE_BLE_GAMEPAD:
    case MODE_BLE_KEYBOARD:
    case MODE_BLE_MOUSE: return bleSend(true);
    default: return false;
  }
}

static inline bool sendRelease() {
  switch (activeMode) {
    case MODE_GAMEPAD:  gamepad->releaseButton(BUTTON_X); return true;
    case MODE_KEYBOARD: keyboard->release(' '); return true;
    case MODE_MOUSE:    mouse->release(MOUSE_LEFT); return true;
    case MODE_BLE_GAMEPAD:
    case MODE_BLE_KEYBOARD:
    case MODE_BLE_MOUSE: return bleSend(false);
    default: return false;
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
  if (autoIsValidation) {
    return VALIDATE_GAP_MIN_MS +
           (esp_random() % (VALIDATE_GAP_MAX_MS - VALIDATE_GAP_MIN_MS + 1));
  }
  return AUTO_GAP_MIN_MS + (esp_random() % (AUTO_GAP_MAX_MS - AUTO_GAP_MIN_MS + 1));
}

static uint16_t runIterations() {
  return autoIsValidation ? VALIDATE_ITERATIONS : AUTO_TEST_ITERATIONS;
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
  autoIsValidation = false;
  autoStartRequested = true;
}

// The validation run: identical plumbing to the auto test — same press
// path, same abort gesture, same completion signalling — differing only
// in count and pacing. Everything that makes it a *validation* (idle
// watching, post-crossing watching, the verdict report) lives in the
// board layer, which knows it started one; main.cpp only paces it.
// Validation samples never reach the stats or a run file: the board
// skips appRecordSample() for them, so runCount stays 0 and
// boardWriteRun() declines empty records.
void appStartValidation() {
  autoIsValidation = true;
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
  rec.planned = runIterations();
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
    // A run in a BLE mode with nothing listening would spend three to
    // five minutes sending nothing and recording nothing, and would
    // write out an empty file at the end of it. Refusing is the kinder
    // failure: the screen already says the link is down, and the menu
    // item can simply be selected again once it isn't.
    if (modeIsBle(activeMode) && !bleReady) return;
    autoDone = 0;
    autoPhase = AUTO_GAP;
    runCount = 0;
    runAborted = false;
    runRecording = true;
    sampleReady = false;  // anything left over belongs to a manual press
    // A normal gap before the first press too, rather than firing the
    // instant the button comes up: the button is the screen face, inches
    // from wherever the sensor is aimed, so letting go of it is exactly
    // the moment not to be taking a reading. Plus AUTO_START_DELAY_MS on
    // top of it, so that "not immediately" has a floor rather than being
    // whatever the die rolled. It goes into runGapMs rather than only
    // into autoResumeAtMs so the run file records the gap that actually
    // preceded the first sample.
    runGapMs = autoGapMs() + AUTO_START_DELAY_MS;
    autoResumeAtMs = now + runGapMs;
    boardShowAutoTest(0, runIterations());
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
        bool sent = sendPress();
        if (!sent) {
          // Only reachable in a BLE mode whose host went away mid-run.
          // Stop, rather than grind through the remaining iterations
          // pressing a button nothing is listening to — and stop before
          // telling the board about a press, so it neither starts a
          // measurement nor shows a press that will never be released.
          // What was collected up to here is still written out.
          stopAutoTest();
          return;
        }
        boardShowPress(true, activeMode, edgeMicros, true);
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

      {
        bool sent = sendRelease();
        boardShowPress(false, activeMode, esp_timer_get_time(), sent);
      }
      autoDone++;

      // Getting here means the measurement is done (or the hold ceiling
      // gave up on it, in which case there is nothing to collect and the
      // iteration simply contributes no row) — so the board's half of
      // this sample is already in the stash, still paired with the gap
      // that preceded this press.
      collectSample();

      if (autoDone >= runIterations()) {
        autoPhase = AUTO_OFF;
        boardShowAutoTest(0, 0);
        runFlushPending = true;  // written on the next pass, by finishRun()
      } else {
        autoPhase = AUTO_GAP;
        runGapMs = autoGapMs();
        autoResumeAtMs = now + runGapMs;
        boardShowAutoTest(autoDone, runIterations());
      }
      return;
  }
}

void setup() {
  boardBegin();

  prefs.begin(PREFS_NAMESPACE, false);
  // `stored < MODE_COUNT` is the validation, and it keeps working
  // unchanged now that MODE_STORAGE exists — precisely because this key
  // only ever holds one of the six HID identities. Storage lives in its
  // own key, so a stored value of MODE_STORAGE here would still be
  // rejected as the corruption it would be.
  //
  // MODE_COUNT growing from 3 to 6 is the whole of what the BLE modes
  // needed here, and it is safe in the direction that matters: values
  // 0..2 still mean what they always meant, so a device that was in
  // MOUSE stays in MOUSE across the upgrade. The other direction is
  // worth knowing rather than guarding — a device left in a BLE mode and
  // then flashed with a firmware that predates them stores a 3, 4 or 5
  // that the OLD code's `stored < 3` rejects, and it comes up as a
  // gamepad. That is the right failure: the old firmware cannot be a BLE
  // anything, so falling back beats honouring a value it would
  // misinterpret.
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
    //
    // The three BLE modes land here too, and for them it is just as
    // deliberate: constructing a USBHIDKeyboard in MODE_BLE_KEYBOARD
    // would put a HID collection in the USB descriptor and hand the host
    // a second, wired keyboard that shadows the one being measured. A
    // BLE boot is a CDC serial port over USB and nothing else.
    default: break;
  }
  // Set even in a BLE mode: USBSerial is still there, and a debug port
  // that names the mode it belongs to beats an anonymous one. In a BLE
  // mode this is not the name the Bluetooth side shows — see
  // modeBleName() in mode.h.
  USB.productName(modeProductName(activeMode));
  USBSerial.begin();
  USB.begin();

  // Only now that USB is up is it safe to spend time on the indicator.
  boardShowBoot(activeMode, pendingMode);
  boardShowHoldHint(RUNG_STATS);

  // The radio last, after the screen exists, so the several hundred
  // milliseconds the controller takes to come up are spent with
  // something already on the panel rather than in front of a black one.
  // A no-op in the four non-BLE modes, which is what keeps the BLE
  // stack's RAM cost off every other boot despite it being linked in
  // unconditionally.
  bleBegin(activeMode);
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
        // In a BLE mode with no host listening nothing is sent, and the
        // board is told so: a measurement started against a report that
        // never left would time out by construction and land a bogus
        // 500ms in the statistics.
        bool sent = sendPress();
        boardShowPress(true, activeMode, edgeMicros, sent);
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
        bool sent = sendRelease();
        boardShowPress(false, activeMode, edgeMicros, sent);
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
