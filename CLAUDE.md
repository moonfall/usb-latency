# usb-latency

PlatformIO firmware for measuring/minimizing input-to-USB latency: a board
with one button emulates a USB HID gamepad, keyboard, or mouse, and a press
is turned into a HID report as directly as possible.

Two boards are supported, one PlatformIO env each, from one shared source
tree:

| env | board | input | indicator |
| --- | --- | --- | --- |
| `esp32-s3-zero` | Waveshare ESP32-S3-Zero | BOOT button (GPIO0) | onboard WS2812 (GPIO21) |
| `m5stack-atoms3r` | M5Stack AtomS3R | screen button (GPIO41) | 128x128 LCD |

The AtomS3R variant also reads an M5Stack Unit Light (U012) on the Grove
port. That sensor does the actual latency measurement: in the three HID
modes a press starts a clock at the button edge and stops it when the
light crosses `LIGHT_THRESHOLD` (default 3000 ADC counts), so the figure
on screen covers button-down → USB → host → compositor → panel. A fourth
mode, `MODE_LIGHT`, constructs no HID device and just meters the sensor,
for aiming it and checking that the threshold falls between the display's
two states. The S3-Zero has neither a Grove port nor a screen, so it
reports a mode count of 3 and never offers `MODE_LIGHT`.

## Build

```
pio run -e esp32-s3-zero              # compile one variant
pio run -e m5stack-atoms3r
pio run                               # both
pio run -e <env> -t upload            # flash (see esptool gotcha below)
```

## Project layout

- `platformio.ini` — a shared `[env]` section holds everything common:
  native USB-OTG mode (`ARDUINO_USB_MODE=0`) and `ARDUINO_USB_CDC_ON_BOOT=0`
  (deliberately not `1` — see the CDC-on-boot gotcha below; a manual
  `USBCDC` in `src/main.cpp` (`USBSerial`) is begun explicitly instead, so
  Serial-style debug output over USB is still available). The two envs then
  differ only in `board`, `build_src_filter` (which `board_*.cpp` gets
  compiled), and — for the AtomS3R — a `lib_deps` on M5GFX. Both envs
  restate their board profile's `extra_flags` minus `ARDUINO_USB_MODE=1`,
  which would otherwise redefine the `=0` set above.
- `src/main.cpp` — board-independent core: the mode state machine, NVS
  persistence, the single HID device, and the debounce loop. The button
  drives exactly one of three USB HID device modes at a time: gamepad "X"
  button (`USBHIDGamepad`), keyboard space key (`USBHIDKeyboard`), or mouse
  left click (`USBHIDMouse`). Only the active mode's device class is ever
  constructed (`new`'d at runtime in `setup()`, not declared as a global),
  so the USB descriptor for a given boot contains a single HID collection —
  the device enumerates as a genuine single-purpose gamepad, keyboard, or
  mouse, not a multi-collection composite. The USB product name is set per
  mode too (`modeProductName()`, e.g. "USB Latency Tester - Keyboard"), so
  the host's device picker identifies the active mode by name. Holding the
  button advances a *pending* mode (gamepad → keyboard → mouse → gamepad)
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
  repainting a screen is never in the latency path.
- `src/mode.h` — the `Mode` enum plus the strings naming it (mode name,
  the action a press sends, the USB product string) and `modeSendsHid()`.
  Header-only lookup tables, shared by the core and the board layer.
  `MODE_LIGHT` must stay last in the enum: a board without a sensor
  returns `MODE_LIGHT` from `boardModeCount()`, and because it is last,
  its index *is* the number of modes that precede it.
- `src/board.h` — the board I/O contract: `boardBegin()`,
  `boardModeCount()`, `boardButtonPressed()`, `boardShowBoot()`,
  `boardShowPress()`, `boardShowPending()`. Exactly one implementation is compiled per env, so
  there are no board `#ifdef`s in `main.cpp` and nothing in the board layer
  touches USB. Mode state is passed in rather than duplicated there.
- `src/board_s3zero.cpp` — ESP32-S3-Zero: BOOT button (GPIO0, active-low),
  onboard WS2812 (GPIO21). The LED lights up while the button is held, in a
  colour identifying the active mode (red/green/blue). Once a hold crosses
  `MODE_HOLD_MS`, it flashes white for `MODE_SWITCH_FLASH_MS` (500ms) to
  mark the first mode change in that hold, then shows the new pending
  mode's colour; further changes within the same hold skip the flash and
  jump straight to the next colour. On boot, the LED also briefly flashes
  the active mode's colour (`BOOT_FLASH_MS`, 200ms) so the mode is visible
  without pressing the button first.
- `src/board_atoms3r.cpp` — AtomS3R: screen button (GPIO41, active-low —
  the whole LCD face is the button), 0.85" 128x128 LCD via M5GFX, and the
  Unit Light's analog output on GPIO1 (ADC1_CH0). Because a screen can
  show state permanently, it does: the live light reading (raw ADC count,
  volts, and a bar) across the top, then the active mode's name in its
  colour, a rounded "action box" naming what a press sends (filled with
  the mode colour while held, outlined when idle), and a footer that is
  either `hold 3s: next mode` or — once a hold has queued a change — the
  pending mode plus `on next reset`.

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
  core-0-starvation gotcha below. The top strip shows the last figure plus
  a running count/min/mean.

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
- **A press in a measuring mode doesn't light the action box until the
  measurement finishes.** This is deliberate, not a dropped frame: the UI
  task must not be pushing pixels over SPI while it is sampling the
  sensor, because a repaint both delays the first sample and can straddle
  the very change being timed. So `boardShowPress()` hands over the
  timestamp, and the box is repainted after `runMeasurement()` returns —
  tens of milliseconds normally, `MEASURE_TIMEOUT_MS` in the no-response
  case. A tap shorter than the measurement may never show the box lit at
  all.
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
  and hold-to-cycle, and a third gesture would be one too many. `MODE_LIGHT`
  draws the threshold as a tick on its bar, which is how you check the
  value is in the right place without being able to edit it live.
