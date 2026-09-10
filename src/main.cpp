/*
 * ESP32-S3-Zero USB HID Gamepad / Keyboard / Mouse (single active device)
 * -------------------------------------------------------------------------
 * Emulates exactly ONE USB HID device at a time — gamepad ("X" button),
 * keyboard (Space), or mouse (left click) — selected by the onboard BOOT
 * button (GPIO0, active-low):
 *   - Short press: send the active mode's action.
 *   - Hold for MODE_HOLD_MS (3s): switch to the next mode
 *     (gamepad -> keyboard -> mouse -> gamepad) and re-enumerate on USB.
 *
 * Why a reboot, not a runtime switch: the ESP32 Arduino core's TinyUSB HID
 * wrapper registers a device's report descriptor once, in that device
 * class's *constructor* (USBHIDGamepad::USBHIDGamepad() etc. all call
 * hid.addDevice(this, ...) unconditionally) — merely skipping .begin() on
 * the other two doesn't stop them contributing their report collection to
 * the composite descriptor. Constructing all three, like a normal
 * composite-HID sketch does, is exactly what made the device enumerate as
 * a multi-collection gadget (which macOS then splits into what look like
 * several devices). To present as a single, genuine gamepad *or* keyboard
 * *or* mouse, only the active mode's class must ever be constructed —
 * which this sketch does by `new`-ing only one of them, at runtime, based
 * on a mode persisted in NVS (`Preferences`). Switching modes therefore
 * means: persist the new mode, then `ESP.restart()`. The reboot's USB PHY
 * disconnect/reconnect is what makes the mode switch look like a real
 * hot-plug to a different device, rather than a live descriptor change
 * (which this core's HID wrapper has no supported way to do anyway).
 *
 * This also means every USB identity/descriptor call (constructing the
 * one HID device, USB.productName(), etc.) MUST happen in setup(), before
 * this sketch's own USB.begin() — which is exactly why
 * ARDUINO_USB_CDC_ON_BOOT must be 0 (see platformio.ini and the CLAUDE.md
 * gotcha): with it set to 1, the Arduino core's app_main() calls
 * USB.begin() itself before setup() ever runs, locking in a USB
 * descriptor that doesn't know about our HID device or product name at
 * all — which is why, with that flag on, none of the HID actions
 * (button/keypress/click) ever reached the host, even though everything
 * that doesn't depend on USB enumeration (LED, NVS-persisted mode) looked
 * fine. With CDC_ON_BOOT off, `USBSerial` below is begun manually instead,
 * so Serial-style debug output over USB remains available.
 *
 * The USB product name includes the active mode (see modeProductName()),
 * so the host's device picker/System Information shows e.g. "USB Latency
 * Tester - Keyboard" rather than a generic, mode-less name.
 *
 * The onboard WS2812 RGB LED (GPIO21) lights up in a colour identifying
 * the active mode while the button is held — red/green/blue — and
 * briefly flashes white just before a mode-switch reboot.
 *
 * Debounce strategy: leading-edge / "lockout" debounce (act immediately
 * on the edge, then ignore further changes for DEBOUNCE_MS) to keep
 * added latency at zero for the actual button action; only the reboot
 * path is deliberately slow.
 *
 * Requires ARDUINO_USB_MODE=0 (native USB-OTG / TinyUSB) — set in
 * platformio.ini. Do not press/hold BOOT while plugging in via a normal
 * (non-flashing) USB session; that combination is reserved for entering
 * the ROM bootloader.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <USB.h>
#include <USBCDC.h>
#include <USBHIDGamepad.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>

// --- Configuration ---------------------------------------------------
static const uint8_t BOOT_BUTTON_PIN = 0;      // GPIO0 on the S3-Zero
static const uint8_t RGB_LED_PIN = 21;         // onboard WS2812
static const uint32_t DEBOUNCE_MS = 25;        // lockout window after a trigger
static const uint32_t MODE_HOLD_MS = 3000;     // hold time to cycle modes

static const char *PREFS_NAMESPACE = "usbmode";
static const char *PREFS_KEY = "mode";

enum Mode : uint8_t {
  MODE_GAMEPAD = 0,
  MODE_KEYBOARD,
  MODE_MOUSE,
  MODE_COUNT,
};

static Preferences prefs;
static Mode activeMode = MODE_GAMEPAD;

// Manual CDC serial (see file header on why ARDUINO_USB_CDC_ON_BOOT is 0).
static USBCDC USBSerial;

// Only the active mode's device is ever constructed (see file header) —
// the other two pointers stay null for the life of this boot.
static USBHIDGamepad *gamepad = nullptr;
static USBHIDKeyboard *keyboard = nullptr;
static USBHIDMouse *mouse = nullptr;

static const char *modeProductName(Mode mode) {
  switch (mode) {
    case MODE_GAMEPAD:  return "USB Latency Tester - Gamepad";
    case MODE_KEYBOARD: return "USB Latency Tester - Keyboard";
    case MODE_MOUSE:    return "USB Latency Tester - Mouse";
    default:            return "USB Latency Tester";
  }
}

static bool stableState = false;   // false = released, true = pressed
static uint32_t lastTriggerMs = 0;
static uint32_t pressStartMs = 0;

// This board's onboard WS2812 shows red where rgbLedWrite()'s green_val
// argument is nonzero and green where its red_val argument is nonzero —
// i.e. its red/green channels are swapped relative to the library's
// assumed wire order. blue_val is unaffected. Swap red/green here so
// every other call site can use normal, intuitive (r, g, b) values.
static inline void setPixel(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(RGB_LED_PIN, g, r, b);
}

static inline void setLed(bool on) {
  if (!on) {
    setPixel(0, 0, 0);
    return;
  }
  switch (activeMode) {
    case MODE_GAMEPAD:  setPixel(40, 0, 0); break;  // red
    case MODE_KEYBOARD: setPixel(0, 40, 0); break;  // green
    case MODE_MOUSE:    setPixel(0, 0, 40); break;  // blue
    default: break;
  }
}

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

// Persists the next mode and reboots so the device re-enumerates on USB
// as that mode's device alone — a real hot-plug, not a live switch.
static void switchModeAndReboot() {
  Mode next = static_cast<Mode>((activeMode + 1) % MODE_COUNT);
  prefs.putUChar(PREFS_KEY, next);
  prefs.end();

  setPixel(40, 40, 40);  // brief white flash: hold registered, rebooting
  delay(150);
  ESP.restart();
}

void setup() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  prefs.begin(PREFS_NAMESPACE, false);
  uint8_t stored = prefs.getUChar(PREFS_KEY, MODE_GAMEPAD);
  activeMode = (stored < MODE_COUNT) ? static_cast<Mode>(stored) : MODE_GAMEPAD;

  // Construct the one active-mode device, and set the product name to
  // match, before USB.begin() — both are rejected as no-ops afterwards.
  switch (activeMode) {
    case MODE_GAMEPAD:  gamepad = new USBHIDGamepad();  gamepad->begin();  break;
    case MODE_KEYBOARD: keyboard = new USBHIDKeyboard(); keyboard->begin(); break;
    case MODE_MOUSE:    mouse = new USBHIDMouse();      mouse->begin();    break;
    default: break;
  }
  USB.productName(modeProductName(activeMode));
  USBSerial.begin();
  USB.begin();

  setLed(false);
}

void loop() {
  bool raw = (digitalRead(BOOT_BUTTON_PIN) == LOW);  // active-low
  uint32_t now = millis();

  // Outside the lockout window, any change is a real edge: act on it
  // immediately, then start the lockout to swallow bounce.
  if ((now - lastTriggerMs) >= DEBOUNCE_MS && raw != stableState) {
    stableState = raw;
    lastTriggerMs = now;

    if (stableState) {
      pressStartMs = now;
      sendPress();
      setLed(true);
    } else {
      sendRelease();
      setLed(false);
    }
  }

  // A long-press cycles to the next mode via a reboot (see file header).
  if (stableState && (now - pressStartMs) >= MODE_HOLD_MS) {
    sendRelease();
    switchModeAndReboot();  // never returns
  }
}
