# usb-latency

PlatformIO firmware project for a Waveshare **ESP32-S3-Zero** that emulates a
USB HID gamepad, as a test vehicle for measuring/minimizing input-to-USB
latency.

## Build

```
pio run          # compile
pio run -t upload   # flash (see esptool gotcha below)
```

## Project layout

- `platformio.ini` — env `esp32-s3-zero`, board `esp32-s3-devkitc-1` (same
  chip/USB wiring as the S3-Zero). Native USB-OTG mode
  (`ARDUINO_USB_MODE=0`). `ARDUINO_USB_CDC_ON_BOOT` is deliberately `0`,
  not `1` — see the CDC-on-boot gotcha below; a manual `USBCDC` in
  `src/main.cpp` (`USBSerial`) is begun explicitly instead, so
  Serial-style debug output over USB is still available.
- `src/main.cpp` — BOOT button (GPIO0, active-low) drives exactly one of
  three USB HID device modes at a time: gamepad "X" button
  (`USBHIDGamepad`), keyboard space key (`USBHIDKeyboard`), or mouse left
  click (`USBHIDMouse`). Only the active mode's device class is ever
  constructed (`new`'d at runtime in `setup()`, not declared as a global),
  so the USB descriptor for a given boot contains a single HID
  collection — the device enumerates as a genuine single-purpose gamepad,
  keyboard, or mouse, not a multi-collection composite. The USB product
  name is set per mode too (`modeProductName()`, e.g. "USB Latency Tester
  - Keyboard"), so the host's device picker identifies the active mode by
  name. Holding the button for 3s (`MODE_HOLD_MS`) persists the next mode
  (gamepad → keyboard → mouse → gamepad) to NVS via `Preferences` and
  calls `ESP.restart()` — the reboot's USB disconnect/reconnect is what
  makes the switch a real hot-plug to a different device; see the
  mode-switch-needs-a-reboot gotcha below for why. Leading-edge/lockout
  debounce (act immediately on the edge, then ignore further changes for
  `DEBOUNCE_MS`) rather than trailing-edge debounce, to keep added latency
  at zero for the button action itself — only the mode-switch path is
  deliberately slow. Onboard WS2812 LED (GPIO21) lights up while the
  button is held, in a colour identifying the active mode (red/green/blue),
  and flashes white just before a mode-switch reboot.

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
- **This board's onboard WS2812 has red/green swapped** relative to
  `rgbLedWrite()`'s assumed GRB wire order: passing a nonzero `red_val`
  shows as green and a nonzero `green_val` shows as red (confirmed on
  hardware — a "keyboard mode" LED meant to be green displayed red, and
  vice versa for gamepad mode). `blue_val` is unaffected. `src/main.cpp`
  compensates in a `setPixel(r, g, b)` wrapper that swaps the first two
  args before calling `rgbLedWrite()`, so every other call site can use
  normal, intuitive `(r, g, b)` values.
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
  (`new` it in `setup()`, gated on a value read from `Preferences`/NVS),
  and switch modes by persisting the next value and calling
  `ESP.restart()` — a real reboot, which the host sees as a genuine
  disconnect/reconnect.
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
- **A mode-switch reboot must wait for BOOT to be released first.**
  GPIO0 (the BOOT button) is also the ESP32's boot-mode strapping pin: if
  it's still held low at the instant `esp_restart()` resets the chip, the
  ROM bootloader reads that as "enter USB/UART download mode" instead of
  booting this firmware — so the device never re-enumerates as anything
  HID at all (it silently drops into the download bootloader). Since the
  3s-hold mode switch fires *while the button is still down by
  definition*, `switchModeAndReboot()` was calling `ESP.restart()` before
  the user had physically released the button — timing-dependent on how
  fast they let go, which made it look like a flaky, mode-specific bug
  (worked twice, then didn't) rather than what it was: this hazard on
  every single mode-switch reboot. Fixed by blocking in
  `switchModeAndReboot()` on `digitalRead(BOOT_BUTTON_PIN) == LOW` (plus a
  short settle delay) before calling `ESP.restart()`. This is the same
  underlying hazard as the "don't hold BOOT while plugging in" note
  above, just self-inflicted via software reset instead of a physical
  power-up.
- **Local esptool/PlatformIO toolchain is broken on this machine**: `pio
  run -t upload` (and any build step needing `esptool`, e.g. building
  `bootloader.bin`) fails — `tool-esptoolpy` package install errors
  (`idf_tools.py installation failed`, "does not appear to be a Python
  project"), and `esptool` isn't found on PATH. This wasn't fixed yet — the
  compile step (`pio run`, no upload) works fine and is enough to validate
  code changes; flashing needs the esptool install issue resolved first.
