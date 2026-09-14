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
  the action a press sends, the USB product string). Header-only lookup
  tables, shared by the core and the board layer.
- `src/board.h` — the board I/O contract: `boardBegin()`,
  `boardButtonPressed()`, `boardShowBoot()`, `boardShowPress()`,
  `boardShowPending()`. Exactly one implementation is compiled per env, so
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
  the whole LCD face is the button), 0.85" 128x128 LCD via M5GFX. Because a
  screen can show the mode permanently, it does: title, the active mode's
  name in its colour, a rounded "action box" naming what a press sends
  (filled with the mode colour while held, outlined when idle), and a
  footer that is either `hold 3s: next mode` or — once a hold has queued a
  change — the pending mode plus `on next reset`. A press repaints only the
  action box, not the whole screen. `display.init()` runs in
  `boardShowBoot()`, i.e. *after* `USB.begin()`, so panel autodetect and
  backlight bring-up don't delay enumeration.

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
