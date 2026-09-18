# usb-latency

PlatformIO firmware for measuring/minimizing input-to-photon latency: a
board with one button emulates a HID gamepad, keyboard, or mouse — over
USB **or** Bluetooth LE — and a press is turned into a HID report as
directly as possible. On the AtomS3R it can also boot as a USB drive
instead, handing the host the flash partition the recorded runs live on.

The six HID identities are three actions times two transports, and the
second transport is the whole point of having it: the same button, the
same sensor, the same statistics and the same run files, so a BLE number
and a USB number differ only in how the report reached the host. See the
`Mode` table below.

Two boards are supported, one PlatformIO env each, from one shared source
tree:

| env | board | input | indicator |
| --- | --- | --- | --- |
| `esp32-s3-zero` | Waveshare ESP32-S3-Zero | BOOT button (GPIO0) | onboard WS2812 (GPIO21) |
| `m5stack-atoms3r` | M5Stack AtomS3R | screen button (GPIO41) | 128x128 LCD |

The AtomS3R variant also reads an M5Stack Unit Light (U012) on the Grove
port. That sensor does the actual latency measurement: a press starts a
clock at the button edge and stops it when the light crosses
`LIGHT_THRESHOLD` (default 3000 ADC counts), so the figure on screen
covers button-down → USB or BLE → host → compositor → panel.

## Modes

A `Mode` is a host-facing identity, fixed for the life of a boot, chosen
before USB or the radio is brought up, and changed only by a manual
reboot (see the two mode-switching gotchas below). There are six in the
rotation plus one outside it:

| Mode | transport | a press sends | LED / screen colour |
| --- | --- | --- | --- |
| `MODE_GAMEPAD` | USB HID | gamepad "X" button | red |
| `MODE_KEYBOARD` | USB HID | Space | green |
| `MODE_MOUSE` | USB HID | left click | blue |
| `MODE_BLE_GAMEPAD` | BLE HoGP | gamepad "X" button | yellow |
| `MODE_BLE_KEYBOARD` | BLE HoGP | Space | cyan |
| `MODE_BLE_MOUSE` | BLE HoGP | left click | magenta |
| `MODE_STORAGE` | USB MSC | nothing (see USB drive mode) | — |

The colour column is one table for both boards: it is what the S3-Zero's
single WS2812 shows while the button is held, and what the AtomS3R draws
the mode name in. USB gets the primaries, BLE the secondaries; the
pairing is arbitrary (no colour means "the same, but wireless") but six
distinguishable colours on a board whose whole output is one pixel is
worth more than a mnemonic.

In a **BLE mode** the device does not enumerate as a USB HID device at
all — USB is a CDC serial port for debug output and nothing else — and
the press becomes a HID-over-GATT notification to a bonded host. The
Bluetooth name is e.g. `Latency Tester BLE Keyboard` (`modeBleName()`),
used for both the advertisement and the GAP device name, so a host shows
the same string in its picker and after connecting. The USB product
string for the same boot is the longer `USB Latency Tester - BLE
Keyboard` — see the BLE-name-length gotcha below for why those cannot be
one string.

A press in a BLE mode with **no host connected sends nothing and starts
no measurement** — timing a report that never left would only bank a
guaranteed `MEASURE_TIMEOUT_MS` in the statistics. The AtomS3R says so in
two places: the mode line carries a right-aligned `LINK` / `ADV` / `PAIR`
tag at all times, and the headline shows `not connected` for that press.
The S3-Zero lights the mode colour *dim* instead of full.

**Pairing** concretely means: forget every bonded host, drop the current
link, and advertise again. It is not "become discoverable for 30
seconds" — a BLE mode advertises from boot and re-advertises the moment a
host goes away, so discoverability is never the scarce thing. A *stale
bond* is: the device holds a key for a machine that has forgotten it (or
holds three, NimBLE's `CONFIG_BT_NIMBLE_MAX_BONDS`), and the pairing
attempt then fails in a way that looks like broken hardware. On the
AtomS3R it is a menu item, present only in a BLE mode. **The S3-Zero has
no pairing gesture and no link display, deliberately** — its BLE story is
"advertises from boot, re-advertises when a host goes away, pairs with
anything unbonded", and a stale bond there is fixed by forgetting the
device on the *host*, or failing that by erasing flash. Inventing a
second hold gesture on a button that already carries press-to-send and
hold-to-cycle-mode, on a board with no way to show that the gesture
registered, would be worse than not having one. Note the device can only
ever drop its own half of a bond; moving between machines usually also
needs the old machine to forget the device.

## Interaction

The one button drives two entirely different things depending on whether
a menu (AtomS3R only) is open:

| state | gesture | action | needs a sensor |
| --- | --- | --- | --- |
| normal | press | send the mode's HID report, and time the response | — |
| normal | hold 1s | reset the measurement statistics | yes |
| normal | hold 2s | open the menu (below) | yes |
| normal | hold 3s, repeating | advance the *pending* mode, 1 of 6 (reboot to apply) | no (S3-Zero only) |
| menu | tap | advance — move the selection, or the mode picker's candidate | yes |
| menu | hold 1s | trigger the highlighted item | yes |
| auto test running | press | stop the run (and nothing else) | yes |
| threshold cal running | press | abort it (and nothing else) | yes |
| storage boot | hold 1s | toggle whether the *next* reset is a drive | AtomS3R only |

The S3-Zero has no screen and so no menu — its button is exactly what it
always was: press sends the action, holding cycles the pending mode every
3s through all six. On the AtomS3R, a 2s hold from normal operation opens
a small menu (`Light meter`, `Auto test`, `Validate`, `Sensor`,
`Change mode`, `Pairing` — BLE modes only — `Exit`) that owns
every subsequent press until it exits: tap cycles the highlighted item, a
1s+ hold triggers it. `Change mode` drops into a picker where tap advances
the candidate (persisting it immediately, same NVS write the
S3-Zero's hold-to-cycle gesture always did) and hold confirms by just
leaving. That picker's list is **seven** entries, not six: the six modes
in rotation order, then `USB DRIVE`, then round to `GAMEPAD` again — one
picker for the whole question "what does the next reset come up as",
since the drive is the other answer to it (see USB drive mode below).
Only six of the seven are a `Mode`; landing on the drive arms a flag
instead, and landing on any real mode disarms it again — the flag wins at
boot, so leaving it set would make the mode just picked look ignored.
`Light meter` toggles instantly and exits; `Pairing` (BLE modes only)
drops every bond and re-advertises, then exits; `Exit` just exits. No press
reaches the HID/measurement path while the menu is open — see
`wasMenuActiveAtPress` in `main.cpp` for how that's decided once per press
rather than re-checked live.

`Auto test` refuses to start in a BLE mode with no host connected (three
to five minutes of sending nothing, ending in an empty file, is not a
kinder failure than saying no) and stops itself if the link drops
mid-run, writing out what it had. Otherwise it runs
`AUTO_TEST_ITERATIONS` (500) presses in the current mode unattended, spaced by a random 200-500ms gap, with a further fixed
`AUTO_START_DELAY_MS` (1s) added to the first gap only so there is always
at least a second to let go and aim the sensor — roughly three to five
minutes, abortable at any point with a press — each going through exactly
the same send-and-measure path a real button press does. It lives in
`main.cpp` (`serviceAutoTest()`), not the board layer, because only
`main.cpp` may touch USB and an automated press has to be a genuine HID
report or it isn't measuring the same thing. It does **not** clear the
existing stats first — reset with a 1s hold beforehand if a clean
distribution is wanted.

`tools/analyze_runs.html` is the desk-side half of the auto test: a
standalone, dependency-free page that takes the run CSVs (drag them
straight off the USB-drive mode) and pools any number of them into
All/Rising/Falling panels — mean/median/sample-stddev, min/max,
P1/P5/P10/P90/P95/P99, Tukey-trimmed (1.5×IQR) mean/median with the
drop count shown, and stacked histograms in the device's own rise/fall
colours on one shared x-axis with per-bin hover readout. Timeout rows
are tallied but excluded from the numbers; re-dropping a file replaces
it rather than double-counting; pooling files from different modes is
flagged as a warning.

Every run is recorded to flash, on the AtomS3R only: one CSV file per
run on a wear-levelled FAT partition, named `RUNnnnnn.CSV` from a counter
kept in NVS. Aborted runs are written too, with whatever they collected.
Nothing is written while a run is in progress — see the
no-flash-writes-during-a-run gotcha below, and the run-storage block in
`board_atoms3r.cpp` for the file format.

**USB drive mode** (AtomS3R only) is how those files get off the device:
picking `USB DRIVE` in the mode picker arms a flag in NVS, and the *next* boot
enumerates as a small mass-storage device — product name "USB Latency
Tester - Storage" — whose blocks are the `ffat` partition itself, so the
host mounts the run CSVs with no firmware in the loop. As with a pending
mode change, arming does nothing this session and the firmware never
reboots itself; a manual reset (or replug) applies it.

A drive boot has **no HID device at all** — a press sends nothing — and
the firmware does not mount FFat on that partition while the host holds
it (see the two-writers gotcha below). The screen still works, and is the
whole UI for that boot: a `USB DRIVE` screen showing `READY`, or
`EJECTED` once the host has let go (i.e. safe to unplug), with a line
stating what the next reset will do. It is **sticky**, not one-shot — a
drive that turned back into a gamepad on every replug would be useless
for carrying files between machines — so leaving it is explicit: hold the
screen for 1s to toggle the flag back off, then reset.

## Build

```
pio run -e esp32-s3-zero              # compile one variant
pio run -e m5stack-atoms3r
pio run                               # both
pio run -e <env> -t upload            # flash (see esptool gotcha below)
```

## Project layout

- `platformio.ini` — a shared `[env]` section holds everything common:
  native USB-OTG mode (`ARDUINO_USB_MODE=0`), `ARDUINO_USB_CDC_ON_BOOT=0`
  (deliberately not `1` — see the CDC-on-boot gotcha below; a manual
  `USBCDC` in `src/main.cpp` (`USBSerial`) is begun explicitly instead, so
  Serial-style debug output over USB is still available), and a `lib_deps`
  on `h2zero/NimBLE-Arduino@^2.3`, which both envs get because every mode
  has to be reachable on either board. The two envs then
  differ only in `board`, `build_src_filter` (which `board_*.cpp` gets
  compiled), `board_build.partitions`, and — for the AtomS3R — an extra
  `lib_deps` entry for M5GFX. Both envs restate their board profile's
  `extra_flags` minus `ARDUINO_USB_MODE=1`, which would otherwise
  redefine the `=0` set above. **Do not add dependencies with `pio pkg
  install`** — see the gotcha below; it rewrites this file and strips
  every comment in it.
- `partitions_atoms3r_8MB.csv` — the AtomS3R's partition table, replacing
  the AtomS3 board profile's stock `default_8MB.csv` (which has a SPIFFS
  partition this project can't use). `nvs` stays at `0x9000`/`0x5000`,
  byte-for-byte where the stock table puts it, so reflashing an existing
  device doesn't lose its persisted mode; then a single 4MB `factory` app
  at `0x10000` (no OTA — generous on purpose, a later task adds a BLE
  stack), `ffat` filling `0x410000`-`0x7F0000`, and `coredump` in the last
  64KB where the stock tables keep it — and the "a later task adds a BLE
  stack" it was sized generously for is the task that has now landed, at
  a cost of ~290KB. The S3-Zero env has 4MB, no sensor
  and therefore no automated test, so it keeps stock `default.csv` and
  stores nothing. That last point was re-checked rather than assumed when
  BLE landed: the expectation was that a NimBLE host plus controller
  would overflow `default.csv`'s ~1.25MB OTA slot and force a
  single-factory-app table there too. It does not — 684KB, 52% of the
  slot — so the S3-Zero still has no table of its own. If that ever
  changes, `nvs` has to stay at `0x9000`/`0x5000`, or every device loses
  both its persisted mode and its BLE bonds.
- `src/main.cpp` — board-independent core: the mode state machine, NVS
  persistence, the single HID device (USB or BLE), the BLE stack, and the
  debounce loop. It is the only file that touches USB or the radio. The
  button drives exactly one of six HID modes at a time: gamepad "X"
  button (`USBHIDGamepad`), keyboard space key (`USBHIDKeyboard`), or mouse
  left click (`USBHIDMouse`) over USB, and the same three actions over BLE
  as HID-over-GATT notifications. Only the active mode's device class is ever
  constructed (`new`'d at runtime in `setup()`, not declared as a global),
  so the USB descriptor for a given boot contains a single HID collection —
  the device enumerates as a genuine single-purpose gamepad, keyboard, or
  mouse, not a multi-collection composite. A BLE mode constructs none of
  them, so USB is CDC-only that boot, and builds a `NimBLEHIDDevice`
  instead — one HID service, one report map, one input report — which is
  the same rule one layer out. The USB product name is set per
  mode too (`modeProductName()`, e.g. "USB Latency Tester - Keyboard"), so
  the host's device picker identifies the active mode by name; in a BLE
  mode a shorter `modeBleName()` is used for everything Bluetooth shows.
  `sendPress()`/`sendRelease()`
  now return whether anything actually left the device, which is false in
  a BLE mode with no host subscribed and is what stops a measurement being
  started against a report that never went out. Holding the
  button advances a *pending* mode (gamepad → keyboard → mouse → BLE
  gamepad → BLE keyboard → BLE mouse → gamepad)
  by one step every `MODE_HOLD_MS` (3s) for as long as it's held,
  persisting each step to NVS via `Preferences` — this only selects what a
  future reboot will pick up; it does not change what's active this
  session, and does **not** reboot automatically (see the
  mode-switch-needs-a-reboot gotcha below for why). A manual reboot (reset
  button, or unplug/replug) is required for the pending mode to take
  effect. Leading-edge/lockout debounce (act immediately on the edge, then
  ignore further changes for `DEBOUNCE_MS`) rather than trailing-edge
  debounce, to keep added latency at zero for the button action itself —
  only the mode-cycling feedback is deliberately slow. The HID report is
  always sent *before* any board feedback is drawn, so lighting an LED or
  repainting a screen is never in the latency path. A separate NVS key
  (`usbmode`/`storage`, read only where `boardHasStorage()` is true)
  selects the fourth identity instead: `MODE_STORAGE`, where a `USBMSC`
  is constructed in place of any HID class and its read / write /
  start-stop callbacks are one-line trampolines onto `boardStorage*()`.
  The armed flag is a key of its own rather than a fourth value in
  `mode`, because the HID identity has to go on being remembered while a
  drive is armed — that's what the drive screen offers as the way back.
- `src/mode.h` — the `Mode` enum plus the strings naming it (mode name,
  the action a press sends, the USB/GAP product string, the shorter BLE
  advertising name) and `modeIsBle()`, the one place the two transports
  are told apart. Header-only lookup
  tables, shared by the core and the board layer. A `Mode` is a
  host-facing identity and nothing more, which is exactly why it is stuck
  until a reboot — see the light-meter-is-not-a-mode gotcha below.
  **The enum's order is a stored format**: 0..2 still mean what they
  always meant, so the BLE three are appended after `MODE_MOUSE` rather
  than interleaved with their USB counterparts, which would have read
  better and silently changed what every deployed device was.
  `MODE_COUNT` grew from 3 to 6 — which is the entire change the rotation
  needed — and `MODE_STORAGE` deliberately still sits *past* it, so the
  `% MODE_COUNT` rotation can never reach it and it is never a stored
  value of the mode key. The AtomS3R's picker does offer a drive as a
  seventh candidate, but virtually, without this enum. See the
  storage-is-a-mode-but-not-in-the-rotation gotcha below.
- `src/board.h` — the board I/O contract: `boardBegin()`,
  `boardHasSensor()`, `boardButtonPressed()`, `boardShowBoot()`,
  `boardShowPress()` (which takes a `sent` flag, false when a BLE press
  had no host to go to), `boardShowLink()` (connected / advertising /
  pairing, pushed down from `main.cpp`, often from a NimBLE callback —
  so implementations must be a store and a poke, like every other
  cross-core entry point here), `boardShowPending()`, `boardResetStats()`,
  `boardShowHoldHint()`, plus the menu contract —
  `boardMenuActive()`, `boardEnterMenu()`, `boardMenuTap()`,
  `boardMenuSelect()`, and the two that run the other way —
  `appSetPendingMode(Mode)` (implemented in `main.cpp`, called by the
  board's mode-picker submenu — it takes the mode rather than advancing
  by one because that picker cycles a seven-entry list, six modes plus a
  virtual `USB DRIVE`, so stepping off the drive entry has to land on
  `MODE_GAMEPAD`; values at or past `MODE_COUNT` are refused there) and
  `appBlePairingMode()` (likewise,
  called by the menu's BLE-only `Pairing` item). The threshold
  calibration adds three more of that second kind: `appCalPress()` /
  `appCalPressBusy()`, the one input event that flow sends to change the
  display under test (asynchronous, because the send belongs on core 1
  while the board is blocked on core 0), and `appCanSendInput()`, the
  "would a press reach a host right now" query it refuses on. Run recording adds `RunSample`/`RunRecord`
  plus one function each way: `appRecordSample()` (board → `main.cpp`,
  one measurement's latency/direction/timeout as soon as it resolves) and
  `boardWriteRun()` (`main.cpp` → board, the whole buffered run, once,
  after it ends). The split follows who knows what — `main.cpp` paces the
  run and so owns the mode, the iteration count and the gap before each
  press; the board did the measuring and so owns the result. USB drive
  mode splits the same way: `boardHasStorage()`, `boardStorageBegin()`,
  `boardStorageRead()`, `boardStorageWrite()` and
  `boardShowStorageEjected()` (the board owns the flash) against
  `appSetStorageArmed()` / `appStorageArmed()` (`main.cpp` owns USB and
  the `usbmode` NVS namespace). main.cpp owns the normal-operation
  ladder's timing and pushes the resulting hint down, so the board
  renders a label and never duplicates a threshold; it also decides, once
  per press edge via `boardMenuActive()`, whether that press is HID input
  or menu input. Exactly one implementation is compiled per env, so there
  are no board `#ifdef`s in `main.cpp` and nothing in the board layer
  touches USB or the radio. Mode state is passed in rather than
  duplicated there.
- `src/board_s3zero.cpp` — ESP32-S3-Zero: BOOT button (GPIO0, active-low),
  onboard WS2812 (GPIO21). No sensor, no menu, no filesystem and no data
  partition at all, so the measurement, menu, run-storage and USB-drive
  halves of the contract are all no-ops here — `boardHasStorage()`
  returning false is what stops `main.cpp` even *reading* the armed flag
  on this board, so it can never boot into a drive it has no blocks for.
  BLE has no display and no pairing gesture here either — see the Modes
  section above for why that is a refusal rather than an omission. The LED
  lights up while the button is held, in a
  colour identifying the active mode (the six-colour table in Modes,
  above). A press in a BLE mode with no host connected lights it *dim*
  instead of full, which is the board's whole "that went nowhere" story.
  Once a hold crosses
  `MODE_HOLD_MS`, it flashes white for `MODE_SWITCH_FLASH_MS` (500ms) to
  mark the first mode change in that hold, then shows the new pending
  mode's colour; further changes within the same hold skip the flash and
  jump straight to the next colour — six colours now, so one continuous
  hold takes 18s to get back where it started. On boot, the LED also
  briefly flashes the active mode's colour (`BOOT_FLASH_MS`, 200ms) so the
  mode is visible without pressing the button first.
- `src/board_atoms3r.cpp` — AtomS3R: screen button (GPIO41, active-low —
  the whole LCD face is the button), 0.85" 128x128 LCD via M5GFX, and the
  Unit Light's analog output on GPIO1 (ADC1_CH0). There is no separate
  press indicator (no action box, no LED — see the no-usable-RGB-LED
  gotcha below); the recent-measurement line and the histogram, below,
  are the feedback. Normal-operation screen, top to bottom: the last
  measurement (or the live meter reading, in the meter view — see the
  light-meter-is-not-a-mode gotcha), the R/F count-and-mean line, the
  active mode's name — with, in a BLE mode only, a right-aligned
  `LINK`/`ADV`/`PAIR` tag beside it in green/dim/amber — a hint for what
  continuing to hold would do next
  (or the pending mode once a hold has queued a change), and a stacked
  R/F histogram of the last `HIST_CAPACITY` (default 500, overridable)
  samples of each direction sharing one time axis, with the axis's min
  and max labelled at its ends. The headline doubles as the press-time
  BLE feedback: a press with no host connected shows `not connected`
  there and starts no measurement at all, so the statistics are untouched
  by it. A 2s hold replaces all of that with a
  small menu instead — see the menu bullet in the interaction table above
  and the file header in `board_atoms3r.cpp` for the screens it uses. It
  also owns the wear-levelling side of USB drive mode
  (`wl_mount`/`wl_read`/`wl_erase_range`/`wl_write` on the `ffat`
  partition) and the `USB DRIVE` screen a storage boot lives on — which
  is a *menu state* (`MENU_DRIVE`) it starts in and never leaves, so
  `boardMenuActive()` is true for that whole boot and `main.cpp` needs no
  storage-awareness in its press path at all.

  **Nothing here draws from `loop()`.** All panel access and all ADC
  sampling happen in one task (`uiTaskFn`) pinned to core 0, while
  `loop()` has core 1 to itself (`ARDUINO_RUNNING_CORE=1`), so a repaint
  can never stretch a loop iteration and delay noticing the next button
  edge. The entry points called from `loop()` only store a value into a
  `volatile` and call `xTaskNotifyGive()` — a few microseconds, and it
  can't preempt core 1. The task blocks in `ulTaskNotifyTake()` with a
  `LIGHT_PERIOD_MS` (100ms) timeout, so it wakes either on a press/release
  edge or on the timeout. `display.init()` also runs in that task, so
  panel autodetect and backlight bring-up are off the critical core too.
  Menu state (`menuState`, `topIndex`) lives here too, touched only by
  this task — `loop()` only ever requests a menu action
  (`enterMenuRequested` etc.), never mutates the menu directly.

  `runMeasurement()` is the measurement itself, also on core 0. `t0` is
  `esp_timer_get_time()` sampled in `loop()` at the button edge, *before*
  the HID report is queued, and handed over through `boardShowPress()` —
  so the HID send counts as part of what's measured. The crossing
  direction is not configured: a baseline is sampled at the start and the
  clock stops on the first reading that has reached the other side of the
  threshold, which makes a dark screen flashing bright and a bright screen
  going dark both work off one threshold value. The poll is tight and
  unyielding (`analogRead()` is tens of µs, so it resolves far finer than
  a millisecond), bounded by `MEASURE_TIMEOUT_MS` (500ms) — see the
  core-0-starvation gotcha below. Every measurement goes to three places:
  a `DirStats` (running count/min/mean), a `History` ring buffer (the last
  `HIST_CAPACITY` raw values) — one of each per direction, kept separate
  rather than pooled, see the direction-bucketing gotcha below for why —
  and up to `main.cpp` via `appRecordSample()`, which is what puts it in
  a run's file if a run is going.

  It also owns run storage: the `ffat` partition is mounted (formatting it
  if this is a fresh flash) inside `uiTaskFn` at startup, and
  `boardWriteRun()` writes one CSV per automated run. File numbers come
  from a counter in its own `Preferences` namespace (`runlog`/`next`),
  separate from `main.cpp`'s `usbmode`. Format — a `#` metadata block,
  then a header row, then one row per iteration:

  ```
  # usb-latency run 42
  # mode,GAMEPAD
  # planned,500
  # recorded,500
  # aborted,no
  seq,gap_ms,dir,latency_us,status
  1,342,R,24381,ok
  3,455,R,,timeout
  ```

  The mode is a metadata line rather than a column repeated 500 times
  (it's a property of the run, and `#` is the comment character every
  sane CSV reader already takes); `latency_us` is left *empty* on a
  timeout with the reason moved to `status`, so the column stays purely
  numeric instead of carrying a sentinel to be parsed out.

## Gotchas already hit

- **Don't name a local constant `GAMEPAD_BUTTON_X`.** TinyUSB's
  `class/hid/hid.h` (pulled in transitively via `USB.h` → `USBHID.h`)
  `#define`s a macro of that exact name for a *different*, bitmask-style
  enum (`GAMEPAD_BUTTON_3` = `TU_BIT(3)`). It silently collides with an
  identically-named `static const uint8_t` and fails to compile with a
  confusing "redeclared as different kind of entity" error.
- **Button index convention**: `USBHIDGamepad.h` names buttons after Linux
  input-event codes — `BUTTON_A=0, BUTTON_B=1, BUTTON_C=2, BUTTON_X=3,
  BUTTON_Y=4, ...` — not the SDL/XInput convention (A=0,B=1,X=2,Y=3). Use
  the library's own `BUTTON_X` etc. macros rather than hardcoding an index.
- **`neopixelWrite()` is deprecated** on this core version; use
  `rgbLedWrite()` instead.
- **The S3-Zero's onboard WS2812 has red/green swapped** relative to
  `rgbLedWrite()`'s assumed GRB wire order: passing a nonzero `red_val`
  shows as green and a nonzero `green_val` shows as red (confirmed on
  hardware — a "keyboard mode" LED meant to be green displayed red, and
  vice versa for gamepad mode). `blue_val` is unaffected.
  `src/board_s3zero.cpp` compensates in a `setPixel(r, g, b)` wrapper that
  swaps the first two args before calling `rgbLedWrite()`, so every other
  call site can use normal, intuitive `(r, g, b)` values.
- **Mode switching needs a reboot, not a live descriptor swap.** The
  ESP32 Arduino core's TinyUSB HID wrapper (`USBHID.cpp`) registers a
  device's report descriptor once, unconditionally, in that device
  class's *constructor* (`USBHIDGamepad::USBHIDGamepad()` etc. all call
  `hid.addDevice(this, ...)` outside of `begin()`) — so merely skipping
  `.begin()` on the classes you don't want active does not stop them
  contributing a collection to the composite descriptor; declaring all
  three as globals (the original multi-mode design) is exactly what made
  the device enumerate as a multi-collection gadget that macOS then shows
  as several devices. There is also no supported API in this core to tear
  down and rebuild the HID descriptor at runtime (`tinyusb_hid_is_initialized`
  latches after the first load, and the registration state is `static` file
  scope inside `USBHID.cpp`, unreachable from sketch code). The fix: only
  ever construct the one device class matching the persisted mode
  (`new` it in `setup()`, gated on a value read from `Preferences`/NVS).
  A reboot is therefore unavoidable to change modes — but see the next
  gotcha for why that reboot is a manual step, not something the sketch
  triggers itself.
- **`ARDUINO_USB_CDC_ON_BOOT=1` broke all HID functionality once mode
  selection and `USB.productName()` moved into `setup()`.** When that
  flag is `1`, the Arduino core's own `main.cpp` (`app_main()`) calls
  `USB.begin()` itself — synchronously, before `initArduino()`, i.e.
  before `setup()` ever runs. Once this sketch's HID device selection and
  `USB.productName()` call moved from being unconditional globals into
  `setup()` (to support single-device mode + reboot-based switching), that
  automatic early `USB.begin()` finalized the USB descriptor *before* our
  code ran, so the HID interface/product name we configured never made it
  into the enumerated device — the device still enumerated fine (as
  CDC-only), the LED and NVS-persisted mode still worked (neither depends
  on USB enumeration), but no gamepad/keyboard/mouse report ever reached
  the host. Fix: set `ARDUINO_USB_CDC_ON_BOOT=0` so nothing calls
  `USB.begin()` before this sketch's own `setup()` does, and begin a
  manual `USBCDC` there instead if serial output is still wanted. Rule of
  thumb for this project: anything that must precede `USB.begin()`
  (`USB.productName()`, `USB.VID()`, HID device construction, etc.) must
  happen in `setup()`, in a sketch that fully owns when `USB.begin()` is
  called — which requires this flag to stay `0`.
- **Mode switching must not reboot automatically — GPIO0 is also the
  chip's boot-mode strapping pin.** An earlier version called
  `ESP.restart()` itself once a hold crossed `MODE_HOLD_MS`. But the
  hold-to-switch gesture means BOOT (GPIO0) is, by definition, still held
  low at that instant — and if GPIO0 is low when the chip actually
  resets, the ROM bootloader reads that as "enter USB/UART download mode"
  instead of booting this firmware, so the device never re-enumerates as
  anything HID at all. This was timing-dependent on how fast the user let
  go relative to the restart call, which made it look like a flaky,
  mode-specific bug (worked for two transitions, then didn't) rather than
  what it was: this hazard on every single mode-switch reboot. An
  in-between fix (block until BOOT reads released before calling
  `ESP.restart()`) worked but was fragile in spirit — it's still a
  software-triggered reset racing a strapping pin. The real fix: don't
  reboot from code at all. Holding only advances `pendingMode` and
  persists it (`advancePendingMode()`); actually applying it requires a
  manual reboot (reset button or unplug/replug), at a time when BOOT is
  definitely not being touched. Same underlying hazard as the "don't hold
  BOOT while plugging in" note above — this just avoids ever creating the
  race in the first place.
- **`pio run` prints a scary esptool install failure and succeeds
  anyway.** Every build logs `idf_tools.py installation failed` and
  `tool-esptoolpy does not appear to be a Python project` (the package
  has no `pyproject.toml`/`setup.py` for `uv pip install -e` to chew on),
  plus an unrelated `esp-idf-size ... No such option '--ng'`. Neither is
  fatal: a bundled `esptool v5.4.0` still runs, and both
  `bootloader.bin` and `firmware.bin` are produced for both envs. Only
  `-t upload` has not been re-verified since — if flashing does fail, the
  `tool-esptoolpy` install is the thing to fix, not the build.
- **There is no `m5stack-atoms3r` board in the platform** — `pio boards
  m5stack` lists only `m5stack-atoms3` / `m5stack-atoms3u`. The AtomS3
  profile is used instead: same ESP32-S3, same 8MB flash, same native-USB
  wiring. The one real difference, the panel, is autodetected by M5GFX at
  runtime, so nothing in the board profile needs to know about it.
- **M5Stack (and Espressif devkit) board profiles hardcode
  `ARDUINO_USB_MODE=1` in their `build.extra_flags`**, which this project
  needs to be `0`. Adding `-DARDUINO_USB_MODE=0` to `build_flags` does not
  replace it — both reach the compiler, producing a `"ARDUINO_USB_MODE"
  redefined` warning and leaving the outcome to flag ordering. The fix is
  to override `board_build.extra_flags` in the env, restating the
  profile's other flags (`-DARDUINO_M5Stack_ATOMS3`,
  `-DARDUINO_RUNNING_CORE=1`, `-DARDUINO_EVENT_RUNNING_CORE=1`) and simply
  omitting the USB one. Both envs do this.
- **Don't read the AtomS3R button through M5Unified's `M5.BtnA`.**
  `Button_Class` debounces on the trailing edge with a 10ms default
  threshold — i.e. it deliberately adds up to 10ms before reporting a
  press, which is exactly the quantity this project exists to measure.
  `src/board_atoms3r.cpp` reads GPIO41 directly with the same
  leading-edge/lockout debounce the S3-Zero uses, and pulls in M5GFX alone
  (not M5Unified) for the display. Pin confirmed from M5Unified's own
  board tables: AtomS3 / AtomS3Lite / AtomS3U / AtomS3R all use GPIO41,
  active-low.
- **The AtomS3R's LCD backlight is not on a GPIO** — unlike the plain
  AtomS3, it hangs off an LP5562 LED driver on an internal I2C bus
  (SDA GPIO45 / SCL GPIO0), and the panel itself is a GC9107 or an ST7735S
  depending on batch. That's the reason this variant depends on M5GFX
  rather than hand-rolling a panel driver: `display.init()` probes for
  both panels and installs the right backlight shim. Note the consequence
  for GPIO0 — on the AtomS3R it is an I2C clock line, not a button, so
  none of the GPIO0-strapping-pin warnings elsewhere in this file apply to
  that board.
- **Unit Light analog output lands on GPIO1, not GPIO2.** Chasing it
  through three mappings: the unit (U012, photoresistor + LM393) puts its
  *digital* comparator output on the yellow wire and its *analog* output
  on the white wire; on a Port A Grove connector yellow is the SDA line
  and white is the SCL line; and M5Unified's pin table gives the AtomS3R
  external port as SCL=GPIO1, SDA=GPIO2. So analog → GPIO1 (ADC1_CH0),
  digital → GPIO2. Note the inversion in M5Unified's naming while you're
  in there: `port_a_pin1` is SCL and `port_a_pin2` is SDA, which is the
  opposite order from the wire colours. Only the analog side is read.
  Caveat not yet checked on hardware: the unit is a 5V part, so if its
  analog swing really does reach 5V it will clip at the ADC's ~3.3V
  ceiling (4095) rather than damaging anything visible in the reading.
- **Sensor configurations are a runtime picker now — the
  `-DLIGHT_SENSOR_PIN`/`-DLIGHT_GND_PIN` build knobs are GONE** (they
  required a reflash to swap sensors, exactly the wrong shape once two
  sensors were wired at once). The `SENSORS[]` table in
  `board_atoms3r.cpp` holds the configs: [0] Unit Light (Grove analog on
  GPIO1, default threshold `LIGHT_THRESHOLD` 3000) and [1] BPW34 (bare
  photodiode straddling the bottom pads — 5.08mm lead pitch lands anode
  on G6/sense, cathode on G8, which `activateSensor()` drives LOW as a
  virtual ground; default threshold `BPW34_THRESHOLD` 250, photovoltaic
  swing tops out ~0.35–0.45V). The menu's `Sensor` item cycles configs,
  applying and persisting per tap (NVS `sensor`/`cfg`) and clearing the
  stats/histogram, since one circuit's numbers mean nothing against
  another's. Index 0 must stay the sensor a fresh device is assumed to
  have: a stored selection of unwired hardware reads a floating pin, so
  the safe config is what NVS-less boots get. **Calibrated thresholds
  are per-config** — NVS keys `thr0`/`thr1`, loaded by
  `activateSensor()`, written by the calibration report's hold — one
  global key would let calibrating one sensor clobber the other's. (The
  old global `thr` key is simply ignored now; a leftover value there
  does nothing.) A meter view pinned near zero in bright light on the
  BPW34 config means the diode is backwards — swap the legs. The
  pinMode-exactly-once peripheral-manager rule still applies to the
  ground pin, with the one licensed exception that `activateSensor()`
  itself re-asserts OUTPUT/LOW on every activation.
- **The on-screen numbers don't update until the measurement finishes.**
  This is deliberate, not a dropped frame: the UI task must not be
  pushing pixels over SPI while it is sampling the sensor, because a
  repaint both delays the first sample and can straddle the very change
  being timed. So `boardShowPress()` hands over the timestamp, and
  everything — the recent-measurement line, the histogram — repaints
  after `runMeasurement()` returns, tens of milliseconds normally,
  `MEASURE_TIMEOUT_MS` in the no-response case.
- **While the menu is open, no press reaches `sendPress()`/`sendRelease()`
  at all.** `main.cpp` decides this once per press, at the edge
  (`wasMenuActiveAtPress = boardHasSensor() && boardMenuActive()`), and
  deliberately does *not* re-check `boardMenuActive()` live for the rest
  of that hold or at release. The reason: the hold that *opens* the menu
  (crossing the 2s threshold) does so partway through its own press — if
  release-time code re-checked `boardMenuActive()` fresh, that release
  would see the menu already open and misfire as the menu's first
  navigation tap. Capturing the decision once, at the press edge, makes
  that entry hold's release read as an ordinary HID release (matching the
  physical gesture the host actually saw) instead.
- **`runMeasurement()` deliberately starves core 0's idle task**, which is
  why `MEASURE_TIMEOUT_MS` exists and why it is 500ms. The poll loop does
  not yield: yielding on every sample (`vTaskDelay(1)`) would cap
  resolution at the 1ms tick, which is 5% of a typical click-to-photon
  figure and defeats the point. 500ms is 10% of the 5s task-watchdog
  budget, so the starvation is safe; raising the timeout much past ~2s
  would not be. None of this touches core 1, where `loop()` runs, and the
  USB task sits at a higher priority on core 0 and still preempts the poll
  freely.
- **The threshold is a compile-time default, not a runtime setting.**
  `LIGHT_THRESHOLD` defaults to 3000 and is `#ifndef`-guarded, so
  `-DLIGHT_THRESHOLD=<counts>` in an env's `build_flags` overrides it.
  There is no button UI for it: the button already carries press-to-send
  and hold-to-cycle, and a third gesture would be one too many. The
  meter view draws the threshold as a tick on its bar, which is how you
  check the value is in the right place without being able to edit it
  live.
- **The light meter is not a `Mode`, and making it one was a mistake worth
  not repeating.** It was briefly `MODE_LIGHT`, a fourth entry in the
  rotation that constructed no HID device — which meant switching into or
  out of it needed a reboot, for a feature that touches no USB state at
  all. It also meant a press sent nothing while the meter was up, so you
  couldn't make the display do the thing you were trying to aim at. The
  split that actually holds: a `Mode` is a host-facing identity
  (boot-fixed, reboot to change, six of them now), and the meter is a
  *view* the board layer flips at runtime — from the menu now (its "Light meter" item), no
  USB involvement, HID reports and measurements carrying on underneath it
  whenever the menu isn't the one holding the button. If something new
  needs switching, the first question is still which of those two it is.
- **The AtomS3R has no usable RGB status LED — don't try to add one.**
  Confirmed on hardware after a failed attempt. The docs' pin map lists
  "LP5562 (RGB Driver)" on the internal I2C bus, which reads like there's
  an addressable status LED behind it, and the chip *is* there — M5GFX
  drives the LCD backlight through it on the W channel (register 0x0E),
  which works. But the R/G/B channels (registers 0x04/0x03/0x02) reach
  nothing accessible on this board: bringing them up exactly as M5GFX
  brings up W — the chip is already enabled, clocked, and has LED_MAP = 0
  putting every channel under direct PWM control, so it is only a register
  write per channel — produces no visible light. Whatever the reason
  (unpopulated, or not wired out), there is nothing to drive. The only
  press indicator on this board is the on-screen recent-measurement line
  and histogram, and note that neither can repaint until
  `runMeasurement()` returns.
- **Latency stats are kept as two populations, R and F, not one pooled
  average.** `runMeasurement()` already knows which way the ADC crossed
  the threshold (`waitForRise`, needed anyway to support both a
  dark-to-light and a light-to-dark change off the one threshold value —
  see the direction-inference comment on that function). Bucketing by that
  same flag rather than discarding it after use is deliberate: a
  photoresistor does not generally respond at the same speed getting
  brighter as it does getting dimmer, so a pooled average of both
  directions can look bimodal for a reason that has nothing to do with
  the thing being measured. Splitting the stats turns "is this bimodal"
  from a guess into something you can just read off the screen — if R and
  F have visibly different means, that's the sensor; if they're the same
  and each is *individually* bimodal, look at the display's refresh
  timing instead. Which of R (rise) or F (fall) is your display's
  dark→light vs light→dark isn't asserted anywhere in the firmware — it
  depends on the specific photoresistor circuit's polarity, which hasn't
  been characterized here. Read it off the meter view: watch which way the
  bar moves for a known transition.
- **The automated test paces itself on measurement completion, not on a
  guessed interval.** `serviceAutoTest()` won't release a press — and so
  won't start the next one — until `boardMeasurementBusy()` goes false.
  Sequencing on a fixed delay instead would eventually start a press
  while the previous measurement was still running, and the new press
  overwrites the `pressMicros` that measurement is still timing against,
  silently corrupting the sample. Note `measureBusy` is a *separate* flag
  from `measurePending`: the UI task clears pending the moment it picks
  the work up, which is the start of the measurement, not the end.
  There's still a hard `AUTO_HOLD_MAX_MS` ceiling on how long a press may
  stay down regardless — a wedged measurement must not leave the host
  holding a HID button forever, which is a much worse failure than a lost
  sample.
- **The gap between automated presses is random on purpose.** A fixed
  interval can alias with the display's refresh cadence, parking every
  sample at the same phase within a frame and biasing the distribution
  the tool exists to measure — which matters especially here, given the
  bimodality this firmware was being used to chase. The first gap is
  randomised too rather than firing immediately, because the button *is*
  the screen face, inches from wherever the sensor is aimed: the instant
  the user lets go of it is exactly the wrong time to take a reading.
  That first gap additionally carries a fixed `AUTO_START_DELAY_MS` (1s)
  on top of the random part — *added*, not substituted, so the guarantee
  of a second's positioning time doesn't cost the first press of every
  run its random phase within a frame. It goes into `runGapMs` rather
  than only into the deadline, so the run file's `gap_ms` for row 1 is
  the gap that actually preceded it. A validation run shares this pacing
  and so gets the same 1s.
- **No flash write may happen while a run is in progress — and this is
  not a performance nicety.** An erase or program cycle on the ESP32-S3's
  SPI flash stalls the flash cache, which stalls instruction fetch on
  *both* cores, not just the one doing the writing. `runMeasurement()`
  polls an ADC in a tight loop on core 0 and `loop()` watches the button
  on core 1; a multi-millisecond cache stall in the middle of either one
  lands directly in the number the tool exists to measure. So the whole
  run is buffered in RAM (`runSamples[]` in `main.cpp` — 500 samples is
  4KB against 320KB, i.e. nothing) and written exactly once, after the
  last sample. The same reasoning is why the `ffat` mount happens during
  `uiTaskFn`'s startup rather than lazily on first use: boot is the one
  moment when nothing is being timed.
- **Storage is FFat (wear-levelled FAT), not LittleFS or SPIFFS, and that
  choice is load-bearing.** USB drive mode exposes the `ffat` partition
  raw over USB MSC, so the host mounts the flash itself and reads run
  files with no firmware in the loop — which only works if what's down
  there is a genuine FAT volume. Consequences worth remembering: filenames are 8.3
  and uppercase (`RUN00042.CSV` — a long name would cost VFAT entries and
  get mangled), and the run counter wraps at 99999 rather than growing a
  sixth digit.
- **`USBMSC`'s constructor registers the mass-storage interface, exactly
  like the HID classes register their report descriptors.** Same file,
  same pattern (`USBMSC::USBMSC()` calls `tinyusb_enable_interface(
  USB_INTERFACE_MSC, ...)` unconditionally), so the rule established for
  HID extends verbatim: *only* construct the device class the boot means
  to be. `msc` is a pointer `new`'d in `setup()` behind the armed-flag
  check, never a global — a global would put a drive interface into every
  gamepad's descriptor. The corollary is what keeps the S3-Zero clean:
  its `boardHasStorage()` is false, so the `new USBMSC()` is never
  reached and that env's descriptor is byte-for-byte what it was before
  any of this existed, despite `USBMSC.h` being included unconditionally.
- **Storage is a `Mode`, but it must not be in the mode *rotation* —
  hence the gap after `MODE_COUNT` — even though the AtomS3R's picker
  offers it as a seventh candidate.** Those are two different lists, and
  keeping them apart is the whole trick. Storage genuinely is an identity
  of its own (the fourth when this was written, the seventh now): fixed
  at boot, decided before `USB.begin()`, with its own product string. So
  it's a `Mode`, and that is what makes it free in the press path —
  `sendPress()`/`sendRelease()` already end in `default: break;`, which
  is precisely "this identity sends nothing", so no new branch goes into
  the hot path and no null HID pointer can be dereferenced. But
  `MODE_COUNT` — 3 when this was written, 6 since the BLE modes landed —
  bounds the *rotation*, and `MODE_STORAGE` sits *past* it, so
  `(pendingMode + 1) % MODE_COUNT` structurally cannot produce it however
  the count grows: the S3-Zero's hold-to-cycle gesture, which is literally
  that expression, cannot reach a drive on a board with no partition to
  expose. Note `MODE_COUNT` doubles as `board_atoms3r.cpp`'s "nothing
  drawn yet" sentinel, which is the other reason `MODE_STORAGE` could not
  simply *be* `MODE_COUNT`. The persisted value is separate again
  (`usbmode`/`storage`): the `mode` key must keep holding the HID
  identity to come back to, so a fourth enum value in it would have had
  nowhere to remember that.

  The AtomS3R picker is the deliberate exception, and it does **not**
  weaken any of the above, because it never touches this enum. Its list
  is `0..MODE_COUNT` inclusive, where `MODE_COUNT` stands in for a
  **virtual** `USB DRIVE` entry; landing there calls
  `appSetStorageArmed(true)` and leaves `pendingMode` alone, landing on
  any real mode calls `appSetStorageArmed(false)` and then
  `appSetPendingMode()`. That last disarm is not cosmetic: the armed flag
  outranks the mode key at boot, so a picked mode with the flag still set
  would silently be ignored on the next reset. The candidate shown is
  derived from the truth each frame (`appStorageArmed() ? drive :
  pendingMode`) rather than tracked in a variable of its own, with the
  same precedence boot uses, so the screen cannot drift from what a reset
  would do. This replaced a separate `MENU_STORAGE` arm/disarm picker
  reached from a top-level `USB drive` item — two identical-looking
  screens answering one question ("what does the next reset come up
  as"), which is one screen too many.
- **Storage mode needs no special case in `main.cpp`'s loop, because it
  reuses "the menu owns the button".** A drive boot starts in a menu
  state (`MENU_DRIVE`) and never leaves it, so `boardMenuActive()` is
  true from the first `loop()` pass onward and the existing branch
  already does everything wanted: no HID report, no measurement started,
  no hold ladder, and the automated test unreachable (its menu item
  cannot be highlighted on a screen with no items). Getting there needed
  one ordering detail: `menuState` is set synchronously in
  `boardShowBoot()` on core 1, *not* left for `uiTaskFn` to notice —
  `main.cpp` starts calling `boardMenuActive()` on its first loop pass,
  which can beat a freshly created task to its first iteration, and a
  drive boot that lost that race would route its first press at a HID
  device that does not exist.
- **The firmware must not have FFat mounted on `ffat` while MSC is
  exposing it.** Two FAT drivers on one volume — the host's, with its own
  block cache, and FatFs's — diverge silently and end with a directory
  table that makes sense to neither. So `uiTaskFn` skips `FFat.begin()`
  entirely when `wantActive == MODE_STORAGE`, which is why run recording
  is inert that boot (`storageReady` stays false). Nothing is lost by it:
  no run can be started from a screen with no menu items. The inverse
  holds too — `boardStorageBegin()`'s `wl_mount()` is the *only* claim on
  the partition in a drive boot.
- **MSC block size is `wl_sector_size()` (4096 here), not the customary
  512.** The FAT volume on `ffat` was formatted by FatFs on top of wear
  levelling with 4096-byte logical sectors (`CONFIG_WL_SECTOR_SIZE`, and
  the S3 sdkconfig picks 4096), so its BPB says 4096. Exposing 512-byte
  USB blocks would leave every host's FAT driver reconciling a 4096-byte
  BPB against 512-byte blocks for no benefit, and would make the
  LBA↔offset arithmetic stop matching FatFs's. Matching instead means the
  MSC glue is literally ESP-IDF's `fatfs/diskio/diskio_wl.c`: block count
  is `wl_size(h) / wl_sector_size(h)`, block N lives at
  `N * wl_sector_size(h)`, and writing one is `wl_erase_range()` that
  sector then `wl_write()` it whole. Unverified on hardware — a host that
  refuses 4096-byte USB blocks would be the first thing to check if the
  drive enumerates but won't mount.
- **`wl_write()` will not write a partial sector — it needs the sector
  erased first.** So the MSC write callback cannot pass its buffer
  straight through: erasing to write a fragment would destroy the
  fragments already staged for that block. Chunks are accumulated in a
  one-sector RAM buffer and committed only when a whole sector is
  present. In this core `CFG_TUD_MSC_EP_BUFSIZE` is 4096, equal to the
  block size, so the host always fills it in one call and the staging is
  a no-op — but that's a build-time constant in somebody else's package,
  not a contract, and the SCSI callback signature has an `offset`
  parameter precisely because fragments are legal.
- **Blocking on flash inside the MSC callbacks is fine, and it is the one
  place in this firmware where that's true.** They run on the TinyUSB
  task and an erase stalls the cache on both cores — the exact hazard the
  no-flash-writes-during-a-run gotcha exists for. It doesn't bite here
  because a drive boot measures nothing: no HID device, no sensor poll,
  no run. If measurement ever became reachable from a storage boot, this
  would immediately become a real problem.
- **Leaving USB drive mode is a hold plus a *manual* reset, and the
  no-automatic-reboot rule is why.** The drive screen's 1s hold only
  toggles the armed flag; it cannot change what the host is mounting
  right now (the descriptor was fixed before enumeration) and it
  deliberately does not call `ESP.restart()` — see the
  mode-switching-must-not-reboot gotcha above, which this follows rather
  than re-litigates. Taps on that screen are ignored outright: the whole
  LCD face is the button, and a stray brush while the host is copying
  files must not change what the next reset does. Storage is sticky
  rather than one-shot on the same reasoning that made it a `Mode` — a
  drive that turned back into a gamepad on every replug is useless for
  carrying files between machines.
- **The AtomS3R top menu is six items in a BLE mode, and the row pitch
  has shrunk twice for them.** 16px from y=28 fitted four; 14px from y=22
  fitted five; six (the BLE list, with `Pairing`) would have put the last
  row at y=92, overlapping the "tap: next" hint at y=96, so it is 12px
  from y=20 now — last row at y=80, eight clear pixels below it, and
  Font0 being 8px tall means a 12px pitch still shows a gap between rows.
  **That is the panel full.** A seventh item needs a scrolling menu, not
  another row. Note the items are also no longer a fixed array indexed by
  row: `buildTopMenu()` composes the list once per boot from the mode, and
  the select switch dispatches on a `TopItem` identity, so a list that
  changes length cannot silently make `case 3:` mean something new.
- **There is no Bluedroid in this platform's prebuilt libraries, so the
  Arduino core's bundled `BLE` library cannot be used at all.** The
  library is right there in
  `framework-arduinoespressif32/libraries/BLE` (ESP32 BLE Arduino 2.0.0,
  `BLEDevice.h`, `BLEHIDDevice.h`), which makes it look like the obvious
  route — and so does ESP32-BLE-Gamepad / -Keyboard / -Mouse, which are
  all built on it. But
  `framework-arduinoespressif32-libs/esp32s3/sdkconfig` says
  `CONFIG_BT_NIMBLE_ENABLED=y` with `# CONFIG_BT_BLUEDROID_ENABLED is not
  set`, i.e. the host those libraries call into is not in `libbt.a`.
  Check that sdkconfig before reaching for any BLE library, not after the
  link errors. The host that IS there is NimBLE, and
  `h2zero/NimBLE-Arduino` is the Arduino-facing wrapper for it —
  including `NimBLEHIDDevice`, which builds the whole HoGP service (report
  map, input report plus report-reference descriptor, HID info, HID
  control point, protocol mode, plus Device Information and Battery) from
  a report descriptor you hand it. Version 2.5.1 builds clean against
  Arduino core 3.3.0 / IDF 5.x with no compatibility shims.
- **The BLE name and the USB product name have to be two different
  strings, and the binding limit is 31 bytes, not the 29 you'd expect.**
  Two separate caps apply, and "USB Latency Tester - BLE Keyboard" (33)
  busts both:
  - **GAP device name, 31 bytes.** `ble_svc_gap_device_name_set()`
    returns `BLE_HS_EINVAL` for anything longer, so the name is
    **silently not set** and the device keeps NimBLE's default. This is
    the nasty one: nothing fails, nothing logs, the device just has the
    wrong name. And
    `CONFIG_BT_NIMBLE_GAP_DEVICE_NAME_MAX_LEN` is *not* `#ifndef`-guarded
    in NimBLE-Arduino's `nimconfig.h`, so it cannot be raised with a
    `-D`. Note NimBLE-Arduino compiles its own copy of the NimBLE host
    (only the controller comes from `libbt.a`), so its `nimconfig.h` is
    what governs, not the IDF `sdkconfig`.
  - **Advertisement name field, 29 bytes** (`BLE_HS_ADV_MAX_FIELD_SZ`).
    NimBLE does not refuse an over-long one here — it truncates and
    downgrades the AD type from "complete local name" to "shortened local
    name", which would have put `USB Latency Tester - BLE Keyb` in every
    picker.

  So `modeBleName()` is a second, shorter set of strings (`Latency Tester
  BLE Keyboard`, 27 bytes — slack against both limits rather than sitting
  exactly on one) used for the GAP name *and* the advertisement, while
  `modeProductName()` stays the USB product string, where neither limit
  applies. Related ordering trap in the same place:
  `NimBLEAdvertising::setName()` only puts the name in the scan response
  if `enableScanResponse(true)` has *already* been called — otherwise it
  spends the advertisement's own 31 bytes on it, which are already taken
  by flags, appearance, the HID service UUID and the preferred connection
  interval.
- **`runMeasurement()`'s unyielding poll does not starve NimBLE, and the
  reason is priority, not luck.** The existing core-0-starvation gotcha
  says the poll deliberately never yields for up to `MEASURE_TIMEOUT_MS`,
  and NimBLE's host and controller tasks are pinned to core 0 as well
  (`CONFIG_BT_NIMBLE_PINNED_TO_CORE=0`) — which reads like a
  half-second hole in every BLE measurement. It isn't: `uiTaskFn` runs at
  priority 1 and the BLE tasks run near the top of the range
  (`ESP_TASK_BT_CONTROLLER_PRIO` is `configMAX_PRIORITIES - 2`), so they
  preempt the poll freely, exactly as the USB task already did. What the
  poll starves is core 0's *idle* task and nothing else. Anything ever
  added to core 0 at priority 1 or below, however, would genuinely be
  blocked for the length of a measurement.
- **A BLE send must not be treated as a USB send that happens to be
  wireless, but `t0` still does not move.** The connection-interval wait
  is real BLE latency and belongs inside the measurement, so `t0` is
  still `esp_timer_get_time()` at the button edge, before the report is
  queued, and nothing is subtracted anywhere. What was checked is the
  other half: that `sendPress()` does not *block* `loop()`.
  `NimBLECharacteristic::notify(value, len, connHandle)` allocates an
  mbuf and calls `ble_gattc_notify_custom()`, which queues the ATT PDU on
  that connection's transmit queue and returns — it neither waits for the
  next connection event nor for an acknowledgement. (An *indication*
  would wait for the acknowledgement. That is why this is a notification.)
  The connection handle is passed explicitly rather than left to default
  for a second reason: the no-handle overload walks
  `getServer()->getPeerDevices()`, which returns a `std::vector` **by
  value** — a heap allocation on the press path. The only thing left that
  can block is NimBLE's host mutex, for microseconds, on the other core.
  None of this has been timed on hardware.
- **Connected is not the same as "a press will land", so the send path
  gates on subscription.** A host can be connected, and even bonded,
  without having written the input report's CCCD — and a notification
  sent in that window goes nowhere. `bleReady` is therefore set from
  `NimBLECharacteristicCallbacks::onSubscribe()` (bit 0 of `subValue`,
  "notifications enabled"), not from `onConnect()`, and it is what both
  `sendPress()` and the on-screen `LINK` tag mean. The cost is that the
  brief connect-pair-subscribe window shows as `ADV`, which is honest:
  nothing sent in it would have arrived.
- **A press with nothing to send must not start a measurement.** The
  temptation is to let it run and record the timeout — it is, after all,
  a press that produced no photons. But a timeout means "the report went
  out and the display didn't answer", which is a statement about the
  machine under test, and this is "nothing went out", which is a
  statement about the device. Recording them in the same bucket would put
  a `MEASURE_TIMEOUT_MS` sample into the R/F statistics and a `timeout`
  row into the run file for a reason that has nothing to do with the
  display. So `sendPress()` returns a bool, `boardShowPress()` takes it
  as `sent`, and the AtomS3R shows `not connected` in the headline
  *instead of* starting the clock — and in particular does not set
  `measureBusy`, which the automated test paces itself on and would
  otherwise wait forever for.
- **`pio pkg install` rewrites `platformio.ini` and deletes every comment
  in it.** It reserialises the file from its parsed form, so the entire
  commented rationale (the CDC-on-boot explanation, the
  `board_build.extra_flags` restatement, the partition-table reasoning)
  vanishes in exchange for adding one `lib_deps` line. This has already
  happened once here and had to be restored by hand. Add dependencies by
  editing `lib_deps` directly; `pio run` fetches whatever is missing on
  the next build.
- **The platform this actually builds with is not pinned.** `platform =
  espressif32` resolves to the newest installed copy, and there are two:
  54.03.20 and 55.03.30 (Arduino core 3.3.0 / IDF 5.5). Builds currently
  pick **55.3.30**, not the 54.03.20 the project was originally developed
  against. Nothing has broken because of it, but a build that suddenly
  behaves differently after an unrelated `pio pkg update` is worth
  suspecting here first — pin the version in `[env]` if that ever
  matters.
- **Don't move or resize the `nvs` partition.** `partitions_atoms3r_8MB.csv`
  keeps it at `0x9000`, size `0x5000`, identical to the stock
  `default_8MB.csv` the AtomS3 board profile ships. That's what lets an
  existing device be reflashed with the new table and still come up in the
  mode it was left in — the mode selection, the run counter, and now
  NimBLE's bonds all live in NVS. A table that shifted `nvs` by even one
  sector would silently reset every device back to gamepad mode and
  forget every host it had ever paired with, and it would look like
  a firmware bug rather than a partitioning one. The same applies to the
  S3-Zero's stock `default.csv`, which puts `nvs` at the same place.
- **The `ffat` partition is formatted on first mount, and the screen is
  black while that happens.** `FFat.begin(true)` — format-on-fail — is
  required, not optional: a device flashed with this table for the first
  time has whatever the old SPIFFS partition left behind at `0x410000`,
  which will not mount as FAT. The format runs inside `uiTaskFn`, and that
  task is also the one that draws, so the very first boot after reflashing
  shows nothing until it finishes. One-off, not a hang. Note this is the
  only thing that ever formats the volume, and a drive boot skips it (see
  the two-writers gotcha above) — which is fine only because arming a
  drive requires reaching the menu on an ordinary boot first, so the
  format has always already happened by then. A device that could somehow
  come up as a drive on its very first boot would hand the host an
  unformatted volume.
- **`appRecordSample()` must be called before the board drops
  `measureBusy`, not after.** It's the same handover as `pressMicros` in
  the other direction: the automated test on core 1 waits for
  `boardMeasurementBusy()` to go false and then reads the sample the UI
  task left for it, so "result stored, *then* flag cleared" is the entire
  synchronisation. Calling it from `uiTaskFn` after `runMeasurement()`
  returns would look equivalent and would race — which is why the calls
  live at `runMeasurement()`'s own two exit points instead.
- **A run stopped mid-press still has a measurement running on the other
  core, so the file write is deferred rather than done at the abort.** The
  abort is handled on core 1 at the press edge, which can be tens of
  milliseconds into core 0's measurement of the previous press — a
  perfectly good sample that belongs in the file. `stopAutoTest()`
  therefore only raises `runFlushPending` (plus `runAwaitSample`, if the
  run was in its holding phase), and `finishRun()` waits for the board to
  report nothing outstanding before collecting the straggler and writing.
  `runAwaitSample` is what keeps that wait from swallowing a measurement
  belonging to some *manual* press made in the meantime: a run that ended
  normally already has its last sample, so it never waits at all.
- **Each BLE mode is its own Bluetooth device, with its own banked bonds
  — sharing one identity forced a forget-and-re-pair on every mode
  switch.** Two mechanisms, both real, observed on hardware: hosts cache
  the GATT database/HID report map *and* their pairing keys per device
  address, so one address serving three different report maps breaks the
  host's cache on every switch; and NimBLE's bond store
  (`ble_store_nvs.c`, NVS namespace `nimble_bond`, blob-only) is keyed by
  *peer*, so the same host pairing in a second mode overwrites the first
  mode's keys device-side. Fix in `main.cpp`: `bleIdentityFor()` gives
  each mode a static random address (BT MAC, mode index XORed into the
  low byte, top two bits forced to 0b11 — and note NimBLE addresses are
  little-endian, `out[5]` is the MSB); `bleBankSwitch()` swaps the whole
  `nimble_bond` namespace against per-mode `bondbank_N` namespaces on
  each BLE boot, before `NimBLEDevice::init()` reads the store. Owner
  tracked in the `usbmode` prefs under `bondowner`. Bonds from
  pre-banking firmware are wiped rather than adopted (they'd pair one
  mode with keys the host now files under a different identity) — one
  final re-pair per mode is the migration cost. A side benefit: the
  pairing menu item's `deleteAllBonds()` now only clears the active
  mode's bonds. Call-order trap: `setOwnAddr()` must precede
  `setOwnAddrType(BLE_OWN_ADDR_RANDOM)` — the latter validates that a
  random address is already installed.
- **In the meter view, the Auto test menu slot runs a two-phase threshold
  calibration instead (label: "Find threshold").** Two labelled 10s
  captures (`SENSOR_CAPTURE_MS` each, ~1kHz raw ADC into a static 4096-bin
  histogram per phase — 8KB each, far too big for the UI task's stack):
  the user aims the sensor and selects, capture 1 runs, **the firmware
  presses its own button** to flip the display to its other state, and
  capture 2 runs. The
  suggested threshold is the dead centre of the measured gap between the
  two sets, with the margin (counts of clear air each side) reported
  next to it — or, when the sets overlap, an honest wrong-side sample
  count instead of a fake margin. This replaced a single-capture Otsu
  design within a day of it landing: labelling the sets beats clustering
  a mixture, and the margin becomes a measurement rather than an
  inference. **It runs unattended** — the manual "switch the display and
  tap" step in the middle is gone. Timeline: `GET READY` frame plus
  `CAL_START_DELAY_MS` (1s, positioning time — the hand that selected the
  item is still on the screen face the sensor is aimed at, and capture 1
  would otherwise open by characterising a finger), capture 1 (10s),
  `SWITCHING` frame plus one firmware-made press, `CAL_SETTLE_MS` (500ms,
  for the host to notice the report, the page to repaint, the panel to
  finish its transition and the sensor to follow), capture 2 (10s),
  report. About 22s end to end.

  The press is the part that could not stay in this file: only `main.cpp`
  may touch USB or the radio, and `runThresholdCal()` is blocked on the
  UI task for the whole flow. So the board asks
  (`appCalPress()` / `appCalPressBusy()` in `board.h`) and `loop()`
  services it on core 1 — `sendPress()`, `AUTO_HOLD_MS`, `sendRelease()`,
  paced across passes like the automated test's press rather than held
  through a `delay()`. Two deliberate details there: the busy flag is
  raised by `appCalPress()` itself, because `loop()` can finish the whole
  press before the waiter next looks; and **`boardShowPress()` is not
  called**, so the press starts no measurement — a measurement would have
  the UI task timing the same ADC the capture is sampling, for a press
  nobody asked to time.

  `menuState` is still `MENU_CAPTURE` for the *whole* flow, so no press
  **of the user's** is ever a HID send. What that rule protects has
  changed shape, and the split is now: the firmware's press is the one
  input event the flow wants, and the user's button is the **abort**. It
  was the "I've set the other state, go" tap in the manual design; with
  no manual step left, stop is the only thing it can usefully mean — and
  a user press that also clicked would flip the display halfway through a
  capture, which is the old hazard from the other end. Both gestures
  abort, `calAbortRequested()` is checked in every wait *and* inside both
  capture loops (so an abort never waits out ten seconds), and the report
  screen says `CANCELLED` rather than the flow vanishing back to the
  meter. `cal` is reset at the *start* of the flow, not just before its
  results are stored: an abort that left a previous run's numbers on
  screen would let the report's hold apply a threshold belonging to some
  other display state. Sampling yields every iteration (`vTaskDelay(1)`)
  — a 10s unyielding poll would trip the 5s core-0 watchdog, as would
  spinning through any of the waits.

  **It refuses to start in a BLE mode with no subscribed host**
  (`appCanSendInput()`, which is `bleReady` in a BLE mode and true
  otherwise): the press is the entire mechanism by which the display
  changes, so without one both captures would characterise the same state
  and the report would blame the sensor with an `OVERLAP` for something
  the radio did. Same judgement `serviceAutoTest()` makes about starting
  a run with no link, and it lands on the same report screen, as
  `NO LINK`. On the report,
  **hold applies the calibrated threshold**: `lightThreshold` is mutable,
  the value is persisted to NVS (namespace `sensor`, key `thr`) and
  re-loaded at UI-task startup — which means **a stored calibration
  outranks `-DLIGHT_THRESHOLD` from then on**. Changing the build flag
  and reflashing will appear to do nothing on a device that has ever
  had a calibration applied; re-calibrate (or erase the `sensor`
  namespace) to change it. 0/out-of-range in NVS falls back to the
  flag. The threshold is read and written only on the UI task, which is
  what makes the plain `int` safe.
- **The "Validate" menu item is the diagnostic for impossibly-fast
  detections, and its idle-flip counter is the number to look at first.**
  A validation run is 20 presses with 1.5-3s settling gaps, the first of
  them plus the shared 1s start delay (main.cpp
  paces it through the same machinery as the auto test; see
  `appStartValidation()`), during which the board watches the sensor the
  whole time: between presses the UI task's wait is replaced by ~1kHz
  idle watching, so any confirmed threshold crossing with no input in
  flight is counted as an *idle flip* — a light change nobody asked for,
  which is what backlight PWM, pixel-inversion flicker, mains-flickering
  room lights, or a too-thin threshold margin look like. After each
  press's crossing the watcher keeps going for `VAL_POSTWATCH_MS` and
  counts extra crossings (a press should change the light exactly once),
  and anything under `VAL_FAST_US` (1ms — the full-speed-USB polling
  floor) is classed impossible rather than banked. Validation samples
  never touch the stats, histogram, or run files (`appRecordSample()` is
  skipped, so `boardWriteRun()` sees an empty record and declines). The
  report ends in a verdict naming the likeliest culprit, idle flips
  outranking everything — unrequested changes make every other number
  unreliable.
- **The false-trigger protections were LOST for a while — shelving the
  TEPT4400 branch took them with it.** `MEASURE_CONFIRM` (3 consecutive
  agreeing samples per crossing, clock stopped at the run's first
  sample) and the 4-read averaged baseline were added to
  `runMeasurement()` in the SFH309 era — on what became the tept4400
  branch. When that branch was shelved, main's `runMeasurement()`
  silently reverted to single-read baseline and single-sample crossings,
  and the impossibly-fast detections duly returned with the next
  low-margin sensor. Both protections are back in main (restored
  alongside the validation mode) — if a measurement-path hardening ever
  looks missing, check whether it lives on an unmerged branch before
  re-deriving it.
- **The top menu peaked at eight rows, which is the hard ceiling at its
  11px pitch; it is back to seven.** The "MENU" heading went at seven
  items (Validate); the row pitch dropped 12px→11px at eight (Sensor),
  putting row eight at y=85 with three clear pixels per gap and the hints
  untouched at 96/108. `USB drive` then left the top menu for the mode
  picker (see the storage-is-a-mode gotcha), so a BLE mode's list is
  seven again — last row at y=74, one row of headroom. The ceiling has
  not moved: a ninth item needs a scrolling menu, not another row, and
  there is nothing left to shave.
